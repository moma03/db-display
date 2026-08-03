// rgbmatrix_backend.h
// Direct-GPIO output via rpi-rgb-led-matrix.

#ifndef LED_DRIVER_RGBMATRIX_BACKEND_H
#define LED_DRIVER_RGBMATRIX_BACKEND_H

#include "dither.h"
#include "driver_config.h"
#include "gamma.h"
#include "output_backend.h"

namespace rgb_matrix {
class RGBMatrix;
class FrameCanvas;
}  // namespace rgb_matrix

namespace led {

class RgbMatrixBackend : public IOutputBackend {
public:
    ~RgbMatrixBackend() override;

    // Command-line overrides are applied on top of `cfg` before Init(): the
    // hub launches us with --led-* flags and those must win. Call this first,
    // and read PwmBits()/Width()/Height() back afterwards rather than trusting
    // the config file.
    bool ApplyCommandLine(int *argc, char ***argv, DriverConfig *cfg, std::string *err);

    bool Init(const DriverConfig &cfg, std::string *err) override;
    int  Width()  const override { return width_; }
    int  Height() const override { return height_; }
    int  PwmBits() const override { return pwm_bits_; }
    void PresentAndWait(const uint8_t *rgb888, uint32_t dither_phase) override;
    void WaitIdle() override;
    void Shutdown() override;

    // Startup diagnostics, logged before the hot loop takes over.
    int BlackClampByte() const { return gamma_.BlackClampByte(); }

private:
    rgb_matrix::RGBMatrix   *matrix_ = nullptr;
    rgb_matrix::FrameCanvas *canvas_ = nullptr;
    GammaLut       gamma_;
    TemporalDither dither_;
    int width_ = 0, height_ = 0, pwm_bits_ = 4;
    // `swap_every_n_vsync`: passed to SwapOnVSync so a reduced dither rate
    // stays locked to the panel refresh instead of drifting against it.
    unsigned framerate_fraction_ = 1;
    // Precomputed byte to hand SetPixel for each output level, so the hot path
    // does no shifting per channel. See gamma.h for why this is exact.
    uint8_t level_byte_[256] = {};
};

}  // namespace led

#endif  // LED_DRIVER_RGBMATRIX_BACKEND_H
