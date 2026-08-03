#include "hot_loop.h"

namespace led {

// ===========================================================================
// The entire steady state of the driver.
//
// Invariants this loop maintains, and which are the reason it stays this
// small:
//
//   * It never blocks on the renderer. TryAcquireFrame is a single acquire
//     load; if nothing new has been published it returns immediately and the
//     last good frame stays on the panel.
//   * It never allocates, logs, or makes a syscall. The only blocking point is
//     inside the backend -- SwapOnVSync for the GPIO backend -- which is also
//     the only thing pacing it. WaitIdle's sleep is the sole exception and is
//     reached only when there is genuinely nothing to do.
//   * It holds a pointer into the shm slot for the whole quantise pass without
//     copying. MarkReading tells the renderer to stay off that slot, which
//     with three slots makes a tear impossible rather than unlikely.
//   * A renderer that dies, hangs, or restarts changes nothing here: the
//     sequence simply stops advancing and the last frame keeps being shown.
// ===========================================================================
void RunHotLoop(FrameShm *shm, IOutputBackend *out, const HotLoopParams &p,
                const std::atomic<bool> *stop, HotLoopStats *stats) {
    uint64_t       last_seq = 0;
    uint32_t       slot     = 0;
    const uint8_t *frame    = nullptr;   // last good frame; never null once set
    uint32_t       phase    = 0;
    bool           fresh    = false;     // frame changed since the last present

    uint64_t presented = 0, unique = 0, starved = 0;

    while (!stop->load(std::memory_order_relaxed)) {
        // --- pick up a new frame if the renderer published one -------------
        if (shm->TryAcquireFrame(&last_seq, &slot)) {
            frame = shm->slot(slot);
            shm->MarkReading(slot);
            fresh = true;
            ++unique;
        } else {
            ++starved;
        }

        // Nothing has ever been published. Idle rather than push black: a
        // driver started before the renderer should leave the panel alone,
        // not blank whatever is on it.
        //
        // With dithering off and no new frame there is likewise nothing to do
        // -- the panel is already scanning the right pixels. Both cases wait
        // out a vsync without touching the canvas, so an idle driver costs
        // essentially nothing.
        //
        // `swap_every_n_vsync` is not handled here: it is passed to
        // SwapOnVSync as the framerate fraction, so the library holds each
        // frame for that many refreshes and blocks us for the same span. That
        // keeps a reduced dither rate locked to the panel refresh instead of
        // drifting against a sleep of our own.
        if (frame == nullptr || (!p.dither_enabled && !fresh)) {
            out->WaitIdle();
            continue;
        }

        out->PresentAndWait(frame, phase);
        fresh = false;
        ++presented;
        if (p.dither_enabled && ++phase >= p.dither_phases) phase = 0;

        // Relaxed stores; the diagnostics thread is the only reader and does
        // not care about ordering.
        stats->presented.store(presented, std::memory_order_relaxed);
        stats->unique.store(unique, std::memory_order_relaxed);
        stats->starved.store(starved, std::memory_order_relaxed);
        stats->last_seq.store(last_seq, std::memory_order_relaxed);
        shm->header()->frames_presented.store(presented, std::memory_order_relaxed);
    }
}

}  // namespace led
