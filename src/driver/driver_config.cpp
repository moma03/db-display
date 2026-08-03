#include "driver_config.h"

#include <fstream>
#include <set>
#include <sstream>

#include <json.hpp>

using json = nlohmann::json;

namespace led {

namespace {

// Pull a value if present, and record which keys we understood so anything
// left over can be reported as a typo.
template <typename T>
void Get(const json &j, const char *key, T *out, std::set<std::string> *seen) {
    seen->insert(key);
    if (j.contains(key) && !j[key].is_null()) *out = j[key].get<T>();
}

void ReportUnknown(const json &obj, const std::set<std::string> &seen,
                   const char *section, std::ostringstream *warn) {
    for (auto it = obj.begin(); it != obj.end(); ++it)
        if (!seen.count(it.key()))
            *warn << "  unknown key: " << section << "." << it.key() << "\n";
}

}  // namespace

bool LoadDriverConfig(const std::string &path, DriverConfig *cfg, std::string *err) {
    std::ifstream f(path);
    if (!f.is_open()) { *err = "cannot open " + path; return false; }

    json j;
    try {
        // ignore_comments = true: JSON with // comments, so the config can
        // explain itself without a YAML dependency.
        j = json::parse(f, nullptr, true, true);
    } catch (const std::exception &e) {
        *err = std::string("parse error in ") + path + ": " + e.what();
        return false;
    }

    std::ostringstream warn;
    try {
        if (j.contains("shm")) {
            const json &s = j["shm"];
            std::set<std::string> seen;
            Get(s, "path", &cfg->shm_path, &seen);
            Get(s, "slot_count", &cfg->slot_count, &seen);
            ReportUnknown(s, seen, "shm", &warn);
        }
        if (j.contains("matrix")) {
            const json &m = j["matrix"];
            std::set<std::string> seen;
            Get(m, "rows", &cfg->rows, &seen);
            Get(m, "cols", &cfg->cols, &seen);
            Get(m, "chain_length", &cfg->chain_length, &seen);
            Get(m, "parallel", &cfg->parallel, &seen);
            Get(m, "multiplexing", &cfg->multiplexing, &seen);
            Get(m, "pwm_bits", &cfg->pwm_bits, &seen);
            Get(m, "pwm_lsb_nanoseconds", &cfg->pwm_lsb_nanoseconds, &seen);
            Get(m, "gpio_slowdown", &cfg->gpio_slowdown, &seen);
            Get(m, "brightness", &cfg->brightness, &seen);
            Get(m, "limit_refresh_rate_hz", &cfg->limit_refresh_rate_hz, &seen);
            Get(m, "show_refresh_rate", &cfg->show_refresh_rate, &seen);
            Get(m, "hardware_mapping", &cfg->hardware_mapping, &seen);
            Get(m, "pixel_mapper_config", &cfg->pixel_mapper_config, &seen);
            Get(m, "led_rgb_sequence", &cfg->led_rgb_sequence, &seen);
            Get(m, "disable_hardware_pulsing", &cfg->disable_hardware_pulsing, &seen);
            ReportUnknown(m, seen, "matrix", &warn);
        }
        if (j.contains("gamma")) {
            const json &g = j["gamma"];
            std::set<std::string> seen{"mode"};
            if (g.contains("mode")) {
                const std::string mode = g["mode"].get<std::string>();
                if (!ParseGammaMode(mode, &cfg->gamma_mode)) {
                    *err = "gamma.mode must be cie1931|power|linear, got '" + mode + "'";
                    return false;
                }
            }
            Get(g, "exponent", &cfg->gamma_exponent, &seen);
            Get(g, "brightness_percent", &cfg->brightness, &seen);
            ReportUnknown(g, seen, "gamma", &warn);
        }
        if (j.contains("dither")) {
            const json &d = j["dither"];
            std::set<std::string> seen;
            Get(d, "enabled", &cfg->dither_enabled, &seen);
            Get(d, "phases", &cfg->dither_phases, &seen);
            Get(d, "swap_every_n_vsync", &cfg->swap_every_n_vsync, &seen);
            ReportUnknown(d, seen, "dither", &warn);
        }
        if (j.contains("realtime")) {
            const json &r = j["realtime"];
            std::set<std::string> seen;
            Get(r, "sched_fifo_priority", &cfg->sched_fifo_priority, &seen);
            Get(r, "cpu_affinity", &cfg->cpu_affinity, &seen);
            Get(r, "lock_memory", &cfg->lock_memory, &seen);
            Get(r, "drop_privileges", &cfg->drop_privileges, &seen);
            ReportUnknown(r, seen, "realtime", &warn);
        }
        if (j.contains("diagnostics")) {
            const json &d = j["diagnostics"];
            std::set<std::string> seen;
            Get(d, "stats_interval_s", &cfg->stats_interval_s, &seen);
            ReportUnknown(d, seen, "diagnostics", &warn);
        }
    } catch (const std::exception &e) {
        *err = std::string("bad value in ") + path + ": " + e.what();
        return false;
    }

    // --- validation: fail loudly at startup, never mid-run ---
    if (cfg->pwm_bits < 1 || cfg->pwm_bits > 8) {
        // Above 8 the exact level<->byte mapping in gamma.h stops being onto,
        // because an 8-bit Canvas API cannot address 2^9 levels.
        *err = "matrix.pwm_bits must be 1..8 (got " + std::to_string(cfg->pwm_bits) +
               "); above 8 the backend needs a direct framebuffer path";
        return false;
    }
    if (cfg->slot_count < kMinSlots || cfg->slot_count > kMaxSlots) {
        *err = "shm.slot_count must be " + std::to_string(kMinSlots) + ".." +
               std::to_string(kMaxSlots);
        return false;
    }
    if (cfg->dither_phases < 1 || cfg->dither_phases > kBayerCount ||
        kBayerCount % cfg->dither_phases != 0) {
        // A phase count that does not divide the 16 Bayer thresholds leaves
        // some pixels permanently biased up or down.
        *err = "dither.phases must divide " + std::to_string(kBayerCount) +
               " (1, 2, 4, 8 or 16)";
        return false;
    }
    if (cfg->swap_every_n_vsync < 1) {
        *err = "dither.swap_every_n_vsync must be >= 1";
        return false;
    }
    if (cfg->brightness < 1 || cfg->brightness > 100) {
        *err = "brightness must be 1..100";
        return false;
    }

    if (!warn.str().empty()) *err = "warnings:\n" + warn.str();
    return true;
}

std::string DescribeDriverConfig(const DriverConfig &cfg) {
    std::ostringstream o;
    o << "shm      " << cfg.shm_path << " (" << cfg.slot_count << " slots)\n"
      << "panel    " << cfg.cols * cfg.chain_length << "x" << cfg.rows * cfg.parallel
      << "  rows=" << cfg.rows << " cols=" << cfg.cols
      << " chain=" << cfg.chain_length << " parallel=" << cfg.parallel << "\n"
      << "pwm      " << cfg.pwm_bits << " bits, lsb=" << cfg.pwm_lsb_nanoseconds
      << "ns, slowdown=" << cfg.gpio_slowdown
      << ", limit=" << cfg.limit_refresh_rate_hz << "Hz\n"
      << "gamma    " << GammaModeName(cfg.gamma_mode)
      << " (exp=" << cfg.gamma_exponent << "), brightness " << cfg.brightness
      << "% -- applied here, library correction OFF\n"
      << "dither   " << (cfg.dither_enabled ? "on" : "off")
      << ", phases=" << cfg.dither_phases
      << ", swap every " << cfg.swap_every_n_vsync << " vsync\n"
      << "realtime SCHED_FIFO " << cfg.sched_fifo_priority
      << ", cpu " << cfg.cpu_affinity
      << ", mlockall " << (cfg.lock_memory ? "yes" : "no");
    return o.str();
}

}  // namespace led
