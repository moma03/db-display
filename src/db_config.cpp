#include "db_config.h"
#include <fstream>
#include <iostream>
#include <common/third_party/json/json.hpp>

using json = nlohmann::json;

bool LoadDBConfig(const std::string &path, DBConfig &cfg) {
    std::ifstream f(path);
    if (!f.is_open()) return false;
    try {
        json j = json::parse(f);
        if (j.contains("station")) cfg.station = j["station"].get<std::string>();
        if (j.contains("ticker")) cfg.ticker = j["ticker"].get<std::string>();
        if (j.contains("font_big")) cfg.font_big = j["font_big"].get<std::string>();
        if (j.contains("font_small")) cfg.font_small = j["font_small"].get<std::string>();
        if (j.contains("departures") && j["departures"].is_array()) {
            for (auto &it : j["departures"]) {
                DBDeparture d;
                if (it.contains("platform")) d.platform = it["platform"].get<std::string>();
                if (it.contains("line")) d.line = it["line"].get<std::string>();
                if (it.contains("dest")) d.dest = it["dest"].get<std::string>();
                if (it.contains("note")) d.note = it["note"].get<std::string>();
                if (it.contains("time")) d.time = it["time"].get<std::string>();
                cfg.departures.push_back(d);
            }
        }
    } catch (const std::exception &e) {
        std::cerr << "db-config parse error: " << e.what() << std::endl;
        return false;
    }
    return true;
}
