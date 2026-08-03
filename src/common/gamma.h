// gamma.h
// ===========================================================================
// WHERE GAMMA LIVES  --  read this before changing anything in here.
// ===========================================================================
//
// Gamma is applied EXACTLY ONCE, HERE, in the driver. rpi-rgb-led-matrix's own
// CIE1931 correction is switched OFF (`matrix->set_luminance_correct(false)`)
// in rgbmatrix_backend.cpp. Turning it back on would apply the curve twice and
// crush the midtones.
//
// Why own it rather than let the library do it:
//
//   With luminance correction ON, the library maps an 8-bit channel through a
//   CIE1931 LUT to an 11-bit value (kBitPlanes = 11) and then displays only the
//   top `pwm_bits` planes, discarding the rest. The only way for us to command
//   a specific output level would be to search a reverse table for a byte whose
//   CIE image happens to land in the right bucket -- which means carrying a
//   duplicate of the library's formula that silently desyncs if the library
//   ever changes it, and re-deriving it whenever brightness changes.
//
//   With luminance correction OFF the library uses DirectMapColor:
//
//       value11 = (c * brightness / 100) << 3        // 8 -> 11 bits
//       level   = value11 >> (11 - pwm_bits)
//
//   Pinning the library's brightness to 100 makes that exactly `c << 3`, so
//
//       level L  <=>  byte = L << (8 - pwm_bits)
//
//   is an exact, onto mapping for pwm_bits <= 8. Every output level is
//   addressable, the arithmetic is integer-exact, and it is testable on a
//   laptop with no panel attached. Brightness is folded into our own LUT
//   instead, which also applies it *before* quantisation rather than after --
//   the correct order, and the reason a dim panel keeps its shadow detail.
//
// The `pwm_bits <= 8` constraint is asserted at startup in the driver; above
// that the 8-bit Canvas API physically cannot address every level and the
// backend must grow a direct framebuffer path.
//
// Why this matters more than it sounds: at pwm_bits = 4 the panel has 16
// levels per channel. Without correction the bottom of the range is unusable --
// see kBlackClampByte in gamma.cpp for the measured numbers.

#ifndef LED_COMMON_GAMMA_H
#define LED_COMMON_GAMMA_H

#include <cstdint>
#include <string>

namespace led {

// Number of bit planes rpi-rgb-led-matrix keeps internally. Must match
// internal::Framebuffer::kBitPlanes -- asserted against the library at startup.
inline constexpr int kLibBitPlanes = 11;
inline constexpr int kLibMaxValue  = (1 << kLibBitPlanes) - 1;   // 2047

enum class GammaMode {
    kCie1931,   // perceptually linear; matches what the library would have done
    kPower,     // classic power law, `exponent` from config
    kLinear,    // no correction, for measuring the panel
};

bool ParseGammaMode(const std::string &s, GammaMode *out);
const char *GammaModeName(GammaMode m);

// Maps an 8-bit sRGB channel to a fixed-point target in LEVEL SPACE:
// `shift = kLibBitPlanes - pwm_bits` fractional bits below an integer part
// that runs 0..max_level. So `target >> shift` is the level to display and
// `target & ((1<<shift)-1)` is the residual the dither spends over time.
//
// Full scale maps to `max_level << shift`, NOT to 2047. Scaling to 2047 would
// put full white at target/128 = 15.99 while the panel's ceiling is level 15,
// so the top of the range could never average correctly and the brightest
// pixels would sit a full level low.
//
// Brightness is baked in here, so the library must stay at brightness 100.
class GammaLut {
public:
    void Build(GammaMode mode, double exponent, int brightness_percent, int pwm_bits);

    // Fixed-point target in level space for an 8-bit input.
    uint16_t operator[](uint8_t c) const { return lut_[c]; }
    const uint16_t *data() const { return lut_; }

    int pwm_bits()  const { return pwm_bits_; }
    int shift()     const { return shift_; }
    int max_level() const { return (1 << pwm_bits_) - 1; }
    uint16_t full_scale() const { return static_cast<uint16_t>(max_level() << shift_); }

    // The highest input byte that still renders as pure black at this bit
    // depth. Purely diagnostic -- the driver logs it at startup so the cost of
    // the configured PWM depth is visible without a light meter.
    int BlackClampByte() const;

private:
    uint16_t lut_[256] = {};
    int pwm_bits_ = 4;
    int shift_    = kLibBitPlanes - 4;
};

// Byte to hand to Canvas::SetPixel so DirectMapColor produces exactly `level`.
// Valid for pwm_bits in 1..8; see the header comment for the derivation.
inline uint8_t ByteForLevel(uint32_t level, int pwm_bits) {
    return static_cast<uint8_t>(level << (8 - pwm_bits));
}

}  // namespace led

#endif  // LED_COMMON_GAMMA_H
