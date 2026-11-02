// The dope sheet under a model's 3D view, like Blender's: a frame ruler with the playhead
// (click or drag it to scrub), a row per keyed object and per channel group with a
// diamond on every key. Drag a diamond to move those keys (Alt+drag copies), click to pick
// it and Delete removes it, I keys the selected objects at the playhead, and Play loops the
// frame range. Every edit is one ordinary model.* command; scrubbing and playback use
// model.scrub, which isn't an undo step.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <sstream>

#include "shell.hpp"

namespace wetwaresuite::gui {

namespace {

double nowS() { return static_cast<double>(SDL_GetTicks()) / 1000.0; }

std::vector<int> parseFrames(const std::string& s) {
    std::vector<int> out;
    std::stringstream in(s);
    std::string part;
    while (std::getline(in, part, ',')) {
        if (!part.empty()) out.push_back(std::atoi(part.c_str()));
    }
    return out;
}

std::int64_t intField(const Result& r, const char* key, std::int64_t fallback) {
    const Value* v = r.field(key);
    if (!v) return fallback;
    if (auto i = std::get_if<std::int64_t>(v)) return *i;
    if (auto d = std::get_if<double>(v)) return static_cast<std::int64_t>(*d);
    return fallback;
}

}  // namespace

float Shell::dopeSheetHeight() const {
    const float line = ImGui::GetFrameHeightWithSpacing();
    if (!dope_.open) return line + 4;
    const std::size_t rows = std::max<std::size_t>(dope_.rows.size(), 1);
    return line * 2 + std::min<float>(static_cast<float>(rows), 8.0f) * ImGui::GetFontSize() * 1.4f + 10;
}

void Shell::dopeSheetRefresh(bool force) {
    DopeSheet& d = dope_;
    if (d.doc != currentDoc_) {
        d = DopeSheet{};
        d.doc = currentDoc_;
        force = true;
    }
    const double t = nowS();
    if (!force && t - d.lastQuery < 0.3) return;
    d.lastQuery = t;
    const Result r = query("model.keys", {{"doc", currentDoc_}});
    if (!r.ok) return;
    d.frame = static_cast<int>(intField(r, "frame", d.frame));
    d.start = static_cast<int>(intField(r, "start", d.start));
    d.end = static_cast<int>(intField(r, "end", d.end));
    if (const Value* v = r.field("fps"))
        if (auto f = std::get_if<double>(v)) d.fps = *f;
    d.rows.clear();
    const Value* rows = r.field("rows");
    const std::string text = rows ? toText(*rows) : std::string();
    std::stringstream in(text);
    std::string line;
    while (std::getline(in, line)) {
        const auto a = line.find('\t');
        const auto b = a == std::string::npos ? a : line.find('\t', a + 1);
        if (b == std::string::npos) continue;
        DopeSheet::Row row;
        row.object = line.substr(0, a);
        row.group = line.substr(a + 1, b - a - 1);
        row.frames = parseFrames(line.substr(b + 1));
        d.rows.push_back(std::move(row));
    }
}

void Shell::dopeSheetScrub(int frame, bool wrap) {
    const Result r = run("model.scrub", {{"doc", currentDoc_}, {"frame", std::int64_t{frame}}, {"wrap", wrap}});
    if (r.ok) dope_.frame = static_cast<int>(intField(r, "frame", frame));
    canvas_.orbitDirty = true;
}

void Shell::dopeSheetView() {
    dopeSheetRefresh(false);
    DopeSheet& d = dope_;
    ImGuiIO& io = ImGui::GetIO();

    // toolbar: open/close, play, frame, range, key
    if (ImGui::ArrowButton("##dopeopen", d.open ? ImGuiDir_Down : ImGuiDir_Right)) d.open = !d.open;
    ImGui::SetItemTooltip(d.open ? "Hide the dope sheet" : "Show the dope sheet");
    ImGui::SameLine();
    if (ImGui::Button(d.playing ? "Pause" : "Play") || (!io.WantTextInput && io.KeyAlt && ImGui::IsKeyPressed(ImGuiKey_A, false))) {
        d.playing = !d.playing;
        d.lastStep = nowS();
    }
    ImGui::SetItemTooltip("Play the frame range on a loop (Alt+A)");
    ImGui::SameLine();
    int f = d.frame;
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 7.5f);
    if (ImGui::InputInt("##frame", &f, 1, 10) && f != d.frame) dopeSheetScrub(f, false);
    ImGui::SetItemTooltip("Current frame");
    ImGui::SameLine();
    int range[2] = {d.start, d.end};
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 7);
    if (ImGui::InputInt2("##range", range, ImGuiInputTextFlags_EnterReturnsTrue) && range[1] >= range[0]) {
        run("model.frame", {{"doc", currentDoc_}, {"start", std::int64_t{range[0]}}, {"end", std::int64_t{range[1]}}});
        dopeSheetRefresh(true);
    }
    ImGui::SetItemTooltip("First and last frame");
    ImGui::SameLine();
    if (ImGui::Button("Key") || (!io.WantTextInput && ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows | ImGuiHoveredFlags_RootWindow) &&
                                 ImGui::IsKeyPressed(ImGuiKey_I, false) && !io.KeyCtrl)) {
        const Result r = run("model.keyframe", {{"doc", currentDoc_}, {"frame", std::int64_t{d.frame}}});
        if (!r.ok) result_ = Result::failure(friendlyError(r.message));
        dopeSheetRefresh(true);
    }
    ImGui::SetItemTooltip("Key the selected objects' location, rotation and scale at this frame (I)");
    ImGui::SameLine();
    ImGui::BeginDisabled(d.selFrame < 0);
    if (ImGui::Button("Delete key") ||
        (d.selFrame >= 0 && !io.WantTextInput && (ImGui::IsKeyPressed(ImGuiKey_Delete, false) || ImGui::IsKeyPressed(ImGuiKey_X, false)) &&
         d.hovered)) {
        const Result r = run("model.key_delete", {{"doc", currentDoc_}, {"object", d.selObject}, {"frame", std::int64_t{d.selFrame}}, {"row", d.selGroup}});
        if (!r.ok) result_ = Result::failure(friendlyError(r.message));
        d.selFrame = -1;
        dopeSheetRefresh(true);
        canvas_.orbitDirty = true;
    }
    ImGui::EndDisabled();
    ImGui::SetItemTooltip("Remove the picked key (Delete or X)");

    // playback
    if (d.playing) {
        const double now = nowS();
        const double step = 1.0 / std::max(1.0, d.fps);
        if (now - d.lastStep >= step) {
            const int adv = std::max(1, static_cast<int>((now - d.lastStep) / step));
            d.lastStep += adv * step;
            if (now - d.lastStep > 0.5) d.lastStep = now;  // fell far behind: don't race to catch up
            dopeSheetScrub(d.frame + adv, true);
        }
    }
    if (!d.open) return;

    // the sheet
    const float font = ImGui::GetFontSize();
    const float labelW = font * 10;
    const float rulerH = font * 1.3f;
    const float rowH = font * 1.4f;
    ImVec2 o = ImGui::GetCursorScreenPos();
    ImVec2 size = ImGui::GetContentRegionAvail();
    size.y = std::max(size.y, rulerH + rowH);
    ImGui::InvisibleButton("##dopesheet", size, ImGuiButtonFlags_MouseButtonLeft);
    d.hovered = ImGui::IsItemHovered();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImU32 bg = ImGui::GetColorU32(ImGuiCol_FrameBg);
    const ImU32 lane = ImGui::GetColorU32(ImGuiCol_WindowBg);
    const ImU32 text = ImGui::GetColorU32(ImGuiCol_Text);
    const ImU32 muted = ImGui::GetColorU32(ImGuiCol_TextDisabled);
    const ImU32 accent = ImGui::ColorConvertFloat4ToU32(accent_);
    dl->AddRectFilled(o, {o.x + size.x, o.y + size.y}, lane);

    const int first = std::min(d.start, d.frame) - 1;
    const int last = std::max(d.end, d.frame) + 1;
    const float x0 = o.x + labelW, x1 = o.x + size.x - font * 0.5f;
    const float pxPerFrame = std::max(1.0f, (x1 - x0) / static_cast<float>(std::max(1, last - first)));
    auto xOf = [&](double frame) { return x0 + static_cast<float>(frame - first) * pxPerFrame; };
    auto frameAt = [&](float x) { return static_cast<int>(std::lround(first + (x - x0) / pxPerFrame)); };

    // ruler, with the range shaded
    dl->AddRectFilled({x0, o.y}, {x1, o.y + rulerH}, bg);
    dl->AddRectFilled({xOf(d.start - 0.5), o.y + rulerH}, {xOf(d.end + 0.5), o.y + size.y}, (accent & 0x00FFFFFF) | 0x10000000u);
    int every = 1;
    for (int s : {1, 2, 5, 10, 20, 50, 100, 200, 500, 1000}) {
        every = s;
        if (pxPerFrame * s >= font * 2.6f) break;
    }
    for (int fr = (first / every) * every; fr <= last; fr += every) {
        if (fr < first) continue;
        const float x = xOf(fr);
        char buf[16];
        std::snprintf(buf, sizeof buf, "%d", fr);
        dl->AddLine({x, o.y + rulerH * 0.6f}, {x, o.y + rulerH}, muted);
        dl->AddLine({x, o.y + rulerH}, {x, o.y + size.y}, (muted & 0x00FFFFFF) | 0x30000000u);
        dl->AddText({x + 2, o.y + 1}, muted, buf);
    }

    // rows
    std::size_t maxRows = static_cast<std::size_t>(std::max(1.0f, (size.y - rulerH) / rowH));
    const float diamond = font * 0.32f;
    int hoverRow = -1, hoverFrame = -1;
    const ImVec2 mouse = io.MousePos;
    if (d.rows.empty()) dl->AddText({o.x + font * 0.5f, o.y + rulerH + 4}, muted, "No keys yet: select something and press Key (I) to key it here");
    for (std::size_t i = 0; i < d.rows.size() && i < maxRows; ++i) {
        const DopeSheet::Row& row = d.rows[i];
        const float y = o.y + rulerH + static_cast<float>(i) * rowH;
        const bool summary = row.group == "all";
        if (summary) dl->AddRectFilled({o.x, y}, {o.x + size.x, y + rowH}, (bg & 0x00FFFFFF) | 0x90000000u);
        const std::string label = summary ? row.object : "   " + row.group;
        dl->PushClipRect({o.x, y}, {x0 - 4, y + rowH}, true);
        dl->AddText({o.x + font * 0.4f, y + (rowH - font) / 2}, summary ? text : muted, label.c_str());
        dl->PopClipRect();
        const float cy = y + rowH / 2;
        for (int fr : row.frames) {
            const bool dragged = d.dragging && d.dragRow == static_cast<int>(i) && fr == d.selFrame;
            const float cx = xOf(dragged ? d.dragTo : fr);
            const bool picked = d.selObject == row.object && d.selGroup == row.group && d.selFrame == fr;
            const float r = summary ? diamond * 1.15f : diamond;
            const ImVec2 pts[4] = {{cx, cy - r}, {cx + r, cy}, {cx, cy + r}, {cx - r, cy}};
            dl->AddConvexPolyFilled(pts, 4, picked ? accent : (summary ? text : muted));
            dl->AddPolyline(pts, 4, lane, ImDrawFlags_Closed, 1.0f);
            if (d.hovered && std::fabs(mouse.x - cx) <= r + 2 && std::fabs(mouse.y - cy) <= rowH / 2) hoverRow = static_cast<int>(i), hoverFrame = fr;
        }
    }

    // playhead
    const float px = xOf(d.frame);
    dl->AddLine({px, o.y}, {px, o.y + size.y}, accent, 2.0f);
    {
        char buf[16];
        std::snprintf(buf, sizeof buf, "%d", d.frame);
        const ImVec2 ts = ImGui::CalcTextSize(buf);
        dl->AddRectFilled({px - ts.x / 2 - 3, o.y}, {px + ts.x / 2 + 3, o.y + rulerH}, accent, 3);
        dl->AddText({px - ts.x / 2, o.y + (rulerH - ts.y) / 2}, ImGui::ColorConvertFloat4ToU32(accentText_), buf);
    }

    // mouse
    if (ImGui::IsItemActivated()) {
        if (mouse.y < o.y + rulerH && mouse.x >= x0) {
            d.scrubbing = true;
        } else if (hoverRow >= 0) {
            const DopeSheet::Row& row = d.rows[static_cast<std::size_t>(hoverRow)];
            d.selObject = row.object;
            d.selGroup = row.group;
            d.selFrame = hoverFrame;
            d.dragRow = hoverRow;
            d.dragTo = hoverFrame;
            d.dragging = true;
            d.dragCopy = io.KeyAlt;
        } else {
            d.selFrame = -1;
            if (mouse.x >= x0) d.scrubbing = true;  // clicking an empty lane moves the playhead too
        }
    }
    if (d.scrubbing) {
        if (ImGui::IsItemActive()) {
            const int target = std::clamp(frameAt(mouse.x), first, last);
            if (target != d.frame) dopeSheetScrub(target, false);
        } else {
            d.scrubbing = false;
        }
    }
    if (d.dragging) {
        if (ImGui::IsItemActive()) {
            d.dragTo = std::clamp(frameAt(mouse.x), first - 1000, last + 1000);
            ImGui::SetTooltip("frame %d", d.dragTo);
        } else {
            d.dragging = false;
            if (d.dragTo != d.selFrame) {
                Params p{{"doc", currentDoc_}, {"object", d.selObject}, {"frame", std::int64_t{d.selFrame}}, {"to", std::int64_t{d.dragTo}},
                         {"row", d.selGroup}};
                if (d.dragCopy) p["copy"] = true;
                const Result r = run("model.key_move", p);
                if (!r.ok) result_ = Result::failure(friendlyError(r.message));
                else d.selFrame = d.dragTo;
                canvas_.orbitDirty = true;
            }
            dopeSheetRefresh(true);
        }
    }
    if (hoverRow >= 0 && !d.dragging) ImGui::SetTooltip("frame %d: drag to move, Alt+drag to copy, Delete removes", hoverFrame);
}

}  // namespace wetwaresuite::gui
