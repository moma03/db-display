#include "layout_config.h"

#include <fstream>
#include <sstream>
#include <utility>

#include <json.hpp>

using json = nlohmann::json;

namespace led {

bool ParseHexColor(const std::string &s, Rgb *out) {
    if (s.size() != 7 || s[0] != '#') return false;
    auto hex = [](char c, int *v) {
        if (c >= '0' && c <= '9') { *v = c - '0';      return true; }
        if (c >= 'a' && c <= 'f') { *v = c - 'a' + 10; return true; }
        if (c >= 'A' && c <= 'F') { *v = c - 'A' + 10; return true; }
        return false;
    };
    int v[6];
    for (int i = 0; i < 6; ++i)
        if (!hex(s[1 + i], &v[i])) return false;
    out->r = static_cast<uint8_t>(v[0] * 16 + v[1]);
    out->g = static_cast<uint8_t>(v[2] * 16 + v[3]);
    out->b = static_cast<uint8_t>(v[4] * 16 + v[5]);
    return true;
}

const Widget *LayoutConfig::FindById(const std::string &id) const {
    for (const Widget &w : widgets)
        if (w.id == id) return &w;
    return nullptr;
}

const Widget *LayoutConfig::Find(WidgetType t) const {
    for (const Widget &w : widgets)
        if (w.type == t) return &w;
    return nullptr;
}

Rgb LayoutConfig::LineColor(const std::string &line) const {
    // Longest matching prefix wins, so "ICE" beats a hypothetical "I".
    const Rgb *best = nullptr;
    size_t best_len = 0;
    for (const auto &kv : line_colors) {
        const std::string &pfx = kv.first;
        if (pfx == "default" || pfx.size() <= best_len) continue;
        if (line.compare(0, pfx.size(), pfx) != 0) continue;
        // A single-letter prefix like "S" must not match "SEV": require the
        // next character to be a separator or end of string.
        if (line.size() > pfx.size()) {
            const char nxt = line[pfx.size()];
            if (nxt != ' ' && !(nxt >= '0' && nxt <= '9')) continue;
        }
        best = &kv.second;
        best_len = pfx.size();
    }
    if (best) return *best;
    auto it = line_colors.find("default");
    return it != line_colors.end() ? it->second : line_color_default;
}

namespace {

struct ParseCtx {
    std::string *err;
    bool ok = true;
    bool Fail(const std::string &m) { *err = m; ok = false; return false; }
};

bool GetColor(const json &j, const char *key, Rgb *out, ParseCtx *ctx) {
    if (!j.contains(key)) return true;
    if (!j[key].is_string()) return ctx->Fail(std::string(key) + " must be a \"#RRGGBB\" string");
    const std::string s = j[key].get<std::string>();
    if (!ParseHexColor(s, out))
        return ctx->Fail(std::string(key) + ": '" + s + "' is not #RRGGBB");
    return true;
}

bool GetRect(const json &j, Rect *out, ParseCtx *ctx) {
    if (!j.contains("rect")) return ctx->Fail("widget is missing \"rect\"");
    const json &r = j["rect"];
    if (!r.is_array() || r.size() != 4)
        return ctx->Fail("\"rect\" must be [x, y, w, h]");
    out->x = r[0].get<int>(); out->y = r[1].get<int>();
    out->w = r[2].get<int>(); out->h = r[3].get<int>();
    return true;
}

void GetScroll(const json &j, const char *key, ScrollSpec *out) {
    if (!j.contains(key)) return;
    const json &s = j[key];
    if (s.contains("speed_px_s")) out->speed_px_s = s["speed_px_s"].get<float>();
    if (s.contains("wait_s"))     out->wait_s     = s["wait_s"].get<float>();
    if (s.contains("gap_px"))     out->gap_px     = s["gap_px"].get<int>();
}

bool ParseType(const std::string &s, WidgetType *out) {
    if (s == "station_name")   { *out = WidgetType::kStationName;   return true; }
    if (s == "clock")          { *out = WidgetType::kClock;         return true; }
    if (s == "separator")      { *out = WidgetType::kSeparator;     return true; }
    if (s == "hero")           { *out = WidgetType::kHero;          return true; }
    if (s == "departure_list") { *out = WidgetType::kDepartureList; return true; }
    if (s == "ticker")         { *out = WidgetType::kTicker;        return true; }
    return false;
}

std::string DirOf(const std::string &path) {
    const size_t slash = path.rfind('/');
    return (slash == std::string::npos) ? std::string(".") : path.substr(0, slash);
}

}  // namespace

bool LoadLayoutConfig(const std::string &path, LayoutConfig *out, std::string *err) {
    std::ifstream f(path);
    if (!f.is_open()) { *err = "cannot open " + path; return false; }

    std::stringstream ss;
    ss << f.rdbuf();
    const std::string text = ss.str();

    json j;
    try {
        j = json::parse(text, nullptr, true, /*ignore_comments=*/true);
    } catch (const std::exception &e) {
        *err = std::string("parse error: ") + e.what();
        return false;
    }

    // Build into a scratch object; *out is only touched once everything
    // validates, so a bad edit leaves the running layout alone.
    LayoutConfig cfg;
    ParseCtx ctx{err};

    try {
        if (j.contains("canvas")) {
            const json &c = j["canvas"];
            if (c.contains("width"))  cfg.width  = c["width"].get<int>();
            if (c.contains("height")) cfg.height = c["height"].get<int>();
            if (!GetColor(c, "background", &cfg.background, &ctx)) return false;
        }

        if (j.contains("fonts")) {
            const std::string base = DirOf(path);
            for (auto it = j["fonts"].begin(); it != j["fonts"].end(); ++it) {
                std::string p = it.value().get<std::string>();
                if (!p.empty() && p[0] != '/') p = base + "/" + p;
                cfg.fonts[it.key()] = p;
            }
        }
        if (cfg.fonts.empty()) { *err = "\"fonts\" must define at least one font"; return false; }

        if (j.contains("line_colors")) {
            for (auto it = j["line_colors"].begin(); it != j["line_colors"].end(); ++it) {
                Rgb c;
                if (!ParseHexColor(it.value().get<std::string>(), &c)) {
                    *err = "line_colors." + it.key() + " is not #RRGGBB";
                    return false;
                }
                cfg.line_colors[it.key()] = c;
            }
        }

        if (!j.contains("widgets") || !j["widgets"].is_array()) {
            *err = "\"widgets\" must be an array";
            return false;
        }

        for (const json &wj : j["widgets"]) {
            Widget w;
            if (!wj.contains("type")) { *err = "widget is missing \"type\""; return false; }
            const std::string type = wj["type"].get<std::string>();
            if (!ParseType(type, &w.type)) { *err = "unknown widget type '" + type + "'"; return false; }
            if (!GetRect(wj, &w.rect, &ctx)) return false;

            if (wj.contains("id"))     w.id     = wj["id"].get<std::string>();
            if (wj.contains("font"))   w.font   = wj["font"].get<std::string>();
            if (wj.contains("source")) w.source = wj["source"].get<std::string>();
            if (!GetColor(wj, "color", &w.color, &ctx)) return false;

            if (wj.contains("format")) w.time_format = wj["format"].get<std::string>();
            if (wj.contains("align"))  w.align       = wj["align"].get<std::string>();
            if (wj.contains("thickness")) w.thickness = wj["thickness"].get<int>();
            if (wj.contains("font_big"))  w.font_big  = wj["font_big"].get<std::string>();
            if (wj.contains("baseline"))  w.baseline  = wj["baseline"].get<int>();
            if (wj.contains("flow_after")) w.flow_after = wj["flow_after"].get<std::string>();
            if (wj.contains("flow_gap"))   w.flow_gap   = wj["flow_gap"].get<int>();
            if (wj.contains("flow_before")) w.flow_before = wj["flow_before"].get<std::string>();
            if (wj.contains("flow_gap_before"))
                w.flow_gap_before = wj["flow_gap_before"].get<int>();
            if (wj.contains("row_gap"))      w.row_gap      = wj["row_gap"].get<int>();
            if (wj.contains("note_tighten")) w.note_tighten = wj["note_tighten"].get<int>();

            if (wj.contains("colors")) {
                const json &c = wj["colors"];
                if (!GetColor(c, "fg",   &w.color,       &ctx)) return false;
                if (!GetColor(c, "via",  &w.color_via,   &ctx)) return false;
                if (!GetColor(c, "note", &w.color_note,  &ctx)) return false;
                if (!GetColor(c, "delay", &w.color_delay, &ctx)) return false;
                if (!GetColor(c, "dest_changed", &w.color_dest_changed, &ctx)) return false;
                if (!GetColor(c, "via_cancelled", &w.color_via_cancelled, &ctx)) return false;
                if (!GetColor(c, "via_added", &w.color_via_added, &ctx)) return false;
                if (!GetColor(c, "bus_fg",    &w.bus_fg,    &ctx)) return false;
                if (!GetColor(c, "bus_bg",    &w.bus_bg,    &ctx)) return false;
                if (!GetColor(c, "bus_sub",   &w.bus_sub,   &ctx)) return false;
                if (!GetColor(c, "bus_delay", &w.bus_delay, &ctx)) return false;
                if (!GetColor(c, "cancelled_bg", &w.cancelled_bg, &ctx)) return false;
                if (!GetColor(c, "background", &w.ticker_bg, &ctx)) return false;
            }

            GetScroll(wj, "scroll", &w.scroll);
            GetScroll(wj, "dest_scroll", &w.dest_scroll);

            if (wj.contains("scrollbar")) {
                const json &s = wj["scrollbar"];
                if (s.contains("x")) w.scrollbar_x = s["x"].get<int>();
                if (s.contains("w")) w.scrollbar_w = s["w"].get<int>();
                if (!GetColor(s, "track", &w.scrollbar_track, &ctx)) return false;
                if (!GetColor(s, "thumb", &w.scrollbar_thumb, &ctx)) return false;
            }

            // --- validation --------------------------------------------------
            if (w.rect.w <= 0 || w.rect.h <= 0) {
                *err = "widget '" + (w.id.empty() ? type : w.id) + "' has a zero-size rect";
                return false;
            }
            if (w.rect.x < 0 || w.rect.y < 0 ||
                w.rect.x + w.rect.w > cfg.width || w.rect.y + w.rect.h > cfg.height) {
                *err = "widget '" + (w.id.empty() ? type : w.id) + "' rect [" +
                       std::to_string(w.rect.x) + "," + std::to_string(w.rect.y) + "," +
                       std::to_string(w.rect.w) + "," + std::to_string(w.rect.h) +
                       "] falls outside the " + std::to_string(cfg.width) + "x" +
                       std::to_string(cfg.height) + " canvas";
                return false;
            }
            if (!cfg.fonts.count(w.font)) {
                *err = "widget '" + (w.id.empty() ? type : w.id) + "' uses font '" +
                       w.font + "', which is not in \"fonts\"";
                return false;
            }
            if (w.type == WidgetType::kHero && !cfg.fonts.count(w.font_big)) {
                *err = "hero uses font_big '" + w.font_big + "', not in \"fonts\"";
                return false;
            }
            cfg.widgets.push_back(w);
        }
    } catch (const std::exception &e) {
        *err = std::string("bad value: ") + e.what();
        return false;
    }

    if (cfg.widgets.empty()) { *err = "\"widgets\" is empty"; return false; }

    for (const Widget &w : cfg.widgets) {
        for (auto [field, target] : {std::make_pair("flow_after", w.flow_after),
                                     std::make_pair("flow_before", w.flow_before)}) {
            if (target.empty()) continue;
            if (!cfg.FindById(target)) {
                *err = "widget '" + w.id + "' has " + field + " '" + target +
                       "', which is not the id of any widget";
                return false;
            }
            if (target == w.id) {
                *err = "widget '" + w.id + "' has " + std::string(field) +
                       " pointing at itself";
                return false;
            }
        }
    }

    // Cheap content hash, so an unchanged file does not trigger a rebuild and
    // reset every scroll position. FNV-1a over the raw text.
    uint64_t h = 1469598103934665603ull;
    for (unsigned char c : text) { h ^= c; h *= 1099511628211ull; }
    cfg.fingerprint = h;

    *out = std::move(cfg);
    return true;
}

}  // namespace led
