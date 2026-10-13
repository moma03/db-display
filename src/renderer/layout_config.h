// layout_config.h
// The hot-reloadable dashboard layout.
//
// Everything here comes from layout.json and can change at run time without a
// restart or a recompile. A reload that fails to parse or validate is
// discarded and the running layout keeps going -- a half-saved file must never
// blank the panel.

#ifndef LED_RENDERER_LAYOUT_CONFIG_H
#define LED_RENDERER_LAYOUT_CONFIG_H

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace led {

struct Rgb {
    uint8_t r = 255, g = 255, b = 255;
    bool operator==(const Rgb &o) const { return r == o.r && g == o.g && b == o.b; }
};

struct Rect {
    int x = 0, y = 0, w = 0, h = 0;
};

enum class WidgetType {
    kStationName,
    kClock,
    kSeparator,
    kHero,
    kDepartureList,
    kTicker,
};

struct ScrollSpec {
    float speed_px_s = 20.0f;
    float wait_s     = 2.0f;
    int   gap_px     = 12;
};

// One entry in layout.json's "widgets" array. Not every field applies to every
// type; the parser only reads what the type uses and reports the rest.
struct Widget {
    WidgetType  type = WidgetType::kStationName;
    std::string id;
    Rect        rect;
    std::string font   = "small";   // key into LayoutConfig::fonts
    std::string source;             // data source name
    Rgb         color{255, 255, 255};

    // clock
    std::string time_format = "%H:%M:%S";
    std::string align       = "left";   // left | right

    // hero / departure_list row colouring
    Rgb  color_via{180, 180, 180};
    Rgb  color_note{255, 180, 0};
    Rgb  color_delay{255, 60, 60};
    Rgb  color_dest_changed{255, 60, 60};
    // Hero via line of a train on a changed route.
    Rgb  color_via_cancelled{255, 60, 60};   // dropped stops, also struck through
    Rgb  color_via_added{60, 220, 60};       // stops not in the plan
    // Bus and cancelled rows invert or tint; these are their palettes.
    Rgb  bus_fg{255, 255, 0};
    Rgb  bus_bg{90, 0, 130};
    Rgb  bus_sub{190, 150, 0};
    Rgb  bus_delay{255, 120, 90};
    Rgb  cancelled_bg{255, 255, 255};

    // hero
    std::string font_big = "big";
    int  baseline        = 27;

    // departure_list
    ScrollSpec  scroll;
    ScrollSpec  dest_scroll;
    int  scrollbar_x = 1;
    int  scrollbar_w = 2;
    Rgb  scrollbar_track{60, 60, 60};
    Rgb  scrollbar_thumb{255, 255, 255};
    int  row_gap       = 2;
    int  note_tighten  = 2;

    // ticker
    Rgb  ticker_bg{255, 255, 255};

    // separator
    int  thickness = 1;

    // Vertical auto-flow. Two widgets here are variable height, and a layout
    // that reserves worst-case space for both wastes most of the panel most of
    // the time:
    //
    //   * the hero is 19 px for a solo departure and 47 px for a two-part wing
    //     group carrying a delay reason;
    //   * the ticker only exists while there is a disruption message.
    //
    // So a widget between them can have neither a fixed top nor a fixed
    // bottom. `flow_after` makes the TOP follow the named widget's actual
    // rendered bottom; `flow_before` makes the BOTTOM follow the named
    // widget's top -- or, when that widget is not currently shown, extend to
    // the bottom of the canvas so its space is reclaimed rather than left
    // blank.
    //
    // With both set, `rect` supplies only x and width. That is the honest
    // reading: the vertical extent genuinely depends on the data, and the
    // layout file says so rather than the drawing code deciding silently.
    std::string flow_after;
    int         flow_gap = 3;
    std::string flow_before;
    int         flow_gap_before = 0;
};

struct LayoutConfig {
    int  width  = 256;
    int  height = 128;
    Rgb  background{0, 0, 120};

    // Logical font name -> .bdf path (relative to the config file's directory
    // unless absolute).
    std::map<std::string, std::string> fonts;

    // Line-name colouring, e.g. {"S": "#FFFF00", "ICE": "#FFFFFF"}. Longest
    // matching prefix wins; "default" is the fallback.
    std::map<std::string, Rgb> line_colors;
    Rgb line_color_default{255, 60, 60};

    std::vector<Widget> widgets;

    // For change detection on reload, so an unchanged file does not rebuild
    // scrollers and reset every scroll position.
    uint64_t fingerprint = 0;

    const Widget *Find(WidgetType t) const;
    const Widget *FindById(const std::string &id) const;
    Rgb LineColor(const std::string &line) const;
};

// Parse and fully validate. On failure `err` explains why and *out is
// untouched, so the caller can keep running the previous layout.
bool LoadLayoutConfig(const std::string &path, LayoutConfig *out, std::string *err);

bool ParseHexColor(const std::string &s, Rgb *out);

}  // namespace led

#endif  // LED_RENDERER_LAYOUT_CONFIG_H
