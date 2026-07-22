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
    string category;   // "RE", "S", "ICE", "Bus", ... (empty if unknown)
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
           a.category == b.category && a.notes == b.notes;
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
    d.cancelled = fd.cancelled;
    d.category  = fd.category;
    for (auto &fn : fd.notes)
        d.notes.push_back({fn.id, fn.text});
    return d;
}

// -------------------------------------------------------------------------
// Dummy data for --debug mode
// -------------------------------------------------------------------------
static void loadDummyData(vector<Departure> &list, string &station, string &ticker) {
    list = {
        {"1", "",  "RE 11", "Dortmund Hbf", "12:38", "12:40", "Düsseldorf Hbf, Duisburg Hbf",{{1, "technische Störung am Zug"}}, false},
        {"2", "",  "S 1", "Solingen Hbf", "12:42", "12:45", "Düsseldorf Hbf, Neuss Hbf"},
        {"3", "5", "S 11", "Düsseldorf Flughafen Terminal", "12:35", "12:37", "Düsseldorf Hbf", {{1, "Verspätung wegen technischer Störung"}}},
        {"4", "",  "RE 6", "Minden (Westf)", "12:50", "12:55", "Düsseldorf Hbf, Duisburg Hbf, Oberhausen Hbf"},
        {"5", "",  "S 8", "Wuppertal-Oberbarmen", "12:40", "12:42", "Düsseldorf Hbf, Neuss Hbf, Krefeld Hbf"},
        {"1", "",  "RE 11", "Dortmund Hbf", "12:38", "12:40", "Düsseldorf Hbf, Duisburg Hbf"},
        {"2", "3", "S 1", "Solingen Hbf", "12:42", "12:45", "Düsseldorf Hbf, Neuss Hbf"},
        {"3", "",  "S 11", "Düsseldorf Flughafen Terminal", "12:35", "12:37", "Düsseldorf Hbf", {{1, "Verspätung wegen technischer Störung"}}},
        {"4", "",  "RE 6", "Minden (Westf)", "12:50", "12:55", "Düsseldorf Hbf, Duisburg Hbf, Oberhausen Hbf", {}, true},
        {"5", "",  "S 8", "Wuppertal-Oberbarmen", "12:40", "12:42", "Düsseldorf Hbf, Neuss Hbf, Krefeld Hbf"},
        {"1", "",  "RE 11", "Dortmund Hbf", "12:38", "12:40", "Düsseldorf Hbf, Duisburg Hbf"},
        {"2", "",  "S 1", "Solingen Hbf", "12:42", "12:45", "Düsseldorf Hbf, Neuss Hbf"},
        {"3", "5", "S 11", "Düsseldorf Flughafen Terminal", "12:35", "12:37", "Düsseldorf Hbf", {{1, "Verspätung wegen technischer Störung"}}},
        {"4", "",  "RE 6", "Minden (Westf)", "12:50", "12:55", "Düsseldorf Hbf, Duisburg Hbf, Oberhausen Hbf"},
        {"5", "",  "S 8", "Wuppertal-Oberbarmen", "12:40", "12:42", "Düsseldorf Hbf, Neuss Hbf, Krefeld Hbf"},
        {"",  "",  "Bus SEV 1", "Paderborn Hbf", "12:52", "", "Altenbeken, Bad Driburg", {{1, "Schienenersatzverkehr"}}, false, "Bus"},
        {"",  "",  "Bus 481", "Höxter Rathaus", "12:58", "13:05", "Steinheim Markt, Nieheim", {}, false, "Bus"},
    };
    station = "Steinheim (Westf.)";
    ticker  = "Ein Unwetter behindert den Bahnverkehr. Für weitere Informationen beachten Sie Durchsagen.";
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

// Bus rows are drawn yellow on purple to set them apart from rail services.
static const Color BUS_FG(255, 255, 0);
static const uint8_t BUS_BG_R = 90, BUS_BG_G = 0, BUS_BG_B = 130;

// A bus row is styled as one whenever the departure is a bus; the BUS label
// itself only replaces the platform when there is no platform to show.
static bool busStyled(const Departure &dep) {
    return isBus(dep) && !dep.cancelled;
}

void drawPlatform(Canvas *canvas, const Font &font, int x, int baseline,
                  const Departure &dep, int plat_w, const Color &textCol) {
    const string &plat = platformLabel(dep);
    bool changed = (!dep.cPlatform.empty() && dep.cPlatform != dep.platform);
    // Changed platforms blink like on the real DB displays: the inverted
    // white box alternates with the normal rendering every 500 ms.
    bool inverted = changed &&
        (chrono::duration_cast<chrono::milliseconds>(
             chrono::steady_clock::now().time_since_epoch()).count() / 500) % 2 == 0;
    if (inverted) {
        // The highlight box grows with the text so wide platform numbers
        // are never drawn outside of it.
        int box_w = max(plat_w, MeasureTextHalfSpace(font, plat));
        int top = baseline - font.baseline();
        for (int py = top; py < top + font.height(); ++py)
            for (int px = x - 1; px < x + box_w + 1; ++px)
                canvas->SetPixel(px, py, 255, 255, 255);
        DrawTextHalfSpace(canvas, font, x, baseline, Color(0, 0, 120), plat);
    } else {
        DrawTextHalfSpace(canvas, font, x, baseline, textCol, plat);
    }
}

// Width to reserve on the right for the time column: the widest planned +
// delayed time over all departures, each measured in the font its row uses.
static int computeTimeReserve(const Font &bigFont, const Font &smallFont,
                              const vector<Departure> &list) {
    int reserve = 0;
    for (size_t i = 0; i < list.size(); ++i) {
        const Font &f = (i == 0) ? bigFont : smallFont;
        int w = led_util::MeasureText(f, list[i].pTime);
        if (!list[i].cancelled && !list[i].cTime.empty())
            w += 2 + led_util::MeasureText(f, list[i].cTime);
        reserve = max(reserve, w);
    }
    return reserve;
}

// -------------------------------------------------------------------------
// Column layout
// -------------------------------------------------------------------------
// All four columns are in *screen* coordinates and shared by every row, so
// the big first departure and the small list rows line up vertically.
//
// The list rows are drawn inside a ViewPort that already starts at
// LIST_CONTENT_X, so drawing them means subtracting LIST_CONTENT_X from
// these values -- that offset is what used to make the first departure's
// platform sit 4 px left of every platform beneath it.
//
// Widths come from the widest entry in each column (measured in the font
// that row is drawn in, big for the first departure and small for the rest)
// so every row gets the same indent and nothing has to be nudged
// individually.
struct Columns {
    int plat_x = 0;
    int line_x = 0;
    int dest_x = 0;
    int time_x = 0;   // left edge of the time column
    int dest_w = 0;   // room for destination text between dest_x and time_x
    int plat_w = 0;   // column width, for the changed-platform highlight box
};

static Columns computeColumns(const Font &bigFont, const Font &smallFont,
                              const vector<Departure> &list, int width) {
    Columns c;
    c.plat_x = 5;

    // Two digits in the big font is the floor, so single-digit platforms
    // don't make the whole board jump inward when the data changes.
    c.plat_w   = led_util::MeasureText(bigFont, "55");
    int line_w = 0;
    for (size_t i = 0; i < list.size(); ++i) {
        const Font &f = (i == 0) ? bigFont : smallFont;
        c.plat_w = max(c.plat_w, MeasureTextHalfSpace(f, platformLabel(list[i])));
        line_w   = max(line_w,   MeasureTextHalfSpace(f, list[i].line));
    }

    c.line_x = c.plat_x + c.plat_w + 3;
    c.dest_x = c.line_x + line_w + 4;
    c.time_x = width - computeTimeReserve(bigFont, smallFont, list);
    c.dest_w = max(1, c.time_x - c.dest_x - 2);
    return c;
}

void drawStrikethrough(Canvas *canvas, int x, int baseline, int text_w,
                       const Font &font) {
    int mid_y = baseline - font.baseline() / 2;
    for (int px = x; px < x + text_w; ++px)
        canvas->SetPixel(px, mid_y, 255, 0, 0);
}

void drawRowBg(Canvas *canvas, int x, int y, int w, int h,
               uint8_t r, uint8_t g, uint8_t b) {
    for (int py = y; py < y + h; ++py)
        for (int px = x; px < x + w; ++px)
            canvas->SetPixel(px, py, r, g, b);
}

void drawInvertedRowBg(Canvas *canvas, int x, int y, int w, int h) {
    drawRowBg(canvas, x, y, w, h, 255, 255, 255);
}

// -------------------------------------------------------------------------
// Scroller state — rebuilt every time data changes
// -------------------------------------------------------------------------
struct ScrollerState {
    ScrollingTextBox *firstDestScroller  = nullptr;
    ScrollingTextBox *firstViaScroller   = nullptr;
    ScrollingTextBox *firstNoteScroller  = nullptr;
    ScrollingTextBox *tickerScroller     = nullptr;
    vector<ScrollingTextBox*> destScrollers;
    vector<ScrollingTextBox*> noteScrollers;
    ViewPort         *listVP             = nullptr;
    PageScroller     *pageScroll         = nullptr;
    led_util::ScrollBar *scrollBar       = nullptr;

    vector<int> row_y_offsets;
    int content_h    = 0;
    int row_h        = 0;
    int list_y       = 0;
    int list_h       = 0;
    int ticker_h     = 0;
    bool dep0_has_note = false;

    void destroy() {
        delete firstDestScroller; firstDestScroller = nullptr;
        delete firstViaScroller;  firstViaScroller  = nullptr;
        delete firstNoteScroller; firstNoteScroller = nullptr;
        delete tickerScroller;    tickerScroller    = nullptr;
        for (auto *s : destScrollers) delete s;
        destScrollers.clear();
        for (auto *s : noteScrollers) delete s;
        noteScrollers.clear();
        delete pageScroll; pageScroll = nullptr;
        delete listVP;     listVP     = nullptr;
        delete scrollBar;  scrollBar  = nullptr;
        row_y_offsets.clear();
    }

    ~ScrollerState() { destroy(); }
};

// Build all scrollers for the current departure list.
static void buildScrollers(ScrollerState &ss,
                           Canvas *canvas,
                           const Font &bigFont, const Font &smallFont,
                           const vector<Departure> &list,
                           const string &ticker,
                           int width, int height, const Columns &cols) {
    ss.destroy();

    // Destination x inside the list viewport: the same screen column as the
    // first departure's, shifted back by where the viewport itself starts.
    const int list_dest_x = cols.dest_x - LIST_CONTENT_X;

    // --- Layout constants ---
    const int first_dep_baseline = 30;
    const int via_baseline       = first_dep_baseline + smallFont.height();
    const int note0_baseline     = via_baseline + smallFont.height();
    ss.row_h                     = smallFont.height() + 2;
    ss.dep0_has_note             = !list[0].notes.empty();
    ss.list_y                    = (ss.dep0_has_note ? note0_baseline : via_baseline) + 4;
    ss.ticker_h                  = ticker.empty() ? 0 : (smallFont.height() + 2);
    ss.list_h                    = height - ss.list_y - ss.ticker_h;

    // First departure scrollers
    ss.firstDestScroller = new ScrollingTextBox(
        canvas, cols.dest_x, first_dep_baseline - bigFont.baseline(),
        cols.dest_w, bigFont.height(),
        bigFont, list[0].cancelled ? Color(0, 0, 120)
               : busStyled(list[0]) ? BUS_FG : Color(255, 255, 255), list[0].dest,
        20.0f, 2.0f, 12);

    string via_text = "via " + list[0].stops;
    ss.firstViaScroller = new ScrollingTextBox(
        canvas, cols.dest_x, via_baseline - smallFont.baseline(),
        cols.dest_w, smallFont.height(),
        smallFont, Color(180, 180, 180), via_text,
        20.0f, 2.0f, 12);

    if (ss.dep0_has_note) {
        string note0_str;
        for (size_t n = 0; n < list[0].notes.size(); ++n) {
            if (n > 0) note0_str += " | ";
            note0_str += list[0].notes[n].text;
        }
        ss.firstNoteScroller = new ScrollingTextBox(
            canvas, cols.dest_x, note0_baseline - smallFont.baseline(),
            cols.dest_w, smallFont.height(),
            smallFont, Color(255, 180, 0), note0_str,
            20.0f, 2.0f, 12);
    }

    // Page-scroller viewport for departures 1..N
    int total_content_h = 0;
    for (int i = 1; i < (int)list.size(); ++i) {
        ss.row_y_offsets.push_back(total_content_h);
        total_content_h += ss.row_h;
        if (!list[i].notes.empty()) total_content_h += ss.row_h;
    }
    ss.content_h = total_content_h;

    ss.listVP    = new ViewPort(canvas, LIST_CONTENT_X, ss.list_y,
                                width - LIST_CONTENT_X, ss.list_h);
    ss.pageScroll = new PageScroller(ss.listVP, ss.content_h, ss.row_h, 3.0f, 40.0f);
    ss.scrollBar  = new led_util::ScrollBar(canvas, SCROLLBAR_X, ss.list_y, ss.list_h, SCROLLBAR_W);

    // Ticker (only when there are station messages)
    if (!ticker.empty()) {
        ss.tickerScroller = new ScrollingTextBox(
            canvas, 0, height - ss.ticker_h,
            width, ss.ticker_h,
            smallFont, Color(200, 0, 0),
            ticker,
            35.0f, 2.0f, 40);
    }

    // Per-departure scrollers for list entries
    for (int i = 1; i < (int)list.size(); ++i) {
        int idx        = i - 1;
        int y_pos      = ss.row_y_offsets[idx];
        // Cancelled rows draw their destination via this scroller too (blue
        // on the inverted white row), so long names still clip and scroll
        // instead of running into the time column.
        ss.destScrollers.push_back(new ScrollingTextBox(
            ss.listVP, list_dest_x, y_pos + 1,
            cols.dest_w, ss.row_h,
            smallFont, list[i].cancelled ? Color(0, 0, 120)
                     : busStyled(list[i]) ? BUS_FG : Color(255, 255, 255), list[i].dest,
            20.0f, 1.5f, 12));
        if (!list[i].notes.empty()) {
            string note_str;
            for (size_t n = 0; n < list[i].notes.size(); ++n) {
                if (n > 0) note_str += " | ";
                note_str += list[i].notes[n].text;
            }
            ss.noteScrollers.push_back(new ScrollingTextBox(
                ss.listVP, list_dest_x, y_pos + ss.row_h + 1,
                cols.dest_w, ss.row_h,
                smallFont, Color(255, 180, 0), note_str,
                20.0f, 2.0f, 12));
        } else {
            ss.noteScrollers.push_back(nullptr);
        }
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

    // --- Column layout, shared by the first departure and the list rows ---
    Columns cols = computeColumns(bigFont, smallFont, list, width);

    // Layout constants
    const int header_bottom      = 14;
    const int first_dep_baseline = 30;

    // Build initial scroller state
    ScrollerState ss;
    buildScrollers(ss, off, bigFont, smallFont, list, ticker,
                   width, height, cols);

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
                cols = computeColumns(bigFont, smallFont, list, width);

                buildScrollers(ss, off, bigFont, smallFont, list, ticker,
                               width, height, cols);
            }
        }

        Canvas *c = off;

        // Dark blue background
        c->Fill(0, 0, 120);

        // Station name header
        DrawText(c, bigFont, cols.plat_x, bigFont.baseline() + 1,
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

        // --- First departure (big font) ---
        Departure &d = list[0];
        {
            int row_top = first_dep_baseline - bigFont.baseline();
            if (d.cancelled)
                drawInvertedRowBg(c, 0, row_top, width, bigFont.height());
            else if (busStyled(d))
                drawRowBg(c, 0, row_top, width, bigFont.height(),
                          BUS_BG_R, BUS_BG_G, BUS_BG_B);
        }
        {
            Color textCol = d.cancelled  ? Color(0, 0, 120)
                          : busStyled(d) ? BUS_FG : Color(255, 255, 255);
            drawPlatform(c, bigFont, cols.plat_x, first_dep_baseline, d, cols.plat_w,
                         textCol);
            DrawTextHalfSpace(c, bigFont, cols.line_x, first_dep_baseline,
                              (d.cancelled || busStyled(d)) ? textCol : lineColor(d.line),
                              d.line);
            ss.firstDestScroller->SetCanvas(c);
            ss.firstDestScroller->Update();
            int pt0_w = DrawText(c, bigFont, cols.time_x, first_dep_baseline,
                                 textCol, nullptr, d.pTime.c_str());
            if (d.cancelled) {
                drawStrikethrough(c, cols.time_x, first_dep_baseline, pt0_w, bigFont);
            } else if (!d.cTime.empty()) {
                DrawText(c, bigFont, cols.time_x + pt0_w + 2, first_dep_baseline,
                         Color(255, 60, 60), nullptr, d.cTime.c_str());
            }
            ss.firstViaScroller->SetCanvas(c);
            ss.firstViaScroller->Update();
            if (ss.firstNoteScroller) {
                ss.firstNoteScroller->SetCanvas(c);
                ss.firstNoteScroller->Update();
            }
        }

        // --- Following departures inside page-scrolled list ---
        ss.listVP->SetParent(c);
        ss.pageScroll->Update();

        ss.scrollBar->SetCanvas(c);
        ss.scrollBar->Draw(ss.pageScroll->scrollFraction(), ss.pageScroll->viewFraction(),
                           60, 60, 60,
                           255, 255, 255);

        // Inside the viewport every column sits LIST_CONTENT_X further left
        // than its screen position, which puts it in the same place on the
        // panel as the first departure's above.
        const int vp_plat_x = cols.plat_x - LIST_CONTENT_X;
        const int vp_line_x = cols.line_x - LIST_CONTENT_X;
        const int vp_time_x = cols.time_x - LIST_CONTENT_X;

        for (int i = 1; i < (int)list.size(); ++i) {
            int idx      = i - 1;
            int y_pos    = ss.row_y_offsets[idx];
            int baseline = y_pos + smallFont.baseline() + 1;

            if (list[i].cancelled) {
                drawInvertedRowBg(ss.listVP, 0, y_pos, width, ss.row_h);
                DrawTextHalfSpace(ss.listVP, smallFont, vp_plat_x, baseline,
                                  Color(0, 0, 120), platformLabel(list[i]));
                DrawTextHalfSpace(ss.listVP, smallFont, vp_line_x, baseline,
                                  Color(0, 0, 120), list[i].line);
                ss.destScrollers[idx]->SetCanvas(ss.listVP);
                ss.destScrollers[idx]->Update();
                int tw = DrawText(ss.listVP, smallFont, vp_time_x, baseline,
                                  Color(0, 0, 120), nullptr, list[i].pTime.c_str());
                drawStrikethrough(ss.listVP, vp_time_x, baseline, tw, smallFont);
            } else {
                bool bus = busStyled(list[i]);
                if (bus)
                    drawRowBg(ss.listVP, 0, y_pos, width, ss.row_h,
                              BUS_BG_R, BUS_BG_G, BUS_BG_B);
                Color rowCol = bus ? BUS_FG : Color(255, 255, 255);
                drawPlatform(ss.listVP, smallFont, vp_plat_x, baseline, list[i],
                             cols.plat_w, rowCol);
                DrawTextHalfSpace(ss.listVP, smallFont, vp_line_x, baseline,
                                  bus ? rowCol : lineColor(list[i].line), list[i].line);
                ss.destScrollers[idx]->SetCanvas(ss.listVP);
                ss.destScrollers[idx]->Update();
                int pt_w = DrawText(ss.listVP, smallFont, vp_time_x, baseline,
                                    rowCol, nullptr, list[i].pTime.c_str());
                DrawText(ss.listVP, smallFont, vp_time_x + pt_w + 2, baseline,
                         Color(255, 60, 60), nullptr, list[i].cTime.c_str());
            }

            if (ss.noteScrollers[idx]) {
                ss.noteScrollers[idx]->SetCanvas(ss.listVP);
                ss.noteScrollers[idx]->Update();
            }
        }

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
