// rt_setup.h
// Real-time configuration for the driver's hot loop.
//
// ORDERING MATTERS. rpi-rgb-led-matrix drops privileges to `daemon` inside
// CreateFromOptions() once it has the GPIO mapped. sched_setscheduler() needs
// CAP_SYS_NICE (or an rtprio rlimit) and mlockall() needs CAP_IPC_LOCK (or a
// memlock rlimit), so both have to happen BEFORE the matrix is created. The
// shared-memory mapping is established earlier still and survives the drop,
// because an existing mmap is unaffected by a change of uid.
//
// A NOTE ON CPU AFFINITY, which is easy to get backwards:
//
//   The library already runs its own GPIO update thread at SCHED_FIFO 99
//   pinned to core 3 (verified on the target: tid 19325, FIFO 99, cpus=3).
//   That thread is the one doing the timing-critical bit-banging. Pinning the
//   driver's hot loop to the SAME core makes things worse, not better -- the
//   quantise pass is real work and would be competing with the bit-banger for
//   the one core that must never be interrupted.
//
//   So `cpu_affinity` defaults to -1 (leave the mask alone). With isolcpus=3
//   set, that already keeps the hot loop on cores 0-2 and off the library's
//   core. Setting it to 3 is almost certainly a mistake and the driver warns.
//
// The priority is likewise deliberately below 99: if the hot loop ever does
// land on the library's core, the bit-banger must still win.

#ifndef LED_DRIVER_RT_SETUP_H
#define LED_DRIVER_RT_SETUP_H

#include <string>
#include <vector>

namespace led {

// Below this much RLIMIT_MEMLOCK, MCL_FUTURE is refused: locking every future
// allocation against a small cap turns an ordinary `new` into an uncaught
// std::bad_alloc. 64 MiB leaves ample room for the matrix library's buffers on
// the largest panel this is likely to drive.
inline constexpr unsigned long kMinMemlockForFuture = 64ul * 1024 * 1024;

struct RtReport {
    bool scheduler_ok = false;
    bool affinity_ok  = false;
    bool memlock_ok   = false;
    bool memlock_future = false;        // false => only current pages locked
    unsigned long memlock_limit = 0;    // RLIMIT_MEMLOCK after our attempt to raise it
    std::vector<std::string> notes;     // warnings and remediation hints
};

// Apply real-time settings to the calling thread. Never fatal on its own: a
// driver running at normal priority is degraded but functional, and refusing
// to start would be worse. Everything that failed lands in `report.notes` so
// the caller can log it loudly at startup.
RtReport ApplyRealtime(int fifo_priority, int cpu_affinity, bool lock_memory,
                       int library_core);

}  // namespace led

#endif  // LED_DRIVER_RT_SETUP_H
