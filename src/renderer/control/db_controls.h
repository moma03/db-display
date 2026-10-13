// db_controls.h
// What db-display exposes to Home Assistant through the LED_Panel_Controller
// hub (LED_Panel_Controller/docs/program-controls.md): the manifest printed by
// `--manifest`, and the state reported back over the control channel. The
// program itself has no MQTT code.

#ifndef LED_RENDERER_DB_CONTROLS_H
#define LED_RENDERER_DB_CONTROLS_H

#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

#include <json.hpp>

#include "control_channel.h"
#include "departures.h"

namespace led {

struct DbDeviceInfo {
    std::string id = "db_display";
    std::string name = "DB Abfahrtstafel";
};

// `default_station`: what the display shows before Home Assistant says
// otherwise -- the EVA number from renderer.json.
nlohmann::json BuildDbManifest(const DbDeviceInfo &device, const std::string &default_station);

struct DbDisplayState {
    std::string station;         // as asked for: EVA number, name or DS100
    std::string station_name;    // resolved; empty until known
    std::string status = "ok";   // "ok", "Bahnhof wird gesucht ...", or an error
    std::string next_departure;  // "RE 7 Kiel Hbf 04:03 (04:08)"
    int64_t last_update = 0;     // unix seconds of the last successful fetch
};

nlohmann::json DbStateValues(const DbDisplayState &s);

// "RE 7 Kiel Hbf 04:03", with the changed time in brackets when there is one:
// the first departure that is not cancelled. Empty if there is none.
std::string NextDepartureText(const std::vector<Departure> &list);

// The hub's control channel for this program: a station from Home Assistant
// goes to `on_station`, state changes go back -- only what changed.
class DbHubLink {
public:
    // `station_pinned`: --station/--eva on the command line wins over the
    // stored station the hub sends at startup (later changes from Home
    // Assistant still apply).
    DbHubLink(std::function<void(const std::string &)> on_station, bool station_pinned);

    // Opens the channel if the hub started us; false otherwise (no-op mode).
    // Set the initial station with Update() first: it is reported as soon as
    // the hub's settings arrive.
    bool Start(ControlChannel *channel);

    // Change the reported state under the link's lock, then report the diff.
    void Update(const std::function<void(DbDisplayState &)> &change);

private:
    void Report(bool everything);

    std::function<void(const std::string &)> on_station_;
    bool pinned_;
    ControlChannel *channel_ = nullptr;
    std::mutex mtx_;
    DbDisplayState state_;
    nlohmann::json last_ = nlohmann::json::object();
};

}  // namespace led

#endif  // LED_RENDERER_DB_CONTROLS_H
