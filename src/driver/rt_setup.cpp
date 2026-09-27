#include "rt_setup.h"

#include <sched.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <string>

namespace led {

static std::string Err(const char *what) {
    return std::string(what) + " failed: " + std::strerror(errno);
}

RtReport ApplyRealtime(int fifo_priority, int cpu_affinity, bool lock_memory,
                       int library_core) {
    RtReport r;

    // --- CPU affinity ------------------------------------------------------
    if (cpu_affinity >= 0) {
        if (cpu_affinity == library_core) {
            r.notes.push_back(
                "realtime.cpu_affinity=" + std::to_string(cpu_affinity) +
                " is the core rpi-rgb-led-matrix pins its GPIO thread to at "
                "SCHED_FIFO 99. The quantise pass will compete with the "
                "bit-banger for it. Use -1 (with isolcpus) unless you are "
                "deliberately testing this.");
        }
        cpu_set_t set;
        CPU_ZERO(&set);
        CPU_SET(cpu_affinity, &set);
        if (sched_setaffinity(0, sizeof(set), &set) == 0) {
            r.affinity_ok = true;
        } else {
            r.notes.push_back(Err("sched_setaffinity"));
        }
    } else {
        // Not pinning is the default and is usually right: isolcpus already
        // keeps us off the library's core.
        r.affinity_ok = true;
    }

    // --- SCHED_FIFO --------------------------------------------------------
    if (fifo_priority > 0) {
        struct sched_param p;
        std::memset(&p, 0, sizeof(p));
        p.sched_priority = fifo_priority;
        if (sched_setscheduler(0, SCHED_FIFO, &p) == 0) {
            r.scheduler_ok = true;
        } else {
            const int e = errno;
            r.notes.push_back(Err("sched_setscheduler(SCHED_FIFO)"));
            if (e == EPERM) {
                r.notes.push_back(
                    "  -> needs CAP_SYS_NICE or LimitRTPRIO. In the systemd "
                    "unit add: AmbientCapabilities=CAP_SYS_NICE and "
                    "LimitRTPRIO=99. Running at normal priority for now.");
            }
        }
    } else {
        r.scheduler_ok = true;
    }

    // --- lock pages --------------------------------------------------------
    // The hot path must not take a page fault, so MCL_FUTURE is what we want:
    // it covers everything mapped later, including the shm segment.
    //
    // But MCL_FUTURE is a loaded gun. Once it is set, EVERY subsequent
    // allocation must be locked, and locked memory is capped by
    // RLIMIT_MEMLOCK. When the cap is hit, the allocation fails -- which in
    // C++ means operator new throws std::bad_alloc, far away from here, in
    // whatever code happened to allocate next.
    //
    // That is not hypothetical. systemd's DefaultLimitMEMLOCK is 8 MiB, while
    // an interactive root shell on this host gets 125 MiB. So this worked in
    // every hand-run test and aborted the moment the hub launched it: the
    // matrix library allocates its bitplane buffers (~720 KiB per canvas) plus
    // the pixel designator map right after this returns, crossed 8 MiB, and
    // died with an uncaught bad_alloc (exit 134).
    //
    // So: raise our own limit first, while we still have CAP_SYS_RESOURCE, and
    // only take MCL_FUTURE if the resulting headroom is actually generous.
    // Otherwise lock what exists and say plainly why the rest was skipped -- a
    // driver that page-faults occasionally beats one that aborts at startup.
    if (lock_memory) {
        rlimit before{};
        getrlimit(RLIMIT_MEMLOCK, &before);

        rlimit want{};
        want.rlim_cur = RLIM_INFINITY;
        want.rlim_max = RLIM_INFINITY;
        if (setrlimit(RLIMIT_MEMLOCK, &want) != 0) {
            // No CAP_SYS_RESOURCE: settle for raising the soft limit to the
            // hard one, which never needs privilege.
            want.rlim_cur = before.rlim_max;
            want.rlim_max = before.rlim_max;
            setrlimit(RLIMIT_MEMLOCK, &want);
        }

        rlimit have{};
        getrlimit(RLIMIT_MEMLOCK, &have);
        r.memlock_limit = have.rlim_cur;

        const bool generous = (have.rlim_cur == RLIM_INFINITY) ||
                              (have.rlim_cur >= kMinMemlockForFuture);

        const int flags = generous ? (MCL_CURRENT | MCL_FUTURE) : MCL_CURRENT;
        r.memlock_future = generous;

        if (!generous) {
            r.notes.push_back(
                "RLIMIT_MEMLOCK is only " + std::to_string(have.rlim_cur / 1024) +
                " KiB, so MCL_FUTURE was SKIPPED -- with it set, the matrix "
                "library's allocations would fail and throw std::bad_alloc. "
                "Add LimitMEMLOCK=infinity to the unit (or run the driver from "
                "deploy/led-driver.service) to get fully locked memory.");
        }

        if (mlockall(flags) == 0) {
            r.memlock_ok = true;
        } else {
            const int e = errno;
            r.memlock_future = false;
            r.notes.push_back(Err("mlockall"));
            if (e == EPERM || e == ENOMEM) {
                r.notes.push_back(
                    "  -> needs CAP_IPC_LOCK or LimitMEMLOCK=infinity. "
                    "Page faults in the hot path are possible.");
            }
        }
    } else {
        r.memlock_ok = true;
    }

    return r;
}

}  // namespace led
