// Free transform on a picture (Ctrl+T, like Photoshop): a box around the selection (or the
// layer's pixels) with handles. Drag a corner to scale (Shift: not in proportion), an edge to
// stretch one way, inside to move, just outside to rotate; Ctrl+drag a corner moves it alone for
// perspective. The picture previews while you drag (a photo.transform trial with the box's
// four corners), Enter or Apply keeps it as one undo step, Esc or Cancel puts it back.
#include <algorithm>
#include <cmath>

#include "shell.hpp"
#include "imgui_stdlib.h"

namespace wetwaresuite::gui {

namespace {

constexpr float kPiF = 3.14159265f;

double nowS() { return static_cast<double>(SDL_GetTicks()) / 1000.0; }

ImVec2 add(ImVec2 a, ImVec2 b) { return {a.x + b.x, a.y + b.y}; }
ImVec2 sub(ImVec2 a, ImVec2 b) { return {a.x - b.x, a.y - b.y}; }
ImVec2 mul(ImVec2 a, float s) { return {a.x * s, a.y * s}; }
float len(ImVec2 a) { return std::sqrt(a.x * a.x + a.y * a.y); }
ImVec2 rotate(ImVec2 p, ImVec2 c, float a) {
    const float s = std::sin(a), co = std::cos(a);
    const ImVec2 d = sub(p, c);
    return {c.x + d.x * co - d.y * s, c.y + d.x * s + d.y * co};
}

bool inside(const ImVec2 q[4], ImVec2 p) {
    bool in = false;
    for (int i = 0, j = 3; i < 4; j = i++) {
        if (((q[i].y > p.y) != (q[j].y > p.y)) && p.x < (q[j].x - q[i].x) * (p.y - q[i].y) / (q[j].y - q[i].y + 1e-9f) + q[i].x) in = !in;
    }
    return in;
}

std::int64_t intOf(const Result& r, const char* key) {
    const Value* v = r.field(key);
    if (!v) return 0;
    if (auto i = std::get_if<std::int64_t>(v)) return *i;
    if (auto d = std::get_if<double>(v)) return static_cast<std::int64_t>(*d);
    return 0;
}

}  // namespace

Params Shell::transformParams() const {
    const FreeTransform& x = xform_;
    NumberList from, to;
    for (int i = 0; i < 4; ++i) {
        from.push_back(x.base[i].x), from.push_back(x.base[i].y);
        to.push_back(x.quad[i].x), to.push_back(x.quad[i].y);
    }
    return {{"doc", currentDoc_}, {"from", from}, {"corners", to}};
}

void Shell::transformStart() {
    FreeTransform& x = xform_;
    x = FreeTransform{};
    const Result r = query("photo.transform_box", {{"doc", currentDoc_}});
    if (!r.ok) {
        result_ = Result::failure(friendlyError(r.message));
        return;
    }
    const float x0 = static_cast<float>(intOf(r, "x0")), y0 = static_cast<float>(intOf(r, "y0"));
    const float x1 = static_cast<float>(intOf(r, "x1")), y1 = static_cast<float>(intOf(r, "y1"));
    if (x1 - x0 < 1 || y1 - y0 < 1) return;
    x.base[0] = {x0, y0}, x.base[1] = {x1, y0}, x.base[2] = {x1, y1}, x.base[3] = {x0, y1};
    for (int i = 0; i < 4; ++i) x.quad[i] = x.base[i];
    x.doc = currentDoc_;
    x.active = true;
}

void Shell::transformFinish(bool keep) {
    FreeTransform& x = xform_;
    if (!x.active) return;
    const bool moved = [&] {
        for (int i = 0; i < 4; ++i)
            if (len(sub(x.quad[i], x.base[i])) > 0.01f) return true;
        return false;
    }();
    if (keep && moved) {
        const Params p = transformParams();
        if (!keepTrial("photo.transform", p)) {
            Result r = run("photo.transform", p);
            resultFor_ = "Free transform";
            r.message = r.ok ? sentence(r.message) : friendlyError(r.message);
            result_ = std::move(r);
        } else {
            resultFor_ = "Free transform";
            result_ = Result::success("Transformed.");
        }
    } else {
        dropTrial();
    }
    x = FreeTransform{};
    canvas_.dirty = true;
}

void Shell::transformToolbar() {
    if (!xform_.active || xform_.doc != currentDoc_) {
        ImGui::TextDisabled("Click the picture to put handles on the selection (or the layer)");
        return;
    }
    if (ImGui::Button("Apply")) transformFinish(true);
    ImGui::SetItemTooltip("Keep the transform (Enter)");
    ImGui::SameLine();
    if (ImGui::Button("Cancel")) transformFinish(false);
    ImGui::SetItemTooltip("Put it back (Esc)");
    ImGui::SameLine();
    ImGui::TextDisabled("corners scale (Shift: freely), edges stretch, outside rotates (Shift: 15 deg), Ctrl+corner: perspective");
}

void Shell::transformInput(ImDrawList* dl, ImVec2 p0, bool hovered, bool active) {
    FreeTransform& x = xform_;
    ImGuiIO& io = ImGui::GetIO();
    if (x.active && x.doc != currentDoc_) transformFinish(false);
    if (!x.active) {
        if (hovered && ImGui::IsItemActivated() && ImGui::IsMouseDown(ImGuiMouseButton_Left)) transformStart();
        if (!x.active) return;
    }
    const float z = canvas_.zoom;
    auto scr = [&](ImVec2 img) { return ImVec2{p0.x + img.x * z, p0.y + img.y * z}; };
    auto img = [&](ImVec2 s) { return ImVec2{(s.x - p0.x) / z, (s.y - p0.y) / z}; };

    ImVec2 sq[4];
    for (int i = 0; i < 4; ++i) sq[i] = scr(x.quad[i]);
    ImVec2 handles[8];
    for (int i = 0; i < 4; ++i) {
        handles[i] = sq[i];
        handles[4 + i] = mul(add(sq[i], sq[(i + 1) % 4]), 0.5f);
    }
    const ImVec2 centreS = mul(add(add(sq[0], sq[1]), add(sq[2], sq[3])), 0.25f);
    const float hs = ImGui::GetFontSize() * 0.32f;

    // which handle is under the mouse
    int hover = -1;  // 0..3 corners, 4..7 edges (top, right, bottom, left), 8 inside, 9 rotate
    for (int i = 0; i < 8 && hover < 0; ++i)
        if (std::fabs(io.MousePos.x - handles[i].x) <= hs + 3 && std::fabs(io.MousePos.y - handles[i].y) <= hs + 3) hover = i;
    if (hover < 0 && inside(sq, io.MousePos)) hover = 8;
    if (hover < 0 && hovered) hover = 9;

    if (ImGui::IsItemActivated() && ImGui::IsMouseDown(ImGuiMouseButton_Left) && hover >= 0) {
        x.drag = hover;
        x.startMouse = img(io.MousePos);
        for (int i = 0; i < 4; ++i) x.startQuad[i] = x.quad[i];
        x.startAngle = std::atan2(io.MousePos.y - centreS.y, io.MousePos.x - centreS.x);
    }
    if (x.drag >= 0 && active) {
        const ImVec2 m = img(io.MousePos);
        const ImVec2 d = sub(m, x.startMouse);
        const ImVec2* s = x.startQuad;
        if (x.drag == 8) {
            for (int i = 0; i < 4; ++i) x.quad[i] = add(s[i], d);
        } else if (x.drag == 9) {
            float a = std::atan2(io.MousePos.y - centreS.y, io.MousePos.x - centreS.x) - x.startAngle;
            if (io.KeyShift) a = std::round(a / (kPiF / 12)) * (kPiF / 12);
            const ImVec2 c = mul(add(add(s[0], s[1]), add(s[2], s[3])), 0.25f);
            for (int i = 0; i < 4; ++i) x.quad[i] = rotate(s[i], c, a);
        } else if (x.drag < 4 && io.KeyCtrl) {
            for (int i = 0; i < 4; ++i) x.quad[i] = s[i];
            x.quad[x.drag] = add(s[x.drag], d);  // perspective: one corner on its own
        } else if (x.drag < 4) {
            // scale about the opposite corner in the box's own axes
            const int o = (x.drag + 2) % 4;
            const ImVec2 ax = sub(s[1], s[0]), ay = sub(s[3], s[0]);
            const float lx = std::max(len(ax), 1e-3f), ly = std::max(len(ay), 1e-3f);
            const ImVec2 ux = mul(ax, 1 / lx), uy = mul(ay, 1 / ly);
            const ImVec2 v0 = sub(s[x.drag], s[o]), v1 = sub(m, s[o]);
            const float px0 = v0.x * ux.x + v0.y * ux.y, py0 = v0.x * uy.x + v0.y * uy.y;
            const float px1 = v1.x * ux.x + v1.y * ux.y, py1 = v1.x * uy.x + v1.y * uy.y;
            float fx = std::fabs(px0) > 1e-3f ? px1 / px0 : 1, fy = std::fabs(py0) > 1e-3f ? py1 / py0 : 1;
            if (!io.KeyShift) {  // in proportion, like Photoshop
                const float f = std::fabs(fx) > std::fabs(fy) ? fx : fy;
                fx = fy = f;
            }
            for (int i = 0; i < 4; ++i) {
                const ImVec2 v = sub(s[i], s[o]);
                const float a = v.x * ux.x + v.y * ux.y, b = v.x * uy.x + v.y * uy.y;
                x.quad[i] = add(s[o], add(mul(ux, a * fx), mul(uy, b * fy)));
            }
        } else {
            // an edge: move its two corners along the edge's normal
            const int e = x.drag - 4, a = e, b = (e + 1) % 4;
            const ImVec2 edge = sub(s[b], s[a]);
            ImVec2 n{-edge.y, edge.x};
            const float ln = std::max(len(n), 1e-3f);
            n = mul(n, 1 / ln);
            const float k = d.x * n.x + d.y * n.y;
            for (int i = 0; i < 4; ++i) x.quad[i] = s[i];
            x.quad[a] = add(s[a], mul(n, k));
            x.quad[b] = add(s[b], mul(n, k));
        }
        if (nowS() - x.lastTry > 0.12) {
            x.lastTry = nowS();
            tryRun("photo.transform", transformParams());
        }
    }
    if (x.drag >= 0 && !active) {
        x.drag = -1;
        tryRun("photo.transform", transformParams());
    }
    if (!io.WantTextInput && ImGui::IsKeyPressed(ImGuiKey_Enter, false)) transformFinish(true);
    else if (!io.WantTextInput && ImGui::IsKeyPressed(ImGuiKey_Escape, false)) transformFinish(false);
    if (!x.active) return;

    // draw
    for (int i = 0; i < 4; ++i) sq[i] = scr(x.quad[i]);
    const ImU32 line = ImGui::ColorConvertFloat4ToU32(accent_);
    dl->AddPolyline(sq, 4, IM_COL32(0, 0, 0, 160), ImDrawFlags_Closed, 3.0f);
    dl->AddPolyline(sq, 4, line, ImDrawFlags_Closed, 1.5f);
    for (int i = 0; i < 4; ++i) {
        handles[i] = sq[i];
        handles[4 + i] = mul(add(sq[i], sq[(i + 1) % 4]), 0.5f);
    }
    for (int i = 0; i < 8; ++i) {
        const ImVec2 h = handles[i];
        const bool hot = hover == i || x.drag == i;
        dl->AddRectFilled({h.x - hs, h.y - hs}, {h.x + hs, h.y + hs}, hot ? line : IM_COL32(255, 255, 255, 255));
        dl->AddRect({h.x - hs, h.y - hs}, {h.x + hs, h.y + hs}, IM_COL32(0, 0, 0, 200));
    }
    const char* tip = nullptr;
    if (x.drag < 0) {
        if (hover >= 0 && hover < 4) tip = "Drag to scale (Shift: freely). Ctrl+drag: move this corner alone (perspective)";
        else if (hover >= 4 && hover < 8) tip = "Drag to stretch one way";
        else if (hover == 8) tip = "Drag to move";
        else if (hover == 9) tip = "Drag to rotate (Shift: 15 degree steps)";
        if (tip) ImGui::SetTooltip("%s\nEnter keeps it, Esc puts it back", tip);
    }
}

}  // namespace wetwaresuite::gui

// ---- typing on the picture ----

namespace wetwaresuite::gui {

void Shell::imageTextInput(ImVec2 p0, bool hovered) {
    ImageText& t = imgText_;
    if (t.open && t.doc != currentDoc_) imageTextFinish(false);
    if (!t.open && hovered && ImGui::IsItemClicked(ImGuiMouseButton_Left)) {
        t = ImageText{};
        t.open = true;
        t.doc = currentDoc_;
        t.at = toImage(ImGui::GetIO().MousePos);
        t.focus = true;
    }
    if (!t.open) return;
    const ImVec2 at{p0.x + t.at.x * canvas_.zoom, p0.y + t.at.y * canvas_.zoom};
    // a caret where the words start, while the box takes the typing
    ImGui::GetWindowDrawList()->AddLine(at, {at.x, at.y + static_cast<float>(t.size) * canvas_.zoom}, ImGui::ColorConvertFloat4ToU32(accent_), 2.0f);
    ImGui::SetNextWindowPos({at.x, at.y + static_cast<float>(t.size) * canvas_.zoom + 6}, ImGuiCond_Appearing);
    ImGui::SetNextWindowSize({ImGui::GetFontSize() * 20, 0}, ImGuiCond_Appearing);
    bool open = true;
    bool commit = false;
    if (ImGui::Begin("Type###imagetext", &open, ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings)) {
        if (t.focus) {
            ImGui::SetKeyboardFocusHere();
            t.focus = false;
        }
        ImGui::SetNextItemWidth(-1);
        bool changed = ImGui::InputTextMultiline("##words", &t.words, {-1, ImGui::GetFontSize() * 3.2f}, ImGuiInputTextFlags_CtrlEnterForNewLine);
        // Enter (without Ctrl) ends the box: that's keeping it
        if (ImGui::IsItemDeactivated() && (ImGui::IsKeyPressed(ImGuiKey_Enter, false) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter, false))) commit = true;
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 6);
        changed |= ImGui::DragFloat("size", &t.size, 0.5f, 4, 800, "%.0f px");
        ImGui::SameLine();
        changed |= ImGui::ColorEdit4("##textcolour", canvas_.color, ImGuiColorEditFlags_NoInputs);
        ImGui::SameLine();
        changed |= ImGui::Checkbox("Bold", &t.bold);
        ImGui::SameLine();
        if (ImGui::Button("Done")) commit = true;
        ImGui::TextDisabled("Enter keeps it (Ctrl+Enter: new line), Esc throws it away");
        if (ImGui::IsKeyPressed(ImGuiKey_Escape)) open = false;
        if (changed && !t.words.empty()) tryRun("paint.text", imageTextParams());
        if (changed && t.words.empty()) dropTrial();
        if (changed) canvas_.dirty = true;
    }
    ImGui::End();
    if (commit) imageTextFinish(true);
    else if (!open) imageTextFinish(false);
}

Params Shell::imageTextParams() const {
    const ImageText& t = imgText_;
    std::string words = t.words;
    while (!words.empty() && (words.back() == '\n' || words.back() == '\r')) words.pop_back();
    return {{"doc", currentDoc_},
            {"text", words},
            {"x", static_cast<double>(t.at.x)},
            {"y", static_cast<double>(t.at.y)},
            {"size", static_cast<double>(t.size)},
            {"bold", t.bold},
            {"color", toHex({canvas_.color[0], canvas_.color[1], canvas_.color[2], canvas_.color[3]})}};
}

void Shell::imageTextFinish(bool keep) {
    ImageText& t = imgText_;
    if (!t.open) return;
    if (keep && !t.words.empty()) {
        const Params p = imageTextParams();
        Result r;
        if (keepTrial("paint.text", p)) r = Result::success("Added a text layer.");
        else r = run("paint.text", p);
        resultFor_ = "Text";
        r.message = r.ok ? sentence(r.message) : friendlyError(r.message);
        result_ = std::move(r);
    } else {
        dropTrial();
    }
    t = ImageText{};
}

}  // namespace wetwaresuite::gui
