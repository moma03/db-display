// cpp_backend.cpp
// Ported from the original db_display.cc. The drawing logic is deliberately
// unchanged -- the board should look identical after the split -- but every
// position, size, colour and font now comes from the layout config instead of
// a constant in this file.

#include "cpp_backend.h"

#include <page_scroller.h>
#include <scrolling_textbox.h>
#include <viewport.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <ctime>

using rgb_matrix::Canvas;
using rgb_matrix::Color;
using rgb_matrix::DrawText;
using rgb_matrix::Font;

namespace led {

namespace {

inline Color C(const Rgb &c) { return Color(c.r, c.g, c.b); }

// A "[" grouping bracket for wing trains: a 1px vertical bar with 3px ticks.
constexpr int kBracketW = 3;

// Draw text with every space at half width, so line names ("RE 11") and
// platform numbers stay compact. Returns the total width drawn, like DrawText.
int DrawTextHalfSpace(Canvas *canvas, const Font &font, int x, int y,
                      const Color &color, const std::string &text) {
    const int half_sp = (font.CharacterWidth(' ') + 1) / 2;
    int cursor = x;
    size_t pos = 0;
    while (pos < text.size()) {
        const size_t sp = text.find(' ', pos);
        if (sp == std::string::npos) {
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

int MeasureTextHalfSpace(const Font &font, const std::string &text) {
    static led_util::NullCanvas nc;
    return DrawTextHalfSpace(&nc, font, 0, 0, Color(0, 0, 0), text);
}

void DrawRowBg(Canvas *canvas, int x, int y, int w, int h,
               uint8_t r, uint8_t g, uint8_t b) {
    for (int py = y; py < y + h; ++py)
        for (int px = x; px < x + w; ++px)
            canvas->SetPixel(px, py, r, g, b);
}

void DrawStrikethrough(Canvas *canvas, int x, int baseline, int text_w, const Font &font) {
    const int mid_y = baseline - font.baseline() / 2;
    for (int px = x; px < x + text_w; ++px)
        canvas->SetPixel(px, mid_y, 255, 0, 0);
}

void DrawWingBracket(Canvas *canvas, int x, int y_top, int y_bottom, const Color &col) {
    for (int y = y_top; y < y_bottom; ++y)
        canvas->SetPixel(x, y, col.r, col.g, col.b);
    for (int dx = 0; dx < kBracketW; ++dx) {
        canvas->SetPixel(x + dx, y_top, col.r, col.g, col.b);
        canvas->SetPixel(x + dx, y_bottom - 1, col.r, col.g, col.b);
    }
}

}  // namespace

// ---------------------------------------------------------------------------
// DepartureScroller -- page scrolling that always lands on a departure.
//
// PageScroller advances by a fixed number of fixed-height items. Rows here are
// not fixed height (a departure carrying a delay reason is taller), so that
// arithmetic drifts and can leave a stray reason line alone at the top of the
// viewport. This one only ever comes to rest on the start of a departure.
// ---------------------------------------------------------------------------
class DepartureScroller {
public:
    DepartureScroller(ViewPort *vp, std::vector<int> starts, std::vector<int> heights,
                      int content_h, float wait, float speed)
        : vp_(vp), starts_(std::move(starts)), heights_(std::move(heights)),
          content_h_(content_h), wait_(wait), speed_(speed),
          last_(std::chrono::steady_clock::now()) {}

    void Update() {
        const int visible = vp_->visibleHeight();
        if (content_h_ <= visible) { cur_ = target_ = 0.0f; Apply(); return; }

        const auto now = std::chrono::steady_clock::now();
        const float dt = std::chrono::duration<float>(now - last_).count();

        if (scrolling_) {
            cur_ += speed_ * dt;
            last_ = now;
            if (cur_ >= target_) { cur_ = target_; scrolling_ = false; }
        } else if (dt >= wait_) {
            const int next = NextStart(visible);
            last_ = now;
            if (next < 0) cur_ = target_ = 0.0f;
            else { target_ = static_cast<float>(next); scrolling_ = true; }
        }
        Apply();
    }

    float scrollFraction() const {
        const int max_off = std::max(0, content_h_ - vp_->visibleHeight());
        return max_off <= 0 ? 0.0f : std::min(1.0f, cur_ / static_cast<float>(max_off));
    }
    float viewFraction() const {
        return content_h_ <= 0 ? 1.0f
             : std::min(1.0f, vp_->visibleHeight() / static_cast<float>(content_h_));
    }

private:
    int NextStart(int visible) const {
        const int top = static_cast<int>(target_ + 0.5f);
        const int bottom = top + visible;
        int next = -1;
        for (size_t i = 0; i < starts_.size(); ++i)
            if (starts_[i] + heights_[i] > bottom && starts_[i] > top) { next = starts_[i]; break; }
        if (next < 0) return -1;

        // Last page: scrolling all the way to `next` would run off the end and
        // leave a gap under the final departure. Back up to the earliest start
        // that still shows the tail in full -- repeating a row reads better
        // than empty space.
        if (next + visible > content_h_)
            for (size_t i = 0; i < starts_.size(); ++i)
                if (starts_[i] + visible >= content_h_ && starts_[i] > top) return starts_[i];
        return next;
    }
    void Apply() { vp_->SetScrollY(static_cast<int>(cur_)); }

    ViewPort *vp_;
    std::vector<int> starts_, heights_;
    int content_h_;
    float wait_, speed_;
    float cur_ = 0.0f, target_ = 0.0f;
    bool scrolling_ = false;
    std::chrono::steady_clock::time_point last_;
};

// ---------------------------------------------------------------------------

CppRenderBackend::~CppRenderBackend() { DestroyScrollers(); }

void CppRenderBackend::DestroyScrollers() {
    for (auto *s : heroDests_) delete s;
    heroDests_.clear();
    for (auto *s : heroVias_) delete s;
    heroVias_.clear();
    delete heroNote_;       heroNote_ = nullptr;
    delete tickerScroller_; tickerScroller_ = nullptr;
    for (auto *s : destScrollers_) delete s;
    destScrollers_.clear();
    for (auto *s : noteScrollers_) delete s;
    noteScrollers_.clear();
    delete pageScroll_; pageScroll_ = nullptr;
    delete listVP_;     listVP_ = nullptr;
    delete scrollBar_;  scrollBar_ = nullptr;
    row_y_offsets_.clear();
    row_heights_.clear();
    row_styles_.clear();
}

bool CppRenderBackend::LoadFonts(const LayoutConfig &lc, std::string *err) {
    std::map<std::string, std::unique_ptr<Font>> loaded;
    for (const auto &kv : lc.fonts) {
        auto f = std::make_unique<Font>();
        if (!f->LoadFont(kv.second.c_str())) {
            *err = "cannot load font '" + kv.first + "' from " + kv.second;
            return false;
        }
        loaded[kv.first] = std::move(f);
    }
    fonts_ = std::move(loaded);
    return true;
}

bool CppRenderBackend::Init(std::shared_ptr<const LayoutConfig> layout,
                            int w, int h, std::string *err) {
    width_ = w;
    height_ = h;
    canvas_ = std::make_unique<MemoryCanvas>(nullptr, w, h);
    return Reload(std::move(layout), err);
}

bool CppRenderBackend::Reload(std::shared_ptr<const LayoutConfig> layout, std::string *err) {
    if (layout->width != width_ || layout->height != height_) {
        *err = "layout canvas is " + std::to_string(layout->width) + "x" +
               std::to_string(layout->height) + " but the frame is " +
               std::to_string(width_) + "x" + std::to_string(height_);
        return false;
    }
    // Load fonts into a scratch map first; a bad font path must not leave the
    // backend half-configured.
    if (!LoadFonts(*layout, err)) return false;

    layout_    = std::move(layout);
    w_station_ = layout_->Find(WidgetType::kStationName);
    w_clock_   = layout_->Find(WidgetType::kClock);
    w_sep_     = layout_->Find(WidgetType::kSeparator);
    w_hero_    = layout_->Find(WidgetType::kHero);
    w_list_    = layout_->Find(WidgetType::kDepartureList);
    w_ticker_  = layout_->Find(WidgetType::kTicker);

    if (!w_hero_ || !w_list_) {
        *err = "layout must contain a \"hero\" and a \"departure_list\" widget";
        return false;
    }
    list_content_x_ = w_list_->scrollbar_x + w_list_->scrollbar_w + 1;

    data_dirty_ = true;   // geometry changed, so scrollers must be rebuilt
    return true;
}

void CppRenderBackend::SetData(const FrameData &d) {
    // Rebuilding the scrollers resets every scroll position, so only do it when
    // the refresh actually changed something.
    if (!data_dirty_ && d == data_) return;
    data_ = d;
    if (data_.departures.empty()) data_.departures.push_back(NoDeparturesPlaceholder());
    GroupWings(data_.departures);
    data_dirty_ = true;
}

CppRenderBackend::RowStyle CppRenderBackend::StyleFor(const Departure &dep,
                                                      const Widget &w) const {
    RowStyle s;
    s.fg    = C(w.color);
    s.via   = C(w.color_via);
    s.note  = C(w.color_note);
    s.delay = C(w.color_delay);

    if (dep.cancelled) {
        // Inverted: background-coloured text on a filled block.
        const Color bg = C(layout_->background);
        s.fg = s.via = s.note = s.delay = bg;
        s.fillBg = true;
        s.bg_r = w.cancelled_bg.r; s.bg_g = w.cancelled_bg.g; s.bg_b = w.cancelled_bg.b;
        s.lineTypeColor = false;
        s.strikeTime    = true;
    } else if (IsBus(dep)) {
        // Replacement buses: their own palette, because plain red would sink
        // into the background they sit on.
        s.fg    = C(w.bus_fg);
        s.via   = s.note = C(w.bus_sub);
        s.delay = C(w.bus_delay);
        s.fillBg = true;
        s.bg_r = w.bus_bg.r; s.bg_g = w.bus_bg.g; s.bg_b = w.bus_bg.b;
        s.lineTypeColor = false;
    }
    return s;
}

CppRenderBackend::Columns CppRenderBackend::ComputeColumns(
    const Font &font, const std::vector<const Departure *> &deps,
    int x0, int width, bool wings) const {
    Columns c;
    c.plat_x = x0;

    // Two digits is the floor, so single-digit platforms do not make the board
    // jump inward when the data changes.
    c.plat_w = led_util::MeasureText(font, "55");
    int line_w = 0, reserve = 0;
    for (const Departure *d : deps) {
        c.plat_w = std::max(c.plat_w, MeasureTextHalfSpace(font, PlatformLabel(*d)));
        line_w   = std::max(line_w, MeasureTextHalfSpace(font, d->line));
        int w = led_util::MeasureText(font, d->pTime);
        if (!d->cancelled && !d->cTime.empty())
            w += 2 + led_util::MeasureText(font, d->cTime);
        reserve = std::max(reserve, w);
    }

    const int after_plat = c.plat_x + c.plat_w;
    if (wings) {
        c.bracket_x = after_plat + 2;
        c.line_x    = c.bracket_x + kBracketW + 2;
    } else {
        c.line_x    = after_plat + 3;
    }
    c.dest_x = c.line_x + line_w + 4;
    c.time_x = width - reserve;
    c.dest_w = std::max(1, c.time_x - c.dest_x - 2);
    c.note_w = std::max(1, c.time_x - c.line_x - 2);
    return c;
}

void CppRenderBackend::BuildScrollers() {
    DestroyScrollers();

    // Invariant: there is always at least one departure to lay out. Enforced
    // here rather than trusting callers, because the drawing code indexes
    // list[0] unconditionally (the hero is always present).
    //
    // SetData() already guarantees this, but in live mode SetData() is not
    // called until the first database fetch returns -- so the first frame after
    // startup used to render against an empty vector and segfault. Demo mode
    // seeds data before the loop starts, which is exactly why this only ever
    // showed up against a real database.
    if (data_.departures.empty())
        data_.departures.push_back(NoDeparturesPlaceholder());

    const Widget &hw = *w_hero_;
    const Widget &lw = *w_list_;
    const std::vector<Departure> &list = data_.departures;

    const Font &bigFont   = *fonts_.at(hw.font_big);
    const Font &smallFont = *fonts_.at(lw.font);

    hero_count_ = HeroGroupSize(list);
    if (hero_count_ < 1) hero_count_ = 1;

    // --- column measurement, hero and list independently -------------------
    {
        std::vector<const Departure *> deps;
        for (int i = 0; i < hero_count_; ++i) deps.push_back(&list[i]);
        heroCols_ = ComputeColumns(bigFont, deps, hw.rect.x, width_, hero_count_ > 1);
    }
    {
        std::vector<const Departure *> deps;
        bool wings = false;
        for (size_t i = hero_count_; i < list.size(); ++i) {
            deps.push_back(&list[i]);
            if (list[i].wingCount > 1) wings = true;
        }
        // x0 = 1 leaves room for the changed-platform box, which starts at x-1.
        listCols_ = ComputeColumns(smallFont, deps, 1, width_ - list_content_x_, wings);
    }

    // --- hero block --------------------------------------------------------
    hero_style_      = StyleFor(list[0], hw);
    // `baseline` is relative to the hero's own rect, so moving the widget in
    // layout.json moves the text with it.
    hero_baseline_   = hw.rect.y + hw.baseline;
    hero_part_block_ = bigFont.height() + smallFont.height();
    const int last_via_base =
        hero_baseline_ + (hero_count_ - 1) * hero_part_block_ + smallFont.height();
    hero_note_baseline_ = last_via_base + smallFont.height();

    std::string heroNotes;
    for (int i = 0; i < hero_count_; ++i) {
        const std::string n = JoinNotes(list[i]);
        if (!n.empty()) { if (!heroNotes.empty()) heroNotes += " | "; heroNotes += n; }
    }
    const bool hero_has_note = !heroNotes.empty();
    const int hero_last_base = hero_has_note ? hero_note_baseline_ : last_via_base;
    hero_top_ = hero_baseline_ - bigFont.baseline();
    hero_h_   = (hero_last_base - smallFont.baseline() + smallFont.height()) - hero_top_;

    // Geometry comes from the layout file, not from where the hero happened to
    // end. The rects in layout.json are authoritative -- a layout file whose
    // rects do not control position is not much of a layout file.
    //
    // The hero is the one genuinely variable-height element (a two-part wing
    // group with a delay reason is much taller than a solo departure), so it
    // is the one that can outgrow its rect. That is reported rather than
    // silently pushing everything below it down the screen.
    row_h_    = smallFont.height() + lw.row_gap;
    ticker_h_ = (data_.ticker.empty() || !w_ticker_) ? 0 : w_ticker_->rect.h;

    // Geometry comes from the layout file. The rect fixes left, width and
    // BOTTOM; the top either comes from the rect too, or -- when the file says
    // `flow_after` -- follows the actual rendered bottom of the named widget.
    //
    // The hero is the one genuinely variable-height element: a two-part wing
    // group with a delay reason is more than twice as tall as a solo
    // departure. flow_after is how the layout file expresses that dependency,
    // instead of it being an unwritten rule inside this function.
    const int list_bottom = lw.rect.y + lw.rect.h;
    if (!lw.flow_after.empty()) {
        const Widget *anchor = layout_->FindById(lw.flow_after);
        int anchor_bottom = lw.rect.y;
        if (anchor == w_hero_) anchor_bottom = hero_top_ + hero_h_;
        else if (anchor)       anchor_bottom = anchor->rect.y + anchor->rect.h;
        list_y_ = std::max(lw.rect.y, anchor_bottom + lw.flow_gap);
    } else {
        list_y_ = lw.rect.y;
    }
    list_h_ = list_bottom - list_y_;

    if (list_h_ < row_h_) {
        // The hero has eaten the list. Clamp so the viewport stays valid and
        // say so -- silently rendering a zero-height list would just look like
        // the departures had vanished.
        std::fprintf(stderr,
            "renderer: hero content (%d px) leaves only %d px for the "
            "departure list; clamping to one row. Grow the list rect or move "
            "the hero up in layout.json.\n",
            hero_top_ + hero_h_ - hw.rect.y, list_h_);
        list_h_ = row_h_;
    }

    for (int i = 0; i < hero_count_; ++i) {
        const int big_base = hero_baseline_ + i * hero_part_block_;
        const int via_base = big_base + smallFont.height();
        const Color destCol =
            (list[i].destChanged && hero_style_.lineTypeColor)
                ? C(hw.color_dest_changed) : hero_style_.fg;
        heroDests_.push_back(new ScrollingTextBox(
            canvas_.get(), heroCols_.dest_x, big_base - bigFont.baseline(),
            heroCols_.dest_w, bigFont.height(), bigFont, destCol, list[i].dest,
            hw.dest_scroll.speed_px_s, hw.dest_scroll.wait_s, hw.dest_scroll.gap_px));
        heroVias_.push_back(new ScrollingTextBox(
            canvas_.get(), heroCols_.dest_x, via_base - smallFont.baseline(),
            heroCols_.dest_w, smallFont.height(), smallFont, hero_style_.via,
            "via " + list[i].stops,
            hw.scroll.speed_px_s, hw.scroll.wait_s, hw.scroll.gap_px));
    }
    if (hero_has_note) {
        heroNote_ = new ScrollingTextBox(
            canvas_.get(), heroCols_.dest_x, hero_note_baseline_ - smallFont.baseline(),
            heroCols_.dest_w, smallFont.height(), smallFont, hero_style_.note, heroNotes,
            hw.scroll.speed_px_s, hw.scroll.wait_s, hw.scroll.gap_px);
    }

    // --- list rows: measure each departure's full height up front ----------
    const int note_h = row_h_ - lw.note_tighten;
    int total = 0;
    for (size_t i = hero_count_; i < list.size(); ++i) {
        const int h = row_h_ + (list[i].notes.empty() ? 0 : note_h);
        row_y_offsets_.push_back(total);
        row_heights_.push_back(h);
        row_styles_.push_back(StyleFor(list[i], lw));
        total += h;
    }
    content_h_ = total;

    listVP_ = new ViewPort(canvas_.get(), list_content_x_, list_y_,
                           width_ - list_content_x_, list_h_);
    pageScroll_ = new DepartureScroller(listVP_, row_y_offsets_, row_heights_,
                                        content_h_, lw.scroll.wait_s, lw.scroll.speed_px_s);
    scrollBar_ = new led_util::ScrollBar(canvas_.get(), lw.scrollbar_x, list_y_,
                                         list_h_, lw.scrollbar_w);

    if (ticker_h_ > 0) {
        tickerScroller_ = new ScrollingTextBox(
            canvas_.get(), w_ticker_->rect.x, w_ticker_->rect.y,
            w_ticker_->rect.w, ticker_h_, smallFont, C(w_ticker_->color), data_.ticker,
            w_ticker_->scroll.speed_px_s, w_ticker_->scroll.wait_s, w_ticker_->scroll.gap_px);
    }

    for (size_t i = hero_count_; i < list.size(); ++i) {
        const size_t idx = i - hero_count_;
        const int y_pos = row_y_offsets_[idx];
        const RowStyle &st = row_styles_[idx];
        const Color destCol =
            (list[i].destChanged && st.lineTypeColor) ? C(lw.color_dest_changed) : st.fg;

        destScrollers_.push_back(new ScrollingTextBox(
            listVP_, listCols_.dest_x, y_pos + 1, listCols_.dest_w, row_h_,
            smallFont, destCol, list[i].dest,
            lw.dest_scroll.speed_px_s, lw.dest_scroll.wait_s, lw.dest_scroll.gap_px));

        if (!list[i].notes.empty()) {
            // The reason line only has to clear the platform -- it may run
            // under the line number, which buys it a lot of reading width.
            noteScrollers_.push_back(new ScrollingTextBox(
                listVP_, listCols_.line_x, y_pos + row_h_ - lw.note_tighten + 1,
                listCols_.note_w, note_h, smallFont, st.note, JoinNotes(list[i]),
                lw.dest_scroll.speed_px_s, lw.dest_scroll.wait_s, lw.dest_scroll.gap_px));
        } else {
            noteScrollers_.push_back(nullptr);
        }
    }
}

void CppRenderBackend::DrawHeader(Canvas *c) {
    const LayoutConfig &lc = *layout_;

    if (w_station_) {
        const Font &f = *fonts_.at(w_station_->font);
        DrawText(c, f, w_station_->rect.x, w_station_->rect.y + f.baseline() + 1,
                 C(w_station_->color), nullptr, data_.station.c_str());
    }

    if (w_clock_) {
        const Font &f = *fonts_.at(w_clock_->font);
        const time_t now_t = time(nullptr);
        struct tm tmv;
        localtime_r(&now_t, &tmv);
        char clk[64];
        strftime(clk, sizeof(clk), w_clock_->time_format.c_str(), &tmv);

        const int clk_w = led_util::MeasureText(f, clk);
        const int clk_x = (w_clock_->align == "right")
                            ? w_clock_->rect.x + w_clock_->rect.w - clk_w
                            : w_clock_->rect.x;
        // Clear behind the clock first, so a long station name can never bleed
        // into the time.
        DrawRowBg(c, clk_x - 2, w_clock_->rect.y,
                  w_clock_->rect.w - (clk_x - w_clock_->rect.x) + 2, w_clock_->rect.h,
                  lc.background.r, lc.background.g, lc.background.b);
        DrawText(c, f, clk_x, w_clock_->rect.y + f.baseline() + 1,
                 C(w_clock_->color), nullptr, clk);
    }

    if (w_sep_) {
        DrawRowBg(c, w_sep_->rect.x, w_sep_->rect.y, w_sep_->rect.w,
                  std::max(1, w_sep_->thickness),
                  w_sep_->color.r, w_sep_->color.g, w_sep_->color.b);
    }
}

void CppRenderBackend::DrawHero(Canvas *c) {
    const Widget &hw = *w_hero_;
    const Font &bigFont = *fonts_.at(hw.font_big);
    const std::vector<Departure> &list = data_.departures;
    const RowStyle &st = hero_style_;

    if (st.fillBg)
        DrawRowBg(c, w_hero_->rect.x, hero_top_, w_hero_->rect.w, hero_h_,
                  st.bg_r, st.bg_g, st.bg_b);

    for (int i = 0; i < hero_count_; ++i) {
        const Departure &dep = list[i];
        const int base = hero_baseline_ + i * hero_part_block_;

        // Platform (or BUS) once, on the first part of a coupled group.
        if (i == 0) {
            const std::string &plat = PlatformLabel(dep);
            const bool changed = (!dep.cPlatform.empty() && dep.cPlatform != dep.platform);
            // Changed platforms blink like the real DB boards: every 500 ms the
            // platform swaps to a filled box in this row's colours.
            const bool inverted = changed &&
                (std::chrono::duration_cast<std::chrono::milliseconds>(
                     std::chrono::steady_clock::now().time_since_epoch()).count() / 500) % 2 == 0;
            if (inverted) {
                const int box_w = std::max(heroCols_.plat_w, MeasureTextHalfSpace(bigFont, plat));
                const int top = base - bigFont.baseline();
                DrawRowBg(c, heroCols_.plat_x - 1, top, box_w + 2, bigFont.height(),
                          st.fg.r, st.fg.g, st.fg.b);
                const Color txt = st.fillBg ? Color(st.bg_r, st.bg_g, st.bg_b)
                                            : C(layout_->background);
                DrawTextHalfSpace(c, bigFont, heroCols_.plat_x, base, txt, plat);
            } else {
                DrawTextHalfSpace(c, bigFont, heroCols_.plat_x, base, st.fg, plat);
            }
        }

        DrawTextHalfSpace(c, bigFont, heroCols_.line_x, base,
                          st.lineTypeColor ? C(layout_->LineColor(dep.line)) : st.fg,
                          dep.line);
        heroDests_[i]->SetCanvas(c);
        heroDests_[i]->Update();

        const int pt_w = DrawText(c, bigFont, heroCols_.time_x, base, st.fg, nullptr,
                                  dep.pTime.c_str());
        if (st.strikeTime)
            DrawStrikethrough(c, heroCols_.time_x, base, pt_w, bigFont);
        else if (!dep.cTime.empty())
            DrawText(c, bigFont, heroCols_.time_x + pt_w + 2, base, st.delay, nullptr,
                     dep.cTime.c_str());

        heroVias_[i]->SetCanvas(c);
        heroVias_[i]->Update();
    }

    if (hero_count_ > 1 && heroCols_.bracket_x >= 0)
        DrawWingBracket(c, heroCols_.bracket_x, hero_top_,
                        hero_top_ + hero_count_ * hero_part_block_, st.fg);

    if (heroNote_) { heroNote_->SetCanvas(c); heroNote_->Update(); }
}

void CppRenderBackend::DrawListRow(Canvas *vp, const Departure &dep, int idx, int width) {
    const Widget &lw = *w_list_;
    const Font &smallFont = *fonts_.at(lw.font);
    const RowStyle &st = row_styles_[idx];
    const int y_pos = row_y_offsets_[idx];
    const int baseline = y_pos + smallFont.baseline() + 1;

    if (st.fillBg)
        DrawRowBg(vp, 0, y_pos, width, row_heights_[idx], st.bg_r, st.bg_g, st.bg_b);

    if (dep.wingPos == 0) {
        const std::string &plat = PlatformLabel(dep);
        const bool changed = (!dep.cPlatform.empty() && dep.cPlatform != dep.platform);
        const bool inverted = changed &&
            (std::chrono::duration_cast<std::chrono::milliseconds>(
                 std::chrono::steady_clock::now().time_since_epoch()).count() / 500) % 2 == 0;
        if (inverted) {
            const int box_w = std::max(listCols_.plat_w, MeasureTextHalfSpace(smallFont, plat));
            const int top = baseline - smallFont.baseline();
            DrawRowBg(vp, listCols_.plat_x - 1, top, box_w + 2, smallFont.height(),
                      st.fg.r, st.fg.g, st.fg.b);
            const Color txt = st.fillBg ? Color(st.bg_r, st.bg_g, st.bg_b)
                                        : C(layout_->background);
            DrawTextHalfSpace(vp, smallFont, listCols_.plat_x, baseline, txt, plat);
        } else {
            DrawTextHalfSpace(vp, smallFont, listCols_.plat_x, baseline, st.fg, plat);
        }
    }

    if (dep.wingPos == 0 && dep.wingCount > 1 && listCols_.bracket_x >= 0) {
        const int last = std::min(idx + dep.wingCount - 1,
                                  static_cast<int>(row_y_offsets_.size()) - 1);
        DrawWingBracket(vp, listCols_.bracket_x, row_y_offsets_[idx],
                        row_y_offsets_[last] + row_h_, st.fg);
    }

    DrawTextHalfSpace(vp, smallFont, listCols_.line_x, baseline,
                      st.lineTypeColor ? C(layout_->LineColor(dep.line)) : st.fg, dep.line);

    destScrollers_[idx]->SetCanvas(vp);
    destScrollers_[idx]->Update();

    const int pt_w = DrawText(vp, smallFont, listCols_.time_x, baseline, st.fg, nullptr,
                              dep.pTime.c_str());
    if (st.strikeTime)
        DrawStrikethrough(vp, listCols_.time_x, baseline, pt_w, smallFont);
    else if (!dep.cTime.empty())
        DrawText(vp, smallFont, listCols_.time_x + pt_w + 2, baseline, st.delay, nullptr,
                 dep.cTime.c_str());

    if (noteScrollers_[idx]) {
        noteScrollers_[idx]->SetCanvas(vp);
        noteScrollers_[idx]->Update();
    }
}

void CppRenderBackend::RenderFrame(uint8_t *rgb, int64_t /*t_ns*/) {
    canvas_->Retarget(rgb);

    if (data_dirty_) { BuildScrollers(); data_dirty_ = false; }

    Canvas *c = canvas_.get();
    const LayoutConfig &lc = *layout_;

    // Every pixel is written: the slot may hold an older frame, and a partial
    // write would ghost.
    c->Fill(lc.background.r, lc.background.g, lc.background.b);

    DrawHeader(c);
    DrawHero(c);

    listVP_->SetParent(c);
    pageScroll_->Update();

    scrollBar_->SetCanvas(c);
    scrollBar_->Draw(pageScroll_->scrollFraction(), pageScroll_->viewFraction(),
                     w_list_->scrollbar_track.r, w_list_->scrollbar_track.g,
                     w_list_->scrollbar_track.b,
                     w_list_->scrollbar_thumb.r, w_list_->scrollbar_thumb.g,
                     w_list_->scrollbar_thumb.b);

    for (size_t i = hero_count_; i < data_.departures.size(); ++i)
        DrawListRow(listVP_, data_.departures[i], static_cast<int>(i) - hero_count_,
                    width_ - list_content_x_);

    if (tickerScroller_) {
        DrawRowBg(c, w_ticker_->rect.x, w_ticker_->rect.y, w_ticker_->rect.w, ticker_h_,
                  w_ticker_->ticker_bg.r, w_ticker_->ticker_bg.g, w_ticker_->ticker_bg.b);
        tickerScroller_->SetCanvas(c);
        tickerScroller_->Update();
    }
}

}  // namespace led
