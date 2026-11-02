// The modifier stack (model.modifier_*), UVs and materials.
#include <algorithm>
#include <cmath>
#include <filesystem>

#include "cmd.hpp"
#include "wetwaresuite/raster/codec.hpp"

namespace wetwaresuite::model::cmd {

namespace fs = std::filesystem;

namespace {

std::vector<ParamSpec> settingParams() {
    return {{"levels", ParamType::Int, "subsurf: subdivision levels", false},
            {"count", ParamType::Int, "array: copies", false},
            {"relative", ParamType::NumberList, "array: offset as a factor of the size, x,y,z", false},
            {"constant", ParamType::NumberList, "array: offset in units, x,y,z", false},
            {"merge", ParamType::Bool, "mirror/array: weld touching vertices", false},
            {"distance", ParamType::Number, "mirror/array/weld: merge distance", false},
            {"axis", ParamType::String, "mirror: axes, e.g. x, xy, xyz", false},
            {"bisect", ParamType::Bool, "mirror: cut away what's past the mirror plane first", false},
            {"thickness", ParamType::Number, "solidify: thickness", false},
            {"offset", ParamType::Number, "solidify: -1 grows inward, 1 outward, 0 both", false},
            {"rim", ParamType::Bool, "solidify: close the rim", false},
            {"width", ParamType::Number, "bevel: width", false},
            {"segments", ParamType::Int, "bevel: segments", false},
            {"angle", ParamType::Number, "bevel: only edges sharper than this; decimate planar: angle", false},
            {"limit", ParamType::String, "bevel: which edges", false, {"angle", "none"}},
            {"cutter", ParamType::String, "boolean: the other object", false},
            {"operation", ParamType::String, "boolean: operation", false, {"difference", "union", "intersect"}},
            {"hide", ParamType::Bool, "boolean (add): hide the cutter object (default true)", false},
            {"ratio", ParamType::Number, "decimate collapse: share of triangles to keep", false},
            {"decimate", ParamType::String, "decimate: method", false, {"collapse", "planar"}},
            {"factor", ParamType::Number, "smooth: strength per step", false},
            {"repeat", ParamType::Int, "smooth: steps", false},
            {"strength", ParamType::Number, "displace: distance at full value", false},
            {"midlevel", ParamType::Number, "displace: value that doesn't move", false},
            {"scale", ParamType::Number, "displace: noise scale", false},
            {"seed", ParamType::Int, "displace: noise seed", false},
            {"texture", ParamType::String, "displace: height image (through UVs) instead of noise", false},
            {"direction", ParamType::String, "displace: along", false, {"normal", "x", "y", "z"}},
            {"enabled", ParamType::Bool, "turn the modifier on or off", false},
            {"name", ParamType::String, "modifier name", false}};
}

// Copies the given settings into the modifier; fails on settings it doesn't have.
void applySettings(Modifier& m, const Params& p) {
    const Modifier defaults = makeModifier(m.type);
    auto setNum = [&](const std::string& param, const std::string& key, double v) {
        if (!defaults.num.count(key)) throw std::runtime_error(param + " doesn't apply to a " + m.type + " modifier");
        m.num[key] = v;
    };
    auto setText = [&](const std::string& param, const std::string& key, const std::string& v) {
        if (!defaults.text.count(key)) throw std::runtime_error(param + " doesn't apply to a " + m.type + " modifier");
        m.text[key] = v;
    };
    for (const char* k : {"levels", "count", "segments", "repeat", "seed"})
        if (has(p, k)) setNum(k, k, static_cast<double>(intParam(p, k)));
    for (const char* k : {"distance", "thickness", "offset", "width", "angle", "ratio", "factor", "strength", "midlevel", "scale"})
        if (has(p, k)) setNum(k, k, numberParam(p, k));
    for (const char* k : {"merge", "rim", "bisect"})
        if (has(p, k)) setNum(k, k, boolParam(p, k) ? 1 : 0);
    for (const char* k : {"relative", "constant"}) {
        if (!has(p, k)) continue;
        const Vec3 v = vec3(p, k, {0, 0, 0});
        setNum(k, std::string(k) + "_x", v.x);
        setNum(k, std::string(k) + "_y", v.y);
        setNum(k, std::string(k) + "_z", v.z);
    }
    for (const char* k : {"axis", "limit", "operation", "texture", "direction"})
        if (has(p, k)) setText(k, k, stringParam(p, k));
    if (has(p, "cutter")) setText("cutter", "object", stringParam(p, "cutter"));
    if (has(p, "decimate")) setText("decimate", "mode", stringParam(p, "decimate"));
    if (has(p, "enabled")) m.enabled = boolParam(p, "enabled");
    if (has(p, "name")) m.name = stringParam(p, "name");
    if (m.type == "mirror")
        for (char c : m.str("axis", "x"))
            if (c != 'x' && c != 'y' && c != 'z') throw std::runtime_error("axis is made of x, y and z");
}

std::size_t findModifier(const Object& o, const Params& p) {
    if (o.modifiers.empty()) throw std::runtime_error("'" + o.name + "' has no modifiers");
    if (has(p, "modifier")) {
        const std::string n = stringParam(p, "modifier");
        for (std::size_t i = 0; i < o.modifiers.size(); ++i)
            if (o.modifiers[i].name == n) return i;
        throw std::runtime_error("'" + o.name + "' has no modifier called '" + n + "'");
    }
    if (has(p, "index")) {
        const auto i = intParam(p, "index");
        if (i < 0 || i >= static_cast<std::int64_t>(o.modifiers.size())) throw std::runtime_error("index out of range");
        return static_cast<std::size_t>(i);
    }
    return o.modifiers.size() - 1;
}

std::vector<ParamSpec> pick() {
    return {objectParam(),
            {"modifier", ParamType::String, "modifier name (default: the last one)", false},
            {"index", ParamType::Int, "or its position in the stack, from 0", false}};
}

std::vector<ParamSpec> join(std::vector<ParamSpec> a, const std::vector<ParamSpec>& b) {
    a.insert(a.end(), b.begin(), b.end());
    return a;
}

std::string stackLine(const Object& o) {
    std::string s;
    for (const auto& m : o.modifiers) s += (s.empty() ? "" : " > ") + m.name + (m.enabled ? "" : " (off)");
    return o.name + ": " + (s.empty() ? "no modifiers" : s);
}

void checkCutter(ModelDoc& d, const Object& o, const Modifier& m) {
    if (m.type != "boolean") return;
    const std::string c = m.str("object", "");
    if (c.empty()) throw std::runtime_error("a boolean needs cutter=<object>");
    const Object* other = d.object(c);
    if (!other || !d.meshOf(*other)) throw std::runtime_error("no mesh object called '" + c + "'");
    if (other->name == o.name) throw std::runtime_error("an object can't cut itself");
}

double srgb(double c) {
    c = std::clamp(c, 0.0, 1.0);
    return c <= 0.0031308 ? c * 12.92 : 1.055 * std::pow(c, 1 / 2.4) - 0.055;
}

}  // namespace

void registerModifierCommands(Session& session) {
    auto& reg = session.registry();

    std::string types;
    for (const auto& t : modifierTypes()) types += (types.empty() ? "" : ", ") + t;

    reg.add({"model.modifier_add",
             "Add modifier",
             "Add a non-destructive modifier to the end of an object's stack (" + types +
                 "). The viewport, renders and exports show the result; the mesh itself stays editable. Settings: "
                 "mirror axis/merge/distance/bisect; array count/relative/constant/merge; subsurf levels (Catmull-Clark); "
                 "solidify thickness/offset/rim; bevel width/segments/angle/limit; boolean cutter/operation (the cutter is "
                 "hidden unless hide=false); decimate ratio or decimate=planar angle; smooth factor/repeat; displace "
                 "strength/midlevel/scale/seed or texture=; weld distance.",
             withDoc(join({objectParam(), {"type", ParamType::String, "modifier type", true, modifierTypes()}}, settingParams())),
             true},
            edit(session, [](ModelDoc& d, const Params& p) {
                Object& o = target(d, p);
                if (o.type != "mesh") return Result::failure("modifiers go on mesh objects");
                Modifier m = makeModifier(stringParam(p, "type"));
                std::string base = m.name;
                for (int n = 1; std::any_of(o.modifiers.begin(), o.modifiers.end(), [&](const Modifier& x) { return x.name == m.name; }); ++n)
                    m.name = base + "." + std::to_string(n);
                applySettings(m, p);
                checkCutter(d, o, m);
                if (m.type == "boolean" && boolParam(p, "hide", true)) d.object(m.str("object", ""))->visible = false;
                o.modifiers.push_back(m);
                d.refresh();
                Result r = done("added " + m.name + " — " + stackLine(o), d, p);
                for (const auto& e : d.evaluated(o.name).errors) r.message += "\nproblem: " + e;
                return r.with("modifier", m.name);
            }));

    reg.add({"model.modifier_set",
             "Change modifier",
             "Change a modifier's settings (the same ones model.modifier_add takes), rename it, or turn it on or off.",
             withDoc(join(pick(), settingParams())),
             true},
            edit(session, [](ModelDoc& d, const Params& p) {
                Object& o = target(d, p);
                Modifier& m = o.modifiers[findModifier(o, p)];
                applySettings(m, p);
                checkCutter(d, o, m);
                d.refresh();
                Result r = done("updated " + m.name, d, p);
                for (const auto& e : d.evaluated(o.name).errors) r.message += "\nproblem: " + e;
                return r;
            }));

    reg.add({"model.modifier_remove",
             "Remove modifier",
             "Take a modifier off the stack (the mesh is untouched).",
             withDoc(pick()),
             true},
            edit(session, [](ModelDoc& d, const Params& p) {
                Object& o = target(d, p);
                const std::size_t i = findModifier(o, p);
                const std::string name = o.modifiers[i].name;
                o.modifiers.erase(o.modifiers.begin() + static_cast<long>(i));
                return done("removed " + name + " — " + stackLine(o), d, p);
            }));

    reg.add({"model.modifier_move",
             "Reorder modifier",
             "Move a modifier up or down the stack, or to= a position (0 runs first).",
             withDoc(join(pick(), {{"direction", ParamType::String, "one step", false, {"up", "down"}},
                                   {"to", ParamType::Int, "new position", false}})),
             true},
            edit(session, [](ModelDoc& d, const Params& p) {
                Object& o = target(d, p);
                const std::size_t i = findModifier(o, p);
                std::int64_t to = static_cast<std::int64_t>(i);
                if (has(p, "to")) to = intParam(p, "to");
                else if (stringParam(p, "direction") == "up") --to;
                else if (stringParam(p, "direction") == "down") ++to;
                else return Result::failure("give direction=up/down or to=");
                to = std::clamp<std::int64_t>(to, 0, static_cast<std::int64_t>(o.modifiers.size()) - 1);
                Modifier m = o.modifiers[i];
                o.modifiers.erase(o.modifiers.begin() + static_cast<long>(i));
                o.modifiers.insert(o.modifiers.begin() + to, m);
                return done(stackLine(o), d, p);
            }));

    reg.add({"model.modifier_apply",
             "Apply modifier",
             "Make a modifier permanent (Ctrl+A over it): its result becomes the mesh and it leaves the stack. Like "
             "Blender it runs on the plain mesh, so apply from the top of the stack down for the result you see; "
             "all=true applies the whole stack in order.",
             withDoc(join(pick(), {{"all", ParamType::Bool, "apply every modifier", false, {}, false}})),
             true},
            edit(session, [](ModelDoc& d, const Params& p) {
                Object& o = target(d, p);
                if (!d.meshOf(o)) return Result::failure("'" + o.name + "' has no mesh");
                if (boolParam(p, "all", false)) {
                    if (o.modifiers.empty()) return Result::failure("'" + o.name + "' has no modifiers");
                    std::vector<std::string> errors;
                    PolyMesh m = evaluate(d, o, &errors);
                    if (!errors.empty()) return Result::failure(errors.front());
                    singleUser(d, o) = std::move(m);
                    o.modifiers.clear();
                    return done("applied every modifier", d, p);
                }
                const std::size_t i = findModifier(o, p);
                const Modifier mod = o.modifiers[i];
                PolyMesh m = applyModifier(d, o, *d.meshOf(o), mod);
                singleUser(d, o) = std::move(m);
                o.modifiers.erase(o.modifiers.begin() + static_cast<long>(i));
                return done("applied " + mod.name + (i ? " (it wasn't first, so it ran on the plain mesh)" : ""), d, p);
            }));
}

void registerMaterialCommands(Session& session) {
    auto& reg = session.registry();

    reg.add({"model.uv_unwrap",
             "UV unwrap",
             "Lay the mesh out flat for textures (U): smart (project each group of faces along its main direction), "
             "cube, cylinder or sphere projection, planar (along axis), or unwrap (cut along seams from model.mark "
             "type=seam and flattened with least-squares conformal maps). Islands are packed into 0..1. Works on the "
             "selected faces in edit mode (all when none are).",
             withDoc({objectParam(),
                      {"method", ParamType::String, "how", false, {"smart", "cube", "cylinder", "sphere", "planar", "unwrap"}, std::string("smart")},
                      {"angle", ParamType::Number, "smart: angle limit", false, {}, 66.0},
                      {"margin", ParamType::Number, "gap between islands (0..1 of the layout)", false, {}, 0.02},
                      {"axis", ParamType::String, "planar/cylinder/sphere axis", false, {"x", "y", "z"}, std::string("z")},
                      {"scale", ParamType::Number, "cube/planar: UV units per model unit before packing", false, {}, 1.0},
                      {"pack", ParamType::Bool, "pack the islands into 0..1", false, {}, true}}),
             true},
            edit(session, meshEdit([](ModelDoc& d, Object&, PolyMesh& m, const Params& p) {
                UvOptions o;
                o.method = *parseUvMethod(stringParam(p, "method", "smart"));
                o.angle = numberParam(p, "angle", 66);
                o.margin = numberParam(p, "margin", 0.02);
                const std::string axis = stringParam(p, "axis", "z");
                o.axis = axis == "x" ? 0 : axis == "y" ? 1 : 2;
                o.scale = numberParam(p, "scale", 1);
                o.pack = boolParam(p, "pack", true);
                o.selectedOnly = !selectedFaces(m).empty();
                const std::size_t islands = unwrap(m, o);
                return done("unwrapped into " + std::to_string(islands) + " islands", d, p).with("islands", static_cast<std::int64_t>(islands));
            })));

    reg.add({"model.uv_pack",
             "Pack UV islands",
             "Rearrange the existing UV islands to fill 0..1 without overlapping, keeping their relative sizes.",
             withDoc({objectParam(), {"margin", ParamType::Number, "gap between islands", false, {}, 0.02}}),
             true},
            edit(session, meshEdit([](ModelDoc& d, Object&, PolyMesh& m, const Params& p) {
                if (!m.hasUvs()) return Result::failure("no UVs yet: model.uv_unwrap first");
                const std::size_t n = packIslands(m, numberParam(p, "margin", 0.02), !selectedFaces(m).empty());
                return done("packed " + std::to_string(n) + " islands", d, p);
            })));

    reg.add({"model.uv_export",
             "Export UV layout",
             "Save the UV layout as a picture (islands filled, edges drawn) to paint a texture over.",
             withDoc({objectParam(),
                      {"path", ParamType::String, "image file, e.g. layout.png", true},
                      {"size", ParamType::Int, "pixels", false, {}, std::int64_t{1024}},
                      {"selected", ParamType::Bool, "only selected faces", false, {}, false}},
                     false),
             false},
            query(session, [](ModelDoc& d, const Params& p) {
                const Object& o = target(d, p);
                const PolyMesh* m = d.meshOf(o);
                if (!m || !m->hasUvs()) return Result::failure("'" + o.name + "' has no UVs: model.uv_unwrap first");
                const int size = static_cast<int>(std::clamp<std::int64_t>(intParam(p, "size", 1024), 16, 8192));
                raster::save(uvLayout(*m, size, boolParam(p, "selected", false)), stringParam(p, "path"));
                return Result::success("wrote " + stringParam(p, "path")).with("path", stringParam(p, "path"));
            }));

    reg.add({"model.material",
             "Material",
             "Make or change a material (Principled-style: colour, roughness, metallic, emission, glass, alpha, an "
             "image texture through the UVs, or a preset like gold, glass, rubber). assign=true (default) puts it on "
             "the active object, and in edit mode on its selected faces.",
             withDoc({objectParam(),
                      {"name", ParamType::String, "material name", true},
                      {"preset", ParamType::String, "start from a preset", false, render::materialPresets()},
                      {"color", ParamType::String, "base colour, e.g. #cc3322", false},
                      {"roughness", ParamType::Number, "0 mirror .. 1 matte", false},
                      {"metallic", ParamType::Number, "0..1", false},
                      {"emission", ParamType::String, "glow colour", false},
                      {"emission_strength", ParamType::Number, "glow strength", false},
                      {"transmission", ParamType::Number, "1 = glass", false},
                      {"ior", ParamType::Number, "index of refraction", false},
                      {"alpha", ParamType::Number, "opacity", false},
                      {"texture", ParamType::String, "image file for the colour", false},
                      {"texture_scale", ParamType::Number, "texture repeats", false},
                      {"normal_map", ParamType::String, "normal map image (green up) for fine bumps in renders", false},
                      {"normal_strength", ParamType::Number, "normal map strength 0..4 (default 1)", false},
                      {"roughness_map", ParamType::String, "grey image multiplied into roughness in renders", false},
                      {"assign", ParamType::Bool, "put it on the object", false, {}, true}}),
             true},
            edit(session, [](ModelDoc& d, const Params& p) {
                const std::string name = stringParam(p, "name");
                if (name.empty()) return Result::failure("name can't be empty");
                Material* mat = nullptr;
                for (auto& m : d.scene.materials)
                    if (m.name == name) mat = &m;
                if (!mat) {
                    d.scene.materials.push_back({});
                    mat = &d.scene.materials.back();
                    mat->name = name;
                }
                if (has(p, "preset")) {
                    render::Material r;
                    if (!render::applyPreset(r, stringParam(p, "preset"))) return Result::failure("unknown preset");
                    mat->color = {srgb(r.color.x), srgb(r.color.y), srgb(r.color.z)};
                    mat->roughness = r.roughness;
                    mat->metallic = r.metallic;
                    mat->emission = {srgb(r.emission.x), srgb(r.emission.y), srgb(r.emission.z)};
                    mat->emissionStrength = r.emissionStrength;
                    mat->transmission = r.transmission;
                    mat->ior = r.ior;
                    mat->alpha = r.opacity;
                }
                mat->color = color(p, "color", mat->color);
                mat->roughness = std::clamp(numberParam(p, "roughness", mat->roughness), 0.0, 1.0);
                mat->metallic = std::clamp(numberParam(p, "metallic", mat->metallic), 0.0, 1.0);
                mat->emission = color(p, "emission", mat->emission);
                if (has(p, "emission") && !has(p, "emission_strength") && mat->emissionStrength == 0) mat->emissionStrength = 1;
                mat->emissionStrength = std::max(0.0, numberParam(p, "emission_strength", mat->emissionStrength));
                mat->transmission = std::clamp(numberParam(p, "transmission", mat->transmission), 0.0, 1.0);
                mat->ior = numberParam(p, "ior", mat->ior);
                mat->alpha = std::clamp(numberParam(p, "alpha", mat->alpha), 0.0, 1.0);
                if (has(p, "texture")) mat->texture = stringParam(p, "texture");
                mat->textureScale = numberParam(p, "texture_scale", mat->textureScale);
                if (has(p, "normal_map")) mat->normalMap = stringParam(p, "normal_map");
                mat->normalStrength = std::clamp(numberParam(p, "normal_strength", mat->normalStrength), 0.0, 4.0);
                if (has(p, "roughness_map")) mat->roughnessMap = stringParam(p, "roughness_map");
                const Material copy = *mat;
                std::string where;
                if (boolParam(p, "assign", true) && (has(p, "object") || !d.scene.active.empty())) {
                    Object& o = target(d, p);
                    if (o.type == "mesh") {
                        auto it = std::find(o.materials.begin(), o.materials.end(), name);
                        int slot = static_cast<int>(it - o.materials.begin());
                        if (it == o.materials.end()) {
                            o.materials.push_back(name);
                            slot = static_cast<int>(o.materials.size()) - 1;
                        }
                        const PolyMesh* pm = d.meshOf(o);
                        const bool faces = d.scene.editMode && o.name == d.scene.active && pm && !selectedFaces(*pm).empty();
                        PolyMesh& m = d.editMesh(o);
                        for (auto& f : m.faces)
                            if (!faces || f.sel) f.material = slot;
                        where = faces ? " on the selected faces of " + o.name : " on " + o.name;
                    }
                }
                return done("material " + copy.name + " " + hex(copy.color) + where, d, p);
            }));

    reg.add({"model.material_assign",
             "Assign material",
             "Put an existing material on an object (a new slot if needed) and its faces: the selected ones in edit "
             "mode, all of them otherwise. remove=true takes the slot off (its faces fall back to the first slot).",
             withDoc({objectParam(),
                      {"material", ParamType::String, "material name", true},
                      {"remove", ParamType::Bool, "remove the slot instead", false, {}, false}}),
             true},
            edit(session, [](ModelDoc& d, const Params& p) {
                Object& o = target(d, p);
                const std::string name = stringParam(p, "material");
                if (!d.material(name)) return Result::failure("no material called '" + name + "' (model.material makes one)");
                if (o.type != "mesh") return Result::failure("materials go on mesh objects");
                auto it = std::find(o.materials.begin(), o.materials.end(), name);
                if (boolParam(p, "remove", false)) {
                    if (it == o.materials.end()) return Result::failure(o.name + " doesn't use " + name);
                    const int slot = static_cast<int>(it - o.materials.begin());
                    o.materials.erase(it);
                    PolyMesh& m = d.editMesh(o);
                    for (auto& f : m.faces) f.material = f.material == slot ? 0 : f.material > slot ? f.material - 1 : f.material;
                    return done("removed " + name + " from " + o.name, d, p);
                }
                int slot = static_cast<int>(it - o.materials.begin());
                if (it == o.materials.end()) {
                    o.materials.push_back(name);
                    slot = static_cast<int>(o.materials.size()) - 1;
                }
                const PolyMesh* pm = d.meshOf(o);
                const bool faces = d.scene.editMode && o.name == d.scene.active && pm && !selectedFaces(*pm).empty();
                PolyMesh& m = d.editMesh(o);
                std::size_t n = 0;
                for (auto& f : m.faces)
                    if (!faces || f.sel) {
                        f.material = slot;
                        ++n;
                    }
                return done("assigned " + name + " to " + std::to_string(n) + " faces of " + o.name, d, p);
            }));

    reg.add({"model.world",
             "World",
             "Set what lights renders from all around (Blender's World): a gradient sky, one colour, none, or an HDRI "
             "panorama photo (hdri= a 2:1 equirectangular image; rotation turns it). strength scales the light; "
             "visible=true shows it behind the objects in renders.",
             withDoc({{"type", ParamType::String, "kind of world (hdri= implies hdri)", false, {"gradient", "color", "hdri", "none"}},
                      {"hdri", ParamType::String, "panorama image file", false},
                      {"rotation", ParamType::Number, "degrees to turn the hdri", false},
                      {"color", ParamType::String, "sky colour (gradient: straight up; color: everywhere)", false},
                      {"strength", ParamType::Number, "how much light it gives (default 0.5)", false},
                      {"visible", ParamType::Bool, "show it behind objects in renders", false}}),
             true},
            edit(session, [](ModelDoc& d, const Params& p) {
                Scene& s = d.scene;
                if (has(p, "hdri")) {
                    const std::string f = stringParam(p, "hdri");
                    std::error_code ec;
                    if (!f.empty() && !fs::exists(f, ec)) return Result::failure("hdri file not found: " + f);
                    s.hdri = f.empty() ? std::string() : fs::absolute(f, ec).lexically_normal().string();
                    if (!has(p, "type")) s.worldType = f.empty() ? "gradient" : "hdri";
                    if (!has(p, "visible") && !f.empty()) s.worldVisible = true;
                }
                if (has(p, "type")) s.worldType = stringParam(p, "type");
                if (s.worldType == "hdri" && s.hdri.empty()) return Result::failure("type hdri needs hdri= (a panorama image)");
                if (has(p, "rotation")) s.hdriRotation = numberParam(p, "rotation");
                if (has(p, "color")) {
                    s.worldColor = color(p, "color", s.worldColor);
                    if (!has(p, "type") && s.worldType == "hdri") s.worldType = "color";
                }
                if (has(p, "strength")) {
                    if (numberParam(p, "strength") < 0) return Result::failure("strength can't be negative");
                    s.worldStrength = numberParam(p, "strength");
                }
                if (has(p, "visible")) s.worldVisible = boolParam(p, "visible");
                std::string msg = "world: " + s.worldType;
                if (s.worldType == "hdri") msg += " " + fs::path(s.hdri).filename().string();
                msg += ", strength " + num(s.worldStrength);
                return done(msg, d, p);
            }));
}

}  // namespace wetwaresuite::model::cmd
