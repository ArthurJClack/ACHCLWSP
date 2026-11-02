// The canvas: shows the open document's picture with zoom and pan, and turns
// mouse and pen input into paint.stroke / paint.erase / paint.fill commands
// (Heal and Remove strokes go to retouch.cpp).
// Images are drawn from a mipmap pyramid: zoomed out, the texture holds a half,
// quarter, ... size copy instead of the full picture, and after an edit only the
// changed area is re-flattened and re-uploaded.
#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>

#include "shell.hpp"
// after imgui.h
#include "imgui_internal.h"
#include "imgui_stdlib.h"
#include "wetwaresuite/raster/codec.hpp"
#include "wetwaresuite/raster/document.hpp"
#include "wetwaresuite/raster/pyramid.hpp"

namespace wetwaresuite::gui {

namespace {

bool hasParam(const CommandInfo* info, const std::string& name) {
    if (!info) return false;
    for (const auto& p : info->params) {
        if (p.name == name) return true;
    }
    return false;
}

// The pyramid of the image on the canvas, and which of its levels the texture holds.
struct MipCache {
    std::weak_ptr<raster::ImageDoc> doc;
    raster::Pyramid pyramid;
    int level = -1;
};

std::map<const void*, MipCache>& mipCaches() {
    static std::map<const void*, MipCache> caches;  // one per window
    return caches;
}

int maxTextureSide(SDL_Renderer* renderer) {
    const int side = static_cast<int>(
        SDL_GetNumberProperty(SDL_GetRendererProperties(renderer), SDL_PROP_RENDERER_MAX_TEXTURE_SIZE_NUMBER, 4096));
    return side > 0 ? side : 4096;
}

// Puts the right pyramid level for `zoom` on the texture: a full upload when the level changes,
// else (when `changes` is set) just the area the last update touched.
bool showLevel(SDL_Renderer* renderer, Texture& tex, MipCache& mc, float zoom, bool changes) {
    if (mc.pyramid.levels() == 0) return false;
    const int level = mc.pyramid.levelFor(zoom, maxTextureSide(renderer));
    const raster::Image& img = mc.pyramid.level(level);
    if (level != mc.level || !tex.tex || tex.width != img.width() || tex.height != img.height()) {
        if (!tex.upload(renderer, img.data(), img.width(), img.height())) return false;
        mc.level = level;
    } else if (changes) {
        const raster::PixelRect r = mc.pyramid.changed(level);
        if (!r.empty()) {
            const SDL_Rect rect{r.x0, r.y0, r.width(), r.height()};
            SDL_UpdateTexture(tex.tex, &rect, img.pixel(r.x0, r.y0), img.width() * 4);
        }
    }
    // the zoom maths works in full-size picture px whatever level is shown
    tex.srcWidth = mc.pyramid.level(0).width();
    tex.srcHeight = mc.pyramid.level(0).height();
    return true;
}

}  // namespace

bool Shell::toolAvailable(CanvasTool tool) const {
    if (tool == CanvasTool::Move) return true;
    if (tool == CanvasTool::Pick || tool == CanvasTool::Anchor) return vectorDoc();
    if (tool == CanvasTool::VecRect) return vectorDoc() && find("vector.add_rect");
    if (tool == CanvasTool::VecEllipse) return vectorDoc() && find("vector.add_ellipse");
    if (tool == CanvasTool::VecLine) return vectorDoc() && find("vector.add_line");
    if (tool == CanvasTool::VecPen) return vectorDoc() && find("vector.add_path");
    if (tool == CanvasTool::VecText) return vectorDoc() && find("vector.add_text");
    if (tool == CanvasTool::VecGradient) return vectorDoc() && find("vector.gradient");
    if (tool == CanvasTool::VecBuilder) return vectorDoc() && find("vector.shape_builder") && find("vector.select_area");
    if (glyphTool(tool)) return fontDoc();
    if (boardTool(tool)) {
        if (!boardDoc()) return false;
        if (tool == CanvasTool::BoardConnect) return find("board.connect") != nullptr;
        if (tool == CanvasTool::BoardPen) return find("board.ink") != nullptr;
        return find("board.add") != nullptr;
    }
    auto doc = session_.workspace().get(currentDoc_);
    if (!doc || doc->kind() != "image") return false;
    switch (tool) {
        case CanvasTool::Brush: return find("paint.stroke") != nullptr;
        case CanvasTool::Eraser: return find("paint.erase") != nullptr;
        case CanvasTool::Fill: return find("paint.fill") != nullptr;
        case CanvasTool::Heal: return find("photo.heal") != nullptr;
        case CanvasTool::Remove: return find("photo.remove_object") != nullptr;
        case CanvasTool::Select: return find("photo.select") != nullptr;
        case CanvasTool::Type: return find("paint.text") != nullptr;
        case CanvasTool::Transform: return find("photo.transform") != nullptr && find("photo.transform_box") != nullptr;
        case CanvasTool::Clone: return find("paint.clone") != nullptr;
        case CanvasTool::Dodge:
        case CanvasTool::Burn:
        case CanvasTool::Sponge: return find("paint.dodge") != nullptr;
        case CanvasTool::Smudge: return find("paint.smudge") != nullptr;
        case CanvasTool::Blur: return find("paint.blur_brush") != nullptr;
        default: return false;
    }
}

void Shell::refreshCanvas() {
    if (!canvas_.dirty && canvas_.shownDoc == currentDoc_) return;
    canvas_.dirty = false;
    canvas_.shownDoc = currentDoc_;
    ++canvas_.version;
    canvas_.summary.clear();
    auto doc = session_.workspace().get(currentDoc_);
    if (!doc) {
        mipCaches().erase(this);
        canvas_.texture.reset();
        return;
    }
    canvas_.summary = doc->summary();
    const bool newDoc = canvas_.shown3dDoc != currentDoc_;
    canvas_.shown3dDoc = currentDoc_;
    canvas_.is3d = probe3d(*doc);
    if (canvas_.is3d) {
        mipCaches().erase(this);
        if (newDoc) canvas_.orbit = OrbitView{};  // a different model starts from iso
        canvas_.orbitDirty = true;               // drawn by orbitCanvas at the view's size
        return;
    }
    if (auto img = std::dynamic_pointer_cast<raster::ImageDoc>(doc); img && img->image.width > 0 && img->image.height > 0) {
        MipCache& mc = mipCaches()[this];
        const raster::ImageDoc::Changes changes = img->takeChanges();
        if (mc.doc.lock() != img || changes.all || !mc.pyramid.update(img->image, changes.rect)) {
            mc.pyramid.build(img->image);
            mc.level = -1;
        }
        mc.doc = img;
        // a pending "fit" would zoom out right away, so start on the level it will need
        float zoom = canvas_.zoom;
        if (canvas_.fit && canvas_.viewSize.x > 0 && canvas_.viewSize.y > 0) {
            zoom = std::min({canvas_.viewSize.x * 0.95f / static_cast<float>(img->image.width),
                             canvas_.viewSize.y * 0.95f / static_cast<float>(img->image.height), 8.0f});
        }
        if (showLevel(renderer_, canvas_.texture, mc, zoom, true)) return;
    }
    mipCaches().erase(this);
    if (newDoc) canvas_.texture.reset();  // not another document's picture while the first frame is drawn
    if (timelineCanvas(doc)) return;
    // a level or sprite: the picture its click tools work on (game_view.cpp)
    std::string png = gamePicture();
    const bool game = !png.empty();
    // anything else: the document's own preview, else the tool's preview command
    if (png.empty() && deckDoc()) png = deckPicture();  // the slide shown big, not the sorter
    if (png.empty() && layoutDoc()) png = layoutPicture();  // the page shown, sharp enough to work on
    if (png.empty() && doc->kind() == "audio" && find("audio.preview") && canvas_.viewSize.x > 64 && canvas_.viewSize.y > 64) {
        // drawn to fill the view, so the whole length is wide and easy to select in
        canvas_.waveSize = canvas_.viewSize;
        const auto px = [](float v, float lo, float hi) { return static_cast<std::int64_t>(std::clamp(v * 0.95f, lo, hi)); };
        Result r = session_.run("audio.preview", {{"doc", currentDoc_},
                                                  {"width", px(canvas_.viewSize.x, 300, 4000)},
                                                  {"height", px(canvas_.viewSize.y, 120, 2000)}});
        if (r.ok && !r.attachments.empty()) png = r.attachments[0].bytes;
        canvas_.fit = true;
    }
    if (png.empty() && doc->kind() == "vector" && find("vector.preview")) {  // sharper than the small built-in preview, for zooming in
        Result r = session_.run("vector.preview", {{"doc", currentDoc_}, {"size", std::int64_t{1400}}});
        if (r.ok && !r.attachments.empty()) png = r.attachments[0].bytes;
    }
    if (png.empty()) {
        if (auto att = doc->preview()) png = att->bytes;
    }
    if (png.empty()) {
        std::vector<std::string> owners;
        if (auto it = docTool_.find(currentDoc_); it != docTool_.end()) owners.push_back(it->second);
        for (const auto& t : tools_) owners.push_back(t);
        for (const auto& tool : owners) {
            const CommandInfo* preview = find(tool + ".preview");
            if (!hasParam(preview, "doc")) continue;
            // read-only, so it stays out of the history
            Result r = session_.run(preview->id, {{"doc", currentDoc_}});
            if (!r.ok) continue;
            for (const auto& a : r.attachments) {
                if (a.mime.rfind("image/", 0) == 0) {
                    png = a.bytes;
                    break;
                }
            }
            if (!png.empty()) break;
        }
    }
    if (png.empty()) {
        canvas_.texture.reset();
        return;
    }
    try {
        raster::Image pic = raster::decode(png);
        if (game) {  // crisp pixels when zoomed in, and the colours for the picker
            canvas_.game.picture.assign(pic.data(), pic.data() + static_cast<std::size_t>(pic.width()) * pic.height() * 4);
            canvas_.game.pictureW = pic.width();
            canvas_.game.pictureH = pic.height();
        }
        if (!canvas_.texture.upload(renderer_, pic.data(), pic.width(), pic.height())) canvas_.texture.reset();
        else if (game) SDL_SetTextureScaleMode(canvas_.texture.tex, SDL_SCALEMODE_NEAREST);
    } catch (const std::exception&) {
        canvas_.texture.reset();
    }
}

namespace {

// The picture and drawing tools in the rail, with their Photoshop letters.
struct CanvasToolDef {
    const char* name;
    CanvasTool tool;
    const char* key;
    const char* tip;
};
const CanvasToolDef kCanvasTools[] = {
    {"Move", CanvasTool::Move, "V", "Drag to move around, scroll to zoom"},
    {"Pick", CanvasTool::Pick, "Q",
     "Click a shape to pick it, drag to move it, drag a corner or edge handle to resize (Shift keeps the "
     "proportions). Arrow keys nudge, Delete removes"},
    {"Anchors", CanvasTool::Anchor, "A",
     "Click a path to show its anchors and handles, drag them to reshape it (Alt breaks a smooth pair). "
     "Alt+click on the path adds an anchor"},
    {"Rectangle", CanvasTool::VecRect, "M", "Drag to draw a rectangle (Shift makes a square)"},
    {"Ellipse", CanvasTool::VecEllipse, "L", "Drag to draw an ellipse (Shift makes a circle)"},
    {"Line", CanvasTool::VecLine, "", "Drag to draw a straight line"},
    {"Pen", CanvasTool::VecPen, "P",
     "Click to put down corners. Click the first one again to close the shape (filled); Enter or double-click ends an "
     "open line; Esc throws it away"},
    {"Text", CanvasTool::VecText, "T", "Click where the words go, type, press Enter"},
    {"Gradient", CanvasTool::VecGradient, "G",
     "Drag across a shape to fill it with a gradient from the first colour to the second, in the drag's direction "
     "(Alt: radial)"},
    {"Builder", CanvasTool::VecBuilder, "^M",
     "Shape builder: drag across overlapping shapes to merge the pieces you cross into one shape; Alt+drag deletes them"},
    {"Select", CanvasTool::BoardSelect, "V",
     "Click to pick, drag to move, drag the corner dot to resize. Double-click a shape or note to type on it. Delete removes"},
    {"Note", CanvasTool::BoardNote, "N", "Click to stick a note and type on it straight away (drag to make it a size)"},
    {"Box", CanvasTool::BoardShape, "R", "Click or drag to draw a box, then type its label"},
    {"Circle", CanvasTool::BoardEllipse, "O", "Click or drag to draw an ellipse, then type its label"},
    {"Words", CanvasTool::BoardText, "T", "Click and type: free text on the board"},
    {"Connect", CanvasTool::BoardConnect, "C",
     "Drag from one shape to another: the arrow stays attached when they move. Let go on empty board for a loose end"},
    {"Ink", CanvasTool::BoardPen, "P", "Draw freehand with the mouse or a pen (pressure works)"},
    {"Edit glyph", CanvasTool::GlyphEdit, "A",
     "One letter big with its metrics: drag its points to reshape it (handles follow their point). Type another letter on the bar"},
    {"Draw glyph", CanvasTool::GlyphDraw, "P", "Click corners to draw a new contour into the letter; click the first corner to close it (tick Cut a hole for a counter)"},
    {"Select", CanvasTool::Select, "M",
     "Choose the part of the picture to change: drag a box, oval or lasso, or click with the wand. Shift adds, "
     "Alt takes away. Edits then only change inside it"},
    {"Transform", CanvasTool::Transform, "^T",
     "Free transform: handles around the selection (or the layer). Drag corners to scale, edges to stretch, outside to "
     "rotate, Ctrl+corner for perspective; Enter keeps it"},
    {"Type", CanvasTool::Type, "T", "Click on the picture and type: a text layer appears as you write. Enter keeps it"},
    {"Brush", CanvasTool::Brush, "B", "Paint with the mouse or a pen (pressure works)"},
    {"Eraser", CanvasTool::Eraser, "E", "Rub out paint on the current layer"},
    {"Fill", CanvasTool::Fill, "G", "Click to fill an area with the colour, like a paint bucket"},
    {"Heal", CanvasTool::Heal, "J",
     "Paint over a spot, pimple or scratch to make it disappear. To copy texture from a clean spot, "
     "Alt+click it first (Option+click on a Mac)"},
    {"Remove", CanvasTool::Remove, "R",
     "Paint over something you don't want (a person, a sign, a wire), then press Remove: the gap is filled "
     "from the picture around it"},
    {"Clone", CanvasTool::Clone, "S",
     "Paint with a copy of another part of the picture: Alt+click (Option+click on a Mac) the spot to copy "
     "from, then paint"},
    {"Dodge", CanvasTool::Dodge, "O", "Paint to lighten (like dodging a print)"},
    {"Burn", CanvasTool::Burn, "", "Paint to darken"},
    {"Sponge", CanvasTool::Sponge, "", "Paint to wash the colour out"},
    {"Smudge", CanvasTool::Smudge, "W", "Drag colours along like a finger in wet paint"},
    {"Blur", CanvasTool::Blur, "U", "Paint to soften (or sharpen) just there"}};

}  // namespace

void Shell::canvasTools() {
    const CanvasTool editing[] = {CanvasTool::GlyphEdit, CanvasTool::BoardSelect, CanvasTool::Pick, CanvasTool::Anchor, CanvasTool::Select, CanvasTool::Transform, CanvasTool::Type, CanvasTool::Brush, CanvasTool::Eraser, CanvasTool::Fill, CanvasTool::Heal,
                                  CanvasTool::Remove, CanvasTool::Clone, CanvasTool::Dodge, CanvasTool::Smudge, CanvasTool::Blur};
    if (std::none_of(std::begin(editing), std::end(editing), [this](CanvasTool t) { return toolAvailable(t); })) return;
    for (const auto& t : kCanvasTools) {
        if (!toolAvailable(t.tool)) continue;
        if (railItem(t.name, t.key, t.tip, canvas_.tool == t.tool)) canvas_.tool = t.tool;
    }
}

void Shell::railTools() {
    auto doc = session_.workspace().get(currentDoc_);
    if (!doc || canvas_.is3d) return;
    if (doc->kind() == "pdf" && find("pdf.view")) return;  // the PDF editor has its own tools
    if (circuitDoc()) return circuitTools();
    if (mapsDoc()) return mapsTools();
    if (gameActive()) return gameTools();
    if (board_.doc != currentDoc_ && boardDoc()) {  // a whiteboard opens ready to pick and type, like Miro
        board_ = BoardView{};
        board_.doc = currentDoc_;
        if (canvas_.tool == CanvasTool::Move || !boardTool(canvas_.tool)) canvas_.tool = CanvasTool::BoardSelect;
    }
    canvasTools();
    // Ctrl+T: free transform, as in Photoshop
    const ImGuiIO& io = ImGui::GetIO();
    if (!io.WantTextInput && io.KeyCtrl && !io.KeyShift && ImGui::IsKeyPressed(ImGuiKey_T, false) && toolAvailable(CanvasTool::Transform)) {
        canvas_.tool = CanvasTool::Transform;
        if (!xform_.active) transformStart();
    }
}

void Shell::canvasToolbar() {
    if (canvas_.is3d) {
        if (sketchActive()) sketchToolbar();
        else orbitToolbar();
        return;
    }
    if (ImGui::Button("Fit")) canvas_.fit = true;
    ImGui::SetItemTooltip("Show the whole picture");
    ImGui::SameLine();
    if (ImGui::Button("100%")) {
        const ImVec2 centre{canvas_.viewSize.x / 2, canvas_.viewSize.y / 2};
        const ImVec2 img{(centre.x - canvas_.pan.x) / canvas_.zoom, (centre.y - canvas_.pan.y) / canvas_.zoom};
        canvas_.zoom = 1;
        canvas_.pan = {centre.x - img.x, centre.y - img.y};
    }
    ImGui::SameLine();
    ImGui::TextDisabled("%d%%", static_cast<int>(std::lround(canvas_.zoom * 100)));
    if (circuitDoc()) {
        circuitToolbar();
        return;
    }
    if (const std::string opener = docOpener(currentDoc_); opener == "office.doc_new" || opener == "office.doc_open") {
        writeBar();
        return;
    }
    if (mapsDoc()) {
        mapsToolbar();
        return;
    }
    if (gameActive()) {
        gameToolbar();
        return;
    }
    if (canvas_.tool == CanvasTool::Move || !toolAvailable(canvas_.tool)) return;
    for (const auto& t : kCanvasTools) {
        if (t.tool != canvas_.tool) continue;
        ImGui::SameLine();
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted((std::string("   ") + t.name + ":").c_str());  // the options bar names its tool, like Photoshop's
        ImGui::SameLine();
    }
    if (canvas_.tool == CanvasTool::Pick || canvas_.tool == CanvasTool::Anchor) {
        vectorSettings();
        return;
    }
    if (boardTool(canvas_.tool)) {
        boardSettings();
        return;
    }
    if (glyphTool(canvas_.tool)) {
        glyphSettings();
        return;
    }
    if (canvas_.tool == CanvasTool::VecGradient) {
        barLabel("From");
        ImGui::ColorEdit4("##gradfrom", canvas_.color, ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_AlphaBar);
        ImGui::SameLine();
        barLabel("To");
        ImGui::ColorEdit4("##gradto", canvas_.color2, ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_AlphaBar);
        ImGui::SameLine();
        ImGui::TextDisabled("drag across a shape (Alt: radial)");
        return;
    }
    if (canvas_.tool == CanvasTool::VecBuilder) {
        ImGui::TextDisabled("drag across overlapping shapes to merge the pieces; Alt+drag deletes pieces");
        return;
    }
    if (vectorDrawTool(canvas_.tool)) {
        barLabel(canvas_.tool == CanvasTool::VecLine || canvas_.tool == CanvasTool::VecPen || canvas_.tool == CanvasTool::VecText ? "Colour" : "Fill");
        ImGui::ColorEdit4("##shapecolour", canvas_.color, ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_AlphaBar);
        return;
    }
    if (canvas_.tool == CanvasTool::Heal || canvas_.tool == CanvasTool::Remove) {
        retouchSettings();
        return;
    }
    if (canvas_.tool == CanvasTool::Select) {
        selectSettings();
        return;
    }
    if (canvas_.tool == CanvasTool::Transform) {
        transformToolbar();
        return;
    }
    if (canvas_.tool == CanvasTool::Type) {
        barLabel("Colour");
        ImGui::ColorEdit4("##typecolour", canvas_.color, ImGuiColorEditFlags_NoInputs);
        ImGui::SameLine();
        ImGui::TextDisabled("click where the words go and type; Paint > Edit text changes them later");
        return;
    }
    if (brushTool(canvas_.tool)) {
        brushToolSettings();
        return;
    }
    // the tool's settings follow its name on the options bar
    const float unit = ImGui::GetFontSize();
    bool first = false;
    auto next = [&first] {
        if (!first) ImGui::SameLine();
        first = false;
    };
    if (canvas_.tool == CanvasTool::Brush || canvas_.tool == CanvasTool::Fill) {
        next();
        barLabel("Colour");
        ImGui::ColorEdit4("##colour", canvas_.color, ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_AlphaBar);
    }
    if (canvas_.tool == CanvasTool::Fill) return;
    if (const CommandInfo* stroke = find("paint.stroke")) {
        for (const auto& p : stroke->params) {
            if (p.name != "brush" || p.choices.empty()) continue;
            next();
            barLabel("Tip");
            ImGui::SetNextItemWidth(unit * 6);
            if (ImGui::BeginCombo("##tip", canvas_.brush.c_str())) {
                for (const auto& c : p.choices) {
                    if (ImGui::Selectable(c.c_str(), c == canvas_.brush)) canvas_.brush = c;
                }
                ImGui::EndCombo();
            }
        }
    }
    next();
    barLabel("Size");
    ImGui::SetNextItemWidth(unit * 5.5f);
    ImGui::SliderFloat("##size", &canvas_.size, 1, 300, "%.0f px", ImGuiSliderFlags_Logarithmic);
    next();
    barLabel("Opacity");
    ImGui::SetNextItemWidth(unit * 4.5f);
    ImGui::SliderFloat("##opacity", &canvas_.opacity, 1, 100, "%.0f%%");
}

// A text document: type a paragraph under the toolbar and Enter adds it to the end, so writing
// doesn't need the Add form every time.
void Shell::writeBar() {
    const float unit = ImGui::GetFontSize();
    static const char* kinds[] = {"paragraph", "heading", "list", "numbered", "quote"};
    ImGui::SameLine(0, unit);
    ImGui::SetNextItemWidth(unit * 6.5f);
    if (ImGui::BeginCombo("##kind", writeKind_.c_str())) {
        for (const char* k : kinds) {
            if (ImGui::Selectable(k, writeKind_ == k)) writeKind_ = k;
        }
        ImGui::EndCombo();
    }
    ImGui::SetItemTooltip("What the next line adds");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(-1);
    // the box stays active after Enter, so the next line can be typed straight away
    ImGuiIO& io = ImGui::GetIO();
    const bool keep = io.ConfigInputTextEnterKeepActive;
    io.ConfigInputTextEnterKeepActive = true;
    const bool entered = ImGui::InputTextWithHint("##write", "Type here, Enter adds it to the end (**bold**, *italic*)", &writeText_,
                                                  ImGuiInputTextFlags_EnterReturnsTrue);
    io.ConfigInputTextEnterKeepActive = keep;
    if (entered && !writeText_.empty()) {
        Result r = run("office.doc_add", {{"doc", currentDoc_}, {"kind", writeKind_}, {"text", writeText_}});
        resultFor_ = "Add";
        r.message = r.ok ? sentence(r.message) : friendlyError(r.message);
        if (r.ok) {
            writeText_.clear();
            if (ImGuiInputTextState* box = ImGui::GetInputTextState(ImGui::GetItemID())) box->ReloadUserBufAndMoveToEnd();
        }
        result_ = std::move(r);
        canvas_.dirty = true;
    }
}

ImVec2 Shell::toImage(ImVec2 screen) const {
    // the texture may be smaller than the picture; the zoom is in picture px
    return {(screen.x - canvas_.viewMin.x - canvas_.pan.x) / canvas_.zoom,
            (screen.y - canvas_.viewMin.y - canvas_.pan.y) / canvas_.zoom};
}

void Shell::beginStroke(ImVec2 screen, float pressure, bool pen, bool eraser) {
    canvas_.stroking = true;
    canvas_.usedPen = pen;
    canvas_.penErasing = eraser;
    canvas_.points.clear();
    canvas_.pressures.clear();
    addStrokePoint(screen, pressure);
}

void Shell::addStrokePoint(ImVec2 screen, float pressure) {
    if (!canvas_.stroking) return;
    const ImVec2 p = toImage(screen);
    auto& pts = canvas_.points;
    if (pts.size() >= 2) {
        const double dx = p.x - pts[pts.size() - 2], dy = p.y - pts.back();
        if (dx * dx + dy * dy < 0.5) return;
    }
    pts.push_back(p.x);
    pts.push_back(p.y);
    canvas_.pressures.push_back(std::clamp(pressure, 0.0f, 1.0f));
}

void Shell::endStroke() {
    if (!canvas_.stroking) return;
    canvas_.stroking = false;
    if (canvas_.points.size() < 2) return;
    if (canvas_.tool == CanvasTool::Heal || canvas_.tool == CanvasTool::Remove) {
        retouchRelease();
        return;
    }
    if (brushTool(canvas_.tool)) {
        runBrushTool();
        return;
    }
    const bool erase = canvas_.tool == CanvasTool::Eraser || canvas_.penErasing;
    const std::string id = erase ? "paint.erase" : "paint.stroke";
    const CommandInfo* info = find(id);
    if (!info) return;
    Params p{{"doc", currentDoc_}, {"points", canvas_.points}};
    if (hasParam(info, "brush")) p["brush"] = canvas_.brush;
    if (hasParam(info, "size")) p["size"] = static_cast<double>(canvas_.size);
    if (hasParam(info, "opacity")) p["opacity"] = static_cast<double>(canvas_.opacity);
    if (!erase && hasParam(info, "color")) p["color"] = toHex({canvas_.color[0], canvas_.color[1], canvas_.color[2], canvas_.color[3]});
    if (canvas_.usedPen && hasParam(info, "pressures")) {
        p["pressures"] = NumberList(canvas_.pressures.begin(), canvas_.pressures.end());
    }
    Result r = run(id, p);
    resultFor_ = info->title;
    r.message = r.ok ? sentence(r.message) : friendlyError(r.message);
    result_ = std::move(r);
}

void Shell::onEvent(const SDL_Event& e) {
    switch (e.type) {
        case SDL_EVENT_PEN_PROXIMITY_IN: canvas_.usedPen = canvas_.usedPen || canvas_.stroking; break;
        case SDL_EVENT_PEN_DOWN:
            penDown_ = true;
            penEraser_ = e.ptouch.eraser;
            break;
        case SDL_EVENT_PEN_UP: penDown_ = false; break;
        case SDL_EVENT_PEN_AXIS:
            if (e.paxis.axis == SDL_PEN_AXIS_PRESSURE) penPressure_ = e.paxis.value;
            break;
        case SDL_EVENT_PEN_MOTION:
            if (canvas_.stroking && canvas_.usedPen) addStrokePoint({e.pmotion.x, e.pmotion.y}, penPressure_);
            break;
        case SDL_EVENT_MOUSE_MOTION:
            if (canvas_.stroking && !canvas_.usedPen && e.motion.which != SDL_PEN_MOUSEID) addStrokePoint({e.motion.x, e.motion.y}, 1.0f);
            break;
        case SDL_EVENT_WINDOW_FOCUS_GAINED:  // another app may have sent something or changed the colours
            refreshInbox();
            paletteStale_ = true;
            installedStale_ = true;
            break;
        case SDL_EVENT_DROP_FILE:
            if (e.drop.data) dropFile(e.drop.data);
            break;
        default: break;
    }
}

void Shell::canvasView() {
    if (deckDoc()) {
        // slides: the slide strip on the left, like PowerPoint
        const ImVec2 avail = ImGui::GetContentRegionAvail();
        const float side = std::min(ImGui::GetFontSize() * 13, avail.x * 0.3f);
        ImGui::BeginChild("deck_side", {side, avail.y}, ImGuiChildFlags_Borders);
        deckStrip();
        ImGui::EndChild();
        ImGui::SameLine();
        ImGui::BeginChild("deck_canvas", {0, avail.y}, ImGuiChildFlags_None, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
        canvasArea();
        ImGui::EndChild();
        return;
    }
    if (!gameActive()) {
        canvasArea();
        return;
    }
    // a level or sprite: the canvas, with its layers, tiles, frames and colours beside it
    const ImVec2 avail = ImGui::GetContentRegionAvail();
    // a scene's tree and inspector want more room than layers and tiles
    const float side = std::min(ImGui::GetFontSize() * (canvas_.game.kind == "scene" ? 21.0f : 16.0f), avail.x * 0.4f);
    const ImGuiWindowFlags still = ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse;
    ImGui::BeginChild("game_canvas", {std::max(32.0f, avail.x - side - ImGui::GetStyle().ItemSpacing.x), avail.y}, ImGuiChildFlags_None, still);
    canvasArea();
    ImGui::EndChild();
    ImGui::SameLine();
    ImGui::BeginChild("game_side", {0, avail.y}, ImGuiChildFlags_Borders);
    gameSide();
    ImGui::EndChild();
}

void Shell::canvasArea() {
    if (canvas_.pending && canvas_.pending->shown) runPendingRetouch();  // the busy sign is on screen: run it now
    if (!videoPlaybackFrame()) refreshCanvas();  // a playing timeline draws its own frames
    ImGuiIO& io = ImGui::GetIO();
    const ImVec2 avail = ImGui::GetContentRegionAvail();
    const ImVec2 size{std::max(avail.x, 32.0f), std::max(avail.y, 32.0f)};
    ImGui::InvisibleButton("canvas", size, ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight | ImGuiButtonFlags_MouseButtonMiddle);
    const bool hovered = ImGui::IsItemHovered(), active = ImGui::IsItemActive();
    canvas_.viewMin = ImGui::GetItemRectMin();
    canvas_.viewSize = size;
    if (auto d = session_.workspace().get(currentDoc_); d && d->kind() == "audio" &&
        (std::abs(size.x - canvas_.waveSize.x) > 24 || std::abs(size.y - canvas_.waveSize.y) > 24)) {
        canvas_.dirty = true;  // the waveform is redrawn at the new size next frame
    }
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 v0 = canvas_.viewMin, v1{v0.x + size.x, v0.y + size.y};
    dl->PushClipRect(v0, v1, true);
    dl->AddRectFilled(v0, v1, darkLook() ? IM_COL32(40, 42, 46, 255) : IM_COL32(200, 202, 206, 255));

    if (canvas_.is3d) {
        if (sketchActive()) sketchCanvas(dl, v0, size, hovered, active);
        else orbitCanvas(dl, v0, size, hovered, active);
        dl->PopClipRect();
        return;
    }
    const Texture& t = canvas_.texture;
    if (!t.tex) {
        const std::string text = canvas_.summary.empty() ? "No picture for this file." : canvas_.summary;
        const ImVec2 ts = ImGui::CalcTextSize(text.c_str(), nullptr, false, size.x * 0.8f);
        dl->AddText(nullptr, 0, {v0.x + (size.x - ts.x) / 2, v0.y + (size.y - ts.y) / 2}, ImGui::GetColorU32(ImGuiCol_Text),
                    text.c_str(), nullptr, size.x * 0.8f);
        dl->PopClipRect();
        return;
    }
    const float w = static_cast<float>(t.srcWidth), h = static_cast<float>(t.srcHeight);
    auto shownDoc = session_.workspace().get(currentDoc_);
    // documents read like Word: pages fill the width from the top, the wheel scrolls (Ctrl+wheel zooms)
    const bool pages = shownDoc && shownDoc->kind() == "doc" && h > w * 1.6f;
    if (canvas_.fit) {
        // pixel art is tiny: let fit zoom further in on levels and sprites
        canvas_.zoom = std::min({size.x * 0.95f / w, size.y * 0.95f / h, gameActive() ? 48.0f : 8.0f});
        canvas_.pan = {(size.x - w * canvas_.zoom) / 2, (size.y - h * canvas_.zoom) / 2};
        if (pages) {
            canvas_.zoom = std::min(size.x * 0.95f / w, 1.0f);
            canvas_.pan = {(size.x - w * canvas_.zoom) / 2, 12};
        }
        canvas_.fit = false;
    }
    if (pages && hovered && io.MouseWheel != 0 && !io.KeyCtrl) {
        canvas_.pan.y = std::clamp(canvas_.pan.y + io.MouseWheel * ImGui::GetFontSize() * 5, size.y * 0.5f - h * canvas_.zoom, 12.0f);
    } else if (hovered && io.MouseWheel != 0 && !canvas_.stroking) {
        const ImVec2 img = toImage(io.MousePos);
        canvas_.zoom = std::clamp(canvas_.zoom * std::pow(1.15f, io.MouseWheel), 0.02f, 64.0f);
        canvas_.pan = {io.MousePos.x - v0.x - img.x * canvas_.zoom, io.MousePos.y - v0.y - img.y * canvas_.zoom};
    }
    const bool game = gameActive();
    const bool painting = game ? gamePainting() && !ImGui::IsKeyDown(ImGuiKey_Space)
                               : toolAvailable(canvas_.tool) && canvas_.tool != CanvasTool::Move && !ImGui::IsKeyDown(ImGuiKey_Space) &&
                                     !canvas_.pending;
    const bool waveform = shownDoc && shownDoc->kind() == "audio";  // left-drag picks a time range instead
    const bool slides = shownDoc && shownDoc->kind() == "deck" && deckDoc();  // left-drag moves objects, a click edits
    const bool layoutPage = shownDoc && shownDoc->kind() == "layout" && layoutDoc();  // left-drag moves frames
    const bool dragPan = active && (ImGui::IsMouseDown(ImGuiMouseButton_Middle) || ImGui::IsMouseDown(ImGuiMouseButton_Right) ||
                                    (ImGui::IsMouseDown(ImGuiMouseButton_Left) && !painting && !canvas_.stroking &&
                                     ((!circuitEditing() && !mapsEditing() && !waveform && !slides && !layoutPage) || ImGui::IsKeyDown(ImGuiKey_Space))));
    if (dragPan) {
        canvas_.pan.x += io.MouseDelta.x;
        canvas_.pan.y += io.MouseDelta.y;
    }
    if (auto it = mipCaches().find(this); it != mipCaches().end() && !it->second.doc.expired()) {
        showLevel(renderer_, canvas_.texture, it->second, canvas_.zoom, false);  // zoom changed: maybe another level
    }
    const ImVec2 p0{v0.x + canvas_.pan.x, v0.y + canvas_.pan.y}, p1{p0.x + w * canvas_.zoom, p0.y + h * canvas_.zoom};
    if (game && canvas_.game.kind == "sprite") {
        // a transparency checkerboard, like Aseprite: squares of a few sprite pixels, only where it shows
        const float cell = std::max(8.0f, canvas_.zoom * (canvas_.zoom >= 8 ? 1.0f : 4.0f));
        const ImVec2 c0{std::max(p0.x, v0.x), std::max(p0.y, v0.y)}, c1{std::min(p1.x, v0.x + size.x), std::min(p1.y, v0.y + size.y)};
        dl->AddRectFilled(c0, c1, IM_COL32(204, 204, 204, 255));
        if (c1.x > c0.x && c1.y > c0.y) {
            const int i0 = static_cast<int>(std::floor((c0.x - p0.x) / cell)), j0 = static_cast<int>(std::floor((c0.y - p0.y) / cell));
            const int i1 = static_cast<int>(std::ceil((c1.x - p0.x) / cell)), j1 = static_cast<int>(std::ceil((c1.y - p0.y) / cell));
            if (static_cast<long long>(i1 - i0) * (j1 - j0) < 40000) {
                for (int j = j0; j < j1; ++j)
                    for (int i = i0 + ((i0 + j) & 1); i < i1; i += 2) {
                        const ImVec2 a{std::max(c0.x, p0.x + i * cell), std::max(c0.y, p0.y + j * cell)};
                        const ImVec2 b{std::min(c1.x, p0.x + (i + 1) * cell), std::min(c1.y, p0.y + (j + 1) * cell)};
                        if (b.x > a.x && b.y > a.y) dl->AddRectFilled(a, b, IM_COL32(255, 255, 255, 255));
                    }
            }
        }
    } else {
        dl->AddRectFilled(p0, p1, IM_COL32(255, 255, 255, 255));
    }
    dl->AddImage(t.id(), p0, p1);
    dl->AddRect({p0.x - 1, p0.y - 1}, {p1.x + 1, p1.y + 1}, ImGui::GetColorU32(ImGuiCol_Border));
    if (glyphTool(canvas_.tool) && toolAvailable(canvas_.tool)) {  // the glyph editor covers the overview
        glyphCanvas(dl, v0, size, hovered, active);
        dl->PopClipRect();
        return;
    }
    if (game) {
        gameInput(dl, p0, hovered && painting, active);
        dl->PopClipRect();
        return;
    }

    // painting
    const bool retouch = canvas_.tool == CanvasTool::Heal || canvas_.tool == CanvasTool::Remove;
    const bool retouchBrush = brushTool(canvas_.tool);
    const bool stroke = canvas_.tool == CanvasTool::Brush || canvas_.tool == CanvasTool::Eraser || retouch || retouchBrush;
    if (painting && stroke && ImGui::IsItemActivated() && ImGui::IsMouseDown(ImGuiMouseButton_Left) &&
        !(retouch && retouchPress(io.MousePos)) && !(retouchBrush && brushToolPress(io.MousePos))) {
        const bool pen = io.MouseSource == ImGuiMouseSource_Pen || penDown_;
        beginStroke(io.MousePos, pen ? penPressure_ : 1.0f, pen, pen && penEraser_);
    }
    if (canvas_.stroking) addStrokePoint(io.MousePos, canvas_.usedPen ? penPressure_ : 1.0f);
    if (canvas_.stroking && !active) endStroke();
    if (painting && canvas_.tool == CanvasTool::Select) {
        if (ImGui::IsItemActivated() && ImGui::IsMouseDown(ImGuiMouseButton_Left)) selectPress(io.MousePos);
        if (canvas_.selecting && active) selectDrag(io.MousePos);
        if (canvas_.selecting && !active) selectRelease(io.MousePos);
    }
    if (canvas_.tool == CanvasTool::Transform && toolAvailable(CanvasTool::Transform) && !ImGui::IsKeyDown(ImGuiKey_Space)) {
        transformInput(dl, p0, hovered, active);
    } else if (xform_.active) {
        transformFinish(false);  // another tool: the handles go
    }
    if (canvas_.tool == CanvasTool::Type && toolAvailable(CanvasTool::Type)) imageTextInput(p0, hovered && painting);
    else if (imgText_.open) imageTextFinish(false);
    // (while typing on a shape, Space is a space, not the pan key)
    if (boardTool(canvas_.tool) && toolAvailable(canvas_.tool) && (!ImGui::IsKeyDown(ImGuiKey_Space) || board_.editing)) boardCanvas(dl, p0, hovered, active);
    const bool vectorTool = canvas_.tool == CanvasTool::Pick || canvas_.tool == CanvasTool::Anchor || vectorDrawTool(canvas_.tool);
    if (painting && vectorTool) {
        if (ImGui::IsItemActivated() && ImGui::IsMouseDown(ImGuiMouseButton_Left)) vectorPress(io.MousePos);
        if (canvas_.vecDrag) vectorDrag(io.MousePos);
        if (canvas_.vecDrag && !active) vectorRelease();
        if (hovered) vectorKeys();
        vectorPenKeys();
    }
    drawVectorDraft(dl, p0);
    if (vectorTool) vectorTextBox(p0);
    if (painting && canvas_.tool == CanvasTool::Fill && ImGui::IsItemClicked(ImGuiMouseButton_Left)) {
        const ImVec2 img = toImage(io.MousePos);
        if (img.x >= 0 && img.y >= 0 && img.x < w && img.y < h && find("paint.fill")) {
            Result r = run("paint.fill", {{"doc", currentDoc_},
                                          {"x", static_cast<std::int64_t>(img.x)},
                                          {"y", static_cast<std::int64_t>(img.y)},
                                          {"color", toHex({canvas_.color[0], canvas_.color[1], canvas_.color[2], canvas_.color[3]})}});
            resultFor_ = "Bucket fill";
            r.message = r.ok ? sentence(r.message) : friendlyError(r.message);
            result_ = std::move(r);
        }
    }

    // the stroke being drawn, before the command runs
    if (canvas_.stroking && canvas_.points.size() >= 2 && !retouch) {
        const bool erasing = canvas_.tool == CanvasTool::Eraser || canvas_.penErasing;
        const ImU32 col = erasing || retouchBrush ? IM_COL32(255, 255, 255, 110)
                                  : ImGui::ColorConvertFloat4ToU32({canvas_.color[0], canvas_.color[1], canvas_.color[2],
                                                                    canvas_.color[3] * canvas_.opacity / 100.0f});
        std::vector<ImVec2> pts;
        for (std::size_t i = 0; i + 1 < canvas_.points.size(); i += 2) {
            pts.push_back({p0.x + static_cast<float>(canvas_.points[i]) * canvas_.zoom,
                           p0.y + static_cast<float>(canvas_.points[i + 1]) * canvas_.zoom});
        }
        const float thick = std::max(1.0f, canvas_.size * canvas_.zoom);
        if (pts.size() == 1) dl->AddCircleFilled(pts[0], thick / 2, col);
        else dl->AddPolyline(pts.data(), static_cast<int>(pts.size()), col, ImDrawFlags_None, thick);
    }
    if (hovered && painting && stroke) {
        dl->AddCircle(io.MousePos, std::max(2.0f, canvas_.size * canvas_.zoom / 2), IM_COL32(128, 128, 128, 255), 0, 1.5f);
    }
    drawRetouch(dl, p0, hovered);
    drawBrushTool(dl, p0, hovered);

    if (waveform) audioCanvas(dl, p0, p1, hovered, active);
    if (shownDoc && shownDoc->kind() == "doc" && !painting) docCanvas(dl, p0, hovered);
    if (slides) deckCanvas(dl, p0, hovered, active);
    if (layoutPage) layoutCanvas(dl, p0, hovered, active);
    // the playhead over a waveform; a click (not a drag) moves it there
    if (playheadFraction_ >= 0) {
        const float x = p0.x + (p1.x - p0.x) * std::clamp(playheadFraction_, 0.0f, 1.0f);
        dl->AddLine({x, p0.y}, {x, p1.y}, ImGui::ColorConvertFloat4ToU32(accent_), 2.0f);
        if (hovered && ImGui::IsMouseReleased(ImGuiMouseButton_Left) && ImGui::GetMouseDragDelta(ImGuiMouseButton_Left).x == 0 &&
            ImGui::GetMouseDragDelta(ImGuiMouseButton_Left).y == 0 && io.MousePos.x >= p0.x && io.MousePos.x <= p1.x && transport_.length > 0) {
            seekTo(transport_.length * (io.MousePos.x - p0.x) / (p1.x - p0.x));
        }
    }
    drawSelection(dl, p0, hovered);
    if (vectorTool && toolAvailable(canvas_.tool)) drawVector(dl, p0);
    if (circuitDoc()) circuitCanvas(dl, p0, hovered, ImGui::IsItemClicked(ImGuiMouseButton_Left));
    if (mapsDoc()) mapsCanvas(dl, p0, hovered, ImGui::IsItemClicked(ImGuiMouseButton_Left));
    dl->PopClipRect();
}

// Audacity's way: drag over the waveform to pick a time range (shaded), then any action
// with start/end gets it; Delete cuts it, a click clears it and moves the playhead.
void Shell::audioCanvas(ImDrawList* dl, ImVec2 p0, ImVec2 p1, bool hovered, bool active) {
    AudioSelection& sel = audioSel_;
    if (sel.doc != currentDoc_) sel = {currentDoc_};
    if (sel.seen != changes_ || sel.lengthDoc != currentDoc_) {
        sel.seen = changes_;
        sel.lengthDoc = currentDoc_;
        Result info = query("audio.info", {{"doc", currentDoc_}});
        sel.length = 0;
        if (const Value* v = info.ok ? info.field("seconds") : nullptr) {
            if (auto d = std::get_if<double>(v)) sel.length = *d;
            if (auto i = std::get_if<std::int64_t>(v)) sel.length = static_cast<double>(*i);
        }
        if (sel.b > sel.length) sel.b = sel.length;
        if (sel.a >= sel.b) sel.a = sel.b = -1;
    }
    if (sel.length <= 0 || p1.x - p0.x < 4) return;
    ImGuiIO& io = ImGui::GetIO();
    auto timeAt = [&](float x) { return std::clamp((x - p0.x) / (p1.x - p0.x), 0.0f, 1.0f) * sel.length; };
    // fade handles in the top corners (of the selection, else the whole clip) and a gain knob
    // in the middle, like Audacity's clip handles: drag, and the release applies it
    const double ra = sel.any() ? sel.a : 0.0, rb = sel.any() ? sel.b : sel.length;
    auto xAt = [&](double t) { return p0.x + static_cast<float>(t / sel.length) * (p1.x - p0.x); };
    const float hs = ImGui::GetFontSize() * 0.45f;
    const float top = std::max(p0.y, canvas_.viewMin.y) + 2;
    const ImVec2 fiPos{xAt(ra) + (sel.handle == 1 ? static_cast<float>(sel.handleValue / sel.length) * (p1.x - p0.x) : 0) + hs, top + hs};
    const ImVec2 foPos{xAt(rb) - (sel.handle == 2 ? static_cast<float>(sel.handleValue / sel.length) * (p1.x - p0.x) : 0) - hs, top + hs};
    const ImVec2 gPos{(xAt(ra) + xAt(rb)) / 2, top + hs};
    auto overHandle = [&](ImVec2 c) { return std::fabs(io.MousePos.x - c.x) <= hs * 1.6f && std::fabs(io.MousePos.y - c.y) <= hs * 1.6f; };
    if (ImGui::IsItemActivated() && ImGui::IsMouseDown(ImGuiMouseButton_Left) && !ImGui::IsKeyDown(ImGuiKey_Space)) {
        sel.handle = overHandle(fiPos) ? 1 : overHandle(foPos) ? 2 : overHandle(gPos) ? 3 : 0;
        sel.handleValue = 0;
        sel.grabY = io.MousePos.y;
        if (!sel.handle) {
            sel.dragging = true;
            sel.a = sel.b = timeAt(io.MousePos.x);
        }
    }
    if (sel.handle) {
        const double span = rb - ra;
        if (sel.handle == 1) sel.handleValue = std::clamp(timeAt(io.MousePos.x) - ra, 0.0, span);
        else if (sel.handle == 2) sel.handleValue = std::clamp(rb - timeAt(io.MousePos.x), 0.0, span);
        else sel.handleValue = std::clamp(std::round(static_cast<double>(sel.grabY - io.MousePos.y) / 4.0 * 2) / 2, -24.0, 24.0);  // 0.5 dB per 2 px
        const ImU32 ghost = ImGui::ColorConvertFloat4ToU32({accent_.x, accent_.y, accent_.z, 0.35f});
        if (sel.handle == 1 && sel.handleValue > 0) dl->AddTriangleFilled({xAt(ra), p0.y}, {xAt(ra + sel.handleValue), p0.y}, {xAt(ra), p1.y}, ghost);
        if (sel.handle == 2 && sel.handleValue > 0) dl->AddTriangleFilled({xAt(rb - sel.handleValue), p0.y}, {xAt(rb), p0.y}, {xAt(rb), p1.y}, ghost);
        if (sel.handle == 3) {
            const float mid = (p0.y + p1.y) / 2, half = (p1.y - p0.y) / 2 * static_cast<float>(std::min(1.0, std::pow(10.0, sel.handleValue / 20) * 0.5));
            dl->AddRect({xAt(ra), mid - half}, {xAt(rb), mid + half}, ImGui::ColorConvertFloat4ToU32(accent_), 0, 0, 1.5f);
        }
        ImGui::SetTooltip(sel.handle == 3 ? "gain %+.1f dB" : sel.handle == 1 ? "fade in %.2f s" : "fade out %.2f s", sel.handleValue);
        if (!active) {
            const int h = sel.handle;
            sel.handle = 0;
            Params p{{"doc", currentDoc_}};
            if (sel.any()) p["start"] = sel.a, p["end"] = sel.b;
            if (h == 3 && std::fabs(sel.handleValue) > 0.01) {
                p["db"] = sel.handleValue;
                Result r = run("audio.gain", p);
                resultFor_ = "Gain";
                r.message = r.ok ? sentence(r.message) : friendlyError(r.message);
                result_ = std::move(r);
            } else if (h != 3 && sel.handleValue > 0.005) {
                p[h == 1 ? "in" : "out"] = sel.handleValue;
                p["curve"] = std::string("s_curve");
                Result r = run("audio.fade", p);
                resultFor_ = "Fade";
                r.message = r.ok ? sentence(r.message) : friendlyError(r.message);
                result_ = std::move(r);
            }
        }
    }
    {
        const ImU32 knob = IM_COL32(255, 255, 255, 220), edge = IM_COL32(0, 0, 0, 160);
        for (const ImVec2& c : {fiPos, foPos}) {
            dl->AddRectFilled({c.x - hs, c.y - hs}, {c.x + hs, c.y + hs}, knob, 2);
            dl->AddRect({c.x - hs, c.y - hs}, {c.x + hs, c.y + hs}, edge, 2);
        }
        dl->AddCircleFilled(gPos, hs, IM_COL32(255, 230, 120, 230));
        dl->AddCircle(gPos, hs, edge);
        if (hovered && !sel.handle && !sel.dragging) {
            if (overHandle(fiPos)) ImGui::SetTooltip("Drag right to fade in%s", sel.any() ? " the selection" : "");
            else if (overHandle(foPos)) ImGui::SetTooltip("Drag left to fade out%s", sel.any() ? " the selection" : "");
            else if (overHandle(gPos)) ImGui::SetTooltip("Drag up or down to change the volume%s", sel.any() ? " of the selection" : "");
        }
    }
    static double anchor = 0;
    if (sel.dragging) {
        if (ImGui::IsItemActivated()) anchor = sel.a;
        const double t = timeAt(io.MousePos.x);
        sel.a = std::min(anchor, t);
        sel.b = std::max(anchor, t);
        if (!active) {
            sel.dragging = false;
            const double minSpan = sel.length * 3.0 / (p1.x - p0.x);  // under 3 px is a click
            if (sel.b - sel.a < minSpan) {
                seekTo(anchor);
                sel.a = sel.b = -1;
            } else {
                fillAudioRange(true);
                char text[96];
                std::snprintf(text, sizeof text, "Selected %.2f s to %.2f s (%.2f s). Pick an action, or Delete to cut it.", sel.a, sel.b,
                              sel.b - sel.a);
                result_ = Result::success(text);
                resultFor_ = "Selection";
            }
        }
    }
    if (sel.any() && hovered && !io.WantTextInput && (ImGui::IsKeyPressed(ImGuiKey_Delete) || ImGui::IsKeyPressed(ImGuiKey_Backspace))) {
        Result r = run("audio.cut", {{"doc", currentDoc_}, {"start", sel.a}, {"end", sel.b}});
        resultFor_ = "Cut";
        r.message = r.ok ? sentence(r.message) : friendlyError(r.message);
        result_ = std::move(r);
        sel.a = sel.b = -1;
    }
    if (sel.any() || sel.dragging) {
        const float x0 = p0.x + static_cast<float>(sel.a / sel.length) * (p1.x - p0.x);
        const float x1 = p0.x + static_cast<float>(sel.b / sel.length) * (p1.x - p0.x);
        ImVec4 c = accent_;
        dl->AddRectFilled({x0, p0.y}, {x1, p1.y}, ImGui::ColorConvertFloat4ToU32({c.x, c.y, c.z, 0.28f}));
        dl->AddLine({x0, p0.y}, {x0, p1.y}, ImGui::ColorConvertFloat4ToU32(c), 1.5f);
        dl->AddLine({x1, p0.y}, {x1, p1.y}, ImGui::ColorConvertFloat4ToU32(c), 1.5f);
    }
}

// Word's way, roughly: click a paragraph on the page and type. The block is framed and its
// text (inline Markdown for styles) opens in a box beside it; Save puts it back as one undo step.
namespace {

// Ctrl+B / I / U wrap the selection (or put empty markers at the caret); Enter asks to split.
int docTextCallback(ImGuiInputTextCallbackData* data) {
    auto* e = static_cast<DocEditAccess*>(data->UserData);
    if (*e->caret >= 0) {
        data->CursorPos = data->SelectionStart = data->SelectionEnd = std::clamp(*e->caret, 0, data->BufTextLen);
        *e->caret = -1;
    }
    if (*e->pending) {
        const char* open = *e->pending == 'b' ? "**" : *e->pending == 'i' ? "*" : "<u>";
        const char* close = *e->pending == 'b' ? "**" : *e->pending == 'i' ? "*" : "</u>";
        int a = std::min(data->SelectionStart, data->SelectionEnd), b = std::max(data->SelectionStart, data->SelectionEnd);
        if (a == b) a = b = data->CursorPos;
        data->InsertChars(b, close);
        data->InsertChars(a, open);
        data->SelectionStart = a + static_cast<int>(std::strlen(open));
        data->SelectionEnd = b + static_cast<int>(std::strlen(open));
        data->CursorPos = data->SelectionEnd;
        *e->pending = 0;
    }
    *e->cursor = data->CursorPos;
    return 0;
}

// The character a click lands on, laid out the way the box wraps it.
int charAt(const std::string& text, float wrap, ImVec2 rel) {
    ImFont* font = ImGui::GetFont();
    const float size = ImGui::GetFontSize(), lineH = ImGui::GetTextLineHeight();
    const char* s = text.c_str();
    const char* end = s + text.size();
    float y = 0;
    while (s < end) {
        const char* nl = std::find(s, end, '\n');
        const char* lineEnd = font->CalcWordWrapPosition(size, s, nl, wrap);
        if (lineEnd == s) lineEnd = std::min(nl, s + 1);
        if (rel.y < y + lineH || (lineEnd >= nl && nl == end)) {
            const char* best = s;
            float bestD = 1e9f;
            for (const char* c = s; c <= lineEnd; ++c) {
                const float x = font->CalcTextSizeA(size, FLT_MAX, 0, s, c).x;
                if (std::abs(x - rel.x) < bestD) bestD = std::abs(x - rel.x), best = c;
            }
            return static_cast<int>(best - text.c_str());
        }
        y += lineH;
        s = lineEnd;
        if (s < end && *s == '\n') ++s;
        else while (s < end && *s == ' ') ++s;
    }
    return static_cast<int>(text.size());
}

}  // namespace

// Notes-style [[links]]: the note is looked for next to the open file and in the folders under
// it (and the folder above, for a vault); a new name makes the note.
void Shell::followWikiLink(const std::string& name) {
    auto doc = session_.workspace().get(currentDoc_);
    if (!doc || doc->path.empty()) {
        result_ = Result::failure("Save this note first, so [[links]] know which folder they point into.");
        resultFor_ = "Open link";
        return;
    }
    namespace fs = std::filesystem;
    std::error_code ec;
    const fs::path here = fs::absolute(fs::path(doc->path), ec).parent_path();
    std::string want = name;
    for (auto& ch : want) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    if (want.size() < 3 || want.compare(want.size() - 3, 3, ".md") != 0) want += ".md";
    auto lower = [](std::string v) {
        for (auto& ch : v) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
        return v;
    };
    fs::path found;
    for (const fs::path& root : {here, here.parent_path()}) {
        if (fs::exists(root / fs::path(want), ec)) found = root / fs::path(want);
        int seen = 0;
        for (auto it = fs::recursive_directory_iterator(root, fs::directory_options::skip_permission_denied, ec);
             found.empty() && !ec && it != fs::recursive_directory_iterator() && ++seen < 5000; it.increment(ec)) {
            if (it.depth() > 4) it.disable_recursion_pending();
            const fs::path rel = fs::relative(it->path(), root, ec);
            if (it->is_regular_file(ec) && (lower(it->path().filename().string()) == fs::path(want).filename().string() || lower(rel.generic_string()) == want)) {
                found = it->path();
            }
        }
        if (!found.empty()) break;
    }
    if (found.empty()) {
        const bool hasExt = lower(name).size() > 3 && lower(name).compare(name.size() - 3, 3, ".md") == 0;
        found = here / fs::path(hasExt ? name : name + ".md");
        std::ofstream out(found, std::ios::binary);
        out << "# " << name << "\n";
    }
    openFile(found.string());
}

void Shell::docCommit(bool close) {
    DocBlockEdit& e = docEdit_;
    if (e.block <= 0) return;
    if (e.text != e.original) {
        Result r = run("office.doc_edit", {{"doc", currentDoc_}, {"block", static_cast<std::int64_t>(e.block)}, {"text", e.text}});
        resultFor_ = "Edit";
        r.message = r.ok ? sentence(r.message) : friendlyError(r.message);
        result_ = std::move(r);
        canvas_.dirty = true;
        e.original = e.text;
    }
    if (close) e.block = 0;
}

// Click on the page to type right there, like Word: the block turns into a text box in place with
// the caret where you clicked. Ctrl+B/I/U format the selection, Enter starts a new paragraph,
// Backspace on an empty one removes it, Esc or a click elsewhere keeps the words.
void Shell::docCanvas(ImDrawList* dl, ImVec2 p0, bool hovered) {
    DocBlockEdit& e = docEdit_;
    if (e.doc != currentDoc_) e = {currentDoc_};
    ImGuiIO& io = ImGui::GetIO();
    auto openAt = [&](float sheetY, ImVec2 click) {
        Params hp{{"doc", currentDoc_}, {"sheet_y", static_cast<double>(sheetY)}};
        if (click.x >= 0) hp["sheet_x"] = static_cast<double>((click.x - p0.x) / canvas_.zoom);
        Result r = query("office.doc_hit", hp);
        auto num = [&r](const char* key, double fallback = 0.0) {
            const Value* v = r.field(key);
            if (auto d = v ? std::get_if<double>(v) : nullptr) return *d;
            if (auto i = v ? std::get_if<std::int64_t>(v) : nullptr) return static_cast<double>(*i);
            return fallback;
        };
        auto text = [&r](const char* key) {
            const Value* v = r.field(key);
            auto s = v ? std::get_if<std::string>(v) : nullptr;
            return s ? *s : std::string();
        };
        if (r.ok && r.field("wiki") && io.KeyCtrl) {  // Ctrl+click a [[note]] link: open that note (made if it's new)
            followWikiLink(text("wiki"));
            return false;
        }
        if (r.ok && r.field("task")) {  // a task box: tick or untick it, no typing
            std::string t = text("text");
            const int item = static_cast<int>(num("task"));
            std::size_t at = 0;
            for (int k = 1; k < item && at != std::string::npos; ++k) at = t.find('\n', at), at = at == std::string::npos ? at : at + 1;
            if (at != std::string::npos) at = t.find("\xE2\x98", at);
            if (at != std::string::npos && at + 2 < t.size()) {
                t[at + 2] = t[at + 2] == '\x90' ? '\x91' : '\x90';
                Result done = run("office.doc_edit", {{"doc", currentDoc_}, {"block", static_cast<std::int64_t>(num("block"))}, {"text", t}});
                resultFor_ = "Tick";
                done.message = done.ok ? sentence(done.message) : friendlyError(done.message);
                result_ = std::move(done);
                canvas_.dirty = true;
            }
            return false;
        }
        if (r.ok && text("kind") != "image" && text("kind") != "pagebreak" && text("kind") != "toc") {
            e.block = static_cast<int>(num("block"));
            e.kind = text("kind");
            e.text = text("text");
            while (!e.text.empty() && e.text.back() == '\n') e.text.pop_back();
            e.original = e.text;
            e.top = static_cast<float>(num("sheet_top"));
            e.bottom = static_cast<float>(num("sheet_bottom"));
            e.left = static_cast<float>(num("sheet_left", 0));
            e.right = static_cast<float>(num("sheet_right", canvas_.texture.srcWidth));
            e.fontPx = static_cast<float>(num("font_px", 14));
            e.focus = true;
            e.caret = -1;
            ++e.generation;  // a fresh text box: the old one's buffer mustn't write back over this block
            e.cursor = static_cast<int>(e.text.size());
            if (click.x >= 0 && r.field("char")) {  // the caret goes on the letter that was clicked
                e.caret = std::clamp(static_cast<int>(num("char")), 0, static_cast<int>(e.text.size()));
            } else if (click.x >= 0) {  // older office: guess from the box's own wrapping
                const float px = std::max(6.0f, e.fontPx * canvas_.zoom);
                pushFontPx(px);
                const float wrap = (e.right - e.left) * canvas_.zoom - ImGui::GetStyle().FramePadding.x * 2;
                const ImVec2 rel{click.x - (p0.x + e.left * canvas_.zoom) - ImGui::GetStyle().FramePadding.x,
                                 click.y - (p0.y + e.top * canvas_.zoom) - ImGui::GetStyle().FramePadding.y};
                e.caret = charAt(e.text, wrap, rel);
                ImGui::PopFont();
            }
            return true;
        }
        if (r.ok) {
            result_ = Result::success("That block can't be typed into here; use the " + text("kind") + " actions on the right.");
            resultFor_ = "Edit";
        }
        return false;
    };
    // a click on the page outside the open box: keep its words, then open the block under the click
    if (hovered && ImGui::IsMouseReleased(ImGuiMouseButton_Left) && io.MouseDragMaxDistanceSqr[ImGuiMouseButton_Left] < 16 &&
        !ImGui::IsKeyDown(ImGuiKey_Space) && find("office.doc_hit")) {
        docCommit(true);
        const ImVec2 img = toImage(io.MousePos);
        openAt(img.y, io.MousePos);
    }
    if (e.block <= 0) return;

    const float zoom = canvas_.zoom;
    const float px = std::max(6.0f, e.fontPx * zoom);
    pushFontPx(px);
    const ImVec2 pad = ImGui::GetStyle().FramePadding;
    const float boxW = std::max(40.0f, (e.right - e.left) * zoom);
    const float textH = ImGui::CalcTextSize(e.text.empty() ? " " : e.text.c_str(), nullptr, false, boxW - pad.x * 2).y;
    const float boxH = std::max((e.bottom - e.top) * zoom, textH + ImGui::GetTextLineHeight() + pad.y * 2);
    const ImVec2 at{p0.x + e.left * zoom - pad.x, p0.y + e.top * zoom - pad.y};
    // the box looks like the page: white, dark words, a thin accent edge
    const ImVec4 c = accent_;
    dl->AddRectFilled({at.x - 2, at.y - 2}, {at.x + boxW + pad.x * 2 + 2, at.y + boxH + 2}, IM_COL32(255, 255, 255, 255));
    ImGui::SetNextWindowPos(at);
    ImGui::SetNextWindowSize({boxW + pad.x * 2, boxH});
    ImGui::PushStyleColor(ImGuiCol_WindowBg, IM_COL32(255, 255, 255, 255));
    ImGui::PushStyleColor(ImGuiCol_FrameBg, IM_COL32(255, 255, 255, 255));
    ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(20, 20, 24, 255));
    ImGui::PushStyleColor(ImGuiCol_Border, ImGui::ColorConvertFloat4ToU32({c.x, c.y, c.z, 0.8f}));
    ImGui::PushStyleColor(ImGuiCol_TextSelectedBg, ImGui::ColorConvertFloat4ToU32({c.x, c.y, c.z, 0.35f}));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {0, 0});
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 1);
    const ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoScrollWithMouse;
    bool commit = false, splitNow = false, removeNow = false;
    if (ImGui::Begin("##docinplace", nullptr, flags)) {
        if (e.focus) {
            ImGui::SetKeyboardFocusHere();
            e.focus = false;
        }
        const bool typing = ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);
        if (typing && io.KeyCtrl && !io.KeyShift) {
            if (ImGui::IsKeyPressed(ImGuiKey_B, false)) e.pending = 'b';
            if (ImGui::IsKeyPressed(ImGuiKey_I, false)) e.pending = 'i';
            if (ImGui::IsKeyPressed(ImGuiKey_U, false)) e.pending = 'u';
        }
        // paragraphs, headings and quotes: Enter starts the next paragraph; lists and tables keep Enter for new lines
        const bool lines = e.kind == "list" || e.kind == "numbered" || e.kind == "table" || e.kind == "code";
        const bool enter = typing && !lines && !io.KeyShift && (ImGui::IsKeyPressed(ImGuiKey_Enter, false) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter, false));
        const bool emptyBack = typing && e.text.empty() && ImGui::IsKeyPressed(ImGuiKey_Backspace, false);
        DocEditAccess access{&e.caret, &e.pending, &e.cursor};
        // Esc keeps the words: the box would put the old text back, so hold on to what's typed
        const bool escape = typing && ImGui::IsKeyPressed(ImGuiKey_Escape, false);
        const std::string typed = e.text;
        ImGuiInputTextFlags tf = ImGuiInputTextFlags_WordWrap | ImGuiInputTextFlags_CallbackAlways | ImGuiInputTextFlags_NoHorizontalScroll;
        if (enter) tf |= ImGuiInputTextFlags_ReadOnly;  // the Enter goes to the split, not into the text
        ImGui::PushID(e.generation);
        ImGui::InputTextMultiline("##inplace", &e.text, {-1, -1}, tf, docTextCallback, &access);
        ImGui::PopID();
        if (escape) e.text = typed;
        if (enter) splitNow = true, e.split = e.cursor;
        else if (emptyBack) removeNow = true;
        else if (escape || (typing && io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_Enter, false))) commit = true;
        else if (ImGui::IsItemDeactivated() && !ImGui::IsWindowFocused()) commit = true;
    }
    ImGui::End();
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor(5);
    ImGui::PopFont();

    if (removeNow) {
        Result r = run("office.doc_delete", {{"doc", currentDoc_}, {"block", static_cast<std::int64_t>(e.block)}});
        resultFor_ = "Delete";
        r.message = r.ok ? sentence(r.message) : friendlyError(r.message);
        result_ = std::move(r);
        canvas_.dirty = true;
        e.block = 0;
    } else if (splitNow) {
        // the words after the caret move to a new paragraph under this one, and typing carries on there
        const std::size_t at0 = static_cast<std::size_t>(std::clamp(e.split, 0, static_cast<int>(e.text.size())));
        std::string rest = e.text.substr(at0);
        e.text.erase(at0);
        while (!rest.empty() && rest.front() == ' ') rest.erase(0, 1);
        const int block = e.block;
        const float bottom = e.bottom;
        docCommit(true);
        Result r = run("office.doc_add", {{"doc", currentDoc_}, {"kind", std::string("paragraph")}, {"text", rest.empty() ? std::string(" ") : rest},
                                          {"at", static_cast<std::int64_t>(block + 1)}});
        resultFor_ = "New paragraph";
        canvas_.dirty = true;
        if (r.ok) {
            refreshCanvas();
            if (openAt(bottom + 2, {-1, -1})) {  // the new block sits right under the old one
                if (rest.empty()) e.text.clear(), e.original = " ";
                e.caret = 0;
            }
        } else {
            r.message = friendlyError(r.message);
            result_ = std::move(r);
        }
    } else if (commit) {
        docCommit(true);
    }
    (void)dl;
}

}  // namespace wetwaresuite::gui
