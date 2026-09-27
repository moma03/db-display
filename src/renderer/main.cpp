// main.cpp -- led_renderer
//
// Owns all data fetching, layout and drawing, and writes complete RGB888
// frames into shared memory. It is allowed to be slow, to block on the network
// and to stutter: none of that reaches the driver, which is the point of the
// split.
//
// Threads:
//   main    draw -> publish, paced to target_fps
//   fetch   blocking database queries (low priority)
//   watch   inotify on layout.json (low priority, inside LayoutWatcher)

#include <signal.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "cpp_backend.h"
#if HAVE_LIBPQ
#include "data/db_fetcher.h"
#endif
#include "frame_shm.h"
#include "layout_watcher.h"
#include "renderer_config.h"

namespace {

std::atomic<bool> g_stop{false};
void OnSignal(int) { g_stop.store(true); }

void Usage(const char *a0) {
    std::fprintf(stderr,
        "usage: %s [--config PATH] [--layout PATH] [--demo] [--eva N] [--once OUT.ppm]\n"
        "\n"
        "  --config PATH   renderer config JSON (default config/renderer.json)\n"
        "  --layout PATH   override the layout path from the config\n"
        "  --demo          use built-in sample data, no database needed\n"
        "  --eva N         override the station EVA number\n"
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
    std::string layout_override, once_path;
    bool demo = false;
    int eva_override = 0;

    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if      (a == "--config" && i + 1 < argc) config_path = argv[++i];
        else if (a == "--layout" && i + 1 < argc) layout_override = argv[++i];
        else if (a == "--once"   && i + 1 < argc) once_path = argv[++i];
        else if (a == "--eva"    && i + 1 < argc) eva_override = std::atoi(argv[++i]);
        else if (a == "--demo") demo = true;
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
    if (eva_override != 0) cfg.eva = eva_override;

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

    // ---- one-shot mode: render to a PPM and exit. No shm, no panel, no
    // database -- this is how the layout gets iterated on without hardware.
    if (!once_path.empty()) {
        if (!backend.Init(layout, layout->width, layout->height, &err)) {
            std::fprintf(stderr, "renderer: %s\n", err.c_str());
            return 1;
        }
        std::vector<uint8_t> buf(static_cast<size_t>(layout->width) * layout->height * 3);
        backend.SetData(DemoData());
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

    std::thread fetch;
    if (demo) {
        std::fprintf(stderr, "renderer: demo mode, no database\n");
        backend.SetData(DemoData());
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
        // Never log db.password.
        std::fprintf(stderr, "renderer: live mode, %s@%s:%s/%s eva=%d every %ds "
                             "(credentials from %s)\n",
                     db.user.empty() ? "<no user>" : db.user.c_str(),
                     db.host.c_str(), db.port.c_str(), db.dbname.c_str(),
                     db.eva, cfg.refresh_seconds,
                     cfg.env_file_used.empty() ? "the environment"
                                               : cfg.env_file_used.c_str());
        if (db.user.empty())
            std::fprintf(stderr,
                "renderer: WARNING no DB_PG_USER set -- copy config/.env.example "
                "to config/.env and fill it in, or pass --demo.\n");

        fetch = std::thread([&shared, db, &cfg] {
            while (!g_stop.load(std::memory_order_relaxed)) {
                FetchedStationInfo st;
                std::vector<FetchedDeparture> fetched;
                std::string ticker;
                // Blocking network I/O. This is exactly the work that used to
                // share a process with the GPIO loop.
                const int n = FetchDepartures(db, st, fetched, ticker);
                if (n >= 0) {
                    FrameData fd;
                    fd.station = st.name;
                    fd.ticker  = ticker;
                    for (const auto &f : fetched) {
                        Departure d;
                        d.platform = f.platform; d.cPlatform = f.cPlatform;
                        d.line = f.line; d.dest = f.dest;
                        d.pTime = f.pTime; d.cTime = f.cTime; d.stops = f.stops;
                        d.cancelled = f.cancelled; d.category = f.category;
                        d.destChanged = f.destChanged; d.stop_id = f.stop_id;
                        d.wings = f.wings;
                        for (const auto &fn : f.notes) d.notes.push_back({fn.id, fn.text});
                        fd.departures.push_back(std::move(d));
                    }
                    std::lock_guard<std::mutex> lk(shared.mtx);
                    shared.data = std::move(fd);
                    shared.has_new = true;
                } else {
                    std::fprintf(stderr, "renderer: DB refresh failed, keeping previous data\n");
                }
                for (int i = 0; i < cfg.refresh_seconds * 10 &&
                                !g_stop.load(std::memory_order_relaxed); ++i)
                    std::this_thread::sleep_for(std::chrono::milliseconds(100));
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
    watcher.Stop();
    shm.Close();
    return 0;
}
