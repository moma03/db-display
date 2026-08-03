// dither.h
// Temporal dithering: recover perceptual bit depth the PWM depth cannot show.
//
// The renderer produces 8-bit-per-channel frames. The gamma LUT turns each
// channel into an 11-bit target (see gamma.h). The panel can only show
// `pwm_bits` of that -- 4 on the current hardware, i.e. 16 levels -- so the
// bottom 7 bits of every channel would normally be thrown away by the library,
// which truncates rather than rounds. At pwm_bits = 4 that means every input
// byte from 0 to 76 renders as pure black: 30% of the input range, gone.
//
// Instead of dropping the residual we spend it over time. For a target value
// `t`, `q = t >> shift` is the level below and `r = t & mask` the residual.
// Showing level `q+1` on a fraction r/2^shift of refreshes and `q` on the rest
// gives a time-averaged luminance of exactly `t`, provided the alternation is
// fast enough that the eye integrates it.
//
// Which refresh gets the higher level is decided by a 4x4 Bayer matrix indexed
// by (x, y, phase). Two properties matter:
//
//   * Spatially it is an ordered dither, so neighbouring pixels of the same
//     colour take the step on different refreshes. The panel as a whole never
//     steps at once, which is what a naive per-pixel threshold would do and is
//     the difference between "smoother gradient" and "the whole display
//     breathes".
//   * Temporally it cycles with `phases`, so each pixel sees every threshold in
//     turn. `phases` must divide 16 (the matrix has 16 distinct thresholds) for
//     the average to come out unbiased; the driver validates this.
//
// The alternation rate is the driver's swap rate, NOT the panel refresh rate:
// rpi-rgb-led-matrix rescans the same FrameCanvas until SwapOnVSync() replaces
// it, so content can only change once per swap. Driving this at the full 190 Hz
// costs a full-frame quantise pass per refresh; `swap_every_n_vsync` in
// driver.json trades that cost against dither quality. See README.

#ifndef LED_COMMON_DITHER_H
#define LED_COMMON_DITHER_H

#include <cstdint>

namespace led {

inline constexpr int kBayerN     = 4;
inline constexpr int kBayerCount = kBayerN * kBayerN;   // 16 thresholds

// Standard 4x4 ordered-dither matrix, values 0..15.
inline constexpr uint8_t kBayer4x4[kBayerN][kBayerN] = {
    {  0,  8,  2, 10 },
    { 12,  4, 14,  6 },
    {  3, 11,  1,  9 },
    { 15,  7, 13,  5 },
};

// Quantises 11-bit gamma-corrected targets down to `pwm_bits` levels, spending
// the residual across successive frames. No allocation, no branching on
// per-pixel data beyond the clamp -- this runs in the driver's hot path.
class TemporalDither {
public:
    // `phases` must divide kBayerCount. `pwm_bits` must be in 1..8.
    void Configure(int pwm_bits, int phases, bool enabled);

    bool enabled()  const { return enabled_; }
    int  phases()   const { return phases_; }
    int  pwm_bits() const { return pwm_bits_; }
    int  max_level() const { return max_level_; }

    // Level to display for an 11-bit target at pixel (x, y) on frame `phase`.
    // Marked inline and kept branch-light on purpose.
    inline uint32_t Level(uint16_t target, int x, int y, uint32_t phase) const {
        const uint32_t q = static_cast<uint32_t>(target) >> shift_;
        if (!enabled_) return q;
        if (q >= static_cast<uint32_t>(max_level_)) return max_level_;

        const uint32_t residual = static_cast<uint32_t>(target) & residual_mask_;
        // Threshold for this pixel on this phase, scaled into residual space.
        const uint32_t th = threshold_[(phase % phases_) * kBayerCount +
                                       ((y & 3) * kBayerN + (x & 3))];
        return q + (residual > th ? 1u : 0u);
    }

private:
    bool     enabled_       = true;
    int      pwm_bits_      = 4;
    int      phases_        = 4;
    int      shift_         = 7;     // kLibBitPlanes - pwm_bits
    uint32_t residual_mask_ = 0x7F;
    int      max_level_     = 15;
    // phases_ * 16 precomputed thresholds in residual space.
    uint32_t threshold_[kBayerCount * kBayerCount] = {};
};

}  // namespace led

#endif  // LED_COMMON_DITHER_H
