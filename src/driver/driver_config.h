// driver_config.h
// Startup-only configuration. Nothing here is read from the hot path.

#ifndef LED_DRIVER_DRIVER_CONFIG_H
#define LED_DRIVER_DRIVER_CONFIG_H

#include <string>

#include "gamma.h"

namespace led {

struct DriverConfig {
    // --- shared memory ---
    std::string shm_path   = "/led-frames";
    uint32_t    slot_count = 3;

    // --- matrix geometry; defaults are the current hardware ---
    int  rows                  = 64;
    int  cols                  = 128;
    int  chain_length          = 2;
    int  parallel              = 2;
    int  multiplexing          = 0;
    int  pwm_bits              = 4;
    int  pwm_lsb_nanoseconds   = 130;
    int  gpio_slowdown         = 2;
    int  brightness            = 100;   // applied in our gamma LUT, not by the library
    int  limit_refresh_rate_hz = 190;
    bool show_refresh_rate     = false; // see README: this printf()s from the RT loop
    std::string hardware_mapping = "regular";
    std::string pixel_mapper_config;
    std::string led_rgb_sequence = "RGB";
    bool disable_hardware_pulsing = false;

    // --- gamma (see gamma.h for why it lives here and not in the library) ---
    GammaMode gamma_mode      = GammaMode::kCie1931;
    double    gamma_exponent  = 2.2;

    // --- dither ---
    bool dither_enabled     = true;
    int  dither_phases      = 4;
    int  swap_every_n_vsync = 1;

    // --- real-time ---
    int  sched_fifo_priority = 80;
    int  cpu_affinity        = 3;    // -1 to leave alone
    bool lock_memory         = true;
    bool drop_privileges     = true;

    // --- diagnostics (low-priority thread, never the hot path) ---
    int  stats_interval_s = 0;   // 0 = off
};

// Load from JSON. Missing keys keep their defaults; unknown keys are reported
// so a typo in the config does not silently do nothing.
bool LoadDriverConfig(const std::string &path, DriverConfig *cfg, std::string *err);

// Human-readable dump for the startup log.
std::string DescribeDriverConfig(const DriverConfig &cfg);

}  // namespace led

#endif  // LED_DRIVER_DRIVER_CONFIG_H
