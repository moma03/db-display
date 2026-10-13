// test_departures.cpp
// Host-side tests for the departure rules that decide what stays on the board:
// expiry against the wall clock, and which changes can be shown in place.
//
// Build and run:  cmake --build build --target led_test_departures && ./build/led_test_departures

#include "../renderer/departures.h"

#include <cstdio>
#include <limits>
#include <string>
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

static Departure Dep(const char *dest, int64_t at) {
    Departure d;
    d.dest = dest;
    d.departAt = at;
    return d;
}

// ---------------------------------------------------------------------------
static void TestPrune() {
    std::printf("prune departed\n");
    const int64_t t = 1'000'000;   // any whole minute will do
    std::vector<Departure> list = {Dep("A", t), Dep("B", t), Dep("C", t + 120),
                                   Dep("demo", 0)};

    auto l = list;
    PruneDeparted(l, t + 59, 60);
    CHECK(l.size() == 4, "still in the grace period, got %zu", l.size());

    l = list;
    PruneDeparted(l, t + 60, 60);
    CHECK(l.size() == 2, "both trains of that minute go together, got %zu", l.size());
    CHECK(l.size() == 2 && l[0].dest == "C" && l[1].dest == "demo", "wrong survivors");

    l = list;
    PruneDeparted(l, t + 10'000, 60);
    CHECK(l.size() == 1 && l[0].dest == "demo", "departAt 0 must never expire");
}

// ---------------------------------------------------------------------------
static void TestNextExpiry() {
    std::printf("next expiry\n");
    const int64_t t = 1'000'000;
    CHECK(NextExpiry({Dep("C", t + 120), Dep("A", t)}, 60) == t + 60, "earliest + grace");
    CHECK(NextExpiry({Dep("demo", 0)}, 60) == std::numeric_limits<int64_t>::max(),
          "nothing expires");
    CHECK(NextExpiry({}, 60) == std::numeric_limits<int64_t>::max(), "empty list");

    // After pruning at the reported expiry, the next expiry lies in the future:
    // the render loop must not re-reconcile every frame.
    std::vector<Departure> l = {Dep("A", t), Dep("C", t + 120)};
    const int64_t e = NextExpiry(l, 60);
    PruneDeparted(l, e, 60);
    CHECK(NextExpiry(l, 60) > e, "expiry did not advance");
}

// ---------------------------------------------------------------------------
static void TestWingPruneThenGroup() {
    std::printf("wing group expires as a whole\n");
    const int64_t t = 1'000'000;
    auto a = Dep("Dortmund", t);  a.stop_id = "100-2507311238-3"; a.wings = "200-2507311238";
    auto b = Dep("Iserlohn", t);  b.stop_id = "200-2507311238-3"; b.wings = "100-2507311238";
    auto c = Dep("Minden", t + 300);
    std::vector<Departure> l = {a, b, c};

    PruneDeparted(l, t + 60, 60);
    GroupWings(l);
    CHECK(l.size() == 1 && l[0].dest == "Minden", "wing pair not removed together");
    CHECK(HeroGroupSize(l) == 1, "stale wing count on the new hero");
}

// ---------------------------------------------------------------------------
static void TestSameTrains() {
    std::printf("same trains\n");
    const int64_t t = 1'000'000;
    auto a = Dep("A", t); a.stop_id = "1-2507311238-3";
    auto b = Dep("B", t + 60); b.stop_id = "2-2507311239-3";
    std::vector<Departure> base = {a, b};

    auto l = base; l[0].cTime = "12:45"; l[0].departAt += 420;
    CHECK(SameTrains(base, l), "a delay does not make it a different train");

    l = {b, a};
    CHECK(!SameTrains(base, l), "reordering must be detected");

    l = {b};
    CHECK(!SameTrains(base, l), "a departed train must be detected");

    // Rows without a stop_id (demo data) are told apart by their contents.
    CHECK(!SameTrains({Dep("X", 0)}, {Dep("Y", 0)}), "keyless rows compared by content");
}

// ---------------------------------------------------------------------------
static void TestSameShape() {
    std::printf("same shape\n");
    auto a = Dep("Dortmund", 1'000'000); a.stop_id = "1-2507311238-3";
    a.pTime = "12:38"; a.platform = "4"; a.line = "RE 11";

    // Drawn fresh every frame: may be patched in place.
    auto b = a; b.cTime = "12:45"; b.departAt += 420;
    CHECK(SameShape(a, b), "delay must be patchable");
    b = a; b.cPlatform = "7";
    CHECK(SameShape(a, b), "platform change must be patchable");

    // Baked into scrollers or geometry: needs a rebuild.
    b = a; b.notes = {{1, "Verspätung"}};
    CHECK(!SameShape(a, b), "a new reason line changes the row height");
    b = a; b.dest = "Hamm";
    CHECK(!SameShape(a, b), "destination lives in a scroller");
    b = a; b.cancelled = true;
    CHECK(!SameShape(a, b), "cancellation recolours the scrollers");
    b = a; b.category = "Bus";
    CHECK(!SameShape(a, b), "turning into a bus recolours the scrollers");
    b = a; b.stop_id = "9-2507311238-3";
    CHECK(!SameShape(a, b), "a different train is never the same shape");
}

// ---------------------------------------------------------------------------
int main() {
    TestPrune();
    TestNextExpiry();
    TestWingPruneThenGroup();
    TestSameTrains();
    TestSameShape();
    if (g_failures) std::printf("%d FAILURE(S)\n", g_failures);
    else            std::printf("all passed\n");
    return g_failures ? 1 : 0;
}
