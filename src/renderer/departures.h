// departures.h
// The domain model the dashboard draws, and the German-railway rules that go
// with it. This stays in C++ rather than moving to layout.json: wing grouping,
// replacement-bus detection and cancellation styling are behaviour, not layout,
// and making them declarative would be a much bigger project than the one the
// layout file is solving.

#ifndef LED_RENDERER_DEPARTURES_H
#define LED_RENDERER_DEPARTURES_H

#include <string>
#include <vector>

namespace led {

struct Note { int id = 0; std::string text; };

struct Departure {
    std::string platform, cPlatform, line, dest;
    std::string pTime, cTime, stops;
    std::vector<Note> notes;
    bool cancelled = false;
    std::string category;        // "RE", "S", "ICE", "Bus", ... ("" if unknown)
    bool destChanged = false;    // destination differs from the planned one
    std::string stop_id;         // "<tripId>-<stopIndex>"
    std::string wings;           // pipe-separated trip ids of coupled parts

    // Filled in by GroupWings(): members of a wing group are made adjacent.
    int wingCount = 1;           // parts in this train's group (1 = solo)
    int wingPos   = 0;           // 0-based position within the group
};

bool operator==(const Note &a, const Note &b);
bool operator==(const Departure &a, const Departure &b);
inline bool operator!=(const Departure &a, const Departure &b) { return !(a == b); }

// A wing train ("Fluegelzug") departs coupled and splits en route, each part
// keeping a different destination. Both parts stop here at the same time on the
// same platform, as two separate departures linked by their `wings` field.
// Reorders `list` so partners are contiguous and tags each with its position,
// so the drawing code can put one bracket around them.
void GroupWings(std::vector<Departure> &list);

// Number of leading departures forming the hero's wing group.
int HeroGroupSize(const std::vector<Departure> &list);

// Replacement-bus services have no track, so their platform arrives empty and
// the display writes BUS there instead. The API reports them with category
// "Bus", but when planned_line is set the category can be missing, so a line
// name beginning with "Bus" counts too ("Bus SB1").
bool IsBus(const Departure &d);

// The platform actually shown: the changed one when it differs, else planned.
const std::string &PlatformShown(const Departure &d);
// Same, but substituting BUS where a bus has no platform.
const std::string &PlatformLabel(const Departure &d);

// Join a departure's notes into one line.
std::string JoinNotes(const Departure &d);

// Placeholder shown when the fetcher returns nothing, so the renderer always
// has at least one row and the layout code never sees an empty list.
Departure NoDeparturesPlaceholder();

}  // namespace led

#endif  // LED_RENDERER_DEPARTURES_H
