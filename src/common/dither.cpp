#include "dither.h"

#include "gamma.h"

namespace led {

void TemporalDither::Configure(int pwm_bits, int phases, bool enabled) {
    if (pwm_bits < 1) pwm_bits = 1;
    if (pwm_bits > 8) pwm_bits = 8;
    if (phases  < 1)  phases  = 1;
    if (phases  > kBayerCount) phases = kBayerCount;

    enabled_       = enabled;
    pwm_bits_      = pwm_bits;
    phases_        = phases;
    shift_         = kLibBitPlanes - pwm_bits;
    residual_mask_ = (1u << shift_) - 1u;
    max_level_     = (1 << pwm_bits) - 1;

    // Each phase rotates the Bayer thresholds so a given pixel sees a different
    // one on every frame of the cycle. Thresholds are scaled from 0..15 into
    // residual space (0..2^shift-1) and offset by half a step, so a residual of
    // exactly r/16 of the way up steps on exactly r of the 16 phases -- that
    // half-step is what keeps the time-average unbiased instead of consistently
    // rounding one way.
    const uint32_t span = residual_mask_ + 1u;
    for (int p = 0; p < phases_; ++p) {
        for (int y = 0; y < kBayerN; ++y) {
            for (int x = 0; x < kBayerN; ++x) {
                const int base = kBayer4x4[y][x];
                // Rotate through the 16 threshold values as the phase advances.
                const int rotated = (base + p * (kBayerCount / phases_)) % kBayerCount;
                threshold_[p * kBayerCount + y * kBayerN + x] =
                    (static_cast<uint32_t>(rotated) * 2u + 1u) * span / (2u * kBayerCount);
            }
        }
    }
}

}  // namespace led
