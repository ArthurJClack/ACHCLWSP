// Whiteboards like Miro and Excalidraw: pick, drag and resize shapes, double-click to type on
// them, stick notes, draw boxes and circles, connect shapes with arrows that stay attached, and
// draw ink. Every change is a board.* command, so it undoes and Claude sees the same board.
#include <algorithm>
#include <cmath>

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

std::string textField(const Result& r, const char* key) {
    const Value* v = r.field(key);
    auto s = v ? std::get_if<std::string>(v) : nullptr;
    return s ? *s : std::string();
}

}  // namespace

bool Shell::boardDoc() const {
    auto doc = session_.workspace().get(currentDoc_);
    return doc && doc->kind() == "board" && find("board.view") && find("board.hit") && find("board.set");
}

void Shell::boardSync() {
    BoardView& b = board_;
    if (b.doc != currentDoc_) {
        b = BoardView{};
        b.doc = currentDoc_;
    }
    if (b.seen == changes_) return;
    b.seen = changes_;
    Result v = query("board.view", {{"doc", currentDoc_}});
    if (!v.ok) return;
    const double x = numberField(v, "x"), y = numberField(v, "y"), scale = std::max(1e-6, numberField(v, "scale", 1));
    if (b.known && (std::abs(x - b.x) > 1e-6 || std::abs(y - b.y) > 1e-6 || std::abs(scale - b.scale) > 1e-9) && canvas_.zoom > 0) {
        // the picture grew or moved with the content: keep the board still on screen
        const double k = b.scale * canvas_.zoom;  // screen px per board unit, unchanged
        canvas_.pan.x -= static_cast<float>((b.x - x) * k);
        canvas_.pan.y -= static_cast<float>((b.y - y) * k);
        canvas_.zoom = static_cast<float>(k / scale);
    }
    b.x = x, b.y = y, b.w = numberField(v, "width", 1), b.h = numberField(v, "height", 1), b.scale = scale;
    b.known = true;
    if (!b.sel.empty()) {  // re-read the picked element's box (it may have moved, or gone with undo)
        Result info = query("board.info", {{"doc", currentDoc_}, {"id", b.sel}});
        if (!info.ok) {
            b.sel.clear();
            b.editing = false;
        }
    }
}

ImVec2 Shell::boardAt(ImVec2 screen) const {
    const ImVec2 img = toImage(screen);  // picture px
    return {static_cast<float>(board_.x + img.x / board_.scale), static_cast<float>(board_.y + img.y / board_.scale)};
}

ImVec2 Shell::boardScreen(ImVec2 p0, float x, float y) const {
    const float k = static_cast<float>(board_.scale) * canvas_.zoom;
    return {p0.x + (x - static_cast<float>(board_.x)) * k, p0.y + (y - static_cast<float>(board_.y)) * k};
}

void Shell::boardRun(const std::string& id, Params p, const char* title) {
    p["doc"] = currentDoc_;
    Result r = run(id, p);
    resultFor_ = title;
    r.message = r.ok ? sentence(r.message) : friendlyError(r.message);
    result_ = std::move(r);
    canvas_.dirty = true;
}

void Shell::boardSettings() {
    BoardView& b = board_;
    const CanvasTool t = canvas_.tool;
    if (t == CanvasTool::BoardNote || t == CanvasTool::BoardShape || t == CanvasTool::BoardEllipse || t == CanvasTool::BoardText ||
        t == CanvasTool::BoardPen || t == CanvasTool::BoardConnect) {
        barLabel(t == CanvasTool::BoardPen || t == CanvasTool::BoardConnect ? "Colour" : "Fill");
        if (ImGui::ColorEdit4("##boardcolour", canvas_.color, ImGuiColorEditFlags_NoInputs)) b.fillChosen = true;
        if (!b.fillChosen && t != CanvasTool::BoardPen && t != CanvasTool::BoardConnect) {
            ImGui::SameLine();
            ImGui::TextDisabled("theme colours until you pick one");
        }
        if (t == CanvasTool::BoardPen) {
            ImGui::SameLine();
            barLabel("Width");
            ImGui::SetNextItemWidth(ImGui::GetFontSize() * 5);
            ImGui::SliderFloat("##inkwidth", &canvas_.size, 1, 40, "%.0f");
        }
        return;
    }
    if (b.sel.empty()) {
        ImGui::TextDisabled("click a shape to pick it; double-click to type on it");
        return;
    }
    ImGui::Text("%s (%s)", b.sel.c_str(), b.selType.c_str());
    ImGui::SameLine();
    if (ImGui::SmallButton("Edit text")) {
        b.editing = true;
        b.focus = true;
    }
    ImGui::SameLine();
    if (ImGui::SmallButton("Front")) boardRun("board.order", {{"ids", b.sel}, {"to", std::string("front")}}, "Arrange");
    ImGui::SameLine();
    if (ImGui::SmallButton("Back")) boardRun("board.order", {{"ids", b.sel}, {"to", std::string("back")}}, "Arrange");
    ImGui::SameLine();
    if (ImGui::SmallButton("Duplicate") && find("board.duplicate")) boardRun("board.duplicate", {{"ids", b.sel}}, "Duplicate");
    ImGui::SameLine();
    float fill[4] = {canvas_.color[0], canvas_.color[1], canvas_.color[2], canvas_.color[3]};
    if (ImGui::ColorEdit4("##selfill", fill, ImGuiColorEditFlags_NoInputs)) std::copy(fill, fill + 4, canvas_.color);
    if (ImGui::IsItemDeactivatedAfterEdit()) boardRun("board.set", {{"ids", b.sel}, {b.selType == "connector" || b.selType == "ink" ? "stroke" : "fill", toHex({fill[0], fill[1], fill[2], fill[3]})}}, "Colour");
    ImGui::SetItemTooltip("Colour of the picked shape");
    ImGui::SameLine();
    if (ImGui::SmallButton("Delete")) {
        boardRun("board.remove", {{"ids", b.sel}}, "Delete");
        b.sel.clear();
    }
}

void Shell::boardEditText(ImVec2 p0) {
    BoardView& b = board_;
    if (!b.editing || b.sel.empty()) return;
    const ImVec2 a = boardScreen(p0, b.box[0], b.box[1]), c = boardScreen(p0, b.box[0] + b.box[2], b.box[1] + b.box[3]);
    const float unit = ImGui::GetFontSize();
    ImGui::SetNextWindowPos(a);
    ImGui::SetNextWindowSize({std::max(c.x - a.x, unit * 10), std::max(c.y - a.y, unit * 3)});
    ImGui::SetNextWindowBgAlpha(0.92f);
    const ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings;
    bool done = false, cancel = false;
    if (ImGui::Begin("##boardtext", nullptr, flags)) {
        if (b.focus) {
            ImGui::SetKeyboardFocusHere();
            b.focus = false;
        }
        const bool enter = ImGui::InputTextMultiline("##t", &b.text, ImGui::GetContentRegionAvail(),
                                                     ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_CtrlEnterForNewLine);
        if (ImGui::IsKeyPressed(ImGuiKey_Escape)) cancel = true;
        else if (enter || ImGui::IsItemDeactivated()) done = true;
    }
    ImGui::End();
    if (cancel) b.editing = false;
    if (done) {
        b.editing = false;
        boardRun("board.set", {{"ids", b.sel}, {"text", b.text}}, "Text");
    }
}

void Shell::boardCanvas(ImDrawList* dl, ImVec2 p0, bool hovered, bool active) {
    boardSync();
    BoardView& b = board_;
    ImGuiIO& io = ImGui::GetIO();
    const CanvasTool tool = canvas_.tool;
    const float k = static_cast<float>(b.scale) * canvas_.zoom;  // screen px per board unit
    const float reach = 6 / std::max(k, 1e-3f);
    const ImVec2 at = boardAt(io.MousePos);
    const ImU32 accent = ImGui::ColorConvertFloat4ToU32(accent_);
    const std::string colour = toHex({canvas_.color[0], canvas_.color[1], canvas_.color[2], canvas_.color[3]});

    auto pick = [&](const Result& hit) {
        b.sel = textField(hit, "id");
        b.selType = textField(hit, "type");
        b.box[0] = static_cast<float>(numberField(hit, "x")), b.box[1] = static_cast<float>(numberField(hit, "y"));
        b.box[2] = static_cast<float>(numberField(hit, "w")), b.box[3] = static_cast<float>(numberField(hit, "h"));
        b.text = textField(hit, "text");
    };
    auto hitAt = [&](ImVec2 q) {
        return query("board.hit", {{"doc", currentDoc_}, {"x", static_cast<double>(q.x)}, {"y", static_cast<double>(q.y)}, {"tolerance", static_cast<double>(reach)}});
    };

    // press
    if (hovered && ImGui::IsItemActivated() && ImGui::IsMouseDown(ImGuiMouseButton_Left) && !b.editing) {
        b.start = b.now = at;
        b.drag = 0;
        if (tool == CanvasTool::BoardSelect) {
            const bool canResize = !b.sel.empty() && b.selType != "connector" && b.selType != "ink";
            if (canResize && std::hypot(at.x - (b.box[0] + b.box[2]), at.y - (b.box[1] + b.box[3])) < reach * 1.5f) {
                b.drag = 2;
            } else {
                Result hit = hitAt(at);
                pick(hit);
                if (!b.sel.empty()) {
                    b.drag = 1;
                    if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left) && b.selType != "connector" && b.selType != "ink") {
                        b.editing = true;
                        b.focus = true;
                        b.drag = 0;
                    }
                }
            }
        } else if (tool == CanvasTool::BoardConnect) {
            Result hit = hitAt(at);
            b.from = textField(hit, "type") == "connector" ? std::string() : textField(hit, "id");
            b.drag = 4;
        } else if (tool == CanvasTool::BoardPen) {
            b.ink = {at.x, at.y};
            b.drag = 5;
        } else {
            b.drag = 3;
        }
    }
    // drag
    if (b.drag && active) {
        b.now = at;
        if (b.drag == 5) {
            const double lx = b.ink[b.ink.size() - 2], ly = b.ink.back();
            if (std::hypot(at.x - lx, at.y - ly) > 1.5 / std::max(k, 1e-3f)) b.ink.push_back(at.x), b.ink.push_back(at.y);
        }
    }
    // release
    if (b.drag && !active) {
        const int drag = b.drag;
        b.drag = 0;
        const float dx = b.now.x - b.start.x, dy = b.now.y - b.start.y;
        const bool moved = std::hypot(dx, dy) * k > 3;
        if (drag == 1 && moved) {
            boardRun("board.set", {{"ids", b.sel}, {"dx", static_cast<double>(dx)}, {"dy", static_cast<double>(dy)}}, "Move");
            b.box[0] += dx, b.box[1] += dy;
        } else if (drag == 2 && moved) {
            const double w = std::max(10.0f, b.box[2] + dx), h = std::max(10.0f, b.box[3] + dy);
            boardRun("board.set", {{"ids", b.sel}, {"w", w}, {"h", h}}, "Resize");
            b.box[2] = static_cast<float>(w), b.box[3] = static_cast<float>(h);
        } else if (drag == 3) {
            // a click makes a default-sized shape centred there; a drag makes it that size
            std::string type = tool == CanvasTool::BoardNote ? "note" : tool == CanvasTool::BoardEllipse ? "ellipse" : tool == CanvasTool::BoardText ? "text" : "rect";
            double w = tool == CanvasTool::BoardNote ? 180 : tool == CanvasTool::BoardEllipse ? 150 : 170, h = tool == CanvasTool::BoardNote ? 160 : 100;
            double x = b.start.x - w / 2, y = b.start.y - h / 2;
            if (moved) x = std::min(b.start.x, b.now.x), y = std::min(b.start.y, b.now.y), w = std::abs(dx), h = std::abs(dy);
            Params p{{"type", type}, {"x", x}, {"y", y}};
            if (type != "text") p["w"] = w, p["h"] = h;
            else p["text"] = std::string("Text");
            if (b.fillChosen) p[type == "text" ? "text_color" : "fill"] = colour;
            Params full = p;
            full["doc"] = currentDoc_;
            Result r = run("board.add", full);
            resultFor_ = "Add";
            canvas_.dirty = true;
            if (r.ok) {  // type on it straight away
                b.sel = textField(r, "id");
                b.selType = type;
                b.box[0] = static_cast<float>(numberField(r, "x", x)), b.box[1] = static_cast<float>(numberField(r, "y", y));
                b.box[2] = static_cast<float>(numberField(r, "w", w)), b.box[3] = static_cast<float>(numberField(r, "h", h));
                b.text = type == "text" ? "Text" : "";
                b.editing = true;
                b.focus = true;
                canvas_.tool = CanvasTool::BoardSelect;  // like Miro: back to picking after one
            }
            r.message = r.ok ? sentence(r.message) : friendlyError(r.message);
            result_ = std::move(r);
        } else if (drag == 4 && moved) {
            Result hit = hitAt(b.now);
            const std::string to = textField(hit, "type") == "connector" ? std::string() : textField(hit, "id");
            Params p;
            if (b.fillChosen) p["stroke"] = colour;
            if (!b.from.empty()) p["from"] = b.from;
            else p["start"] = NumberList{b.start.x, b.start.y};
            if (!to.empty() && to != b.from) p["to"] = to;
            else p["end"] = NumberList{b.now.x, b.now.y};
            boardRun("board.connect", p, "Connect");
        } else if (drag == 5 && b.ink.size() >= 4) {
            Params p{{"points", NumberList(b.ink.begin(), b.ink.end())}, {"width", static_cast<double>(std::clamp(canvas_.size, 1.0f, 40.0f))}};
            if (b.fillChosen) p["color"] = colour;
            boardRun("board.ink", p, "Ink");
            b.ink.clear();
        }
    }
    // keys
    if (hovered && !io.WantTextInput && !b.sel.empty() && !b.editing) {
        if (ImGui::IsKeyPressed(ImGuiKey_Delete) || ImGui::IsKeyPressed(ImGuiKey_Backspace)) {
            boardRun("board.remove", {{"ids", b.sel}}, "Delete");
            b.sel.clear();
        } else if (ImGui::IsKeyPressed(ImGuiKey_Enter) && b.selType != "connector" && b.selType != "ink") {
            b.editing = true;
            b.focus = true;
        } else {
            const struct {
                ImGuiKey key;
                double dx, dy;
            } nudges[] = {{ImGuiKey_LeftArrow, -1, 0}, {ImGuiKey_RightArrow, 1, 0}, {ImGuiKey_UpArrow, 0, -1}, {ImGuiKey_DownArrow, 0, 1}};
            for (const auto& n : nudges) {
                if (!ImGui::IsKeyPressed(n.key)) continue;
                const double step = io.KeyShift ? 20 : 2;
                boardRun("board.set", {{"ids", b.sel}, {"dx", n.dx * step}, {"dy", n.dy * step}}, "Nudge");
                b.box[0] += static_cast<float>(n.dx * step), b.box[1] += static_cast<float>(n.dy * step);
            }
        }
    }

    // draw: the picked element, the shape or line being made
    if (!b.sel.empty()) {
        float bx = b.box[0], by = b.box[1], bw = b.box[2], bh = b.box[3];
        if (b.drag == 1) bx += b.now.x - b.start.x, by += b.now.y - b.start.y;
        if (b.drag == 2) bw = std::max(10.0f, bw + b.now.x - b.start.x), bh = std::max(10.0f, bh + b.now.y - b.start.y);
        const ImVec2 a = boardScreen(p0, bx, by), c = boardScreen(p0, bx + bw, by + bh);
        dl->AddRect({a.x - 2, a.y - 2}, {c.x + 2, c.y + 2}, accent, 3, 0, 1.5f);
        if (b.selType != "connector" && b.selType != "ink") {
            dl->AddCircleFilled(c, 5, IM_COL32(255, 255, 255, 255));
            dl->AddCircle(c, 5, accent, 0, 1.5f);
        }
    }
    if (b.drag == 3) {
        const ImVec2 a = boardScreen(p0, b.start.x, b.start.y), c = boardScreen(p0, b.now.x, b.now.y);
        if (tool == CanvasTool::BoardEllipse) dl->AddEllipse({(a.x + c.x) / 2, (a.y + c.y) / 2}, {std::abs(c.x - a.x) / 2, std::abs(c.y - a.y) / 2}, accent, 0, 0, 2);
        else dl->AddRect({std::min(a.x, c.x), std::min(a.y, c.y)}, {std::max(a.x, c.x), std::max(a.y, c.y)}, accent, 4, 0, 2);
    }
    if (b.drag == 4) dl->AddLine(boardScreen(p0, b.start.x, b.start.y), io.MousePos, accent, 2);
    if (b.drag == 5 && b.ink.size() >= 4) {
        std::vector<ImVec2> pts;
        for (std::size_t i = 0; i + 1 < b.ink.size(); i += 2) pts.push_back(boardScreen(p0, static_cast<float>(b.ink[i]), static_cast<float>(b.ink[i + 1])));
        dl->AddPolyline(pts.data(), static_cast<int>(pts.size()), ImGui::ColorConvertFloat4ToU32({canvas_.color[0], canvas_.color[1], canvas_.color[2], 1}), ImDrawFlags_None,
                        std::max(1.0f, canvas_.size * k));
    }
    boardEditText(p0);
}

}  // namespace wetwaresuite::gui
