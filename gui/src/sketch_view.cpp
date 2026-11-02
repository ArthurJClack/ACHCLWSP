// Sketching a cad part with the mouse, as in Onshape or Fusion: while a sketch is open the view
// looks straight at it, and Line, Rectangle and Circle draw on a snapping grid. Points snap to
// the grid and to the sketch's own points (so lines join up), with the length or size shown
// while you draw. Each shape is one cad.sketch_* command, so it undoes and Claude sees it.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <tuple>

#include "shell.hpp"

namespace wetwaresuite::gui {

namespace {

NumberList listField(const Result& r, const char* key) {
    const Value* v = r.field(key);
    auto l = v ? std::get_if<NumberList>(v) : nullptr;
    return l ? *l : NumberList{};
}

std::string textOf(const Result& r, const char* key) {
    const Value* v = r.field(key);
    auto s = v ? std::get_if<std::string>(v) : nullptr;
    return s ? *s : std::string();
}

std::string num(double v) {
    char b[32];
    std::snprintf(b, sizeof b, "%.6g", std::fabs(v) < 1e-9 ? 0.0 : v);
    return b;
}

struct SketchToolDef {
    const char* name;
    SketchEdit::Tool tool;
    ImGuiKey key;
    const char* letter;
    const char* tip;
};
const SketchToolDef kSketchTools[] = {
    {"Select", SketchEdit::Tool::Select, ImGuiKey_S, "S", "Drag a point to move it: the sketch follows its relations and dimensions. Double-click a dimension to change it"},
    {"Line", SketchEdit::Tool::Line, ImGuiKey_L, "L", "Click, click, click... to draw joined lines. Click the first point again, right-click or Esc to stop"},
    {"Rectangle", SketchEdit::Tool::Rect, ImGuiKey_R, "R", "Drag from one corner to the other (or click both corners)"},
    {"Circle", SketchEdit::Tool::Circle, ImGuiKey_C, "C", "Drag from the centre out to the size (or click the centre, then the edge)"},
    {"Dimension", SketchEdit::Tool::Dimension, ImGuiKey_D, "D", "Click a line for its length, a circle for its diameter, or two points for the distance between them, then type the value"}};

std::vector<std::string> splitOn(const std::string& s, char sep) {
    std::vector<std::string> out;
    std::string item;
    for (char c : s) {
        if (c == sep) {
            out.push_back(item);
            item.clear();
        } else {
            item += c;
        }
    }
    if (!s.empty()) out.push_back(item);
    return out;
}

float segDist(ImVec2 p, ImVec2 a, ImVec2 b) {
    const float dx = b.x - a.x, dy = b.y - a.y, l2 = dx * dx + dy * dy;
    const float t = l2 > 0 ? std::clamp(((p.x - a.x) * dx + (p.y - a.y) * dy) / l2, 0.0f, 1.0f) : 0.0f;
    const float ex = a.x + dx * t - p.x, ey = a.y + dy * t - p.y;
    return std::sqrt(ex * ex + ey * ey);
}

bool plainNumber(const std::string& s) {
    char* end = nullptr;
    std::strtod(s.c_str(), &end);
    return !s.empty() && end && *end == 0;
}

}  // namespace

bool Shell::sketchActive() const { return sketch_.active && sketch_.doc == currentDoc_ && find("cad.sketch_geometry"); }

void Shell::enterSketch(const std::string& doc, const std::string& name) {
    if (!find("cad.sketch_geometry")) return;
    sketch_ = SketchEdit{};
    sketch_.doc = doc;
    sketch_.sketch = name;
    sketch_.active = true;
    sketch_.fit = true;
}

void Shell::leaveSketch() {
    sketch_.active = false;
    sketch_.drawing = false;
    canvas_.orbitDirty = true;
}

void Shell::refreshSketch() {
    SketchEdit& k = sketch_;
    if (k.seen == changes_) return;
    k.seen = changes_;
    Params p{{"doc", k.doc}};
    if (!k.sketch.empty()) p["sketch"] = k.sketch;
    Result r = query("cad.sketch_geometry", p);
    if (!r.ok) {  // the sketch was undone away
        leaveSketch();
        return;
    }
    k.sketch = textOf(r, "sketch");
    k.status = textOf(r, "status");
    k.units = textOf(r, "units");
    k.lines = listField(r, "lines");
    k.construction = listField(r, "construction");
    k.points = listField(r, "points");
    k.free = listField(r, "free");
    k.profiles = listField(r, "profiles");
    k.pointIds = splitOn(textOf(r, "point_ids"), ',');
    k.pointXy = listField(r, "point_xy");
    k.entities.clear();
    for (const auto& e : splitOn(textOf(r, "entities"), ',')) {
        const auto colon = e.find(':');
        k.entities.emplace_back(e.substr(0, colon), colon == std::string::npos ? std::string() : e.substr(colon + 1));
    }
    k.outlines.clear();
    {
        const NumberList o = listField(r, "entity_outlines");
        for (std::size_t i = 0; i < o.size();) {
            const std::size_t n = static_cast<std::size_t>(o[i++]);
            NumberList pts;
            for (std::size_t j = 0; j < n && i + 1 < o.size(); ++j, i += 2) pts.insert(pts.end(), {o[i], o[i + 1]});
            k.outlines.push_back(std::move(pts));
        }
    }
    k.dims.clear();
    {
        const auto ids = splitOn(textOf(r, "dims"), ',');
        const auto values = splitOn(textOf(r, "dim_values"), '\n');
        const NumberList at = listField(r, "dim_at"), lines = listField(r, "dim_lines"), measured = listField(r, "dim_measured");
        for (std::size_t i = 0; i < ids.size(); ++i) {
            SketchEdit::Dim d;
            const auto colon = ids[i].find(':');
            d.id = ids[i].substr(0, colon);
            d.type = colon == std::string::npos ? std::string() : ids[i].substr(colon + 1);
            d.value = i < values.size() ? values[i] : std::string();
            if (2 * i + 1 < at.size()) d.x = at[2 * i], d.y = at[2 * i + 1];
            for (int j = 0; j < 4 && 4 * i + 3 < lines.size(); ++j) d.line[j] = lines[4 * i + static_cast<std::size_t>(j)];
            if (i < measured.size()) d.measured = measured[i];
            k.dims.push_back(std::move(d));
        }
    }
    const NumberList o = listField(r, "origin"), u = listField(r, "u"), v = listField(r, "v"), n = listField(r, "n");
    if (o.size() == 3 && u.size() == 3 && v.size() == 3 && n.size() == 3) {
        for (int i = 0; i < 3; ++i) k.origin[i] = o[i], k.u[i] = u[i], k.v[i] = v[i], k.n[i] = n[i];
    }
    // the face it was started on, flattened into the sketch: lines to trace and corners to snap to
    k.faceEdges.clear();
    for (std::size_t i = 0; i < k.face3d.size();) {
        const std::size_t count = static_cast<std::size_t>(k.face3d[i++]);
        std::vector<double> xy;
        for (std::size_t j = 0; j < count && i + 2 < k.face3d.size() + 1; ++j, i += 3) {
            double d[3] = {k.face3d[i] - k.origin[0], k.face3d[i + 1] - k.origin[1], k.face3d[i + 2] - k.origin[2]};
            xy.push_back(d[0] * k.u[0] + d[1] * k.u[1] + d[2] * k.u[2]);
            xy.push_back(d[0] * k.v[0] + d[1] * k.v[1] + d[2] * k.v[2]);
        }
        for (std::size_t j = 0; j + 1 < xy.size(); j += 2) {
            const std::size_t next = (j + 2) % xy.size();
            k.faceEdges.insert(k.faceEdges.end(), {xy[j], xy[j + 1], xy[next], xy[next + 1]});
        }
    }
}

void Shell::sketchToolbar() {
    SketchEdit& k = sketch_;
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(("Sketching " + k.sketch + ":").c_str());
    for (const auto& t : kSketchTools) {
        ImGui::SameLine();
        const bool on = k.tool == t.tool;
        if (on) {
            ImGui::PushStyleColor(ImGuiCol_Button, accent_);
            ImGui::PushStyleColor(ImGuiCol_Text, accentText_);
        }
        if (ImGui::Button((std::string(t.name) + " (" + t.letter + ")").c_str())) {
            k.tool = t.tool;
            k.drawing = false;
        }
        if (on) ImGui::PopStyleColor(2);
        ImGui::SetItemTooltip("%s", t.tip);
    }
    ImGui::SameLine();
    if (ImGui::Button("Fit")) k.fit = true;
    ImGui::SetItemTooltip("Show the whole sketch (or double-click)");
    ImGui::SameLine();
    ImGui::BeginDisabled(k.profiles.empty() || !find("cad.sketch_extrude"));
    if (accentButton("Extrude...")) {
        const std::string name = k.sketch, body = k.onBody;
        leaveSketch();
        select("cad.sketch_extrude");
        if (form_) {
            for (auto& f : form_->fields) {
                if (f.spec.name == "sketch") setFieldText(f, name);
                if (f.spec.name == "target" && !body.empty()) setFieldText(f, body);  // a sketch on a face adds to that body
            }
        }
    }
    ImGui::EndDisabled();
    ImGui::SetItemTooltip(k.profiles.empty() ? "Draw a closed shape first (a rectangle, a circle, or lines that end where they started)"
                                             : "Pull the closed shapes into a solid: drag the arrow on it or type the depth, and the part shows the result as you go");
    ImGui::SameLine();
    if (ImGui::Button("Finish sketch")) leaveSketch();
    ImGui::SetItemTooltip("Back to the 3D view");
    ImGui::SameLine();
    ImGui::TextDisabled("%s", k.status.c_str());
}

void Shell::sketchCanvas(ImDrawList* dl, ImVec2 v0, ImVec2 size, bool hovered, bool active) {
    SketchEdit& k = sketch_;
    refreshSketch();
    if (!k.active) return;
    ImGuiIO& io = ImGui::GetIO();
    const ImVec2 mid{v0.x + size.x / 2, v0.y + size.y / 2};

    if (k.fit) {  // the whole sketch, or 50 units around the origin when it's empty
        double x0 = -25, y0 = -25, x1 = 25, y1 = 25;
        bool any = false;
        auto grow = [&](const NumberList& l) {
            for (std::size_t i = 0; i + 1 < l.size(); i += 2) {
                if (!any) x0 = x1 = l[i], y0 = y1 = l[i + 1];
                any = true;
                x0 = std::min(x0, l[i]), x1 = std::max(x1, l[i]), y0 = std::min(y0, l[i + 1]), y1 = std::max(y1, l[i + 1]);
            }
        };
        grow(k.lines);
        grow(k.construction);
        grow(k.faceEdges);
        if (any) {
            const double pad = std::max({x1 - x0, y1 - y0, 10.0}) * 0.15;
            x0 -= pad, x1 += pad, y0 -= pad, y1 += pad;
        }
        k.cx = (x0 + x1) / 2;
        k.cy = (y0 + y1) / 2;
        k.ppm = std::min(size.x / (x1 - x0), size.y / (y1 - y0));
        k.fit = false;
    }
    auto toScreen = [&](double x, double y) {
        return ImVec2{mid.x + static_cast<float>((x - k.cx) * k.ppm), mid.y - static_cast<float>((y - k.cy) * k.ppm)};
    };
    auto toSketch = [&](ImVec2 s, double& x, double& y) {
        x = k.cx + (s.x - mid.x) / k.ppm;
        y = k.cy - (s.y - mid.y) / k.ppm;
    };

    // pan with the right or middle button, zoom with the wheel around the mouse
    if (active && (ImGui::IsMouseDown(ImGuiMouseButton_Right) || ImGui::IsMouseDown(ImGuiMouseButton_Middle))) {
        k.cx -= io.MouseDelta.x / k.ppm;
        k.cy += io.MouseDelta.y / k.ppm;
    }
    if (hovered && io.MouseWheel != 0) {
        double bx, by;
        toSketch(io.MousePos, bx, by);
        k.ppm = std::clamp(k.ppm * std::pow(1.15, static_cast<double>(io.MouseWheel)), 1e-3, 1e5);
        double ax, ay;
        toSketch(io.MousePos, ax, ay);
        k.cx += bx - ax;
        k.cy += by - ay;
    }
    if (hovered && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left) && !k.drawing) k.fit = true;

    // grid: the smallest of 1, 2, 5 x 10^n that keeps lines 12 px apart
    double step = 1e-3;
    for (int e = -3; e < 7 && step * k.ppm < 12; ++e) {
        for (double m : {1.0, 2.0, 5.0}) {
            step = m * std::pow(10.0, e);
            if (step * k.ppm >= 12) break;
        }
    }
    const bool dark = darkLook();
    dl->AddRectFilled(v0, {v0.x + size.x, v0.y + size.y}, dark ? IM_COL32(32, 34, 38, 255) : IM_COL32(250, 250, 252, 255));
    {
        double x0, y0, x1, y1;
        toSketch(v0, x0, y1);
        toSketch({v0.x + size.x, v0.y + size.y}, x1, y0);
        const ImU32 minor = dark ? IM_COL32(48, 51, 56, 255) : IM_COL32(232, 234, 238, 255);
        const ImU32 major = dark ? IM_COL32(64, 68, 75, 255) : IM_COL32(210, 213, 220, 255);
        for (double x = std::floor(x0 / step) * step; x <= x1; x += step) {
            const bool big = std::fmod(std::fabs(std::round(x / step)), 5.0) < 0.5;
            const float sx = toScreen(x, 0).x;
            dl->AddLine({sx, v0.y}, {sx, v0.y + size.y}, big ? major : minor);
        }
        for (double y = std::floor(y0 / step) * step; y <= y1; y += step) {
            const bool big = std::fmod(std::fabs(std::round(y / step)), 5.0) < 0.5;
            const float sy = toScreen(0, y).y;
            dl->AddLine({v0.x, sy}, {v0.x + size.x, sy}, big ? major : minor);
        }
        const ImVec2 o = toScreen(0, 0);
        dl->AddLine({v0.x, o.y}, {v0.x + size.x, o.y}, IM_COL32(220, 70, 70, 200), 1.5f);  // x axis
        dl->AddLine({o.x, v0.y}, {o.x, v0.y + size.y}, IM_COL32(60, 170, 80, 200), 1.5f);  // y axis
    }

    // the part behind the sketch, seen straight on (as Onshape turns the view to the sketch plane)
    if (auto doc = session_.workspace().get(k.doc)) {
        const int w = static_cast<int>(size.x), h = static_cast<int>(size.y);
        const bool moving = active && (ImGui::IsMouseDown(ImGuiMouseButton_Right) || ImGui::IsMouseDown(ImGuiMouseButton_Middle));
        ViewCamera cam;
        for (int i = 0; i < 3; ++i) cam.right[i] = k.u[i], cam.up[i] = k.v[i], cam.forward[i] = -k.n[i];
        cam.cx = k.origin[0] * k.u[0] + k.origin[1] * k.u[1] + k.origin[2] * k.u[2] + k.cx;
        cam.cy = k.origin[0] * k.v[0] + k.origin[1] * k.v[1] + k.origin[2] * k.v[2] + k.cy;
        cam.scale = k.ppm;
        char key[160];
        std::snprintf(key, sizeof key, "%d %d %.9g %.9g %.9g %llu %d", w, h, cam.cx, cam.cy, cam.scale, static_cast<unsigned long long>(changes_), moving ? 0 : 1);
        if (key != k.backdropKey && w >= 8 && h >= 8) {
            k.backdropKey = key;
            OrbitView view;
            view.camera = cam;
            view.antialias = !moving && io.MouseWheel == 0;
            std::vector<std::uint8_t> rgba;
            if (doc->renderView(view, w, h, rgba)) {
                // the renderer's light grey background lets the grid show through
                const std::uint8_t bg[3] = {246, 247, 249};  // mesh::RenderOptions' background
                for (std::size_t i = 0; i + 3 < rgba.size(); i += 4)
                    if (rgba[i] == bg[0] && rgba[i + 1] == bg[1] && rgba[i + 2] == bg[2]) rgba[i + 3] = 0;
                k.backdrop.upload(renderer_, rgba.data(), w, h);
            }
        }
        if (k.backdrop.tex) dl->AddImage(k.backdrop.id(), v0, {v0.x + size.x, v0.y + size.y}, {0, 0}, {1, 1}, IM_COL32(255, 255, 255, 150));
    }

    // closed profiles shaded (what an extrude will use), then the lines and points
    const ImVec4 c = accent_;
    for (std::size_t i = 0; i < k.profiles.size();) {
        const std::size_t n = static_cast<std::size_t>(k.profiles[i++]);
        std::vector<ImVec2> pts;
        for (std::size_t j = 0; j < n && i + 1 < k.profiles.size(); ++j, i += 2) pts.push_back(toScreen(k.profiles[i], k.profiles[i + 1]));
        if (pts.size() >= 3) dl->AddConcavePolyFilled(pts.data(), static_cast<int>(pts.size()), ImGui::ColorConvertFloat4ToU32({c.x, c.y, c.z, 0.16f}));
    }
    const ImU32 ink = dark ? IM_COL32(235, 236, 240, 255) : IM_COL32(25, 27, 32, 255);
    for (std::size_t i = 0; i + 3 < k.lines.size(); i += 4) dl->AddLine(toScreen(k.lines[i], k.lines[i + 1]), toScreen(k.lines[i + 2], k.lines[i + 3]), ink, 2.0f);
    for (std::size_t i = 0; i + 3 < k.construction.size(); i += 4)
        dl->AddLine(toScreen(k.construction[i], k.construction[i + 1]), toScreen(k.construction[i + 2], k.construction[i + 3]), IM_COL32(60, 120, 230, 200), 1.0f);
    for (std::size_t i = 0; i + 3 < k.faceEdges.size(); i += 4)
        dl->AddLine(toScreen(k.faceEdges[i], k.faceEdges[i + 1]), toScreen(k.faceEdges[i + 2], k.faceEdges[i + 3]), IM_COL32(120, 125, 140, 220), 1.5f);
    for (std::size_t i = 0; i + 1 < k.points.size(); i += 2) dl->AddCircleFilled(toScreen(k.points[i], k.points[i + 1]), 3.0f, ink);
    for (std::size_t i = 0; i + 1 < k.free.size(); i += 2) dl->AddCircleFilled(toScreen(k.free[i], k.free[i + 1]), 3.0f, IM_COL32(60, 120, 230, 255));

    // tools' letters
    if (!io.WantTextInput && !io.KeyCtrl && !io.KeyAlt) {
        for (const auto& t : kSketchTools) {
            if (ImGui::IsKeyPressed(t.key, false)) {
                k.tool = t.tool;
                k.drawing = false;
            }
        }
    }
    if (k.drawing && (ImGui::IsKeyPressed(ImGuiKey_Escape) || (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Right)))) k.drawing = false;

    // where the mouse is, snapped: to a point of the sketch within 10 px, else to the grid (Alt: free)
    double mx, my;
    toSketch(io.MousePos, mx, my);
    bool onPoint = false;
    auto snapTo = [&](const NumberList& l) {
        for (std::size_t i = 0; i + 1 < l.size(); i += 2) {
            const ImVec2 s = toScreen(l[i], l[i + 1]);
            const float dx = s.x - io.MousePos.x, dy = s.y - io.MousePos.y;
            if (dx * dx + dy * dy < 100) {
                mx = l[i], my = l[i + 1];
                onPoint = true;
                return;
            }
        }
    };
    snapTo(k.points);
    if (!onPoint) snapTo(k.free);
    if (!onPoint) snapTo(k.faceEdges);  // the corners of the face the sketch is on
    if (!onPoint && k.drawing && k.tool == SketchEdit::Tool::Line) snapTo(NumberList{k.chainX, k.chainY});
    if (!onPoint && !io.KeyAlt) {
        mx = std::round(mx / step) * step;
        my = std::round(my / step) * step;
    }
    if (hovered) {
        const ImVec2 s = toScreen(mx, my);
        dl->AddCircle(s, onPoint ? 6.0f : 4.0f, ImGui::ColorConvertFloat4ToU32(c), 0, 2.0f);
    }

    auto report = [&](Result r, const char* what) {
        resultFor_ = what;
        r.message = r.ok ? sentence(r.message) : friendlyError(r.message);
        result_ = std::move(r);
    };
    auto base = [&]() { return Params{{"doc", k.doc}, {"sketch", k.sketch}}; };
    const bool shapeTool = k.tool == SketchEdit::Tool::Line || k.tool == SketchEdit::Tool::Rect || k.tool == SketchEdit::Tool::Circle;
    const double now = static_cast<double>(SDL_GetTicks()) / 1000.0;

    // the dimensions, written on the sketch: double-click one (or click it with Select) to change it
    int hotDim = -1;
    for (std::size_t i = 0; i < k.dims.size(); ++i) {
        SketchEdit::Dim& d = k.dims[i];
        const ImVec2 a = toScreen(d.line[0], d.line[1]), b = toScreen(d.line[2], d.line[3]);
        float nx = -(b.y - a.y), ny = b.x - a.x;
        const float nl = std::sqrt(nx * nx + ny * ny);
        if (nl > 1e-3f) nx /= nl, ny /= nl;
        else nx = 0.7f, ny = -0.7f;
        const ImVec2 at{toScreen(d.x, d.y).x + nx * 16, toScreen(d.x, d.y).y + ny * 16};
        std::string label = d.value;
        if (!plainNumber(d.value)) label += " = " + num(std::round(d.measured * 1000) / 1000);
        if (d.type == "diameter") label = "\xC3\x98" + label;  // the diameter sign
        else if (d.type == "radius") label = "R" + label;
        else if (d.type == "angle") label += "\xC2\xB0";
        const ImVec2 ts = ImGui::CalcTextSize(label.c_str());
        d.boxMin = {at.x - ts.x / 2 - 5, at.y - ts.y / 2 - 2};
        d.boxMax = {at.x + ts.x / 2 + 5, at.y + ts.y / 2 + 2};
        const bool over = hovered && io.MousePos.x >= d.boxMin.x && io.MousePos.x <= d.boxMax.x && io.MousePos.y >= d.boxMin.y && io.MousePos.y <= d.boxMax.y;
        if (over) hotDim = static_cast<int>(i);
        if (d.type != "radius" && d.type != "diameter" && d.type != "angle") {  // witness: the measured span, offset to the label
            const ImU32 wit = IM_COL32(60, 120, 230, 160);
            dl->AddLine({a.x + nx * 16, a.y + ny * 16}, {b.x + nx * 16, b.y + ny * 16}, wit, 1.0f);
            dl->AddLine(a, {a.x + nx * 20, a.y + ny * 20}, wit, 1.0f);
            dl->AddLine(b, {b.x + nx * 20, b.y + ny * 20}, wit, 1.0f);
        }
        dl->AddRectFilled(d.boxMin, d.boxMax, over ? ImGui::ColorConvertFloat4ToU32(c) : (dark ? IM_COL32(40, 60, 100, 235) : IM_COL32(225, 235, 252, 240)), 4.0f);
        dl->AddText({d.boxMin.x + 5, d.boxMin.y + 2}, over ? ImGui::ColorConvertFloat4ToU32(accentText_) : (dark ? IM_COL32(220, 230, 255, 255) : IM_COL32(30, 70, 160, 255)),
                    label.c_str());
    }
    auto openEdit = [&](const std::string& id, const std::string& type, const std::string& refs, const std::string& value) {
        k.editId = id, k.editType = type, k.editRefs = refs;
        std::snprintf(k.editBuf, sizeof k.editBuf, "%s", value.c_str());
        k.editOpen = true;
        k.drawing = false;
        ImGui::OpenPopup("##sketchdim");
    };
    if (hotDim >= 0 && !k.drawing &&
        (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left) || (k.tool == SketchEdit::Tool::Select && ImGui::IsMouseClicked(ImGuiMouseButton_Left)))) {
        const auto& d = k.dims[static_cast<std::size_t>(hotDim)];
        openEdit(d.id, d.type, "", d.value);
        k.fit = false;  // not the double-click that fits the view
    }
    if (hotDim >= 0 && !k.editOpen) ImGui::SetTooltip("%s", "Double-click to change this dimension (a number, or a formula like width/2)");

    // the point under the mouse (Select drags it, Dimension measures from it)
    int hotPoint = -1;
    if (hovered && hotDim < 0) {
        float best = 64;
        for (std::size_t i = 0; i < k.pointIds.size() && 2 * i + 1 < k.pointXy.size(); ++i) {
            const ImVec2 s2 = toScreen(k.pointXy[2 * i], k.pointXy[2 * i + 1]);
            const float dx = s2.x - io.MousePos.x, dy = s2.y - io.MousePos.y;
            if (dx * dx + dy * dy < best) best = dx * dx + dy * dy, hotPoint = static_cast<int>(i);
        }
    }
    const bool click = hovered && hotDim < 0 && !k.editOpen && ImGui::IsMouseClicked(ImGuiMouseButton_Left) && !ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left);

    if (k.tool == SketchEdit::Tool::Select) {
        if (k.dragPoint.empty()) {
            if (hotPoint >= 0) {
                dl->AddCircle(toScreen(k.pointXy[2 * static_cast<std::size_t>(hotPoint)], k.pointXy[2 * static_cast<std::size_t>(hotPoint) + 1]), 7.0f,
                              ImGui::ColorConvertFloat4ToU32(c), 0, 2.5f);
                if (click) {
                    k.dragPoint = k.pointIds[static_cast<std::size_t>(hotPoint)];
                    k.dragX = mx, k.dragY = my;
                    k.triedX = k.triedY = 1e300;
                    k.movedAt = now;
                }
            }
        } else {
            if (mx != k.dragX || my != k.dragY) k.dragX = mx, k.dragY = my, k.movedAt = now;
            Params p = base();
            p["point"] = k.dragPoint, p["x"] = num(k.dragX), p["y"] = num(k.dragY);
            // resting: try it on the sketch, so it and the part follow as you go
            if (now - k.movedAt > 0.1 && (k.dragX != k.triedX || k.dragY != k.triedY)) {
                tryRun("cad.sketch_move", p);
                k.triedX = k.dragX, k.triedY = k.dragY;
            }
            dl->AddCircleFilled(toScreen(k.dragX, k.dragY), 5.0f, ImGui::ColorConvertFloat4ToU32(c));
            if (ImGui::IsKeyPressed(ImGuiKey_Escape)) {
                dropTrial();
                k.dragPoint.clear();
            } else if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
                resultFor_ = "Move point";
                if (keepTrial("cad.sketch_move", p)) {
                    Result r = trial_.result;
                    r.message = sentence(r.message);
                    result_ = std::move(r);
                } else {
                    report(run("cad.sketch_move", p), "Move point");
                }
                k.dragPoint.clear();
            }
        }
    } else if (k.tool == SketchEdit::Tool::Dimension && click) {
        auto pointAt = [&](int i) { return std::make_pair(k.pointXy[2 * static_cast<std::size_t>(i)], k.pointXy[2 * static_cast<std::size_t>(i) + 1]); };
        if (hotPoint >= 0) {
            const std::string id = k.pointIds[static_cast<std::size_t>(hotPoint)];
            if (k.dimFirst.empty() || k.dimFirst == id) {
                k.dimFirst = id;
            } else {
                double ax = 0, ay = 0;
                for (std::size_t i = 0; i < k.pointIds.size(); ++i)
                    if (k.pointIds[i] == k.dimFirst) std::tie(ax, ay) = pointAt(static_cast<int>(i));
                const auto [bx, by] = pointAt(hotPoint);
                openEdit("", "distance", k.dimFirst + "," + id, num(std::round(std::hypot(bx - ax, by - ay) * 1000) / 1000));
                k.dimFirst.clear();
            }
        } else {
            // the line, circle or arc under the mouse
            float best = 7;
            int hit = -1;
            for (std::size_t i = 0; i < k.outlines.size() && i < k.entities.size(); ++i) {
                const NumberList& o = k.outlines[i];
                for (std::size_t j = 0; j + 3 < o.size(); j += 2) {
                    const float dd = segDist(io.MousePos, toScreen(o[j], o[j + 1]), toScreen(o[j + 2], o[j + 3]));
                    if (dd < best) best = dd, hit = static_cast<int>(i);
                }
            }
            k.dimFirst.clear();
            if (hit >= 0) {
                const auto& [id, kind] = k.entities[static_cast<std::size_t>(hit)];
                const NumberList& o = k.outlines[static_cast<std::size_t>(hit)];
                if (kind == "line" && o.size() >= 4) {
                    openEdit("", "length", id, num(std::round(std::hypot(o[o.size() - 2] - o[0], o[o.size() - 1] - o[1]) * 1000) / 1000));
                } else if ((kind == "circle" || kind == "arc") && o.size() >= 6) {
                    // the circle through the first, middle and last points of its outline
                    const std::size_t m = (o.size() / 2 / 2) * 2, l = o.size() - 2;
                    const double ax = o[0], ay = o[1], bx = o[m], by = o[m + 1], cx = o[l], cy = o[l + 1];
                    double r = 0;
                    const double d = 2 * (ax * (by - cy) + bx * (cy - ay) + cx * (ay - by));
                    if (std::fabs(d) > 1e-12) {
                        const double ux = ((ax * ax + ay * ay) * (by - cy) + (bx * bx + by * by) * (cy - ay) + (cx * cx + cy * cy) * (ay - by)) / d;
                        const double uy = ((ax * ax + ay * ay) * (cx - bx) + (bx * bx + by * by) * (ax - cx) + (cx * cx + cy * cy) * (bx - ax)) / d;
                        r = std::hypot(ax - ux, ay - uy);
                    }
                    if (kind == "circle") openEdit("", "diameter", id, num(std::round(2 * r * 1000) / 1000));
                    else openEdit("", "radius", id, num(std::round(r * 1000) / 1000));
                }
            }
        }
    }
    if (k.tool == SketchEdit::Tool::Dimension) {
        if (!k.dimFirst.empty())
            for (std::size_t i = 0; i < k.pointIds.size() && 2 * i + 1 < k.pointXy.size(); ++i)
                if (k.pointIds[i] == k.dimFirst) dl->AddCircle(toScreen(k.pointXy[2 * i], k.pointXy[2 * i + 1]), 8.0f, ImGui::ColorConvertFloat4ToU32(c), 0, 3.0f);
        if (ImGui::IsKeyPressed(ImGuiKey_Escape)) k.dimFirst.clear();
    }

    // typing a dimension's value
    if (ImGui::BeginPopup("##sketchdim")) {
        ImGui::TextUnformatted(k.editId.empty() ? ("New " + k.editType + " (a number or a formula):").c_str() : ("Change " + k.editId + " (" + k.editType + "):").c_str());
        if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
        ImGui::SetNextItemWidth(200);
        const bool enter = ImGui::InputText("##dimvalue", k.editBuf, sizeof k.editBuf, ImGuiInputTextFlags_EnterReturnsTrue);
        ImGui::SameLine();
        if (enter || ImGui::Button("OK")) {
            Params p = base();
            const char* what = k.editId.empty() ? "Dimension" : "Change dimension";
            if (k.editId.empty()) {
                p["type"] = k.editType, p["refs"] = k.editRefs, p["value"] = std::string(k.editBuf);
                report(run("cad.sketch_dimension", p), what);
            } else {
                p["constraint"] = k.editId, p["value"] = std::string(k.editBuf);
                report(run("cad.sketch_set", p), what);
            }
            k.editOpen = false;
            ImGui::CloseCurrentPopup();
        }
        if (ImGui::IsKeyPressed(ImGuiKey_Escape)) {
            k.editOpen = false;
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    } else {
        k.editOpen = false;
    }
    auto finishShape = [&](double bx, double by) {
        if (k.tool == SketchEdit::Tool::Line) {
            if (std::fabs(bx - k.ax) < 1e-9 && std::fabs(by - k.ay) < 1e-9) return;
            Params p = base();
            p["x1"] = num(k.ax), p["y1"] = num(k.ay), p["x2"] = num(bx), p["y2"] = num(by);
            Result r = run("cad.sketch_line", p);
            const bool ok = r.ok;
            report(std::move(r), "Line");
            // keep going from the end, unless it closed the shape
            const bool closed = std::fabs(bx - k.chainX) < 1e-9 && std::fabs(by - k.chainY) < 1e-9;
            k.drawing = ok && !closed;
            k.ax = bx, k.ay = by;
            return;
        }
        k.drawing = false;
        if (k.tool == SketchEdit::Tool::Rect) {
            const double w = std::fabs(bx - k.ax), h = std::fabs(by - k.ay);
            if (w < 1e-9 || h < 1e-9) return;
            Params p = base();
            p["x"] = num(std::min(k.ax, bx)), p["y"] = num(std::min(k.ay, by)), p["width"] = num(w), p["height"] = num(h);
            report(run("cad.sketch_rect", p), "Rectangle");
        } else {
            const double r = std::hypot(bx - k.ax, by - k.ay);
            if (r < 1e-9) return;
            Params p = base();
            p["cx"] = num(k.ax), p["cy"] = num(k.ay), p["diameter"] = num(2 * r);
            report(run("cad.sketch_circle", p), "Circle");
        }
    };

    const bool press = shapeTool && click;
    if (press) {
        if (!k.drawing) {
            k.drawing = true;
            k.ax = k.chainX = mx;
            k.ay = k.chainY = my;
        } else {
            finishShape(mx, my);
        }
        k.pressAt = io.MousePos;
    }
    // a drag (rather than two clicks) makes a shape in one go; a dragged line keeps going from its end
    if (shapeTool && k.drawing && ImGui::IsMouseReleased(ImGuiMouseButton_Left)) {
        const float dx = io.MousePos.x - k.pressAt.x, dy = io.MousePos.y - k.pressAt.y;
        if (dx * dx + dy * dy > 64) finishShape(mx, my);
    }

    // the shape being drawn, with its size
    if (k.drawing) {
        const ImU32 draft = ImGui::ColorConvertFloat4ToU32(c);
        const ImVec2 a = toScreen(k.ax, k.ay), b = toScreen(mx, my);
        std::string label;
        if (k.tool == SketchEdit::Tool::Line) {
            dl->AddLine(a, b, draft, 2.0f);
            label = num(std::round(std::hypot(mx - k.ax, my - k.ay) * 1000) / 1000) + " " + k.units;
        } else if (k.tool == SketchEdit::Tool::Rect) {
            dl->AddRect({std::min(a.x, b.x), std::min(a.y, b.y)}, {std::max(a.x, b.x), std::max(a.y, b.y)}, draft, 0, 0, 2.0f);
            label = num(std::round(std::fabs(mx - k.ax) * 1000) / 1000) + " x " + num(std::round(std::fabs(my - k.ay) * 1000) / 1000) + " " + k.units;
        } else {
            const double r = std::hypot(mx - k.ax, my - k.ay);
            dl->AddCircle(a, static_cast<float>(r * k.ppm), draft, 96, 2.0f);
            dl->AddLine(a, b, draft, 1.0f);
            label = "diameter " + num(std::round(2 * r * 1000) / 1000) + " " + k.units;
        }
        dl->AddText({b.x + 12, b.y + 10}, ink, label.c_str());
    } else if (hovered && shapeTool) {
        const std::string at = num(std::round(mx * 1000) / 1000) + ", " + num(std::round(my * 1000) / 1000);
        dl->AddText({io.MousePos.x + 12, io.MousePos.y + 10}, IM_COL32(128, 128, 128, 255), at.c_str());
    }
    const char* hint = k.tool == SketchEdit::Tool::Line        ? "Line: click to start, click for each corner; click the first point, right-click or Esc to stop"
                       : k.tool == SketchEdit::Tool::Rect      ? "Rectangle: drag corner to corner"
                       : k.tool == SketchEdit::Tool::Circle    ? "Circle: drag from the centre"
                       : k.tool == SketchEdit::Tool::Select    ? "Select: drag a point to move it; double-click a dimension to change it"
                                                               : "Dimension: click a line, a circle, or two points, then type the value";
    dl->AddText({v0.x + 10, v0.y + size.y - ImGui::GetFontSize() - 8}, IM_COL32(128, 128, 128, 255), hint);
}

}  // namespace wetwaresuite::gui
