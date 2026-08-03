// shmgen.cpp -- synthetic frame generator.
//
// Writes test patterns into the shared-memory segment so the driver can be
// validated with no renderer, no database and no network. If the panel looks
// wrong with shmgen running, the problem is the driver or the wiring, not the
// dashboard.
//
//   led_shmgen --pattern bars              colour bars + greyscale ramp
//   led_shmgen --pattern gradient          moving gradient (tests dither)
//   led_shmgen --pattern refresh           refresh-rate / tearing pattern
//   led_shmgen --pattern shadow            near-black ramp (tests black clamp)
//   led_shmgen --pattern checker           1px checkerboard (tests addressing)
//
// By default it opens an existing segment (created by the driver) and fails if
// there is none. --create makes it create one instead, so patterns can be
// generated with no driver running at all.

#include <signal.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>

#include "frame_shm.h"

using namespace led;

namespace {

std::atomic<bool> g_stop{false};
void OnSignal(int) { g_stop.store(true); }

inline void Px(uint8_t *f, int w, int x, int y, uint8_t r, uint8_t g, uint8_t b) {
    uint8_t *p = f + (static_cast<size_t>(y) * w + x) * 3;
    p[0] = r; p[1] = g; p[2] = b;
}

// Eight saturated bars over the top two thirds, a full 0..255 greyscale ramp
// underneath. The ramp is the useful part: at 4 PWM bits without dithering it
// shows as ~16 visible steps with the bottom third solid black.
void Bars(uint8_t *f, int w, int h, double) {
    static const uint8_t c[8][3] = {
        {255,255,255},{255,255,0},{0,255,255},{0,255,0},
        {255,0,255},{255,0,0},{0,0,255},{0,0,0},
    };
    const int split = h * 2 / 3;
    for (int y = 0; y < split; ++y)
        for (int x = 0; x < w; ++x) {
            const int i = x * 8 / w;
            Px(f, w, x, y, c[i][0], c[i][1], c[i][2]);
        }
    for (int y = split; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            const uint8_t v = static_cast<uint8_t>(x * 255 / (w - 1));
            Px(f, w, x, y, v, v, v);
        }
}

// A slow diagonal gradient. Banding here is the thing temporal dithering is
// supposed to remove, so this is the A/B pattern for dither.enabled.
void Gradient(uint8_t *f, int w, int h, double t) {
    const double phase = t * 0.15;
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            double u = (double(x) / w + double(y) / h) * 0.5 + phase;
            u -= std::floor(u);
            const uint8_t v = static_cast<uint8_t>(u * 255.0);
            Px(f, w, x, y, v, v, v);
        }
}

// A bright bar sweeping across a dark field, plus a 1px column that alternates
// every frame. Tearing shows as a broken bar; dropped frames show as a stutter
// in the sweep; the alternating column should look like a steady half-bright
// line, and flickers visibly if swaps are being missed.
void Refresh(uint8_t *f, int w, int h, double t) {
    std::memset(f, 0, static_cast<size_t>(w) * h * 3);
    const int bar = static_cast<int>(t * 60.0) % w;
    static bool flip = false;
    flip = !flip;
    for (int y = 0; y < h; ++y) {
        for (int d = 0; d < 4; ++d)
            Px(f, w, (bar + d) % w, y, 255, 255, 255);
        Px(f, w, w / 2, y, flip ? 255 : 0, flip ? 255 : 0, flip ? 255 : 0);
        // Static reference blocks so brightness drift is visible against them.
        if (y < h / 8) {
            Px(f, w, 2, y, 255, 0, 0);
            Px(f, w, 3, y, 255, 0, 0);
        }
    }
}

// A ramp confined to the bottom of the input range, where low PWM depth does
// its damage. With pwm_bits=4 and no dithering almost all of this is black.
void Shadow(uint8_t *f, int w, int h, double) {
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            const uint8_t v = static_cast<uint8_t>(x * 96 / (w - 1));
            Px(f, w, x, y, v, v, v);
        }
}

// 1px checkerboard: any pixel-mapping or chain-order error turns this into
// stripes or blocks instead of an even grey.
void Checker(uint8_t *f, int w, int h, double) {
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            const uint8_t v = ((x ^ y) & 1) ? 255 : 0;
            Px(f, w, x, y, v, v, v);
        }
}

}  // namespace

int main(int argc, char **argv) {
    std::string path = "/led-frames";
    std::string pattern = "bars";
    double fps = 30.0;
    bool create = false;
    uint32_t cw = 256, ch = 128, slots = 3;

    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto next = [&](const char *name) -> std::string {
            if (i + 1 >= argc) { std::fprintf(stderr, "%s needs a value\n", name); exit(1); }
            return argv[++i];
        };
        if      (a == "--shm")     path    = next("--shm");
        else if (a == "--pattern") pattern = next("--pattern");
        else if (a == "--fps")     fps     = std::stod(next("--fps"));
        else if (a == "--create")  create  = true;
        else if (a == "--width")   cw      = std::stoul(next("--width"));
        else if (a == "--height")  ch      = std::stoul(next("--height"));
        else if (a == "--slots")   slots   = std::stoul(next("--slots"));
        else {
            std::fprintf(stderr,
                "usage: %s [--shm PATH] [--pattern bars|gradient|refresh|shadow|checker]\n"
                "          [--fps N] [--create [--width W --height H --slots N]]\n",
                argv[0]);
            return a == "--help" ? 0 : 1;
        }
    }

    void (*fn)(uint8_t *, int, int, double) = nullptr;
    if      (pattern == "bars")     fn = Bars;
    else if (pattern == "gradient") fn = Gradient;
    else if (pattern == "refresh")  fn = Refresh;
    else if (pattern == "shadow")   fn = Shadow;
    else if (pattern == "checker")  fn = Checker;
    else { std::fprintf(stderr, "unknown pattern '%s'\n", pattern.c_str()); return 1; }

    FrameShm shm;
    std::string err;
    if (create) {
        if (!shm.Create(path, cw, ch, 3, slots, 0644, &err)) {
            std::fprintf(stderr, "shmgen: create failed: %s\n", err.c_str());
            return 1;
        }
        std::fprintf(stderr, "shmgen: created %s %ux%u\n", path.c_str(), cw, ch);
    } else {
        if (!shm.Open(path, &err)) {
            std::fprintf(stderr,
                "shmgen: %s\n"
                "        Start the driver first (it creates the segment), or "
                "pass --create to make one here.\n", err.c_str());
            return 1;
        }
    }

    const int w = static_cast<int>(shm.header()->width);
    const int h = static_cast<int>(shm.header()->height);
    std::fprintf(stderr, "shmgen: %s pattern into %s, %dx%d at %.1f fps\n",
                 pattern.c_str(), path.c_str(), w, h, fps);

    signal(SIGINT, OnSignal);
    signal(SIGTERM, OnSignal);

    const auto t0 = std::chrono::steady_clock::now();
    const auto interval = std::chrono::duration<double>(1.0 / fps);
    auto next_at = std::chrono::steady_clock::now();

    while (!g_stop.load()) {
        const double t = std::chrono::duration<double>(
            std::chrono::steady_clock::now() - t0).count();

        const uint32_t slot = shm.AcquireWriteSlot();
        const uint64_t t_start = NowNs();
        fn(shm.slot(slot), w, h, t);
        shm.Publish(slot, NowNs() - t_start);

        next_at += std::chrono::duration_cast<std::chrono::steady_clock::duration>(interval);
        std::this_thread::sleep_until(next_at);
    }

    std::fprintf(stderr, "\nshmgen: stopped after %llu frames\n",
                 (unsigned long long)shm.header()->frames_published.load());
    return 0;
}
