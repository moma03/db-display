// main.cpp -- led_renderer
//
// Owns all data fetching, layout and drawing, and writes complete RGB888
// frames into shared memory. It is allowed to be slow, to block on the network
// and to stutter: none of that reaches the driver, which is the point of the
// split.
//
// Threads:
//   main    draw -> publish, paced to target_fps
//   fetch   one persistent DB session per station: waits for the fetcher's
//           NOTIFY, re-queries, keeps this display's subscription alive
//   control the hub's control channel: the station from Home Assistant in,
//           status out (see control/db_controls.h)
//   watch   inotify on layout.json (low priority, inside LayoutWatcher)

#include <signal.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "control/control_channel.h"
#include "control/db_controls.h"
#include "cpp_backend.h"
#if HAVE_LIBPQ
#include "data/db_fetcher.h"
#include "data/db_live.h"
#endif
#include "frame_shm.h"
#include "layout_watcher.h"
#include "renderer_config.h"

namespace {

std::atomic<bool> g_stop{false};
void OnSignal(int) { g_stop.store(true); }

void Usage(const char *a0) {
    std::fprintf(stderr,
        "usage: %s [--config PATH] [--layout PATH] [--demo] [--eva N | --station S]\n"
        "          [--display-id ID]\n"
        "          [--once OUT.ppm]\n"
        "\n"
        "  --config PATH   renderer config JSON (default config/renderer.json)\n"
        "  --layout PATH   override the layout path from the config\n"
        "  --demo          use built-in sample data, no database needed\n"
        "  --demo-minimal  sample data with ONE solo departure and no ticker,\n"
        "                  i.e. the layout's minimum content -- use with --once\n"
        "                  to check that optional widgets give their space back\n"
        "  --eva N         override the station EVA number\n"
        "  --station S     station by EVA number, name or DS100 code; a name is\n"
        "                  resolved via the stations table or the fetcher\n"
        "  --manifest      print the Home Assistant controls for the hub and exit\n"
        "  --display-id ID name of this panel in the fetcher's subscription table\n"
        "                  (default <hostname>:<shm path>)\n"
        "  --once OUT.ppm  render a single frame to a PPM file and exit\n"
        "                  (no shared memory, no panel -- works anywhere)\n",
        a0);
}

// Sample data covering the cases that are easy to break: a wing pair as the
// next departure, a wing pair inside the list, a cancellation, a rerouting, a
// changed platform, and replacement buses.
led::FrameData DemoData() {
    using led::Departure;
    led::FrameData d;
    d.station = "Steinheim (Westf.)";
    d.ticker  = "Ein Unwetter behindert den Bahnverkehr. Für weitere Informationen "
                "beachten Sie Durchsagen.";
    auto mk = [](const char *plat, const char *cplat, const char *line, const char *dest,
                 const char *pt, const char *ct, const char *stops) {
        Departure x;
        x.platform = plat; x.cPlatform = cplat; x.line = line; x.dest = dest;
        x.pTime = pt; x.cTime = ct; x.stops = stops;
        return x;
    };
    auto a = mk("1", "", "RE 11", "Dortmund Hbf", "12:38", "12:40", "Kamen, Unna");
    a.notes = {{1, "technische Störung am Zug"}};
    a.stop_id = "100-2507311238-3"; a.wings = "200-2507311238";
    auto b = mk("1", "", "RE 11", "Iserlohn", "12:38", "12:40", "Kamen, Menden");
    b.stop_id = "200-2507311238-3"; b.wings = "100-2507311238";
    d.departures.push_back(a);
    d.departures.push_back(b);

    auto c = mk("3", "5", "S 11", "Düsseldorf Flughafen Terminal", "12:35", "12:37",
                "Düsseldorf Hbf");
    c.notes = {{1, "Verspätung wegen technischer Störung"}};
    d.departures.push_back(c);
    d.departures.push_back(mk("4", "", "RE 6", "Minden (Westf)", "12:50", "12:55",
                              "Düsseldorf Hbf, Duisburg Hbf, Oberhausen Hbf"));
    d.departures.push_back(mk("5", "", "S 8", "Wuppertal-Oberbarmen", "12:40", "12:42",
                              "Düsseldorf Hbf, Neuss Hbf, Krefeld Hbf"));
    auto cancelled = mk("4", "", "RE 6", "Minden (Westf)", "12:50", "12:55",
                        "Düsseldorf Hbf, Duisburg Hbf");
    cancelled.cancelled = true;
    d.departures.push_back(cancelled);

    auto w1 = mk("4", "", "RB 89", "Paderborn Hbf", "13:02", "", "Soest, Lippstadt");
    w1.stop_id = "300-2507311302-5"; w1.wings = "400-2507311302";
    auto w2 = mk("4", "", "RB 89", "Warburg (Westf)", "13:02", "", "Soest, Lippstadt");
    w2.stop_id = "400-2507311302-5"; w2.wings = "300-2507311302";
    w2.notes = {{1, "Zug wird geflügelt"}};
    d.departures.push_back(w1);
    d.departures.push_back(w2);

    auto rerouted = mk("2", "", "RE 6", "Kassel-Wilhelmshöhe", "12:48", "13:20",
                       "Warburg (Westf), Hofgeismar");
    rerouted.destChanged = true;
    rerouted.notes = {{1, "Umleitung"}};
    d.departures.push_back(rerouted);

    auto bus = mk("", "", "Bus SEV 1", "Paderborn Hbf", "12:52", "",
                  "Altenbeken, Bad Driburg");
    bus.category = "Bus";
    bus.notes = {{1, "Schienenersatzverkehr"}};
    d.departures.push_back(bus);
    return d;
}

// The layout's minimum content: one solo departure (no wing partner, no delay
// reason) and no disruption message. Everything optional is absent, so this is
// what proves the optional widgets actually give their space back instead of
// leaving it blank.
led::FrameData DemoDataMinimal() {
    led::FrameData d;
    d.station = "Steinheim (Westf.)";
    d.ticker.clear();
    led::Departure x;
    x.platform = "1"; x.line = "RE 11"; x.dest = "Dortmund Hbf";
    x.pTime = "12:38"; x.stops = "Kamen, Unna";
    d.departures.push_back(x);
    for (int i = 0; i < 8; ++i) {
        led::Departure y = x;
        y.platform = std::to_string(2 + (i % 5));
        y.dest = "Zielbahnhof " + std::to_string(i + 1);
        y.pTime = "13:0" + std::to_string(i);
        d.departures.push_back(y);
    }
    return d;
}

bool WritePpm(const char *path, const uint8_t *rgb, int w, int h) {
    FILE *f = fopen(path, "wb");
    if (!f) return false;
    std::fprintf(f, "P6\n%d %d\n255\n", w, h);
    const size_t n = static_cast<size_t>(w) * h * 3;
    const bool ok = fwrite(rgb, 1, n, f) == n;
    fclose(f);
    return ok;
}

}  // namespace

int main(int argc, char **argv) {
    using namespace led;

    std::string config_path = "config/renderer.json";
    std::string layout_override, once_path, display_id_override, station_arg;
    bool demo = false, demo_minimal = false, print_manifest = false;
    int eva_override = 0;

    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if      (a == "--config" && i + 1 < argc) config_path = argv[++i];
        else if (a == "--layout" && i + 1 < argc) layout_override = argv[++i];
        else if (a == "--once"   && i + 1 < argc) once_path = argv[++i];
        else if (a == "--eva"    && i + 1 < argc) eva_override = std::atoi(argv[++i]);
        else if (a == "--display-id" && i + 1 < argc) display_id_override = argv[++i];
        else if (a == "--station" && i + 1 < argc) station_arg = argv[++i];
        else if (a == "--manifest") print_manifest = true;
        else if (a == "--demo") demo = true;
        else if (a == "--demo-minimal") { demo = true; demo_minimal = true; }
        else if (a == "--help" || a == "-h") { Usage(argv[0]); return 0; }
        else { std::fprintf(stderr, "unknown argument '%s'\n", a.c_str()); Usage(argv[0]); return 1; }
    }

    RendererConfig cfg;
    std::string err;
    if (!LoadRendererConfig(config_path, &cfg, &err)) {
        std::fprintf(stderr, "renderer: %s\n", err.c_str());
        return 1;
    }
    if (!layout_override.empty()) cfg.layout_path = layout_override;

    // For the hub, at its startup: no panel, no layout, no database.
    if (print_manifest) {
        std::printf("%s\n", BuildDbManifest(DbDeviceInfo{}, "").dump(2).c_str());
        return 0;
    }

    // The station: --station (EVA number, name or DS100), else --eva, else
    // renderer.json. Home Assistant can change it at runtime through the hub.
    const bool station_pinned = !station_arg.empty() || eva_override != 0;
    std::string initial_station = std::to_string(eva_override != 0 ? eva_override : cfg.eva);
    {
        size_t b = station_arg.find_first_not_of(" \t");
        size_t e = station_arg.find_last_not_of(" \t");
        if (b != std::string::npos) initial_station = station_arg.substr(b, e - b + 1);
    }
    if (!display_id_override.empty()) cfg.display_id = display_id_override;
    if (cfg.display_id.empty()) {
#if HAVE_LIBPQ
        cfg.display_id = DefaultDisplayId(cfg.shm_path);
#endif
    }

    LayoutWatcher watcher;
    if (!watcher.Start(cfg.layout_path, &err)) {
        std::fprintf(stderr, "renderer: layout: %s\n", err.c_str());
        return 1;
    }
    auto layout = watcher.Current();
    std::fprintf(stderr, "renderer: layout %s -> %dx%d, %zu widgets\n",
                 cfg.layout_path.c_str(), layout->width, layout->height,
                 layout->widgets.size());

    CppRenderBackend backend;
    backend.SetExpiry(cfg.departure_grace_seconds, cfg.max_defer_seconds);

    // ---- one-shot mode: render to a PPM and exit. No shm, no panel, no
    // database -- this is how the layout gets iterated on without hardware.
    if (!once_path.empty()) {
        if (!backend.Init(layout, layout->width, layout->height, &err)) {
            std::fprintf(stderr, "renderer: %s\n", err.c_str());
            return 1;
        }
        std::vector<uint8_t> buf(static_cast<size_t>(layout->width) * layout->height * 3);
        backend.SetData(demo_minimal ? DemoDataMinimal() : DemoData());
        backend.RenderFrame(buf.data(), 0);
        if (!WritePpm(once_path.c_str(), buf.data(), layout->width, layout->height)) {
            std::fprintf(stderr, "renderer: cannot write %s\n", once_path.c_str());
            return 1;
        }
        std::fprintf(stderr, "renderer: wrote %s (%dx%d)\n",
                     once_path.c_str(), layout->width, layout->height);
        return 0;
    }

    // ---- attach to the driver's shared memory -----------------------------
    FrameShm shm;
    {
        const auto deadline = std::chrono::steady_clock::now() +
                              std::chrono::seconds(cfg.wait_for_shm_s);
        bool opened = false;
        for (;;) {
            if (shm.Open(cfg.shm_path, &err)) { opened = true; break; }
            if (std::chrono::steady_clock::now() >= deadline || g_stop.load()) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(250));
        }
        if (!opened) {
            std::fprintf(stderr,
                "renderer: could not attach to %s after %ds: %s\n"
                "          The driver creates the segment -- start led_driver first.\n",
                cfg.shm_path.c_str(), cfg.wait_for_shm_s, err.c_str());
            return 1;
        }
    }

    const int W = static_cast<int>(shm.header()->width);
    const int H = static_cast<int>(shm.header()->height);
    if (!shm.Validate(static_cast<uint32_t>(layout->width),
                      static_cast<uint32_t>(layout->height), 3, &err)) {
        std::fprintf(stderr, "renderer: FATAL layout does not match the panel: %s\n",
                     err.c_str());
        return 1;
    }
    std::fprintf(stderr, "renderer: attached to %s, %dx%d\n", cfg.shm_path.c_str(), W, H);

    if (!backend.Init(layout, W, H, &err)) {
        std::fprintf(stderr, "renderer: %s\n", err.c_str());
        return 1;
    }

    // ---- data ------------------------------------------------------------
    struct Shared {
        std::mutex mtx;
        FrameData data;
        bool has_new = false;
    } shared;

    // ---- control: Home Assistant, through the hub --------------------------
    // The hub publishes this program's controls (--manifest) and hands the
    // station over at launch and on every change; the fetch thread picks a
    // new one up within 250 ms.
    struct StationRequest {
        std::mutex mtx;
        std::string value;
        std::atomic<uint64_t> generation{0};
    } station_req;
    station_req.value = initial_station;
    DbHubLink hub([&station_req](const std::string &station) {
        {
            std::lock_guard<std::mutex> lk(station_req.mtx);
            station_req.value = station;
        }
        station_req.generation.fetch_add(1);
    }, station_pinned);
    hub.Update([&](DbDisplayState &s) { s.station = initial_station; });
    ControlChannel channel;
    if (!demo) {
        if (hub.Start(&channel))
            std::fprintf(stderr, "renderer: station from Home Assistant via the hub%s\n",
                         station_pinned ? " (the command line wins at startup)" : "");
        else
            std::fprintf(stderr, "renderer: not started by the hub -- showing '%s'\n",
                         initial_station.c_str());
    }

    std::thread fetch;
    if (demo) {
        std::fprintf(stderr, "renderer: demo mode, no database\n");
        backend.SetData(demo_minimal ? DemoDataMinimal() : DemoData());
    } else {
#if !HAVE_LIBPQ
        std::fprintf(stderr,
            "renderer: built without libpq -- live mode unavailable. "
            "Rebuild with libpq-dev installed, or pass --demo.\n");
        return 1;
#else
        DBConnectionConfig db;
        db.host = cfg.db_host; db.port = cfg.db_port; db.dbname = cfg.db_name;
        db.user = cfg.db_user; db.password = cfg.db_password; db.eva = cfg.eva;
        db.grace_seconds = cfg.departure_grace_seconds;
        // Never log db.password.
        std::fprintf(stderr, "renderer: live mode, %s@%s:%s/%s station '%s', on NOTIFY "
                             "(fallback every %ds) (credentials from %s)\n",
                     db.user.empty() ? "<no user>" : db.user.c_str(),
                     db.host.c_str(), db.port.c_str(), db.dbname.c_str(),
                     initial_station.c_str(), cfg.refresh_seconds,
                     cfg.env_file_used.empty() ? "the environment"
                                               : cfg.env_file_used.c_str());
        if (db.user.empty())
            std::fprintf(stderr,
                "renderer: WARNING no DB_PG_USER set -- copy config/.env.example "
                "to config/.env and fill it in, or pass --demo.\n");

        fetch = std::thread([&shared, db, &cfg, &station_req, &hub]() mutable {
            using clock = std::chrono::steady_clock;
            const auto stopping = [] { return g_stop.load(std::memory_order_relaxed); };

            // One DB session per station: when Home Assistant picks another
            // one, the session ends (unsubscribing) and a new one starts.
            while (!stopping()) {
                std::string station;
                uint64_t generation;
                {
                    std::lock_guard<std::mutex> lk(station_req.mtx);
                    station = station_req.value;
                    generation = station_req.generation.load();
                }
                const auto changed = [&station_req, generation] {
                    return station_req.generation.load() != generation;
                };
                // A number is an EVA; anything else is resolved by name.
                DBConnectionConfig sdb = db;
                std::string query;
                if (!station.empty() && station.find_first_not_of("0123456789") == std::string::npos)
                    sdb.eva = std::atoi(station.c_str());
                else
                    query = station;
                std::fprintf(stderr, "renderer: station '%s'\n", station.c_str());
                hub.Update([&](DbDisplayState &s) {
                    s.station = station;
                    s.station_name.clear();
                    s.next_departure.clear();
                    s.status = query.empty() ? "ok" : "Bahnhof wird gesucht ...";
                });

                // Re-query on the fetcher's NOTIFY instead of on a timer. The
                // timer stays as a slow fallback; departures still expire and
                // countdowns still tick every frame, independent of fetches.
                DbLiveSession live(sdb, cfg.display_id, query);
                // Until a station given by name is resolved there is nothing to
                // query; the board says so instead.
                std::string shown_state;
                auto show_station_state = [&shared, &live, &shown_state, &hub] {
                    const std::string msg = live.resolve_error().empty()
                                                ? "Bahnhof wird gesucht ..." : live.resolve_error();
                    if (msg == shown_state) return;
                    shown_state = msg;
                    hub.Update([&](DbDisplayState &s) { s.status = msg; });
                    FrameData fd;
                    fd.station = live.station_query();
                    Departure d = NoDeparturesPlaceholder();
                    d.dest = msg;
                    fd.departures.push_back(d);
                    std::lock_guard<std::mutex> lk(shared.mtx);
                    shared.data = std::move(fd);
                    shared.has_new = true;
                };
                if (!live.resolved()) show_station_state();
                auto next_resolution_check = clock::now();
                const auto refresh_every   = std::chrono::seconds(cfg.refresh_seconds);
                const auto heartbeat_every = std::chrono::seconds(cfg.heartbeat_seconds);
                auto next_refresh   = clock::now();
                auto next_heartbeat = clock::now() + heartbeat_every;
                bool want_fetch = true;
                bool connected_reported = true;

                while (!stopping() && !changed()) {
                    bool fresh = false;
                    if (!live.EnsureConnected(&fresh)) {
                        if (connected_reported) {
                            connected_reported = false;
                            hub.Update([](DbDisplayState &s) { s.status = "Keine Verbindung zur Datenbank"; });
                        }
                        // Retried with backoff inside; just stay responsive to stop.
                        std::this_thread::sleep_for(std::chrono::milliseconds(250));
                        continue;
                    }
                    connected_reported = true;
                    if (fresh) {
                        want_fetch = true;   // whatever was announced while down is lost
                        next_heartbeat = clock::now() + heartbeat_every;
                    }

                    auto now = clock::now();
                    if (now >= next_heartbeat) {
                        next_heartbeat = now + heartbeat_every;
                        if (!live.Heartbeat()) continue;
                    }

                    if (!live.resolved()) {
                        // Waiting for the fetcher (NOTIFY station_resolved), with a
                        // slow poll in case the notification is lost.
                        if (want_fetch || now >= next_resolution_check) {
                            next_resolution_check = now + std::chrono::seconds(5);
                            want_fetch = false;
                            live.CheckResolution();
                        }
                        if (!live.resolved()) show_station_state();
                    }
                    // Resolved just now -- by the fetcher, or from the stations
                    // table while connecting.
                    if (live.resolved() && sdb.eva != live.eva()) {
                        sdb.eva = live.eva();
                        want_fetch = true;
                    }

                    if (live.resolved() && (want_fetch || now >= next_refresh)) {
                        FetchedStationInfo st;
                        std::vector<FetchedDeparture> fetched;
                        std::string ticker;
                        const int n = FetchDeparturesOn(live.conn(), sdb, st, fetched, ticker);
                        if (n >= 0) {
                            FrameData fd;
                            fd.station = st.name;
                            fd.ticker  = ticker;
                            for (const auto &f : fetched) {
                                Departure d;
                                d.platform = f.platform; d.cPlatform = f.cPlatform;
                                d.line = f.line; d.dest = f.dest;
                                d.pTime = f.pTime; d.cTime = f.cTime; d.stops = f.stops;
                                d.departAt = f.departAt;
                                d.cancelled = f.cancelled; d.category = f.category;
                                d.destChanged = f.destChanged; d.stop_id = f.stop_id;
                                d.wings = f.wings;
                                for (const auto &fn : f.notes) d.notes.push_back({fn.id, fn.text});
                                if (!f.cancelled) ApplyRouteChange(d, f.plannedPath, f.changedPath);
                                fd.departures.push_back(std::move(d));
                            }
                            const std::string next = NextDepartureText(fd.departures);
                            hub.Update([&](DbDisplayState &s) {
                                // The field shows names, whatever was typed or
                                // preset (EVA, DS100, odd spelling) -- unless a
                                // newer request is already on its way.
                                if (st.name_known && !changed()) s.station = st.name;
                                s.station_name = st.name;
                                s.status = "ok";
                                s.next_departure = next;
                                s.last_update = static_cast<int64_t>(std::time(nullptr));
                            });
                            std::lock_guard<std::mutex> lk(shared.mtx);
                            shared.data = std::move(fd);
                            shared.has_new = true;
                        } else {
                            std::fprintf(stderr, "renderer: DB refresh failed, keeping previous data\n");
                            hub.Update([](DbDisplayState &s) { s.status = "Datenbankabfrage fehlgeschlagen"; });
                        }
                        want_fetch = false;
                        now = clock::now();
                        next_refresh = now + refresh_every;
                    }

                    // Block on the connection's socket until a notification, the
                    // next timer, or at most 250 ms so a stop request or a new
                    // station is seen.
                    const auto until = std::min(live.resolved() ? next_refresh : next_resolution_check,
                                                next_heartbeat);
                    const auto wait_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                        until - now).count();
                    const int timeout = static_cast<int>(std::max<long long>(
                        0, std::min<long long>(wait_ms, 250)));
                    if (live.WaitForUpdate(timeout) == DbLiveSession::Wait::kNotified)
                        want_fetch = true;
                }
                // `live` unsubscribes in its destructor, so the fetcher stops
                // fetching a temporary station without waiting for the TTL.
            }
        });
#endif  // HAVE_LIBPQ
    }

    signal(SIGINT, OnSignal);
    signal(SIGTERM, OnSignal);

    // ---- render loop ------------------------------------------------------
    const auto interval = std::chrono::duration<double>(1.0 / cfg.target_fps);
    auto next_at = std::chrono::steady_clock::now();
    uint64_t frames = 0;

    while (!g_stop.load(std::memory_order_relaxed)) {
        // Layout reloads land at a frame boundary, so they can never tear.
        if (auto fresh = watcher.TakeIfChanged()) {
            std::string rerr;
            if (backend.Reload(fresh, &rerr))
                std::fprintf(stderr, "renderer: layout applied\n");
            else
                std::fprintf(stderr, "renderer: layout rejected by backend, "
                                     "keeping previous: %s\n", rerr.c_str());
        }

        if (!demo) {
            FrameData fresh;
            bool got = false;
            {
                std::lock_guard<std::mutex> lk(shared.mtx);
                if (shared.has_new) { fresh = std::move(shared.data); shared.has_new = false; got = true; }
            }
            if (got) backend.SetData(fresh);
        }

        const uint32_t slot = shm.AcquireWriteSlot();
        const uint64_t t0 = NowNs();
        backend.RenderFrame(shm.slot(slot), static_cast<int64_t>(t0));
        // Publish only once the frame is complete. Until this release-store,
        // the driver cannot see the slot at all.
        shm.Publish(slot, NowNs() - t0);
        ++frames;

        next_at += std::chrono::duration_cast<std::chrono::steady_clock::duration>(interval);
        const auto now = std::chrono::steady_clock::now();
        if (next_at < now) next_at = now;   // fell behind; do not spiral
        std::this_thread::sleep_until(next_at);
    }

    std::fprintf(stderr, "renderer: stopping after %llu frames\n",
                 (unsigned long long)frames);
    if (fetch.joinable()) fetch.join();
    channel.Stop();
    watcher.Stop();
    shm.Close();
    return 0;
}
