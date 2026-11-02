// Click tools for game documents on the canvas: tile painting, rectangles,
// erasing, bucket fill, objects and a tile picker for levels (levels.*), and a
// pixel pencil, eraser, lines, boxes, ellipses, bucket and colour picker with
// a frame strip, onion skin, playback, layers and palette for sprites
// (forge.sprite_*, forge.draw). Every edit is one command through
// Session::run, so it undoes in one step and scripts see the same thing.
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <iterator>
#include <sstream>
#include <string>
#include <vector>

#include "shell.hpp"
#include "tool_panels.hpp"

namespace wetwaresuite::gui {

namespace {

std::vector<std::string> splitComma(const std::string& text) {
    std::vector<std::string> out;
    if (text.empty()) return out;
    std::stringstream ss(text);
    std::string item;
    while (std::getline(ss, item, ',')) out.push_back(item);
    return out;
}

std::string hexOf(const float c[4]) {
    auto byte = [](float v) { return static_cast<int>(std::lround(std::clamp(v, 0.0f, 1.0f) * 255)); };
    char buf[16];
    std::snprintf(buf, sizeof buf, "#%02x%02x%02x%02x", byte(c[0]), byte(c[1]), byte(c[2]), byte(c[3]));
    return buf;
}

bool parseHex(const std::string& text, float out[4]) {
    std::string h = text;
    if (!h.empty() && h[0] == '#') h.erase(0, 1);
    if (h.size() != 6 && h.size() != 8) return false;
    unsigned v[4] = {0, 0, 0, 255};
    for (std::size_t i = 0; i * 2 < h.size(); ++i) v[i] = static_cast<unsigned>(std::stoul(h.substr(i * 2, 2), nullptr, 16));
    for (int i = 0; i < 4; ++i) out[i] = static_cast<float>(v[i]) / 255.0f;
    return true;
}

bool isLevelTool(GameTool t) {
    return t == GameTool::Paint || t == GameTool::Rect || t == GameTool::Erase || t == GameTool::Fill || t == GameTool::Object ||
           t == GameTool::Pick || t == GameTool::Move;
}

bool isSpriteTool(GameTool t) {
    return t == GameTool::Pencil || t == GameTool::Eraser || t == GameTool::Line || t == GameTool::Box || t == GameTool::Ellipse ||
           t == GameTool::Bucket || t == GameTool::Pick || t == GameTool::Move;
}

// Drag tools act on the rectangle between press and release; the others on each point.
bool dragsShape(GameTool t) {
    return t == GameTool::Rect || t == GameTool::Erase || t == GameTool::Object || t == GameTool::Line || t == GameTool::Box ||
           t == GameTool::Ellipse;
}

}  // namespace

bool Shell::gameActive() const {
    auto doc = session_.workspace().get(currentDoc_);
    if (!doc) return false;
    if (doc->kind() == "level") return find("levels.preview") && find("levels.paint");
    if (doc->kind() == "sprite") return find("forge.sprite_preview") && find("forge.draw");
    if (doc->kind() == "scene") return find("scene.preview") && find("scene.transform");
    return false;
}

bool Shell::gamePainting() const {
    const GameCanvas& g = canvas_.game;
    if (g.tool == GameTool::Move) return false;
    if (g.kind == "scene") return true;
    return g.kind == "level" ? isLevelTool(g.tool) : isSpriteTool(g.tool);
}

void Shell::gameSync() {
    GameCanvas& g = canvas_.game;
    auto doc = session_.workspace().get(currentDoc_);
    if (!doc) return;
    const bool newDoc = g.doc != currentDoc_;
    if (!newDoc && g.seen == changes_) return;
    if (newDoc) {
        g.tilesetTex.reset();
        g = GameCanvas{};
        g.doc = currentDoc_;
        g.kind = doc->kind();
    }
    g.seen = changes_;
    g.error.clear();
    if (g.kind == "scene") return sceneSync(newDoc);
    if (g.kind == "level") {
        Result r = query("levels.info", {{"doc", currentDoc_}});
        if (!r.ok) return;
        g.tileW = std::max(1, static_cast<int>(tp::number(r, "tile_width", 16)));
        g.tileH = std::max(1, static_cast<int>(tp::number(r, "tile_height", 16)));
        g.mapW = static_cast<int>(tp::number(r, "width"));
        g.mapH = static_cast<int>(tp::number(r, "height"));
        g.layers = splitComma(tp::text(r, "layers"));
        g.layerKinds = splitComma(tp::text(r, "layer_kinds"));
        g.layerKinds.resize(g.layers.size(), "tiles");
        g.tilesets = splitComma(tp::text(r, "tilesets"));
        if (newDoc) {  // start on the top tile layer
            g.layer = 0;
            for (std::size_t i = 0; i < g.layers.size(); ++i) {
                if (g.layerKinds[i] == "tiles") g.layer = static_cast<int>(i);
            }
            if (!g.tilesets.empty()) g.tool = GameTool::Paint;
        }
        g.layer = std::clamp(g.layer, 0, std::max(0, static_cast<int>(g.layers.size()) - 1));
        g.tileset = std::clamp(g.tileset, 0, std::max(0, static_cast<int>(g.tilesets.size()) - 1));
        g.tilesetFor.clear();  // tiles may have been redrawn
        return;
    }
    Result r = query("forge.sprite_info", {{"doc", currentDoc_}});
    if (!r.ok) return;
    g.width = static_cast<int>(tp::number(r, "width"));
    g.height = static_cast<int>(tp::number(r, "height"));
    g.frames = std::max(1, static_cast<int>(tp::number(r, "frames", 1)));
    g.frame = std::clamp(g.frame, 0, g.frames - 1);
    g.spriteLayers = splitComma(tp::text(r, "layer_names"));
    g.layerShown.clear();
    for (const auto& v : splitComma(tp::text(r, "layer_visible"))) g.layerShown.push_back(v == "1");
    g.layerShown.resize(g.spriteLayers.size(), true);
    if (newDoc) g.spriteLayer = static_cast<int>(g.spriteLayers.size()) - 1;
    g.spriteLayer = std::clamp(g.spriteLayer, 0, std::max(0, static_cast<int>(g.spriteLayers.size()) - 1));
    g.durations.clear();
    for (const auto& d : splitComma(tp::text(r, "durations"))) g.durations.push_back(std::max(10, std::atoi(d.c_str())));
    g.palette = splitComma(tp::text(r, "palette"));
    g.tags = splitComma(tp::text(r, "tags"));
    g.tagRanges = splitComma(tp::text(r, "tag_ranges"));
    if (newDoc) {
        g.tool = GameTool::Pencil;
        // start on a solid palette colour so the first stroke shows
        for (const auto& c : g.palette) {
            float rgba[4];
            if (parseHex(c, rgba) && rgba[3] > 0.5f) {
                std::copy(rgba, rgba + 4, g.color);
                break;
            }
        }
    }
}

std::string Shell::gamePicture() {
    if (!gameActive()) return {};
    gameSync();
    GameCanvas& g = canvas_.game;
    if (g.kind == "scene") return scenePicture();
    Result r = g.kind == "level"
                   ? query("levels.preview", {{"doc", currentDoc_}, {"scale", 1.0}, {"max_size", std::int64_t{8192}}})
                   : query("forge.sprite_preview", {{"doc", currentDoc_},
                                                    {"frame", static_cast<std::int64_t>(g.frame)},
                                                    {"scale", std::int64_t{1}},
                                                    {"onion_before", std::int64_t{g.onion && !g.playing ? 1 : 0}},
                                                    {"onion_after", std::int64_t{g.onion && !g.playing ? 1 : 0}}});
    if (!r.ok) return {};
    for (const auto& a : r.attachments) {
        if (a.mime == "image/png") return a.bytes;
    }
    return {};
}

void Shell::gameRun(const std::string& id, Params params, const std::string& title) {
    const CommandInfo* info = find(id);
    if (!info) return;
    params["doc"] = currentDoc_;
    Result r = run(id, params);
    resultFor_ = title;
    r.message = r.ok ? sentence(r.message) : friendlyError(r.message);
    canvas_.game.error = r.ok ? std::string() : r.message;
    result_ = std::move(r);
    canvas_.dirty = true;
}

void Shell::gameTools() {
    GameCanvas& g = canvas_.game;
    gameSync();
    if (g.kind == "scene") return sceneTools();
    struct Item {
        const char* name;
        GameTool tool;
        const char* key;
        const char* tip;
    };
    // Tiled and Aseprite letters where they have one
    static const Item level[] = {
        {"Move", GameTool::Move, "V", "Drag to move around, scroll to zoom"},
        {"Paint", GameTool::Paint, "B", "Click or drag to put the chosen tile in cells"},
        {"Rect", GameTool::Rect, "R", "Drag a rectangle of the chosen tile"},
        {"Erase", GameTool::Erase, "E", "Drag over cells to clear them"},
        {"Fill", GameTool::Fill, "G", "Click to flood-fill the area with the chosen tile"},
        {"Object", GameTool::Object, "O", "Drag a box (or click for a point) on the object layer"},
        {"Pick", GameTool::Pick, "I", "Click a cell to choose its tile"}};
    static const Item sprite[] = {
        {"Move", GameTool::Move, "V", "Drag to move around, scroll to zoom"},
        {"Pencil", GameTool::Pencil, "B", "Draw single pixels; drag for a freehand line"},
        {"Eraser", GameTool::Eraser, "E", "Rub pixels out to transparent"},
        {"Line", GameTool::Line, "L", "Drag a straight line"},
        {"Box", GameTool::Box, "U", "Drag a rectangle"},
        {"Ellipse", GameTool::Ellipse, "O", "Drag an ellipse"},
        {"Bucket", GameTool::Bucket, "G", "Click to fill the touching pixels of one colour"},
        {"Pick", GameTool::Pick, "I", "Click a pixel to take its colour"}};
    const bool isLevel = g.kind == "level";
    const Item* items = isLevel ? level : sprite;
    const std::size_t count = isLevel ? std::size(level) : std::size(sprite);
    for (std::size_t i = 0; i < count; ++i) {
        if (railItem(items[i].name, items[i].key, items[i].tip, g.tool == items[i].tool)) {
            g.tool = items[i].tool;
            g.dragging = false;
            g.points.clear();
        }
    }
}

void Shell::gameToolbar() {
    GameCanvas& g = canvas_.game;
    gameSync();
    if (g.kind == "scene") return sceneToolbar();
    const bool isLevel = g.kind == "level";
    if (!isLevel && (g.tool == GameTool::Box || g.tool == GameTool::Ellipse)) {
        ImGui::SameLine();
        ImGui::Checkbox("Filled", &g.filled);
    }
    if (isLevel) {
        ImGui::SameLine();
        ImGui::Checkbox("Grid", &g.grid);
    }
}

void Shell::gameInput(ImDrawList* dl, ImVec2 origin, bool hovered, bool active) {
    GameCanvas& g = canvas_.game;
    if (g.kind == "scene") return sceneInput(dl, origin, hovered, active);
    ImGuiIO& io = ImGui::GetIO();
    const float z = canvas_.zoom;
    const bool isLevel = g.kind == "level";
    // a map bigger than the preview's size limit comes back shrunk: cells are then smaller on the picture
    const float shrinkX = isLevel && g.mapW > 0 && canvas_.texture.srcWidth > 0
                              ? std::min(1.0f, static_cast<float>(canvas_.texture.srcWidth) / static_cast<float>(g.mapW * g.tileW))
                              : 1.0f;
    const float shrinkY = isLevel && g.mapH > 0 && canvas_.texture.srcHeight > 0
                              ? std::min(1.0f, static_cast<float>(canvas_.texture.srcHeight) / static_cast<float>(g.mapH * g.tileH))
                              : 1.0f;
    const float cw = isLevel ? static_cast<float>(g.tileW) * shrinkX : 1.0f, ch = isLevel ? static_cast<float>(g.tileH) * shrinkY : 1.0f;
    const int cols = isLevel ? g.mapW : g.width, rows = isLevel ? g.mapH : g.height;
    auto screenOf = [&](float x, float y) { return ImVec2{origin.x + x * z, origin.y + y * z}; };

    // grid: tile cells for levels, pixels for sprites once they are big enough to see
    if ((isLevel ? g.grid : true) && cw * z >= 6 && cols > 0 && rows > 0 && cols <= 1024 && rows <= 1024) {
        const ImU32 line = IM_COL32(128, 128, 128, isLevel ? 90 : 50);
        for (int x = 0; x <= cols; ++x) dl->AddLine(screenOf(x * cw, 0), screenOf(x * cw, rows * ch), line);
        for (int y = 0; y <= rows; ++y) dl->AddLine(screenOf(0, y * ch), screenOf(cols * cw, y * ch), line);
    }

    const ImVec2 img = toImage(io.MousePos);
    const int cx = static_cast<int>(std::floor(img.x / cw)), cy = static_cast<int>(std::floor(img.y / ch));
    const bool inside = cx >= 0 && cy >= 0 && cx < cols && cy < rows;
    const ImU32 accent = ImGui::ColorConvertFloat4ToU32(accent_);

    // press
    if (hovered && ImGui::IsItemActivated() && ImGui::IsMouseDown(ImGuiMouseButton_Left) && inside) {
        g.dragging = true;
        g.dragFrom = g.dragTo = {static_cast<float>(cx), static_cast<float>(cy)};
        g.points = {static_cast<double>(cx), static_cast<double>(cy)};
    }
    // drag (also on the release frame, so a fast flick keeps its last cells)
    if (g.dragging) {
        const int ex = std::clamp(cx, 0, std::max(0, cols - 1)), ey = std::clamp(cy, 0, std::max(0, rows - 1));
        g.dragTo = {static_cast<float>(ex), static_cast<float>(ey)};
        const bool path = g.tool == GameTool::Paint || g.tool == GameTool::Pencil || g.tool == GameTool::Eraser;
        if (path && inside && (g.points.size() < 2 || g.points[g.points.size() - 2] != cx || g.points.back() != cy)) {
            // tiles: fill in the cells a fast drag jumped over (the pencil joins its dots itself)
            if (isLevel && g.points.size() >= 2) {
                const int x0 = static_cast<int>(g.points[g.points.size() - 2]), y0 = static_cast<int>(g.points.back());
                const int steps = std::max(std::abs(cx - x0), std::abs(cy - y0));
                for (int i = 1; i < steps; ++i) {
                    g.points.push_back(std::lround(x0 + static_cast<double>(cx - x0) * i / steps));
                    g.points.push_back(std::lround(y0 + static_cast<double>(cy - y0) * i / steps));
                }
            }
            g.points.push_back(cx);
            g.points.push_back(cy);
        }
    }
    // release
    if (g.dragging && !active) {
        g.dragging = false;
        if (isLevel) gameLevelRelease();
        else gameSpriteRelease();
        g.points.clear();
    }

    // what the tool will do
    const ImU32 fill = IM_COL32(255, 255, 255, 70);
    if (g.dragging && dragsShape(g.tool)) {
        const float x0 = std::min(g.dragFrom.x, g.dragTo.x), y0 = std::min(g.dragFrom.y, g.dragTo.y);
        const float x1 = std::max(g.dragFrom.x, g.dragTo.x) + 1, y1 = std::max(g.dragFrom.y, g.dragTo.y) + 1;
        if (g.tool == GameTool::Line) {
            dl->AddLine(screenOf(g.dragFrom.x + 0.5f, g.dragFrom.y + 0.5f), screenOf(g.dragTo.x + 0.5f, g.dragTo.y + 0.5f), accent,
                        std::max(1.0f, z));
        } else if (g.tool == GameTool::Ellipse) {
            const ImVec2 a = screenOf(x0 * cw, y0 * ch), b = screenOf(x1 * cw, y1 * ch);
            dl->AddEllipse({(a.x + b.x) / 2, (a.y + b.y) / 2}, {(b.x - a.x) / 2, (b.y - a.y) / 2}, accent, 0, 0, 2);
        } else {
            dl->AddRectFilled(screenOf(x0 * cw, y0 * ch), screenOf(x1 * cw, y1 * ch), fill);
            dl->AddRect(screenOf(x0 * cw, y0 * ch), screenOf(x1 * cw, y1 * ch), accent, 0, 0, 2);
        }
    }
    if (g.dragging && !dragsShape(g.tool)) {
        for (std::size_t i = 0; i + 1 < g.points.size(); i += 2) {
            const float px = static_cast<float>(g.points[i]), py = static_cast<float>(g.points[i + 1]);
            dl->AddRectFilled(screenOf(px * cw, py * ch), screenOf((px + 1) * cw, (py + 1) * ch),
                              isLevel ? fill : ImGui::ColorConvertFloat4ToU32({g.color[0], g.color[1], g.color[2], g.tool == GameTool::Eraser ? 0.3f : g.color[3]}));
        }
    }
    if (hovered && inside && !g.dragging) {
        const ImVec2 a = screenOf(cx * cw, cy * ch), b = screenOf((cx + 1) * cw, (cy + 1) * ch);
        // a ghost of the tile about to go down
        if (isLevel && (g.tool == GameTool::Paint || g.tool == GameTool::Rect || g.tool == GameTool::Fill) && g.tilesetTex.tex && g.tsTiles > 0) {
            const int c = std::max(1, g.tsColumns), r = std::max(1, (g.tsTiles + c - 1) / c);
            const float u0 = static_cast<float>(g.tile % c) / c, v0 = static_cast<float>(g.tile / c) / r;
            dl->AddImage(g.tilesetTex.id(), a, b, {u0, v0}, {u0 + 1.0f / c, v0 + 1.0f / r}, IM_COL32(255, 255, 255, 170));
        }
        dl->AddRect(a, b, accent, 0, 0, 1.5f);
    }
}

void Shell::gameLevelRelease() {
    GameCanvas& g = canvas_.game;
    const std::string layer = g.layer < static_cast<int>(g.layers.size()) ? g.layers[static_cast<std::size_t>(g.layer)] : std::string();
    const std::string kind = g.layer < static_cast<int>(g.layerKinds.size()) ? g.layerKinds[static_cast<std::size_t>(g.layer)] : std::string();
    const int x0 = static_cast<int>(std::min(g.dragFrom.x, g.dragTo.x)), y0 = static_cast<int>(std::min(g.dragFrom.y, g.dragTo.y));
    const int w = static_cast<int>(std::fabs(g.dragTo.x - g.dragFrom.x)) + 1, h = static_cast<int>(std::fabs(g.dragTo.y - g.dragFrom.y)) + 1;
    Params tile{{"layer", layer}, {"tile", static_cast<std::int64_t>(g.tile)}};
    if (!g.tilesets.empty()) tile["tileset"] = g.tilesets[static_cast<std::size_t>(std::clamp(g.tileset, 0, static_cast<int>(g.tilesets.size()) - 1))];
    const bool tileLayer = kind == "tiles" || kind == "collision";
    if (g.tool != GameTool::Object && g.tool != GameTool::Pick && !tileLayer) {
        g.error = "Pick a tile layer on the right to paint on.";
        return;
    }
    switch (g.tool) {
        case GameTool::Paint: {
            Params p = tile;
            p["cells"] = g.points;
            gameRun("levels.paint", p, "Paint tiles");
            break;
        }
        case GameTool::Rect: {
            Params p = tile;
            p["x"] = static_cast<std::int64_t>(x0);
            p["y"] = static_cast<std::int64_t>(y0);
            p["width"] = static_cast<std::int64_t>(w);
            p["height"] = static_cast<std::int64_t>(h);
            gameRun("levels.fill_rect", p, "Fill rectangle");
            break;
        }
        case GameTool::Fill: {
            Params p = tile;
            p["x"] = static_cast<std::int64_t>(g.dragFrom.x);
            p["y"] = static_cast<std::int64_t>(g.dragFrom.y);
            gameRun("levels.flood_fill", p, "Flood fill");
            break;
        }
        case GameTool::Erase:
            gameRun("levels.erase", {{"layer", layer}, {"x", static_cast<std::int64_t>(x0)}, {"y", static_cast<std::int64_t>(y0)},
                                     {"width", static_cast<std::int64_t>(w)}, {"height", static_cast<std::int64_t>(h)}},
                    "Erase tiles");
            break;
        case GameTool::Object: {
            // the chosen layer when it holds objects, else the top object layer
            std::string target = kind == "objects" ? layer : std::string();
            for (std::size_t i = g.layers.size(); target.empty() && i-- > 0;) {
                if (g.layerKinds[i] == "objects") target = g.layers[i];
            }
            if (target.empty()) {
                g.error = "Add an object layer first (+ Objects on the right).";
                return;
            }
            Params p{{"layer", target}, {"x", static_cast<double>(x0 * g.tileW)}, {"y", static_cast<double>(y0 * g.tileH)}};
            if (w == 1 && h == 1 && ImGui::GetIO().KeyAlt) p["shape"] = std::string("point");
            p["width"] = static_cast<double>(w * g.tileW);
            p["height"] = static_cast<double>(h * g.tileH);
            gameRun("levels.object_add", p, "Add object");
            break;
        }
        case GameTool::Pick: {
            Result r = query("levels.cell", {{"doc", currentDoc_}, {"x", static_cast<std::int64_t>(g.dragFrom.x)}, {"y", static_cast<std::int64_t>(g.dragFrom.y)}});
            const double id = r.ok && !layer.empty() ? tp::number(r, layer, -1) : -1;
            if (id >= 0) g.tile = static_cast<int>(id);
            else g.error = "That cell is empty on " + layer + ".";
            break;
        }
        default: break;
    }
}

void Shell::gameSpriteRelease() {
    GameCanvas& g = canvas_.game;
    Params p{{"frame", static_cast<std::int64_t>(g.frame)}};
    if (g.spriteLayer < static_cast<int>(g.spriteLayers.size())) p["layer"] = std::to_string(g.spriteLayer);
    const NumberList corners{g.dragFrom.x, g.dragFrom.y, g.dragTo.x, g.dragTo.y};
    switch (g.tool) {
        case GameTool::Pencil:
        case GameTool::Eraser:
            p["tool"] = std::string(g.tool == GameTool::Pencil ? "pencil" : "erase");
            p["points"] = g.points;
            break;
        case GameTool::Line: p["tool"] = std::string("line"); p["points"] = corners; break;
        case GameTool::Box: p["tool"] = std::string(g.filled ? "fill_rect" : "rect"); p["points"] = corners; break;
        case GameTool::Ellipse: p["tool"] = std::string(g.filled ? "fill_ellipse" : "ellipse"); p["points"] = corners; break;
        case GameTool::Bucket: p["tool"] = std::string("bucket"); p["points"] = NumberList{g.dragFrom.x, g.dragFrom.y}; break;
        case GameTool::Pick: {
            const int x = static_cast<int>(g.dragFrom.x), y = static_cast<int>(g.dragFrom.y);
            if (x < g.pictureW && y < g.pictureH && !g.picture.empty()) {
                const std::uint8_t* px = g.picture.data() + (static_cast<std::size_t>(y) * g.pictureW + x) * 4;
                for (int i = 0; i < 4; ++i) g.color[i] = static_cast<float>(px[i]) / 255.0f;
            }
            return;
        }
        default: return;
    }
    if (g.tool != GameTool::Eraser) p["color"] = hexOf(g.color);
    gameRun("forge.draw", p, "Draw");
}

void Shell::gameSide() {
    gameSync();
    if (canvas_.game.kind == "scene") sceneSide();
    else if (canvas_.game.kind == "level") gameLevelSide();
    else gameSpriteSide();
    if (!canvas_.game.error.empty()) {
        ImGui::Spacing();
        tp::errorText(canvas_.game.error);
    }
}

void Shell::gameLevelSide() {
    GameCanvas& g = canvas_.game;
    tp::title("Layers", 1.05f);
    for (std::size_t i = g.layers.size(); i-- > 0;) {  // top first, like Tiled
        const std::string label = g.layers[i] + "  (" + g.layerKinds[i] + ")##layer" + std::to_string(i);
        if (ImGui::Selectable(label.c_str(), static_cast<int>(i) == g.layer)) g.layer = static_cast<int>(i);
    }
    auto addLayer = [&](const char* kind, const char* base) {
        int n = static_cast<int>(g.layers.size()) + 1;
        std::string name = std::string(base) + " " + std::to_string(n);
        while (std::find(g.layers.begin(), g.layers.end(), name) != g.layers.end()) name = std::string(base) + " " + std::to_string(++n);
        gameRun("levels.layer_add", {{"name", name}, {"kind", std::string(kind)}}, "Add layer");
        gameSync();
        g.layer = static_cast<int>(g.layers.size()) - 1;
    };
    if (ImGui::SmallButton("+ Tiles")) addLayer("tiles", "Tiles");
    ImGui::SameLine();
    if (ImGui::SmallButton("+ Objects")) addLayer("objects", "Objects");

    ImGui::Spacing();
    tp::title("Tiles", 1.05f);
    if (g.tilesets.empty()) {
        tp::muted("No tileset yet. Add one with Add Tileset, or Placeholder Tileset to start without art.");
        return;
    }
    if (g.tilesets.size() > 1) {
        ImGui::SetNextItemWidth(-1);
        if (ImGui::BeginCombo("##tileset", g.tilesets[static_cast<std::size_t>(g.tileset)].c_str())) {
            for (std::size_t i = 0; i < g.tilesets.size(); ++i) {
                if (ImGui::Selectable(g.tilesets[i].c_str(), static_cast<int>(i) == g.tileset)) {
                    g.tileset = static_cast<int>(i);
                    g.tile = 0;
                }
            }
            ImGui::EndCombo();
        }
    }
    const std::string key = currentDoc_ + "/" + std::to_string(g.tileset) + "/" + std::to_string(g.seen);
    if (g.tilesetFor != key) {
        g.tilesetFor = key;
        Result r = query("levels.tileset_preview", {{"doc", currentDoc_}, {"tileset", g.tilesets[static_cast<std::size_t>(g.tileset)]}});
        if (r.ok && tp::attachmentTexture(renderer_, g.tilesetTex, r)) {
            SDL_SetTextureScaleMode(g.tilesetTex.tex, SDL_SCALEMODE_NEAREST);
            g.tsColumns = std::max(1, static_cast<int>(tp::number(r, "columns", 1)));
            g.tsTiles = static_cast<int>(tp::number(r, "tiles"));
            g.tsTileW = std::max(1, static_cast<int>(tp::number(r, "tile_width", 16)));
            g.tsTileH = std::max(1, static_cast<int>(tp::number(r, "tile_height", 16)));
        } else {
            g.tilesetTex.reset();
            g.tsTiles = 0;
        }
    }
    g.tile = std::clamp(g.tile, 0, std::max(0, g.tsTiles - 1));
    ImGui::Text("Tile %d", g.tile);
    if (!g.tilesetTex.tex || g.tsTiles == 0) return;
    // the tileset, scaled to the panel's width (whole steps when it fits), each tile a click target
    const int rows = (g.tsTiles + g.tsColumns - 1) / g.tsColumns;
    const float sheetW = static_cast<float>(g.tsColumns * g.tsTileW), sheetH = static_cast<float>(rows * g.tsTileH);
    const float scale = std::min(ImGui::GetContentRegionAvail().x / sheetW, 6.0f);
    const ImVec2 size{sheetW * scale, sheetH * scale};
    const ImVec2 at = ImGui::GetCursorScreenPos();
    ImGui::InvisibleButton("tiles", size);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(at, {at.x + size.x, at.y + size.y}, IM_COL32(60, 60, 60, 255));
    dl->AddImage(g.tilesetTex.id(), at, {at.x + size.x, at.y + size.y});
    const float tw = g.tsTileW * scale, th = g.tsTileH * scale;
    if (ImGui::IsItemClicked()) {
        const ImVec2 m = ImGui::GetIO().MousePos;
        const int c = static_cast<int>((m.x - at.x) / tw), r = static_cast<int>((m.y - at.y) / th);
        const int id = r * g.tsColumns + c;
        if (c >= 0 && c < g.tsColumns && id >= 0 && id < g.tsTiles) {
            g.tile = id;
            if (g.tool == GameTool::Move || g.tool == GameTool::Pick || g.tool == GameTool::Erase || g.tool == GameTool::Object) g.tool = GameTool::Paint;
        }
    }
    if (ImGui::IsItemHovered()) {
        const ImVec2 m = ImGui::GetIO().MousePos;
        const int c = static_cast<int>((m.x - at.x) / tw), r = static_cast<int>((m.y - at.y) / th);
        if (r * g.tsColumns + c < g.tsTiles) ImGui::SetTooltip("tile %d", r * g.tsColumns + c);
    }
    const ImVec2 s0{at.x + (g.tile % g.tsColumns) * tw, at.y + static_cast<float>(g.tile / g.tsColumns) * th};
    dl->AddRect(s0, {s0.x + tw, s0.y + th}, ImGui::ColorConvertFloat4ToU32(accent_), 0, 0, 2.5f);
}

void Shell::gameSpriteSide() {
    GameCanvas& g = canvas_.game;
    const float unit = ImGui::GetFontSize();
    // playback: advance by each frame's own duration
    if (g.playing) {
        const double now = tp::seconds();
        const int ms = g.frame < static_cast<int>(g.durations.size()) ? g.durations[static_cast<std::size_t>(g.frame)] : 100;
        if (now - g.playClock >= ms / 1000.0) {
            g.playClock = now;
            g.frame = (g.frame + 1) % std::max(1, g.frames);
            canvas_.dirty = true;
        }
    }
    tp::title("Frames", 1.05f);
    auto show = [&](int f) {
        g.frame = std::clamp(f, 0, g.frames - 1);
        canvas_.dirty = true;
    };
    if (ImGui::ArrowButton("##prev", ImGuiDir_Left)) show(g.frame == 0 ? g.frames - 1 : g.frame - 1);
    ImGui::SameLine();
    ImGui::Text("%d / %d", g.frame + 1, g.frames);
    ImGui::SameLine();
    if (ImGui::ArrowButton("##next", ImGuiDir_Right)) show((g.frame + 1) % g.frames);
    ImGui::SameLine();
    if (ImGui::SmallButton(g.playing ? "Stop" : "Play")) {
        g.playing = !g.playing;
        g.playClock = tp::seconds();
        canvas_.dirty = true;
    }
    if (ImGui::Checkbox("Onion skin", &g.onion)) canvas_.dirty = true;
    ImGui::SetItemTooltip("Show the frames before (red) and after (blue) faintly under this one");
    if (ImGui::SmallButton("+ Frame")) {
        gameRun("forge.frame_add", {{"at", static_cast<std::int64_t>(g.frame + 1)}, {"copy", static_cast<std::int64_t>(g.frame)}}, "Add frame");
        gameSync();
        show(g.frame + 1);
    }
    ImGui::SetItemTooltip("Add a copy of this frame after it");
    ImGui::SameLine();
    if (ImGui::SmallButton("+ Empty")) {
        gameRun("forge.frame_add", {{"at", static_cast<std::int64_t>(g.frame + 1)}, {"empty", true}}, "Add frame");
        gameSync();
        show(g.frame + 1);
    }
    ImGui::SameLine();
    ImGui::BeginDisabled(g.frames < 2);
    if (ImGui::SmallButton("Delete")) gameRun("forge.frame_remove", {{"frame", static_cast<std::int64_t>(g.frame)}}, "Remove frame");
    ImGui::EndDisabled();
    int ms = g.frame < static_cast<int>(g.durations.size()) ? g.durations[static_cast<std::size_t>(g.frame)] : 100;
    ImGui::SetNextItemWidth(unit * 6);
    if (ImGui::InputInt("ms##dur", &ms, 10, 50, ImGuiInputTextFlags_EnterReturnsTrue) && ms > 0) {
        gameRun("forge.frame_duration", {{"ms", static_cast<std::int64_t>(ms)}, {"frames", NumberList{static_cast<double>(g.frame)}}}, "Frame duration");
    }
    ImGui::SetItemTooltip("How long this frame shows (Enter to set)");
    for (std::size_t t = 0; t < g.tags.size(); ++t) {
        const std::string range = t < g.tagRanges.size() ? g.tagRanges[t] : std::string();
        const std::string label = g.tags[t] + "  " + range + "##tag" + std::to_string(t);
        if (ImGui::Selectable(label.c_str(), false)) show(std::atoi(range.c_str()));
    }

    ImGui::Spacing();
    tp::title("Layers", 1.05f);
    for (std::size_t i = g.spriteLayers.size(); i-- > 0;) {
        ImGui::PushID(static_cast<int>(i));
        bool shown = i < g.layerShown.size() ? g.layerShown[i] : true;
        if (ImGui::Checkbox("##shown", &shown)) {
            gameRun("forge.sprite_layer_set", {{"layer", std::to_string(i)}, {"visible", shown}}, shown ? "Show layer" : "Hide layer");
        }
        ImGui::SetItemTooltip("Show or hide this layer");
        ImGui::SameLine();
        if (ImGui::Selectable(g.spriteLayers[i].c_str(), static_cast<int>(i) == g.spriteLayer)) g.spriteLayer = static_cast<int>(i);
        ImGui::PopID();
    }
    if (ImGui::SmallButton("+ Layer")) {
        int n = static_cast<int>(g.spriteLayers.size()) + 1;
        std::string name = "Layer " + std::to_string(n);
        while (std::find(g.spriteLayers.begin(), g.spriteLayers.end(), name) != g.spriteLayers.end()) name = "Layer " + std::to_string(++n);
        gameRun("forge.sprite_layer_add", {{"name", name}}, "Add layer");
        gameSync();
        g.spriteLayer = static_cast<int>(g.spriteLayers.size()) - 1;
    }

    ImGui::Spacing();
    tp::title("Colour", 1.05f);
    ImGui::ColorEdit4("##colour", g.color, ImGuiColorEditFlags_AlphaBar | ImGuiColorEditFlags_NoInputs);
    ImGui::SameLine();
    ImGui::TextDisabled("%s", hexOf(g.color).c_str());
    const float swatch = unit * 1.3f;
    const int perRow = std::max(1, static_cast<int>(ImGui::GetContentRegionAvail().x / (swatch + ImGui::GetStyle().ItemSpacing.x)));
    int shownCount = 0;
    // no palette of its own yet: PICO-8's 16 colours to pick from, as Aseprite starts with one
    static const std::vector<std::string> starter = {"#000000", "#1d2b53", "#7e2553", "#008751", "#ab5236", "#5f574f", "#c2c3c7", "#fff1e8",
                                                     "#ff004d", "#ffa300", "#ffec27", "#00e436", "#29adff", "#83769c", "#ff77a8", "#ffccaa"};
    const std::vector<std::string>& colours = g.palette.empty() ? starter : g.palette;
    for (std::size_t i = 0; i < colours.size(); ++i) {
        float c[4];
        if (!parseHex(colours[i], c)) continue;
        if (shownCount++ % perRow) ImGui::SameLine();
        ImGui::PushID(static_cast<int>(i));
        if (ImGui::ColorButton("##swatch", {c[0], c[1], c[2], c[3]}, ImGuiColorEditFlags_AlphaPreviewHalf, {swatch, swatch})) std::copy(c, c + 4, g.color);
        ImGui::SetItemTooltip("%zu: %s", i, colours[i].c_str());
        ImGui::PopID();
    }
    if (g.palette.empty()) tp::muted("Sprite palette (ribbon) loads others: db16, db32, gameboy...");
}

}  // namespace wetwaresuite::gui
