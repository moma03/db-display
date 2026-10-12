// test_departures.cpp
// Host-side tests for the departure rules that decide what stays on the board:
// expiry against the wall clock, and whether a change touches the hero.
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
static void TestHeroChanged() {
    std::printf("hero changed\n");
    const int64_t t = 1'000'000;
    std::vector<Departure> base = {Dep("A", t), Dep("B", t + 60), Dep("C", t + 120)};

    auto l = base;
    CHECK(!HeroChanged(base, l), "identical lists");

    l = base; l[2].cTime = "12:45";
    CHECK(!HeroChanged(base, l), "a list-row change must not count as a hero change");

    l = base; l.push_back(Dep("D", t + 600));
    CHECK(!HeroChanged(base, l), "a train added at the bottom is not a hero change");

    l = base; l[0].cTime = "12:40";
    CHECK(HeroChanged(base, l), "hero delay not detected");

    l = base; l.erase(l.begin());
    CHECK(HeroChanged(base, l), "hero departure not detected");

    CHECK(HeroChanged({}, base), "first data must count as a hero change");
}

// ---------------------------------------------------------------------------
int main() {
    TestPrune();
    TestNextExpiry();
    TestWingPruneThenGroup();
    TestHeroChanged();
    if (g_failures) std::printf("%d FAILURE(S)\n", g_failures);
    else            std::printf("all passed\n");
    return g_failures ? 1 : 0;
}
