// The vector tools on the canvas. Pick clicks a shape (vector.hit), drags it
// (vector.transform dx/dy, snapped by vector.snap) or one of the eight handles
// round its box (vector.transform scale_x/scale_y about the opposite side).
// Anchors shows the picked path's anchors and handles (vector.nodes) and drags
// them (vector.move_node); Alt+click on the path adds an anchor. Everything
// goes through commands, so it is undoable and the same as the CLI and MCP.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>

#include "shell.hpp"
#include "imgui_stdlib.h"

namespace wetwaresuite::gui {

namespace {

std::string fieldText(const Result& r, const char* key) {
    const Value* v = r.field(key);
    if (!v) return {};
    if (auto s = std::get_if<std::string>(v)) return *s;
    return toText(*v);
}

double fieldNum(const Result& r, const char* key) {
    const Value* v = r.field(key);
    if (!v) return 0;
    if (auto d = std::get_if<double>(v)) return *d;
    if (auto i = std::get_if<std::int64_t>(v)) return static_cast<double>(*i);
    return 0;
}

// "sub:x,y,ix,iy,ox,oy:type;..." from vector.nodes
std::vector<float> parseNodes(const std::string& text) {
    std::vector<float> out;
    std::size_t at = 0;
    while (at < text.size()) {
        std::size_t end = text.find(';', at);
        if (end == std::string::npos) end = text.size();
        const std::string item = text.substr(at, end - at);
        const std::size_t a = item.find(':'), b = item.rfind(':');
        if (a != std::string::npos && b != a) {
            float v[6];
            if (std::sscanf(item.substr(a + 1, b - a - 1).c_str(), "%f,%f,%f,%f,%f,%f", &v[0], &v[1], &v[2], &v[3], &v[4], &v[5]) == 6) {
                out.insert(out.end(), v, v + 6);
            }
        }
        at = end + 1;
    }
    return out;
}

// the eight scale handles: corners then edge middles, as fractions of the box
const float kHandle[8][2] = {{0, 0}, {1, 0}, {1, 1}, {0, 1}, {0.5f, 0}, {1, 0.5f}, {0.5f, 1}, {0, 0.5f}};

}  // namespace

bool Shell::vectorDoc() const {
    auto doc = session_.workspace().get(currentDoc_);
    return doc && doc->kind() == "vector" && find("vector.hit") != nullptr;
}

namespace {

// Drawing units per picture px: the canvas shows the drawing's preview, which may be smaller or larger.
float unitsPerPx(const std::string& summary, const Texture& t) {
    int w = 0, h = 0;
    if (t.srcWidth <= 0 || std::sscanf(summary.c_str(), "%dx%d", &w, &h) != 2 || w <= 0) return 1;
    return static_cast<float>(w) / static_cast<float>(t.srcWidth);
}

}  // namespace

void Shell::vectorSettings() {
    // reload the picked shape's box and anchors after any change (undo included)
    if (canvas_.vecLoaded != changes_ || canvas_.shownDoc != currentDoc_) {
        canvas_.vecLoaded = changes_;
        canvas_.vecHasBox = false;
        canvas_.vecNodes.clear();
        if (!canvas_.vecSel.empty()) {
            Result b = query("vector.bounds", {{"doc", currentDoc_}, {"shape", canvas_.vecSel}});
            if (b.ok) {
                canvas_.vecHasBox = true;
                canvas_.vecBox[0] = static_cast<float>(fieldNum(b, "x"));
                canvas_.vecBox[1] = static_cast<float>(fieldNum(b, "y"));
                canvas_.vecBox[2] = static_cast<float>(fieldNum(b, "width"));
                canvas_.vecBox[3] = static_cast<float>(fieldNum(b, "height"));
                if (canvas_.tool == CanvasTool::Anchor) {
                    Result n = query("vector.nodes", {{"doc", currentDoc_}, {"shape", canvas_.vecSel}});
                    if (n.ok) canvas_.vecNodes = parseNodes(fieldText(n, "nodes"));
                }
            } else {
                canvas_.vecSel.clear();
            }
        }
    }
    if (canvas_.vecSel.empty()) {
        ImGui::SameLine();
        ImGui::TextDisabled(canvas_.tool == CanvasTool::Anchor ? "  Click a path to edit its anchors" : "  Click a shape to pick it");
        return;
    }
    auto edit = [this](const char* id, Params p, const char* title) {
        p["doc"] = currentDoc_;
        p["shape"] = canvas_.vecSel;
        Result r = run(id, p);
        resultFor_ = title;
        if (r.ok && std::string(id) == "vector.duplicate") canvas_.vecSel = fieldText(r, "shape");
        if (r.ok && std::string(id) == "vector.delete") canvas_.vecSel.clear();
        r.message = r.ok ? sentence(r.message) : friendlyError(r.message);
        result_ = std::move(r);
    };
    ImGui::SameLine();
    ImGui::Text("  %s", canvas_.vecSel.c_str());
    const struct {
        const char* label;
        const char* key;
        const char* value;
        const char* tip;
    } aligns[] = {{"|<", "horizontal", "left", "Align left (to the canvas)"},     {"|", "horizontal", "center", "Centre across"},
                  {">|", "horizontal", "right", "Align right"},                   {"T", "vertical", "top", "Align top"},
                  {"-", "vertical", "middle", "Centre up and down"},             {"_", "vertical", "bottom", "Align bottom"}};
    for (const auto& a : aligns) {
        ImGui::SameLine();
        if (ImGui::SmallButton(a.label)) edit("vector.align", {{a.key, std::string(a.value)}}, "Align");
        ImGui::SetItemTooltip("%s", a.tip);
    }
    ImGui::SameLine();
    if (ImGui::SmallButton("Front")) edit("vector.reorder", {{"to", std::string("front")}}, "Arrange");
    ImGui::SameLine();
    if (ImGui::SmallButton("Back")) edit("vector.reorder", {{"to", std::string("back")}}, "Arrange");
    ImGui::SameLine();
    if (ImGui::SmallButton("Duplicate")) edit("vector.duplicate", {{"dx", 10.0}, {"dy", 10.0}}, "Duplicate");
    ImGui::SameLine();
    if (ImGui::SmallButton("Delete")) edit("vector.delete", {}, "Delete");
}

void Shell::vectorPress(ImVec2 screen) {
    const float u = unitsPerPx(canvas_.summary, canvas_.texture);
    const ImVec2 img = toImage(screen);
    const ImVec2 at{img.x * u, img.y * u};
    const float reach = 6 * u / std::max(canvas_.zoom, 0.01f);  // 6 screen px in drawing units
    canvas_.vecStart = canvas_.vecNow = at;
    canvas_.vecDrag = 0;
    if (canvas_.tool == CanvasTool::VecPen) {  // a corner, or back on the first one: a closed shape
        auto& pts = canvas_.penPoints;
        if (pts.size() >= 3 && std::hypot(pts[0].x - at.x, pts[0].y - at.y) < reach * 1.5f) {
            penFinish(true);
        } else if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left) && pts.size() >= 2) {
            penFinish(false);
        } else {
            pts.push_back(at);
            canvas_.penIn.push_back(at);
            canvas_.penOut.push_back(at);
            canvas_.vecDrag = 5;  // dragging now pulls out curve handles
        }
        return;
    }
    if (canvas_.tool == CanvasTool::VecText) {  // the words go here: see vectorTextBox
        canvas_.textOpen = true;
        canvas_.textAt = at;
        canvas_.textDraft.clear();
        return;
    }
    if (vectorDrawTool(canvas_.tool)) {  // drawing a new shape: see vectorRelease
        canvas_.vecDrag = 4;
        canvas_.builderPts = {at.x, at.y};
        return;
    }
    if (canvas_.tool == CanvasTool::Anchor && !canvas_.vecNodes.empty()) {
        const auto& n = canvas_.vecNodes;
        float best = reach;
        for (std::size_t i = 0; i + 5 < n.size(); i += 6) {
            for (int part = 0; part < 3; ++part) {
                const float d = std::hypot(n[i + part * 2] - at.x, n[i + part * 2 + 1] - at.y);
                if (d < best) {
                    best = d;
                    canvas_.vecDrag = 3;
                    canvas_.vecHandle = static_cast<int>(i / 6) * 3 + part;
                }
            }
        }
        if (canvas_.vecDrag) return;
        if (ImGui::GetIO().KeyAlt) {  // Alt+click on the path: a new anchor there
            Result r = run("vector.add_node", {{"doc", currentDoc_}, {"shape", canvas_.vecSel}, {"x", static_cast<double>(at.x)}, {"y", static_cast<double>(at.y)}});
            resultFor_ = "Add anchor";
            r.message = r.ok ? sentence(r.message) : friendlyError(r.message);
            result_ = std::move(r);
            return;
        }
    }
    if (canvas_.tool == CanvasTool::Pick && canvas_.vecHasBox) {
        const float* b = canvas_.vecBox;
        for (int h = 0; h < 8; ++h) {
            if (std::hypot(b[0] + b[2] * kHandle[h][0] - at.x, b[1] + b[3] * kHandle[h][1] - at.y) < reach) {
                canvas_.vecDrag = 2;
                canvas_.vecHandle = h;
                return;
            }
        }
    }
    Result hit = query("vector.hit", {{"doc", currentDoc_}, {"x", static_cast<double>(at.x)}, {"y", static_cast<double>(at.y)},
                                      {"tolerance", static_cast<double>(reach)}});
    const std::string picked = fieldText(hit, canvas_.tool == CanvasTool::Anchor ? "shape" : "select");
    if (picked != canvas_.vecSel) {
        canvas_.vecSel = picked;
        canvas_.vecLoaded = ~0ull;  // read its box and anchors
        // like a properties panel: the picked shape's style is ready to change
        if (!picked.empty() && canvas_.tool == CanvasTool::Pick) {
            if (!form_ && find("vector.style")) select("vector.style");
            else fillPicked(true);
        }
    }
    if (!picked.empty() && canvas_.tool == CanvasTool::Pick) {
        canvas_.vecDrag = 1;
        // smart guides: the other shapes' boxes to line up with
        canvas_.guideBoxes.clear();
        Result boxes = query("vector.boxes", {{"doc", currentDoc_}, {"except", picked}});
        const std::string text = fieldText(boxes, "boxes");
        std::size_t at0 = 0;
        while (at0 < text.size()) {
            std::size_t end = text.find('\n', at0);
            if (end == std::string::npos) end = text.size();
            char id[64];
            float v[4];
            if (std::sscanf(text.substr(at0, end - at0).c_str(), "%63s %f %f %f %f", id, &v[0], &v[1], &v[2], &v[3]) == 5)
                canvas_.guideBoxes.insert(canvas_.guideBoxes.end(), v, v + 4);
            at0 = end + 1;
        }
        int cw = 0, ch = 0;  // the canvas edges and middle count too
        if (std::sscanf(canvas_.summary.c_str(), "%dx%d", &cw, &ch) == 2) {
            const float c[4] = {0, 0, static_cast<float>(cw), static_cast<float>(ch)};
            canvas_.guideBoxes.insert(canvas_.guideBoxes.end(), c, c + 4);
        }
    }
}

// Figma-style smart guides: the dragged box's left/centre/right (top/middle/bottom) snap to the
// same lines of the other shapes within 6 screen px; the lines it snapped to are drawn pink.
void Shell::vectorGuides(float& dx, float& dy) {
    canvas_.guideX = canvas_.guideY = NAN;
    if (!canvas_.vecHasBox || ImGui::GetIO().KeyCtrl) return;  // Ctrl: no snapping
    const float u = unitsPerPx(canvas_.summary, canvas_.texture);
    const float reach = 6 * u / std::max(canvas_.zoom, 0.01f);
    const float* b = canvas_.vecBox;
    const float mine[2][3] = {{b[0] + dx, b[0] + dx + b[2] / 2, b[0] + dx + b[2]}, {b[1] + dy, b[1] + dy + b[3] / 2, b[1] + dy + b[3]}};
    float best[2] = {reach, reach}, shift[2] = {0, 0}, line[2] = {NAN, NAN};
    const auto& g = canvas_.guideBoxes;
    for (std::size_t i = 0; i + 3 < g.size(); i += 4) {
        const float theirs[2][3] = {{g[i], g[i] + g[i + 2] / 2, g[i] + g[i + 2]}, {g[i + 1], g[i + 1] + g[i + 3] / 2, g[i + 1] + g[i + 3]}};
        for (int axis = 0; axis < 2; ++axis)
            for (float m : mine[axis])
                for (float t : theirs[axis])
                    if (std::abs(t - m) < best[axis]) best[axis] = std::abs(t - m), shift[axis] = t - m, line[axis] = t;
    }
    dx += shift[0], dy += shift[1];
    canvas_.guideX = line[0], canvas_.guideY = line[1];
}

void Shell::vectorDrag(ImVec2 screen) {
    const float u = unitsPerPx(canvas_.summary, canvas_.texture);
    const ImVec2 img = toImage(screen);
    canvas_.vecNow = {img.x * u, img.y * u};
    if (canvas_.vecDrag == 5 && !canvas_.penPoints.empty()) {  // Illustrator's pen: drag pulls a smooth pair, Alt just this side
        const ImVec2 a = canvas_.penPoints.back();
        canvas_.penOut.back() = canvas_.vecNow;
        if (!ImGui::GetIO().KeyAlt) canvas_.penIn.back() = {2 * a.x - canvas_.vecNow.x, 2 * a.y - canvas_.vecNow.y};
        return;
    }
    if (canvas_.vecDrag == 4 && canvas_.tool == CanvasTool::VecBuilder) {  // the regions the drag crosses
        auto& b = canvas_.builderPts;
        if (b.size() < 2 || std::hypot(b[b.size() - 2] - canvas_.vecNow.x, b.back() - canvas_.vecNow.y) > 4 * u / std::max(canvas_.zoom, 0.01f)) {
            b.push_back(canvas_.vecNow.x);
            b.push_back(canvas_.vecNow.y);
        }
        return;
    }
    if (canvas_.vecDrag == 1 && canvas_.vecHasBox) {
        float sdx = canvas_.vecNow.x - canvas_.vecStart.x, sdy = canvas_.vecNow.y - canvas_.vecStart.y;
        vectorGuides(sdx, sdy);
        if (!std::isnan(canvas_.guideX) || !std::isnan(canvas_.guideY)) {
            canvas_.vecNow = {canvas_.vecStart.x + sdx, canvas_.vecStart.y + sdy};
            return;
        }
    }
    if (canvas_.vecDrag == 1 && canvas_.vecHasBox) {  // no shape to line up with: snap the corner to the grid, guides or anchors
        const float dx = canvas_.vecNow.x - canvas_.vecStart.x, dy = canvas_.vecNow.y - canvas_.vecStart.y;
        Result s = query("vector.snap", {{"doc", currentDoc_}, {"x", static_cast<double>(canvas_.vecBox[0] + dx)},
                                         {"y", static_cast<double>(canvas_.vecBox[1] + dy)}, {"radius", static_cast<double>(8 * u / canvas_.zoom)}});
        if (s.ok && fieldText(s, "snapped") != "none") {
            canvas_.vecNow.x = static_cast<float>(fieldNum(s, "x")) - canvas_.vecBox[0] + canvas_.vecStart.x;
            canvas_.vecNow.y = static_cast<float>(fieldNum(s, "y")) - canvas_.vecBox[1] + canvas_.vecStart.y;
        }
    }
}

// the shape being drawn, before the command runs
void Shell::drawVectorDraft(ImDrawList* dl, ImVec2 p0) const {
    const float u = unitsPerPx(canvas_.summary, canvas_.texture);
    auto screen = [&](ImVec2 at) { return ImVec2{p0.x + at.x / u * canvas_.zoom, p0.y + at.y / u * canvas_.zoom}; };
    if (canvas_.tool == CanvasTool::VecPen && !canvas_.penPoints.empty()) {
        const ImU32 col = ImGui::ColorConvertFloat4ToU32(accent_);
        const auto& a = canvas_.penPoints;
        for (std::size_t i = 0; i + 1 < a.size(); ++i)
            dl->AddBezierCubic(screen(a[i]), screen(canvas_.penOut[i]), screen(canvas_.penIn[i + 1]), screen(a[i + 1]), col, 2);
        if (canvas_.vecDrag != 5) dl->AddLine(screen(a.back()), ImGui::GetIO().MousePos, col, 1);
        for (std::size_t i = 0; i < a.size(); ++i) {
            const ImVec2 q = screen(a[i]);
            dl->AddRectFilled({q.x - 3, q.y - 3}, {q.x + 3, q.y + 3}, col);
            if (i + 1 == a.size() || i + 2 == a.size()) {  // the handles of the newest anchors
                for (const ImVec2& h : {canvas_.penIn[i], canvas_.penOut[i]}) {
                    if (std::abs(h.x - a[i].x) < 1e-4f && std::abs(h.y - a[i].y) < 1e-4f) continue;
                    dl->AddLine(q, screen(h), col, 1);
                    dl->AddCircleFilled(screen(h), 3.5f, col);
                }
            }
        }
        if (a.size() >= 3) dl->AddCircle(screen(a[0]), 7, col, 0, 1.5f);  // click here to close
        return;
    }
    if (canvas_.vecDrag == 4 && canvas_.tool == CanvasTool::VecGradient) {
        const ImVec2 a = screen(canvas_.vecStart), b = screen(canvas_.vecNow);
        dl->AddLine(a, b, IM_COL32(255, 255, 255, 255), 3);
        dl->AddLine(a, b, ImGui::ColorConvertFloat4ToU32(accent_), 1.5f);
        dl->AddCircleFilled(a, 5, ImGui::ColorConvertFloat4ToU32({canvas_.color[0], canvas_.color[1], canvas_.color[2], 1}));
        dl->AddCircleFilled(b, 5, ImGui::ColorConvertFloat4ToU32({canvas_.color2[0], canvas_.color2[1], canvas_.color2[2], 1}));
        return;
    }
    if (canvas_.vecDrag == 4 && canvas_.tool == CanvasTool::VecBuilder) {
        std::vector<ImVec2> pts;
        for (std::size_t i = 0; i + 1 < canvas_.builderPts.size(); i += 2)
            pts.push_back(screen({static_cast<float>(canvas_.builderPts[i]), static_cast<float>(canvas_.builderPts[i + 1])}));
        const ImU32 col = ImGui::GetIO().KeyAlt ? IM_COL32(220, 60, 60, 220) : ImGui::ColorConvertFloat4ToU32(accent_);
        if (pts.size() >= 2) dl->AddPolyline(pts.data(), static_cast<int>(pts.size()), col, ImDrawFlags_None, 3);
        return;
    }
    if (canvas_.vecDrag != 4) return;
    const ImVec2 a = screen(canvas_.vecStart), b = screen(vectorDrawEnd());
    const ImU32 col = ImGui::ColorConvertFloat4ToU32(accent_);
    if (canvas_.tool == CanvasTool::VecLine) {
        dl->AddLine(a, b, col, 2);
    } else if (canvas_.tool == CanvasTool::VecEllipse) {
        dl->AddEllipse({(a.x + b.x) / 2, (a.y + b.y) / 2}, {std::abs(b.x - a.x) / 2, std::abs(b.y - a.y) / 2}, col, 0, 0, 2);
    } else {
        dl->AddRect({std::min(a.x, b.x), std::min(a.y, b.y)}, {std::max(a.x, b.x), std::max(a.y, b.y)}, col, 0, 0, 2);
    }
}

ImVec2 Shell::vectorDrawEnd() const {
    ImVec2 end = canvas_.vecNow;
    if (ImGui::GetIO().KeyShift && canvas_.tool != CanvasTool::VecLine) {  // a square or a circle
        const float dx = end.x - canvas_.vecStart.x, dy = end.y - canvas_.vecStart.y;
        const float side = std::max(std::abs(dx), std::abs(dy));
        end = {canvas_.vecStart.x + (dx < 0 ? -side : side), canvas_.vecStart.y + (dy < 0 ? -side : side)};
    }
    return end;
}

void Shell::vectorDrawRelease() {
    if (canvas_.tool == CanvasTool::VecGradient) {  // the drag is the gradient's direction, on the shape under its start
        const ImVec2 a = canvas_.vecStart, b = canvas_.vecNow;
        if (std::hypot(b.x - a.x, b.y - a.y) < 1) return;
        std::string shape = canvas_.vecSel;
        Result hit = query("vector.hit", {{"doc", currentDoc_}, {"x", static_cast<double>(a.x)}, {"y", static_cast<double>(a.y)}, {"tolerance", 2.0}});
        if (const std::string under = fieldText(hit, "select"); !under.empty()) shape = under;
        if (shape.empty()) {
            resultFor_ = "Gradient";
            result_ = Result::failure("Start the drag on a shape to fill it with the gradient.");
            return;
        }
        const double angle = std::atan2(b.y - a.y, b.x - a.x) * 180.0 / 3.14159265358979;
        const std::string c1 = toHex({canvas_.color[0], canvas_.color[1], canvas_.color[2], canvas_.color[3]});
        const std::string c2 = toHex({canvas_.color2[0], canvas_.color2[1], canvas_.color2[2], canvas_.color2[3]});
        Params p{{"doc", currentDoc_}, {"shape", shape}, {"colors", c1 + "," + c2}};
        if (ImGui::GetIO().KeyAlt) {  // Alt: radial, centred where the drag started
            p["type"] = std::string("radial");
        } else {
            p["angle"] = angle;
        }
        Result r = run("vector.gradient", p);
        resultFor_ = "Gradient";
        canvas_.vecSel = shape;
        canvas_.vecLoaded = ~0ull;
        r.message = r.ok ? sentence(r.message) : friendlyError(r.message);
        result_ = std::move(r);
        return;
    }
    if (canvas_.tool == CanvasTool::VecBuilder) {  // drag across overlapping shapes: merge the regions crossed (Alt deletes them)
        auto pts = std::move(canvas_.builderPts);
        canvas_.builderPts.clear();
        if (pts.size() < 2) return;
        double x0 = pts[0], y0 = pts[1], x1 = pts[0], y1 = pts[1];
        for (std::size_t i = 0; i + 1 < pts.size(); i += 2) x0 = std::min(x0, pts[i]), x1 = std::max(x1, pts[i]), y0 = std::min(y0, pts[i + 1]), y1 = std::max(y1, pts[i + 1]);
        Result area = query("vector.select_area", {{"doc", currentDoc_}, {"x", x0}, {"y", y0}, {"width", std::max(1.0, x1 - x0)}, {"height", std::max(1.0, y1 - y0)}, {"touch", true}});
        const std::string shapes = fieldText(area, "shapes");
        if (shapes.empty()) return;
        Params p{{"doc", currentDoc_}, {"shapes", shapes}};
        p[ImGui::GetIO().KeyAlt ? "erase" : "merge"] = NumberList(pts.begin(), pts.end());
        Result r = run("vector.shape_builder", p);
        resultFor_ = "Shape builder";
        canvas_.vecSel.clear();
        canvas_.vecLoaded = ~0ull;
        r.message = r.ok ? sentence(r.message) : friendlyError(r.message);
        result_ = std::move(r);
        return;
    }
    const ImVec2 a = canvas_.vecStart, b = vectorDrawEnd();
    const double x0 = std::min(a.x, b.x), y0 = std::min(a.y, b.y), w = std::abs(b.x - a.x), h = std::abs(b.y - a.y);
    if (w < 1 && h < 1) return;  // a click, not a drag
    const std::string colour = toHex({canvas_.color[0], canvas_.color[1], canvas_.color[2], canvas_.color[3]});
    Params p{{"doc", currentDoc_}};
    std::string id, title;
    if (canvas_.tool == CanvasTool::VecLine) {
        id = "vector.add_line", title = "Line";
        p["x1"] = static_cast<double>(a.x), p["y1"] = static_cast<double>(a.y);
        p["x2"] = static_cast<double>(b.x), p["y2"] = static_cast<double>(b.y);
        p["stroke"] = colour;
    } else if (canvas_.tool == CanvasTool::VecEllipse) {
        if (w < 1 || h < 1) return;
        id = "vector.add_ellipse", title = "Ellipse";
        p["cx"] = x0 + w / 2, p["cy"] = y0 + h / 2, p["rx"] = w / 2, p["ry"] = h / 2;
        p["fill"] = colour;
    } else {
        if (w < 1 || h < 1) return;
        id = "vector.add_rect", title = "Rectangle";
        p["x"] = x0, p["y"] = y0, p["width"] = w, p["height"] = h;
        p["fill"] = colour;
    }
    Result r = run(id, p);
    resultFor_ = title;
    if (r.ok) canvas_.vecSel = fieldText(r, "shape");  // the new shape is picked, ready to adjust
    canvas_.vecLoaded = ~0ull;
    r.message = r.ok ? sentence(r.message) : friendlyError(r.message);
    result_ = std::move(r);
}

void Shell::vectorRelease() {
    const int drag = canvas_.vecDrag;
    canvas_.vecDrag = 0;
    canvas_.guideX = canvas_.guideY = NAN;
    if (drag == 5) return;  // a pen anchor (and its handles) is down; the path is made at the end
    if (drag == 4) {
        vectorDrawRelease();
        return;
    }
    const float dx = canvas_.vecNow.x - canvas_.vecStart.x, dy = canvas_.vecNow.y - canvas_.vecStart.y;
    if (!drag || canvas_.vecSel.empty() || (std::abs(dx) < 1e-3f && std::abs(dy) < 1e-3f)) return;
    Params p{{"doc", currentDoc_}};
    std::string id, title;
    if (drag == 1) {
        id = "vector.transform", title = "Move";
        p["shape"] = canvas_.vecSel;
        p["dx"] = static_cast<double>(dx);
        p["dy"] = static_cast<double>(dy);
    } else if (drag == 2) {
        const float* b = canvas_.vecBox;
        const float hx = kHandle[canvas_.vecHandle][0], hy = kHandle[canvas_.vecHandle][1];
        const float ox = b[0] + b[2] * (1 - hx), oy = b[1] + b[3] * (1 - hy);  // the opposite side stays put
        double sx = hx == 0.5f || b[2] <= 0 ? 1.0 : (b[0] + b[2] * hx + dx - ox) / (b[0] + b[2] * hx - ox);
        double sy = hy == 0.5f || b[3] <= 0 ? 1.0 : (b[1] + b[3] * hy + dy - oy) / (b[1] + b[3] * hy - oy);
        if (ImGui::GetIO().KeyShift && hx != 0.5f && hy != 0.5f) sx = sy = std::max(std::abs(sx), std::abs(sy));
        if (std::abs(sx) < 1e-3 || std::abs(sy) < 1e-3) return;
        id = "vector.transform", title = "Scale";
        p["shapes"] = canvas_.vecSel;
        p["scale_x"] = sx;
        p["scale_y"] = sy;
        p["origin_x"] = static_cast<double>(ox);
        p["origin_y"] = static_cast<double>(oy);
    } else {
        const int node = canvas_.vecHandle / 3, part = canvas_.vecHandle % 3;
        id = "vector.move_node", title = "Move anchor";
        p["shape"] = canvas_.vecSel;
        p["node"] = std::to_string(node);
        p["part"] = std::string(part == 0 ? "anchor" : part == 1 ? "in" : "out");
        p["dx"] = static_cast<double>(dx);
        p["dy"] = static_cast<double>(dy);
        if (ImGui::GetIO().KeyAlt) p["corner"] = true;  // Alt breaks a smooth pair
    }
    Result r = run(id, p);
    resultFor_ = title;
    r.message = r.ok ? sentence(r.message) : friendlyError(r.message);
    result_ = std::move(r);
}

void Shell::penFinish(bool closed) {
    auto pts = std::move(canvas_.penPoints);
    auto in = std::move(canvas_.penIn), out = std::move(canvas_.penOut);
    canvas_.penPoints.clear();
    canvas_.penIn.clear();
    canvas_.penOut.clear();
    if (pts.size() < 2 || in.size() != pts.size() || out.size() != pts.size()) return;
    std::string d;
    char buf[160];
    auto same = [](ImVec2 a, ImVec2 b) { return std::abs(a.x - b.x) < 1e-4f && std::abs(a.y - b.y) < 1e-4f; };
    auto segment = [&](std::size_t i, std::size_t j) {  // straight when neither end has a handle, else a cubic
        if (same(out[i], pts[i]) && same(in[j], pts[j])) std::snprintf(buf, sizeof buf, "L%.2f %.2f ", pts[j].x, pts[j].y);
        else std::snprintf(buf, sizeof buf, "C%.2f %.2f %.2f %.2f %.2f %.2f ", out[i].x, out[i].y, in[j].x, in[j].y, pts[j].x, pts[j].y);
        d += buf;
    };
    std::snprintf(buf, sizeof buf, "M%.2f %.2f ", pts[0].x, pts[0].y);
    d += buf;
    for (std::size_t i = 0; i + 1 < pts.size(); ++i) segment(i, i + 1);
    if (closed) {
        segment(pts.size() - 1, 0);
        d += "Z";
    }
    const std::string colour = toHex({canvas_.color[0], canvas_.color[1], canvas_.color[2], canvas_.color[3]});
    Params p{{"doc", currentDoc_}, {"d", d}};
    if (closed) {
        p["fill"] = colour;
    } else {
        p["fill"] = std::string("none");
        p["stroke"] = colour;
        p["stroke_width"] = 3.0;
    }
    Result r = run("vector.add_path", p);
    resultFor_ = "Pen";
    if (r.ok) canvas_.vecSel = fieldText(r, "shape");
    canvas_.vecLoaded = ~0ull;
    r.message = r.ok ? sentence(r.message) : friendlyError(r.message);
    result_ = std::move(r);
}

void Shell::vectorPenKeys() {
    if (canvas_.tool != CanvasTool::VecPen || canvas_.penPoints.empty() || ImGui::GetIO().WantTextInput) return;
    if (ImGui::IsKeyPressed(ImGuiKey_Enter) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter)) penFinish(false);
    else if (ImGui::IsKeyPressed(ImGuiKey_Escape)) canvas_.penPoints.clear(), canvas_.penIn.clear(), canvas_.penOut.clear();
    else if (ImGui::IsKeyPressed(ImGuiKey_Backspace) || ImGui::IsKeyPressed(ImGuiKey_Delete))
        canvas_.penPoints.pop_back(), canvas_.penIn.pop_back(), canvas_.penOut.pop_back();
}

// Text tool: a small box where you clicked; Enter puts the words on the drawing
void Shell::vectorTextBox(ImVec2 p0) {
    if (canvas_.tool != CanvasTool::VecText) canvas_.textOpen = false;
    if (!canvas_.textOpen) return;
    const float u = unitsPerPx(canvas_.summary, canvas_.texture);
    const ImVec2 at{p0.x + canvas_.textAt.x / u * canvas_.zoom, p0.y + canvas_.textAt.y / u * canvas_.zoom};
    ImGui::SetNextWindowPos(at, ImGuiCond_Appearing);
    ImGui::SetNextWindowSize({ImGui::GetFontSize() * 18, 0}, ImGuiCond_Appearing);
    bool open = true;
    if (ImGui::Begin("Text###vectortext", &open, ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings)) {
        if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
        ImGui::SetNextItemWidth(-1);
        const bool enter = ImGui::InputTextWithHint("##words", "type, then Enter", &canvas_.textDraft, ImGuiInputTextFlags_EnterReturnsTrue);
        if (ImGui::IsKeyPressed(ImGuiKey_Escape)) open = false;
        if (enter && !canvas_.textDraft.empty()) {
            // a size that reads well on this drawing: a twelfth of its height
            const double size = std::clamp(static_cast<double>(canvas_.texture.srcHeight) * u / 12.0, 6.0, 200.0);
            Result r = run("vector.add_text", {{"doc", currentDoc_},
                                               {"text", canvas_.textDraft},
                                               {"x", static_cast<double>(canvas_.textAt.x)},
                                               {"y", static_cast<double>(canvas_.textAt.y) + size * 0.8},
                                               {"size", size},
                                               {"fill", toHex({canvas_.color[0], canvas_.color[1], canvas_.color[2], canvas_.color[3]})}});
            resultFor_ = "Text";
            if (r.ok) canvas_.vecSel = fieldText(r, "shape");
            canvas_.vecLoaded = ~0ull;
            r.message = r.ok ? sentence(r.message) : friendlyError(r.message);
            result_ = std::move(r);
            open = false;
        }
    }
    ImGui::End();
    if (!open) canvas_.textOpen = false;
}

void Shell::vectorKeys() {
    if (canvas_.vecSel.empty() || ImGui::GetIO().WantTextInput) return;
    if (ImGui::IsKeyPressed(ImGuiKey_Delete) || ImGui::IsKeyPressed(ImGuiKey_Backspace)) {
        Result r = run("vector.delete", {{"doc", currentDoc_}, {"shape", canvas_.vecSel}});
        canvas_.vecSel.clear();
        resultFor_ = "Delete";
        r.message = r.ok ? sentence(r.message) : friendlyError(r.message);
        result_ = std::move(r);
        return;
    }
    const struct {
        ImGuiKey key;
        double dx, dy;
    } nudges[] = {{ImGuiKey_LeftArrow, -1, 0}, {ImGuiKey_RightArrow, 1, 0}, {ImGuiKey_UpArrow, 0, -1}, {ImGuiKey_DownArrow, 0, 1}};
    for (const auto& n : nudges) {
        if (!ImGui::IsKeyPressed(n.key)) continue;
        const double step = ImGui::GetIO().KeyShift ? 10 : 1;
        Result r = run("vector.transform", {{"doc", currentDoc_}, {"shape", canvas_.vecSel}, {"dx", n.dx * step}, {"dy", n.dy * step}});
        if (!r.ok) {
            resultFor_ = "Nudge";
            r.message = friendlyError(r.message);
            result_ = std::move(r);
        }
    }
}

void Shell::drawVector(ImDrawList* dl, ImVec2 origin) {
    if (canvas_.vecSel.empty() || !canvas_.vecHasBox) return;
    const float u = unitsPerPx(canvas_.summary, canvas_.texture);
    const float k = canvas_.zoom / u;  // screen px per drawing unit
    auto screen = [&](float x, float y) { return ImVec2{origin.x + x * k, origin.y + y * k}; };
    const ImU32 accent = ImGui::ColorConvertFloat4ToU32(accent_);
    float b[4] = {canvas_.vecBox[0], canvas_.vecBox[1], canvas_.vecBox[2], canvas_.vecBox[3]};
    const float dx = canvas_.vecNow.x - canvas_.vecStart.x, dy = canvas_.vecNow.y - canvas_.vecStart.y;
    if (canvas_.vecDrag == 1) b[0] += dx, b[1] += dy;
    if (canvas_.vecDrag == 2) {  // the box as it will be
        const float hx = kHandle[canvas_.vecHandle][0], hy = kHandle[canvas_.vecHandle][1];
        if (hx == 0) b[0] += dx, b[2] -= dx;
        if (hx == 1) b[2] += dx;
        if (hy == 0) b[1] += dy, b[3] -= dy;
        if (hy == 1) b[3] += dy;
    }
    dl->AddRect(screen(b[0], b[1]), screen(b[0] + b[2], b[1] + b[3]), accent, 0, 0, 1.5f);
    if (canvas_.vecDrag == 1) {  // smart guides: pink lines across the canvas where it lined up
        const ImU32 pink = IM_COL32(255, 60, 170, 255);
        const ImVec2 lo = canvas_.viewMin, hi{canvas_.viewMin.x + canvas_.viewSize.x, canvas_.viewMin.y + canvas_.viewSize.y};
        if (!std::isnan(canvas_.guideX)) dl->AddLine({screen(canvas_.guideX, 0).x, lo.y}, {screen(canvas_.guideX, 0).x, hi.y}, pink, 1);
        if (!std::isnan(canvas_.guideY)) dl->AddLine({lo.x, screen(0, canvas_.guideY).y}, {hi.x, screen(0, canvas_.guideY).y}, pink, 1);
    }
    if (canvas_.tool == CanvasTool::Pick) {
        for (const auto& h : kHandle) {
            const ImVec2 c = screen(b[0] + b[2] * h[0], b[1] + b[3] * h[1]);
            dl->AddRectFilled({c.x - 4, c.y - 4}, {c.x + 4, c.y + 4}, IM_COL32(255, 255, 255, 255));
            dl->AddRect({c.x - 4, c.y - 4}, {c.x + 4, c.y + 4}, accent);
        }
        return;
    }
    const auto& n = canvas_.vecNodes;
    for (std::size_t i = 0; i + 5 < n.size(); i += 6) {
        float v[6];
        std::copy(n.begin() + static_cast<std::ptrdiff_t>(i), n.begin() + static_cast<std::ptrdiff_t>(i) + 6, v);
        if (canvas_.vecDrag == 3 && static_cast<int>(i / 6) == canvas_.vecHandle / 3) {
            const int part = canvas_.vecHandle % 3;
            if (part == 0) {
                for (int q = 0; q < 3; ++q) v[q * 2] += dx, v[q * 2 + 1] += dy;
            } else {
                v[part * 2] += dx, v[part * 2 + 1] += dy;
            }
        }
        const ImVec2 a = screen(v[0], v[1]);
        for (int part = 1; part < 3; ++part) {
            if (std::abs(v[part * 2] - v[0]) < 1e-4f && std::abs(v[part * 2 + 1] - v[1]) < 1e-4f) continue;
            const ImVec2 hnd = screen(v[part * 2], v[part * 2 + 1]);
            dl->AddLine(a, hnd, accent, 1.0f);
            dl->AddCircleFilled(hnd, 3.5f, accent);
        }
        dl->AddRectFilled({a.x - 3.5f, a.y - 3.5f}, {a.x + 3.5f, a.y + 3.5f}, IM_COL32(255, 255, 255, 255));
        dl->AddRect({a.x - 3.5f, a.y - 3.5f}, {a.x + 3.5f, a.y + 3.5f}, accent, 0, 0, 1.5f);
    }
}

}  // namespace wetwaresuite::gui
