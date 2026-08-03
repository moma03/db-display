#include "gamma.h"

#include <cmath>

namespace led {

bool ParseGammaMode(const std::string &s, GammaMode *out) {
    if (s == "cie1931") { *out = GammaMode::kCie1931; return true; }
    if (s == "power")   { *out = GammaMode::kPower;   return true; }
    if (s == "linear")  { *out = GammaMode::kLinear;  return true; }
    return false;
}

const char *GammaModeName(GammaMode m) {
    switch (m) {
    case GammaMode::kCie1931: return "cie1931";
    case GammaMode::kPower:   return "power";
    case GammaMode::kLinear:  return "linear";
    }
    return "?";
}

void GammaLut::Build(GammaMode mode, double exponent, int brightness_percent,
                     int pwm_bits) {
    if (brightness_percent < 1)   brightness_percent = 1;
    if (brightness_percent > 100) brightness_percent = 100;
    if (pwm_bits < 1) pwm_bits = 1;
    if (pwm_bits > 8) pwm_bits = 8;

    pwm_bits_ = pwm_bits;
    shift_    = kLibBitPlanes - pwm_bits;

    const double scale = brightness_percent / 100.0;
    const double full  = static_cast<double>(max_level() << shift_);

    for (int c = 0; c < 256; ++c) {
        // Initialised so a future GammaMode that forgets a case here degrades
        // to black rather than to whatever was on the stack.
        double y = 0.0;  // normalised luminance in [0,1]
        switch (mode) {
        case GammaMode::kCie1931: {
            // Same curve rpi-rgb-led-matrix uses, evaluated in double and kept
            // at full 11-bit precision instead of being truncated to the top
            // `pwm_bits`. The extra precision is exactly what the temporal
            // dither spends to buy back the missing bit depth.
            const double v = c * 100.0 / 255.0;
            y = (v <= 8.0) ? (v / 902.3) : std::pow((v + 16.0) / 116.0, 3.0);
            break;
        }
        case GammaMode::kPower:
            y = std::pow(c / 255.0, exponent);
            break;
        case GammaMode::kLinear:
            y = c / 255.0;
            break;
        }
        double v = y * scale * full;
        if (v < 0.0)  v = 0.0;
        if (v > full) v = full;
        // Round rather than truncate: the dither spends the residual below this
        // value, so a systematic downward bias here would darken the whole
        // image by half a step.
        lut_[c] = static_cast<uint16_t>(std::lround(v));
    }
}

int GammaLut::BlackClampByte() const {
    int last = -1;
    for (int c = 0; c < 256; ++c)
        if ((lut_[c] >> shift_) == 0) last = c;
    return last;
}

}  // namespace led
