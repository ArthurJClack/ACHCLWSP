// Circuit documents on the canvas: show the schematic, the board or the board in 3D, and click
// to select, place parts, draw wires, move, delete, draw traces and add vias. Every click asks
// circuit.hit what is under it, then runs the ordinary circuit.* command, so it can be undone
// and scripts or Claude see the same steps.
#include <cmath>

#include "tool_panels.hpp"
// after imgui.h
#include "imgui_stdlib.h"

namespace wetwaresuite::gui {

namespace {

const char* const kSymbols[] = {"resistor", "capacitor", "capacitor_polarized", "inductor", "led", "diode", "zener", "npn", "pnp", "nmos",
                                "pmos", "switch", "header", "crystal", "lm358", "ne555", "ams1117", "atmega328p", "attiny85", "esp32_wroom",
                                "rp2040", "gnd", "vcc", "+5v", "+3v3", "battery", "test_point", "mounting_hole"};

}  // namespace

bool Shell::circuitDoc() const {
    if (!find("circuit.hit")) return false;
    auto doc = session_.workspace().get(currentDoc_);
    return doc && doc->kind() == "circuit";
}

bool Shell::circuitEditing() const { return circuitDoc() && circuit_.tool != "select" && circuit_.view != "3d"; }

void Shell::circuitTools() {
    if (circuit_.doc != currentDoc_ || circuit_.view == "3d") return;  // the toolbar reads the view first
    struct ToolDef {
        const char* label;
        const char* tool;
        const char* key;
        const char* tip;
    };
    const ToolDef sheetTools[] = {{"Select", "select", "V", "Click anything to see what it is (pin, net, part); drag to move around"},
                                  {"Place", "place", "P", "Click to place the part chosen on the options bar"},
                                  {"Wire", "wire", "W", "Click where a wire starts, then at each corner; right-click or Esc ends it"},
                                  {"Move", "move", "M", "Click a part, then where it should go"},
                                  {"Delete", "delete", "D", "Click a part or a wire to remove it"}};
    const ToolDef boardTools[] = {{"Select", "select", "V", "Click a pad, trace or footprint to see what it is"},
                                  {"Move", "move", "M", "Click a footprint, then where it should go"},
                                  {"Trace", "trace", "X", "Click a pad, then each corner; right-click or Esc ends it"},
                                  {"Via", "via", "Y", "Click to drop a via (it takes the net of the copper there)"}};
    const bool board = circuit_.view == "pcb";
    for (const auto& t : board ? std::vector<ToolDef>(std::begin(boardTools), std::end(boardTools))
                               : std::vector<ToolDef>(std::begin(sheetTools), std::end(sheetTools))) {
        if (railItem(t.label, t.key, t.tip, circuit_.tool == t.tool)) {
            circuit_.tool = t.tool;
            circuit_.hasStart = false;
            circuit_.moving.clear();
        }
    }
}

void Shell::circuitToolbar() {
    if (circuit_.doc != currentDoc_) {
        // what the document shows (another client may have switched it): circuit.hit says, read-only
        circuit_ = CircuitCanvas{currentDoc_};
        Result h = query("circuit.hit", {{"doc", currentDoc_}, {"x", 0.0}, {"y", 0.0}});
        circuit_.view = h.ok ? tp::text(h, "view") : std::string("3d");
    }
    auto show = [&](Result r, const char* what) {
        resultFor_ = what;
        r.message = r.ok ? sentence(r.message) : friendlyError(r.message);
        result_ = std::move(r);
        canvas_.dirty = true;
    };
    ImGui::SameLine();
    ImGui::TextUnformatted("  Show:");
    const struct {
        const char* label;
        const char* view;
    } views[] = {{"Schematic", "schematic"}, {"Board", "pcb"}, {"3D", "3d"}};
    for (const auto& v : views) {
        ImGui::SameLine();
        if (ImGui::RadioButton(v.label, circuit_.view == v.view) && circuit_.view != v.view) {
            Result r = run("circuit.view", {{"doc", currentDoc_}, {"view", std::string(v.view)}});
            if (r.ok) {
                circuit_.view = v.view;
                circuit_.hasStart = false;
                circuit_.moving.clear();
                if (circuit_.tool == "place" || circuit_.tool == "wire" || circuit_.tool == "trace" || circuit_.tool == "via") circuit_.tool = "select";
                canvas_.fit = true;
            }
            canvas_.dirty = true;
        }
    }
    if (circuit_.view == "3d") {
        ImGui::SameLine();
        ImGui::TextDisabled("(drag to move around)");
        return;
    }
    ImGui::SameLine();
    const bool board = circuit_.view == "pcb";
    const float unit = ImGui::GetFontSize();
    if (circuit_.tool == "place") {
        ImGui::SameLine();
        ImGui::SetNextItemWidth(unit * 9);
        if (ImGui::BeginCombo("##symbol", circuit_.symbol.c_str())) {
            for (const char* s : kSymbols) {
                if (ImGui::Selectable(s, circuit_.symbol == s)) circuit_.symbol = s;
            }
            ImGui::EndCombo();
        }
        ImGui::SetItemTooltip("The part to place (circuit.symbols lists them all; type another name below)");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(unit * 6);
        ImGui::InputTextWithHint("##value", "value", &circuit_.value);
    }
    if (circuit_.tool == "trace") {
        ImGui::SameLine();
        ImGui::SetNextItemWidth(unit * 4);
        if (ImGui::BeginCombo("Layer", circuit_.layer.c_str())) {
            for (const char* l : {"F", "B", "In1", "In2"}) {
                if (ImGui::Selectable(l, circuit_.layer == l)) circuit_.layer = l;
            }
            ImGui::EndCombo();
        }
    }
    // the usual next steps, one click each
    ImGui::SameLine();
    ImGui::TextUnformatted(" ");
    if (board) {
        ImGui::SameLine();
        if (ImGui::Button("Place parts")) show(run("circuit.pcb_place", {{"doc", currentDoc_}}), "Place footprints");
        ImGui::SetItemTooltip("Put every footprint not yet on the board onto it");
        ImGui::SameLine();
        if (ImGui::Button("Route")) show(run("circuit.route", {{"doc", currentDoc_}}), "Autoroute");
        ImGui::SetItemTooltip("Draw traces for every unrouted connection");
        ImGui::SameLine();
        if (ImGui::Button("Check")) show(run("circuit.drc", {{"doc", currentDoc_}}), "Design rule check");
        ImGui::SetItemTooltip("Clearances, widths, unrouted links, drills (DRC)");
    } else {
        ImGui::SameLine();
        if (ImGui::Button("Check")) show(run("circuit.erc", {{"doc", currentDoc_}}), "Electrical check");
        ImGui::SetItemTooltip("Unconnected pins, shorted rails, duplicate references (ERC)");
    }
}

void Shell::circuitClick(ImVec2 picture) {
    // the sheet grows to fit what's on it, which moves its corner: shift the view to match,
    // so what was under the mouse stays there
    double ox = 0, oy = 0, unit = 0;
    auto show = [&](Result r, const std::string& what) {
        if (r.ok && unit > 0) {
            Result after = query("circuit.hit", {{"doc", currentDoc_}, {"x", 0.0}, {"y", 0.0},
                                                 {"view", circuit_.view == "pcb" ? std::string("pcb") : std::string("schematic")}});
            if (after.ok && std::abs(tp::number(after, "scale") - unit) < 1e-9) {
                const float dx = static_cast<float>((tp::number(after, "origin_x") - ox) * unit) * canvas_.zoom;
                const float dy = static_cast<float>((tp::number(after, "origin_y") - oy) * unit) * canvas_.zoom;
                canvas_.pan.x += dx;
                canvas_.pan.y += dy;
                circuit_.startPx.x -= dx / canvas_.zoom;
                circuit_.startPx.y -= dy / canvas_.zoom;
            }
        }
        resultFor_ = what;
        r.message = r.ok ? sentence(r.message) : friendlyError(r.message);
        result_ = std::move(r);
        canvas_.dirty = true;
    };
    Result hit = query("circuit.hit", {{"doc", currentDoc_},
                                       {"x", static_cast<double>(picture.x)},
                                       {"y", static_cast<double>(picture.y)},
                                       {"view", circuit_.view == "pcb" ? std::string("pcb") : std::string("schematic")}});
    if (!hit.ok) return show(hit, "Click");
    ox = tp::number(hit, "origin_x"), oy = tp::number(hit, "origin_y"), unit = tp::number(hit, "scale");
    const std::string kind = tp::text(hit, "kind"), part = tp::text(hit, "part");
    const double x = tp::number(hit, "x"), y = tp::number(hit, "y");
    const bool board = circuit_.view == "pcb";
    const std::string& tool = circuit_.tool;
    if (tool == "select") return show(hit, "Selected");
    if (tool == "place") {
        Params p{{"doc", currentDoc_}, {"symbol", circuit_.symbol}, {"x", x}, {"y", y}};
        if (!circuit_.value.empty()) p["value"] = circuit_.value;
        return show(run("circuit.place", p), "Place " + circuit_.symbol);
    }
    if (tool == "via") return show(run("circuit.via", {{"doc", currentDoc_}, {"x", x}, {"y", y}}), "Add via");
    if (tool == "delete") {
        if (kind == "part") return show(run("circuit.remove", {{"doc", currentDoc_}, {"ref", part}}), "Remove " + part);
        if (kind == "wire" || kind == "label") {
            return show(run("circuit.unwire", {{"doc", currentDoc_}, {"x", tp::number(hit, "exact_x")}, {"y", tp::number(hit, "exact_y")}}), "Remove wire");
        }
        return show(hit, "Nothing to delete");
    }
    if (tool == "move") {
        if (circuit_.moving.empty()) {
            if (kind != "part" && kind != "footprint" && kind != "pin" && kind != "pad") return show(hit, "Click a part to move");
            circuit_.moving = part;
            hit.message = "picked up " + part + ": click where it goes";
            return show(hit, "Move");
        }
        const std::string ref = circuit_.moving;
        circuit_.moving.clear();
        return show(run(board ? "circuit.footprint_move" : "circuit.move", {{"doc", currentDoc_}, {"ref", ref}, {"x", x}, {"y", y}}), "Move " + ref);
    }
    if (tool == "wire" || tool == "trace") {
        if (!circuit_.hasStart) {
            circuit_.hasStart = true;
            circuit_.startX = x, circuit_.startY = y;
            circuit_.startPx = picture;
            hit.message = std::string(tool == "wire" ? "wire" : "trace") + " starts at " + hit.message + "; click where it goes";
            return show(hit, tool == "wire" ? "Wire" : "Trace");
        }
        std::vector<double> pts{circuit_.startX, circuit_.startY};
        // wires go across then down, as drawn by hand on a grid
        if (tool == "wire" && circuit_.startX != x && circuit_.startY != y) pts.insert(pts.end(), {x, circuit_.startY});
        pts.insert(pts.end(), {x, y});
        Result r = tool == "wire" ? run("circuit.wire", {{"doc", currentDoc_}, {"points", pts}})
                                  : run("circuit.trace", {{"doc", currentDoc_}, {"points", pts}, {"layer", circuit_.layer}, {"shove", true}});
        // keep going from the end, unless it landed on a pin or pad (the usual end of a wire)
        if (r.ok && kind != "pin" && kind != "pad") {
            circuit_.startX = x, circuit_.startY = y;
            circuit_.startPx = picture;
        } else {
            circuit_.hasStart = false;
        }
        return show(r, tool == "wire" ? "Wire" : "Trace");
    }
}

void Shell::circuitCanvas(ImDrawList* dl, ImVec2 origin, bool hovered, bool clicked) {
    if (circuit_.doc != currentDoc_) circuit_ = CircuitCanvas{currentDoc_};
    if (circuit_.view == "3d") return;
    ImGuiIO& io = ImGui::GetIO();
    if (circuit_.hasStart && (ImGui::IsKeyPressed(ImGuiKey_Escape) || (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Right)))) circuit_.hasStart = false;
    if (!circuit_.moving.empty() && ImGui::IsKeyPressed(ImGuiKey_Escape)) circuit_.moving.clear();
    if (clicked && !ImGui::IsKeyDown(ImGuiKey_Space)) circuitClick(toImage(io.MousePos));
    // the wire or trace being drawn follows the mouse
    if (circuit_.hasStart) {
        const ImVec2 a{origin.x + circuit_.startPx.x * canvas_.zoom, origin.y + circuit_.startPx.y * canvas_.zoom};
        const ImU32 col = circuit_.tool == "trace" ? IM_COL32(200, 60, 50, 220) : IM_COL32(20, 140, 40, 220);
        dl->AddCircleFilled(a, 4, col);
        if (hovered) {
            const ImVec2 b = io.MousePos;
            if (circuit_.tool == "wire") {
                dl->AddLine(a, {b.x, a.y}, col, 2);
                dl->AddLine({b.x, a.y}, b, col, 2);
            } else {
                dl->AddLine(a, b, col, 3);
            }
        }
    }
    if (hovered && !circuit_.moving.empty()) {
        dl->AddText({io.MousePos.x + 12, io.MousePos.y + 8}, ImGui::GetColorU32(ImGuiCol_Text), circuit_.moving.c_str());
    }
}

}  // namespace wetwaresuite::gui
