// Godot-style scenes on the canvas: the viewport with Select (Q), Move (W), Rotate (E) and
// Scale (S) tools, a Scene dock (the node tree: add, delete, duplicate, reorder, rename,
// attach a script) and an Inspector for the selected node. Everything goes through scene.*
// commands, so each drag or edit is one undo step and a script sees the same thing.
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cctype>
#include <cstdlib>
#include <iterator>
#include <sstream>
#include <string>
#include <vector>

#include "shell.hpp"
#include "imgui_stdlib.h"
#include "tool_panels.hpp"

namespace wetwaresuite::gui {

namespace {

constexpr double kPi = 3.14159265358979323846;

std::vector<std::string> split(const std::string& text, char sep) {
    std::vector<std::string> out;
    if (text.empty()) return out;
    std::stringstream ss(text);
    std::string item;
    while (std::getline(ss, item, sep)) out.push_back(item);
    if (!text.empty() && text.back() == sep) out.emplace_back();
    return out;
}

std::vector<float> floats(const std::string& text) {
    std::vector<float> out;
    const char* p = text.c_str();
    while (*p) {
        char* end = nullptr;
        const double v = std::strtod(p, &end);
        if (end == p) {
            ++p;
            continue;
        }
        out.push_back(static_cast<float>(v));
        p = end;
    }
    return out;
}

std::string num(double v) {
    if (std::abs(v) < 1e-6) v = 0;
    char b[32];
    std::snprintf(b, sizeof b, "%.4g", v);
    return b;
}

std::string leaf(const std::string& path) {
    const auto slash = path.rfind('/');
    return slash == std::string::npos ? path : path.substr(slash + 1);
}

std::string parentPath(const std::string& path) {
    const auto slash = path.rfind('/');
    return slash == std::string::npos ? std::string(".") : path.substr(0, slash);
}

int depthOf(const std::string& path) {
    if (path == ".") return 0;
    return 1 + static_cast<int>(std::count(path.begin(), path.end(), '/'));
}

bool sceneTool(GameTool t) { return t == GameTool::Select || t == GameTool::Translate || t == GameTool::Rotate || t == GameTool::Scale; }

}  // namespace

void Shell::sceneSync(bool newDoc) {
    GameCanvas& g = canvas_.game;
    SceneCanvas& s = g.scene;
    if (newDoc) {
        g.tool = GameTool::Select;
        if (s.playing && s.navKeys) ImGui::GetIO().ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
        s.playing = false;  // a play test belongs to the scene it started on
        s.playTex.reset();
    }
    Result r = query("scene.tree", {{"doc", currentDoc_}});
    if (!r.ok) return;
    s.paths = split(tp::text(r, "nodes"), ',');
    s.types = split(tp::text(r, "types"), ',');
    s.types.resize(s.paths.size());
    s.dims = static_cast<int>(tp::number(r, "dims", 2));
    if (std::find(s.paths.begin(), s.paths.end(), s.selected) == s.paths.end()) s.selected = ".";
    s.inspectedFor.clear();
    if (s.classes2.empty()) {
        s.classes2 = split(tp::text(query("scene.classes", {{"dims", std::int64_t{2}}}), "classes"), ',');
        s.classes3 = split(tp::text(query("scene.classes", {{"dims", std::int64_t{3}}}), "classes"), ',');
    }
}

void Shell::sceneSelect(const std::string& path) {
    SceneCanvas& s = canvas_.game.scene;
    if (s.selected == path) return;
    s.selected = path.empty() ? "." : path;
    s.inspectedFor.clear();
    canvas_.dirty = true;  // the highlight moves
}

std::string Shell::scenePicture() {
    SceneCanvas& s = canvas_.game.scene;
    s.haveBox = false;
    Result r;
    if (s.dims == 3) {
        r = query("scene.preview", {{"doc", currentDoc_},
                                    {"view", std::string("3d")},
                                    {"width", std::int64_t{1200}},
                                    {"height", std::int64_t{800}},
                                    {"angle", s.angle},
                                    {"tilt", s.tilt},
                                    {"camera", s.camera},
                                    {"selected", s.selected}});
    } else {
        if (!s.viewSet) {
            // fit once, then keep that view with room around it, so things can be dragged about
            Result fit = query("scene.preview", {{"doc", currentDoc_}, {"view", std::string("2d")}, {"size", std::int64_t{1400}}});
            if (!fit.ok) return {};
            const double w = tp::number(fit, "width", 800), h = tp::number(fit, "height", 600);
            s.scale = std::max(1e-4, tp::number(fit, "scale", 1));
            s.originX = tp::number(fit, "origin_x") - w * 0.25 / s.scale;
            s.originY = tp::number(fit, "origin_y") - h * 0.25 / s.scale;
            s.viewW = static_cast<int>(w * 1.5);
            s.viewH = static_cast<int>(h * 1.5);
            s.viewSet = true;
            canvas_.fit = true;
        }
        r = query("scene.preview", {{"doc", currentDoc_},
                                    {"view", std::string("2d")},
                                    {"origin_x", s.originX},
                                    {"origin_y", s.originY},
                                    {"scale", s.scale},
                                    {"width", static_cast<std::int64_t>(s.viewW)},
                                    {"height", static_cast<std::int64_t>(s.viewH)},
                                    {"helpers", s.helpers},
                                    {"selected", s.selected}});
        if (r.ok && std::any_of(r.data.begin(), r.data.end(), [](const auto& f) { return f.first == "sel_x0"; })) {
            s.haveBox = true;
            s.box[0] = tp::number(r, "sel_x0");
            s.box[1] = tp::number(r, "sel_y0");
            s.box[2] = tp::number(r, "sel_x1");
            s.box[3] = tp::number(r, "sel_y1");
        }
    }
    if (!r.ok) return {};
    for (const auto& a : r.attachments) {
        if (a.mime == "image/png") return a.bytes;
    }
    return {};
}

void Shell::sceneTools() {
    GameCanvas& g = canvas_.game;
    struct Item {
        const char* name;
        GameTool tool;
        const char* key;
        const char* tip;
    };
    // Godot's letters
    static const Item items[] = {
        {"Select", GameTool::Select, "Q", "Click a node to select it; drag to move it (3D: drag to orbit)"},
        {"Move", GameTool::Translate, "W", "Drag to move the selected node (Ctrl snaps to 8 px, 3D: Shift moves up and down)"},
        {"Rotate", GameTool::Rotate, "E", "Drag around the selected node to turn it (Ctrl snaps to 15 degrees)"},
        {"Scale", GameTool::Scale, "S", "Drag away from or towards the selected node to resize it"},
        {"Pan", GameTool::Move, "V", "Drag to move around, scroll to zoom"}};
    for (const auto& it : items) {
        if (railItem(it.name, it.key, it.tip, g.tool == it.tool)) {
            g.tool = it.tool;
            g.dragging = false;
        }
    }
}

void Shell::sceneToolbar() {
    SceneCanvas& s = canvas_.game.scene;
    ImGui::SameLine();
    if (s.dims == 3) {
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 6);
        float a = static_cast<float>(s.angle), t = static_cast<float>(s.tilt);
        if (ImGui::SliderFloat("##angle", &a, -180, 180, "turn %.0f")) {
            s.angle = a;
            canvas_.dirty = true;
        }
        ImGui::SameLine();
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 6);
        if (ImGui::SliderFloat("##tilt", &t, -89, 89, "tilt %.0f")) {
            s.tilt = t;
            canvas_.dirty = true;
        }
        ImGui::SameLine();
        if (ImGui::Checkbox("Camera view", &s.camera)) canvas_.dirty = true;
        ImGui::SetItemTooltip("Look through the scene's Camera3D (Godot's Preview)");
        return;
    }
    if (ImGui::Button(s.playing ? "Stop" : "Play")) scenePlayToggle();
    ImGui::SetItemTooltip(s.playing ? "Stop the play test (F5 or Esc)" : "Play the scene here (F5): arrows move, Space jumps. Starter scripts, physics bodies, tiles and pickups work; for anything else press Play in Godot");
    if (s.playing) {
        ImGui::SameLine();
        ImGui::TextDisabled("arrows move, Space jumps, Esc stops");
        return;
    }
    ImGui::SameLine();
    if (ImGui::Button("Frame all")) {
        s.viewSet = false;
        canvas_.dirty = true;
    }
    ImGui::SetItemTooltip("Fit the view to everything in the scene");
    ImGui::SameLine();
    if (ImGui::Checkbox("Helpers", &s.helpers)) canvas_.dirty = true;
    ImGui::SetItemTooltip("Collision shapes, cameras and markers");
}

void Shell::scenePlayToggle() {
    SceneCanvas& s = canvas_.game.scene;
    ImGuiIO& io = ImGui::GetIO();
    if (s.playing) {
        query("scene.play_step", {{"doc", currentDoc_}, {"stop", true}});
        s.playing = false;
        if (s.navKeys) io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
        s.playTex.reset();
        canvas_.dirty = true;
        return;
    }
    s.playLog.clear();
    s.playNotes.clear();
    Result r = query("scene.play_step", {{"doc", currentDoc_}, {"restart", true}, {"seconds", 0.0}, {"raw", true}});
    if (!r.ok) {
        result_ = r;
        resultFor_ = "Play";
        return;
    }
    s.playNotes = tp::text(r, "notes");
    s.playing = true;
    s.navKeys = (io.ConfigFlags & ImGuiConfigFlags_NavEnableKeyboard) != 0;
    io.ConfigFlags &= ~ImGuiConfigFlags_NavEnableKeyboard;  // arrows and Space drive the game, not the buttons
    s.playLast = ImGui::GetTime();
}

// One frame of the play test: keys in, the game window's picture out, fitted into the viewport.
void Shell::scenePlayFrame(ImDrawList* dl) {
    SceneCanvas& s = canvas_.game.scene;
    ImGuiIO& io = ImGui::GetIO();
    if (ImGui::IsKeyPressed(ImGuiKey_Escape, false) || ImGui::IsKeyPressed(ImGuiKey_F5, false)) {
        scenePlayToggle();
        return;
    }
    const double now = ImGui::GetTime();
    const double dt = std::clamp(now - s.playLast, 0.0, 0.1);
    s.playLast = now;
    std::string keys;
    if (!io.WantTextInput) {
        if (ImGui::IsKeyDown(ImGuiKey_LeftArrow)) keys += "left,";
        if (ImGui::IsKeyDown(ImGuiKey_RightArrow)) keys += "right,";
        if (ImGui::IsKeyDown(ImGuiKey_UpArrow)) keys += "up,";
        if (ImGui::IsKeyDown(ImGuiKey_DownArrow)) keys += "down,";
        if (ImGui::IsKeyDown(ImGuiKey_Space) || ImGui::IsKeyDown(ImGuiKey_Enter)) keys += "jump,";
    }
    const ImVec2 v0 = canvas_.viewMin, size = canvas_.viewSize;
    const double fit = std::min(size.x / 1152.0, size.y / 648.0);
    Result r = query("scene.play_step", {{"doc", currentDoc_}, {"keys", keys}, {"seconds", dt}, {"raw", true}, {"scale", std::clamp(fit, 0.25, 2.0)}});
    animating_ = true;
    if (!r.ok) {
        s.playing = false;
        if (s.navKeys) io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
        result_ = r;
        resultFor_ = "Play";
        return;
    }
    const int w = static_cast<int>(tp::number(r, "width")), h = static_cast<int>(tp::number(r, "height"));
    for (const auto& a : r.attachments) {
        if (a.mime == "image/x-rgba" && w > 0 && h > 0 && a.bytes.size() == static_cast<std::size_t>(w) * h * 4)
            s.playTex.upload(renderer_, reinterpret_cast<const std::uint8_t*>(a.bytes.data()), w, h);
    }
    const std::string ev = tp::text(r, "events");
    for (const auto& line : split(ev, '\n')) {
        if (line.empty()) continue;
        s.playLog.push_back(line);
        if (s.playLog.size() > 6) s.playLog.erase(s.playLog.begin());
    }
    dl->AddRectFilled(v0, {v0.x + size.x, v0.y + size.y}, IM_COL32(20, 20, 22, 255));
    if (s.playTex.tex && s.playTex.srcWidth > 0) {
        const float k = std::min(size.x / static_cast<float>(s.playTex.srcWidth), size.y / static_cast<float>(s.playTex.srcHeight));
        const ImVec2 dims{s.playTex.srcWidth * k, s.playTex.srcHeight * k};
        const ImVec2 p0{v0.x + (size.x - dims.x) / 2, v0.y + (size.y - dims.y) / 2};
        dl->AddImage(s.playTex.id(), p0, {p0.x + dims.x, p0.y + dims.y});
    }
    float y = v0.y + 8;
    const ImU32 ink = IM_COL32(240, 240, 240, 230), shade = IM_COL32(0, 0, 0, 170);
    auto line = [&](const std::string& text, ImU32 col) {
        const ImVec2 ts = ImGui::CalcTextSize(text.c_str());
        dl->AddRectFilled({v0.x + 6, y - 2}, {v0.x + 14 + ts.x, y + ts.y + 2}, shade, 3);
        dl->AddText({v0.x + 10, y}, col, text.c_str());
        y += ts.y + 6;
    };
    char head[64];
    std::snprintf(head, sizeof head, "playing  %.1f s   (Esc stops)", tp::number(r, "time"));
    line(head, ink);
    for (const auto& n : split(s.playNotes, '\n')) {
        if (!n.empty()) line("note: " + n, IM_COL32(255, 200, 120, 230));
    }
    for (const auto& e : s.playLog) line(e, IM_COL32(170, 220, 255, 230));
}

void Shell::sceneInput(ImDrawList* dl, ImVec2 origin, bool hovered, bool active) {
    GameCanvas& g = canvas_.game;
    SceneCanvas& s = g.scene;
    ImGuiIO& io = ImGui::GetIO();
    if (s.playing) {
        scenePlayFrame(dl);
        return;
    }
    if (s.dims != 3 && !io.WantTextInput && ImGui::IsKeyPressed(ImGuiKey_F5, false)) {
        scenePlayToggle();
        return;
    }
    const float z = canvas_.zoom;
    const ImU32 accent = ImGui::ColorConvertFloat4ToU32(accent_);
    const ImVec2 mouse = io.MousePos;

    // keys, like Godot: Delete removes, Ctrl+D duplicates, F frames
    if (!io.WantTextInput && ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows)) {
        if (s.selected != "." && ImGui::IsKeyPressed(ImGuiKey_Delete, false)) {
            gameRun("scene.remove", {{"node", s.selected}}, "Delete node");
            sceneSelect(parentPath(s.selected));
        }
        if (s.selected != "." && io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_D, false)) {
            gameRun("scene.duplicate", {{"node", s.selected}}, "Duplicate node");
            if (result_ && result_->ok) sceneSelect(tp::text(*result_, "node"));
        }
        if (!io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_F, false) && s.dims != 3) {
            s.viewSet = false;
            canvas_.dirty = true;
        }
    }

    if (s.dims == 3) {
        // no picking in 3D: select in the Scene dock; Select orbits, Move/Rotate/Scale act on the selection
        if (hovered && ImGui::IsItemActivated() && ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
            g.dragging = true;
            g.dragFrom = mouse;
        }
        if (!g.dragging) return;
        const float dx = (mouse.x - g.dragFrom.x) / z, dy = (mouse.y - g.dragFrom.y) / z;
        if (g.tool == GameTool::Select) {
            s.angle = std::fmod(s.angle - io.MouseDelta.x * 0.4 + 540.0, 360.0) - 180.0;
            s.tilt = std::clamp(s.tilt + io.MouseDelta.y * 0.4, -89.0, 89.0);
            canvas_.dirty = true;
            if (!active) g.dragging = false;
            return;
        }
        dl->AddLine(g.dragFrom, mouse, accent, 2);
        if (active) return;
        g.dragging = false;
        if (s.selected == "." && g.tool != GameTool::Translate) return;
        const double yaw = s.angle * kPi / 180;
        const double metres = 0.015;  // per picture pixel: about right for the default framing
        Params p{{"node", s.selected}};
        if (g.tool == GameTool::Translate) {
            if (io.KeyShift) {
                p["dy"] = -dy * metres;
            } else {
                // drag right moves along the view's right, drag up moves away from the viewer, on the ground
                const double rx = std::cos(yaw), rz = -std::sin(yaw), fx = -std::sin(yaw), fz = -std::cos(yaw);
                p["dx"] = (dx * rx - dy * fx) * metres;
                p["dz"] = (dx * rz - dy * fz) * metres;
            }
            gameRun("scene.transform", p, "Move node");
        } else if (g.tool == GameTool::Rotate) {
            double deg = -dx * 0.5;
            if (io.KeyCtrl) deg = std::round(deg / 15) * 15;
            p["rotate"] = deg;
            p["axis"] = std::string("y");
            gameRun("scene.transform", p, "Rotate node");
        } else if (g.tool == GameTool::Scale) {
            p["scale"] = std::clamp(std::exp(-dy * 0.01), 0.05, 20.0);
            gameRun("scene.transform", p, "Scale node");
        }
        return;
    }

    // 2D: picture pixels <-> world
    auto toWorld = [&](ImVec2 screen) {
        const ImVec2 img = toImage(screen);
        return std::array<double, 2>{s.originX + img.x / s.scale, s.originY + img.y / s.scale};
    };
    auto toScreen = [&](double wx, double wy) {
        return ImVec2{origin.x + static_cast<float>((wx - s.originX) * s.scale) * z, origin.y + static_cast<float>((wy - s.originY) * s.scale) * z};
    };
    const double perPx = 1.0 / std::max(1e-6, s.scale * z);  // world units per screen pixel
    const ImVec2 centre = s.haveBox ? toScreen((s.box[0] + s.box[2]) / 2, (s.box[1] + s.box[3]) / 2) : mouse;

    if (hovered && sceneTool(g.tool) && ImGui::IsItemActivated() && ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
        const auto w = toWorld(mouse);
        const bool inSelected = s.haveBox && s.selected != "." && w[0] >= s.box[0] - 6 * perPx && w[0] <= s.box[2] + 6 * perPx &&
                                w[1] >= s.box[1] - 6 * perPx && w[1] <= s.box[3] + 6 * perPx;
        // Select and Move pick what is under the mouse first, unless that's the selection already
        if (!inSelected && (g.tool == GameTool::Select || g.tool == GameTool::Translate || s.selected == ".")) {
            Result hit = query("scene.hit", {{"doc", currentDoc_}, {"x", w[0]}, {"y", w[1]}, {"radius", 5 * perPx}});
            const std::string node = hit.ok ? tp::text(hit, "node") : std::string();
            sceneSelect(node);
            g.dragging = !node.empty();
        } else {
            g.dragging = s.selected != ".";
        }
        g.dragFrom = mouse;
    }
    if (g.dragging && s.haveBox) {
        const float dx = mouse.x - g.dragFrom.x, dy = mouse.y - g.dragFrom.y;
        const ImVec2 b0 = toScreen(s.box[0], s.box[1]), b1 = toScreen(s.box[2], s.box[3]);
        double wdx = dx * perPx, wdy = dy * perPx;
        if (io.KeyCtrl) {
            wdx = std::round(wdx / 8) * 8;
            wdy = std::round(wdy / 8) * 8;
        }
        const double a0 = std::atan2(g.dragFrom.y - centre.y, g.dragFrom.x - centre.x), a1 = std::atan2(mouse.y - centre.y, mouse.x - centre.x);
        double deg = (a1 - a0) * 180 / kPi;
        if (deg > 180) deg -= 360;
        if (deg < -180) deg += 360;
        if (io.KeyCtrl) deg = std::round(deg / 15) * 15;
        const double r0 = std::hypot(g.dragFrom.x - centre.x, g.dragFrom.y - centre.y), r1 = std::hypot(mouse.x - centre.x, mouse.y - centre.y);
        const double factor = std::clamp(r0 > 4 ? r1 / r0 : 1.0, 0.05, 20.0);
        const bool moving = g.tool == GameTool::Select || g.tool == GameTool::Translate;
        // what the drag will do, drawn over the picture
        if (moving) {
            const ImVec2 off{static_cast<float>(wdx / perPx), static_cast<float>(wdy / perPx)};
            dl->AddRect({b0.x + off.x, b0.y + off.y}, {b1.x + off.x, b1.y + off.y}, accent, 0, 0, 2);
        } else if (g.tool == GameTool::Rotate) {
            dl->AddLine(centre, mouse, accent, 2);
            dl->AddCircle(centre, static_cast<float>(r0), IM_COL32(255, 160, 40, 120), 48, 1.5f);
            dl->AddText({mouse.x + 12, mouse.y}, IM_COL32(255, 255, 255, 255), (num(deg) + " deg").c_str());
        } else if (g.tool == GameTool::Scale) {
            const auto f = static_cast<float>(factor);
            dl->AddRect({centre.x + (b0.x - centre.x) * f, centre.y + (b0.y - centre.y) * f}, {centre.x + (b1.x - centre.x) * f, centre.y + (b1.y - centre.y) * f},
                        accent, 0, 0, 2);
            dl->AddText({mouse.x + 12, mouse.y}, IM_COL32(255, 255, 255, 255), ("x" + num(factor)).c_str());
        }
        if (!active) {
            g.dragging = false;
            const bool moved = std::abs(dx) + std::abs(dy) > 2;
            if (moved && moving && (wdx != 0 || wdy != 0)) gameRun("scene.transform", {{"node", s.selected}, {"dx", wdx}, {"dy", wdy}}, "Move node");
            if (moved && g.tool == GameTool::Rotate && deg != 0) gameRun("scene.transform", {{"node", s.selected}, {"rotate", deg}}, "Rotate node");
            if (moved && g.tool == GameTool::Scale && std::abs(factor - 1) > 1e-3) gameRun("scene.transform", {{"node", s.selected}, {"scale", factor}}, "Scale node");
        }
    } else if (!active) {
        g.dragging = false;
    }
    // the pivot of the selection
    if (s.haveBox && s.selected != "." && (g.tool == GameTool::Rotate || g.tool == GameTool::Scale)) {
        dl->AddCircleFilled(centre, 4, accent);
    }
}

void Shell::sceneSide() {
    GameCanvas& g = canvas_.game;
    SceneCanvas& s = g.scene;
    tp::title("Scene", 1.05f);
    // the tree: one row per node, indented by depth, like Godot's Scene dock
    const float rowsH = std::min(ImGui::GetContentRegionAvail().y * 0.4f, ImGui::GetFrameHeightWithSpacing() * static_cast<float>(s.paths.size() + 1));
    ImGui::BeginChild("scene_tree", {0, std::max(rowsH, ImGui::GetFrameHeightWithSpacing() * 3)}, ImGuiChildFlags_None);
    for (std::size_t i = 0; i < s.paths.size(); ++i) {
        const std::string& path = s.paths[i];
        const std::string name = path == "." ? std::string("(root)") : leaf(path);
        ImGui::Indent(static_cast<float>(depthOf(path)) * ImGui::GetFontSize() * 0.9f + 1);
        const std::string label = name + "  " + s.types[i] + "##" + path;
        if (ImGui::Selectable(label.c_str(), path == s.selected)) sceneSelect(path);
        ImGui::Unindent(static_cast<float>(depthOf(path)) * ImGui::GetFontSize() * 0.9f + 1);
    }
    ImGui::EndChild();

    // add a child node of a chosen class to the selection
    const auto& classes = s.dims == 3 ? s.classes3 : s.classes2;
    std::vector<std::string> all = classes;
    for (const auto& c : s.dims == 3 ? s.classes2 : s.classes3)
        if (std::find(all.begin(), all.end(), c) == all.end()) all.push_back(c);
    s.addClass = std::clamp(s.addClass, 0, std::max(0, static_cast<int>(all.size()) - 1));
    if (!all.empty()) {
        ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - ImGui::CalcTextSize("+ Add").x - ImGui::GetStyle().FramePadding.x * 4);
        if (ImGui::BeginCombo("##class", all[static_cast<std::size_t>(s.addClass)].c_str(), ImGuiComboFlags_HeightLarge)) {
            for (std::size_t i = 0; i < all.size(); ++i) {
                if (i == classes.size()) ImGui::Separator();
                if (ImGui::Selectable(all[i].c_str(), static_cast<int>(i) == s.addClass)) s.addClass = static_cast<int>(i);
            }
            ImGui::EndCombo();
        }
        ImGui::SameLine();
        if (ImGui::Button("+ Add")) {
            gameRun("scene.add", {{"type", all[static_cast<std::size_t>(s.addClass)]}, {"parent", s.selected}}, "Add node");
            if (result_ && result_->ok) sceneSelect(tp::text(*result_, "node"));
        }
        ImGui::SetItemTooltip("Add a child node to the selected one (Ctrl+A in Godot)");
    }
    const bool canEdit = s.selected != ".";
    ImGui::BeginDisabled(!canEdit);
    // place among the siblings, for Up and Down
    std::int64_t place = 0;
    for (const auto& p : s.paths) {
        if (p == s.selected) break;
        if (p != "." && parentPath(p) == parentPath(s.selected) && depthOf(p) == depthOf(s.selected)) ++place;
    }
    if (ImGui::SmallButton("Up") && place > 0) gameRun("scene.move", {{"node", s.selected}, {"index", place - 1}}, "Move node");
    ImGui::SameLine();
    if (ImGui::SmallButton("Down")) gameRun("scene.move", {{"node", s.selected}, {"index", place + 1}}, "Move node");
    ImGui::SameLine();
    if (ImGui::SmallButton("Duplicate")) {
        gameRun("scene.duplicate", {{"node", s.selected}}, "Duplicate node");
        if (result_ && result_->ok) sceneSelect(tp::text(*result_, "node"));
    }
    ImGui::SameLine();
    if (ImGui::SmallButton("Delete")) {
        gameRun("scene.remove", {{"node", s.selected}}, "Delete node");
        sceneSelect(parentPath(s.selected));
    }
    ImGui::EndDisabled();

    // the inspector, read again when the selection or the scene changes
    const std::string key = s.selected + "#" + std::to_string(g.seen);
    if (s.inspectedFor != key) {
        s.inspectedFor = key;
        Result r = query("scene.inspect", {{"doc", currentDoc_}, {"node", s.selected}});
        s.names = split(tp::text(r, "names"), '|');
        s.values = split(tp::text(r, "values"), '|');
        s.kinds = split(tp::text(r, "kinds"), '|');
        s.sections = split(tp::text(r, "sections"), '|');
        s.choices = split(tp::text(r, "choices"), '|');
        s.values.resize(s.names.size());
        s.kinds.resize(s.names.size());
        s.sections.resize(s.names.size());
        s.choices.resize(s.names.size());
        s.edits = s.values;
        s.type = tp::text(r, "type");
        s.script = tp::text(r, "script");
        s.groups = tp::text(r, "groups");
        std::snprintf(s.rename, sizeof s.rename, "%s", leaf(s.selected).c_str());
    }
    ImGui::Spacing();
    tp::title("Inspector", 1.05f);
    ImGui::TextDisabled("%s", s.type.empty() ? "instanced scene" : s.type.c_str());
    if (canEdit) {
        ImGui::SetNextItemWidth(-1);
        if (ImGui::InputText("##name", s.rename, sizeof s.rename, ImGuiInputTextFlags_EnterReturnsTrue) || ImGui::IsItemDeactivatedAfterEdit()) {
            if (s.rename[0] && leaf(s.selected) != s.rename) {
                gameRun("scene.rename", {{"node", s.selected}, {"name", std::string(s.rename)}}, "Rename node");
                if (result_ && result_->ok) sceneSelect(tp::text(*result_, "node"));
            }
        }
    }
    auto set = [&](const std::string& name, const std::string& value) {
        gameRun("scene.set", {{"node", s.selected}, {"props", name + "=" + value}}, "Set " + name);
    };
    ImGui::BeginChild("inspector", {0, 0}, ImGuiChildFlags_None);
    std::string section;
    for (std::size_t i = 0; i < s.names.size(); ++i) {
        if (s.sections[i] != section) {
            section = s.sections[i];
            ImGui::SeparatorText(section.c_str());
        }
        const std::string& name = s.names[i];
        std::string kind = s.kinds[i];
        const bool isDefault = !kind.empty() && kind.back() == '*';
        if (isDefault) kind.pop_back();
        ImGui::PushID(static_cast<int>(i));
        ImGui::AlignTextToFramePadding();
        if (isDefault) ImGui::TextDisabled("%s", name.c_str());
        else ImGui::TextUnformatted(name.c_str());
        ImGui::SameLine(ImGui::GetContentRegionAvail().x * 0.42f);
        ImGui::SetNextItemWidth(-1);
        const std::string& value = s.values[i];
        if (kind == "bool") {
            bool b = value == "true";
            if (ImGui::Checkbox("##v", &b)) set(name, b ? "true" : "false");
        } else if (kind == "color") {
            float c[4] = {1, 1, 1, 1};
            std::string h = value;
            if (!h.empty() && h[0] == '#') h.erase(0, 1);
            if (h.size() == 6 || h.size() == 8) {
                for (std::size_t k = 0; k * 2 < h.size(); ++k) c[k] = static_cast<float>(std::strtoul(h.substr(k * 2, 2).c_str(), nullptr, 16)) / 255.0f;
            }
            ImGui::ColorEdit4("##v", c, ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_AlphaBar);
            if (ImGui::IsItemDeactivatedAfterEdit()) {
                char b[16];
                auto byte = [](float v) { return static_cast<int>(std::lround(std::clamp(v, 0.0f, 1.0f) * 255)); };
                std::snprintf(b, sizeof b, "#%02x%02x%02x%02x", byte(c[0]), byte(c[1]), byte(c[2]), byte(c[3]));
                set(name, b);
            }
        } else if (kind == "enum") {
            const auto opts = split(s.choices[i], ';');
            if (ImGui::BeginCombo("##v", value.c_str())) {
                for (const auto& o : opts)
                    if (ImGui::Selectable(o.c_str(), o == value)) set(name, o);
                ImGui::EndCombo();
            }
        } else if (kind == "int" || kind == "float" || kind == "angle" || kind == "vec2" || kind == "vec3" || kind == "angle3") {
            auto v = floats(value);
            const int n = kind == "vec2" ? 2 : (kind == "vec3" || kind == "angle3") ? 3 : 1;
            v.resize(static_cast<std::size_t>(n), 0.0f);
            const float speed = kind == "int" ? 0.2f : (kind == "angle" || kind == "angle3") ? 0.5f : 0.1f;
            const char* fmt = kind == "int" ? "%.0f" : "%.3g";
            if (n == 1) ImGui::DragFloat("##v", v.data(), speed, 0, 0, fmt);
            else if (n == 2) ImGui::DragFloat2("##v", v.data(), speed, 0, 0, fmt);
            else ImGui::DragFloat3("##v", v.data(), speed, 0, 0, fmt);
            if (ImGui::IsItemDeactivatedAfterEdit()) {
                std::string text;
                for (int k = 0; k < n; ++k) text += (k ? "," : "") + num(kind == "int" ? std::round(v[static_cast<std::size_t>(k)]) : v[static_cast<std::size_t>(k)]);
                set(name, text);
            }
        } else {
            // text, files (paths or res://), shapes (rect:16x32, circle:8 ...), meshes, points
            ImGui::InputText("##v", &s.edits[i]);
            if (ImGui::IsItemDeactivatedAfterEdit() && s.edits[i] != value) set(name, s.edits[i].empty() ? std::string("none") : s.edits[i]);
            if (kind == "shape2d") ImGui::SetItemTooltip("rect:WxH, circle:R, capsule:RxH or segment");
            else if (kind == "shape3d") ImGui::SetItemTooltip("box:WxHxD, sphere:R, capsule:RxH or cylinder:RxH");
            else if (kind == "mesh") ImGui::SetItemTooltip("box, sphere, cylinder, capsule, plane, or a mesh file");
            else if (kind == "texture" || kind == "stream") ImGui::SetItemTooltip("a file path, or res://...");
        }
        ImGui::PopID();
    }
    // script and groups, as Godot shows them at the bottom of the inspector
    ImGui::SeparatorText("Script");
    if (!s.script.empty()) {
        ImGui::TextWrapped("%s", tp::baseName(s.script).c_str());
        ImGui::SetItemTooltip("%s", s.script.c_str());
        if (ImGui::SmallButton("Open script")) {
            if (find("code.open")) run("code.open", {{"path", s.script}});
        }
        ImGui::SameLine();
        if (ImGui::SmallButton("Detach")) set("script", "none");
    } else {
        static const char* kinds[] = {"empty", "character2d", "topdown2d", "character3d", "area"};
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 8);
        ImGui::Combo("##tmpl", &s.scriptKind, kinds, static_cast<int>(std::size(kinds)));
        ImGui::SameLine();
        if (ImGui::SmallButton("Attach script")) {
            const std::string base = s.selected == "." ? std::string("main") : leaf(s.selected);
            std::string file;
            for (char ch : base) file += static_cast<char>(std::isalnum(static_cast<unsigned char>(ch)) ? std::tolower(static_cast<unsigned char>(ch)) : '_');
            gameRun("scene.script", {{"node", s.selected}, {"path", file + ".gd"}, {"template", std::string(kinds[s.scriptKind])}}, "Attach script");
        }
        ImGui::SetItemTooltip("Writes a starter GDScript next to the scene and attaches it");
    }
    ImGui::SeparatorText("Groups");
    std::string groups = s.groups;
    ImGui::SetNextItemWidth(-1);
    if (ImGui::InputText("##groups", &groups, ImGuiInputTextFlags_EnterReturnsTrue)) gameRun("scene.set", {{"node", s.selected}, {"props", "groups=" + groups}}, "Set groups");
    ImGui::SetItemTooltip("Comma-separated, e.g. enemies, coins");
    ImGui::EndChild();
}

}  // namespace wetwaresuite::gui
