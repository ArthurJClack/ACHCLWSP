// The 3D view for cad parts, meshes, sculpts, scan fits and prints: drag to
// turn the model, right- or middle-drag to move it, scroll to zoom, double-click
// to fit. The document draws itself (Document::renderView, the same software
// renderer app.view uses headless), so it runs without a GPU. Frames drawn
// mid-drag skip antialiasing; letting go redraws it sharp.
#include <algorithm>
#include <cmath>
#include <cstdio>

#include "shell.hpp"

namespace wetwaresuite::gui {

namespace {

struct Angle {
    const char* name;
    double yaw, pitch;
    const char* tip;
};
constexpr Angle kAngles[] = {{"Iso", 45, 35.26, "From the front-right, above"},
                             {"Front", 0, 0, "Straight on from the front"},
                             {"Right", 90, 0, "From the right side"},
                             {"Top", 0, 89.9, "Looking down from above"}};

}  // namespace

bool Shell::probe3d(const Document& doc) const {
    std::vector<std::uint8_t> rgba;
    return doc.renderView(OrbitView{}, 0, 0, rgba);  // 0 x 0 only asks
}

void Shell::orbitToolbar() {
    for (const auto& a : kAngles) {
        if (ImGui::Button(a.name)) {
            canvas_.orbit.yaw = a.yaw;
            canvas_.orbit.pitch = a.pitch;
            canvas_.orbitDirty = true;
        }
        ImGui::SetItemTooltip("%s", a.tip);
        ImGui::SameLine();
    }
    if (ImGui::Button("Fit")) {
        canvas_.orbit.zoom = 1;
        canvas_.orbit.panX = canvas_.orbit.panY = 0;
        canvas_.orbitDirty = true;
    }
    ImGui::SetItemTooltip("Show the whole model (or double-click it)");
    ImGui::SameLine();
    {
        // viewport shading, like Blender's: Alt+Z toggles see-through
        static const char* names[] = {"material", "solid", "matcap", "wireframe", "xray"};
        static const char* labels[] = {"Material", "Solid", "Clay", "Wireframe", "X-ray"};
        int current = 0;
        for (int i = 0; i < 5; ++i)
            if (canvas_.orbit.shading == names[i]) current = i;
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 6.5f);
        if (ImGui::Combo("##shading", &current, labels, 5)) {
            canvas_.orbit.shading = current == 0 ? std::string() : std::string(names[current]);
            canvas_.orbitDirty = true;
        }
        ImGui::SetItemTooltip("How the model is drawn: its materials, one plain colour, studio clay, edges only, or see-through "
                              "(X-ray, Alt+Z: hidden edges show and box select takes them too)");
        if (ImGui::IsKeyPressed(ImGuiKey_Z, false) && ImGui::GetIO().KeyAlt && !ImGui::GetIO().WantTextInput) {
            canvas_.orbit.shading = canvas_.orbit.shading == "xray" ? std::string() : std::string("xray");
            canvas_.orbitDirty = true;
        }
        ImGui::SameLine();
    }
    brush3dToolbar();
    ImGui::SameLine();
    auto doc = session_.workspace().get(currentDoc_);
    if (doc && doc->kind() == "cad" && find("cad.sketch_geometry")) {
        // open the last sketch for drawing
        static std::string lastFor, lastName;  // asked again only after a change
        const std::string key = currentDoc_ + "|" + std::to_string(changes_);
        if (key != lastFor) {
            lastFor = key;
            const Result last = query("cad.sketch_geometry", {{"doc", currentDoc_}});
            const auto* n = last.ok ? std::get_if<std::string>(&last.value) : nullptr;
            const Value* used = last.field("used");
            // a sketch already made into a solid is edited from the tree, not offered here
            lastName = n && !(used && std::get_if<bool>(used) && *std::get_if<bool>(used)) ? *n : std::string();
        }
        if (!lastName.empty()) {
            if (ImGui::Button(("Edit " + lastName).c_str())) enterSketch(currentDoc_, lastName);
            ImGui::SetItemTooltip("Draw on the sketch with the mouse: lines, rectangles, circles");
            ImGui::SameLine();
        }
        if (find("cad.pick")) {
            view3dToolbar();
        } else if (lastName.empty()) {
            if (ImGui::Button("New sketch")) select("cad.sketch");
            ImGui::SetItemTooltip("Start a sketch on a plane, then draw on it with the mouse");
            ImGui::SameLine();
        }
    }
    if (doc && doc->kind() == "model" && canvas_.grab) {
        const char* what = canvas_.grab == 'G' ? "Moving" : canvas_.grab == 'R' ? "Rotating" : "Scaling";
        ImGui::Text("%s%s%s: move the mouse, click to keep, Esc to undo; X Y Z lock an axis", what,
                    canvas_.grabAxis.empty() ? "" : " along ", canvas_.grabAxis.c_str());
    } else if (doc && doc->kind() == "model") {
        ImGui::TextDisabled(canvas_.boxArmed ? "drag a box to select (Shift adds, Esc stops)"
                                              : "drag to turn, right-drag to pan, click to select, B box select; arrows move, rings turn, boxes stretch (Ctrl snaps, Alt snaps to vertices)");
    } else if (!(doc && doc->kind() == "cad" && find("cad.pick"))) {  // cad's own row says it (handles.cpp)
        ImGui::TextDisabled("drag to turn, right-drag to move, scroll to zoom, double-click to fit");
    }
}

void Shell::orbitCanvas(ImDrawList* dl, ImVec2 v0, ImVec2 size, bool hovered, bool active) {
    ImGuiIO& io = ImGui::GetIO();
    OrbitView& o = canvas_.orbit;
    // a sculpt or weight brush, then arrows on faces and selections, take the mouse first (brush3d.cpp, handles.cpp)
    const bool brushing = brush3dInput(dl, v0, size, hovered);
    const bool onHandle = brushing || view3dInput(dl, v0, size, hovered, active);
    // B (edit mode): the next left drag draws a box that selects; in X-ray it takes hidden elements too
    {
        auto bdoc = session_.workspace().get(currentDoc_);
        const bool modelDoc = bdoc && bdoc->kind() == "model";
        if (modelDoc && hovered && !io.WantTextInput && !io.KeyCtrl && !io.KeyAlt && ImGui::IsKeyPressed(ImGuiKey_B, false)) canvas_.boxArmed = true;
        if (canvas_.boxArmed && ImGui::IsKeyPressed(ImGuiKey_Escape)) canvas_.boxArmed = canvas_.boxDragging = false;
        if (canvas_.boxArmed && hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
            canvas_.boxDragging = true;
            canvas_.boxFrom = io.MousePos;
        }
        if (canvas_.boxDragging) {
            const ImVec2 a = canvas_.boxFrom, b = io.MousePos;
            dl->AddRectFilled({std::min(a.x, b.x), std::min(a.y, b.y)}, {std::max(a.x, b.x), std::max(a.y, b.y)}, IM_COL32(255, 160, 40, 40));
            dl->AddRect({std::min(a.x, b.x), std::min(a.y, b.y)}, {std::max(a.x, b.x), std::max(a.y, b.y)}, IM_COL32(255, 160, 40, 220), 0, 0, 1.5f);
            if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
                canvas_.boxDragging = canvas_.boxArmed = false;
                auto uv = [&](float x, float lo, float len) { return std::clamp((x - lo) / len, 0.0f, 1.0f); };
                NumberList rect{uv(std::min(a.x, b.x), v0.x, size.x), uv(std::min(a.y, b.y), v0.y, size.y), uv(std::max(a.x, b.x), v0.x, size.x),
                                uv(std::max(a.y, b.y), v0.y, size.y)};
                Result r = run("model.select", {{"doc", currentDoc_}, {"type", std::string("rect")}, {"rect", rect},
                                                {"xray", o.shading == "xray" || o.shading == "wireframe"}, {"extend", io.KeyShift},
                                                {"yaw", o.yaw}, {"pitch", o.pitch}, {"zoom", o.zoom}, {"pan_x", o.panX}, {"pan_y", o.panY},
                                                {"width", static_cast<std::int64_t>(size.x)}, {"height", static_cast<std::int64_t>(size.y)}});
                resultFor_ = "Box select";
                result_ = r.ok ? Result::success(sentence(r.message)) : Result::failure(friendlyError(r.message));
                canvas_.orbitDirty = true;
                canvas_.grabEndClick = true;  // the release isn't also a click to select
            }
        }
    }
    const bool left = active && ImGui::IsMouseDown(ImGuiMouseButton_Left) && !onHandle && view3d_.dragging < 0 && !canvas_.boxArmed && !canvas_.boxDragging;
    const bool move = active && (ImGui::IsMouseDown(ImGuiMouseButton_Right) || ImGui::IsMouseDown(ImGuiMouseButton_Middle) ||
                                 (left && io.KeyShift));
    const float minSide = std::max(1.0f, std::min(size.x, size.y));
    if ((io.MouseDelta.x != 0 || io.MouseDelta.y != 0) && (left || move) && !canvas_.grab) {
        if (move) {
            // a model radius is about half the shorter side at zoom 1
            const double perPx = 2.0 / (minSide * 0.88 * o.zoom);
            o.panX += io.MouseDelta.x * perPx;
            o.panY -= io.MouseDelta.y * perPx;
        } else {
            o.yaw -= io.MouseDelta.x * 0.4;
            o.pitch = std::clamp(o.pitch + io.MouseDelta.y * 0.4, -89.9, 89.9);
        }
        canvas_.orbitDirty = true;
        canvas_.orbitDragging = true;
    }
    if (hovered && io.MouseWheel != 0) {
        o.zoom = std::clamp(o.zoom * std::pow(1.15, static_cast<double>(io.MouseWheel)), 0.05, 200.0);
        canvas_.orbitDirty = true;
    }
    if (hovered && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left) && !onHandle) {
        o.camera.reset();
        o.zoom = 1;
        o.panX = o.panY = 0;
        canvas_.orbitDirty = true;
    }
    // Blender's G, R and S: the selection follows the mouse until a click (or Enter) keeps it,
    // Esc or a right-click puts it back; X, Y, Z lock it to an axis. One undo step either way.
    auto doc3d = session_.workspace().get(currentDoc_);
    const bool isModel = doc3d && doc3d->kind() == "model";
    if (!isModel) canvas_.grab = 0;
    if (isModel && !canvas_.grab && hovered && !io.WantTextInput && !io.KeyCtrl && !io.KeyAlt) {
        const char keys[] = {'G', 'R', 'S'};
        const ImGuiKey codes[] = {ImGuiKey_G, ImGuiKey_R, ImGuiKey_S};
        for (int i = 0; i < 3; ++i) {
            if (ImGui::IsKeyPressed(codes[i], false)) {
                canvas_.grab = keys[i];
                canvas_.grabApplied = false;
                canvas_.grabStart = io.MousePos;
                canvas_.grabAxis.clear();
                canvas_.grabExact = false;
            }
        }
    }
    if (canvas_.grab) {
        const char* axes[] = {"x", "y", "z"};
        const ImGuiKey axisKeys[] = {ImGuiKey_X, ImGuiKey_Y, ImGuiKey_Z};
        bool changed = io.MouseDelta.x != 0 || io.MouseDelta.y != 0;
        for (int i = 0; i < 3; ++i) {
            if (ImGui::IsKeyPressed(axisKeys[i], false)) {
                canvas_.grabAxis = canvas_.grabAxis == axes[i] ? "" : axes[i];
                changed = true;
            }
        }
        const bool cancel = ImGui::IsKeyPressed(ImGuiKey_Escape) || ImGui::IsMouseClicked(ImGuiMouseButton_Right);
        // an arrow drag keeps it when the mouse is let go; G keeps it on the next click
        const bool keep = canvas_.grabByHandle ? !ImGui::IsMouseDown(ImGuiMouseButton_Left)
                                               : ImGui::IsMouseClicked(ImGuiMouseButton_Left) || ImGui::IsKeyPressed(ImGuiKey_Enter) ||
                                                     ImGui::IsKeyPressed(ImGuiKey_KeypadEnter);
        if (io.KeyCtrl != canvas_.grabCtrl || io.KeyAlt != canvas_.grabAlt) changed = true;  // snapping turned on or off
        canvas_.grabCtrl = io.KeyCtrl;
        canvas_.grabAlt = io.KeyAlt;
        if (cancel) {
            if (canvas_.grabApplied) session_.history().undo();
            canvas_.grab = 0;
            canvas_.grabByHandle = false;
            canvas_.grabExact = false;
            canvas_.orbitDirty = true;
            syncDocs();
        } else if (changed) {
            if (canvas_.grabApplied) session_.history().undo();
            Params p{{"doc", currentDoc_},
                     {"dx", static_cast<double>(io.MousePos.x - canvas_.grabStart.x)},
                     {"dy", static_cast<double>(io.MousePos.y - canvas_.grabStart.y)},
                     {"mode", std::string(canvas_.grab == 'G' ? "move" : canvas_.grab == 'R' ? "rotate" : "scale")},
                     {"yaw", o.yaw}, {"pitch", o.pitch}, {"zoom", o.zoom}, {"pan_x", o.panX}, {"pan_y", o.panY},
                     {"width", static_cast<std::int64_t>(size.x)}, {"height", static_cast<std::int64_t>(size.y)}};
            if (!canvas_.grabAxis.empty()) p["axis"] = canvas_.grabAxis;
            if (canvas_.grabExact && canvas_.grab == 'R') {
                // the ring turns as far as the mouse went round the pivot
                const float ax = canvas_.grabStart.x - canvas_.grabCentre.x, ay = canvas_.grabStart.y - canvas_.grabCentre.y;
                const float bx = io.MousePos.x - canvas_.grabCentre.x, by = io.MousePos.y - canvas_.grabCentre.y;
                const double turn = std::atan2(ax * by - ay * bx, ax * bx + ay * by) * 180 / 3.14159265358979323846;
                p["angle"] = -turn * canvas_.grabFacing;  // screen y is down
            } else if (canvas_.grabExact && canvas_.grab == 'S') {
                // the box stretches by how far it went along the axis, against its distance from the pivot
                const double along = (io.MousePos.x - canvas_.grabCentre.x) * canvas_.grabDir.x + (io.MousePos.y - canvas_.grabCentre.y) * canvas_.grabDir.y;
                const double from = (canvas_.grabStart.x - canvas_.grabCentre.x) * canvas_.grabDir.x + (canvas_.grabStart.y - canvas_.grabCentre.y) * canvas_.grabDir.y;
                p["factor"] = std::abs(from) > 1 ? std::max(0.01, along / from) : 1.0;
            }
            // Ctrl snaps to the grid (and 5 degree / 0.1 steps); Alt snaps a move onto the nearest vertex
            if (io.KeyCtrl) p["snap"] = std::string("grid");
            else if (io.KeyAlt && canvas_.grab == 'G') {
                p["snap"] = std::string("vertex");
                p["mouse_x"] = static_cast<double>(io.MousePos.x - v0.x);
                p["mouse_y"] = static_cast<double>(io.MousePos.y - v0.y);
            }
            Result r = run("model.drag", p);
            canvas_.grabApplied = r.ok;
            if (!r.ok) {
                result_ = Result::failure(friendlyError(r.message));
                resultFor_ = "Move";
                canvas_.grab = 0;
            }
            canvas_.orbitDirty = true;
        }
        if (keep && canvas_.grab) {
            canvas_.grab = 0;
            canvas_.grabEndClick = canvas_.grabByHandle || ImGui::IsMouseClicked(ImGuiMouseButton_Left);
            canvas_.grabByHandle = false;
            canvas_.grabExact = false;
            resultFor_ = "Move";
            result_ = Result::success(canvas_.grabApplied ? "Done. Undo puts it back." : "Nothing moved.");
        }
    }

    // a click (no drag) in a model selects what's under it: objects, or
    // vertices/edges/faces in edit mode (ctrl adds to the selection)
    if (canvas_.grabEndClick && ImGui::IsMouseReleased(ImGuiMouseButton_Left)) {
        canvas_.grabEndClick = false;
    } else if (!canvas_.grab && !brushing && brush3d_.tool.empty() && hovered && ImGui::IsMouseReleased(ImGuiMouseButton_Left) && io.MouseDragMaxDistanceSqr[ImGuiMouseButton_Left] < 16 &&
        !io.KeyShift && size.x >= 8 && size.y >= 8) {
        auto doc = session_.workspace().get(currentDoc_);
        if (doc && doc->kind() == "model") {
            const double u = (io.MousePos.x - v0.x) / size.x, v = (io.MousePos.y - v0.y) / size.y;
            run("model.pick", {{"doc", currentDoc_}, {"u", u}, {"v", v}, {"yaw", o.yaw}, {"pitch", o.pitch}, {"zoom", o.zoom},
                               {"pan_x", o.panX}, {"pan_y", o.panY}, {"width", static_cast<std::int64_t>(size.x)},
                               {"height", static_cast<std::int64_t>(size.y)}, {"extend", io.KeyCtrl}});
            canvas_.orbitDirty = true;
        }
    }
    if (canvas_.orbitDragging && !active) {
        canvas_.orbitDragging = false;
        canvas_.orbitDirty = true;  // redraw sharp
    }
    const int w = static_cast<int>(size.x), h = static_cast<int>(size.y);
    if (w != canvas_.orbitW || h != canvas_.orbitH) canvas_.orbitDirty = true;
    if (canvas_.orbitDirty && w >= 8 && h >= 8) {
        canvas_.orbitDirty = false;
        canvas_.orbitW = w;
        canvas_.orbitH = h;
        if (auto doc = session_.workspace().get(currentDoc_)) {
            std::vector<std::uint8_t> rgba;
            o.antialias = !canvas_.orbitDragging && io.MouseWheel == 0;
            if (doc->renderView(o, w, h, rgba)) canvas_.texture.upload(renderer_, rgba.data(), w, h);
            if (!o.antialias) canvas_.orbitSharpPending = true;
            view3dCamera();  // what the arrows are drawn with
        }
    } else if (canvas_.orbitSharpPending && !canvas_.orbitDragging && io.MouseWheel == 0) {
        canvas_.orbitSharpPending = false;
        canvas_.orbitDirty = true;  // the wheel stopped: draw it sharp next frame
    }
    if (canvas_.texture.tex) dl->AddImage(canvas_.texture.id(), v0, {v0.x + size.x, v0.y + size.y});
    view3dDraw(dl, v0, size);
    char angle[64];
    std::snprintf(angle, sizeof angle, "yaw %.0f  pitch %.0f  zoom %.2g", std::fmod(o.yaw + 3600.0, 360.0), o.pitch, o.zoom);
    dl->AddText({v0.x + 8, v0.y + size.y - ImGui::GetFontSize() - 6}, IM_COL32(110, 110, 120, 255), angle);
}

}  // namespace wetwaresuite::gui
