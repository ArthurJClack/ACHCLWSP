// Layout like InDesign: the page big, every frame outlined. Click a frame to pick it, drag it to
// move, drag a corner or edge handle to resize (a ghost shows where it lands), double-click a
// text frame to retype it right on the page. Arrow keys nudge 1 mm (Shift: 0.1 mm), Delete
// removes it, PgUp/PgDn change page. Every change is still a layout.* command, so it undoes
// and Claude sees the same thing.
#include <algorithm>
#include <cmath>
#include <sstream>

#include "shell.hpp"
// after imgui.h
#include "imgui_stdlib.h"

namespace wetwaresuite::gui {

namespace {

double numberOf(const Result& r, const char* key, double fallback = 0) {
    const Value* v = r.field(key);
    if (auto d = v ? std::get_if<double>(v) : nullptr) return *d;
    if (auto i = v ? std::get_if<std::int64_t>(v) : nullptr) return static_cast<double>(*i);
    return fallback;
}

std::string textOf(const Result& r, const char* key) {
    const Value* v = r.field(key);
    auto s = v ? std::get_if<std::string>(v) : nullptr;
    return s ? *s : std::string();
}

std::vector<std::string> splitComma(const std::string& s) {
    std::vector<std::string> out;
    std::stringstream in(s);
    std::string part;
    while (std::getline(in, part, ',')) out.push_back(part);
    return out;
}

// the handle a point is on: 0..7 clockwise from top-left, -1 none
int handleAt(ImVec2 a, ImVec2 b, ImVec2 m, float r) {
    const ImVec2 pts[8] = {{a.x, a.y}, {(a.x + b.x) / 2, a.y}, {b.x, a.y}, {b.x, (a.y + b.y) / 2},
                           {b.x, b.y}, {(a.x + b.x) / 2, b.y}, {a.x, b.y}, {a.x, (a.y + b.y) / 2}};
    for (int i = 0; i < 8; ++i)
        if (std::abs(m.x - pts[i].x) <= r && std::abs(m.y - pts[i].y) <= r) return i;
    return -1;
}

}  // namespace

bool Shell::layoutDoc() const {
    auto doc = session_.workspace().get(currentDoc_);
    return doc && doc->kind() == "layout" && find("layout.frames") && find("layout.preview") && find("layout.frame_set");
}

std::string Shell::layoutPicture() {
    LayoutView& v = page_;
    if (v.doc != currentDoc_) {
        v = LayoutView{};
        v.doc = currentDoc_;
    }
    Result f = query("layout.frames", {{"doc", currentDoc_}, {"page", static_cast<std::int64_t>(v.page)}});
    if (!f.ok && v.page > 1) {  // the page shown was deleted
        v.page = 1;
        f = query("layout.frames", {{"doc", currentDoc_}, {"page", std::int64_t{1}}});
    }
    if (!f.ok) return {};
    v.pages = static_cast<int>(numberOf(f, "pages", 1));
    if (const Value* s = f.field("size"); s && std::holds_alternative<NumberList>(*s) && std::get<NumberList>(*s).size() == 2) {
        v.pageW = std::get<NumberList>(*s)[0];
        v.pageH = std::get<NumberList>(*s)[1];
    }
    v.seen = ~0ull;  // the frame list is read again with the picture
    const double dpi = std::clamp(1500.0 / std::max(v.pageW, v.pageH) * 25.4, 20.0, 300.0);
    Result r = query("layout.preview", {{"doc", currentDoc_}, {"page", static_cast<std::int64_t>(v.page)}, {"dpi", dpi}, {"guides", true}});
    if (!r.ok || r.attachments.empty()) return {};
    return r.attachments[0].bytes;
}

void Shell::layoutRefreshFrames() {
    LayoutView& v = page_;
    if (v.seen == changes_) return;
    v.seen = changes_;
    Result f = query("layout.frames", {{"doc", currentDoc_}, {"page", static_cast<std::int64_t>(v.page)}});
    v.ids.clear();
    v.types.clear();
    v.boxes.clear();
    if (!f.ok) return;
    v.ids = splitComma(textOf(f, "ids"));
    v.types = splitComma(textOf(f, "types"));
    if (const Value* b = f.field("boxes"); b && std::holds_alternative<NumberList>(*b)) {
        const auto& n = std::get<NumberList>(*b);
        for (std::size_t i = 0; i + 3 < n.size(); i += 4) v.boxes.push_back({static_cast<float>(n[i]), static_cast<float>(n[i + 1]), static_cast<float>(n[i + 2]), static_cast<float>(n[i + 3])});
    }
    v.types.resize(v.ids.size());
    v.boxes.resize(v.ids.size());
    if (v.selected >= static_cast<int>(v.ids.size())) v.selected = -1;
}

void Shell::layoutShowPage(int n) {
    n = std::clamp(n, 1, std::max(1, page_.pages));
    if (n == page_.page) return;
    layoutCommit();
    page_.page = n;
    page_.selected = -1;
    canvas_.dirty = true;
}

void Shell::layoutCanvas(ImDrawList* dl, ImVec2 p0, bool hovered, bool active) {
    LayoutView& v = page_;
    if (v.doc != currentDoc_ || canvas_.texture.srcWidth <= 0) return;
    layoutRefreshFrames();
    ImGuiIO& io = ImGui::GetIO();
    const float ppm = static_cast<float>(canvas_.texture.srcWidth / std::max(1.0, v.pageW)) * canvas_.zoom;  // screen px per mm
    const ImVec4 c = accent_;
    const ImU32 accent = ImGui::ColorConvertFloat4ToU32(c);
    auto toScreen = [&](float x, float y) { return ImVec2{p0.x + x * ppm, p0.y + y * ppm}; };
    auto norm = [](ImVec4 b) {  // lines can run backwards
        if (b.z < 0) b.x += b.z, b.z = -b.z;
        if (b.w < 0) b.y += b.w, b.w = -b.w;
        return b;
    };
    auto frameAt = [&](ImVec2 m) {
        for (int i = static_cast<int>(v.boxes.size()) - 1; i >= 0; --i) {
            ImVec4 b = norm(v.boxes[static_cast<std::size_t>(i)]);
            const float grow = 3 / ppm;  // thin lines are still easy to catch
            const ImVec2 a = toScreen(b.x - grow, b.y - grow), e = toScreen(b.x + b.z + grow, b.y + b.w + grow);
            if (m.x >= a.x && m.x <= e.x && m.y >= a.y && m.y <= e.y) return i;
        }
        return -1;
    };
    auto send = [&](const char* id, Params p, const char* what) {
        p["doc"] = currentDoc_;
        Result r = run(id, p);
        resultFor_ = what;
        const bool ok = r.ok;
        r.message = ok ? sentence(r.message) : friendlyError(r.message);
        result_ = std::move(r);
        canvas_.dirty = true;
        return ok;
    };
    const bool space = ImGui::IsKeyDown(ImGuiKey_Space);

    // press: a handle of the picked frame, else the frame on top under the mouse
    if (!v.editing && !space && ImGui::IsItemActivated() && ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
        v.handle = -1;
        if (v.selected >= 0 && v.types[static_cast<std::size_t>(v.selected)] != "line") {
            const ImVec4 b = norm(v.boxes[static_cast<std::size_t>(v.selected)]);
            v.handle = handleAt(toScreen(b.x, b.y), toScreen(b.x + b.z, b.y + b.w), io.MousePos, 6);
        }
        if (v.handle < 0) v.selected = frameAt(io.MousePos);
        v.dragging = v.selected >= 0;
        v.pressAt = io.MousePos;
        if (v.dragging) v.live = v.start = v.boxes[static_cast<std::size_t>(v.selected)];
    }
    if (v.dragging && active) {
        float dx = (io.MousePos.x - v.pressAt.x) / ppm, dy = (io.MousePos.y - v.pressAt.y) / ppm;
        if (!io.KeyShift) dx = std::round(dx * 2) / 2, dy = std::round(dy * 2) / 2;  // half-mm steps (Shift: free)
        ImVec4 b = norm(v.start);
        if (v.handle < 0) {
            v.live = {v.start.x + dx, v.start.y + dy, v.start.z, v.start.w};
        } else {
            float x0 = b.x, y0 = b.y, x1 = b.x + b.z, y1 = b.y + b.w;
            const int h = v.handle;
            if (h == 0 || h == 6 || h == 7) x0 += dx;
            if (h == 2 || h == 3 || h == 4) x1 += dx;
            if (h == 0 || h == 1 || h == 2) y0 += dy;
            if (h == 4 || h == 5 || h == 6) y1 += dy;
            v.live = {std::min(x0, x1), std::min(y0, y1), std::max(1.0f, std::abs(x1 - x0)), std::max(1.0f, std::abs(y1 - y0))};
        }
    }
    if (v.dragging && !active) {
        v.dragging = false;
        const bool moved = std::abs(v.live.x - v.start.x) > 0.01f || std::abs(v.live.y - v.start.y) > 0.01f ||
                           std::abs(v.live.z - v.start.z) > 0.01f || std::abs(v.live.w - v.start.w) > 0.01f;
        if (moved && v.selected >= 0) {
            Params p{{"frame", v.ids[static_cast<std::size_t>(v.selected)]}, {"x", static_cast<double>(v.live.x)}, {"y", static_cast<double>(v.live.y)}};
            if (v.handle >= 0) p["w"] = static_cast<double>(v.live.z), p["h"] = static_cast<double>(v.live.w);
            if (send("layout.frame_set", p, v.handle >= 0 ? "Resize frame" : "Move frame")) v.boxes[static_cast<std::size_t>(v.selected)] = v.live;
        }
    }
    // double-click a text frame: type over it
    if (!v.editing && hovered && !space && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
        const int hit = frameAt(io.MousePos);
        if (hit >= 0 && v.types[static_cast<std::size_t>(hit)] == "text") {
            Result info = query("layout.info", {{"doc", currentDoc_}, {"frame", v.ids[static_cast<std::size_t>(hit)]}});
            if (info.ok) {
                v.selected = hit;
                v.editing = true;
                v.focus = true;
                v.text = v.original = textOf(info, "markup");
                v.editFrame = v.ids[static_cast<std::size_t>(hit)];
                ++v.generation;
            }
        }
    }
    // a click outside the box being typed in: keep the words
    if (v.editing && hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        const ImVec4 b = norm(v.boxes[static_cast<std::size_t>(std::max(0, v.selected))]);
        const ImVec2 a = toScreen(b.x, b.y), e = toScreen(b.x + b.z, b.y + b.w);
        if (io.MousePos.x < a.x || io.MousePos.x > e.x || io.MousePos.y < a.y || io.MousePos.y > e.y + 40) layoutCommit();
    }

    // keys (not while typing in a box or a form)
    const bool keys = !v.editing && v.selected >= 0 && !io.WantTextInput && (hovered || ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows));
    if (keys) {
        const float step = io.KeyShift ? 0.1f : 1.0f;
        float nx = 0, ny = 0;
        if (ImGui::IsKeyPressed(ImGuiKey_LeftArrow)) nx = -step;
        if (ImGui::IsKeyPressed(ImGuiKey_RightArrow)) nx = step;
        if (ImGui::IsKeyPressed(ImGuiKey_UpArrow)) ny = -step;
        if (ImGui::IsKeyPressed(ImGuiKey_DownArrow)) ny = step;
        if (nx != 0 || ny != 0) {
            ImVec4& b = v.boxes[static_cast<std::size_t>(v.selected)];
            if (send("layout.frame_set", {{"frame", v.ids[static_cast<std::size_t>(v.selected)]}, {"x", static_cast<double>(b.x + nx)}, {"y", static_cast<double>(b.y + ny)}}, "Nudge frame")) {
                b.x += nx, b.y += ny;
            }
        }
        if (ImGui::IsKeyPressed(ImGuiKey_Delete, false) || ImGui::IsKeyPressed(ImGuiKey_Backspace, false)) {
            if (send("layout.frame_delete", {{"frame", v.ids[static_cast<std::size_t>(v.selected)]}}, "Delete frame")) v.selected = -1;
        }
        if (ImGui::IsKeyPressed(ImGuiKey_Escape, false)) v.selected = -1;
    }
    if (!v.editing && hovered && !io.WantTextInput) {
        if (ImGui::IsKeyPressed(ImGuiKey_PageDown, false)) layoutShowPage(v.page + 1);
        if (ImGui::IsKeyPressed(ImGuiKey_PageUp, false)) layoutShowPage(v.page - 1);
    }

    // outlines: the one under the mouse, the picked one with handles, the ghost while dragging
    const int hover = hovered && !v.dragging && !v.editing ? frameAt(io.MousePos) : -1;
    if (hover >= 0 && hover != v.selected) {
        const ImVec4 b = norm(v.boxes[static_cast<std::size_t>(hover)]);
        dl->AddRect(toScreen(b.x, b.y), toScreen(b.x + b.z, b.y + b.w), ImGui::ColorConvertFloat4ToU32({c.x, c.y, c.z, 0.6f}), 0, 0, 1.5f);
    }
    if (v.selected >= 0 && v.selected < static_cast<int>(v.boxes.size()) && !v.editing) {
        const ImVec4 b = norm(v.boxes[static_cast<std::size_t>(v.selected)]);
        const ImVec2 a = toScreen(b.x, b.y), e = toScreen(b.x + b.z, b.y + b.w);
        dl->AddRect(a, e, accent, 0, 0, 2);
        if (v.types[static_cast<std::size_t>(v.selected)] != "line" && !v.dragging) {
            const ImVec2 pts[8] = {{a.x, a.y}, {(a.x + e.x) / 2, a.y}, {e.x, a.y}, {e.x, (a.y + e.y) / 2},
                                   {e.x, e.y}, {(a.x + e.x) / 2, e.y}, {a.x, e.y}, {a.x, (a.y + e.y) / 2}};
            for (const auto& pt : pts) {
                dl->AddRectFilled({pt.x - 4, pt.y - 4}, {pt.x + 4, pt.y + 4}, IM_COL32(255, 255, 255, 255));
                dl->AddRect({pt.x - 4, pt.y - 4}, {pt.x + 4, pt.y + 4}, accent, 0, 0, 1.5f);
            }
        }
        if (v.dragging) {
            const ImVec4 g = norm(v.live);
            const ImVec2 ga = toScreen(g.x, g.y), ge = toScreen(g.x + g.z, g.y + g.w);
            dl->AddRectFilled(ga, ge, ImGui::ColorConvertFloat4ToU32({c.x, c.y, c.z, 0.18f}));
            dl->AddRect(ga, ge, accent, 0, 0, 2);
            char label[96];
            std::snprintf(label, sizeof label, "x %.1f  y %.1f  %.1f x %.1f mm", g.x, g.y, g.z, g.w);
            const ImVec2 ts = ImGui::CalcTextSize(label);
            dl->AddRectFilled({ga.x, ga.y - ts.y - 6}, {ga.x + ts.x + 8, ga.y - 2}, IM_COL32(20, 20, 24, 220), 3);
            dl->AddText({ga.x + 4, ga.y - ts.y - 4}, IM_COL32(255, 255, 255, 255), label);
        }
    }

    // the page bar: which page, and what the mouse does
    ImGui::SetNextWindowPos({canvas_.viewMin.x + 8, canvas_.viewMin.y + canvas_.viewSize.y - 8}, ImGuiCond_Always, {0, 1});
    ImGui::SetNextWindowBgAlpha(0.85f);
    if (ImGui::Begin("##layoutbar", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                                                 ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoFocusOnAppearing)) {
        ImGui::BeginDisabled(v.page <= 1);
        if (ImGui::SmallButton("<")) layoutShowPage(v.page - 1);
        ImGui::EndDisabled();
        ImGui::SameLine();
        ImGui::Text("page %d of %d", v.page, std::max(1, v.pages));
        ImGui::SameLine();
        ImGui::BeginDisabled(v.page >= v.pages);
        if (ImGui::SmallButton(">")) layoutShowPage(v.page + 1);
        ImGui::EndDisabled();
        ImGui::SameLine();
        if (v.selected >= 0 && v.selected < static_cast<int>(v.ids.size())) {
            const std::size_t s = static_cast<std::size_t>(v.selected);
            ImGui::TextDisabled("%s frame %s: drag to move, handles resize%s, arrows nudge, Delete removes", v.types[s].c_str(), v.ids[s].c_str(),
                                v.types[s] == "text" ? ", double-click to type" : "");
        } else {
            ImGui::TextDisabled("click a frame to pick it, double-click text to type");
        }
    }
    ImGui::End();

    if (!v.editing || v.selected < 0) return;
    // the text box right over the frame, in the page's colours
    const ImVec4 b = norm(v.boxes[static_cast<std::size_t>(v.selected)]);
    const ImVec2 a = toScreen(b.x, b.y);
    const float bw = std::max(80.0f, b.z * ppm), bh = std::max(ImGui::GetTextLineHeight() * 3, b.w * ppm);
    ImGui::SetNextWindowPos(a);
    ImGui::SetNextWindowSize({bw, bh + ImGui::GetFrameHeight() + 6});
    ImGui::PushStyleColor(ImGuiCol_WindowBg, IM_COL32(255, 255, 255, 255));
    ImGui::PushStyleColor(ImGuiCol_FrameBg, IM_COL32(255, 255, 255, 255));
    ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(20, 20, 24, 255));
    ImGui::PushStyleColor(ImGuiCol_Border, accent);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {2, 2});
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 2);
    bool commit = false, cancel = false;
    if (ImGui::Begin("##layoutinplace", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings)) {
        if (v.focus) {
            ImGui::SetKeyboardFocusHere();
            v.focus = false;
        }
        // read before the text box takes the keys for itself
        if (io.KeyCtrl && (ImGui::IsKeyPressed(ImGuiKey_Enter, false) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter, false))) commit = true;
        if (ImGui::IsKeyPressed(ImGuiKey_Escape, false)) cancel = true;
        const std::string typed = v.text;
        ImGui::PushID(v.generation);
        ImGui::InputTextMultiline("##frametext", &v.text, {-1, bh - 4}, commit ? ImGuiInputTextFlags_ReadOnly : 0);
        ImGui::PopID();
        if (commit) v.text = typed;  // not the new line Ctrl+Enter would add
        if (ImGui::SmallButton("Done")) commit = true;
        ImGui::SameLine();
        if (ImGui::SmallButton("Cancel")) cancel = true;
        ImGui::SameLine();
        ImGui::TextUnformatted("a paragraph a line, **bold**, *italic*. Ctrl+Enter: done");
    }
    ImGui::End();
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor(4);
    if (cancel) v.editing = false;
    else if (commit) layoutCommit();
}

// Puts the typed words into the frame (one undo step) and closes the box.
void Shell::layoutCommit() {
    LayoutView& v = page_;
    if (!v.editing) return;
    v.editing = false;
    if (v.text == v.original || v.editFrame.empty()) return;
    Result r = run("layout.frame_text", {{"doc", currentDoc_}, {"frame", v.editFrame}, {"text", v.text}});
    resultFor_ = "Type";
    r.message = r.ok ? sentence(r.message) : friendlyError(r.message);
    result_ = std::move(r);
    canvas_.dirty = true;
}

}  // namespace wetwaresuite::gui
