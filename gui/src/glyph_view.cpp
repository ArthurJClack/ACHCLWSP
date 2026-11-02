// The glyph editor: one letter big on the canvas with its metrics (baseline, x-height, cap
// height, ascender, descender, advance). Edit drags points (an on-curve point brings its handles
// along, like every font editor); Draw clicks corners of a new contour (or a hole). Each change
// is a type.draw command, so it undoes and Claude can do the same headless.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <sstream>

#include "shell.hpp"
// after imgui.h
#include "imgui_stdlib.h"

namespace wetwaresuite::gui {

namespace {

double numberField(const Result& r, const char* key, double fallback = 0) {
    const Value* v = r.field(key);
    if (auto d = v ? std::get_if<double>(v) : nullptr) return *d;
    if (auto i = v ? std::get_if<std::int64_t>(v) : nullptr) return static_cast<double>(*i);
    return fallback;
}

// "contour 0: 50,350 50,543.3c 161.929,700c 300,700 ..." lines of type.glyph_info points=true
std::vector<std::vector<GlyphPoint>> parseContours(const std::string& message) {
    std::vector<std::vector<GlyphPoint>> out;
    std::istringstream lines(message);
    std::string line;
    while (std::getline(lines, line)) {
        const std::size_t c = line.find("contour ");
        const std::size_t colon = line.find(':', c == std::string::npos ? 0 : c);
        if (c == std::string::npos || colon == std::string::npos) continue;
        std::istringstream pts(line.substr(colon + 1));
        std::string tok;
        std::vector<GlyphPoint> contour;
        while (pts >> tok) {
            GlyphPoint p;
            if (!tok.empty() && (tok.back() == 'c' || tok.back() == 'q')) p.type = tok.back(), tok.pop_back();
            if (std::sscanf(tok.c_str(), "%f,%f", &p.x, &p.y) == 2) contour.push_back(p);
        }
        if (!contour.empty()) out.push_back(std::move(contour));
    }
    return out;
}

std::string num(float v) {
    char buf[32];
    std::snprintf(buf, sizeof buf, "%g", std::round(v * 100) / 100);
    return buf;
}

// Back to SVG path data: on-curve points joined by lines, cubic (two c handles) or quadratic
// (q handles, with the implied on-curve midpoints TrueType uses between neighbours).
std::string toPath(const std::vector<std::vector<GlyphPoint>>& contours) {
    std::string d;
    for (const auto& raw : contours) {
        if (raw.empty()) continue;
        // start on an on-curve point
        std::size_t first = 0;
        while (first < raw.size() && raw[first].type != 'o') ++first;
        std::vector<GlyphPoint> c;
        if (first == raw.size()) {  // all quadratic controls: start at a midpoint
            const GlyphPoint a = raw.back(), b = raw[0];
            c.push_back({(a.x + b.x) / 2, (a.y + b.y) / 2, 'o'});
            c.insert(c.end(), raw.begin(), raw.end());
        } else {
            for (std::size_t i = 0; i < raw.size(); ++i) c.push_back(raw[(first + i) % raw.size()]);
        }
        d += (d.empty() ? "M" : " M") + num(c[0].x) + " " + num(c[0].y);
        std::size_t i = 0;
        const std::size_t n = c.size();
        while (i < n) {
            std::vector<GlyphPoint> ctrl;
            std::size_t j = i + 1;
            while (c[j % n].type != 'o' && j < i + n) ctrl.push_back(c[j % n]), ++j;
            const GlyphPoint end = c[j % n];
            if (ctrl.empty()) {
                d += " L" + num(end.x) + " " + num(end.y);
            } else if (ctrl[0].type == 'c' && ctrl.size() >= 2) {
                d += " C" + num(ctrl[0].x) + " " + num(ctrl[0].y) + " " + num(ctrl[1].x) + " " + num(ctrl[1].y) + " " + num(end.x) + " " + num(end.y);
            } else {
                for (std::size_t k = 0; k < ctrl.size(); ++k) {
                    const GlyphPoint to = k + 1 < ctrl.size() ? GlyphPoint{(ctrl[k].x + ctrl[k + 1].x) / 2, (ctrl[k].y + ctrl[k + 1].y) / 2, 'o'} : end;
                    d += " Q" + num(ctrl[k].x) + " " + num(ctrl[k].y) + " " + num(to.x) + " " + num(to.y);
                }
            }
            i = j;
        }
        d += "Z";
    }
    return d;
}

}  // namespace

bool Shell::fontDoc() const {
    auto doc = session_.workspace().get(currentDoc_);
    return doc && doc->kind() == "font" && find("type.glyph_info") && find("type.draw");
}

void Shell::glyphSync() {
    GlyphView& g = glyph_;
    if (g.doc != currentDoc_) {
        const std::string keep = g.glyph;
        g = GlyphView{};
        g.doc = currentDoc_;
        g.glyph = g.input = keep;
    }
    if (g.seen == changes_) return;
    g.seen = changes_;
    if (Result info = query("type.info", {{"doc", currentDoc_}}); info.ok) {
        g.ascender = static_cast<float>(numberField(info, "ascender", 800));
        g.descender = static_cast<float>(numberField(info, "descender", -200));
        g.xHeight = static_cast<float>(numberField(info, "x_height", 500));
        g.capHeight = static_cast<float>(numberField(info, "cap_height", 700));
    }
    Result r = query("type.glyph_info", {{"doc", currentDoc_}, {"glyph", g.glyph}, {"points", true}});
    g.exists = r.ok;
    g.contours = r.ok ? parseContours(r.message) : std::vector<std::vector<GlyphPoint>>{};
    if (r.ok) g.advance = static_cast<float>(numberField(r, "advance", 600));
}

void Shell::glyphSettings() {
    GlyphView& g = glyph_;
    barLabel("Glyph");
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 5);
    if (ImGui::InputText("##glyph", &g.input, ImGuiInputTextFlags_EnterReturnsTrue) || ImGui::IsItemDeactivatedAfterEdit()) {
        if (!g.input.empty()) {
            g.glyph = g.input;
            g.seen = ~0ull;
            g.drawing.clear();
        }
    }
    ImGui::SetItemTooltip("A letter (A, é), a glyph name (zero, uni00E9) or U+00E9");
    ImGui::SameLine();
    ImGui::TextDisabled("advance %.0f", g.advance);
    if (canvas_.tool == CanvasTool::GlyphDraw) {
        ImGui::SameLine();
        ImGui::Checkbox("Cut a hole", &g.cut);
        ImGui::SameLine();
        ImGui::TextDisabled("click corners, click the first again to close; Esc drops it");
    } else {
        ImGui::SameLine();
        ImGui::TextDisabled(g.exists ? "drag points (handles follow their point)" : "no such glyph yet: pick Draw to make it");
    }
}

void Shell::glyphCanvas(ImDrawList* dl, ImVec2 v0, ImVec2 size, bool hovered, bool active) {
    glyphSync();
    GlyphView& g = glyph_;
    ImGuiIO& io = ImGui::GetIO();
    // fit the em box (advance wide, descender..ascender tall) in the view
    const float top = std::max(g.ascender, g.capHeight) + 120, bottom = g.descender - 120;
    const float wide = std::max(g.advance, 200.0f) + 400;
    g.scale = std::min(size.x / wide, size.y / (top - bottom));
    g.ox = v0.x + (size.x - g.advance * g.scale) / 2;
    g.oy = v0.y + (size.y + (top + bottom) * g.scale) / 2;
    auto S = [&](float x, float y) { return ImVec2{g.ox + x * g.scale, g.oy - y * g.scale}; };
    auto F = [&](ImVec2 s) { return ImVec2{(s.x - g.ox) / g.scale, (g.oy - s.y) / g.scale}; };
    const bool dark = darkLook();
    dl->AddRectFilled(v0, {v0.x + size.x, v0.y + size.y}, dark ? IM_COL32(30, 32, 36, 255) : IM_COL32(250, 250, 250, 255));
    const ImU32 guide = dark ? IM_COL32(90, 95, 105, 255) : IM_COL32(190, 195, 205, 255);
    const ImU32 label = dark ? IM_COL32(140, 145, 155, 255) : IM_COL32(120, 125, 135, 255);
    const struct {
        float y;
        const char* name;
    } lines[] = {{0, "baseline"}, {g.xHeight, "x-height"}, {g.capHeight, "cap height"}, {g.ascender, "ascender"}, {g.descender, "descender"}};
    for (const auto& l : lines) {
        dl->AddLine({v0.x, S(0, l.y).y}, {v0.x + size.x, S(0, l.y).y}, guide, l.y == 0 ? 2.0f : 1.0f);
        dl->AddText({v0.x + 6, S(0, l.y).y - ImGui::GetFontSize() - 1}, label, l.name);
    }
    dl->AddLine({S(0, 0).x, v0.y}, {S(0, 0).x, v0.y + size.y}, guide);
    dl->AddLine({S(g.advance, 0).x, v0.y}, {S(g.advance, 0).x, v0.y + size.y}, guide);

    // the outline as it is (or as it will be while dragging)
    auto contours = g.contours;
    if (g.dragC >= 0) {
        const float dx = (g.dragNow.x - g.dragStart.x) / g.scale, dy = -(g.dragNow.y - g.dragStart.y) / g.scale;
        auto& c = contours[static_cast<std::size_t>(g.dragC)];
        const std::size_t n = c.size(), i = static_cast<std::size_t>(g.dragP);
        c[i].x += dx, c[i].y += dy;
        if (c[i].type == 'o') {  // its handles come along
            for (std::size_t nb : {(i + n - 1) % n, (i + 1) % n})
                if (c[nb].type != 'o' && nb != i) c[nb].x += dx, c[nb].y += dy;
        }
    }
    const ImU32 ink = dark ? IM_COL32(230, 232, 236, 255) : IM_COL32(25, 25, 30, 255);
    const ImU32 acc = ImGui::ColorConvertFloat4ToU32(accent_);
    for (const auto& c : contours) {
        const std::size_t n = c.size();
        if (n < 2) continue;
        std::size_t first = 0;
        while (first < n && c[first].type != 'o') ++first;
        if (first == n) first = 0;
        for (std::size_t k = 0; k < n;) {  // segment from an on-curve point to the next
            const std::size_t i = (first + k) % n;
            std::vector<GlyphPoint> ctrl;
            std::size_t j = k + 1;
            while (j < k + n && c[(first + j) % n].type != 'o') ctrl.push_back(c[(first + j) % n]), ++j;
            const GlyphPoint a = c[i], e = c[(first + j) % n];
            if (ctrl.empty()) {
                dl->AddLine(S(a.x, a.y), S(e.x, e.y), ink, 2);
            } else if (ctrl[0].type == 'c' && ctrl.size() >= 2) {
                dl->AddBezierCubic(S(a.x, a.y), S(ctrl[0].x, ctrl[0].y), S(ctrl[1].x, ctrl[1].y), S(e.x, e.y), ink, 2);
            } else {
                GlyphPoint from = a;
                for (std::size_t q = 0; q < ctrl.size(); ++q) {
                    const GlyphPoint to = q + 1 < ctrl.size() ? GlyphPoint{(ctrl[q].x + ctrl[q + 1].x) / 2, (ctrl[q].y + ctrl[q + 1].y) / 2, 'o'} : e;
                    dl->AddBezierQuadratic(S(from.x, from.y), S(ctrl[q].x, ctrl[q].y), S(to.x, to.y), ink, 2);
                    from = to;
                }
            }
            k = j;
        }
        if (canvas_.tool == CanvasTool::GlyphEdit) {
            for (std::size_t i = 0; i < n; ++i) {
                const ImVec2 p = S(c[i].x, c[i].y);
                if (c[i].type == 'o') {
                    dl->AddRectFilled({p.x - 4, p.y - 4}, {p.x + 4, p.y + 4}, IM_COL32(255, 255, 255, 255));
                    dl->AddRect({p.x - 4, p.y - 4}, {p.x + 4, p.y + 4}, acc, 0, 0, 1.5f);
                } else {  // handles hang off their on-curve neighbour
                    const GlyphPoint& prev = c[(i + n - 1) % n];
                    const GlyphPoint& next = c[(i + 1) % n];
                    const GlyphPoint& owner = prev.type == 'o' ? prev : next;
                    dl->AddLine(S(owner.x, owner.y), p, acc, 1);
                    dl->AddCircleFilled(p, 3.5f, acc);
                }
            }
        }
    }
    if (!g.exists && g.drawing.empty()) {
        const char* msg = "No outline yet. Pick Draw and click corners to make one.";
        const ImVec2 ts = ImGui::CalcTextSize(msg);
        dl->AddText({v0.x + (size.x - ts.x) / 2, v0.y + size.y * 0.4f}, label, msg);
    }

    const ImVec2 mouse = io.MousePos;
    if (canvas_.tool == CanvasTool::GlyphEdit) {
        if (hovered && ImGui::IsItemActivated() && ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
            float best = 9;
            g.dragC = g.dragP = -1;
            for (std::size_t ci = 0; ci < g.contours.size(); ++ci)
                for (std::size_t pi = 0; pi < g.contours[ci].size(); ++pi) {
                    const ImVec2 p = S(g.contours[ci][pi].x, g.contours[ci][pi].y);
                    const float d = std::hypot(p.x - mouse.x, p.y - mouse.y);
                    if (d < best) best = d, g.dragC = static_cast<int>(ci), g.dragP = static_cast<int>(pi);
                }
            g.dragStart = g.dragNow = mouse;
        }
        if (g.dragC >= 0 && active) g.dragNow = mouse;
        if (g.dragC >= 0 && !active) {
            const bool moved = std::hypot(g.dragNow.x - g.dragStart.x, g.dragNow.y - g.dragStart.y) > 1;
            if (moved) {
                Result r = run("type.draw", {{"doc", currentDoc_}, {"glyph", g.glyph}, {"path", toPath(contours)}, {"mode", std::string("replace")}});
                resultFor_ = "Move point";
                r.message = r.ok ? sentence(r.message) : friendlyError(r.message);
                result_ = std::move(r);
                canvas_.dirty = true;
                g.contours = contours;
            }
            g.dragC = g.dragP = -1;
        }
    } else {  // Draw: a polygon contour, closed on the first corner
        auto& pts = g.drawing;
        if (hovered && ImGui::IsItemClicked(ImGuiMouseButton_Left)) {
            if (pts.size() >= 3 && std::hypot(S(pts[0].x, pts[0].y).x - mouse.x, S(pts[0].x, pts[0].y).y - mouse.y) < 9) {
                NumberList flat;
                for (const auto& p : pts) flat.push_back(std::round(p.x)), flat.push_back(std::round(p.y));
                Result r = run("type.draw", {{"doc", currentDoc_}, {"glyph", g.glyph}, {"points", flat}, {"mode", std::string(g.cut ? "cut" : "add")}});
                resultFor_ = g.cut ? "Cut hole" : "Draw contour";
                r.message = r.ok ? sentence(r.message) : friendlyError(r.message);
                result_ = std::move(r);
                canvas_.dirty = true;
                pts.clear();
            } else {
                ImVec2 f = F(mouse);
                if (io.KeyShift && !pts.empty()) {  // straight across or up
                    if (std::abs(f.x - pts.back().x) > std::abs(f.y - pts.back().y)) f.y = pts.back().y;
                    else f.x = pts.back().x;
                }
                pts.push_back({std::round(f.x), std::round(f.y)});
            }
        }
        if (!io.WantTextInput && ImGui::IsKeyPressed(ImGuiKey_Escape)) pts.clear();
        if (!pts.empty()) {
            std::vector<ImVec2> sp;
            for (const auto& p : pts) sp.push_back(S(p.x, p.y));
            dl->AddPolyline(sp.data(), static_cast<int>(sp.size()), acc, ImDrawFlags_None, 2);
            if (hovered) dl->AddLine(sp.back(), mouse, acc, 1);
            for (const auto& p : sp) dl->AddRectFilled({p.x - 3, p.y - 3}, {p.x + 3, p.y + 3}, acc);
            if (sp.size() >= 3) dl->AddCircle(sp[0], 8, acc, 0, 1.5f);
        }
    }
}

}  // namespace wetwaresuite::gui
