#include "renderer_config.h"

#include <fstream>

#include <json.hpp>

using json = nlohmann::json;

namespace led {

namespace {
std::string DirOf(const std::string &p) {
    const size_t s = p.rfind('/');
    return (s == std::string::npos) ? std::string(".") : p.substr(0, s);
}
}  // namespace

bool LoadRendererConfig(const std::string &path, RendererConfig *cfg, std::string *err) {
    std::ifstream f(path);
    if (!f.is_open()) { *err = "cannot open " + path; return false; }

    json j;
    try {
        j = json::parse(f, nullptr, true, /*ignore_comments=*/true);
    } catch (const std::exception &e) {
        *err = std::string("parse error in ") + path + ": " + e.what();
        return false;
    }

    try {
        if (j.contains("shm") && j["shm"].contains("path"))
            cfg->shm_path = j["shm"]["path"].get<std::string>();
        if (j.contains("target_fps")) cfg->target_fps = j["target_fps"].get<double>();
        if (j.contains("wait_for_shm_s")) cfg->wait_for_shm_s = j["wait_for_shm_s"].get<int>();

        if (j.contains("layout")) {
            std::string p = j["layout"].get<std::string>();
            // Relative layout paths resolve against the config file, so the
            // renderer works from any working directory (systemd gives it /).
            if (!p.empty() && p[0] != '/') p = DirOf(path) + "/" + p;
            cfg->layout_path = p;
        }

        if (j.contains("database")) {
            const json &d = j["database"];
            if (d.contains("host"))     cfg->db_host = d["host"].get<std::string>();
            if (d.contains("port"))     cfg->db_port = d["port"].is_string()
                                            ? d["port"].get<std::string>()
                                            : std::to_string(d["port"].get<int>());
            if (d.contains("dbname"))   cfg->db_name = d["dbname"].get<std::string>();
            if (d.contains("user"))     cfg->db_user = d["user"].get<std::string>();
            if (d.contains("password")) cfg->db_password = d["password"].get<std::string>();
            if (d.contains("eva"))      cfg->eva = d["eva"].get<int>();
            if (d.contains("refresh_seconds"))
                cfg->refresh_seconds = d["refresh_seconds"].get<int>();
        }
    } catch (const std::exception &e) {
        *err = std::string("bad value in ") + path + ": " + e.what();
        return false;
    }

    if (cfg->target_fps <= 0.0 || cfg->target_fps > 240.0) {
        *err = "target_fps must be in (0, 240]";
        return false;
    }
    if (cfg->refresh_seconds < 1) { *err = "database.refresh_seconds must be >= 1"; return false; }
    return true;
}

}  // namespace led
