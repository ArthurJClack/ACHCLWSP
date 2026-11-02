// forge.sprite_* / forge.frame_* / forge.tag_* / forge.draw: Aseprite-style
// animated sprites. Every edit is one SpriteEdit, so it undoes as one step.
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

#include "wetwaresuite/forge/sprite.hpp"
#include "wetwaresuite/forge/texture.hpp"
#include "wetwaresuite/raster/codec.hpp"
#include "wetwaresuite/raster/document.hpp"
#include "wetwaresuite/raster/ops.hpp"
#include "common.hpp"
#include "images.hpp"

namespace wetwaresuite::forge::detail {

namespace {

bool has(const Params& p, const std::string& key) {
    auto it = p.find(key);
    return it != p.end() && !std::holds_alternative<std::monostate>(it->second);
}

std::string plural(std::size_t n, const std::string& word) { return std::to_string(n) + " " + word + (n == 1 ? "" : "s"); }

std::vector<std::string> splitList(const std::string& text) {
    std::vector<std::string> out;
    std::string item;
    for (char c : text + ",") {
        if (c == ',') {
            const auto a = item.find_first_not_of(" \t"), b = item.find_last_not_of(" \t");
            if (a != std::string::npos) out.push_back(item.substr(a, b - a + 1));
            item.clear();
        } else {
            item += c;
        }
    }
    return out;
}

using SpriteFn = std::function<Result(SpriteDoc&, const Params&)>;

ParamSpec spriteParam() { return {"doc", ParamType::String, "id of an open sprite, e.g. spr1", true}; }
ParamSpec frameParam(const std::string& what = "frame number, 0 = first") { return {"frame", ParamType::Int, what, false, {}, std::int64_t{0}}; }
ParamSpec layerParam() { return {"layer", ParamType::String, "layer name or index (0 = bottom; default: the top layer)", false}; }
ParamSpec previewParam() { return {"preview", ParamType::Bool, "attach a PNG of the sprite's frames", false, {}, false}; }

Result edited(const std::string& message, const Params& p) {
    Result r = Result::success(message);
    r.with("doc", stringParam(p, "doc"));
    return r;
}

ActionFactory spriteCommand(Session& session, SpriteFn fn, bool undoable = true) {
    return [&session, fn, undoable](const Params& p) -> std::unique_ptr<Action> {
        const std::string id = stringParam(p, "doc");
        auto doc = session.workspace().getAs<SpriteDoc>(id);
        if (!doc) {
            return std::make_unique<FunctionAction>(
                [id] { return Result::failure("no open sprite called '" + id + "' (use forge.sprite_new or forge.sprite_open first)"); });
        }
        auto run = [fn](SpriteDoc& d, const Params& q) {
            Result r = fn(d, q);
            if (r.ok && boolParam(q, "preview", false)) r.attach(*d.preview());
            return r;
        };
        if (!undoable) {
            return std::make_unique<FunctionAction>([doc, run, p] {
                try {
                    return run(*doc, p);
                } catch (const std::exception& e) {
                    return Result::failure(e.what());
                }
            });
        }
        return std::make_unique<SpriteEdit>(doc, [run, p](SpriteDoc& d) { return run(d, p); });
    };
}

Result created(Session& s, std::shared_ptr<SpriteDoc> doc, const std::string& verb) {
    const std::string id = s.workspace().add(doc, "spr");
    Result r = Result::success(verb + " " + id + ": " + doc->summary(), id);
    r.with("doc", id)
        .with("width", static_cast<std::int64_t>(doc->sprite.width))
        .with("height", static_cast<std::int64_t>(doc->sprite.height))
        .with("frames", static_cast<std::int64_t>(doc->sprite.frames()))
        .with("layers", static_cast<std::int64_t>(doc->sprite.layers.size()));
    return r;
}

// The frame a command works on, checked.
bool frameArg(const Sprite& s, const Params& p, int& out, std::string& error, const std::string& key = "frame") {
    out = static_cast<int>(intParam(p, key, 0));
    if (out < 0 || out >= s.frames()) {
        error = "the sprite has frames 0.." + std::to_string(s.frames() - 1) + ", not " + std::to_string(out);
        return false;
    }
    return true;
}

// The layer a command paints on: layer=, else the top non-group layer.
bool layerArg(const Sprite& s, const Params& p, int& out, std::string& error) {
    if (has(p, "layer")) {
        out = s.layerIndex(stringParam(p, "layer"));
        if (out < 0) error = "no layer called '" + stringParam(p, "layer") + "'";
        else if (s.layers[static_cast<std::size_t>(out)].group) error = "'" + stringParam(p, "layer") + "' is a group; pick a layer inside it";
        return error.empty();
    }
    for (int i = static_cast<int>(s.layers.size()) - 1; i >= 0; --i) {
        if (!s.layers[static_cast<std::size_t>(i)].group) {
            out = i;
            return true;
        }
    }
    error = "the sprite has no layer to draw on";
    return false;
}

// color=#rrggbb or index=N (palette); in indexed sprites colours snap to the palette.
bool colorArg(const Sprite& s, const Params& p, Color& out, std::string& error, const std::string& key = "color") {
    if (has(p, "index") && key == "color") {
        const int i = static_cast<int>(intParam(p, "index"));
        if (i < 0 || i >= static_cast<int>(s.palette.size())) {
            error = "the palette has entries 0.." + std::to_string(static_cast<int>(s.palette.size()) - 1);
            return false;
        }
        out = s.indexed && i == s.transparentIndex ? Color{0, 0, 0, 0} : s.palette[static_cast<std::size_t>(i)];
        return true;
    }
    if (!has(p, key)) {
        error = "give " + key + "=#rrggbb" + (key == "color" ? " (or index= for a palette entry)" : "");
        return false;
    }
    auto c = Color::parse(stringParam(p, key));
    if (!c) {
        error = "not a colour: " + stringParam(p, key);
        return false;
    }
    out = *c;
    if (s.indexed && !s.palette.empty() && out.a >= 128) {
        const int k = nearestIndex(s.palette, out, s.transparentIndex);
        if (k >= 0) out = s.palette[static_cast<std::size_t>(k)];
        out.a = 255;
    } else if (s.indexed && out.a < 128) {
        out = {0, 0, 0, 0};
    }
    return true;
}

std::vector<Color> parseColors(const std::string& text, std::string& error) {
    std::vector<Color> out;
    for (const auto& item : splitList(text)) {
        auto c = Color::parse(item);
        if (!c) {
            error = "not a colour: " + item;
            return {};
        }
        out.push_back(*c);
    }
    return out;
}

std::string colorsText(const std::vector<Color>& cs) {
    std::string s;
    for (const auto& c : cs) s += (s.empty() ? "" : ",") + (c.a == 255 ? c.hex().substr(0, 7) : c.hex());
    return s;
}

// Frames of a new sprite from an image cut into cells.
Sprite spriteFromCells(const Image& sheet, int cw, int ch, int count, int duration) {
    const int cols = std::max(1, sheet.width() / cw), rows = std::max(1, sheet.height() / ch);
    Sprite s = newSprite(cw, ch, 1, duration);
    s.durations.clear();
    s.cels[0].clear();
    for (int r = 0; r < rows; ++r) {
        for (int c = 0; c < cols; ++c) {
            if (count > 0 && s.frames() >= count) break;
            Image cell = raster::crop(sheet, c * cw, r * ch, cw, ch);
            if (count <= 0 && opaqueBounds(cell).empty() && s.frames() > 0) continue;  // trailing empty cells
            s.durations.push_back(duration);
            s.cels[0].push_back(std::move(cell));
        }
    }
    if (s.frames() == 0) {
        s.durations.push_back(duration);
        s.cels[0].push_back(Image(cw, ch));
    }
    return s;
}

const std::vector<std::string>& blendNames() {
    static const std::vector<std::string> n = {"normal", "multiply", "screen", "overlay", "darken", "lighten", "difference", "addition"};
    return n;
}

int blendNumber(const std::string& name) {
    static const std::map<std::string, int> m = {{"normal", 0},  {"multiply", 1},   {"screen", 2},    {"overlay", 3},
                                                 {"darken", 4},  {"lighten", 5},    {"difference", 10}, {"addition", 16}};
    auto it = m.find(name);
    return it == m.end() ? 0 : it->second;
}

std::string blendName(int number) {
    for (const auto& n : blendNames())
        if (blendNumber(n) == number) return n;
    return "mode " + std::to_string(number);
}

}  // namespace

void registerAnimFileCommands(Session& session) {
    auto& reg = session.registry();
    Session* s = &session;

    reg.add({"forge.sprite_new", "New sprite",
             "Create an animated pixel-art sprite (Aseprite style) and return its id (e.g. spr1): width x height px, "
             "a number of frames, one layer. color_mode=indexed keeps every pixel on the palette (palette= a preset "
             "pico8, db16, db32, gameboy, grayscale4, a .gpl/.pal file, or colours #a,#b,...), so palette swaps "
             "recolour the whole sprite. Then draw with forge.draw, add frames and tags, and save as .aseprite or "
             "export a sheet.",
             {{"width", ParamType::Int, "px", true}, {"height", ParamType::Int, "px", true},
              {"frames", ParamType::Int, "number of frames", false, {}, std::int64_t{1}},
              {"duration", ParamType::Int, "ms per frame", false, {}, std::int64_t{100}},
              {"color_mode", ParamType::String, "rgba or indexed (palette only)", false, {"rgba", "indexed"}, std::string("rgba")},
              {"palette", ParamType::String, "preset name, palette file, or comma-separated colours", false},
              {"background", ParamType::String, "fill colour for the first layer (default transparent)", false}},
             false},
            [s](const Params& p) -> std::unique_ptr<Action> {
                return std::make_unique<FunctionAction>([s, p]() -> Result {
                    const int w = static_cast<int>(intParam(p, "width")), h = static_cast<int>(intParam(p, "height"));
                    if (w <= 0 || h <= 0 || w > 4096 || h > 4096) return Result::failure("sprite size must be 1..4096 px");
                    const int frames = static_cast<int>(std::clamp<std::int64_t>(intParam(p, "frames", 1), 1, 1000));
                    auto doc = std::make_shared<SpriteDoc>();
                    doc->sprite = newSprite(w, h, frames, static_cast<int>(std::clamp<std::int64_t>(intParam(p, "duration", 100), 1, 65535)));
                    Sprite& sp = doc->sprite;
                    if (has(p, "palette")) {
                        const std::string pal = stringParam(p, "palette");
                        std::string error;
                        sp.palette = presetPalette(pal);
                        std::error_code ec;
                        if (sp.palette.empty() && fs::exists(pal, ec)) {
                            try {
                                sp.palette = loadPalette(pal);
                            } catch (const std::exception& e) {
                                return Result::failure(e.what());
                            }
                        }
                        if (sp.palette.empty()) sp.palette = parseColors(pal, error);
                        if (!error.empty() || sp.palette.empty()) return Result::failure("palette= is a preset (pico8, db16, db32, gameboy, grayscale4), a .gpl/.pal file or colours");
                    }
                    if (stringParam(p, "color_mode", "rgba") == "indexed") {
                        if (sp.palette.empty()) sp.palette = presetPalette("db32");
                        sp.palette.insert(sp.palette.begin(), Color{0, 0, 0, 0});  // index 0 = transparent
                        if (sp.palette.size() > 256) return Result::failure("indexed sprites hold at most 255 colours");
                        sp.indexed = true;
                        sp.transparentIndex = 0;
                    }
                    if (has(p, "background")) {
                        auto c = Color::parse(stringParam(p, "background"));
                        if (!c) return Result::failure("not a colour: " + stringParam(p, "background"));
                        for (int f = 0; f < sp.frames(); ++f) sp.cel(0, f).fill(*c);
                        sp.layers[0].name = "Background";
                        snapToPalette(sp);
                    }
                    return created(*s, doc, "created");
                });
            });

    reg.add({"forge.sprite_open", "Open sprite",
             "Open an animated sprite: an Aseprite .aseprite/.ase file (layers, frames, durations, tags, palette, "
             "indexed colour), a sprite sheet image cut into frames (cell_width/cell_height or columns/rows), a single "
             "image, a folder of frame images (input_dir, in natural order) or a list (files=).",
             {{"path", ParamType::String, ".aseprite/.ase file or image", false},
              {"input_dir", ParamType::String, "folder of frame images instead", false},
              {"files", ParamType::String, "comma-separated frame images (or open image ids) instead", false},
              {"cell_width", ParamType::Int, "sheet: frame width in px", false}, {"cell_height", ParamType::Int, "sheet: frame height in px", false},
              {"columns", ParamType::Int, "sheet: frames across (instead of cell_width)", false}, {"rows", ParamType::Int, "sheet: frames down", false},
              {"count", ParamType::Int, "sheet: number of frames (default: every non-empty cell)", false},
              {"duration", ParamType::Int, "ms per frame for images", false, {}, std::int64_t{100}}, previewParam()},
             false, true},
            [s](const Params& p) -> std::unique_ptr<Action> {
                return std::make_unique<FunctionAction>([s, p]() -> Result {
                    try {
                        auto doc = std::make_shared<SpriteDoc>();
                        const int duration = static_cast<int>(std::clamp<std::int64_t>(intParam(p, "duration", 100), 1, 65535));
                        std::vector<Image> frames;
                        if (has(p, "path")) {
                            const fs::path path = stringParam(p, "path");
                            const std::string ext = lowerExt(path);
                            if (ext == "aseprite" || ext == "ase") {
                                std::ifstream in(path, std::ios::binary);
                                if (!in) return Result::failure("can't read " + path.string());
                                std::stringstream data;
                                data << in.rdbuf();
                                doc->sprite = readAseprite(data.str());
                            } else {
                                if (!raster::canOpenImage(path)) return Result::failure("can't read " + path.string() + " (use .aseprite, .ase or an image)");
                                Image img = raster::openImage(path)->flatten();
                                int cw = static_cast<int>(intParam(p, "cell_width", 0)), ch = static_cast<int>(intParam(p, "cell_height", 0));
                                if (has(p, "columns")) cw = img.width() / std::max<int>(1, static_cast<int>(intParam(p, "columns")));
                                if (has(p, "rows")) ch = img.height() / std::max<int>(1, static_cast<int>(intParam(p, "rows")));
                                if (cw > 0 && ch <= 0) ch = img.height();
                                if (ch > 0 && cw <= 0) cw = img.width();
                                if (cw > 0) {
                                    doc->sprite = spriteFromCells(img, cw, ch, static_cast<int>(intParam(p, "count", 0)), duration);
                                } else {
                                    frames.push_back(std::move(img));
                                }
                            }
                            doc->path = path.string();
                        } else if (has(p, "input_dir") || has(p, "files")) {
                            std::vector<std::string> refs;
                            if (has(p, "files")) {
                                refs = splitList(stringParam(p, "files"));
                            } else {
                                std::error_code ec;
                                const fs::path dir = stringParam(p, "input_dir");
                                if (!fs::is_directory(dir, ec)) return Result::failure("not a folder: " + dir.string());
                                for (const auto& e : fs::directory_iterator(dir, ec)) {
                                    if (e.is_regular_file() && raster::canOpenImage(e.path())) refs.push_back(e.path().string());
                                }
                                std::sort(refs.begin(), refs.end(), [](const std::string& a, const std::string& b) {
                                    return naturalLess(fs::path(a).filename().string(), fs::path(b).filename().string());
                                });
                            }
                            for (const auto& ref : refs) {
                                std::string error;
                                auto img = resolveImage(*s, ref, error);
                                if (!img) return Result::failure(error);
                                frames.push_back(std::move(*img));
                            }
                            if (frames.empty()) return Result::failure("no frame images found");
                        } else {
                            return Result::failure("give path= (.aseprite or image), input_dir= or files=");
                        }
                        if (!frames.empty()) {
                            int w = 0, h = 0;
                            for (const auto& f : frames) {
                                w = std::max(w, f.width());
                                h = std::max(h, f.height());
                            }
                            doc->sprite = newSprite(w, h, static_cast<int>(frames.size()), duration);
                            for (std::size_t i = 0; i < frames.size(); ++i) blit(doc->sprite.cel(0, static_cast<int>(i)), frames[i], 0, 0);
                        }
                        Result r = created(*s, doc, "opened");
                        if (boolParam(p, "preview", false)) r.attach(*doc->preview());
                        return r;
                    } catch (const std::exception& e) {
                        return Result::failure(e.what());
                    }
                });
            });

    reg.add({"forge.sprite_save", "Save sprite",
             "Save a sprite. .aseprite/.ase writes an Aseprite file (layers, frames, tags, palette; indexed sprites "
             "stay indexed) that Aseprite and engine importers open; .gif an animated GIF (tag= for one animation, "
             "scale= to enlarge); .png/.bmp/.tga one flattened frame (frame=). Sheets: forge.sprite_export.",
             {spriteParam(), {"path", ParamType::String, "output file", true}, frameParam("png: which frame"),
              {"tag", ParamType::String, "gif: only this tag's frames", false},
              {"scale", ParamType::Int, "gif/png: whole-number enlargement", false, {}, std::int64_t{1}}},
             false},
            spriteCommand(
                session,
                [](SpriteDoc& d, const Params& p) {
                    const fs::path path = stringParam(p, "path");
                    const std::string ext = lowerExt(path);
                    const Sprite& sp = d.sprite;
                    const int scale = static_cast<int>(std::clamp<std::int64_t>(intParam(p, "scale", 1), 1, 16));
                    makeParent(path);
                    std::string bytes;
                    if (ext == "aseprite" || ext == "ase") {
                        bytes = writeAseprite(sp);
                    } else if (ext == "gif") {
                        std::vector<int> frames;
                        if (has(p, "tag")) {
                            const int t = sp.tagIndex(stringParam(p, "tag"));
                            if (t < 0) return Result::failure("no tag called '" + stringParam(p, "tag") + "'");
                            frames = sp.tagFrames(sp.tags[static_cast<std::size_t>(t)]);
                        }
                        bytes = encodeSpriteGif(sp, frames, scale);
                    } else if (raster::canWrite(path)) {
                        std::string error;
                        int f = 0;
                        if (!frameArg(sp, p, f, error)) return Result::failure(error);
                        Image img = sp.flatten(f);
                        if (scale > 1) img = raster::resize(img, img.width() * scale, img.height() * scale, raster::Filter::Nearest);
                        if (auto err = writeImage(img, path.string()); !err.empty()) return Result::failure(err);
                    } else {
                        return Result::failure("save sprites as .aseprite, .ase, .gif or an image (.png)");
                    }
                    if (!bytes.empty()) {
                        std::ofstream out(path, std::ios::binary);
                        out << bytes;
                        if (!out) return Result::failure("can't write " + path.string());
                    }
                    if (ext == "aseprite" || ext == "ase") d.path = path.string();
                    std::error_code ec;
                    Result r = Result::success("saved " + path.string(), path.string());
                    return r.with("path", path.string()).with("bytes", static_cast<std::int64_t>(fs::file_size(path, ec)));
                },
                false));

    reg.add({"forge.sprite_export", "Export sprite sheet",
             "Export a sprite's frames as one sheet PNG plus JSON in Aseprite's format (frames with durations, "
             "meta.frameTags, meta.layers; hash or array, which Phaser, PixiJS, Godot and Unity importers read), "
             "optionally a Godot 4 SpriteFrames .tres (one animation per tag) for AnimatedSprite2D, and/or every "
             "frame as its own PNG. layout: horizontal strip, vertical, grid or packed (with trim=true).",
             {spriteParam(), {"output", ParamType::String, "sheet PNG to write", true},
              {"data", ParamType::String, "JSON to write (default: output with .json; 'none' to skip)", false},
              {"format", ParamType::String, "JSON frames as a hash or an array", false, {"hash", "array"}, std::string("hash")},
              {"layout", ParamType::String, "how frames sit on the sheet", false, {"horizontal", "vertical", "grid", "packed"}, std::string("horizontal")},
              {"columns", ParamType::Int, "grid: frames per row (0 = about square)", false, {}, std::int64_t{0}},
              {"padding", ParamType::Int, "px between frames", false, {}, std::int64_t{0}},
              {"trim", ParamType::Bool, "cut transparent borders (offsets go in the JSON)", false, {}, false},
              {"scale", ParamType::Int, "whole-number enlargement", false, {}, std::int64_t{1}},
              {"tag", ParamType::String, "only this tag's frames", false},
              {"godot", ParamType::String, "also write a Godot SpriteFrames .tres here", false},
              {"frames_dir", ParamType::String, "also write each frame as <name>_<n>.png into this folder", false}},
             false},
            spriteCommand(
                session,
                [s](SpriteDoc& d, const Params& p) {
                    const Sprite& sp = d.sprite;
                    const fs::path output = stringParam(p, "output");
                    if (lowerExt(output) != "png") return Result::failure("output should be a .png (sheets need transparency)");
                    SheetOptions o;
                    o.layout = stringParam(p, "layout", "horizontal");
                    o.columns = static_cast<int>(intParam(p, "columns", 0));
                    o.padding = static_cast<int>(std::clamp<std::int64_t>(intParam(p, "padding", 0), 0, 256));
                    o.trim = boolParam(p, "trim", false);
                    o.scale = static_cast<int>(std::clamp<std::int64_t>(intParam(p, "scale", 1), 1, 16));
                    o.format = stringParam(p, "format", "hash");
                    o.imageName = output.filename().string();
                    o.title = d.path.empty() ? output.stem().string() : fs::path(d.path).stem().string();
                    if (has(p, "tag")) {
                        const int t = sp.tagIndex(stringParam(p, "tag"));
                        if (t < 0) return Result::failure("no tag called '" + stringParam(p, "tag") + "'");
                        for (int f = sp.tags[static_cast<std::size_t>(t)].from; f <= sp.tags[static_cast<std::size_t>(t)].to; ++f) o.frames.push_back(f);
                    }
                    SheetResult sheet = exportSheet(sp, o);
                    if (sheet.image.width() > 16384 || sheet.image.height() > 16384) return Result::failure("the sheet would be over 16384 px; use layout=grid");
                    if (auto err = writeImage(sheet.image, output.string()); !err.empty()) return Result::failure(err);
                    Result r = Result::success("exported " + plural(sheet.placed.size(), "frame") + " to " + std::to_string(sheet.image.width()) + "x" +
                                               std::to_string(sheet.image.height()) + " " + output.string());
                    r.with("output", output.string());
                    const std::string data = has(p, "data") ? stringParam(p, "data") : fs::path(output).replace_extension(".json").string();
                    if (data != "none") {
                        makeParent(data);
                        std::ofstream out(data, std::ios::binary);
                        out << sheet.json;
                        if (!out) return Result::failure("can't write " + data);
                        r.message += " + " + data;
                        r.with("data", data);
                    }
                    if (has(p, "godot")) {
                        const fs::path tres = stringParam(p, "godot");
                        makeParent(tres);
                        std::error_code ec;
                        const fs::path base = fs::absolute(tres.parent_path().empty() ? fs::path(".") : tres.parent_path(), ec).lexically_normal();
                        std::string rel = fs::absolute(output, ec).lexically_normal().lexically_relative(base).generic_string();
                        if (rel.empty()) rel = output.filename().string();
                        std::ofstream out(tres, std::ios::binary);
                        out << godotSpriteFrames(sp, sheet, rel, o.scale);
                        if (!out) return Result::failure("can't write " + tres.string());
                        r.message += " + " + tres.string();
                        r.with("godot", tres.string());
                    }
                    if (has(p, "frames_dir")) {
                        const fs::path dir = stringParam(p, "frames_dir");
                        for (const auto& pl : sheet.placed) {
                            Image img = sp.flatten(pl.frame);
                            if (o.scale > 1) img = raster::resize(img, img.width() * o.scale, img.height() * o.scale, raster::Filter::Nearest);
                            char name[32];
                            std::snprintf(name, sizeof name, "_%03d.png", pl.frame);
                            if (auto err = writeImage(img, (dir / (o.title + name)).string()); !err.empty()) return Result::failure(err);
                        }
                        r.message += " + frames in " + dir.string();
                    }
                    r.with("frames", static_cast<std::int64_t>(sheet.placed.size()))
                        .with("width", static_cast<std::int64_t>(sheet.image.width()))
                        .with("height", static_cast<std::int64_t>(sheet.image.height()));
                    (void)s;
                    return r;
                },
                false));

    reg.add({"forge.sprite_info", "Sprite info", "Describe a sprite: size, frames with durations, layers, tags and palette.",
             {spriteParam(), previewParam()}, false, true},
            spriteCommand(
                session,
                [](SpriteDoc& d, const Params& p) {
                    const Sprite& sp = d.sprite;
                    Result r = edited(d.summary(), p);
                    int total = 0;
                    std::string durs;
                    for (int ms : sp.durations) {
                        total += ms;
                        durs += (durs.empty() ? "" : ",") + std::to_string(ms);
                    }
                    r.message += "\nframes: " + std::to_string(sp.frames()) + " (" + durs + " ms; " + std::to_string(total) + " ms in all)";
                    r.message += "\nlayers (bottom first):";
                    for (std::size_t i = 0; i < sp.layers.size(); ++i) {
                        const auto& l = sp.layers[i];
                        int cels = 0;
                        for (int f = 0; f < sp.frames(); ++f) cels += sp.celIf(static_cast<int>(i), f) ? 1 : 0;
                        r.message += "\n  " + std::to_string(i) + ": " + std::string(static_cast<std::size_t>(l.level) * 2, ' ') + l.name +
                                     (l.group ? " (group)" : " (" + plural(static_cast<std::size_t>(cels), "cel") + ")") + (l.visible ? "" : ", hidden") +
                                     (l.opacity < 255 ? ", opacity " + std::to_string(l.opacity) : "") +
                                     (l.blend != 0 ? ", " + blendName(l.blend) : "");
                    }
                    if (!sp.tags.empty()) r.message += "\ntags:";
                    for (const auto& t : sp.tags) {
                        r.message += "\n  " + t.name + ": frames " + std::to_string(t.from) + ".." + std::to_string(t.to) + ", " + t.direction +
                                     (t.repeat ? ", " + std::to_string(t.repeat) + " times" : "");
                    }
                    if (!sp.palette.empty()) r.message += "\npalette: " + colorsText(sp.palette);
                    std::string tags, ranges, names, shown, dirs, repeats;
                    for (const auto& t : sp.tags) {
                        tags += (tags.empty() ? "" : ",") + t.name;
                        ranges += (ranges.empty() ? "" : ",") + std::to_string(t.from) + "-" + std::to_string(t.to);
                        dirs += (dirs.empty() ? "" : ",") + t.direction;
                        repeats += (repeats.empty() ? "" : ",") + std::to_string(t.repeat);
                    }
                    for (const auto& l : sp.layers) {
                        names += (names.empty() ? "" : ",") + l.name;
                        shown += std::string(shown.empty() ? "" : ",") + (l.visible ? "1" : "0");
                    }
                    r.with("width", static_cast<std::int64_t>(sp.width))
                        .with("height", static_cast<std::int64_t>(sp.height))
                        .with("frames", static_cast<std::int64_t>(sp.frames()))
                        .with("layers", static_cast<std::int64_t>(sp.layers.size()))
                        .with("tags", tags)
                        .with("durations", durs)
                        .with("palette", colorsText(sp.palette))
                        .with("indexed", sp.indexed)
                        .with("layer_names", names)
                        .with("layer_visible", shown)
                        .with("tag_ranges", ranges)
                        .with("tag_directions", dirs)
                        .with("tag_repeats", repeats);
                    return r;
                },
                false));

    reg.add({"forge.sprite_preview", "Preview sprite",
             "Render one frame as a PNG attachment (nothing is written), scaled up for pixel art, with optional "
             "onion skin: onion_before previous frames tinted red and onion_after next frames tinted blue under it.",
             {spriteParam(), frameParam(), {"onion_before", ParamType::Int, "previous frames to show", false, {}, std::int64_t{0}},
              {"onion_after", ParamType::Int, "next frames to show", false, {}, std::int64_t{0}},
              {"onion_opacity", ParamType::Int, "ghost opacity 0..255", false, {}, std::int64_t{96}},
              {"scale", ParamType::Int, "enlargement (default: about 256 px)", false}},
             false, true},
            spriteCommand(
                session,
                [](SpriteDoc& d, const Params& p) {
                    const Sprite& sp = d.sprite;
                    std::string error;
                    int f = 0;
                    if (!frameArg(sp, p, f, error)) return Result::failure(error);
                    Image img = onionSkin(sp, f, static_cast<int>(std::clamp<std::int64_t>(intParam(p, "onion_before", 0), 0, 16)),
                                          static_cast<int>(std::clamp<std::int64_t>(intParam(p, "onion_after", 0), 0, 16)),
                                          static_cast<int>(std::clamp<std::int64_t>(intParam(p, "onion_opacity", 96), 0, 255)));
                    const int k = has(p, "scale") ? static_cast<int>(std::clamp<std::int64_t>(intParam(p, "scale"), 1, 32))
                                                  : std::clamp(256 / std::max(1, std::max(sp.width, sp.height)), 1, 32);
                    if (k > 1) img = raster::resize(img, img.width() * k, img.height() * k, raster::Filter::Nearest);
                    Result r = edited("frame " + std::to_string(f) + " (" + std::to_string(sp.durations[static_cast<std::size_t>(f)]) + " ms)", p);
                    r.attach({"image/png", raster::encodePng(img), "frame.png"});
                    return r.with("frame", static_cast<std::int64_t>(f));
                },
                false));

    reg.add({"forge.sprite_close", "Close sprite", "Close an open sprite (unsaved changes are lost).", {spriteParam()}, false},
            [s](const Params& p) -> std::unique_ptr<Action> {
                return std::make_unique<FunctionAction>([s, p] {
                    const std::string id = stringParam(p, "doc");
                    if (!s->workspace().getAs<SpriteDoc>(id)) return Result::failure("no open sprite called '" + id + "'");
                    s->workspace().take(id);
                    return Result::success("closed " + id);
                });
            });
}

void registerAnimFrameCommands(Session& session) {
    auto& reg = session.registry();

    reg.add({"forge.frame_add", "Add frames",
             "Add frames to a sprite: copies of a frame (copy=, default the last one; all layers) or empty ones "
             "(empty=true), inserted at at= (default: the end). Tags that span the insert point grow.",
             {spriteParam(), {"at", ParamType::Int, "position of the new frames (default: after the last)", false},
              {"count", ParamType::Int, "how many", false, {}, std::int64_t{1}},
              {"copy", ParamType::Int, "frame to copy (default: the one before at)", false},
              {"empty", ParamType::Bool, "add empty frames instead of copies", false, {}, false},
              {"duration", ParamType::Int, "ms for the new frames (default: the copied frame's)", false}, previewParam()},
             true},
            spriteCommand(session, [](SpriteDoc& d, const Params& p) {
                Sprite& sp = d.sprite;
                const int at = static_cast<int>(std::clamp<std::int64_t>(intParam(p, "at", sp.frames()), 0, sp.frames()));
                const int count = static_cast<int>(std::clamp<std::int64_t>(intParam(p, "count", 1), 1, 1000));
                const int from = static_cast<int>(intParam(p, "copy", std::max(0, at - 1)));
                if (from < 0 || from >= sp.frames()) return Result::failure("no frame " + std::to_string(from) + " to copy");
                const bool empty = boolParam(p, "empty", false);
                const int dur = static_cast<int>(std::clamp<std::int64_t>(intParam(p, "duration", sp.durations[static_cast<std::size_t>(from)]), 1, 65535));
                for (int k = 0; k < count; ++k) {
                    sp.durations.insert(sp.durations.begin() + at, dur);
                    for (auto& layer : sp.cels) {
                        Image copy = empty ? Image() : layer[static_cast<std::size_t>(from + (from >= at ? k : 0))];
                        layer.insert(layer.begin() + at, std::move(copy));
                    }
                }
                for (auto& t : sp.tags) {
                    if (t.from >= at) t.from += count;
                    if (t.to >= at - 1 && t.to + 1 >= at && t.from < at) t.to += count;
                    else if (t.to >= at) t.to += count;
                }
                Result r = edited("added " + plural(static_cast<std::size_t>(count), "frame") + " at " + std::to_string(at) + " (" +
                                      std::to_string(sp.frames()) + " frames now)",
                                  p);
                return r.with("frames", static_cast<std::int64_t>(sp.frames())).with("at", static_cast<std::int64_t>(at));
            }));

    reg.add({"forge.frame_remove", "Remove frames", "Delete frames (at least one frame stays). Tags shrink to match.",
             {spriteParam(), frameParam("first frame to delete"), {"count", ParamType::Int, "how many", false, {}, std::int64_t{1}}, previewParam()},
             true},
            spriteCommand(session, [](SpriteDoc& d, const Params& p) {
                Sprite& sp = d.sprite;
                std::string error;
                int f = 0;
                if (!frameArg(sp, p, f, error)) return Result::failure(error);
                const int count = static_cast<int>(std::clamp<std::int64_t>(intParam(p, "count", 1), 1, sp.frames() - f));
                if (count >= sp.frames()) return Result::failure("a sprite keeps at least one frame");
                sp.durations.erase(sp.durations.begin() + f, sp.durations.begin() + f + count);
                for (auto& layer : sp.cels) layer.erase(layer.begin() + f, layer.begin() + f + count);
                auto shiftFrom = [&](int x) { return x >= f + count ? x - count : std::min(x, f); };
                auto shiftTo = [&](int x) { return x >= f + count ? x - count : (x >= f ? f - 1 : x); };
                std::vector<SpriteTag> kept;
                for (auto t : sp.tags) {
                    const bool gone = t.from >= f && t.to < f + count;
                    t.from = shiftFrom(t.from);
                    t.to = std::min(shiftTo(t.to), sp.frames() - 1);
                    if (!gone && t.from <= t.to) kept.push_back(t);
                }
                sp.tags = kept;
                return edited("removed " + plural(static_cast<std::size_t>(count), "frame") + " (" + std::to_string(sp.frames()) + " left)", p)
                    .with("frames", static_cast<std::int64_t>(sp.frames()));
            }));

    reg.add({"forge.frame_move", "Move frame", "Move a frame (all its layers and duration) to another position.",
             {spriteParam(), frameParam(), {"to", ParamType::Int, "new position", true}, previewParam()}, true},
            spriteCommand(session, [](SpriteDoc& d, const Params& p) {
                Sprite& sp = d.sprite;
                std::string error;
                int f = 0, to = 0;
                if (!frameArg(sp, p, f, error) || !frameArg(sp, p, to, error, "to")) return Result::failure(error);
                auto move = [&](auto& v) {
                    auto item = std::move(v[static_cast<std::size_t>(f)]);
                    v.erase(v.begin() + f);
                    v.insert(v.begin() + to, std::move(item));
                };
                move(sp.durations);
                for (auto& layer : sp.cels) move(layer);
                return edited("moved frame " + std::to_string(f) + " to " + std::to_string(to), p);
            }));

    reg.add({"forge.frame_duration", "Frame duration", "Set how long frames show, in ms: some frames (frames=), a tag's (tag=) or all.",
             {spriteParam(), {"ms", ParamType::Int, "duration in milliseconds", true},
              {"frames", ParamType::NumberList, "frame numbers (default: all)", false}, {"tag", ParamType::String, "every frame of this tag", false}},
             true},
            spriteCommand(session, [](SpriteDoc& d, const Params& p) {
                Sprite& sp = d.sprite;
                const int ms = static_cast<int>(std::clamp<std::int64_t>(intParam(p, "ms"), 1, 65535));
                std::vector<int> which;
                for (double f : listParam(p, "frames")) which.push_back(static_cast<int>(f));
                if (has(p, "tag")) {
                    const int t = sp.tagIndex(stringParam(p, "tag"));
                    if (t < 0) return Result::failure("no tag called '" + stringParam(p, "tag") + "'");
                    for (int f = sp.tags[static_cast<std::size_t>(t)].from; f <= sp.tags[static_cast<std::size_t>(t)].to; ++f) which.push_back(f);
                }
                if (which.empty()) {
                    for (int f = 0; f < sp.frames(); ++f) which.push_back(f);
                }
                for (int f : which) {
                    if (f < 0 || f >= sp.frames()) return Result::failure("no frame " + std::to_string(f));
                    sp.durations[static_cast<std::size_t>(f)] = ms;
                }
                return edited(plural(which.size(), "frame") + " now last " + std::to_string(ms) + " ms", p);
            }));

    reg.add({"forge.sprite_layer_add", "Add sprite layer",
             "Add a layer to a sprite (on top unless index=), empty or as a copy of another layer (copy=).",
             {spriteParam(), {"name", ParamType::String, "layer name", true}, {"index", ParamType::Int, "position, 0 = bottom", false},
              {"copy", ParamType::String, "layer to duplicate", false}},
             true},
            spriteCommand(session, [](SpriteDoc& d, const Params& p) {
                Sprite& sp = d.sprite;
                const std::string name = stringParam(p, "name");
                if (name.empty()) return Result::failure("give the layer a name");
                if (sp.layerIndex(name) >= 0 && sp.layers[static_cast<std::size_t>(sp.layerIndex(name))].name == name) {
                    return Result::failure("there is already a layer called '" + name + "'");
                }
                const int at = static_cast<int>(std::clamp<std::int64_t>(intParam(p, "index", static_cast<std::int64_t>(sp.layers.size())), 0,
                                                                         static_cast<std::int64_t>(sp.layers.size())));
                SpriteLayer l{name};
                std::vector<Image> cels(static_cast<std::size_t>(sp.frames()));
                if (has(p, "copy")) {
                    const int c = sp.layerIndex(stringParam(p, "copy"));
                    if (c < 0) return Result::failure("no layer called '" + stringParam(p, "copy") + "'");
                    l = sp.layers[static_cast<std::size_t>(c)];
                    l.name = name;
                    cels = sp.cels[static_cast<std::size_t>(c)];
                }
                if (at > 0 && at <= static_cast<int>(sp.layers.size())) l.level = sp.layers[static_cast<std::size_t>(at - 1)].level + (sp.layers[static_cast<std::size_t>(at - 1)].group ? 1 : 0);
                else l.level = 0;
                sp.layers.insert(sp.layers.begin() + at, l);
                sp.cels.insert(sp.cels.begin() + at, std::move(cels));
                return edited("added layer " + name + " at " + std::to_string(at), p).with("layer", name).with("index", static_cast<std::int64_t>(at));
            }));

    reg.add({"forge.sprite_layer_set", "Sprite layer settings", "Rename a layer, show or hide it, change its opacity or blend mode, or move it (index=).",
             {spriteParam(), {"layer", ParamType::String, "layer name or index", true}, {"name", ParamType::String, "new name", false},
              {"visible", ParamType::Bool, "shown", false}, {"opacity", ParamType::Int, "0..255", false},
              {"blend", ParamType::String, "blend mode", false, blendNames()}, {"index", ParamType::Int, "new position, 0 = bottom", false}},
             true},
            spriteCommand(session, [](SpriteDoc& d, const Params& p) {
                Sprite& sp = d.sprite;
                int i = sp.layerIndex(stringParam(p, "layer"));
                if (i < 0) return Result::failure("no layer called '" + stringParam(p, "layer") + "'");
                SpriteLayer& l = sp.layers[static_cast<std::size_t>(i)];
                if (has(p, "name")) l.name = stringParam(p, "name");
                if (has(p, "visible")) l.visible = boolParam(p, "visible");
                if (has(p, "opacity")) l.opacity = static_cast<int>(std::clamp<std::int64_t>(intParam(p, "opacity"), 0, 255));
                if (has(p, "blend")) l.blend = blendNumber(stringParam(p, "blend"));
                if (has(p, "index")) {
                    const int to = static_cast<int>(std::clamp<std::int64_t>(intParam(p, "index"), 0, static_cast<std::int64_t>(sp.layers.size()) - 1));
                    SpriteLayer moved = sp.layers[static_cast<std::size_t>(i)];
                    auto cels = std::move(sp.cels[static_cast<std::size_t>(i)]);
                    sp.layers.erase(sp.layers.begin() + i);
                    sp.cels.erase(sp.cels.begin() + i);
                    sp.layers.insert(sp.layers.begin() + to, moved);
                    sp.cels.insert(sp.cels.begin() + to, std::move(cels));
                    i = to;
                }
                const SpriteLayer& now = sp.layers[static_cast<std::size_t>(i)];
                return edited("layer " + std::to_string(i) + ": " + now.name + (now.visible ? "" : ", hidden") + ", opacity " + std::to_string(now.opacity), p);
            }));

    reg.add({"forge.sprite_layer_remove", "Remove sprite layer", "Delete a layer and its cels (a sprite keeps at least one layer).",
             {spriteParam(), {"layer", ParamType::String, "layer name or index", true}}, true},
            spriteCommand(session, [](SpriteDoc& d, const Params& p) {
                Sprite& sp = d.sprite;
                const int i = sp.layerIndex(stringParam(p, "layer"));
                if (i < 0) return Result::failure("no layer called '" + stringParam(p, "layer") + "'");
                if (sp.layers.size() == 1) return Result::failure("a sprite keeps at least one layer");
                const std::string name = sp.layers[static_cast<std::size_t>(i)].name;
                sp.layers.erase(sp.layers.begin() + i);
                sp.cels.erase(sp.cels.begin() + i);
                return edited("removed layer " + name, p);
            }));

    reg.add({"forge.tag_add", "Add animation tag",
             "Name a range of frames as an animation (idle, run, jump), Aseprite style: direction forward, reverse, "
             "pingpong or pingpong_reverse, repeat count (0 = loop). Re-adding a name changes it.",
             {spriteParam(), {"name", ParamType::String, "tag name", true}, {"from", ParamType::Int, "first frame", true},
              {"to", ParamType::Int, "last frame (inclusive)", true},
              {"direction", ParamType::String, "playback", false, {"forward", "reverse", "pingpong", "pingpong_reverse"}, std::string("forward")},
              {"repeat", ParamType::Int, "times to play (0 = loop forever)", false, {}, std::int64_t{0}},
              {"color", ParamType::String, "tag colour in Aseprite's timeline", false, {}, std::string("#000000")}},
             true},
            spriteCommand(session, [](SpriteDoc& d, const Params& p) {
                Sprite& sp = d.sprite;
                std::string error;
                int a = 0, b = 0;
                if (!frameArg(sp, p, a, error, "from") || !frameArg(sp, p, b, error, "to")) return Result::failure(error);
                if (b < a) return Result::failure("to must not come before from");
                auto c = Color::parse(stringParam(p, "color", "#000000"));
                if (!c) return Result::failure("not a colour: " + stringParam(p, "color"));
                SpriteTag t{stringParam(p, "name"), a, b, stringParam(p, "direction", "forward"),
                            static_cast<int>(std::clamp<std::int64_t>(intParam(p, "repeat", 0), 0, 65535)), *c};
                const int i = sp.tagIndex(t.name);
                if (i >= 0) sp.tags[static_cast<std::size_t>(i)] = t;
                else sp.tags.push_back(t);
                return edited(std::string(i >= 0 ? "changed" : "added") + " tag " + t.name + ": frames " + std::to_string(a) + ".." + std::to_string(b) + ", " + t.direction, p);
            }));

    reg.add({"forge.tag_remove", "Remove animation tag", "Delete an animation tag (the frames stay).",
             {spriteParam(), {"name", ParamType::String, "tag name", true}}, true},
            spriteCommand(session, [](SpriteDoc& d, const Params& p) {
                const int i = d.sprite.tagIndex(stringParam(p, "name"));
                if (i < 0) return Result::failure("no tag called '" + stringParam(p, "name") + "'");
                d.sprite.tags.erase(d.sprite.tags.begin() + i);
                return edited("removed tag " + stringParam(p, "name"), p);
            }));
}

void registerAnimDrawCommands(Session& session) {
    auto& reg = session.registry();
    Session* s = &session;

    reg.add({"forge.draw", "Draw pixels",
             "Draw on one cel (a layer in a frame) with pixel-exact tools: pencil (dots joined by lines: points="
             "x1,y1,x2,y2,... e.g. a mouse drag), line, rect, fill_rect, ellipse, fill_ellipse (two corners), bucket "
             "(fill the same-coloured area at the first point), erase (pencil with transparency), replace (every "
             "pixel of from_color becomes color; all_frames=true for the whole animation), shade (a pencil stroke "
             "that moves each pixel step= entries along a colour ramp, default the palette: 1 lighter/next, -1 "
             "back; each pixel once per stroke, pixels off the ramp stay). color=#rrggbb or index= (palette "
             "entry); indexed sprites snap to their palette.",
             {spriteParam(), frameParam(), layerParam(),
              {"tool", ParamType::String, "what to draw", false,
               {"pencil", "line", "rect", "fill_rect", "ellipse", "fill_ellipse", "bucket", "erase", "replace", "shade"}, std::string("pencil")},
              {"points", ParamType::NumberList, "x,y pairs in px", false}, {"color", ParamType::String, "#rrggbb or #rrggbbaa", false},
              {"index", ParamType::Int, "palette entry instead of color", false},
              {"from_color", ParamType::String, "replace: the colour to change", false},
              {"all_frames", ParamType::Bool, "replace: in every frame of the layer", false, {}, false},
              {"step", ParamType::Int, "shade: entries to move along the ramp (negative goes back)", false, {}, std::int64_t{1}},
              {"ramp", ParamType::String, "shade: comma-separated colours in order (default: the palette)", false}, previewParam()},
             true},
            spriteCommand(session, [](SpriteDoc& d, const Params& p) {
                Sprite& sp = d.sprite;
                std::string error;
                int f = 0, l = 0;
                if (!frameArg(sp, p, f, error) || !layerArg(sp, p, l, error)) return Result::failure(error);
                const std::string tool = stringParam(p, "tool", "pencil");
                Color c{0, 0, 0, 0};
                if (tool != "erase" && tool != "shade" && !colorArg(sp, p, c, error)) return Result::failure(error);
                const NumberList pts = listParam(p, "points");
                if (pts.size() % 2) return Result::failure("points are x,y pairs");
                std::vector<std::pair<int, int>> xy;
                for (std::size_t i = 0; i + 1 < pts.size(); i += 2) xy.push_back({static_cast<int>(std::floor(pts[i])), static_cast<int>(std::floor(pts[i + 1]))});
                const std::size_t need = tool == "replace" ? 0 : (tool == "pencil" || tool == "erase" || tool == "bucket" || tool == "shade") ? 1 : 2;
                if (xy.size() < need) return Result::failure(tool + " needs points=" + (need == 1 ? "x,y" : "x1,y1,x2,y2"));
                int changed = 0;
                auto count = [&](const Image& before, const Image& after) {
                    int n = 0;
                    for (std::size_t i = 0; i < after.bytes(); i += 4) n += std::memcmp(before.data() + i, after.data() + i, 4) != 0;
                    return n;
                };
                if (tool == "replace") {
                    Color from{};
                    if (!colorArg(sp, p, from, error, "from_color")) return Result::failure(error);
                    for (int fr = 0; fr < sp.frames(); ++fr) {
                        if (fr != f && !boolParam(p, "all_frames", false)) continue;
                        if (!sp.celIf(l, fr)) continue;
                        Image& img = sp.cel(l, fr);
                        for (int y = 0; y < img.height(); ++y) {
                            for (int x = 0; x < img.width(); ++x) {
                                if (img.at(x, y) == from) {
                                    img.set(x, y, c);
                                    ++changed;
                                }
                            }
                        }
                    }
                } else if (tool == "shade") {
                    std::vector<Color> ramp;
                    if (p.count("ramp")) {
                        ramp = parseColors(stringParam(p, "ramp"), error);
                        if (ramp.empty()) return Result::failure(error.empty() ? "give ramp=#a,#b,..." : error);
                    } else {
                        for (std::size_t i = 0; i < sp.palette.size(); ++i) {
                            if (!(sp.indexed && static_cast<int>(i) == sp.transparentIndex) && sp.palette[i].a > 0) ramp.push_back(sp.palette[i]);
                        }
                    }
                    if (ramp.size() < 2) return Result::failure("shade needs a ramp of 2+ colours: give ramp= or a palette");
                    const int step = static_cast<int>(intParam(p, "step", 1));
                    Image& img = sp.cel(l, f);
                    // the stroke's pixels, each once: drawn as a mask first
                    Image mask(img.width(), img.height());
                    const Color on{255, 255, 255, 255};
                    if (xy.size() == 1) drawPixelLine(mask, xy[0].first, xy[0].second, xy[0].first, xy[0].second, on);
                    for (std::size_t i = 1; i < xy.size(); ++i) drawPixelLine(mask, xy[i - 1].first, xy[i - 1].second, xy[i].first, xy[i].second, on);
                    for (int y = 0; y < img.height(); ++y) {
                        for (int x = 0; x < img.width(); ++x) {
                            if (mask.at(x, y).a == 0) continue;
                            const Color px = img.at(x, y);
                            if (px.a == 0) continue;
                            for (std::size_t i = 0; i < ramp.size(); ++i) {
                                if (ramp[i].r != px.r || ramp[i].g != px.g || ramp[i].b != px.b) continue;
                                const int j = std::clamp(static_cast<int>(i) + step, 0, static_cast<int>(ramp.size()) - 1);
                                if (j != static_cast<int>(i)) {
                                    img.set(x, y, Color{ramp[static_cast<std::size_t>(j)].r, ramp[static_cast<std::size_t>(j)].g, ramp[static_cast<std::size_t>(j)].b, px.a});
                                    ++changed;
                                }
                                break;
                            }
                        }
                    }
                } else {
                    Image& img = sp.cel(l, f);
                    const Image before = img;
                    if (tool == "pencil" || tool == "erase") {
                        if (xy.size() == 1) drawPixelLine(img, xy[0].first, xy[0].second, xy[0].first, xy[0].second, c);
                        for (std::size_t i = 1; i < xy.size(); ++i) drawPixelLine(img, xy[i - 1].first, xy[i - 1].second, xy[i].first, xy[i].second, c);
                    } else if (tool == "line") {
                        drawPixelLine(img, xy[0].first, xy[0].second, xy[1].first, xy[1].second, c);
                    } else if (tool == "rect" || tool == "fill_rect") {
                        drawPixelRect(img, xy[0].first, xy[0].second, xy[1].first, xy[1].second, c, tool == "fill_rect");
                    } else if (tool == "ellipse" || tool == "fill_ellipse") {
                        drawPixelEllipse(img, xy[0].first, xy[0].second, xy[1].first, xy[1].second, c, tool == "fill_ellipse");
                    } else if (tool == "bucket") {
                        bucketFill(img, xy[0].first, xy[0].second, c);
                    }
                    changed = count(before, img);
                }
                Result r = edited(tool + " on " + sp.layers[static_cast<std::size_t>(l)].name + " frame " + std::to_string(f) + ": " +
                                      plural(static_cast<std::size_t>(changed), "pixel") + " changed",
                                  p);
                return r.with("changed", static_cast<std::int64_t>(changed));
            }));

    reg.add({"forge.cel_paste", "Paste into cel",
             "Put a picture into a cel: an image file or open image (input=/image=) or another cel of the sprite "
             "(from_frame=, from_layer=), at x,y. replace=true clears the cel first; otherwise it's drawn over.",
             {spriteParam(), frameParam(), layerParam(), {"input", ParamType::String, "image file or open image id", false},
              {"from_frame", ParamType::Int, "copy the cel of this frame instead", false},
              {"from_layer", ParamType::String, "...of this layer (default: the target layer)", false},
              {"x", ParamType::Int, "left in px", false, {}, std::int64_t{0}}, {"y", ParamType::Int, "top in px", false, {}, std::int64_t{0}},
              {"replace", ParamType::Bool, "clear the cel first", false, {}, false}, previewParam()},
             true},
            spriteCommand(session, [s](SpriteDoc& d, const Params& p) {
                Sprite& sp = d.sprite;
                std::string error;
                int f = 0, l = 0;
                if (!frameArg(sp, p, f, error) || !layerArg(sp, p, l, error)) return Result::failure(error);
                Image src;
                if (has(p, "input")) {
                    auto img = resolveImage(*s, stringParam(p, "input"), error);
                    if (!img) return Result::failure(error);
                    src = std::move(*img);
                } else if (has(p, "from_frame")) {
                    int ff = 0;
                    if (!frameArg(sp, p, ff, error, "from_frame")) return Result::failure(error);
                    const int fl = has(p, "from_layer") ? sp.layerIndex(stringParam(p, "from_layer")) : l;
                    if (fl < 0) return Result::failure("no layer called '" + stringParam(p, "from_layer") + "'");
                    const Image* c = sp.celIf(fl, ff);
                    src = c ? *c : Image(sp.width, sp.height);
                } else {
                    return Result::failure("give input= (an image) or from_frame= (another cel)");
                }
                Image& dst = sp.cel(l, f);
                if (boolParam(p, "replace", false)) dst.fill({0, 0, 0, 0});
                raster::blendOnto(dst, src, static_cast<int>(intParam(p, "x", 0)), static_cast<int>(intParam(p, "y", 0)), raster::Blend::Normal, 1.0);
                if (sp.indexed) snapToPalette(sp);
                return edited("pasted a " + std::to_string(src.width()) + "x" + std::to_string(src.height()) + " picture into " +
                                  sp.layers[static_cast<std::size_t>(l)].name + " frame " + std::to_string(f),
                              p);
            }));

    reg.add({"forge.cel_clear", "Clear cel", "Empty a cel (one layer in one frame), or the layer in every frame (all_frames=true).",
             {spriteParam(), frameParam(), layerParam(), {"all_frames", ParamType::Bool, "every frame", false, {}, false}}, true},
            spriteCommand(session, [](SpriteDoc& d, const Params& p) {
                Sprite& sp = d.sprite;
                std::string error;
                int f = 0, l = 0;
                if (!frameArg(sp, p, f, error) || !layerArg(sp, p, l, error)) return Result::failure(error);
                auto& cels = sp.cels[static_cast<std::size_t>(l)];
                if (boolParam(p, "all_frames", false)) {
                    for (auto& c : cels) c = Image();
                } else {
                    cels[static_cast<std::size_t>(f)] = Image();
                }
                return edited("cleared " + sp.layers[static_cast<std::size_t>(l)].name + (boolParam(p, "all_frames", false) ? " in every frame" : " frame " + std::to_string(f)), p);
            }));

    reg.add({"forge.sprite_transform", "Transform sprite",
             "Flip, rotate or shift cels: flip_h/flip_v mirror, rotate=90/180/270 turns (90 and 270 turn the whole "
             "sprite and swap its width and height), dx/dy shift pixels (wrap=true wraps them around, handy for "
             "scrolling textures). Applies to every frame and layer unless frame=/layer= narrow it.",
             {spriteParam(), {"frame", ParamType::Int, "only this frame", false}, {"layer", ParamType::String, "only this layer", false},
              {"flip_h", ParamType::Bool, "mirror left-right", false, {}, false}, {"flip_v", ParamType::Bool, "mirror top-bottom", false, {}, false},
              {"rotate", ParamType::Int, "clockwise degrees: 90, 180 or 270", false, {}, std::int64_t{0}},
              {"dx", ParamType::Int, "shift right px", false, {}, std::int64_t{0}}, {"dy", ParamType::Int, "shift down px", false, {}, std::int64_t{0}},
              {"wrap", ParamType::Bool, "pixels shifted off one edge come back on the other", false, {}, false}, previewParam()},
             true},
            spriteCommand(session, [](SpriteDoc& d, const Params& p) {
                Sprite& sp = d.sprite;
                const int rot = static_cast<int>(((intParam(p, "rotate", 0) % 360) + 360) % 360);
                if (rot % 90) return Result::failure("rotate is 90, 180 or 270");
                const bool quarter = rot == 90 || rot == 270;
                if (quarter && (has(p, "frame") || has(p, "layer")) && sp.width != sp.height) {
                    return Result::failure("turning part of a non-square sprite by 90 degrees doesn't fit; turn the whole sprite");
                }
                const int lsel = has(p, "layer") ? sp.layerIndex(stringParam(p, "layer")) : -1;
                if (has(p, "layer") && lsel < 0) return Result::failure("no layer called '" + stringParam(p, "layer") + "'");
                const int fsel = has(p, "frame") ? static_cast<int>(intParam(p, "frame")) : -1;
                if (has(p, "frame") && (fsel < 0 || fsel >= sp.frames())) return Result::failure("no frame " + std::to_string(fsel));
                const int dx = static_cast<int>(intParam(p, "dx", 0)), dy = static_cast<int>(intParam(p, "dy", 0));
                const bool wrap = boolParam(p, "wrap", false);
                int n = 0;
                for (std::size_t li = 0; li < sp.cels.size(); ++li) {
                    if (lsel >= 0 && static_cast<int>(li) != lsel) continue;
                    for (int f = 0; f < sp.frames(); ++f) {
                        if (fsel >= 0 && f != fsel) continue;
                        Image& c = sp.cels[li][static_cast<std::size_t>(f)];
                        if (c.empty()) continue;
                        if (boolParam(p, "flip_h", false)) c = raster::flipHorizontal(c);
                        if (boolParam(p, "flip_v", false)) c = raster::flipVertical(c);
                        if (rot) c = raster::rotate90(c, rot / 90);
                        if (dx || dy) {
                            Image out(c.width(), c.height());
                            for (int y = 0; y < c.height(); ++y) {
                                for (int x = 0; x < c.width(); ++x) {
                                    int tx = x + dx, ty = y + dy;
                                    if (wrap) {
                                        tx = ((tx % c.width()) + c.width()) % c.width();
                                        ty = ((ty % c.height()) + c.height()) % c.height();
                                    }
                                    if (out.contains(tx, ty)) std::copy_n(c.pixel(x, y), 4, out.pixel(tx, ty));
                                }
                            }
                            c = std::move(out);
                        }
                        ++n;
                    }
                }
                if (quarter && lsel < 0 && fsel < 0) std::swap(sp.width, sp.height);
                return edited("transformed " + plural(static_cast<std::size_t>(n), "cel") + " (now " + std::to_string(sp.width) + "x" + std::to_string(sp.height) + ")", p);
            }));

    reg.add({"forge.sprite_resize", "Resize sprite",
             "Change a sprite's size: scale= enlarges every cel by a whole number (crisp pixels), or width/height "
             "change the canvas without scaling (anchor: where the old picture sits).",
             {spriteParam(), {"scale", ParamType::Int, "whole-number enlargement", false},
              {"width", ParamType::Int, "new canvas width", false}, {"height", ParamType::Int, "new canvas height", false},
              {"anchor", ParamType::String, "canvas: where the old picture goes", false,
               {"center", "top_left", "top", "top_right", "left", "right", "bottom_left", "bottom", "bottom_right"}, std::string("center")},
              previewParam()},
             true},
            spriteCommand(session, [](SpriteDoc& d, const Params& p) {
                Sprite& sp = d.sprite;
                if (has(p, "scale")) {
                    const int k = static_cast<int>(intParam(p, "scale"));
                    if (k < 1 || k > 16 || sp.width * k > 8192 || sp.height * k > 8192) return Result::failure("scale must be 1..16 and the result at most 8192 px");
                    for (auto& layer : sp.cels) {
                        for (auto& c : layer) {
                            if (!c.empty()) c = raster::resize(c, c.width() * k, c.height() * k, raster::Filter::Nearest);
                        }
                    }
                    sp.width *= k;
                    sp.height *= k;
                } else if (has(p, "width") || has(p, "height")) {
                    const int w = static_cast<int>(intParam(p, "width", sp.width)), h = static_cast<int>(intParam(p, "height", sp.height));
                    if (w < 1 || h < 1 || w > 8192 || h > 8192) return Result::failure("canvas size must be 1..8192 px");
                    const std::string a = stringParam(p, "anchor", "center");
                    const double ax = a.find("left") != std::string::npos ? 0 : a.find("right") != std::string::npos ? 1 : 0.5;
                    const double ay = a.find("top") != std::string::npos ? 0 : a.find("bottom") != std::string::npos ? 1 : 0.5;
                    for (auto& layer : sp.cels) {
                        for (auto& c : layer) {
                            if (!c.empty()) c = raster::resizeCanvas(c, w, h, ax, ay);
                        }
                    }
                    sp.width = w;
                    sp.height = h;
                } else {
                    return Result::failure("give scale= or width=/height=");
                }
                return edited("sprite is now " + std::to_string(sp.width) + "x" + std::to_string(sp.height), p)
                    .with("width", static_cast<std::int64_t>(sp.width))
                    .with("height", static_cast<std::int64_t>(sp.height));
            }));
}

void registerAnimPaletteCommands(Session& session) {
    auto& reg = session.registry();

    reg.add({"forge.sprite_palette", "Sprite palette",
             "Edit a sprite's palette. action=preset (pico8, db16, db32, gameboy, grayscale4: name=), load (a .gpl/.pal "
             "file: path=), set (colors=#a,#b,...), from_sprite (the colours it uses), quantize (reduce the sprite to "
             "count= colours and use them), set_color (entry index= becomes color=; in an indexed sprite every pixel "
             "of the old colour changes too), swap (a palette swap: colors=#a,#b,... replaces the palette entry for "
             "entry and recolours every pixel of each old entry in every cel, RGBA sprites too, e.g. an enemy's "
             "alternate skin; index= and color= recolours one entry and its pixels; index= and with= trade two "
             "entries and their pixels), add (color=), remove "
             "(index=), save (path= .gpl).",
             {spriteParam(),
              {"action", ParamType::String, "what to do", true, {"preset", "load", "set", "from_sprite", "quantize", "set_color", "swap", "add", "remove", "save"}},
              {"name", ParamType::String, "preset name", false}, {"path", ParamType::String, "palette file", false},
              {"colors", ParamType::String, "comma-separated colours", false}, {"index", ParamType::Int, "palette entry", false},
              {"color", ParamType::String, "a colour", false}, {"count", ParamType::Int, "quantize: number of colours", false, {}, std::int64_t{16}},
              {"with", ParamType::Int, "swap: the entry to trade places with index=", false},
              {"remap", ParamType::Bool, "snap the pixels to the new palette (indexed sprites always do)", false, {}, false}, previewParam()},
             true},
            spriteCommand(session, [](SpriteDoc& d, const Params& p) {
                Sprite& sp = d.sprite;
                const std::string action = stringParam(p, "action");
                std::string error;
                auto keepTransparent = [&](std::vector<Color> pal) {
                    if (sp.indexed) pal.insert(pal.begin(), Color{0, 0, 0, 0});
                    return pal;
                };
                if (action == "preset") {
                    auto pal = presetPalette(stringParam(p, "name"));
                    if (pal.empty()) return Result::failure("presets: pico8, db16, db32, gameboy, grayscale4");
                    sp.palette = keepTransparent(pal);
                } else if (action == "load") {
                    sp.palette = keepTransparent(loadPalette(stringParam(p, "path")));
                } else if (action == "set") {
                    auto pal = parseColors(stringParam(p, "colors"), error);
                    if (pal.empty()) return Result::failure(error.empty() ? "give colors=#a,#b,..." : error);
                    sp.palette = keepTransparent(pal);
                } else if (action == "from_sprite") {
                    std::vector<Color> pal;
                    for (auto c : spriteColors(sp)) {
                        c.a = 255;
                        if (std::find(pal.begin(), pal.end(), c) == pal.end()) pal.push_back(c);
                    }
                    if (pal.size() > 255) return Result::failure("the sprite uses " + std::to_string(pal.size()) + " colours; use action=quantize");
                    sp.palette = keepTransparent(pal);
                } else if (action == "quantize") {
                    const int n = static_cast<int>(std::clamp<std::int64_t>(intParam(p, "count", 16), 2, 255));
                    Image all(std::max(1, sp.width), std::max(1, sp.height * std::max(1, sp.frames() * static_cast<int>(sp.layers.size()))));
                    int row = 0;
                    for (std::size_t l = 0; l < sp.cels.size(); ++l) {
                        for (int f = 0; f < sp.frames(); ++f, ++row) {
                            if (const Image* c = sp.celIf(static_cast<int>(l), f)) blit(all, *c, 0, row * sp.height);
                        }
                    }
                    sp.palette = keepTransparent(buildPalette(all, n, QuantizeMethod::KMeans));
                    snapToPalette(sp);
                } else if (action == "set_color") {
                    const int i = static_cast<int>(intParam(p, "index", -1));
                    if (i < 0 || i >= static_cast<int>(sp.palette.size())) return Result::failure("give index= of an existing entry");
                    auto c = Color::parse(stringParam(p, "color"));
                    if (!c) return Result::failure("give color=#rrggbb");
                    const Color old = sp.palette[static_cast<std::size_t>(i)];
                    sp.palette[static_cast<std::size_t>(i)] = *c;
                    int changed = 0;
                    if (sp.indexed && i != sp.transparentIndex) {
                        for (auto& layer : sp.cels) {
                            for (auto& img : layer) {
                                if (img.empty()) continue;
                                for (int y = 0; y < img.height(); ++y) {
                                    for (int x = 0; x < img.width(); ++x) {
                                        const Color px = img.at(x, y);
                                        if (px.a >= 128 && px.r == old.r && px.g == old.g && px.b == old.b) {
                                            img.set(x, y, Color{c->r, c->g, c->b, 255});
                                            ++changed;
                                        }
                                    }
                                }
                            }
                        }
                    }
                    return edited("palette entry " + std::to_string(i) + " is now " + stringParam(p, "color") +
                                      (changed ? " (" + plural(static_cast<std::size_t>(changed), "pixel") + " recoloured)" : ""),
                                  p)
                        .with("changed", static_cast<std::int64_t>(changed));
                } else if (action == "swap") {
                    std::vector<Color> to = sp.palette;
                    if (p.count("colors")) {
                        auto given = parseColors(stringParam(p, "colors"), error);
                        if (given.empty()) return Result::failure(error.empty() ? "give colors=#a,#b,..." : error);
                        // indexed sprites keep their transparent entry: the list covers the others in order
                        std::size_t k = 0;
                        for (std::size_t i = 0; i < to.size() && k < given.size(); ++i) {
                            if (sp.indexed && static_cast<int>(i) == sp.transparentIndex) continue;
                            to[i] = given[k++];
                        }
                    } else if (p.count("index") && p.count("color")) {
                        const int i = static_cast<int>(intParam(p, "index"));
                        auto c = Color::parse(stringParam(p, "color"));
                        if (i < 0 || i >= static_cast<int>(to.size())) return Result::failure("give index= of an existing entry");
                        if (!c) return Result::failure("give color=#rrggbb");
                        to[static_cast<std::size_t>(i)] = *c;
                    } else if (p.count("index") && p.count("with")) {
                        const int a = static_cast<int>(intParam(p, "index")), b = static_cast<int>(intParam(p, "with"));
                        const int n = static_cast<int>(to.size());
                        if (a < 0 || b < 0 || a >= n || b >= n) return Result::failure("index= and with= must be existing entries");
                        std::swap(to[static_cast<std::size_t>(a)], to[static_cast<std::size_t>(b)]);
                    } else {
                        return Result::failure("give colors=#a,#b,... (the new palette) or index= and with=");
                    }
                    int changed = 0;
                    for (auto& layer : sp.cels) {
                        for (auto& img : layer) {
                            if (img.empty()) continue;
                            for (int y = 0; y < img.height(); ++y) {
                                for (int x = 0; x < img.width(); ++x) {
                                    const Color px = img.at(x, y);
                                    if (px.a == 0) continue;
                                    for (std::size_t i = 0; i < sp.palette.size(); ++i) {
                                        const Color& o = sp.palette[i];
                                        if (sp.indexed && static_cast<int>(i) == sp.transparentIndex) continue;
                                        if (o.r != px.r || o.g != px.g || o.b != px.b) continue;
                                        if (!(to[i] == o)) {
                                            img.set(x, y, Color{to[i].r, to[i].g, to[i].b, px.a});
                                            ++changed;
                                        }
                                        break;
                                    }
                                }
                            }
                        }
                    }
                    sp.palette = to;
                    return edited("palette swapped: " + plural(static_cast<std::size_t>(changed), "pixel") + " recoloured", p)
                        .with("changed", static_cast<std::int64_t>(changed))
                        .with("palette", colorsText(sp.palette));
                } else if (action == "add") {
                    auto c = Color::parse(stringParam(p, "color"));
                    if (!c) return Result::failure("give color=#rrggbb");
                    if (sp.palette.size() >= 256) return Result::failure("the palette is full (256 entries)");
                    sp.palette.push_back(*c);
                } else if (action == "remove") {
                    const int i = static_cast<int>(intParam(p, "index", -1));
                    if (i < 0 || i >= static_cast<int>(sp.palette.size())) return Result::failure("give index= of an existing entry");
                    if (sp.indexed && i == sp.transparentIndex) return Result::failure("that's the transparent entry");
                    sp.palette.erase(sp.palette.begin() + i);
                    if (sp.indexed && i < sp.transparentIndex) --sp.transparentIndex;
                } else if (action == "save") {
                    const std::string path = stringParam(p, "path");
                    if (path.empty()) return Result::failure("give path= (.gpl)");
                    makeParent(path);
                    std::vector<Color> pal;
                    for (std::size_t i = 0; i < sp.palette.size(); ++i) {
                        if (!(sp.indexed && static_cast<int>(i) == sp.transparentIndex)) pal.push_back(sp.palette[i]);
                    }
                    std::ofstream out(path, std::ios::binary);
                    out << paletteGpl(pal, d.path.empty() ? "sprite" : fs::path(d.path).stem().string());
                    if (!out) return Result::failure("can't write " + path);
                    return edited("saved " + plural(pal.size(), "colour") + " to " + path, p);
                }
                if (sp.indexed || boolParam(p, "remap", false)) snapToPalette(sp);
                return edited("palette: " + plural(sp.palette.size(), "colour") + (sp.indexed ? " (indexed)" : ""), p)
                    .with("palette", colorsText(sp.palette))
                    .with("colors", static_cast<std::int64_t>(sp.palette.size()));
            }));

    reg.add({"forge.sprite_color_mode", "Sprite colour mode",
             "Switch a sprite between RGBA and indexed colour. Going indexed snaps every pixel to the palette (making "
             "one from the sprite's colours, quantised to count= when there are too many) and keeps entry 0 as "
             "transparent; going RGBA keeps the pixels and the palette as a swatch list.",
             {spriteParam(), {"mode", ParamType::String, "colour mode", true, {"rgba", "indexed"}},
              {"count", ParamType::Int, "indexed: most colours when a palette has to be made", false, {}, std::int64_t{32}}, previewParam()},
             true},
            spriteCommand(session, [](SpriteDoc& d, const Params& p) {
                Sprite& sp = d.sprite;
                if (stringParam(p, "mode") == "rgba") {
                    sp.indexed = false;
                    return edited("the sprite is RGBA now", p);
                }
                if (sp.indexed) return edited("the sprite is already indexed", p);
                std::vector<Color> pal = sp.palette;
                if (pal.empty()) {
                    for (auto c : spriteColors(sp)) {
                        c.a = 255;
                        if (std::find(pal.begin(), pal.end(), c) == pal.end()) pal.push_back(c);
                    }
                    const int n = static_cast<int>(std::clamp<std::int64_t>(intParam(p, "count", 32), 2, 255));
                    if (static_cast<int>(pal.size()) > n) {
                        Image all(std::max(1, sp.width), std::max(1, sp.height * std::max(1, sp.frames() * static_cast<int>(sp.layers.size()))));
                        int row = 0;
                        for (std::size_t l = 0; l < sp.cels.size(); ++l) {
                            for (int f = 0; f < sp.frames(); ++f, ++row) {
                                if (const Image* c = sp.celIf(static_cast<int>(l), f)) blit(all, *c, 0, row * sp.height);
                            }
                        }
                        pal = buildPalette(all, n, QuantizeMethod::KMeans);
                    }
                    if (pal.empty()) pal = presetPalette("db32");
                }
                pal.erase(std::remove_if(pal.begin(), pal.end(), [](const Color& c) { return c.a < 128; }), pal.end());
                pal.insert(pal.begin(), Color{0, 0, 0, 0});
                if (pal.size() > 256) pal.resize(256);
                sp.palette = pal;
                sp.indexed = true;
                sp.transparentIndex = 0;
                snapToPalette(sp);
                return edited("the sprite is indexed now, " + plural(sp.palette.size() - 1, "colour") + " + transparent", p)
                    .with("palette", colorsText(sp.palette));
            }));
}

}  // namespace wetwaresuite::forge::detail
