#ifndef DB_CONFIG_H
#define DB_CONFIG_H

#include <string>
#include <vector>

struct DBDeparture { std::string platform; std::string line; std::string dest; std::string note; std::string time; };

struct DBConfig {
    std::string station;
    std::string ticker;
    std::string font_big;
    std::string font_small;
    std::vector<DBDeparture> departures;
};

// Load DB display configuration from a JSON file. Returns true on success.
bool LoadDBConfig(const std::string &path, DBConfig &cfg);

#endif // DB_CONFIG_H
