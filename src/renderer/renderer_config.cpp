#include "renderer_config.h"

#include <cstdlib>
#include <fstream>
#include <vector>

#include "dotenv.h"

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
            // Connection details and credentials deliberately do NOT come from
            // here -- see the env-file handling below. Only non-secret
            // behaviour lives in renderer.json.
            if (d.contains("eva")) cfg->eva = d["eva"].get<int>();
            if (d.contains("refresh_seconds"))
                cfg->refresh_seconds = d["refresh_seconds"].get<int>();
            if (d.contains("heartbeat_seconds"))
                cfg->heartbeat_seconds = d["heartbeat_seconds"].get<int>();
            if (d.contains("display_id"))
                cfg->display_id = d["display_id"].get<std::string>();
            for (const char *k : {"host", "port", "dbname", "user", "password"})
                if (d.contains(k))
                    return (*err = std::string("database.") + k +
                                   " must not be set in " + path +
                                   " -- database credentials belong in a .env "
                                   "file (see config/.env.example). Move it to "
                                   "DB_PG_" + [&]{
                                       std::string u(k);
                                       for (char &ch : u) ch = toupper(ch);
                                       return u;
                                   }() + " and delete it here.",
                            false);
        }
        if (j.contains("departures")) {
            const json &d = j["departures"];
            if (d.contains("grace_seconds"))
                cfg->departure_grace_seconds = d["grace_seconds"].get<int>();
            if (d.contains("max_defer_seconds"))
                cfg->max_defer_seconds = d["max_defer_seconds"].get<int>();
        }
        if (j.contains("env_file")) cfg->env_file = j["env_file"].get<std::string>();

    } catch (const std::exception &e) {
        *err = std::string("bad value in ") + path + ": " + e.what();
        return false;
    }

    // --- credentials -------------------------------------------------------
    // Search order, unless env_file / --env names one explicitly:
    //   1. $LED_ENV_FILE
    //   2. <dir of renderer.json>/.env        (config/.env)
    //   3. <dir of renderer.json>/../.env     (repo root, matches
    //                                          timetable-fetcher's placement)
    // A missing file is not an error: the values may come from the real
    // environment instead, e.g. systemd's EnvironmentFile=.
    {
        std::vector<std::string> candidates;
        if (!cfg->env_file.empty()) {
            std::string p = cfg->env_file;
            if (!p.empty() && p[0] != '/') p = DirOf(path) + "/" + p;
            candidates.push_back(p);
        } else {
            if (const char *e = std::getenv("LED_ENV_FILE")) if (*e) candidates.push_back(e);
            candidates.push_back(DirOf(path) + "/.env");
            candidates.push_back(DirOf(path) + "/../.env");
        }

        EnvMap env;
        for (const std::string &cand : candidates) {
            bool existed = false;
            std::string e;
            if (!LoadDotEnv(cand, &env, &existed, &e)) { *err = e; return false; }
            if (existed) { cfg->env_file_used = cand; break; }
        }
        // An explicitly named file that is not there is a mistake worth saying
        // out loud; the searched defaults are allowed to be absent.
        if (!cfg->env_file.empty() && cfg->env_file_used.empty()) {
            *err = "env_file '" + cfg->env_file + "' not found";
            return false;
        }

        std::string v;
        if (EnvLookup(env, "DB_PG_HOST", &v))     cfg->db_host = v;
        if (EnvLookup(env, "DB_PG_PORT", &v))     cfg->db_port = v;
        if (EnvLookup(env, "DB_PG_DBNAME", &v))   cfg->db_name = v;
        if (EnvLookup(env, "DB_PG_USER", &v))     cfg->db_user = v;
        if (EnvLookup(env, "DB_PG_PASSWORD", &v)) cfg->db_password = v;
        if (EnvLookup(env, "DB_EVA", &v))         cfg->eva = std::atoi(v.c_str());
    }

    if (cfg->target_fps <= 0.0 || cfg->target_fps > 240.0) {
        *err = "target_fps must be in (0, 240]";
        return false;
    }
    if (cfg->refresh_seconds < 1) { *err = "database.refresh_seconds must be >= 1"; return false; }
    if (cfg->heartbeat_seconds < 1) { *err = "database.heartbeat_seconds must be >= 1"; return false; }
    if (cfg->departure_grace_seconds < 0) { *err = "departures.grace_seconds must be >= 0"; return false; }
    if (cfg->max_defer_seconds < 0) { *err = "departures.max_defer_seconds must be >= 0"; return false; }
    return true;
}

}  // namespace led
