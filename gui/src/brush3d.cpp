// Brushing in the 3D view: sculpt a mesh with a brush circle (pen pressure works), or paint how
// much a bone moves a skinned model. The stroke previews on the model while you drag (each new
// piece re-runs the whole stroke as one trial) and letting go keeps it as one undo step.
// Alt+drag still turns the view, right-drag moves it.
#include <algorithm>
#include <cmath>
#include <cstdio>

#include "shell.hpp"

namespace wetwaresuite::gui {

namespace {

double nowS() { return static_cast<double>(SDL_GetTicks()) / 1000.0; }

struct BrushName {
    const char* id;
    const char* label;
    const char* tip;
};
constexpr BrushName kSculptBrushes[] = {
    {"", "Off", "Turn, move and zoom the view with the left mouse"},
    {"draw", "Draw", "Raise the surface where you paint (Invert digs in)"},
    {"inflate", "Inflate", "Puff the surface out along its normals"},
    {"smooth", "Smooth", "Even out bumps"},
    {"flatten", "Flatten", "Press the surface flat"},
    {"pinch", "Pinch", "Pull toward the stroke, for sharp creases"},
    {"grab", "Grab", "Pick up the surface and drag it"},
    {"crease", "Crease", "Cut a sharp groove (Invert makes a ridge)"},
};

}  // namespace

Params Shell::brush3dParams() const {
    Params p{{"doc", currentDoc_}};
    const double scale = view3d_.camOk && view3d_.cam.scale > 0 ? view3d_.cam.scale : 1;
    const double radius = brush3d_.radiusPx / scale;
    if (brush3d_.tool == "weight") {
        p["bone"] = brush3d_.bone;
        p["ray"] = brush3d_.rays;
        p["radius"] = radius;
        p["strength"] = static_cast<double>(brush3d_.strength);
        p["mode"] = brush3d_.weightMode;
        return p;
    }
    p["brush"] = brush3d_.tool;
    p["space"] = std::string("ray");
    p["points"] = brush3d_.rays;
    p["radius"] = radius;
    p["strength"] = static_cast<double>(brush3d_.strength);
    p["invert"] = brush3d_.invert;
    p["symmetry"] = brush3d_.symmetry;
    bool pressed = false;
    for (double x : brush3d_.pressures) pressed = pressed || x < 0.999;
    if (pressed) p["pressures"] = NumberList(brush3d_.pressures.begin(), brush3d_.pressures.end());
    return p;
}

void Shell::brush3dToolbar() {
    auto doc = session_.workspace().get(currentDoc_);
    if (!doc) return;
    Brush3d& b = brush3d_;
    if (doc->kind() == "mesh" && find("sculpt.stroke")) {
        ImGui::SameLine();
        int current = 0;
        for (int i = 0; i < 8; ++i)
            if (b.tool == kSculptBrushes[i].id) current = i;
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 6);
        if (ImGui::BeginCombo("##sculptbrush", kSculptBrushes[current].label)) {
            for (int i = 0; i < 8; ++i) {
                if (ImGui::Selectable(kSculptBrushes[i].label, i == current)) b.tool = kSculptBrushes[i].id;
                ImGui::SetItemTooltip("%s", kSculptBrushes[i].tip);
            }
            ImGui::EndCombo();
        }
        ImGui::SetItemTooltip("Sculpt right on the model: pick a brush, then paint with the mouse or a pen");
    } else if (doc->kind() == "model" && find("model.weight_paint")) {
        ImGui::SameLine();
        bool on = b.tool == "weight";
        if (ImGui::Checkbox("Weights", &on)) {
            b.tool = on ? "weight" : "";
            if (!on) run("model.weight_show", {{"doc", currentDoc_}, {"bone", std::string()}});
        }
        ImGui::SetItemTooltip("Weight paint: brush how much a bone moves the skinned mesh (blue none, red all)");
        if (on) {
            static std::string seenKey;
            static std::vector<std::string> bones;
            const std::string key = currentDoc_ + "|" + std::to_string(changes_);
            if (key != seenKey) {
                seenKey = key;
                bones.clear();
                const Result r = query("model.rig_info", {{"doc", currentDoc_}});
                const Value* v = r.ok ? r.field("bones") : nullptr;
                std::string list = v ? toText(*v) : std::string();
                for (std::size_t at = 0; !list.empty();) {
                    const auto comma = list.find(',', at);
                    bones.push_back(list.substr(at, comma == std::string::npos ? std::string::npos : comma - at));
                    if (comma == std::string::npos) break;
                    at = comma + 1;
                }
            }
            if (b.bone.empty() && !bones.empty()) b.bone = bones.front();
            ImGui::SameLine();
            ImGui::SetNextItemWidth(ImGui::GetFontSize() * 6);
            if (ImGui::BeginCombo("##bone", b.bone.empty() ? "(no bones)" : b.bone.c_str())) {
                for (const auto& n : bones)
                    if (ImGui::Selectable(n.c_str(), n == b.bone)) {
                        b.bone = n;
                        run("model.weight_show", {{"doc", currentDoc_}, {"bone", n}});
                        canvas_.orbitDirty = true;
                    }
                ImGui::EndCombo();
            }
            ImGui::SetItemTooltip("The bone whose weights you paint");
            ImGui::SameLine();
            static const char* modes[] = {"add", "subtract", "replace", "smooth"};
            int m = 0;
            for (int i = 0; i < 4; ++i)
                if (b.weightMode == modes[i]) m = i;
            ImGui::SetNextItemWidth(ImGui::GetFontSize() * 5.5f);
            if (ImGui::Combo("##wmode", &m, modes, 4)) b.weightMode = modes[m];
        }
    }
    if (b.tool.empty()) return;
    ImGui::SameLine();
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 5);
    ImGui::SliderFloat("##radius", &b.radiusPx, 4, 300, "radius %.0f px");
    ImGui::SetItemTooltip("Brush size on screen ([ and ] change it)");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 5);
    ImGui::SliderFloat("##strength", &b.strength, 0.02f, 1, "strength %.2f");
    if (b.tool != "weight") {
        ImGui::SameLine();
        ImGui::Checkbox("Invert", &b.invert);
        ImGui::SameLine();
        static const char* syms[] = {"none", "x", "y", "z", "xy", "xyz"};
        int sm = 0;
        for (int i = 0; i < 6; ++i)
            if (b.symmetry == syms[i]) sm = i;
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 4);
        if (ImGui::Combo("##sym", &sm, syms, 6)) b.symmetry = syms[sm];
        ImGui::SetItemTooltip("Mirror the stroke across these axes");
    }
}

bool Shell::brush3dInput(ImDrawList* dl, ImVec2 v0, ImVec2 size, bool hovered) {
    Brush3d& b = brush3d_;
    auto doc = session_.workspace().get(currentDoc_);
    const bool usable = doc && !b.tool.empty() && view3d_.camOk &&
                        ((b.tool == "weight" && doc->kind() == "model") || (b.tool != "weight" && doc->kind() == "mesh"));
    if (!usable) {
        if (b.stroking) dropTrial();
        b.stroking = false;
        return false;
    }
    ImGuiIO& io = ImGui::GetIO();
    if (hovered && !io.WantTextInput) {
        if (ImGui::IsKeyPressed(ImGuiKey_LeftBracket)) b.radiusPx = std::max(4.0f, b.radiusPx / 1.15f);
        if (ImGui::IsKeyPressed(ImGuiKey_RightBracket)) b.radiusPx = std::min(300.0f, b.radiusPx * 1.15f);
    }
    // the brush circle under the mouse
    if (hovered || b.stroking) dl->AddCircle(io.MousePos, b.radiusPx, IM_COL32(255, 255, 255, 220), 48, 3.0f),
        dl->AddCircle(io.MousePos, b.radiusPx, b.invert ? IM_COL32(60, 110, 230, 255) : IM_COL32(230, 90, 60, 255), 48, 1.5f);
    if (io.KeyAlt && !b.stroking) return false;  // Alt+drag turns the view
    const ViewCamera& c = view3d_.cam;
    auto rayAt = [&](ImVec2 m) {
        const double a = (m.x - v0.x - view3d_.camW / 2.0) / c.scale + c.cx, u = (view3d_.camH / 2.0 - (m.y - v0.y)) / c.scale + c.cy;
        const double back = 100.0 * std::max(size.x, size.y) / c.scale;
        for (int k = 0; k < 3; ++k) b.rays.push_back(c.right[k] * a + c.up[k] * u - c.forward[k] * back);
        for (int k = 0; k < 3; ++k) b.rays.push_back(c.forward[k]);
        b.pressures.push_back(penDown_ ? std::clamp(static_cast<double>(penPressure_), 0.0, 1.0) : 1.0);
    };
    static ImVec2 last;
    if (!b.stroking && hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        dropTrial();
        b.stroking = true;
        b.rays.clear();
        b.pressures.clear();
        b.tried = 0;
        b.lastTry = 0;
        rayAt(io.MousePos);
        last = io.MousePos;
    }
    if (!b.stroking) return false;
    const char* command = b.tool == "weight" ? "model.weight_paint" : "sculpt.stroke";
    if (ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
        const float dx = io.MousePos.x - last.x, dy = io.MousePos.y - last.y;
        if (dx * dx + dy * dy >= 9) {
            rayAt(io.MousePos);
            last = io.MousePos;
        }
        const std::size_t n = b.rays.size() / 6;
        if (n != b.tried && nowS() - b.lastTry > 0.08) {  // show the stroke so far
            b.lastTry = nowS();
            b.tried = n;
            tryRun(command, brush3dParams());
            canvas_.orbitDirty = true;
        }
        return true;
    }
    // let go: keep it, as one undo step
    b.stroking = false;
    const Params p = brush3dParams();
    resultFor_ = b.tool == "weight" ? "Weight paint" : "Sculpt";
    if (b.tried == b.rays.size() / 6 && keepTrial(command, p)) {
        result_ = trial_.result;
    } else {
        dropTrial();
        Result r = run(command, p);
        r.message = r.ok ? sentence(r.message) : friendlyError(r.message);
        result_ = std::move(r);
    }
    canvas_.orbitDirty = true;
    return true;
}

}  // namespace wetwaresuite::gui
