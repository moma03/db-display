// test_controls.cpp
// Host-side tests for what db-display exposes to Home Assistant through the
// LED_Panel_Controller hub: the manifest, the reported state, and the link
// over the control channel (against a socketpair standing in for the hub).
//
// Build and run:  cmake --build build --target led_test_controls && ./build/led_test_controls

#include "db_controls.h"

#include <sys/socket.h>
#include <unistd.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <memory>
#include <set>
#include <string>
#include <thread>
#include <vector>

static int g_failures = 0;

#define CHECK(cond, ...)                                              \
    do {                                                              \
        if (!(cond)) {                                                \
            std::printf("  FAIL %s:%d: ", __FILE__, __LINE__);        \
            std::printf(__VA_ARGS__);                                 \
            std::printf("\n");                                        \
            ++g_failures;                                             \
        }                                                             \
    } while (0)

using namespace led;
using nlohmann::json;

// ---------------------------------------------------------------------------
static void TestManifest() {
    std::printf("manifest\n");
    const json m = BuildDbManifest(DbDeviceInfo{}, "Hamburg Hbf");
    CHECK(m["manifest_version"] == 1, "version");
    CHECK(m["device"]["id"] == "db_display", "device id");
    CHECK(m["controls"].size() == 1 && m["controls"][0]["key"] == "station" &&
          m["controls"][0]["type"] == "text" && m["controls"][0]["default"] == "Hamburg Hbf",
          "one station text control: %s", m["controls"].dump().c_str());
    std::set<std::string> sensors;
    for (const auto &s : m["sensors"]) sensors.insert(s["key"].get<std::string>());
    CHECK(sensors == (std::set<std::string>{"station_name", "next_departure", "status", "last_update"}),
          "sensors");
    for (const auto &c : m["controls"])
        CHECK(c["key"] != "brightness" && c["key"] != "power", "hardware stays the hub's");
}

// ---------------------------------------------------------------------------
static void TestState() {
    std::printf("state values\n");
    DbDisplayState s;
    s.station = "Hamburg Dammtor";
    json v = DbStateValues(s);
    CHECK(v["station"] == "Hamburg Dammtor" && v["status"] == "ok", "basics");
    CHECK(v["next_departure"] == "–", "placeholder without departures");
    CHECK(!v.contains("last_update"), "no timestamp before the first fetch");
    s.last_update = 86400;
    CHECK(DbStateValues(s)["last_update"] == "1970-01-02T00:00:00+00:00", "timestamp");

    Departure gone, late, later;
    gone.line = "ICE 71"; gone.dest = "Basel SBB"; gone.pTime = "06:29"; gone.cancelled = true;
    late.line = "RE 7"; late.dest = "Kiel Hbf"; late.pTime = "04:03"; late.cTime = "04:08";
    later.line = "S 1"; later.dest = "Poppenbüttel"; later.pTime = "04:10";
    CHECK(NextDepartureText({gone, late, later}) == "RE 7 Kiel Hbf 04:03 (04:08)",
          "skips cancelled, shows the delay: %s", NextDepartureText({gone, late, later}).c_str());
    CHECK(NextDepartureText({NoDeparturesPlaceholder()}).empty(), "placeholder is no departure");
}

// ---------------------------------------------------------------------------
static json ReadState(int fd, const std::function<bool(const json &)> &until) {
    json merged = json::object();
    std::string buf;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (std::chrono::steady_clock::now() < deadline && !until(merged)) {
        char chunk[4096];
        const ssize_t n = recv(fd, chunk, sizeof(chunk), MSG_DONTWAIT);
        if (n > 0) buf.append(chunk, static_cast<size_t>(n));
        size_t nl;
        while ((nl = buf.find('\n')) != std::string::npos) {
            json j = json::parse(buf.substr(0, nl), nullptr, false);
            buf.erase(0, nl + 1);
            if (j.is_object() && j["type"] == "state") merged.update(j["values"]);
        }
        if (n <= 0) std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return merged;
}

static void Send(int fd, const json &m) {
    const std::string line = m.dump() + "\n";
    CHECK(write(fd, line.data(), line.size()) == static_cast<ssize_t>(line.size()), "write");
}

struct Link {
    int hub_fd = -1;
    std::vector<std::string> requested;
    std::unique_ptr<DbHubLink> link;
    ControlChannel channel;

    Link(bool pinned, const std::string &initial) {
        int sv[2];
        socketpair(AF_UNIX, SOCK_STREAM, 0, sv);
        hub_fd = sv[0];
        link.reset(new DbHubLink([this](const std::string &s) { requested.push_back(s); }, pinned));
        link->Update([&](DbDisplayState &st) { st.station = initial; });
        setenv("LED_PANEL_CONTROL_FD", std::to_string(sv[1]).c_str(), 1);
        CHECK(link->Start(&channel), "started by the hub");
        unsetenv("LED_PANEL_CONTROL_FD");
    }
    ~Link() {
        channel.Stop();
        close(hub_fd);
    }
};

static void TestHubLink() {
    std::printf("hub link\n");
    {
        // The stored station from the hub is taken -- and reported as such at
        // once, never the previous one (the hub would store that over it).
        Link l(false, "8000297");
        Send(l.hub_fd, {{"type", "settings"}, {"values", {{"station", "Hamburg Dammtor"}}}});
        json st = ReadState(l.hub_fd, [](const json &j) { return j.contains("status"); });
        CHECK(l.requested == std::vector<std::string>{"Hamburg Dammtor"}, "station requested");
        CHECK(st["station"] == "Hamburg Dammtor", "reported: %s", st.dump().c_str());

        // A change from Home Assistant.
        Send(l.hub_fd, {{"type", "set"}, {"key", "station"}, {"value", "AH"}});
        st = ReadState(l.hub_fd, [](const json &j) { return j.contains("station"); });
        CHECK(l.requested.size() == 2 && l.requested[1] == "AH", "set");
        CHECK(st.size() == 1 && st["station"] == "AH", "only the change: %s", st.dump().c_str());

        // The fetch thread's progress goes out as diffs.
        l.link->Update([](DbDisplayState &s) { s.station_name = "Hamburg Hbf"; });
        st = ReadState(l.hub_fd, [](const json &j) { return j.contains("station_name"); });
        CHECK(st.size() == 1 && st["station_name"] == "Hamburg Hbf", "diff: %s", st.dump().c_str());

        // Nothing we accept: an empty station, an unknown key.
        Send(l.hub_fd, {{"type", "set"}, {"key", "station"}, {"value", ""}});
        st = ReadState(l.hub_fd, [](const json &j) { return j.contains("station"); });
        CHECK(l.requested.size() == 2 && st["station"] == "AH", "empty station snaps back");
    }
    {
        // --station on the command line wins at startup and is reported.
        Link l(true, "Lippstadt");
        Send(l.hub_fd, {{"type", "settings"}, {"values", {{"station", "Hamburg Dammtor"}}}});
        json st = ReadState(l.hub_fd, [](const json &j) { return j.contains("station"); });
        CHECK(l.requested.empty(), "pinned station not replaced");
        CHECK(st["station"] == "Lippstadt", "pinned station reported: %s", st.dump().c_str());
        // ... but a later change from Home Assistant still applies.
        Send(l.hub_fd, {{"type", "set"}, {"key", "station"}, {"value", "Paderborn Hbf"}});
        ReadState(l.hub_fd, [](const json &j) { return j.contains("station"); });
        CHECK(l.requested == std::vector<std::string>{"Paderborn Hbf"}, "later change applies");
    }
    {
        unsetenv("LED_PANEL_CONTROL_FD");
        DbHubLink none([](const std::string &) {}, false);
        ControlChannel ch;
        CHECK(!none.Start(&ch), "not started by the hub");
        none.Update([](DbDisplayState &s) { s.status = "x"; });   // no-op
    }
}

int main() {
    TestManifest();
    TestState();
    TestHubLink();
    if (g_failures) std::printf("%d FAILURE(S)\n", g_failures);
    else            std::printf("all passed\n");
    return g_failures ? 1 : 0;
}
