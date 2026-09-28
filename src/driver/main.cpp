// main.cpp -- led_driver
//
// Everything in this file is startup or shutdown. The steady state is
// RunHotLoop() in hot_loop.cpp, which is the part held to the size and
// no-allocation/no-logging/no-syscall budget.
//
// Startup order matters and is not arbitrary:
//
//   1. config + --led-* flags   (the hub's flags must win, and pwm_bits is
//                                read back from them, not the file)
//   2. open shared memory       (mapping survives the privilege drop below)
//   3. real-time settings       (needs CAP_SYS_NICE / CAP_IPC_LOCK, so it has
//                                to happen while we are still root)
//   4. matrix init              (drops privileges to `daemon` internally)
//   5. hot loop
//
// Swapping 3 and 4 is the classic way to end up silently running at normal
// priority: see rt_setup.h.

#include <getopt.h>
#include <signal.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <new>
#include <stdexcept>
#include <string>
#include <thread>

#include "driver_config.h"
#include "frame_shm.h"
#include "hot_loop.h"
#include "rgbmatrix_backend.h"
#include "rt_setup.h"

namespace {

std::atomic<bool> g_stop{false};
void OnSignal(int) { g_stop.store(true); }

// The core rpi-rgb-led-matrix pins its own SCHED_FIFO 99 GPIO thread to.
constexpr int kLibraryCore = 3;

void Usage(const char *argv0) {
    std::fprintf(stderr,
        "usage: %s [--config PATH] [--wait-for-renderer SEC] [--dump-config]\n"
        "          [--led-* flags passed through to rpi-rgb-led-matrix]\n"
        "\n"
        "  --config PATH          driver config JSON (default config/driver.json)\n"
        "  --wait-for-renderer N  seconds to wait for the shm segment (default 0)\n"
        "  --dump-config          print the effective config and exit\n"
        "\n"
        "The driver creates the shared-memory segment and owns its geometry.\n"
        "Start it before the renderer, or use --wait-for-renderer in the unit.\n",
        argv0);
}

}  // namespace

static int RunDriver(int argc, char **argv) {
    using namespace led;

    std::string config_path = "config/driver.json";
    bool dump_only = false;

    // Pull out our own flags first, leaving --led-* for the library. Done by
    // hand rather than getopt because getopt would choke on the --led-* flags.
    std::vector<char *> passthrough;
    passthrough.push_back(argv[0]);
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--config") == 0 && i + 1 < argc) {
            config_path = argv[++i];
        } else if (std::strcmp(argv[i], "--dump-config") == 0) {
            dump_only = true;
        } else if (std::strcmp(argv[i], "--help") == 0 || std::strcmp(argv[i], "-h") == 0) {
            Usage(argv[0]);
            return 0;
        } else {
            passthrough.push_back(argv[i]);
        }
    }
    int    pt_argc = static_cast<int>(passthrough.size());
    char **pt_argv = passthrough.data();

    // --- 1. config ---------------------------------------------------------
    DriverConfig cfg;
    std::string err;
    if (!LoadDriverConfig(config_path, &cfg, &err)) {
        std::fprintf(stderr, "driver: %s\n", err.c_str());
        return 1;
    }
    if (!err.empty()) std::fprintf(stderr, "driver: %s", err.c_str());  // warnings

    RgbMatrixBackend backend;
    if (!backend.ApplyCommandLine(&pt_argc, &pt_argv, &cfg, &err)) {
        std::fprintf(stderr, "driver: %s\n", err.c_str());
        return 1;
    }

    std::fprintf(stderr, "driver: effective configuration\n%s\n",
                 DescribeDriverConfig(cfg).c_str());
    if (dump_only) return 0;

    if (cfg.show_refresh_rate) {
        // Not fatal, but the user should know what it costs: the library
        // printf()s the rate from inside its RT update thread on every single
        // refresh, which means taking the stdio lock ~190 times a second on
        // the timing-critical thread. Fine for a bring-up, not for production.
        std::fprintf(stderr,
            "driver: WARNING show_refresh_rate is on -- the library printf()s "
            "from its RT thread every refresh. Turn it off in production.\n");
    }

    // --- 2. shared memory --------------------------------------------------
    // The driver creates the segment because it is the one that knows the
    // authoritative panel geometry. The renderer opens it and adapts.
    const uint32_t want_w = static_cast<uint32_t>(cfg.cols * cfg.chain_length);
    const uint32_t want_h = static_cast<uint32_t>(cfg.rows * cfg.parallel);

    FrameShm shm;
    if (!shm.Create(cfg.shm_path, want_w, want_h, 3, cfg.slot_count, 0644, &err)) {
        std::fprintf(stderr, "driver: shm create failed: %s\n", err.c_str());
        return 1;
    }
    std::fprintf(stderr, "driver: created %s  %ux%u RGB888, %u slots, %zu bytes\n",
                 cfg.shm_path.c_str(), want_w, want_h, cfg.slot_count,
                 SegmentBytes(want_w, want_h, 3, cfg.slot_count));

    // --- 3. real-time, while still privileged ------------------------------
    const RtReport rt = ApplyRealtime(cfg.sched_fifo_priority, cfg.cpu_affinity,
                                      cfg.lock_memory, kLibraryCore);
    for (const std::string &n : rt.notes)
        std::fprintf(stderr, "driver: %s\n", n.c_str());
    std::fprintf(stderr, "driver: realtime sched=%s affinity=%s mlock=%s (%s, limit %s)\n",
                 rt.scheduler_ok ? "ok" : "FAILED",
                 rt.affinity_ok ? "ok" : "FAILED",
                 rt.memlock_ok ? "ok" : "FAILED",
                 rt.memlock_future ? "current+future" : "current only",
                 rt.memlock_limit == ~0ul ? "unlimited"
                     : (std::to_string(rt.memlock_limit / 1024) + " KiB").c_str());

    // --- 4. matrix (drops privileges inside) -------------------------------
    if (!backend.Init(cfg, &err)) {
        std::fprintf(stderr, "driver: %s\n", err.c_str());
        return 1;
    }

    // The panel geometry must match what we sized the segment for. This is the
    // pixel mapper's chance to have changed things under us.
    if (backend.Width() != static_cast<int>(want_w) ||
        backend.Height() != static_cast<int>(want_h)) {
        std::fprintf(stderr,
            "driver: FATAL panel is %dx%d but the segment was sized %ux%u "
            "(a pixel_mapper_config can change this)\n",
            backend.Width(), backend.Height(), want_w, want_h);
        return 1;
    }
    if (!shm.Validate(static_cast<uint32_t>(backend.Width()),
                      static_cast<uint32_t>(backend.Height()), 3, &err)) {
        std::fprintf(stderr, "driver: FATAL %s\n", err.c_str());
        return 1;
    }

    std::fprintf(stderr,
        "driver: gamma applied here, library luminance correction OFF. "
        "At %d PWM bits, input bytes 0..%d would truncate to black; "
        "dithering is %s.\n",
        backend.PwmBits(), backend.BlackClampByte(),
        cfg.dither_enabled ? "on" : "OFF");

    signal(SIGINT, OnSignal);
    signal(SIGTERM, OnSignal);

    // --- optional diagnostics, off the hot path ----------------------------
    HotLoopStats stats;
    std::thread diag;
    if (cfg.stats_interval_s > 0) {
        diag = std::thread([&] {
            uint64_t prev_presented = 0, prev_unique = 0;
            while (!g_stop.load(std::memory_order_relaxed)) {
                for (int i = 0; i < cfg.stats_interval_s * 10 &&
                                !g_stop.load(std::memory_order_relaxed); ++i)
                    std::this_thread::sleep_for(std::chrono::milliseconds(100));
                if (g_stop.load(std::memory_order_relaxed)) break;
                const uint64_t pres = stats.presented.load(std::memory_order_relaxed);
                const uint64_t uniq = stats.unique.load(std::memory_order_relaxed);
                std::fprintf(stderr,
                    "driver: %.1f swap/s, %.1f frame/s from renderer, seq=%llu\n",
                    double(pres - prev_presented) / cfg.stats_interval_s,
                    double(uniq - prev_unique) / cfg.stats_interval_s,
                    (unsigned long long)stats.last_seq.load(std::memory_order_relaxed));
                prev_presented = pres;
                prev_unique    = uniq;
            }
        });
    }

    // --- 5. steady state ---------------------------------------------------
    HotLoopParams p;
    p.dither_enabled     = cfg.dither_enabled;
    p.dither_phases      = static_cast<uint32_t>(cfg.dither_phases);
    p.swap_every_n_vsync = static_cast<uint32_t>(cfg.swap_every_n_vsync);

    std::fprintf(stderr, "driver: running\n");
    RunHotLoop(&shm, &backend, p, &g_stop, &stats);

    std::fprintf(stderr, "driver: stopping\n");
    if (diag.joinable()) diag.join();
    backend.Shutdown();
    shm.Close();
    return 0;
}

// A resource failure -- most often std::bad_alloc when RLIMIT_MEMLOCK is small
// and MCL_FUTURE is in force -- used to surface as a bare `terminate called
// after throwing an instance of 'std::bad_alloc'` and SIGABRT, which reaches
// the hub as exit code 134 with nothing to act on. Catch it here so the
// failure names itself and exits cleanly.
int main(int argc, char **argv) {
    try {
        return RunDriver(argc, argv);
    } catch (const std::bad_alloc &) {
        std::fprintf(stderr,
            "driver: FATAL out of memory during startup.\n"
            "        If RLIMIT_MEMLOCK is small (systemd defaults to 8 MiB) and\n"
            "        memory locking is on, every allocation is charged against\n"
            "        it. Add LimitMEMLOCK=infinity to the unit, or set\n"
            "        realtime.lock_memory=false in driver.json.\n");
        return 1;
    } catch (const std::exception &e) {
        std::fprintf(stderr, "driver: FATAL %s\n", e.what());
        return 1;
    }
}
