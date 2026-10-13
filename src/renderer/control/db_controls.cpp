#include "db_controls.h"

#include <ctime>

namespace led {

using json = nlohmann::json;

json BuildDbManifest(const DbDeviceInfo &device, const std::string &default_station) {
    return {
        {"manifest_version", 1},
        {"device", {{"id", device.id}, {"name", device.name}, {"model", "db-display"}}},
        {"controls", json::array({
            // Anything the fetcher can resolve: an EVA number ("8002549"), a
            // station name ("Hamburg Dammtor") or a DS100 code ("AH").
            {{"key", "station"}, {"type", "text"}, {"name", "Bahnhof"},
             {"icon", "mdi:train"}, {"max", 100}, {"default", default_station}},
        })},
        {"sensors", json::array({
            {{"key", "station_name"}, {"name", "Angezeigter Bahnhof"}, {"icon", "mdi:map-marker"}},
            {{"key", "next_departure"}, {"name", "Nächste Abfahrt"}, {"icon", "mdi:clock-fast"}},
            {{"key", "status"}, {"name", "Status"}, {"icon", "mdi:information-outline"},
             {"entity_category", "diagnostic"}},
            {{"key", "last_update"}, {"name", "Letzte Aktualisierung"}, {"icon", "mdi:update"},
             {"device_class", "timestamp"}, {"entity_category", "diagnostic"}},
        })},
    };
}

json DbStateValues(const DbDisplayState &s) {
    json v = {
        {"station", s.station},
        {"station_name", s.station_name},
        {"status", s.status},
        {"next_departure", s.next_departure.empty() ? "–" : s.next_departure},
    };
    // A timestamp sensor rejects anything else, so nothing until the first fetch.
    if (s.last_update > 0) {
        const time_t t = static_cast<time_t>(s.last_update);
        struct tm g;
        gmtime_r(&t, &g);
        char buf[32];
        std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%S+00:00", &g);
        v["last_update"] = buf;
    }
    return v;
}

std::string NextDepartureText(const std::vector<Departure> &list) {
    for (const Departure &d : list) {
        if (d.cancelled || d.pTime.empty() || d.pTime == "--:--") continue;
        std::string s = d.line + " " + d.dest + " " + d.pTime;
        if (!d.cTime.empty()) s += " (" + d.cTime + ")";
        return s;
    }
    return "";
}

DbHubLink::DbHubLink(std::function<void(const std::string &)> on_station, bool station_pinned)
    : on_station_(std::move(on_station)), pinned_(station_pinned) {}

bool DbHubLink::Start(ControlChannel *channel) {
    if (!channel->Open()) return false;
    channel_ = channel;
    ControlChannel::Handlers h;
    // The reported station follows a request at once: reporting the previous
    // one first would make the hub store it over the value it just sent.
    auto take = [this](const std::string &station) {
        {
            std::lock_guard<std::mutex> lk(mtx_);
            state_.station = station;
        }
        on_station_(station);
    };
    h.on_settings = [this, take](const json &values) {
        const auto st = values.find("station");
        if (!pinned_ && st != values.end() && st->is_string() && !st->get<std::string>().empty())
            take(st->get<std::string>());
        // Everything once, so the hub learns what we actually show (a pinned
        // station) and gets the sensors.
        Report(true);
    };
    h.on_set = [this, take](const std::string &key, const json &v) {
        if (key == "station" && v.is_string() && !v.get<std::string>().empty()) {
            take(v.get<std::string>());
            Report(false);
        } else {
            Report(true);   // nothing we accept: tell the hub what we kept
        }
    };
    channel->Start(std::move(h));
    return true;
}

void DbHubLink::Update(const std::function<void(DbDisplayState &)> &change) {
    {
        std::lock_guard<std::mutex> lk(mtx_);
        change(state_);
    }
    Report(false);
}

void DbHubLink::Report(bool everything) {
    if (!channel_) return;
    std::lock_guard<std::mutex> lk(mtx_);
    if (everything) last_ = json::object();
    const json now = DbStateValues(state_);
    json diff = json::object();
    for (auto it = now.begin(); it != now.end(); ++it) {
        const auto old = last_.find(it.key());
        if (old == last_.end() || *old != it.value()) diff[it.key()] = it.value();
    }
    if (!diff.empty() && channel_->SendState(diff)) last_.update(diff);
}

}  // namespace led
