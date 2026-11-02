// render.*: scene editing, saving and rendering as commands.
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <filesystem>
#include <set>

#include "wetwaresuite/core/process.hpp"
#include "wetwaresuite/mesh/document.hpp"
#include "wetwaresuite/mesh/io.hpp"
#include "wetwaresuite/raster/codec.hpp"
#include "internal.hpp"

namespace wetwaresuite::render {

namespace fs = std::filesystem;
using detail::hexColor;
using detail::parseColor;

// ---- document ----

std::string SceneDoc::summary() const {
    std::size_t visible = 0;
    for (const auto& o : scene.objects) visible += o.visible ? 1 : 0;
    std::string s = std::to_string(scene.objects.size()) + " objects (" + std::to_string(visible) + " visible), " +
                    std::to_string(scene.lights.size()) + " lights, " + std::to_string(scene.settings.width) + "x" +
                    std::to_string(scene.settings.height) + ", camera " + (scene.camera.autoFit ? "auto" : "set");
    if (!path.empty()) s += ", " + path;
    return s;
}

namespace {

raster::Image quickRender(const Scene& scene, int maxSide, const std::string& mode = "preview") {
    RenderOptions o = optionsFrom(scene);
    const double aspect = static_cast<double>(scene.settings.width) / std::max(scene.settings.height, 1);
    if (aspect >= 1) {
        o.width = maxSide;
        o.height = std::max(8, static_cast<int>(std::lround(maxSide / aspect)));
    } else {
        o.height = maxSide;
        o.width = std::max(8, static_cast<int>(std::lround(maxSide * aspect)));
    }
    o.mode = mode;
    if (mode == "path") o.samples = std::min(o.samples, 8);
    return renderScene(scene, o);
}

}  // namespace

std::unique_ptr<Attachment> SceneDoc::preview() const {
    return std::make_unique<Attachment>(Attachment{"image/png", raster::encodePng(quickRender(scene, 512)), "preview.png"});
}

Result SceneEdit::apply() {
    before_ = doc_->scene;
    Result r;
    try {
        r = fn_(*doc_);
    } catch (const std::exception& e) {
        r = Result::failure(e.what());
    }
    if (!r.ok) revert();
    return r;
}

void SceneEdit::revert() { doc_->scene = before_; }

namespace {

Result failed(const std::string& m) { return Result::failure(m); }

std::string num(double v) {
    char buf[32];
    std::snprintf(buf, sizeof buf, "%.4g", v);
    return buf;
}

std::string vtext(const Vec3& v) { return num(v.x) + "," + num(v.y) + "," + num(v.z); }
NumberList vlist(const Vec3& v) { return {v.x, v.y, v.z}; }

std::string joinList(const std::vector<std::string>& items, const std::string& sep = ", ") {
    std::string out;
    for (const auto& i : items) out += (out.empty() ? "" : sep) + i;
    return out;
}

std::string lowerExt(const fs::path& p) {
    std::string e = p.extension().string();
    if (!e.empty() && e[0] == '.') e.erase(0, 1);
    std::transform(e.begin(), e.end(), e.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return e;
}

void makeParent(const fs::path& file) {
    if (auto parent = file.parent_path(); !parent.empty()) {
        std::error_code ec;
        fs::create_directories(parent, ec);
    }
}

// Reads a 3-vector param: 3 numbers, or 1 number when `uniform` (all three the same).
bool vecArg(const Params& p, const std::string& key, Vec3& out, std::string& err, bool uniform = false) {
    if (!p.count(key)) return false;
    const NumberList l = listParam(p, key);
    if (l.size() == 3) {
        out = {l[0], l[1], l[2]};
        return true;
    }
    if (uniform && l.size() == 1) {
        out = {l[0], l[0], l[0]};
        return true;
    }
    err = key + (uniform ? " takes 1 number or 3 (x,y,z)" : " takes 3 numbers (x,y,z)");
    return false;
}

bool colorArg(const Params& p, const std::string& key, Vec3& out, std::string& err) {
    if (!p.count(key)) return false;
    const std::string text = stringParam(p, key);
    auto c = parseColor(text);
    if (!c) {
        err = "can't read colour '" + text + "': use #rrggbb, a name like red, or linear r,g,b floats";
        return false;
    }
    out = *c;
    return true;
}

ParamSpec docParam() { return {"doc", ParamType::String, "id of an open scene, e.g. scene1", true}; }
ParamSpec docOrFileParam() { return {"doc", ParamType::String, "id of an open scene, e.g. scene1 (or give scene=)", false}; }
ParamSpec sceneFileParam() { return {"scene", ParamType::String, "a .scene.json file to use instead of an open scene", false}; }
ParamSpec previewParam() {
    return {"preview", ParamType::Bool, "attach a quick PNG render (rasterized preview) of the scene", false, {}, false};
}
ParamSpec nameParam(const std::string& what) { return {"name", ParamType::String, what, true}; }

std::vector<ParamSpec> materialParams() {
    return {
        {"preset", ParamType::String, "start from a preset (keeps the texture; matte/plastic/glossy/metal/emissive keep the colour)", false, materialPresets()},
        {"color", ParamType::String, "base colour: #rrggbb, a name (red), or linear r,g,b floats", false},
        {"roughness", ParamType::Number, "0 = mirror-smooth .. 1 = fully rough", false},
        {"metallic", ParamType::Number, "0 = plastic/paint .. 1 = metal", false},
        {"emission", ParamType::String, "glow colour (also give emission_strength)", false},
        {"emission_strength", ParamType::Number, "glow brightness; 0 = off, 1..10 typical", false},
        {"transmission", ParamType::Number, "0 = opaque .. 1 = clear glass", false},
        {"ior", ParamType::Number, "glass index of refraction (1.33 water, 1.5 glass, 2.4 diamond)", false},
        {"opacity", ParamType::Number, "1 = solid .. 0 = invisible (cut-out transparency)", false},
        {"texture", ParamType::String, "image file multiplied into the colour; empty string removes it", false},
        {"texture_scale", ParamType::Number, "how many times the texture repeats across the UVs", false},
        {"normal_map", ParamType::String, "tangent-space normal map image (green up, like Blender/glTF) for fine bumps; empty removes it", false},
        {"normal_strength", ParamType::Number, "how strong the normal map is: 0 flat .. 1 as painted .. 4 (default 1)", false},
        {"roughness_map", ParamType::String, "grey image multiplied into roughness (glTF maps: the green channel); empty removes it", false},
    };
}

// Applies material params; returns an error message or empty.
std::string applyMaterial(Material& m, const Params& p) {
    std::string err;
    if (p.count("preset") && !applyPreset(m, stringParam(p, "preset"))) {
        return "unknown preset '" + stringParam(p, "preset") + "': use one of " + joinList(materialPresets());
    }
    Vec3 c;
    if (colorArg(p, "color", c, err)) {
        m.color = c;
        if (m.emissionStrength > 0 && !p.count("emission") && stringParam(p, "preset") == "emissive") m.emission = c;
    }
    if (!err.empty()) return err;
    if (colorArg(p, "emission", c, err)) {
        m.emission = c;
        if (m.emissionStrength <= 0 && !p.count("emission_strength")) m.emissionStrength = 1;
    }
    if (!err.empty()) return err;
    auto unit = [&](const char* key, double& field, double lo, double hi) {
        if (!p.count(key)) return true;
        const double v = numberParam(p, key);
        if (v < lo || v > hi) {
            err = std::string(key) + " must be in " + num(lo) + ".." + num(hi);
            return false;
        }
        field = v;
        return true;
    };
    if (!unit("roughness", m.roughness, 0, 1) || !unit("metallic", m.metallic, 0, 1) ||
        !unit("emission_strength", m.emissionStrength, 0, 1e6) || !unit("transmission", m.transmission, 0, 1) ||
        !unit("ior", m.ior, 1, 3) || !unit("opacity", m.opacity, 0, 1) || !unit("texture_scale", m.textureScale, 1e-6, 1e6) ||
        !unit("normal_strength", m.normalStrength, 0, 4)) {
        return err;
    }
    for (auto [key, field] : {std::pair<const char*, std::string*>{"texture", &m.texture}, {"normal_map", &m.normalMap}, {"roughness_map", &m.roughnessMap}}) {
        if (!p.count(key)) continue;
        const std::string t = stringParam(p, key);
        if (!t.empty()) {
            std::error_code ec;
            if (!fs::exists(t, ec)) return std::string(key) + " file not found: " + t;
            if (!raster::canRead(t)) return "can't read ." + lowerExt(t) + " images; use one of " + joinList(raster::readableExtensions());
            *field = fs::absolute(t, ec).lexically_normal().string();
        } else {
            field->clear();
        }
    }
    return {};
}

std::string describeMaterial(const Material& m) {
    std::string s = "colour " + hexColor(m.color) + ", roughness " + num(m.roughness);
    if (m.metallic > 0) s += ", metallic " + num(m.metallic);
    if (m.transmission > 0) s += ", glass " + num(m.transmission) + " (ior " + num(m.ior) + ")";
    if (m.opacity < 1) s += ", opacity " + num(m.opacity);
    if (m.emissionStrength > 0) s += ", glows " + hexColor(m.emission) + " x" + num(m.emissionStrength);
    if (!m.texture.empty()) s += ", texture " + fs::path(m.texture).filename().string();
    if (!m.normalMap.empty()) s += ", normal map " + fs::path(m.normalMap).filename().string();
    if (!m.roughnessMap.empty()) s += ", roughness map " + fs::path(m.roughnessMap).filename().string();
    return s;
}

std::string describeObject(const Object& o) {
    std::string s = o.name + ": " + o.type;
    if (o.type == "box") s += " " + vtext(o.size);
    if (o.type == "plane") s += o.size.x > 0 ? " " + num(o.size.x) + "x" + num(o.size.y) : std::string(" (auto size)");
    if (o.type == "sphere") s += " r" + num(o.radius);
    if (o.type == "cylinder" || o.type == "cone") s += " r" + num(o.radius) + " h" + num(o.height);
    if (o.type == "torus") s += " r" + num(o.radius);
    if (o.type == "mesh") s += " (" + std::to_string(o.mesh ? o.mesh->triangleCount() : 0) + " triangles" + (o.path.empty() ? "" : ", " + fs::path(o.path).filename().string()) + ")";
    s += " at " + vtext(o.position);
    if (o.rotation.x != 0 || o.rotation.y != 0 || o.rotation.z != 0) s += " rot " + vtext(o.rotation);
    if (o.scale.x != 1 || o.scale.y != 1 || o.scale.z != 1) s += " scale " + vtext(o.scale);
    if (!o.visible) s += " (hidden)";
    if (o.parts.empty()) {
        s += "; " + describeMaterial(o.material);
    } else {
        s += "; " + std::to_string(o.parts.size()) + " materials:";
        for (std::size_t i = 0; i < o.parts.size(); ++i) s += (i ? ", " : " ") + std::to_string(i + 1) + " " + o.parts[i].name;
    }
    return s;
}

std::string describeLight(const Light& l) {
    std::string s = l.name + ": " + l.type + " light at " + vtext(l.position);
    if (l.type != "point") s += " towards " + vtext(l.target);
    s += ", " + hexColor(l.color) + " x" + num(l.intensity);
    if (l.type == "area") s += ", " + num(l.width) + "x" + num(l.height);
    if (l.type == "point" && l.radius > 0) s += ", radius " + num(l.radius);
    if (l.type == "sun") s += ", " + num(l.radius) + " deg";
    if (!l.shadows) s += ", no shadows";
    return s;
}

Result& maybePreview(Result& r, const SceneDoc& doc, const Params& p) {
    if (!r.ok || !boolParam(p, "preview", false)) return r;
    r.attach({"image/png", raster::encodePng(quickRender(doc.scene, 512)), "preview.png"});
    return r;
}

Result sceneResult(const std::string& message, const SceneDoc& doc, const Params& p) {
    Result r = Result::success(message);
    r.with("doc", stringParam(p, "doc"))
        .with("objects", static_cast<std::int64_t>(doc.scene.objects.size()))
        .with("lights", static_cast<std::int64_t>(doc.scene.lights.size()));
    return maybePreview(r, doc, p);
}

// With allowFile, a command that only reads the scene can take scene=<file.scene.json> instead of doc=.
ActionFactory sceneCommand(Session& session, std::function<Result(SceneDoc&, const Params&)> edit, bool undoable = true,
                           bool allowFile = false) {
    return [&session, edit, undoable, allowFile](const Params& p) -> std::unique_ptr<Action> {
        const std::string id = stringParam(p, "doc");
        auto doc = session.workspace().getAs<SceneDoc>(id);
        if (!doc && allowFile && id.empty()) {
            const std::string file = stringParam(p, "scene");
            return std::make_unique<FunctionAction>([file, edit, p] {
                if (file.empty()) return Result::failure("give doc= (an open scene) or scene= (a .scene.json file)");
                try {
                    SceneDoc temp;
                    temp.scene = loadScene(file);
                    temp.path = file;
                    Params q = p;
                    q["doc"] = file;
                    return edit(temp, q);
                } catch (const std::exception& e) {
                    return Result::failure(e.what());
                }
            });
        }
        if (!doc) {
            return std::make_unique<FunctionAction>([id] {
                return Result::failure("no open scene called '" + id + "' (render.new or render.open makes one)");
            });
        }
        if (!undoable) {
            return std::make_unique<FunctionAction>([doc, edit, p] {
                try {
                    return edit(*doc, p);
                } catch (const std::exception& e) {
                    return Result::failure(e.what());
                }
            });
        }
        return std::make_unique<SceneEdit>(doc, [edit, p](SceneDoc& d) { return edit(d, p); });
    };
}

std::optional<Vec3> viewDirection(const std::string& v) {
    if (v == "iso") return mesh::normalize(Vec3{6, -8, 5});
    if (v == "front") return Vec3{0, -1, 0.0001};
    if (v == "back") return Vec3{0, 1, 0.0001};
    if (v == "left") return Vec3{-1, 0, 0.0001};
    if (v == "right" || v == "side") return Vec3{1, 0, 0.0001};
    if (v == "top") return Vec3{0, -0.0001, 1};
    if (v == "bottom") return Vec3{0, -0.0001, -1};
    if (v == "low") return mesh::normalize(Vec3{6, -8, 1.5});
    return std::nullopt;
}

const std::vector<std::string>& viewNames() {
    static const std::vector<std::string> v = {"iso", "front", "back", "left", "right", "top", "bottom", "low"};
    return v;
}

// Shared by render.render and render.turntable.
std::string renderArgs(const SceneDoc& doc, const Params& p, RenderOptions& o) {
    o = optionsFrom(doc.scene);
    o.mode = stringParam(p, "mode", o.mode);
    if (p.count("width")) o.width = static_cast<int>(intParam(p, "width"));
    if (p.count("height")) {
        o.height = static_cast<int>(intParam(p, "height"));
    } else if (p.count("width")) {
        o.height = std::max(1, static_cast<int>(std::lround(o.width * static_cast<double>(doc.scene.settings.height) / doc.scene.settings.width)));
    }
    if (p.count("samples")) o.samples = static_cast<int>(intParam(p, "samples"));
    if (p.count("bounces")) o.bounces = static_cast<int>(intParam(p, "bounces"));
    o.denoise = boolParam(p, "denoise", o.denoise);
    o.exposure = numberParam(p, "exposure", o.exposure);
    o.shadows = boolParam(p, "shadows", true);
    o.antialias = boolParam(p, "antialias", true);
    o.threads = static_cast<int>(intParam(p, "threads", 0));
    o.seed = static_cast<unsigned>(std::max<std::int64_t>(0, intParam(p, "seed", 1)));
    if (o.width < 8 || o.height < 8 || o.width > 8192 || o.height > 8192) return "width and height must be 8..8192";
    if (o.samples < 1 || o.samples > 100000) return "samples must be 1..100000";
    if (o.bounces < 0 || o.bounces > 64) return "bounces must be 0..64";
    if (o.threads < 0) return "threads can't be negative";
    return {};
}

std::vector<ParamSpec> renderParams() {
    return {
        {"mode", ParamType::String, "path = path tracer (realistic); preview = fast rasterizer", false, {"path", "preview"}},
        {"width", ParamType::Int, "image width in px (default: the scene's, 640)", false},
        {"height", ParamType::Int, "image height in px (default: keeps the scene's aspect)", false},
        {"samples", ParamType::Int, "path tracer samples per pixel (default 16; more = less noise, slower)", false},
        {"bounces", ParamType::Int, "path tracer light bounces (default 4; glass needs 4+)", false},
        {"denoise", ParamType::Bool, "smooth path tracer noise (default true)", false},
        {"exposure", ParamType::Number, "brightness in stops: +1 doubles, -1 halves", false},
        {"shadows", ParamType::Bool, "preview mode: ray-traced shadows", false, {}, true},
        {"antialias", ParamType::Bool, "preview mode: 2x supersampling", false, {}, true},
        {"threads", ParamType::Int, "threads to use; 0 = every core", false, {}, std::int64_t{0}},
        {"seed", ParamType::Int, "path tracer noise seed", false, {}, std::int64_t{1}},
    };
}

std::vector<ParamSpec> concat(std::vector<ParamSpec> a, const std::vector<ParamSpec>& b) {
    a.insert(a.end(), b.begin(), b.end());
    return a;
}

Result& statFields(Result& r, const RenderStats& st, const RenderOptions& o) {
    r.with("width", static_cast<std::int64_t>(o.width))
        .with("height", static_cast<std::int64_t>(o.height))
        .with("mode", o.mode)
        .with("seconds", std::round(st.seconds * 1000) / 1000)
        .with("triangles", static_cast<std::int64_t>(st.triangles))
        .with("threads", static_cast<std::int64_t>(st.threads));
    if (o.mode == "path") r.with("samples", static_cast<std::int64_t>(o.samples)).with("bounces", static_cast<std::int64_t>(o.bounces));
    if (!st.warnings.empty()) r.with("warnings", joinList(st.warnings, "; "));
    return r;
}

std::string renderLabel(const RenderOptions& o) {
    std::string s = std::to_string(o.width) + "x" + std::to_string(o.height) + " " + o.mode;
    if (o.mode == "path") s += ", " + std::to_string(o.samples) + " spp, " + std::to_string(o.bounces) + " bounces";
    return s;
}

bool isImageOut(const fs::path& p) {
    const std::string e = lowerExt(p);
    return e == "png" || e == "jpg" || e == "jpeg" || e == "bmp" || e == "tga" || e == "ppm";
}

}  // namespace

namespace {

void registerSceneCommands(Session& session) {
    auto& reg = session.registry();
    Session* s = &session;

    reg.add({"render.new",
             "New scene",
             "Start a 3D scene for rendering. It comes with a sun, a gradient sky and a ground plane that sizes itself "
             "to whatever you add; the camera frames everything automatically until you set it. Returns its id "
             "(scene1). Next: render.add objects, render.material, render.render.",
             {{"ground", ParamType::Bool, "add a ground plane", false, {}, true},
              {"sun", ParamType::Bool, "add a sun light", false, {}, true},
              {"width", ParamType::Int, "render width in px", false, {}, std::int64_t{640}},
              {"height", ParamType::Int, "render height in px", false, {}, std::int64_t{480}},
              previewParam()},
             false, true},
            [s](const Params& p) {
                return std::make_unique<FunctionAction>([s, p] {
                    auto doc = std::make_shared<SceneDoc>();
                    doc->scene = defaultScene();
                    if (!boolParam(p, "ground", true)) doc->scene.objects.clear();
                    if (!boolParam(p, "sun", true)) doc->scene.lights.clear();
                    const auto w = intParam(p, "width", 640), h = intParam(p, "height", 480);
                    if (w < 8 || h < 8 || w > 8192 || h > 8192) return failed("width and height must be 8..8192");
                    doc->scene.settings.width = static_cast<int>(w);
                    doc->scene.settings.height = static_cast<int>(h);
                    const std::string id = s->workspace().add(doc, "scene");
                    Params q = p;
                    q["doc"] = id;
                    return sceneResult("new scene " + id + ": " + doc->summary(), *doc, q);
                });
            });

    reg.add({"render.open",
             "Open scene",
             "Open a scene saved with render.save (.scene.json). Mesh files and textures it refers to are loaded "
             "(relative paths are relative to the scene file).",
             {{"path", ParamType::String, "scene file", true}, previewParam()},
             false, true},
            [s](const Params& p) {
                return std::make_unique<FunctionAction>([s, p] {
                    const std::string path = stringParam(p, "path");
                    try {
                        auto doc = std::make_shared<SceneDoc>();
                        doc->scene = loadScene(path);
                        doc->path = path;
                        const std::string id = s->workspace().add(doc, "scene");
                        Params q = p;
                        q["doc"] = id;
                        return sceneResult("opened " + path + " as " + id + ": " + doc->summary(), *doc, q);
                    } catch (const std::exception& e) {
                        return failed(e.what());
                    }
                });
            });

    reg.add({"render.save",
             "Save scene",
             "Save the scene as JSON (.scene.json is the convention). Meshes that came from open documents rather "
             "than files are written next to it as <name>.<object>.glb.",
             {docParam(), {"path", ParamType::String, "file to write (default: where it was opened from)", false}},
             false},
            sceneCommand(
                session,
                [](SceneDoc& doc, const Params& p) {
                    const std::string path = stringParam(p, "path", doc.path);
                    if (path.empty()) return failed("give path=, e.g. path=shot.scene.json");
                    saveScene(doc.scene, path);
                    doc.path = path;
                    std::error_code ec;
                    return Result::success("saved " + path)
                        .with("path", path)
                        .with("bytes", static_cast<std::int64_t>(fs::file_size(path, ec)));
                },
                false));

    reg.add({"render.close", "Close scene", "Close an open scene (unsaved changes are lost).", {docParam()}, false},
            [s](const Params& p) {
                return std::make_unique<FunctionAction>([s, id = stringParam(p, "doc")] {
                    if (!s->workspace().getAs<SceneDoc>(id)) return failed("no open scene called '" + id + "'");
                    s->workspace().take(id);
                    return Result::success("closed " + id);
                });
            });

    reg.add({"render.add",
             "Add object",
             "Add an object: a primitive (box, sphere, cylinder, cone, torus, plane) or a mesh from a model file "
             "(path=, any format libs/mesh reads: " + joinList(mesh::readableExtensions()) + ") or an open mesh "
             "document (mesh=mesh1 from forge/cad). Each object's origin is the middle of its base, so the default "
             "position 0,0,0 sits it on the ground. Material params can be given here too.",
             concat({docParam(),
                     {"type", ParamType::String, "what to add (mesh is implied by path= or mesh=)", false, objectTypes()},
                     {"name", ParamType::String, "object name (default: type + number, e.g. sphere1)", false},
                     {"path", ParamType::String, "model file for type mesh", false},
                     {"mesh", ParamType::String, "id of an open mesh document for type mesh", false},
                     {"center", ParamType::Bool, "mesh: move the model so the middle of its base is its origin", false, {}, true},
                     {"size", ParamType::NumberList, "box: x,y,z (default 1,1,1); plane: x,y (omit for an endless-looking ground)", false},
                     {"radius", ParamType::Number, "sphere/cylinder/cone radius, torus ring radius (default 0.5)", false},
                     {"radius2", ParamType::Number, "cone top radius (default 0, a point); torus tube radius (default radius/4)", false},
                     {"height", ParamType::Number, "cylinder/cone height (default 1)", false},
                     {"segments", ParamType::Int, "round primitives: facets around (default 48)", false},
                     {"position", ParamType::NumberList, "x,y,z of the base (default 0,0,0)", false},
                     {"rotation", ParamType::NumberList, "degrees about x,y,z", false},
                     {"scale", ParamType::NumberList, "1 number (uniform) or 3 (x,y,z)", false},
                     {"smooth", ParamType::Bool, "smooth shading (false shows flat facets)", false, {}, true}},
                    concat(materialParams(), {previewParam()})),
             true},
            sceneCommand(session, [s](SceneDoc& doc, const Params& p) {
                Object o;
                o.type = stringParam(p, "type", p.count("path") || p.count("mesh") ? "mesh" : "box");
                if (std::find(objectTypes().begin(), objectTypes().end(), o.type) == objectTypes().end()) {
                    return failed("unknown type '" + o.type + "': use one of " + joinList(objectTypes()));
                }
                if (o.type == "plane") o.size = {4, 4, 0};
                if (o.type == "mesh") {
                    mesh::Mesh m;
                    if (p.count("mesh")) {
                        const std::string id = stringParam(p, "mesh");
                        auto md = s->workspace().getAs<mesh::MeshDoc>(id);
                        if (!md) return failed("no open mesh called '" + id + "'");
                        m = md->mesh;  // the document may differ from its file: save writes this geometry
                    } else if (p.count("path")) {
                        o.path = stringParam(p, "path");
                        if (!mesh::canRead(o.path)) return failed("can't read ." + lowerExt(o.path) + " models; use one of " + joinList(mesh::readableExtensions()));
                        std::vector<int> partOf;
                        m = loadMeshFile(o.path, o.parts, partOf);
                        if (!o.parts.empty()) o.partOf = std::make_shared<const std::vector<int>>(std::move(partOf));
                        std::error_code ec;
                        o.path = fs::absolute(o.path, ec).lexically_normal().string();
                    } else {
                        return failed("type mesh needs path= (a model file) or mesh= (an open mesh id)");
                    }
                    if (m.empty()) return failed("that mesh has no triangles");
                    o.centered = boolParam(p, "center", true);
                    if (o.centered) centerMesh(m);
                    o.mesh = std::make_shared<const mesh::Mesh>(std::move(m));
                }
                std::string err;
                o.name = stringParam(p, "name", "");
                if (o.name.empty()) o.name = doc.scene.uniqueName(o.type);
                if (doc.scene.findObject(o.name) || doc.scene.findLight(o.name)) return failed("there's already something called '" + o.name + "'");
                if (p.count("size")) {
                    const NumberList l = listParam(p, "size");
                    if (o.type == "plane" && l.size() == 2) o.size = {l[0], l[1], 0};
                    else if (l.size() == 1) o.size = {l[0], l[0], l[0]};
                    else if (l.size() == 3) o.size = {l[0], l[1], l[2]};
                    else return failed("size takes 1 number, 3 (x,y,z), or 2 for a plane");
                    if (o.size.x <= 0 || o.size.y <= 0 || (o.type == "box" && o.size.z <= 0)) return failed("size must be positive");
                }
                o.radius = numberParam(p, "radius", o.radius);
                o.radius2 = numberParam(p, "radius2", o.radius2);
                o.height = numberParam(p, "height", o.height);
                o.segments = static_cast<int>(std::clamp<std::int64_t>(intParam(p, "segments", o.segments), 3, 512));
                if (o.radius <= 0 || o.height <= 0 || o.radius2 < 0) return failed("radius and height must be positive");
                if (o.type == "torus" && o.radius2 >= o.radius) return failed("torus radius2 (tube) must be smaller than radius");
                vecArg(p, "position", o.position, err);
                vecArg(p, "rotation", o.rotation, err);
                vecArg(p, "scale", o.scale, err, true);
                if (!err.empty()) return failed(err);
                if (o.scale.x == 0 || o.scale.y == 0 || o.scale.z == 0) return failed("scale can't be 0");
                o.smooth = boolParam(p, "smooth", true);
                err = applyMaterial(o.material, p);
                if (!err.empty()) return failed(err);
                const mesh::Mesh wm = worldMesh(o);
                doc.scene.objects.push_back(o);
                Result r = Result::success("added " + describeObject(o));
                r.with("doc", stringParam(p, "doc")).with("name", o.name).with("triangles", static_cast<std::int64_t>(wm.triangleCount()));
                const mesh::Bounds b = mesh::bounds(wm);
                r.with("size", vlist(b.size()));
                return maybePreview(r, doc, p);
            }));

    reg.add({"render.remove",
             "Remove object",
             "Remove an object or a light by name.",
             {docParam(), nameParam("object or light to remove"), previewParam()},
             true},
            sceneCommand(session, [](SceneDoc& doc, const Params& p) {
                const std::string name = stringParam(p, "name");
                auto& objs = doc.scene.objects;
                auto& lights = doc.scene.lights;
                const auto before = objs.size() + lights.size();
                objs.erase(std::remove_if(objs.begin(), objs.end(), [&](const Object& o) { return o.name == name; }), objs.end());
                lights.erase(std::remove_if(lights.begin(), lights.end(), [&](const Light& l) { return l.name == name; }), lights.end());
                if (objs.size() + lights.size() == before) return failed("nothing called '" + name + "' in the scene");
                return sceneResult("removed " + name, doc, p);
            }));

    reg.add({"render.transform",
             "Transform object",
             "Place an object or light: set position, rotation (degrees about x, y, z) and scale, or nudge with "
             "move (offset) and turn (degrees added to the rotation). Lights take position, move and target. Also "
             "hides or shows objects.",
             {docParam(), nameParam("object or light"),
              {"position", ParamType::NumberList, "x,y,z", false},
              {"rotation", ParamType::NumberList, "degrees about x,y,z", false},
              {"scale", ParamType::NumberList, "1 number (uniform) or 3 (x,y,z)", false},
              {"move", ParamType::NumberList, "offset x,y,z", false},
              {"turn", ParamType::NumberList, "degrees x,y,z added to the rotation", false},
              {"target", ParamType::NumberList, "lights: the point a sun or area light shines towards", false},
              {"visible", ParamType::Bool, "show or hide the object", false},
              previewParam()},
             true},
            sceneCommand(session, [](SceneDoc& doc, const Params& p) {
                const std::string name = stringParam(p, "name");
                std::string err;
                Vec3 v;
                if (Light* l = doc.scene.findLight(name)) {
                    if (vecArg(p, "position", v, err)) l->position = v;
                    if (vecArg(p, "move", v, err)) l->position += v, l->target += v;
                    if (vecArg(p, "target", v, err)) l->target = v;
                    if (!err.empty()) return failed(err);
                    return sceneResult("moved " + describeLight(*l), doc, p);
                }
                Object* o = doc.scene.findObject(name);
                if (!o) return failed("nothing called '" + name + "' in the scene");
                if (vecArg(p, "position", v, err)) o->position = v;
                if (vecArg(p, "rotation", v, err)) o->rotation = v;
                if (vecArg(p, "scale", v, err, true)) {
                    if (v.x == 0 || v.y == 0 || v.z == 0) return failed("scale can't be 0");
                    o->scale = v;
                }
                if (vecArg(p, "move", v, err)) o->position += v;
                if (vecArg(p, "turn", v, err)) o->rotation += v;
                if (!err.empty()) return failed(err);
                if (p.count("visible")) o->visible = boolParam(p, "visible");
                Result r = sceneResult("placed " + describeObject(*o), doc, p);
                return r.with("position", vlist(o->position)).with("rotation", vlist(o->rotation)).with("scale", vlist(o->scale));
            }));

}

void registerLookCommands(Session& session) {
    auto& reg = session.registry();

    reg.add({"render.material",
             "Set material",
             "Change how an object looks: colour, roughness, metallic, glow, glass, transparency and an image "
             "texture, plus normal and roughness maps. Start from a preset (" + joinList(materialPresets()) + ") and "
             "adjust. name=* changes every object. Models loaded from an OBJ with several materials keep each one as a "
             "part: part= changes just that one (by name or number from render.info), otherwise every part changes.",
             concat({docParam(), nameParam("object to change, or * for all"),
                     {"part", ParamType::String, "only this material of a multi-material model (name or 1-based number)", false}},
                    concat(materialParams(), {previewParam()})),
             true},
            sceneCommand(session, [](SceneDoc& doc, const Params& p) {
                const std::string name = stringParam(p, "name");
                std::vector<Object*> targets;
                for (auto& o : doc.scene.objects) {
                    if (name == "*" || o.name == name) targets.push_back(&o);
                }
                if (targets.empty()) return failed("no object called '" + name + "'");
                const std::string part = stringParam(p, "part", "");
                if (!part.empty()) {
                    if (targets.size() != 1 || targets[0]->parts.empty()) return failed("part= needs one object with several materials");
                    auto& parts = targets[0]->parts;
                    Material* m = nullptr;
                    for (auto& pm : parts) {
                        if (pm.name == part) m = &pm;
                    }
                    if (!m && !part.empty() && std::all_of(part.begin(), part.end(), [](char ch) { return ch >= '0' && ch <= '9'; })) {
                        const std::size_t i = static_cast<std::size_t>(std::stoul(part));
                        if (i >= 1 && i <= parts.size()) m = &parts[i - 1];
                    }
                    if (!m) {
                        std::vector<std::string> names;
                        for (const auto& pm : parts) names.push_back(pm.name);
                        return failed("no part '" + part + "': " + joinList(names));
                    }
                    const std::string err = applyMaterial(*m, p);
                    if (!err.empty()) return failed(err);
                    Result r = sceneResult(targets[0]->name + " / " + m->name + ": " + describeMaterial(*m), doc, p);
                    return r.with("color", hexColor(m->color)).with("roughness", m->roughness).with("metallic", m->metallic);
                }
                for (Object* o : targets) {
                    std::string err = applyMaterial(o->material, p);
                    for (auto& pm : o->parts) {
                        if (err.empty()) err = applyMaterial(pm, p);
                    }
                    if (!err.empty()) return failed(err);
                }
                Result r = sceneResult(name == "*" ? "set the material of " + std::to_string(targets.size()) + " objects"
                                                   : targets[0]->name + ": " + describeMaterial(targets[0]->material),
                                       doc, p);
                const Material& m = targets[0]->material;
                return r.with("color", hexColor(m.color)).with("roughness", m.roughness).with("metallic", m.metallic);
            }));

    reg.add({"render.light",
             "Light",
             "Add or change a light. point: a bulb at position (intensity falls off with distance squared; radius "
             "softens shadows). sun: parallel light from position towards target (only the direction matters; "
             "intensity ~4 is daylight; radius is its size in degrees). area: a glowing rectangle width x height at "
             "position facing target, for soft studio light. A name that exists is edited; a new name adds a light.",
             {docParam(),
              {"name", ParamType::String, "light to add or change (default: a new one)", false},
              {"type", ParamType::String, "kind of light (new lights default to point)", false, lightTypes()},
              {"position", ParamType::NumberList, "x,y,z", false},
              {"target", ParamType::NumberList, "x,y,z the light points at (sun, area)", false},
              {"direction", ParamType::NumberList, "sun/area: x,y,z direction the light travels, instead of target", false},
              {"color", ParamType::String, "light colour", false},
              {"intensity", ParamType::Number, "brightness (point/area default 100, sun 4)", false},
              {"radius", ParamType::Number, "point: bulb size; sun: angular size in degrees (soft shadows)", false},
              {"width", ParamType::Number, "area light width", false},
              {"height", ParamType::Number, "area light height", false},
              {"shadows", ParamType::Bool, "cast shadows", false},
              previewParam()},
             true},
            sceneCommand(session, [](SceneDoc& doc, const Params& p) {
                std::string name = stringParam(p, "name", "");
                if (!name.empty() && doc.scene.findObject(name)) return failed("'" + name + "' is an object, not a light");
                Light* l = name.empty() ? nullptr : doc.scene.findLight(name);
                bool added = false;
                if (!l) {
                    Light nl;
                    nl.type = stringParam(p, "type", "point");
                    if (nl.type == "sun") nl.intensity = 4, nl.radius = 2, nl.position = {-5, -2, 6};
                    if (nl.type == "area") nl.position = {0, -4, 4}, nl.width = 2, nl.height = 2;
                    if (nl.type == "point") {
                        // somewhere sensible for the scene's size
                        const mesh::Bounds b = sceneBounds(doc.scene);
                        if (!b.empty()) {
                            const double r = std::max(b.diagonal(), 1e-3);
                            nl.position = b.center() + Vec3{-0.6, -0.8, 1.0} * r;
                            nl.intensity = 100 * (r * r) / 4;
                            nl.radius = r * 0.05;
                        }
                    }
                    if (nl.type == "area") {
                        const mesh::Bounds b = sceneBounds(doc.scene);
                        if (!b.empty()) {
                            const double r = std::max(b.diagonal(), 1e-3);
                            nl.target = b.center();
                            nl.position = b.center() + Vec3{0.0, -0.8, 0.8} * r;
                            nl.width = nl.height = r * 0.6;
                            nl.intensity = 100 * (r * r) / 4;
                        }
                    }
                    nl.name = name.empty() ? doc.scene.uniqueName(nl.type) : name;
                    doc.scene.lights.push_back(nl);
                    l = &doc.scene.lights.back();
                    added = true;
                } else if (p.count("type")) {
                    l->type = stringParam(p, "type");
                }
                std::string err;
                Vec3 v;
                if (vecArg(p, "position", v, err)) l->position = v;
                if (vecArg(p, "target", v, err)) l->target = v;
                if (vecArg(p, "direction", v, err)) {
                    if (mesh::length(v) == 0) return failed("direction can't be 0,0,0");
                    l->target = l->position + mesh::normalize(v);
                }
                if (colorArg(p, "color", v, err)) l->color = v;
                if (!err.empty()) return failed(err);
                if (p.count("intensity")) {
                    if (numberParam(p, "intensity") < 0) return failed("intensity can't be negative");
                    l->intensity = numberParam(p, "intensity");
                }
                if (p.count("radius")) l->radius = std::max(0.0, numberParam(p, "radius"));
                if (p.count("width")) l->width = numberParam(p, "width");
                if (p.count("height")) l->height = numberParam(p, "height");
                if (l->width <= 0 || l->height <= 0) return failed("area light width and height must be positive");
                if (p.count("shadows")) l->shadows = boolParam(p, "shadows");
                if (l->type != "point" && mesh::length(l->target - l->position) == 0) return failed("a " + l->type + " light needs a target away from its position");
                Result r = sceneResult((added ? "added " : "changed ") + describeLight(*l), doc, p);
                return r.with("name", l->name);
            }));

    reg.add({"render.sky",
             "Sky",
             "Set the environment: a gradient sky (zenith, horizon and ground colours), one flat colour, none "
             "(black, only your lights), or an HDRI: hdri= a 2:1 panorama photo that lights the scene and shows "
             "behind it (rotation turns it). The sky lights the scene; visible=false shows `background` behind the "
             "objects instead, and transparent=true gives a PNG with a see-through background.",
             {docParam(),
              {"type", ParamType::String, "kind of sky", false, {"gradient", "color", "none", "hdri"}},
              {"hdri", ParamType::String, "equirectangular panorama image for type hdri (implies it)", false},
              {"rotation", ParamType::Number, "degrees to turn the hdri about the up axis", false},
              {"color", ParamType::String, "one colour for type color (sets zenith and horizon)", false},
              {"zenith", ParamType::String, "colour straight up", false},
              {"horizon", ParamType::String, "colour at the horizon", false},
              {"ground", ParamType::String, "colour below the horizon", false},
              {"strength", ParamType::Number, "how much light the sky gives (default 1; 0 = none)", false},
              {"visible", ParamType::Bool, "camera sees the sky (false: sees background)", false},
              {"background", ParamType::String, "colour behind objects when the sky is hidden", false},
              {"transparent", ParamType::Bool, "transparent background in PNG renders", false},
              previewParam()},
             true},
            sceneCommand(session, [](SceneDoc& doc, const Params& p) {
                Sky& k = doc.scene.sky;
                std::string err;
                Vec3 v;
                if (p.count("type")) k.type = stringParam(p, "type");
                if (p.count("hdri")) {
                    const std::string f = stringParam(p, "hdri");
                    std::error_code ec;
                    if (!fs::exists(f, ec)) return failed("hdri file not found: " + f);
                    if (!raster::canRead(f)) return failed("can't read ." + lowerExt(f) + " images; use one of " + joinList(raster::readableExtensions()));
                    k.hdri = fs::absolute(f, ec).lexically_normal().string();
                    if (!p.count("type")) k.type = "hdri";
                    if (!p.count("visible") && !p.count("background")) k.visible = true;
                }
                if (p.count("rotation")) k.rotation = numberParam(p, "rotation");
                if (k.type == "hdri" && k.hdri.empty()) return failed("type hdri needs hdri= (a panorama image)");
                if (colorArg(p, "color", v, err)) {
                    k.zenith = k.horizon = v;
                    if (!p.count("type")) k.type = "color";
                }
                if (colorArg(p, "zenith", v, err)) k.zenith = v;
                if (colorArg(p, "horizon", v, err)) k.horizon = v;
                if (colorArg(p, "ground", v, err)) k.ground = v;
                if (colorArg(p, "background", v, err)) {
                    k.background = v;
                    if (!p.count("visible")) k.visible = false;
                }
                if (!err.empty()) return failed(err);
                if (p.count("strength")) {
                    if (numberParam(p, "strength") < 0) return failed("strength can't be negative");
                    k.strength = numberParam(p, "strength");
                }
                if (p.count("visible")) k.visible = boolParam(p, "visible");
                if (p.count("transparent")) k.transparent = boolParam(p, "transparent");
                std::string msg = "sky: " + k.type;
                if (k.type == "gradient") msg += " " + hexColor(k.zenith) + " to " + hexColor(k.horizon);
                if (k.type == "color") msg += " " + hexColor(k.zenith);
                if (k.type == "hdri") msg += " " + fs::path(k.hdri).filename().string() + (k.rotation != 0 ? " turned " + num(k.rotation) + " deg" : "");
                msg += ", strength " + num(k.strength);
                if (!k.visible) msg += ", background " + hexColor(k.background);
                if (k.transparent) msg += ", transparent";
                return sceneResult(msg, doc, p);
            }));

    reg.add({"render.camera",
             "Camera",
             "Aim the camera: position and target (the point it looks at), field of view, or orthographic. "
             "view=iso/front/back/left/right/top/bottom/low with fit=true frames everything from that side. Until "
             "you set a position the camera frames the scene automatically at render time (auto=true brings that "
             "back).",
             {docParam(),
              {"position", ParamType::NumberList, "x,y,z of the camera", false},
              {"target", ParamType::NumberList, "x,y,z it looks at", false},
              {"up", ParamType::NumberList, "which way is up (default 0,0,1)", false},
              {"fov", ParamType::Number, "vertical field of view in degrees (default 40; small = telephoto)", false},
              {"orthographic", ParamType::Bool, "parallel projection, no perspective", false},
              {"ortho_size", ParamType::Number, "orthographic: visible height in model units", false},
              {"view", ParamType::String, "look from this side", false, viewNames()},
              {"fit", ParamType::Bool, "frame every object now (keeps the direction)", false, {}, false},
              {"auto", ParamType::Bool, "frame every object automatically at render time", false},
              {"orbit", ParamType::Number, "turn the camera this many degrees around the target (about z)", false},
              {"width", ParamType::Int, "render width in px", false},
              {"height", ParamType::Int, "render height in px", false},
              previewParam()},
             true},
            sceneCommand(session, [](SceneDoc& doc, const Params& p) {
                Camera& c = doc.scene.camera;
                std::string err;
                Vec3 v;
                bool placed = false;
                if (p.count("width")) doc.scene.settings.width = static_cast<int>(intParam(p, "width"));
                if (p.count("height")) doc.scene.settings.height = static_cast<int>(intParam(p, "height"));
                if (doc.scene.settings.width < 8 || doc.scene.settings.height < 8 || doc.scene.settings.width > 8192 ||
                    doc.scene.settings.height > 8192) {
                    return failed("width and height must be 8..8192");
                }
                if (p.count("fov")) {
                    const double f = numberParam(p, "fov");
                    if (f < 1 || f > 170) return failed("fov must be 1..170 degrees");
                    c.fov = f;
                }
                if (p.count("orthographic")) c.orthographic = boolParam(p, "orthographic");
                if (p.count("ortho_size")) {
                    if (numberParam(p, "ortho_size") <= 0) return failed("ortho_size must be positive");
                    c.orthoSize = numberParam(p, "ortho_size");
                }
                if (c.autoFit && !p.count("view") && (p.count("position") || p.count("target") || p.count("orbit"))) {
                    fitCamera(doc.scene, c);  // start from what the auto camera showed
                }
                if (p.count("view")) {
                    auto dir = viewDirection(stringParam(p, "view"));
                    if (!dir) return failed("unknown view");
                    const double dist = std::max(mesh::length(c.position - c.target), 1e-6);
                    c.position = c.target + *dir * dist;
                    c.up = {0, 0, 1};
                    if (!p.count("fit") && !p.count("position")) {
                        fitCamera(doc.scene, c);
                    }
                    placed = true;
                }
                if (vecArg(p, "position", v, err)) c.position = v, placed = true;
                if (vecArg(p, "target", v, err)) c.target = v, placed = true;
                if (vecArg(p, "up", v, err)) c.up = v;
                if (!err.empty()) return failed(err);
                if (p.count("orbit")) {
                    const Vec3 off = mesh::Mat4::rotateZ(numberParam(p, "orbit")).direction(c.position - c.target);
                    c.position = c.target + off;
                    placed = true;
                }
                if (boolParam(p, "fit", false)) {
                    fitCamera(doc.scene, c);
                    placed = true;
                }
                if (placed) c.autoFit = false;
                if (p.count("auto")) c.autoFit = boolParam(p, "auto");
                if (mesh::length(c.position - c.target) == 0) return failed("the camera's position and target can't be the same point");
                if (mesh::length(mesh::cross(c.target - c.position, c.up)) < 1e-9) c.up = {0, 1, 0};
                std::string msg = c.autoFit ? std::string("camera: automatic framing")
                                            : "camera at " + vtext(c.position) + " looking at " + vtext(c.target);
                msg += c.orthographic ? ", orthographic " + num(c.orthoSize) : ", fov " + num(c.fov);
                Result r = sceneResult(msg, doc, p);
                return r.with("position", vlist(c.position)).with("target", vlist(c.target)).with("auto", c.autoFit);
            }));

    reg.add({"render.settings",
             "Render settings",
             "Defaults saved with the scene for render.render: size, samples per pixel, bounces, denoise, exposure "
             "and mode.",
             {docParam(),
              {"width", ParamType::Int, "image width in px", false},
              {"height", ParamType::Int, "image height in px", false},
              {"samples", ParamType::Int, "path tracer samples per pixel", false},
              {"bounces", ParamType::Int, "path tracer max bounces", false},
              {"denoise", ParamType::Bool, "smooth path tracer noise", false},
              {"exposure", ParamType::Number, "brightness in stops", false},
              {"mode", ParamType::String, "default render mode", false, {"path", "preview"}},
              previewParam()},
             true},
            sceneCommand(session, [](SceneDoc& doc, const Params& p) {
                Settings& st = doc.scene.settings;
                if (p.count("width")) st.width = static_cast<int>(intParam(p, "width"));
                if (p.count("height")) st.height = static_cast<int>(intParam(p, "height"));
                if (p.count("samples")) st.samples = static_cast<int>(intParam(p, "samples"));
                if (p.count("bounces")) st.bounces = static_cast<int>(intParam(p, "bounces"));
                if (p.count("denoise")) st.denoise = boolParam(p, "denoise");
                if (p.count("exposure")) st.exposure = numberParam(p, "exposure");
                if (p.count("mode")) st.mode = stringParam(p, "mode");
                if (st.width < 8 || st.height < 8 || st.width > 8192 || st.height > 8192) return failed("width and height must be 8..8192");
                if (st.samples < 1 || st.samples > 100000) return failed("samples must be 1..100000");
                if (st.bounces < 0 || st.bounces > 64) return failed("bounces must be 0..64");
                Result r = sceneResult("settings: " + std::to_string(st.width) + "x" + std::to_string(st.height) + ", " + st.mode + ", " +
                                           std::to_string(st.samples) + " spp, " + std::to_string(st.bounces) + " bounces, denoise " +
                                           (st.denoise ? "on" : "off") + ", exposure " + num(st.exposure),
                                       doc, p);
                return r;
            }));

}

void registerOutputCommands(Session& session) {
    auto& reg = session.registry();

    reg.add({"render.list",
             "List scene",
             "List every object (type, size, place, material) and light in the scene.",
             {docOrFileParam(), sceneFileParam()},
             false, true},
            sceneCommand(
                session,
                [](SceneDoc& doc, const Params& p) {
                    std::string text;
                    std::vector<std::string> objs, lights;
                    for (const auto& o : doc.scene.objects) {
                        text += describeObject(o) + "\n";
                        objs.push_back(o.name);
                    }
                    for (const auto& l : doc.scene.lights) {
                        text += describeLight(l) + "\n";
                        lights.push_back(l.name);
                    }
                    if (text.empty()) text = "the scene is empty\n";
                    text.pop_back();
                    Result r = Result::success(text);
                    return r.with("doc", stringParam(p, "doc")).with("objects", joinList(objs, ",")).with("lights", joinList(lights, ","));
                },
                false, true));

    reg.add({"render.info",
             "Scene info",
             "Summarize the scene: counts, triangles, bounds, camera, sky and render settings.",
             {docOrFileParam(), sceneFileParam()},
             false, true},
            sceneCommand(
                session,
                [](SceneDoc& doc, const Params& p) {
                    const Scene& sc = doc.scene;
                    std::size_t tris = 0;
                    for (const auto& o : sc.objects) {
                        if (o.visible) tris += worldMesh(o).triangleCount();
                    }
                    const mesh::Bounds b = sceneBounds(sc);
                    Camera cam = sc.camera;
                    if (cam.autoFit) fitCamera(sc, cam);
                    std::string msg = doc.summary() + "\n" + std::to_string(tris) + " triangles";
                    if (!b.empty()) msg += ", bounds " + vtext(b.min) + " to " + vtext(b.max);
                    msg += "\ncamera at " + vtext(cam.position) + " looking at " + vtext(cam.target) +
                           (cam.orthographic ? ", orthographic" : ", fov " + num(cam.fov)) + (sc.camera.autoFit ? " (auto)" : "");
                    msg += "\nsky " + sc.sky.type + ", strength " + num(sc.sky.strength);
                    msg += "\nrender " + sc.settings.mode + ", " + std::to_string(sc.settings.samples) + " spp, " +
                           std::to_string(sc.settings.bounces) + " bounces";
                    Result r = Result::success(msg);
                    r.with("doc", stringParam(p, "doc"))
                        .with("objects", static_cast<std::int64_t>(sc.objects.size()))
                        .with("lights", static_cast<std::int64_t>(sc.lights.size()))
                        .with("triangles", static_cast<std::int64_t>(tris))
                        .with("size", vlist(b.empty() ? Vec3{} : b.size()))
                        .with("camera_position", vlist(cam.position))
                        .with("camera_target", vlist(cam.target))
                        .with("camera_auto", sc.camera.autoFit)
                        .with("width", static_cast<std::int64_t>(sc.settings.width))
                        .with("height", static_cast<std::int64_t>(sc.settings.height))
                        .with("samples", static_cast<std::int64_t>(sc.settings.samples))
                        .with("bounces", static_cast<std::int64_t>(sc.settings.bounces))
                        .with("mode", sc.settings.mode);
                    return r;
                },
                false, true));

    reg.add({"render.preview",
             "Preview render",
             "Look at the scene: a quick render returned as a PNG (nothing is written). mode=preview (default) is "
             "the instant rasterizer; mode=path gives a small path-traced look.",
             {docOrFileParam(), sceneFileParam(),
              {"mode", ParamType::String, "how to draw it", false, {"preview", "path"}, std::string("preview")},
              {"size", ParamType::Int, "longest side in px", false, {}, std::int64_t{512}},
              {"samples", ParamType::Int, "path mode samples per pixel", false, {}, std::int64_t{8}}},
             false, true},
            sceneCommand(
                session,
                [](SceneDoc& doc, const Params& p) {
                    const int size = static_cast<int>(std::clamp<std::int64_t>(intParam(p, "size", 512), 16, 2048));
                    RenderOptions o = optionsFrom(doc.scene);
                    const double aspect = static_cast<double>(doc.scene.settings.width) / std::max(doc.scene.settings.height, 1);
                    o.width = aspect >= 1 ? size : std::max(8, static_cast<int>(std::lround(size * aspect)));
                    o.height = aspect >= 1 ? std::max(8, static_cast<int>(std::lround(size / aspect))) : size;
                    o.mode = stringParam(p, "mode", "preview");
                    o.samples = static_cast<int>(std::clamp<std::int64_t>(intParam(p, "samples", 8), 1, 4096));
                    RenderStats st;
                    raster::Image img = renderScene(doc.scene, o, &st);
                    const std::string id = stringParam(p, "doc");
                    Result r = Result::success("preview of " + id + " (" + renderLabel(o) + ", " + num(st.seconds) + "s)");
                    r.with("doc", id);
                    statFields(r, st, o);
                    r.attach({"image/png", raster::encodePng(img), id + ".png"});
                    return r;
                },
                false, true));

    reg.add({"render.render",
             "Render",
             "Render the scene to an image file (png, jpg, bmp, tga). mode=path is the path tracer (soft shadows, "
             "reflections, glass, glow; the defaults take a couple of seconds); mode=preview is the instant "
             "rasterizer. Without output= the PNG comes back attached; preview=true attaches it as well.",
             concat({docOrFileParam(), sceneFileParam(),
                     {"output", ParamType::String, "image file to write, e.g. shot.png or shot.jpg", false},
                     {"quality", ParamType::Int, "JPEG quality 1..100", false, {}, std::int64_t{92}},
                     previewParam()},
                    renderParams()),
             false},
            sceneCommand(
                session,
                [](SceneDoc& doc, const Params& p) {
                    RenderOptions o;
                    const std::string err = renderArgs(doc, p, o);
                    if (!err.empty()) return failed(err);
                    const std::string out = stringParam(p, "output", "");
                    if (!out.empty() && !isImageOut(out)) return failed("output must be .png, .jpg, .bmp, .tga or .ppm");
                    RenderStats st;
                    raster::Image img = renderScene(doc.scene, o, &st);
                    const std::string id = stringParam(p, "doc");
                    Result r = Result::success("rendered " + id + " (" + renderLabel(o) + ") in " + num(st.seconds) + "s" +
                                               (out.empty() ? std::string() : " to " + out));
                    r.with("doc", id);
                    if (!out.empty()) {
                        makeParent(out);
                        raster::save(img, out, static_cast<int>(std::clamp<std::int64_t>(intParam(p, "quality", 92), 1, 100)));
                        std::error_code ec;
                        r.with("path", out).with("bytes", static_cast<std::int64_t>(fs::file_size(out, ec)));
                    }
                    statFields(r, st, o);
                    if (out.empty() || boolParam(p, "preview", false)) r.attach({"image/png", raster::encodePng(img), id + ".png"});
                    return r;
                },
                false, true));

    reg.add({"render.turntable",
             "Turntable",
             "Render frames with the camera circling the scene (about the vertical axis through its target) into a "
             "folder as frame_000.png ..., and optionally make a video (.mp4, .webm, .gif) with ffmpeg if it's "
             "installed. Defaults to the fast preview mode; mode=path for path-traced frames (slower: about one "
             "render.render per frame).",
             concat({docOrFileParam(), sceneFileParam(),
                     {"output_dir", ParamType::String, "folder for the frames", true},
                     {"frames", ParamType::Int, "number of frames", false, {}, std::int64_t{36}},
                     {"degrees", ParamType::Number, "how far to turn in total", false, {}, 360.0},
                     {"format", ParamType::String, "frame image format", false, {"png", "jpg"}, std::string("png")},
                     {"video", ParamType::String, "also encode a video file (.mp4, .webm, .mov, .gif); needs ffmpeg", false},
                     {"fps", ParamType::Number, "video frames per second", false, {}, 24.0},
                     previewParam()},
                    renderParams()),
             false},
            sceneCommand(
                session,
                [](SceneDoc& doc, const Params& p) {
                    RenderOptions o;
                    Params q = p;
                    if (!q.count("mode")) q["mode"] = std::string("preview");
                    const std::string err = renderArgs(doc, q, o);
                    if (!err.empty()) return failed(err);
                    const auto frames = intParam(p, "frames", 36);
                    if (frames < 1 || frames > 3600) return failed("frames must be 1..3600");
                    const double degrees = numberParam(p, "degrees", 360);
                    const double fps = numberParam(p, "fps", 24);
                    if (fps <= 0 || fps > 240) return failed("fps must be in (0, 240]");
                    const std::string fmt = stringParam(p, "format", "png");
                    const std::string video = stringParam(p, "video", "");
                    const std::string vext = lowerExt(video);
                    if (!video.empty() && vext != "mp4" && vext != "webm" && vext != "mov" && vext != "mkv" && vext != "gif") {
                        return failed("video must be .mp4, .webm, .mov, .mkv or .gif");
                    }
                    std::optional<fs::path> ffmpeg;
                    if (!video.empty()) {
                        ffmpeg = findProgram("ffmpeg");
                        if (!ffmpeg) {
                            return failed("making a video needs ffmpeg, which isn't installed (or isn't on PATH); leave out video= to "
                                          "get just the frames. Install: Debian/Ubuntu `sudo apt install ffmpeg`, macOS `brew install "
                                          "ffmpeg`, Windows `winget install ffmpeg`.");
                        }
                    }
                    if (video.empty() || vext != "gif") {
                        // H.264 needs even sizes
                        if (!video.empty()) o.width += o.width % 2, o.height += o.height % 2;
                    }
                    const fs::path dir = stringParam(p, "output_dir");
                    std::error_code ec;
                    fs::create_directories(dir, ec);
                    if (!fs::is_directory(dir, ec)) return failed("can't make the folder " + dir.string());

                    Camera base = doc.scene.camera;
                    if (base.autoFit) {
                        Scene tmp;
                        tmp.objects = doc.scene.objects;
                        tmp.settings.width = o.width;
                        tmp.settings.height = o.height;
                        fitCamera(tmp, base);
                    }
                    double total = 0;
                    raster::Image first;
                    std::size_t tris = 0;
                    std::vector<std::string> warnings;
                    for (std::int64_t f = 0; f < frames; ++f) {
                        Camera c = base;
                        const double angle = degrees * static_cast<double>(f) / static_cast<double>(frames);
                        c.position = c.target + mesh::Mat4::rotateZ(angle).direction(base.position - base.target);
                        RenderOptions fo = o;
                        fo.camera = c;
                        fo.seed = o.seed + static_cast<unsigned>(f);
                        RenderStats st;
                        raster::Image img = renderScene(doc.scene, fo, &st);
                        total += st.seconds;
                        tris = st.triangles;
                        warnings = st.warnings;
                        char name[32];
                        std::snprintf(name, sizeof name, "frame_%03d.%s", static_cast<int>(f), fmt.c_str());
                        raster::save(img, dir / name, 92);
                        if (f == 0) first = img;
                    }
                    const std::string id = stringParam(p, "doc");
                    Result r = Result::success("rendered " + std::to_string(frames) + " frames (" + renderLabel(o) + ") to " +
                                               dir.string() + " in " + num(total) + "s");
                    r.with("doc", id).with("output_dir", dir.string()).with("frames", static_cast<std::int64_t>(frames));
                    if (!video.empty()) {
                        makeParent(video);
                        const std::string pattern = (dir / ("frame_%03d." + fmt)).string();
                        const std::string rate = num(fps);
                        std::vector<std::string> args = {ffmpeg->string(), "-hide_banner", "-nostdin", "-loglevel", "error", "-y",
                                                         "-framerate", rate, "-i", pattern};
                        if (vext == "gif") {
                            args.insert(args.end(), {"-vf", "split[a][b];[a]palettegen=stats_mode=diff[p];[b][p]paletteuse=dither=sierra2_4a", "-loop", "0"});
                        } else if (vext == "webm") {
                            args.insert(args.end(), {"-c:v", "libvpx-vp9", "-b:v", "0", "-crf", "32", "-pix_fmt", "yuv420p"});
                        } else {
                            args.insert(args.end(), {"-c:v", "libx264", "-pix_fmt", "yuv420p", "-crf", "20", "-movflags", "+faststart"});
                        }
                        args.push_back(video);
                        ProcessResult pr = runProgram(args);
                        if (pr.exitCode != 0) {
                            std::string outText = pr.output;
                            if (outText.size() > 1500) outText = "..." + outText.substr(outText.size() - 1500);
                            return failed("the frames are in " + dir.string() + " but ffmpeg couldn't make the video (exit " +
                                          std::to_string(pr.exitCode) + "): " + outText);
                        }
                        r.message += ", video " + video;
                        r.with("video", video).with("bytes", static_cast<std::int64_t>(fs::file_size(video, ec)));
                    }
                    RenderStats st;
                    st.seconds = total;
                    st.triangles = tris;
                    st.threads = detail::threadCount(o.threads);
                    st.warnings = warnings;
                    statFields(r, st, o);
                    if (boolParam(p, "preview", false) && !first.empty()) r.attach({"image/png", raster::encodePng(first), id + "_frame0.png"});
                    return r;
                },
                false, true));
}


void registerModelCommand(Session& session) {
    session.registry().add(
        {"render.model",
         "Render a model",
         "One step from a model file to a picture: puts the model (" + joinList(mesh::readableExtensions()) +
             ") on a ground plane under a sun and sky, gives it a material, frames it from `view` and renders. "
             "For more control build a scene with render.new and render.add.",
         concat({{"path", ParamType::String, "model file", true},
                 {"output", ParamType::String, "image to write (png, jpg, ...); omit to get the PNG back", false},
                 {"preset", ParamType::String, "material preset", false, materialPresets(), std::string("clay")},
                 {"color", ParamType::String, "base colour", false},
                 {"view", ParamType::String, "look from this side", false, viewNames(), std::string("iso")},
                 {"ground", ParamType::Bool, "stand it on a ground plane", false, {}, true},
                 {"y_up", ParamType::Bool, "the file is Y-up (many OBJ/STL game exports): stand it upright", false, {}, false},
                 {"quality", ParamType::Int, "JPEG quality 1..100", false, {}, std::int64_t{92}},
                 previewParam()},
                renderParams()),
         false},
        [](const Params& p) {
            return std::make_unique<FunctionAction>([p] {
                try {
                    const std::string path = stringParam(p, "path");
                    if (!mesh::canRead(path)) return failed("can't read ." + lowerExt(path) + " models; use one of " + joinList(mesh::readableExtensions()));
                    mesh::Mesh m = mesh::load(path);
                    if (m.empty()) return failed("that model has no triangles");
                    if (boolParam(p, "y_up", false)) mesh::transform(m, mesh::Mat4::rotateX(90));
                    centerMesh(m);
                    SceneDoc doc;
                    doc.scene = defaultScene();
                    if (!boolParam(p, "ground", true)) doc.scene.objects.clear();
                    Object o;
                    o.name = "model";
                    o.type = "mesh";
                    o.mesh = std::make_shared<const mesh::Mesh>(std::move(m));
                    Params mp;
                    mp["preset"] = stringParam(p, "preset", "clay");
                    if (p.count("color")) mp["color"] = p.at("color");
                    const std::string err = applyMaterial(o.material, mp);
                    if (!err.empty()) return failed(err);
                    doc.scene.objects.push_back(o);
                    if (p.count("width") && p.count("height") && intParam(p, "height") > 0) {
                        doc.scene.settings.width = static_cast<int>(std::clamp<std::int64_t>(intParam(p, "width"), 8, 8192));
                        doc.scene.settings.height = static_cast<int>(std::clamp<std::int64_t>(intParam(p, "height"), 8, 8192));
                    }
                    auto dir = viewDirection(stringParam(p, "view", "iso"));
                    Camera& c = doc.scene.camera;
                    c.position = c.target + *dir;
                    fitCamera(doc.scene, c);
                    c.autoFit = false;
                    RenderOptions o2;
                    const std::string rerr = renderArgs(doc, p, o2);
                    if (!rerr.empty()) return failed(rerr);
                    const std::string out = stringParam(p, "output", "");
                    if (!out.empty() && !isImageOut(out)) return failed("output must be .png, .jpg, .bmp, .tga or .ppm");
                    RenderStats st;
                    raster::Image img = renderScene(doc.scene, o2, &st);
                    Result r = Result::success("rendered " + path + " (" + renderLabel(o2) + ") in " + num(st.seconds) + "s" +
                                               (out.empty() ? std::string() : " to " + out));
                    if (!out.empty()) {
                        makeParent(out);
                        raster::save(img, out, static_cast<int>(std::clamp<std::int64_t>(intParam(p, "quality", 92), 1, 100)));
                        std::error_code ec;
                        r.with("path", out).with("bytes", static_cast<std::int64_t>(fs::file_size(out, ec)));
                    }
                    statFields(r, st, o2);
                    if (out.empty() || boolParam(p, "preview", false)) r.attach({"image/png", raster::encodePng(img), "model.png"});
                    return r;
                } catch (const std::exception& e) {
                    return failed(e.what());
                }
            });
        });
}

}  // namespace

void registerCommands(Session& session) {
    registerSceneCommands(session);
    registerLookCommands(session);
    registerOutputCommands(session);
    registerModelCommand(session);
}

}  // namespace wetwaresuite::render
