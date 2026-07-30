// db_display.cc
// Deutsche Bahn style platform display.
//
// Modes:
//   --debug   Use hardcoded dummy data (no database required)
//   default   Fetch live departure data from the PostgreSQL database
//             populated by the Python main-node fetcher.
//             Data is refreshed every DB_REFRESH_SECONDS.

#include <iostream>
#include <string>
#include <vector>
#include <unordered_map>
#include <functional>
#include <chrono>
#include <thread>
#include <mutex>
#include <atomic>
#include <cstring>
#include <cstdlib>
#include <cstdio>
#include <ctime>
#include <cctype>
#include <algorithm>
#include <csignal>
#include <unistd.h>
#if defined(__APPLE__)
#include <mach-o/dyld.h>
#endif

#include "led-matrix.h"
#include "graphics.h"
#include "config_loader.h"
#include "update_event.h"
#include "viewport.h"
#include "page_scroller.h"
#include "scrolling_textbox.h"
#include "db_fetcher.h"


using namespace rgb_matrix;
using namespace std;

// How often (seconds) to re-query the database for fresh data.
static const int DB_REFRESH_SECONDS = 30;

// Render pace, ~30 fps. Matches the other programs on this panel. Rendering
// faster gains nothing visually and starves the matrix refresh thread.
static const int FRAME_MS = 33;

// The departure list rows are drawn inside a viewport indented past the
// scrollbar; times inside it land LIST_CONTENT_X pixels further right.
static const int SCROLLBAR_X    = 1;
static const int SCROLLBAR_W    = 2;
static const int LIST_CONTENT_X = SCROLLBAR_X + SCROLLBAR_W + 1;

// Set by SIGINT/SIGTERM so the render loop can exit and clean up the panel.
static volatile sig_atomic_t interrupt_received = 0;
static void InterruptHandler(int) { interrupt_received = 1; }

// -------------------------------------------------------------------------
// Helpers: resolve paths relative to the executable so the program works
// from any working directory (e.g. when started as a systemd service).
// -------------------------------------------------------------------------
static string executableDir(const char *argv0) {
    char buf[4096];
    buf[0] = '\0';
#if defined(__linux__)
    ssize_t n = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    if (n > 0) buf[n] = '\0'; else buf[0] = '\0';
#elif defined(__APPLE__)
    uint32_t size = sizeof(buf);
    if (_NSGetExecutablePath(buf, &size) != 0) buf[0] = '\0';
#endif
    string path = buf[0] ? string(buf) : (argv0 ? string(argv0) : string());
    size_t slash = path.rfind('/');
    return (slash == string::npos) ? string(".") : path.substr(0, slash);
}

// Prefer the file next to the executable; fall back to the path as given
// (relative to the current directory, the old behaviour).
static string resolvePath(const string &exe_dir, const string &rel) {
    string cand = exe_dir + "/" + rel;
    if (access(cand.c_str(), F_OK) == 0) return cand;
    return rel;
}

// -------------------------------------------------------------------------
// Shared data types used by the renderer
// -------------------------------------------------------------------------
struct Note { int id; string text; };
struct Departure {
    string platform; string cPlatform; string line; string dest;
    string pTime; string cTime; string stops;
    vector<Note> notes; bool cancelled = false;
    string category;        // "RE", "S", "ICE", "Bus", ... (empty if unknown)
    bool destChanged = false;   // destination differs from the planned one
    string stop_id;         // "<tripId>-<stopIndex>"
    string wings;           // pipe-separated trip ids of coupled parts
    // Filled in by groupWings(): members of a wing group are made adjacent.
    int wingCount = 1;      // parts in this train's wing group (1 = solo)
    int wingPos   = 0;      // 0-based position of this part within the group
};

// Replacement-bus services have no track, so their platform arrives empty;
// the display writes BUS there instead. The timetable API reports them with
// category "Bus", but when planned_line is set the category can be missing,
// so a line name beginning with "Bus" counts too ("Bus SB1").
static bool startsWithBus(const string &s) {
    return s.size() >= 3 &&
           tolower((unsigned char)s[0]) == 'b' &&
           tolower((unsigned char)s[1]) == 'u' &&
           tolower((unsigned char)s[2]) == 's';
}
static bool isBus(const Departure &dep) {
    return startsWithBus(dep.category) || startsWithBus(dep.line);
}

// Equality is used to detect whether a DB refresh actually changed anything;
// unchanged refreshes must not rebuild the scrollers (that would reset all
// scroll positions).
static bool operator==(const Note &a, const Note &b) {
    return a.id == b.id && a.text == b.text;
}
static bool operator==(const Departure &a, const Departure &b) {
    return a.platform == b.platform && a.cPlatform == b.cPlatform &&
           a.line == b.line && a.dest == b.dest &&
           a.pTime == b.pTime && a.cTime == b.cTime &&
           a.stops == b.stops && a.cancelled == b.cancelled &&
           a.category == b.category && a.destChanged == b.destChanged &&
           a.wings == b.wings && a.wingCount == b.wingCount &&
           a.wingPos == b.wingPos && a.notes == b.notes;
}

// -------------------------------------------------------------------------
// Helper: convert FetchedDeparture → Departure
// -------------------------------------------------------------------------
static Departure fromFetched(const FetchedDeparture &fd) {
    Departure d;
    d.platform  = fd.platform;
    d.cPlatform = fd.cPlatform;
    d.line      = fd.line;
    d.dest      = fd.dest;
    d.pTime     = fd.pTime;
    d.cTime     = fd.cTime;
    d.stops     = fd.stops;
    d.cancelled   = fd.cancelled;
    d.category    = fd.category;
    d.destChanged = fd.destChanged;
    d.stop_id     = fd.stop_id;
    d.wings       = fd.wings;
    for (auto &fn : fd.notes)
        d.notes.push_back({fn.id, fn.text});
    return d;
}

// -------------------------------------------------------------------------
// Wing grouping
// -------------------------------------------------------------------------
// A wing train ("Flügelzug") departs coupled and splits en route, each part
// keeping to a different destination. Both parts stop here, at the same time
// on the same platform, as two separate departures linked by their `wings`
// field (a list of the partner's trip ids). This makes the parts adjacent in
// the list and tags each with its position in the group so the renderer can
// draw one bracket around them.

// Trip id = the stop_id without its trailing "-<stopIndex>". The daily trip id
// may itself be negative (leading '-'), so strip only the final segment.
static string tripIdOf(const string &stop_id) {
    size_t p = stop_id.rfind('-');
    return (p == string::npos) ? stop_id : stop_id.substr(0, p);
}

static vector<string> parseWings(const string &wings) {
    vector<string> out;
    size_t start = 0;
    while (start < wings.size()) {
        size_t bar = wings.find('|', start);
        size_t end = (bar == string::npos) ? wings.size() : bar;
        if (end > start) out.push_back(wings.substr(start, end - start));
        if (bar == string::npos) break;
        start = bar + 1;
    }
    return out;
}

// Reorder `list` so wing partners are contiguous (anchored at the earliest
// member, preserving the incoming time order otherwise) and fill in each
// departure's wingCount / wingPos.
static void groupWings(vector<Departure> &list) {
    const int n = (int)list.size();
    if (n < 2) return;

    // Union-find over departure indices.
    vector<int> parent(n);
    for (int i = 0; i < n; ++i) parent[i] = i;
    function<int(int)> find = [&](int x) {
        while (parent[x] != x) { parent[x] = parent[parent[x]]; x = parent[x]; }
        return x;
    };
    auto unite = [&](int a, int b) { parent[find(a)] = find(b); };

    unordered_map<string, int> byTrip;
    for (int i = 0; i < n; ++i)
        if (!list[i].stop_id.empty())
            byTrip[tripIdOf(list[i].stop_id)] = i;

    for (int i = 0; i < n; ++i)
        for (const string &w : parseWings(list[i].wings)) {
            auto it = byTrip.find(w);
            if (it != byTrip.end() && it->second != i)
                unite(i, it->second);
        }

    // Rebuild in original order, emitting each group's members together the
    // first time the group is encountered.
    vector<Departure> out;
    out.reserve(n);
    vector<bool> done(n, false);
    for (int i = 0; i < n; ++i) {
        if (done[i]) continue;
        int root = find(i), pos = 0, count = 0;
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

// Number of leading departures that form the hero's wing group.
static int heroGroupSize(const vector<Departure> &list) {
    if (list.empty()) return 0;
    return min(list[0].wingCount, (int)list.size());
}

// -------------------------------------------------------------------------
// Dummy data for --debug mode
// -------------------------------------------------------------------------
static void loadDummyData(vector<Departure> &list, string &station, string &ticker) {
    list = {
        // Wing pair as the next departure -> rendered as two big hero rows.
        {"1", "",  "RE 11", "Dortmund Hbf", "12:38", "12:40", "Kamen, Unna", {{1, "technische Störung am Zug"}}, false, "", false, "100-2507311238-3", "200-2507311238"},
        {"1", "",  "RE 11", "Iserlohn", "12:38", "12:40", "Kamen, Menden", {}, false, "", false, "200-2507311238-3", "100-2507311238"},
        {"3", "5", "S 11", "Düsseldorf Flughafen Terminal", "12:35", "12:37", "Düsseldorf Hbf", {{1, "Verspätung wegen technischer Störung"}}},
        {"4", "",  "RE 6", "Minden (Westf)", "12:50", "12:55", "Düsseldorf Hbf, Duisburg Hbf, Oberhausen Hbf"},
        {"5", "",  "S 8", "Wuppertal-Oberbarmen", "12:40", "12:42", "Düsseldorf Hbf, Neuss Hbf, Krefeld Hbf"},
        {"1", "",  "RE 11", "Dortmund Hbf", "12:38", "12:40", "Düsseldorf Hbf, Duisburg Hbf"},
        {"2", "3", "S 1", "Solingen Hbf", "12:42", "12:45", "Düsseldorf Hbf, Neuss Hbf"},
        {"3", "",  "S 11", "Düsseldorf Flughafen Terminal", "12:35", "12:37", "Düsseldorf Hbf", {{1, "Verspätung wegen technischer Störung"}}},
        {"4", "",  "RE 6", "Minden (Westf)", "12:50", "12:55", "Düsseldorf Hbf, Duisburg Hbf, Oberhausen Hbf", {}, true},
        {"5", "",  "S 8", "Wuppertal-Oberbarmen", "12:40", "12:42", "Düsseldorf Hbf, Neuss Hbf, Krefeld Hbf"},
        // Wing pair further down -> grouped and bracketed inside the list.
        {"4", "",  "RB 89", "Paderborn Hbf", "13:02", "", "Soest, Lippstadt", {}, false, "", false, "300-2507311302-5", "400-2507311302"},
        {"4", "",  "RB 89", "Warburg (Westf)", "13:02", "", "Soest, Lippstadt", {{1, "Zug wird geflügelt"}}, false, "", false, "400-2507311302-5", "300-2507311302"},
        {"3", "5", "S 11", "Düsseldorf Flughafen Terminal", "12:35", "12:37", "Düsseldorf Hbf", {{1, "Verspätung wegen technischer Störung"}}},
        {"4", "",  "RE 6", "Minden (Westf)", "12:50", "12:55", "Düsseldorf Hbf, Duisburg Hbf, Oberhausen Hbf"},
        {"5", "",  "S 8", "Wuppertal-Oberbarmen", "12:40", "12:42", "Düsseldorf Hbf, Neuss Hbf, Krefeld Hbf"},
        {"2", "",  "RE 6", "Kassel-Wilhelmshöhe", "12:48", "13:20", "Warburg (Westf), Hofgeismar", {{1, "Umleitung"}}, false, "", true},
        {"",  "",  "Bus SEV 1", "Paderborn Hbf", "12:52", "", "Altenbeken, Bad Driburg", {{1, "Schienenersatzverkehr"}}, false, "Bus"},
        {"",  "",  "Bus 481", "Höxter Rathaus", "12:58", "13:05", "Steinheim Markt, Nieheim", {}, false, "Bus"},
    };
    station = "Steinheim (Westf.)";
    ticker  = "Ein Unwetter behindert den Bahnverkehr. Für weitere Informationen beachten Sie Durchsagen.";
    groupWings(list);
}

// -------------------------------------------------------------------------
// Load live data from PostgreSQL
// -------------------------------------------------------------------------
static bool loadLiveData(const DBConnectionConfig &dbCfg,
                         vector<Departure> &list,
                         string &station, string &ticker) {
    FetchedStationInfo stInfo;
    vector<FetchedDeparture> fetched;
    string tk;

    int n = FetchDepartures(dbCfg, stInfo, fetched, tk);
    if (n < 0) return false;

    station = stInfo.name;
    ticker  = tk;
    list.clear();
    for (auto &fd : fetched)
        list.push_back(fromFetched(fd));

    // Ensure we always have at least one entry so the renderer doesn't crash
    if (list.empty()) {
        Departure placeholder;
        placeholder.line = "---";
        placeholder.dest = "Keine Abfahrten";
        placeholder.pTime = "--:--";
        list.push_back(placeholder);
    }

    groupWings(list);   // make wing partners adjacent + tag their positions
    return true;
}

// -------------------------------------------------------------------------
// Drawing helpers (unchanged from original)
// -------------------------------------------------------------------------

// Return colour for a train line name:
//   S-Bahn → yellow, ICE → white, RE / everything else → red
Color lineColor(const string &line) {
    if (line.size() >= 1 && line[0] == 'S' && (line.size() == 1 || line[1] == ' '))
        return Color(255, 255, 0);   // yellow for S-Bahn
    if (line.substr(0, 3) == "ICE")
        return Color(255, 255, 255); // white for ICE
    return Color(255, 60, 60);       // red for RE and everything else
}

// Draw text with every space rendered at half its normal width, so train
// line names ("RE 11") and platform numbers stay compact. Returns the
// total width drawn, like DrawText.
int DrawTextHalfSpace(Canvas *canvas, const Font &font, int x, int y,
                      const Color &color, const string &text) {
    const int half_sp = (font.CharacterWidth(' ') + 1) / 2;
    int cursor = x;
    size_t pos = 0;
    while (pos < text.size()) {
        size_t sp = text.find(' ', pos);
        if (sp == string::npos) {
            cursor += DrawText(canvas, font, cursor, y, color, nullptr,
                               text.substr(pos).c_str());
            break;
        }
        if (sp > pos)
            cursor += DrawText(canvas, font, cursor, y, color, nullptr,
                               text.substr(pos, sp - pos).c_str());
        cursor += half_sp;
        pos = sp + 1;
    }
    return cursor - x;
}

int MeasureTextHalfSpace(const Font &font, const string &text) {
    static led_util::NullCanvas nc;
    return DrawTextHalfSpace(&nc, font, 0, 0, Color(0, 0, 0), text);
}

// The platform actually shown: the changed one when it differs, else planned.
static const string &platformShown(const Departure &dep) {
    return (!dep.cPlatform.empty() && dep.cPlatform != dep.platform)
           ? dep.cPlatform : dep.platform;
}

// Buses depart from the forecourt rather than a track, so where a train shows
// its platform number a bus shows BUS instead.
static const string kBusLabel = "BUS";
static const string &platformLabel(const Departure &dep) {
    const string &plat = platformShown(dep);
    if (plat.empty() && isBus(dep)) return kBusLabel;
    return plat;
}

// -------------------------------------------------------------------------
// Row styling
// -------------------------------------------------------------------------
// The hero block and the list rows are each drawn by one function that takes
// its colours from a RowStyle, rather than testing for cancelled/bus at every
// drawing site. A new kind of row means a new case in styleFor(), not another
// branch through the renderer -- and what is tinted vs. inverted stays
// consistent because every element reads the same struct.
static const Color kBg(0, 0, 120);   // display background (dark blue)

struct RowStyle {
    Color   fg    {255, 255, 255};   // platform, line, destination, time
    Color   via   {180, 180, 180};   // "via ..." line of the hero block
    Color   note  {255, 180, 0};     // delay-reason line
    Color   delay {255, 60, 60};     // changed (delayed) time
    bool    fillBg        = false;   // paint the whole block first
    uint8_t bg_r = 0, bg_g = 0, bg_b = 0;
    bool    lineTypeColor = true;    // colour the line name by train type
    bool    strikeTime    = false;   // strike the planned time through
};

// A rerouted train shows its (new) destination in red, the same red used for
// delayed times. Buses and cancelled rows keep their own colours -- a
// destination change on those is either moot (cancelled) or not meaningful
// (replacement bus).
static const Color CHANGE_RED(255, 60, 60);
static Color destColor(const Departure &dep, const RowStyle &st) {
    return (dep.destChanged && st.lineTypeColor) ? CHANGE_RED : st.fg;
}

static RowStyle styleFor(const Departure &dep) {
    RowStyle s;
    if (dep.cancelled) {
        // Inverted: background-coloured text on a white block.
        s.fg = s.via = s.note = s.delay = kBg;
        s.fillBg = true; s.bg_r = s.bg_g = s.bg_b = 255;
        s.lineTypeColor = false;
        s.strikeTime    = true;
    } else if (isBus(dep)) {
        // Replacement buses: yellow on purple, subtext in a darker yellow.
        s.fg    = Color(255, 255, 0);
        s.via   = s.note = Color(190, 150, 0);
        s.delay = Color(255, 120, 90);   // plain red would sink into the purple
        s.fillBg = true; s.bg_r = 90; s.bg_g = 0; s.bg_b = 130;
        s.lineTypeColor = false;
    }
    return s;
}

void drawRowBg(Canvas *canvas, int x, int y, int w, int h,
               uint8_t r, uint8_t g, uint8_t b) {
    for (int py = y; py < y + h; ++py)
        for (int px = x; px < x + w; ++px)
            canvas->SetPixel(px, py, r, g, b);
}

void drawPlatform(Canvas *canvas, const Font &font, int x, int baseline,
                  const Departure &dep, int plat_w, const RowStyle &st) {
    const string &plat = platformLabel(dep);
    bool changed = (!dep.cPlatform.empty() && dep.cPlatform != dep.platform);
    // Changed platforms blink like on the real DB displays: every 500 ms the
    // platform swaps to a filled box, in whatever colours this row uses.
    bool inverted = changed &&
        (chrono::duration_cast<chrono::milliseconds>(
             chrono::steady_clock::now().time_since_epoch()).count() / 500) % 2 == 0;
    if (inverted) {
        // The box grows with the text so wide platform numbers stay inside it.
        int box_w = max(plat_w, MeasureTextHalfSpace(font, plat));
        int top   = baseline - font.baseline();
        drawRowBg(canvas, x - 1, top, box_w + 2, font.height(),
                  st.fg.r, st.fg.g, st.fg.b);
        Color txt = st.fillBg ? Color(st.bg_r, st.bg_g, st.bg_b) : kBg;
        DrawTextHalfSpace(canvas, font, x, baseline, txt, plat);
    } else {
        DrawTextHalfSpace(canvas, font, x, baseline, st.fg, plat);
    }
}

// -------------------------------------------------------------------------
// Column layout
// -------------------------------------------------------------------------
// The hero block and the list are measured independently: they use different
// fonts and different departures, so sharing one set of widths only made the
// small rows inherit the big font's spacing.
//
// Hero columns are in screen coordinates. List columns are relative to the
// list ViewPort (which itself starts at LIST_CONTENT_X), so list drawing uses
// them as-is.
// A "[" grouping bracket for wing trains: a 1px vertical bar with 3px ticks.
static const int BRACKET_W = 3;

struct Columns {
    int plat_x = 0;
    int bracket_x = -1;   // x of the wing bracket, or -1 when the region has none
    int line_x = 0;
    int dest_x = 0;
    int time_x = 0;   // left edge of the time column
    int dest_w = 0;   // room for destination text between dest_x and time_x
    int note_w = 0;   // reason line: indented past the platform only
    int plat_w = 0;   // column width, for the changed-platform highlight box
};

// `deps` are the departures drawn in `font`; `x0` is the left margin and
// `width` the space available in whatever coordinate system they live in.
// When `wings` is set, a gutter is reserved after the platform for the
// grouping bracket and the line column is indented to clear it -- applied to
// every row in the region so wing and non-wing rows stay aligned.
static Columns computeColumns(const Font &font,
                              const vector<const Departure*> &deps,
                              int x0, int width, bool wings) {
    Columns c;
    c.plat_x = x0;

    // Two digits is the floor, so single-digit platforms don't make the board
    // jump inward when the data changes.
    c.plat_w   = led_util::MeasureText(font, "55");
    int line_w = 0, reserve = 0;
    for (const Departure *d : deps) {
        c.plat_w = max(c.plat_w, MeasureTextHalfSpace(font, platformLabel(*d)));
        line_w   = max(line_w,   MeasureTextHalfSpace(font, d->line));
        int w = led_util::MeasureText(font, d->pTime);
        if (!d->cancelled && !d->cTime.empty())
            w += 2 + led_util::MeasureText(font, d->cTime);
        reserve = max(reserve, w);
    }

    int after_plat = c.plat_x + c.plat_w;
    if (wings) {
        c.bracket_x = after_plat + 2;
        c.line_x    = c.bracket_x + BRACKET_W + 2;
    } else {
        c.line_x    = after_plat + 3;
    }
    c.dest_x = c.line_x + line_w + 4;
    c.time_x = width - reserve;
    c.dest_w = max(1, c.time_x - c.dest_x - 2);
    c.note_w = max(1, c.time_x - c.line_x - 2);
    return c;
}

static Columns heroColumns(const Font &bigFont, const vector<Departure> &list,
                           int heroCount, int width) {
    vector<const Departure*> deps;
    for (int i = 0; i < heroCount; ++i) deps.push_back(&list[i]);
    return computeColumns(bigFont, deps, 5, width, heroCount > 1);
}

static Columns listColumns(const Font &smallFont, const vector<Departure> &list,
                           int heroCount, int width) {
    vector<const Departure*> deps;
    bool wings = false;
    for (size_t i = heroCount; i < list.size(); ++i) {
        deps.push_back(&list[i]);
        if (list[i].wingCount > 1) wings = true;
    }
    // x0 = 1 leaves room for the changed-platform box, which starts at x-1.
    return computeColumns(smallFont, deps, 1, width - LIST_CONTENT_X, wings);
}

void drawStrikethrough(Canvas *canvas, int x, int baseline, int text_w,
                       const Font &font) {
    int mid_y = baseline - font.baseline() / 2;
    for (int px = x; px < x + text_w; ++px)
        canvas->SetPixel(px, mid_y, 255, 0, 0);
}

// A "[" spanning [y_top, y_bottom): vertical bar at x with short top/bottom
// ticks, marking a wing group's rows as one coupled train.
static void drawWingBracket(Canvas *canvas, int x, int y_top, int y_bottom,
                            const Color &col) {
    for (int y = y_top; y < y_bottom; ++y)
        canvas->SetPixel(x, y, col.r, col.g, col.b);
    for (int dx = 0; dx < BRACKET_W; ++dx) {
        canvas->SetPixel(x + dx, y_top, col.r, col.g, col.b);
        canvas->SetPixel(x + dx, y_bottom - 1, col.r, col.g, col.b);
    }
}

// -------------------------------------------------------------------------
// DepartureScroller — page scrolling that always lands on a departure
// -------------------------------------------------------------------------
// PageScroller advances by a fixed number of fixed-height items. Rows here
// are not fixed height (a departure carrying a delay reason is taller), so
// that arithmetic drifts and can leave a stray reason line alone at the top
// of the viewport. This scroller only ever comes to rest on the start offset
// of a departure.
class DepartureScroller {
public:
    DepartureScroller(ViewPort *vp, vector<int> starts, vector<int> heights,
                      int content_h, float wait = 3.0f, float speed = 40.0f)
        : vp_(vp), starts_(std::move(starts)), heights_(std::move(heights)),
          content_h_(content_h), wait_(wait), speed_(speed),
          last_(chrono::steady_clock::now()) {}

    void Update() {
        const int visible = vp_->visibleHeight();
        if (content_h_ <= visible) {          // everything fits: never scroll
            cur_ = target_ = 0.0f;
            Apply();
            return;
        }

        const auto now = chrono::steady_clock::now();
        const float dt = chrono::duration<float>(now - last_).count();

        if (scrolling_) {
            cur_ += speed_ * dt;
            last_ = now;
            if (cur_ >= target_) { cur_ = target_; scrolling_ = false; }
        } else if (dt >= wait_) {
            const int next = NextStart(visible);
            last_ = now;
            if (next < 0) {                   // nothing left below: back to top
                cur_ = target_ = 0.0f;
            } else {
                target_    = static_cast<float>(next);
                scrolling_ = true;
            }
        }
        Apply();
    }

    float scrollFraction() const {
        const int max_off = max(0, content_h_ - vp_->visibleHeight());
        return max_off <= 0 ? 0.0f : min(1.0f, cur_ / static_cast<float>(max_off));
    }
    float viewFraction() const {
        return content_h_ <= 0 ? 1.0f
             : min(1.0f, vp_->visibleHeight() / static_cast<float>(content_h_));
    }

private:
    // Start of the first departure that isn't fully visible from where the
    // list is (or is heading). Returns -1 when the rest already fits.
    int NextStart(int visible) const {
        const int top    = static_cast<int>(target_ + 0.5f);
        const int bottom = top + visible;
        int next = -1;
        for (size_t i = 0; i < starts_.size(); ++i) {
            if (starts_[i] + heights_[i] > bottom && starts_[i] > top) {
                next = starts_[i];
                break;
            }
        }
        if (next < 0) return -1;

        // Last page: scrolling all the way to `next` would run off the end of
        // the content and leave a gap under the final departure. Back up to
        // the earliest departure that still shows the tail in full -- it
        // repeats a row or two, which reads better than empty space.
        if (next + visible > content_h_) {
            for (size_t i = 0; i < starts_.size(); ++i)
                if (starts_[i] + visible >= content_h_ && starts_[i] > top)
                    return starts_[i];
        }
        return next;
    }
    void Apply() { vp_->SetScrollY(static_cast<int>(cur_)); }

    ViewPort *vp_;
    vector<int> starts_, heights_;
    int   content_h_;
    float wait_, speed_;
    float cur_ = 0.0f, target_ = 0.0f;
    bool  scrolling_ = false;
    chrono::steady_clock::time_point last_;
};

// -------------------------------------------------------------------------
// Scroller state — rebuilt every time data changes
// -------------------------------------------------------------------------
struct ScrollerState {
    vector<ScrollingTextBox*> heroDests;   // one per wing part
    ScrollingTextBox *heroVia        = nullptr;
    ScrollingTextBox *heroNote       = nullptr;
    ScrollingTextBox *tickerScroller = nullptr;
    vector<ScrollingTextBox*> destScrollers;
    vector<ScrollingTextBox*> noteScrollers;
    ViewPort          *listVP     = nullptr;
    DepartureScroller *pageScroll = nullptr;
    led_util::ScrollBar *scrollBar = nullptr;

    // Hero block geometry (screen coordinates)
    RowStyle hero_style;
    int hero_count = 1;   // wing parts drawn big up top
    int hero_top = 0, hero_h = 0;
    int hero_baseline = 0, hero_via_baseline = 0, hero_note_baseline = 0;

    // List rows: start offset and full height (including any reason line)
    vector<int>      row_y_offsets;
    vector<int>      row_heights;
    vector<RowStyle> row_styles;

    int content_h = 0;
    int row_h     = 0;
    int list_y    = 0;
    int list_h    = 0;
    int ticker_h  = 0;

    void destroy() {
        for (auto *s : heroDests) delete s;
        heroDests.clear();
        delete heroVia;        heroVia        = nullptr;
        delete heroNote;       heroNote       = nullptr;
        delete tickerScroller; tickerScroller = nullptr;
        for (auto *s : destScrollers) delete s;
        destScrollers.clear();
        for (auto *s : noteScrollers) delete s;
        noteScrollers.clear();
        delete pageScroll; pageScroll = nullptr;
        delete listVP;     listVP     = nullptr;
        delete scrollBar;  scrollBar  = nullptr;
        row_y_offsets.clear();
        row_heights.clear();
        row_styles.clear();
    }

    ~ScrollerState() { destroy(); }
};

// Join a departure's notes into one line.
static string joinNotes(const Departure &dep) {
    string s;
    for (size_t n = 0; n < dep.notes.size(); ++n) {
        if (n > 0) s += " | ";
        s += dep.notes[n].text;
    }
    return s;
}

// --- Vertical layout constants ---
static const int HERO_BASELINE = 27;  // 3 px higher than it used to sit

// Pull a list row's reason line up toward the departure it belongs to. 2 px
// is all the slack there is: the 5x8 glyphs reach 7 px above the baseline
// (ÄÖÜ) and 1 px below it (,.Qgjpqy), so 8 px between baselines is the
// closest two of these lines can sit without their ink touching. The hero's
// via/reason lines are already at that minimum and are left alone.
static const int NOTE_TIGHTEN  = 2;

// Build all scrollers for the current departure list.
static void buildScrollers(ScrollerState &ss,
                           Canvas *canvas,
                           const Font &bigFont, const Font &smallFont,
                           const vector<Departure> &list,
                           const string &ticker,
                           int width, int height, int heroCount,
                           const Columns &heroCols, const Columns &listCols) {
    ss.destroy();

    // --- Hero block: 1..heroCount wing parts as big rows, then via + notes ---
    ss.hero_style    = styleFor(list[0]);
    ss.hero_count    = heroCount;
    ss.hero_baseline = HERO_BASELINE;
    const int last_part_base = HERO_BASELINE + (heroCount - 1) * bigFont.height();
    ss.hero_via_baseline  = last_part_base + smallFont.height();
    ss.hero_note_baseline = ss.hero_via_baseline + smallFont.height();

    // Reason lines from every wing part, combined into one.
    string heroNotes;
    for (int i = 0; i < heroCount; ++i) {
        string n = joinNotes(list[i]);
        if (!n.empty()) { if (!heroNotes.empty()) heroNotes += " | "; heroNotes += n; }
    }
    const bool hero_has_note  = !heroNotes.empty();
    const int  hero_last_base = hero_has_note ? ss.hero_note_baseline
                                              : ss.hero_via_baseline;
    ss.hero_top = ss.hero_baseline - bigFont.baseline();
    ss.hero_h   = (hero_last_base - smallFont.baseline() + smallFont.height())
                  - ss.hero_top;

    ss.row_h    = smallFont.height() + 2;
    ss.ticker_h = ticker.empty() ? 0 : (smallFont.height() + 2);
    ss.list_y   = ss.hero_top + ss.hero_h + 3;
    ss.list_h   = height - ss.list_y - ss.ticker_h;

    for (int i = 0; i < heroCount; ++i) {
        int base = HERO_BASELINE + i * bigFont.height();
        ss.heroDests.push_back(new ScrollingTextBox(
            canvas, heroCols.dest_x, base - bigFont.baseline(),
            heroCols.dest_w, bigFont.height(),
            bigFont, destColor(list[i], ss.hero_style), list[i].dest,
            20.0f, 2.0f, 12));
    }

    // via/notes describe the (shared) trunk, taken from the first part.
    ss.heroVia = new ScrollingTextBox(
        canvas, heroCols.dest_x, ss.hero_via_baseline - smallFont.baseline(),
        heroCols.dest_w, smallFont.height(),
        smallFont, ss.hero_style.via, "via " + list[0].stops, 20.0f, 2.0f, 12);

    if (hero_has_note) {
        ss.heroNote = new ScrollingTextBox(
            canvas, heroCols.dest_x, ss.hero_note_baseline - smallFont.baseline(),
            heroCols.dest_w, smallFont.height(),
            smallFont, ss.hero_style.note, heroNotes, 20.0f, 2.0f, 12);
    }

    // --- List rows: measure each departure's full height up front ---
    const int note_h = ss.row_h - NOTE_TIGHTEN;
    int total = 0;
    for (size_t i = heroCount; i < list.size(); ++i) {
        const int h = ss.row_h + (list[i].notes.empty() ? 0 : note_h);
        ss.row_y_offsets.push_back(total);
        ss.row_heights.push_back(h);
        ss.row_styles.push_back(styleFor(list[i]));
        total += h;
    }
    ss.content_h = total;

    ss.listVP = new ViewPort(canvas, LIST_CONTENT_X, ss.list_y,
                             width - LIST_CONTENT_X, ss.list_h);
    ss.pageScroll = new DepartureScroller(ss.listVP, ss.row_y_offsets,
                                          ss.row_heights, ss.content_h,
                                          3.0f, 40.0f);
    ss.scrollBar  = new led_util::ScrollBar(canvas, SCROLLBAR_X, ss.list_y,
                                            ss.list_h, SCROLLBAR_W);

    // Ticker (only when there are station messages)
    if (!ticker.empty()) {
        ss.tickerScroller = new ScrollingTextBox(
            canvas, 0, height - ss.ticker_h,
            width, ss.ticker_h,
            smallFont, Color(200, 0, 0), ticker,
            35.0f, 2.0f, 40);
    }

    for (size_t i = heroCount; i < list.size(); ++i) {
        const size_t idx = i - heroCount;
        const int y_pos  = ss.row_y_offsets[idx];
        const RowStyle &st = ss.row_styles[idx];

        ss.destScrollers.push_back(new ScrollingTextBox(
            ss.listVP, listCols.dest_x, y_pos + 1,
            listCols.dest_w, ss.row_h,
            smallFont, destColor(list[i], st), list[i].dest, 20.0f, 1.5f, 12));

        if (!list[i].notes.empty()) {
            // The reason line only has to clear the platform -- it may run
            // under the line number, which buys it a lot of reading width.
            ss.noteScrollers.push_back(new ScrollingTextBox(
                ss.listVP, listCols.line_x, y_pos + ss.row_h - NOTE_TIGHTEN + 1,
                listCols.note_w, note_h,
                smallFont, st.note, joinNotes(list[i]), 20.0f, 2.0f, 12));
        } else {
            ss.noteScrollers.push_back(nullptr);
        }
    }
}

// -------------------------------------------------------------------------
// Block renderers — one for the hero, one for a list row. Both take their
// colours from the row's RowStyle, so neither has to know about buses or
// cancellations.
// -------------------------------------------------------------------------
static void drawHero(Canvas *c, const Font &bigFont,
                     const vector<Departure> &list, const ScrollerState &ss,
                     const Columns &cols, int width) {
    const RowStyle &st = ss.hero_style;
    if (st.fillBg)
        drawRowBg(c, 0, ss.hero_top, width, ss.hero_h, st.bg_r, st.bg_g, st.bg_b);

    for (int i = 0; i < ss.hero_count; ++i) {
        const Departure &dep = list[i];
        int base = ss.hero_baseline + i * bigFont.height();
        // Platform (or BUS) once, on the first part of a coupled group.
        if (i == 0)
            drawPlatform(c, bigFont, cols.plat_x, base, dep, cols.plat_w, st);
        DrawTextHalfSpace(c, bigFont, cols.line_x, base,
                          st.lineTypeColor ? lineColor(dep.line) : st.fg, dep.line);
        ss.heroDests[i]->SetCanvas(c);
        ss.heroDests[i]->Update();
        int pt_w = DrawText(c, bigFont, cols.time_x, base,
                            st.fg, nullptr, dep.pTime.c_str());
        if (st.strikeTime)
            drawStrikethrough(c, cols.time_x, base, pt_w, bigFont);
        else if (!dep.cTime.empty())
            DrawText(c, bigFont, cols.time_x + pt_w + 2, base,
                     st.delay, nullptr, dep.cTime.c_str());
    }

    // One bracket around the coupled parts.
    if (ss.hero_count > 1 && cols.bracket_x >= 0)
        drawWingBracket(c, cols.bracket_x, ss.hero_top,
                        ss.hero_top + ss.hero_count * bigFont.height(), st.fg);

    ss.heroVia->SetCanvas(c);
    ss.heroVia->Update();
    if (ss.heroNote) {
        ss.heroNote->SetCanvas(c);
        ss.heroNote->Update();
    }
}

static void drawListRow(Canvas *vp, const Font &smallFont,
                        const Departure &dep, const ScrollerState &ss,
                        const Columns &cols, int idx, int width) {
    const RowStyle &st = ss.row_styles[idx];
    const int y_pos    = ss.row_y_offsets[idx];
    const int baseline = y_pos + smallFont.baseline() + 1;

    if (st.fillBg)
        drawRowBg(vp, 0, y_pos, width, ss.row_heights[idx],
                  st.bg_r, st.bg_g, st.bg_b);

    // Platform (or BUS) once per coupled group, on its first part.
    if (dep.wingPos == 0)
        drawPlatform(vp, smallFont, cols.plat_x, baseline, dep, cols.plat_w, st);
    // Bracket spanning this group's rows, drawn from its first part.
    if (dep.wingPos == 0 && dep.wingCount > 1 && cols.bracket_x >= 0) {
        int last  = idx + dep.wingCount - 1;
        drawWingBracket(vp, cols.bracket_x, ss.row_y_offsets[idx],
                        ss.row_y_offsets[last] + ss.row_h, st.fg);
    }
    DrawTextHalfSpace(vp, smallFont, cols.line_x, baseline,
                      st.lineTypeColor ? lineColor(dep.line) : st.fg, dep.line);

    ss.destScrollers[idx]->SetCanvas(vp);
    ss.destScrollers[idx]->Update();

    int pt_w = DrawText(vp, smallFont, cols.time_x, baseline,
                        st.fg, nullptr, dep.pTime.c_str());
    if (st.strikeTime)
        drawStrikethrough(vp, cols.time_x, baseline, pt_w, smallFont);
    else if (!dep.cTime.empty())
        DrawText(vp, smallFont, cols.time_x + pt_w + 2, baseline,
                 st.delay, nullptr, dep.cTime.c_str());

    if (ss.noteScrollers[idx]) {
        ss.noteScrollers[idx]->SetCanvas(vp);
        ss.noteScrollers[idx]->Update();
    }
}

// =========================================================================
// main
// =========================================================================
int main(int argc, char **argv) {
    signal(SIGINT,  InterruptHandler);
    signal(SIGTERM, InterruptHandler);

    const string exeDir     = executableDir(argv[0]);
    const string configPath = resolvePath(exeDir, "config.json");

    // --- Parse CLI flags ---
    bool debugMode = false;
    int evaOverride = 0;  // 0 means "not given on the command line"
    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--debug") == 0) {
            debugMode = true;
        } else if (strcmp(argv[i], "--eva") == 0 && i + 1 < argc) {
            evaOverride = std::atoi(argv[++i]);
        }
    }

    // --- Matrix setup ---
    RGBMatrix::Options matrix_options;
    rgb_matrix::RuntimeOptions runtime_options;

    matrix_options.rows = 32;
    matrix_options.cols = 64;
    matrix_options.chain_length = 1;
    matrix_options.parallel = 1;
    if (!LoadConfigWithCliOverrides(&argc, &argv, configPath, matrix_options, runtime_options)) {
        rgb_matrix::PrintMatrixFlags(stderr);
        return 1;
    }

    RGBMatrix *matrix = RGBMatrix::CreateFromOptions(matrix_options, runtime_options);
    if (!matrix) return 1;

    // Single offscreen canvas, swapped on vsync from the render loop itself.
    // The panel's refresh thread is very timing sensitive: it must not have
    // to compete with a render thread that is permanently runnable, or
    // individual scan rows get uneven on-time and show up as ghosting.
    // Blocking in SwapOnVSync plus the FRAME_MS delay keeps this thread
    // asleep for most of every frame — same pattern as the other programs
    // driving this panel.
    FrameCanvas *off = matrix->CreateFrameCanvas();

    const string fontDir = "third_party/rpi-rgb-led-matrix-extensions/third_party/rpi-rgb-led-matrix/fonts";
    const string bigFontPath   = resolvePath(exeDir, fontDir + "/clR6x12.bdf");
    const string smallFontPath = resolvePath(exeDir, fontDir + "/5x8.bdf");
    Font bigFont, smallFont;
    if (!bigFont.LoadFont(bigFontPath.c_str()) ||
        !smallFont.LoadFont(smallFontPath.c_str())) {
        cerr << "Couldn't load fonts: " << bigFontPath
             << " / " << smallFontPath << endl;
        delete matrix;
        return 1;
    }

    int width  = off->width();
    int height = off->height();

    // --- DB connection config (only needed in live mode) ---
    DBConnectionConfig dbCfg;
    if (!debugMode) {
        if (!LoadDBConnectionConfig(configPath, dbCfg)) {
            cerr << "Warning: No 'database' section in config.json, using defaults" << endl;
        }
        if (evaOverride != 0) {
            dbCfg.eva = evaOverride;
        }
        cout << "Live mode: connecting to " << dbCfg.host << ":" << dbCfg.port
             << "/" << dbCfg.dbname << " (EVA " << dbCfg.eva << ")" << endl;
    } else {
        cout << "Debug mode: using dummy data" << endl;
    }

    // --- Load initial data ---
    vector<Departure> list;
    string station, ticker;

    if (debugMode) {
        loadDummyData(list, station, ticker);
    } else {
        if (!loadLiveData(dbCfg, list, station, ticker)) {
            cerr << "Initial DB fetch failed, falling back to dummy data" << endl;
            loadDummyData(list, station, ticker);
        }
    }

    // --- Column layout, measured independently for the hero and the list ---
    int heroCount    = heroGroupSize(list);
    Columns heroCols = heroColumns(bigFont, list, heroCount, width);
    Columns listCols = listColumns(smallFont, list, heroCount, width);

    // Layout constants
    const int header_bottom = 14;

    // Build initial scroller state
    ScrollerState ss;
    buildScrollers(ss, off, bigFont, smallFont, list, ticker,
                   width, height, heroCount, heroCols, listCols);

    // --- Background fetch thread (live mode only) ---
    struct SharedFetchData {
        mutex mtx;
        vector<Departure> departures;
        string station;
        string ticker;
        bool hasNewData = false;
    };
    SharedFetchData shared;
    atomic<bool> fetchRunning{true};
    thread fetchThread;

    if (!debugMode) {
        fetchThread = thread([&dbCfg, &shared, &fetchRunning]() {
            while (fetchRunning.load()) {
                vector<Departure> newList;
                string newStation, newTicker;
                if (loadLiveData(dbCfg, newList, newStation, newTicker)) {
                    lock_guard<mutex> lock(shared.mtx);
                    shared.departures = std::move(newList);
                    shared.station    = std::move(newStation);
                    shared.ticker     = std::move(newTicker);
                    shared.hasNewData = true;
                } else {
                    cerr << "DB refresh failed, keeping previous data" << endl;
                }
                // Sleep in small intervals so we can stop promptly
                for (int i = 0; i < DB_REFRESH_SECONDS * 10 && fetchRunning.load(); ++i)
                    this_thread::sleep_for(chrono::milliseconds(100));
            }
        });
    }

    // --- Main render loop ---
    while (!interrupt_received) {
        // --- Check for new data from background thread ---
        if (!debugMode) {
            vector<Departure> newList;
            string newStation, newTicker;
            bool gotNew = false;
            {
                lock_guard<mutex> lock(shared.mtx);
                if (shared.hasNewData) {
                    newList    = std::move(shared.departures);
                    newStation = std::move(shared.station);
                    newTicker  = std::move(shared.ticker);
                    shared.hasNewData = false;
                    gotNew = true;
                }
            }

            // Rebuild the scrollers (which resets all scroll positions)
            // only when the refresh actually changed something.
            if (gotNew && (newList != list || newStation != station ||
                           newTicker != ticker)) {
                list    = std::move(newList);
                station = std::move(newStation);
                ticker  = std::move(newTicker);

                // Re-measure the columns in case platform or line names changed
                heroCount = heroGroupSize(list);
                heroCols  = heroColumns(bigFont, list, heroCount, width);
                listCols  = listColumns(smallFont, list, heroCount, width);

                buildScrollers(ss, off, bigFont, smallFont, list, ticker,
                               width, height, heroCount, heroCols, listCols);
            }
        }

        Canvas *c = off;

        // Dark blue background
        c->Fill(0, 0, 120);

        // Station name header
        DrawText(c, bigFont, heroCols.plat_x, bigFont.baseline() + 1,
                 Color(255, 255, 255), nullptr, station.c_str());

        // Clock in the top-right corner. The patch behind it is cleared
        // first so a long station name can never bleed into the time.
        {
            time_t now_t = time(nullptr);
            struct tm tmv;
            localtime_r(&now_t, &tmv);
            char clk[16];
            snprintf(clk, sizeof(clk), "%02d:%02d:%02d",
                     tmv.tm_hour, tmv.tm_min, tmv.tm_sec);
            int clk_w = led_util::MeasureText(bigFont, clk);
            int clk_x = width - clk_w - 1;
            for (int py = 0; py < header_bottom; ++py)
                for (int px = clk_x - 2; px < width; ++px)
                    c->SetPixel(px, py, 0, 0, 120);
            DrawText(c, bigFont, clk_x, bigFont.baseline() + 1,
                     Color(255, 255, 255), nullptr, clk);
        }

        // Separator line
        for (int x = 0; x < width; ++x)
            c->SetPixel(x, header_bottom, 255, 255, 255);

        // --- Hero block (next departure, or both parts of a wing, big font) ---
        drawHero(c, bigFont, list, ss, heroCols, width);

        // --- Following departures inside page-scrolled list ---
        ss.listVP->SetParent(c);
        ss.pageScroll->Update();

        ss.scrollBar->SetCanvas(c);
        ss.scrollBar->Draw(ss.pageScroll->scrollFraction(), ss.pageScroll->viewFraction(),
                           60, 60, 60,
                           255, 255, 255);

        for (int i = heroCount; i < (int)list.size(); ++i)
            drawListRow(ss.listVP, smallFont, list[i], ss, listCols, i - heroCount,
                        width - LIST_CONTENT_X);

        // --- Ticker strip at bottom (only when active) ---
        if (ss.tickerScroller) {
            for (int y = height - ss.ticker_h; y < height; ++y)
                for (int x = 0; x < width; ++x)
                    c->SetPixel(x, y, 255, 255, 255);
            ss.tickerScroller->SetCanvas(c);
            ss.tickerScroller->Update();
        }

        // Show the frame, then idle until the next one. Nothing on screen
        // moves faster than ~20 px/s, so FRAME_MS is imperceptible here and
        // leaves the CPU free for the panel's refresh thread.
        off = matrix->SwapOnVSync(off);
        this_thread::sleep_for(chrono::milliseconds(FRAME_MS));
    }

    cout << "Received signal, shutting down..." << endl;

    // Stop background fetch thread
    fetchRunning.store(false);
    if (fetchThread.joinable())
        fetchThread.join();

    ss.destroy();
    matrix->Clear();
    delete matrix;
    return 0;
}
