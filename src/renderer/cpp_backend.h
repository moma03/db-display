// cpp_backend.h
// The C++ drawing backend: the dashboard as it looks today, ported from the
// old single-process db_display.cc, with everything the layout file owns
// (positions, sizes, colours, fonts) lifted out of the source.

#ifndef LED_RENDERER_CPP_BACKEND_H
#define LED_RENDERER_CPP_BACKEND_H

#include <graphics.h>

#include <map>
#include <memory>
#include <string>
#include <vector>

#include "departures.h"
#include "memory_canvas.h"
#include "render_backend.h"

class ViewPort;
class ScrollingTextBox;
namespace led_util { class ScrollBar; }

namespace led {

// What the data layer produces, and the only thing the drawing code consumes.
struct FrameData {
    std::vector<Departure> departures;
    std::string station;
    std::string ticker;
    bool operator==(const FrameData &o) const {
        return station == o.station && ticker == o.ticker && departures == o.departures;
    }
};

class CppRenderBackend : public IRenderBackend {
public:
    ~CppRenderBackend() override;

    bool Init(std::shared_ptr<const LayoutConfig> layout, int w, int h,
              std::string *err) override;
    bool Reload(std::shared_ptr<const LayoutConfig> layout, std::string *err) override;
    void SetData(const FrameData &data) override;
    void RenderFrame(uint8_t *rgb, int64_t t_ns) override;

private:
    // Colours and background treatment for one row, so the drawing code never
    // has to branch on cancelled/bus at each site.
    struct RowStyle {
        rgb_matrix::Color fg{255, 255, 255};
        rgb_matrix::Color via{180, 180, 180};
        rgb_matrix::Color note{255, 180, 0};
        rgb_matrix::Color delay{255, 60, 60};
        bool    fillBg = false;
        uint8_t bg_r = 0, bg_g = 0, bg_b = 0;
        bool    lineTypeColor = true;
        bool    strikeTime    = false;
    };

    struct Columns {
        int plat_x = 0, bracket_x = -1, line_x = 0, dest_x = 0;
        int time_x = 0, dest_w = 0, note_w = 0, plat_w = 0;
    };

    bool IsWidgetVisible(const Widget &w) const;
    int  WidgetBottom(const Widget &w) const;
    bool LoadFonts(const LayoutConfig &lc, std::string *err);
    void DestroyScrollers();
    void BuildScrollers();

    RowStyle StyleFor(const Departure &d, const Widget &w) const;
    Columns  ComputeColumns(const rgb_matrix::Font &f,
                            const std::vector<const Departure *> &deps,
                            int x0, int width, bool wings) const;

    void DrawHero(rgb_matrix::Canvas *c);
    void DrawListRow(rgb_matrix::Canvas *vp, const Departure &d, int idx, int width);
    void DrawHeader(rgb_matrix::Canvas *c);

    // --- layout / fonts ---
    std::shared_ptr<const LayoutConfig> layout_;
    std::map<std::string, std::unique_ptr<rgb_matrix::Font>> fonts_;
    int width_ = 0, height_ = 0;

    // Resolved widget pointers into layout_ (owned by the shared_ptr).
    const Widget *w_station_ = nullptr;
    const Widget *w_clock_   = nullptr;
    const Widget *w_sep_     = nullptr;
    const Widget *w_hero_    = nullptr;
    const Widget *w_list_    = nullptr;
    const Widget *w_ticker_  = nullptr;

    // --- data ---
    FrameData data_;
    bool data_dirty_ = true;

    // --- scroller state, rebuilt whenever data or layout changes ---
    std::unique_ptr<MemoryCanvas> canvas_;
    std::vector<ScrollingTextBox *> heroDests_, heroVias_;
    ScrollingTextBox *heroNote_ = nullptr;
    ScrollingTextBox *tickerScroller_ = nullptr;
    std::vector<ScrollingTextBox *> destScrollers_, noteScrollers_;
    ViewPort *listVP_ = nullptr;
    led_util::ScrollBar *scrollBar_ = nullptr;
    class DepartureScroller *pageScroll_ = nullptr;

    RowStyle hero_style_;
    Columns  heroCols_, listCols_;
    int hero_count_ = 1;
    int hero_top_ = 0, hero_h_ = 0, hero_baseline_ = 0;
    int hero_note_baseline_ = 0, hero_part_block_ = 0;
    std::vector<int> row_y_offsets_, row_heights_;
    std::vector<RowStyle> row_styles_;
    int content_h_ = 0, row_h_ = 0, list_y_ = 0, list_h_ = 0, ticker_h_ = 0;
    int list_content_x_ = 4;
    // Last-logged geometry, so the resolved-layout line is printed on
    // change rather than on every data refresh.
    int logged_list_y_ = -1, logged_list_h_ = -1;
    int logged_hero_h_ = -1, logged_ticker_h_ = -1;
};

}  // namespace led

#endif  // LED_RENDERER_CPP_BACKEND_H
