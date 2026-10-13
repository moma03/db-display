#include "departures.h"

#include <algorithm>
#include <cctype>
#include <functional>
#include <limits>
#include <unordered_map>

namespace led {

bool operator==(const Note &a, const Note &b) {
    return a.id == b.id && a.text == b.text;
}

bool operator==(const ViaStop &a, const ViaStop &b) {
    return a.name == b.name && a.state == b.state;
}

// Used to decide whether a database refresh actually changed anything.
// An unchanged refresh must not rebuild the scrollers, because that resets
// every scroll position and makes the board visibly jump.
bool operator==(const Departure &a, const Departure &b) {
    return a.platform == b.platform && a.cPlatform == b.cPlatform &&
           a.line == b.line && a.dest == b.dest &&
           a.pTime == b.pTime && a.cTime == b.cTime &&
           a.stops == b.stops && a.cancelled == b.cancelled &&
           a.category == b.category && a.destChanged == b.destChanged &&
           a.wings == b.wings && a.wingCount == b.wingCount &&
           a.wingPos == b.wingPos && a.notes == b.notes &&
           a.departAt == b.departAt && a.route == b.route;
}

static bool StartsWithBus(const std::string &s) {
    return s.size() >= 3 &&
           std::tolower(static_cast<unsigned char>(s[0])) == 'b' &&
           std::tolower(static_cast<unsigned char>(s[1])) == 'u' &&
           std::tolower(static_cast<unsigned char>(s[2])) == 's';
}

bool IsBus(const Departure &d) {
    return StartsWithBus(d.category) || StartsWithBus(d.line);
}

const std::string &PlatformShown(const Departure &d) {
    return (!d.cPlatform.empty() && d.cPlatform != d.platform) ? d.cPlatform : d.platform;
}

static const std::string kBusLabel = "BUS";

const std::string &PlatformLabel(const Departure &d) {
    const std::string &plat = PlatformShown(d);
    if (plat.empty() && IsBus(d)) return kBusLabel;
    return plat;
}

std::string JoinNotes(const Departure &d) {
    std::string s;
    for (size_t n = 0; n < d.notes.size(); ++n) {
        if (n > 0) s += " | ";
        s += d.notes[n].text;
    }
    return s;
}

// Trip id = the stop_id without its trailing "-<stopIndex>". The daily trip id
// may itself be negative (leading '-'), so strip only the final segment.
static std::string TripIdOf(const std::string &stop_id) {
    const size_t p = stop_id.rfind('-');
    return (p == std::string::npos) ? stop_id : stop_id.substr(0, p);
}

static std::vector<std::string> ParseWings(const std::string &wings) {
    std::vector<std::string> out;
    size_t start = 0;
    while (start < wings.size()) {
        const size_t bar = wings.find('|', start);
        const size_t end = (bar == std::string::npos) ? wings.size() : bar;
        if (end > start) out.push_back(wings.substr(start, end - start));
        if (bar == std::string::npos) break;
        start = bar + 1;
    }
    return out;
}

void GroupWings(std::vector<Departure> &list) {
    const int n = static_cast<int>(list.size());
    if (n < 2) return;

    // Union-find over departure indices.
    std::vector<int> parent(n);
    for (int i = 0; i < n; ++i) parent[i] = i;
    std::function<int(int)> find = [&](int x) {
        while (parent[x] != x) { parent[x] = parent[parent[x]]; x = parent[x]; }
        return x;
    };
    auto unite = [&](int a, int b) { parent[find(a)] = find(b); };

    std::unordered_map<std::string, int> byTrip;
    for (int i = 0; i < n; ++i)
        if (!list[i].stop_id.empty())
            byTrip[TripIdOf(list[i].stop_id)] = i;

    for (int i = 0; i < n; ++i)
        for (const std::string &w : ParseWings(list[i].wings)) {
            auto it = byTrip.find(w);
            if (it != byTrip.end() && it->second != i) unite(i, it->second);
        }

    // Rebuild in the original order, emitting each group's members together
    // the first time the group is encountered.
    std::vector<Departure> out;
    out.reserve(n);
    std::vector<bool> done(n, false);
    for (int i = 0; i < n; ++i) {
        if (done[i]) continue;
        const int root = find(i);
        int pos = 0, count = 0;
        for (int j = i; j < n; ++j) if (find(j) == root) ++count;
        for (int j = i; j < n; ++j) {
            if (find(j) != root) continue;
            done[j] = true;
            list[j].wingCount = count;
            list[j].wingPos   = pos++;
            out.push_back(std::move(list[j]));
        }
    }
    list = std::move(out);
}

int HeroGroupSize(const std::vector<Departure> &list) {
    if (list.empty()) return 0;
    return std::min(list[0].wingCount, static_cast<int>(list.size()));
}

void PruneDeparted(std::vector<Departure> &list, int64_t now, int grace_s) {
    list.erase(std::remove_if(list.begin(), list.end(),
                              [&](const Departure &d) {
                                  return d.departAt > 0 && now >= d.departAt + grace_s;
                              }),
               list.end());
}

int64_t NextExpiry(const std::vector<Departure> &list, int grace_s) {
    int64_t next = std::numeric_limits<int64_t>::max();
    for (const Departure &d : list)
        if (d.departAt > 0) next = std::min(next, d.departAt + grace_s);
    return next;
}

std::string DepartureKey(const Departure &d) {
    if (!d.stop_id.empty()) return d.stop_id;
    return d.line + '\x1f' + d.dest + '\x1f' + d.pTime;
}

bool SameTrains(const std::vector<Departure> &a, const std::vector<Departure> &b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i)
        if (DepartureKey(a[i]) != DepartureKey(b[i])) return false;
    return true;
}

bool SameShape(const Departure &a, const Departure &b) {
    return DepartureKey(a) == DepartureKey(b) &&
           a.dest == b.dest && a.destChanged == b.destChanged &&
           a.stops == b.stops && a.notes == b.notes &&
           a.cancelled == b.cancelled && IsBus(a) == IsBus(b) &&
           a.wingCount == b.wingCount && a.wingPos == b.wingPos &&
           a.route == b.route;
}

std::vector<std::string> SplitPath(const std::string &path) {
    std::vector<std::string> out;
    size_t start = 0;
    while (start <= path.size()) {
        const size_t bar = path.find('|', start);
        const size_t end = (bar == std::string::npos) ? path.size() : bar;
        if (end > start) out.push_back(path.substr(start, end - start));
        if (bar == std::string::npos) break;
        start = bar + 1;
    }
    return out;
}

std::vector<ViaStop> DiffRoute(const std::vector<std::string> &planned,
                               const std::vector<std::string> &changed) {
    if (planned == changed || planned.empty() || changed.empty()) return {};
    const size_t n = planned.size(), m = changed.size();
    // lcs[i][j]: common stops of planned[i..] and changed[j..]. Routes are a
    // few dozen stops, so the table is small.
    std::vector<std::vector<int>> lcs(n + 1, std::vector<int>(m + 1, 0));
    for (size_t i = n; i-- > 0;)
        for (size_t j = m; j-- > 0;)
            lcs[i][j] = planned[i] == changed[j] ? lcs[i + 1][j + 1] + 1
                                                 : std::max(lcs[i + 1][j], lcs[i][j + 1]);
    std::vector<ViaStop> out;
    size_t i = 0, j = 0;
    while (i < n || j < m) {
        if (i < n && j < m && planned[i] == changed[j]) {
            out.push_back({planned[i], ViaStop::State::kServed});
            ++i; ++j;
        } else if (j == m || (i < n && lcs[i + 1][j] >= lcs[i][j + 1])) {
            // Dropped stops first, then their replacements.
            out.push_back({planned[i], ViaStop::State::kCancelled});
            ++i;
        } else {
            out.push_back({changed[j], ViaStop::State::kAdded});
            ++j;
        }
    }
    return out;
}

RouteChange ClassifyRoute(const std::vector<ViaStop> &route) {
    bool any = false, served_after_cancel = false, cancelled_seen = false;
    for (const ViaStop &s : route) {
        if (s.state == ViaStop::State::kAdded) return RouteChange::kChanged;
        if (s.state == ViaStop::State::kCancelled) { any = cancelled_seen = true; continue; }
        if (cancelled_seen) served_after_cancel = true;
    }
    if (!any) return RouteChange::kNone;
    // Only the tail dropped: the train ends early.
    return served_after_cancel ? RouteChange::kChanged : RouteChange::kShortened;
}

void ApplyRouteChange(Departure &d, const std::string &planned_path,
                      const std::string &changed_path) {
    d.route = DiffRoute(SplitPath(planned_path), SplitPath(changed_path));
    if (ClassifyRoute(d.route) != RouteChange::kChanged) return;
    static const char *kText = "Heute anderer Zuglauf";
    for (const Note &n : d.notes)
        if (n.text == kText) return;
    // First, ahead of any delay reason: it is what changes the trip.
    d.notes.insert(d.notes.begin(), Note{-2, kText});
}

Departure NoDeparturesPlaceholder() {
    Departure d;
    d.line  = "---";
    d.dest  = "Keine Abfahrten";
    d.pTime = "--:--";
    return d;
}

}  // namespace led
