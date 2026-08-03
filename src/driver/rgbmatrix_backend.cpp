#include "rgbmatrix_backend.h"

#include <led-matrix.h>

#include <cstdio>

namespace led {

using rgb_matrix::FrameCanvas;
using rgb_matrix::RGBMatrix;

RgbMatrixBackend::~RgbMatrixBackend() { Shutdown(); }

static void FillOptions(const DriverConfig &cfg, RGBMatrix::Options *m,
                        rgb_matrix::RuntimeOptions *rt) {
    m->rows                  = cfg.rows;
    m->cols                  = cfg.cols;
    m->chain_length          = cfg.chain_length;
    m->parallel              = cfg.parallel;
    m->multiplexing          = cfg.multiplexing;
    m->pwm_bits              = cfg.pwm_bits;
    m->pwm_lsb_nanoseconds   = cfg.pwm_lsb_nanoseconds;
    m->limit_refresh_rate_hz = cfg.limit_refresh_rate_hz;
    m->show_refresh_rate     = cfg.show_refresh_rate;
    m->hardware_mapping      = cfg.hardware_mapping.c_str();
    m->led_rgb_sequence      = cfg.led_rgb_sequence.c_str();
    m->disable_hardware_pulsing = cfg.disable_hardware_pulsing;
    if (!cfg.pixel_mapper_config.empty())
        m->pixel_mapper_config = cfg.pixel_mapper_config.c_str();

    // Brightness is applied in OUR gamma LUT, before quantisation. The library
    // must stay at 100 or DirectMapColor's `c * brightness / 100` breaks the
    // exact level<->byte mapping -- at brightness 50 only the bottom half of
    // the levels stay addressable. See gamma.h.
    m->brightness = 100;

    rt->gpio_slowdown   = cfg.gpio_slowdown;
    rt->drop_privileges = cfg.drop_privileges ? 1 : 0;
    rt->daemon          = 0;
}

bool RgbMatrixBackend::ApplyCommandLine(int *argc, char ***argv,
                                        DriverConfig *cfg, std::string *err) {
    RGBMatrix::Options m;
    rgb_matrix::RuntimeOptions rt;
    FillOptions(*cfg, &m, &rt);

    // Let rpi-rgb-led-matrix's own --led-* flags override the config file.
    // Recognised flags are consumed from argv.
    if (!rgb_matrix::ParseOptionsFromFlags(argc, argv, &m, &rt, true)) {
        *err = "failed to parse --led-* flags";
        return false;
    }

    // Read the effective values back. This is the whole point of doing it in
    // this order: if the hub passes --led-pwm-bits=5, a quantiser still
    // targeting the config file's 4 would put every colour on the wrong level.
    cfg->rows                  = m.rows;
    cfg->cols                  = m.cols;
    cfg->chain_length          = m.chain_length;
    cfg->parallel              = m.parallel;
    cfg->multiplexing          = m.multiplexing;
    cfg->pwm_bits              = m.pwm_bits;
    cfg->pwm_lsb_nanoseconds   = m.pwm_lsb_nanoseconds;
    cfg->limit_refresh_rate_hz = m.limit_refresh_rate_hz;
    cfg->show_refresh_rate     = m.show_refresh_rate;
    cfg->hardware_mapping      = m.hardware_mapping ? m.hardware_mapping : "regular";
    cfg->led_rgb_sequence      = m.led_rgb_sequence ? m.led_rgb_sequence : "RGB";
    cfg->disable_hardware_pulsing = m.disable_hardware_pulsing;
    if (m.pixel_mapper_config) cfg->pixel_mapper_config = m.pixel_mapper_config;
    cfg->gpio_slowdown         = rt.gpio_slowdown;
    cfg->drop_privileges       = rt.drop_privileges != 0;

    // --led-brightness is folded into our LUT rather than left with the
    // library, so it applies before quantisation instead of after. That is the
    // correct order -- it is why a dimmed panel keeps its shadow detail -- and
    // it keeps the level<->byte mapping exact.
    if (m.brightness > 0 && m.brightness <= 100) cfg->brightness = m.brightness;

    if (cfg->pwm_bits < 1 || cfg->pwm_bits > 8) {
        *err = "effective pwm_bits is " + std::to_string(cfg->pwm_bits) +
               "; must be 1..8 (see gamma.h)";
        return false;
    }
    return true;
}

bool RgbMatrixBackend::Init(const DriverConfig &cfg, std::string *err) {
    RGBMatrix::Options m;
    rgb_matrix::RuntimeOptions rt;
    FillOptions(cfg, &m, &rt);

    std::string opt_err;
    if (!m.Validate(&opt_err)) { *err = "matrix options: " + opt_err; return false; }

    // Everything below here allocates. Nothing after Init() may.
    matrix_ = RGBMatrix::CreateFromOptions(m, rt);
    if (!matrix_) { *err = "RGBMatrix::CreateFromOptions failed (need root for /dev/mem?)"; return false; }

    // ===================================================================
    // Gamma is applied exactly once, and it is applied by us. See gamma.h.
    // Turning this back on would apply the CIE curve twice.
    // ===================================================================
    matrix_->set_luminance_correct(false);
    matrix_->SetBrightness(100);

    canvas_ = matrix_->CreateFrameCanvas();
    if (!canvas_) { *err = "CreateFrameCanvas failed"; return false; }

    width_    = canvas_->width();
    height_   = canvas_->height();
    pwm_bits_ = cfg.pwm_bits;
    framerate_fraction_ = static_cast<unsigned>(cfg.swap_every_n_vsync);

    gamma_.Build(cfg.gamma_mode, cfg.gamma_exponent, cfg.brightness, pwm_bits_);
    dither_.Configure(pwm_bits_, cfg.dither_phases, cfg.dither_enabled);

    for (int L = 0; L <= dither_.max_level(); ++L)
        level_byte_[L] = ByteForLevel(static_cast<uint32_t>(L), pwm_bits_);

    return true;
}

void RgbMatrixBackend::PresentAndWait(const uint8_t *rgb888, uint32_t dither_phase) {
    // ---- hot path: no allocation, no logging, no syscalls ----
    const uint16_t *lut = gamma_.data();
    const uint8_t  *src = rgb888;
    FrameCanvas    *c   = canvas_;
    const int w = width_, h = height_;

    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            const uint32_t r = dither_.Level(lut[src[0]], x, y, dither_phase);
            const uint32_t g = dither_.Level(lut[src[1]], x, y, dither_phase);
            const uint32_t b = dither_.Level(lut[src[2]], x, y, dither_phase);
            src += 3;
            c->SetPixel(x, y, level_byte_[r], level_byte_[g], level_byte_[b]);
        }
    }
    // Blocks until the panel has finished the current refresh. This is the only
    // thing pacing the driver -- there is no sleep anywhere in the loop.
    //
    // framerate_fraction_ is `swap_every_n_vsync`: the library holds each frame
    // for that many refreshes and blocks us for the same span, so lowering the
    // dither rate stays exactly vsync-locked instead of drifting against it.
    canvas_ = matrix_->SwapOnVSync(c, framerate_fraction_);
}

void RgbMatrixBackend::WaitIdle() {
    // Nothing new to show and dithering is off. The panel is already scanning
    // the right frame, so wait out a vsync without swapping anything.
    //
    // The return value is deliberately discarded: SwapOnVSync(NULL) hands back
    // the ACTIVE canvas, and adopting that as our offscreen buffer would mean
    // drawing into the one the panel is scanning.
    (void)matrix_->SwapOnVSync(nullptr, framerate_fraction_);
}

void RgbMatrixBackend::Shutdown() {
    if (matrix_) {
        matrix_->Clear();
        delete matrix_;
        matrix_ = nullptr;
        canvas_ = nullptr;
    }
}

}  // namespace led
