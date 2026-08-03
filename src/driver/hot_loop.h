// hot_loop.h
// The driver's steady state. Everything else in the driver is startup.

#ifndef LED_DRIVER_HOT_LOOP_H
#define LED_DRIVER_HOT_LOOP_H

#include <atomic>
#include <cstdint>

#include "frame_shm.h"
#include "output_backend.h"

namespace led {

struct HotLoopParams {
    bool     dither_enabled     = true;
    uint32_t dither_phases      = 4;
    uint32_t swap_every_n_vsync = 1;
};

// Counters the diagnostics thread reads. Written with relaxed stores from the
// hot loop: they are never load-bearing, and a torn read costs nothing.
struct HotLoopStats {
    std::atomic<uint64_t> presented{0};   // frames pushed to the panel
    std::atomic<uint64_t> unique{0};      // distinct renderer frames seen
    std::atomic<uint64_t> starved{0};     // passes with no new frame
    std::atomic<uint64_t> last_seq{0};
};

// Runs until `stop` is set. Blocks only inside the backend.
void RunHotLoop(FrameShm *shm, IOutputBackend *out, const HotLoopParams &p,
                const std::atomic<bool> *stop, HotLoopStats *stats);

}  // namespace led

#endif  // LED_DRIVER_HOT_LOOP_H
