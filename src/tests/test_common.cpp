// test_common.cpp
// Host-side tests for the parts that do not need a panel: the shared-memory
// transport, the gamma LUT, and the temporal dither's time-average.
//
// Build and run:  cmake --build build --target led_test_common && ./build/led_test_common

#include "../common/dither.h"
#include "../common/frame_shm.h"
#include "../common/gamma.h"

#include <unistd.h>

#include <cmath>
#include <cstdio>
#include <cstring>
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

// ---------------------------------------------------------------------------
static void TestHeaderLayout() {
    std::printf("header layout\n");
    // These are the guarantees both binaries rely on; if the struct is ever
    // reordered this is the test that should stop it.
    CHECK(sizeof(FrameShmHeader) == 256, "sizeof=%zu", sizeof(FrameShmHeader));
    CHECK(offsetof(FrameShmHeader, magic) == 0, "magic offset");
    CHECK(offsetof(FrameShmHeader, sequence) == 64, "sequence not on line 1");
    CHECK(offsetof(FrameShmHeader, reading_slot) == 128, "reading_slot not on line 2");
    CHECK(SegmentBytes(256, 128, 3, 3) ==
              sizeof(FrameShmHeader) + AlignUp(256 * 128 * 3, 64) * 3,
          "segment size");
}

// ---------------------------------------------------------------------------
static void TestShmRoundTrip() {
    std::printf("shm round trip\n");
    const std::string path = "/led-test-" + std::to_string(getpid());
    const uint32_t W = 64, H = 32, BPP = 3, SLOTS = 3;

    FrameShm writer;
    std::string err;
    if (!writer.Create(path, W, H, BPP, SLOTS, 0644, &err)) {
        std::printf("  FAIL create: %s\n", err.c_str());
        ++g_failures;
        return;
    }

    FrameShm reader;
    if (!reader.Open(path, &err)) {
        std::printf("  FAIL open: %s\n", err.c_str());
        ++g_failures;
        return;
    }
    CHECK(reader.Validate(W, H, BPP, &err), "validate: %s", err.c_str());
    CHECK(!reader.Validate(W + 1, H, BPP, &err), "validate should reject wrong width");

    uint64_t seq = 0;
    uint32_t slot = 0;
    CHECK(!reader.TryAcquireFrame(&seq, &slot), "no frame should be ready yet");

    // Publish three frames; each must arrive intact and in a slot the reader
    // was not holding.
    uint32_t held = kMaxSlots;
    for (int f = 1; f <= 3; ++f) {
        const uint32_t w = writer.AcquireWriteSlot();
        CHECK(w != held, "renderer picked the slot the driver is reading (%u)", w);
        std::memset(writer.slot(w), f * 20, writer.frame_bytes());
        writer.Publish(w, 1234 * f);

        CHECK(reader.TryAcquireFrame(&seq, &slot), "frame %d not seen", f);
        CHECK(seq == static_cast<uint64_t>(f), "seq=%llu want %d",
              (unsigned long long)seq, f);
        reader.MarkReading(slot);
        held = slot;

        const uint8_t *px = reader.slot(slot);
        bool all = true;
        for (size_t i = 0; i < reader.frame_bytes(); ++i)
            if (px[i] != f * 20) { all = false; break; }
        CHECK(all, "frame %d pixel data corrupt", f);
        CHECK(!reader.TryAcquireFrame(&seq, &slot), "same frame reported twice");
    }

    CHECK(reader.header()->frames_published.load() == 3, "publish counter");
    reader.Close();
    writer.Close();

    FrameShm gone;
    CHECK(!gone.Open(path, &err), "segment should be unlinked after close");
}

// ---------------------------------------------------------------------------
static void TestGamma() {
    std::printf("gamma LUT\n");
    GammaLut lut;
    lut.Build(GammaMode::kCie1931, 2.2, 100, 4);

    CHECK(lut[0] == 0, "black must be 0, got %u", lut[0]);
    CHECK(lut[255] == lut.full_scale(), "white must be %u, got %u", lut.full_scale(), lut[255]);
    for (int c = 1; c < 256; ++c)
        CHECK(lut[c] >= lut[c - 1], "LUT not monotonic at %d", c);

    // The measured cost of low PWM depth, and the reason dithering exists.
    // These are the numbers quoted in the README; if the curve changes they
    // should change with it.
    CHECK(lut.BlackClampByte() == 78, "pwm4 black clamp = %d, expected 78",
          lut.BlackClampByte());
    GammaLut lut5;
    lut5.Build(GammaMode::kCie1931, 2.2, 100, 5);
    CHECK(lut5.BlackClampByte() == 53, "pwm5 black clamp = %d, expected 53",
          lut5.BlackClampByte());

    // Brightness must scale the target, not the output level, so shadow
    // detail survives dimming.
    GammaLut dim;
    dim.Build(GammaMode::kCie1931, 2.2, 50, 4);
    CHECK(dim[255] == lut.full_scale() / 2 || dim[255] == lut.full_scale() / 2 + 1,
          "50%% brightness white = %u", dim[255]);

    GammaLut lin;
    lin.Build(GammaMode::kLinear, 1.0, 100, 4);
    CHECK(lin[128] > lut[128], "linear should be brighter at midpoint than CIE");
}

// ---------------------------------------------------------------------------
// The property that actually matters: averaged over a full phase cycle, the
// dithered output must reproduce the 11-bit target, not the truncated one.
static void TestDitherAverage() {
    std::printf("dither time-average\n");
    const int pwm_bits = 4;
    const int phases   = 4;

    TemporalDither d;
    d.Configure(pwm_bits, phases, /*enabled=*/true);
    CHECK(d.max_level() == 15, "max_level=%d", d.max_level());

    GammaLut lut;
    lut.Build(GammaMode::kCie1931, 2.2, 100, 4);

    const int shift = kLibBitPlanes - pwm_bits;      // 7
    const double step = 1 << shift;                  // 128 target units per level

    // Average across the phase cycle and a 4x4 pixel block (the dither is
    // spatial as well as temporal, so both must be integrated).
    double worst = 0.0;
    int worst_c = -1;
    for (int c = 0; c < 256; ++c) {
        const uint16_t target = lut[c];
        double sum = 0.0;
        int n = 0;
        for (uint32_t p = 0; p < static_cast<uint32_t>(phases); ++p)
            for (int y = 0; y < 4; ++y)
                for (int x = 0; x < 4; ++x) {
                    sum += d.Level(target, x, y, p);
                    ++n;
                }
        const double mean_level = sum / n;
        const double want_level = target / step;
        const double err = std::fabs(mean_level - want_level);
        if (err > worst) { worst = err; worst_c = c; }
    }
    // One level is 1/15 of full scale; the residual is quantised to `phases`
    // steps, so the bound is half a phase step.
    const double bound = 0.5 / phases + 1e-9;
    CHECK(worst <= bound, "worst mean-level error %.4f at c=%d exceeds %.4f",
          worst, worst_c, bound);
    std::printf("  worst mean-level error %.4f levels (bound %.4f)\n", worst, bound);

    // Dithering must lift the black clamp: input bytes that truncate to level 0
    // should still light up on some phases.
    int lifted = 0;
    for (int c = 1; c <= 76; ++c) {
        bool any = false;
        for (uint32_t p = 0; p < static_cast<uint32_t>(phases) && !any; ++p)
            for (int y = 0; y < 4 && !any; ++y)
                for (int x = 0; x < 4 && !any; ++x)
                    if (d.Level(lut[c], x, y, p) > 0) any = true;
        if (any) ++lifted;
    }
    std::printf("  %d of 76 otherwise-black input levels now produce light\n", lifted);
    CHECK(lifted > 60, "dither only lifted %d of 76 black-clamped levels", lifted);

    // Disabled must be pure truncation -- that is what makes the A/B honest.
    TemporalDither off;
    off.Configure(pwm_bits, phases, /*enabled=*/false);
    for (int c = 0; c < 256; ++c)
        CHECK(off.Level(lut[c], 1, 2, 3) == static_cast<uint32_t>(lut[c] >> shift),
              "disabled dither must truncate at c=%d", c);

    // Never exceed the level range, at any phase or position.
    for (int c = 0; c < 256; ++c)
        for (uint32_t p = 0; p < 16; ++p)
            for (int y = 0; y < 4; ++y)
                for (int x = 0; x < 4; ++x)
                    CHECK(d.Level(lut[c], x, y, p) <= 15u, "level overflow at c=%d", c);
}

// ---------------------------------------------------------------------------
// The exactness claim from gamma.h: byte = level << (8 - pwm_bits) must survive
// the library's DirectMapColor and come back as the same level.
static void TestLevelRoundTrip() {
    std::printf("level <-> byte round trip\n");
    for (int pwm_bits = 1; pwm_bits <= 8; ++pwm_bits) {
        const int levels = 1 << pwm_bits;
        for (int L = 0; L < levels; ++L) {
            const uint8_t byte = ByteForLevel(static_cast<uint32_t>(L), pwm_bits);
            // DirectMapColor at brightness 100: value11 = byte << 3
            const uint32_t value11 = static_cast<uint32_t>(byte) << 3;
            const uint32_t back = value11 >> (kLibBitPlanes - pwm_bits);
            CHECK(back == static_cast<uint32_t>(L),
                  "pwm_bits=%d level=%d -> byte=%u -> level=%u", pwm_bits, L, byte, back);
        }
    }
}

// ---------------------------------------------------------------------------
int main() {
    std::printf("== led common tests ==\n");
    TestHeaderLayout();
    TestShmRoundTrip();
    TestGamma();
    TestDitherAverage();
    TestLevelRoundTrip();
    if (g_failures == 0) {
        std::printf("== all passed ==\n");
        return 0;
    }
    std::printf("== %d FAILURE(S) ==\n", g_failures);
    return 1;
}
