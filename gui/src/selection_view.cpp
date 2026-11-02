// The Select tool and the "marching ants" outline of the selection. Dragging a
// box, oval or lasso, or clicking with the wand, runs photo.select like any
// other command, so selecting is undoable and scriptable too.
#include <algorithm>
#include <cmath>

#include "shell.hpp"
#include "wetwaresuite/raster/document.hpp"

namespace wetwaresuite::gui {

namespace {

constexpr std::size_t kMaxAntSegments = 60000;

// Shift adds, Alt takes away, both keep the overlap: the keys every editor uses.
std::string selectMode() {
    const ImGuiIO& io = ImGui::GetIO();
    if (io.KeyShift && io.KeyAlt) return "intersect";
    if (io.KeyShift) return "add";
    if (io.KeyAlt) return "subtract";
    return "replace";
}

// Edges between selected (>= 128) and unselected cells, joined into runs, on a grid `step` px wide.
std::vector<float> traceOutline(const raster::Mask& m, int step) {
    std::vector<float> out;
    const int w = (m.width + step - 1) / step, h = (m.height + step - 1) / step;
    auto in = [&](int cx, int cy) {
        if (cx < 0 || cy < 0 || cx >= w || cy >= h) return false;
        return m.at(std::min(m.width - 1, cx * step + step / 2), std::min(m.height - 1, cy * step + step / 2)) >= 128;
    };
    const float s = static_cast<float>(step);
    for (int y = 0; y <= h; ++y) {  // horizontal edges above row y
        int run = -1;
        for (int x = 0; x <= w; ++x) {
            const bool edge = x < w && in(x, y) != in(x, y - 1);
            if (edge && run < 0) run = x;
            if (!edge && run >= 0) {
                out.insert(out.end(), {run * s, y * s, x * s, y * s});
                run = -1;
            }
        }
    }
    for (int x = 0; x <= w; ++x) {  // vertical edges left of column x
        int run = -1;
        for (int y = 0; y <= h; ++y) {
            const bool edge = y < h && in(x, y) != in(x - 1, y);
            if (edge && run < 0) run = y;
            if (!edge && run >= 0) {
                out.insert(out.end(), {x * s, run * s, x * s, y * s});
                run = -1;
            }
        }
    }
    return out;
}

}  // namespace

void Shell::runSelect(Params params, const char* title) {
    params["doc"] = currentDoc_;
    Result r = run("photo.select", params);
    resultFor_ = title;
    r.message = r.ok ? sentence(r.message) : friendlyError(r.message);
    result_ = std::move(r);
}

void Shell::selectSettings() {
    const float unit = ImGui::GetFontSize();
    const char* shapes[] = {"Box", "Oval", "Lasso", "Wand"};
    int shape = static_cast<int>(canvas_.selectShape);
    barLabel("Shape");
    ImGui::SetNextItemWidth(unit * 6);
    if (ImGui::Combo("##shape", &shape, shapes, 4)) canvas_.selectShape = static_cast<SelectShape>(shape);
    ImGui::SetItemTooltip("Box and Oval: drag. Lasso: drag around the part you want. Wand: click a colour to take the area like it.");
    if (canvas_.selectShape == SelectShape::Wand) {
        ImGui::SameLine();
        barLabel("Tolerance");
        ImGui::SetNextItemWidth(unit * 6);
        ImGui::SliderFloat("##tolerance", &canvas_.tolerance, 0, 255, "%.0f");
        ImGui::SameLine();
        ImGui::Checkbox("Touching only", &canvas_.contiguous);
        ImGui::SetItemTooltip("Only take pixels joined to the one you click (off: every pixel of that colour)");
    }
    auto doc = std::dynamic_pointer_cast<raster::ImageDoc>(session_.workspace().get(currentDoc_));
    const bool has = doc && doc->hasSelection();
    ImGui::SameLine();
    ImGui::BeginDisabled(!has);
    if (ImGui::Button("Deselect")) runSelect({{"shape", std::string("none")}}, "Deselect");
    ImGui::SetItemTooltip("Select nothing, so edits change the whole layer again (Ctrl+D)");
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button("Invert")) runSelect({{"shape", std::string("invert")}}, "Invert selection");
    ImGui::SetItemTooltip("Swap what's selected and what isn't (Ctrl+Shift+I)");
    ImGui::SameLine();
    if (ImGui::Button("All")) runSelect({{"shape", std::string("all")}}, "Select all");
    ImGui::SetItemTooltip("Select the whole picture (Ctrl+A)");
    if (has) {
        ImGui::SameLine();
        if (ImGui::Button("Delete")) {
            Result r = run("photo.delete_selected", {{"doc", currentDoc_}});
            resultFor_ = "Delete selected pixels";
            r.message = r.ok ? sentence(r.message) : friendlyError(r.message);
            result_ = std::move(r);
        }
        ImGui::SetItemTooltip("Make the selected pixels of the current layer see-through (Delete)");
    }
    ImGui::PushTextWrapPos(0.0f);
    ImGui::TextDisabled("Shift adds to the selection, Alt takes away. Ribbon actions (Curves, Levels, Fill selection...) then only change what's selected.");
    ImGui::PopTextWrapPos();
    // shortcuts while the picture is up and no text box has the keyboard
    if (!ImGui::GetIO().WantTextInput) {
        if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_D) && has) runSelect({{"shape", std::string("none")}}, "Deselect");
        if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_I)) runSelect({{"shape", std::string("invert")}}, "Invert selection");
        if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_A)) runSelect({{"shape", std::string("all")}}, "Select all");
        if (has && ImGui::IsKeyPressed(ImGuiKey_Delete, false)) {
            Result r = run("photo.delete_selected", {{"doc", currentDoc_}});
            resultFor_ = "Delete selected pixels";
            r.message = r.ok ? sentence(r.message) : friendlyError(r.message);
            result_ = std::move(r);
        }
    }
}

void Shell::selectPress(ImVec2 screen) {
    const ImVec2 p = toImage(screen);
    if (canvas_.selectShape == SelectShape::Wand) {
        int w = 0, h = 0;
        if (!pictureSize(w, h) || p.x < 0 || p.y < 0 || p.x >= static_cast<float>(w) || p.y >= static_cast<float>(h)) return;
        runSelect({{"shape", std::string("wand")},
                   {"x", static_cast<double>(std::floor(p.x))},
                   {"y", static_cast<double>(std::floor(p.y))},
                   {"tolerance", static_cast<std::int64_t>(canvas_.tolerance)},
                   {"contiguous", canvas_.contiguous},
                   {"mode", selectMode()}},
                  "Magic wand");
        return;
    }
    canvas_.selecting = true;
    canvas_.selectStart = p;
    canvas_.lasso = {p.x, p.y};
}

void Shell::selectDrag(ImVec2 screen) {
    if (canvas_.selectShape != SelectShape::Lasso) return;
    const ImVec2 p = toImage(screen);
    auto& pts = canvas_.lasso;
    const double dx = p.x - pts[pts.size() - 2], dy = p.y - pts.back();
    if ((dx * dx + dy * dy) * canvas_.zoom * canvas_.zoom < 4) return;  // a point every 2 screen px is plenty
    pts.push_back(p.x);
    pts.push_back(p.y);
}

void Shell::selectRelease(ImVec2 screen) {
    canvas_.selecting = false;
    const ImVec2 a = canvas_.selectStart, b = toImage(screen);
    const float dragged = std::max(std::fabs(b.x - a.x), std::fabs(b.y - a.y)) * canvas_.zoom;
    const std::string mode = selectMode();
    if (dragged < 3) {  // a click: select nothing, like other editors
        auto doc = std::dynamic_pointer_cast<raster::ImageDoc>(session_.workspace().get(currentDoc_));
        if (mode == "replace" && doc && doc->hasSelection()) runSelect({{"shape", std::string("none")}}, "Deselect");
        return;
    }
    if (canvas_.selectShape == SelectShape::Lasso) {
        if (canvas_.lasso.size() < 6) return;
        runSelect({{"shape", std::string("lasso")}, {"points", NumberList(canvas_.lasso)}, {"mode", mode}}, "Lasso");
        return;
    }
    const double x0 = std::round(std::min(a.x, b.x)), y0 = std::round(std::min(a.y, b.y));
    const double x1 = std::round(std::max(a.x, b.x)), y1 = std::round(std::max(a.y, b.y));
    if (x1 - x0 < 1 || y1 - y0 < 1) return;
    const bool box = canvas_.selectShape == SelectShape::Rect;
    runSelect({{"shape", std::string(box ? "rect" : "ellipse")}, {"x", x0}, {"y", y0}, {"width", x1 - x0}, {"height", y1 - y0}, {"mode", mode}},
              box ? "Box select" : "Oval select");
}

void Shell::drawSelection(ImDrawList* dl, ImVec2 origin, bool hovered) {
    const float z = canvas_.zoom;
    auto toScreen = [&](double x, double y) { return ImVec2{origin.x + static_cast<float>(x) * z, origin.y + static_cast<float>(y) * z}; };

    // the shape being dragged
    if (canvas_.selecting) {
        const ImVec2 a = toScreen(canvas_.selectStart.x, canvas_.selectStart.y), b = ImGui::GetIO().MousePos;
        const ImU32 dark = IM_COL32(0, 0, 0, 220), light = IM_COL32(255, 255, 255, 220);
        if (canvas_.selectShape == SelectShape::Lasso) {
            std::vector<ImVec2> pts;
            for (std::size_t i = 0; i + 1 < canvas_.lasso.size(); i += 2) pts.push_back(toScreen(canvas_.lasso[i], canvas_.lasso[i + 1]));
            pts.push_back(b);
            dl->AddPolyline(pts.data(), static_cast<int>(pts.size()), dark, ImDrawFlags_None, 2.5f);
            dl->AddPolyline(pts.data(), static_cast<int>(pts.size()), light, ImDrawFlags_None, 1.0f);
        } else {
            const ImVec2 lo{std::min(a.x, b.x), std::min(a.y, b.y)}, hi{std::max(a.x, b.x), std::max(a.y, b.y)};
            if (canvas_.selectShape == SelectShape::Rect) {
                dl->AddRect(lo, hi, dark, 0, 0, 2.5f);
                dl->AddRect(lo, hi, light, 0, 0, 1.0f);
            } else {
                const ImVec2 c{(lo.x + hi.x) / 2, (lo.y + hi.y) / 2}, r{(hi.x - lo.x) / 2, (hi.y - lo.y) / 2};
                dl->AddEllipse(c, r, dark, 0, 0, 2.5f);
                dl->AddEllipse(c, r, light, 0, 0, 1.0f);
            }
        }
    }

    // marching ants around the selection
    auto doc = std::dynamic_pointer_cast<raster::ImageDoc>(session_.workspace().get(currentDoc_));
    animating_ = animating_ || (doc && doc->hasSelection());
    if (!doc || !doc->hasSelection()) {
        canvas_.ants.clear();
        canvas_.antsDoc.clear();
        return;
    }
    if (canvas_.antsDoc != currentDoc_ || canvas_.antsVersion != canvas_.version) {
        canvas_.antsDoc = currentDoc_;
        canvas_.antsVersion = canvas_.version;
        // noisy wand selections can have huge outlines: trace them coarser until the outline is small enough to draw
        for (int step = 1;; step *= 2) {
            canvas_.ants = traceOutline(doc->selection, step);
            canvas_.antsStep = step;
            if (canvas_.ants.size() / 4 <= kMaxAntSegments || step >= 64) break;
        }
    }
    const ImVec2 clipMin = dl->GetClipRectMin(), clipMax = dl->GetClipRectMax();
    const float phase = std::fmod(static_cast<float>(ImGui::GetTime()) * 16.0f, 8.0f);
    for (std::size_t i = 0; i + 3 < canvas_.ants.size(); i += 4) {
        ImVec2 a = toScreen(canvas_.ants[i], canvas_.ants[i + 1]), b = toScreen(canvas_.ants[i + 2], canvas_.ants[i + 3]);
        if (std::max(a.x, b.x) < clipMin.x || std::min(a.x, b.x) > clipMax.x || std::max(a.y, b.y) < clipMin.y ||
            std::min(a.y, b.y) > clipMax.y) {
            continue;
        }
        dl->AddLine(a, b, IM_COL32(0, 0, 0, 255));
        // white dashes 4 px long every 8 px, sliding along so the outline seems to move
        const bool horizontal = a.y == b.y;
        const float from = horizontal ? a.x : a.y, to = horizontal ? b.x : b.y;
        const float lo = std::max(from, horizontal ? clipMin.x : clipMin.y), hi = std::min(to, horizontal ? clipMax.x : clipMax.y);
        for (float d = std::floor((lo - phase) / 8.0f) * 8.0f + phase; d < hi; d += 8.0f) {
            const float d0 = std::max(d, lo), d1 = std::min(d + 4.0f, hi);
            if (d1 <= d0) continue;
            if (horizontal) dl->AddLine({d0, a.y}, {d1, a.y}, IM_COL32(255, 255, 255, 255));
            else dl->AddLine({a.x, d0}, {a.x, d1}, IM_COL32(255, 255, 255, 255));
        }
    }
    (void)hovered;
}

}  // namespace wetwaresuite::gui
