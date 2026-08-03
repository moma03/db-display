#include "rt_setup.h"

#include <sched.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>

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
    // The hot path must not take a page fault. MCL_FUTURE covers anything
    // mapped later, including the shm segment if it is opened after this.
    if (lock_memory) {
        if (mlockall(MCL_CURRENT | MCL_FUTURE) == 0) {
            r.memlock_ok = true;
        } else {
            const int e = errno;
            r.notes.push_back(Err("mlockall"));
            if (e == EPERM || e == ENOMEM) {
                r.notes.push_back(
                    "  -> needs CAP_IPC_LOCK or LimitMEMLOCK=infinity in the "
                    "systemd unit. Page faults in the hot path are possible.");
            }
        }
    } else {
        r.memlock_ok = true;
    }

    return r;
}

}  // namespace led
