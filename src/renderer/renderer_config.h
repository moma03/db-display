#ifndef LED_RENDERER_RENDERER_CONFIG_H
#define LED_RENDERER_RENDERER_CONFIG_H

#include <string>

namespace led {

struct RendererConfig {
    std::string shm_path    = "/led-frames";
    std::string layout_path = "config/layout.json";
    double      target_fps  = 30.0;

    // Seconds to wait for the driver to create the shared-memory segment
    // before giving up. The driver owns the segment because it knows the
    // authoritative panel geometry.
    int wait_for_shm_s = 30;

    // --- data source ---
    std::string db_host   = "localhost";
    std::string db_port   = "5442";
    std::string db_name   = "stationsdb";
    std::string db_user;
    std::string db_password;
    int         eva       = 8000297;
    int         refresh_seconds = 30;
};

bool LoadRendererConfig(const std::string &path, RendererConfig *cfg, std::string *err);

}  // namespace led

#endif  // LED_RENDERER_RENDERER_CONFIG_H
