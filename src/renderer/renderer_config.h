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

    // Where credentials come from. Empty = search the default locations (see
    // LoadRendererConfig). Credentials are NOT read from renderer.json.
    std::string env_file;
    std::string env_file_used;   // what was actually loaded, for the log

    // --- data source ---
    // Filled from the .env / process environment, never from renderer.json.
    std::string db_host   = "localhost";
    std::string db_port   = "5442";
    std::string db_name   = "stationsdb";
    std::string db_user;
    std::string db_password;
    int         eva       = 8000297;
    // Fallback re-query interval. Normally the fetcher's NOTIFY triggers a
    // re-query as soon as new data is stored; this only matters if a
    // notification is lost or the fetcher is too old to send them.
    int         refresh_seconds = 120;
    // How often this display renews its row in display_subscriptions. Must
    // stay well below the fetcher's subscription_ttl_seconds (default 120).
    int         heartbeat_seconds = 30;
    // Names this panel in display_subscriptions. Empty = "<hostname>:<shm path>".
    std::string display_id;

    // --- departure expiry ---
    // A departure leaves the board this many seconds after its effective
    // (delayed, else planned) time, independent of the fetch cycle.
    int departure_grace_seconds = 60;
    // Changes that do not touch the hero wait for the list to page back to
    // the top, so the board does not jump mid-scroll -- but never longer than
    // this.
    int max_defer_seconds = 60;
};

bool LoadRendererConfig(const std::string &path, RendererConfig *cfg, std::string *err);

}  // namespace led

#endif  // LED_RENDERER_RENDERER_CONFIG_H
