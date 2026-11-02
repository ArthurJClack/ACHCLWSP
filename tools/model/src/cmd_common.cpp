// Helpers every model.* command file uses, and the render-scene conversion.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <set>
#include <sstream>

#include "cmd.hpp"
#include "wetwaresuite/mesh/render.hpp"
#include "wetwaresuite/raster/codec.hpp"

namespace wetwaresuite::model {

namespace cmd {

ParamSpec docParam() {
    return {"doc", ParamType::String, "id of the open model, e.g. model1 (optional when only one is open)", false};
}

ParamSpec objectParam() { return {"object", ParamType::String, "object name (default: the active object)", false}; }

std::vector<ParamSpec> withDoc(std::vector<ParamSpec> extra, bool preview) {
    std::vector<ParamSpec> out{docParam()};
    out.insert(out.end(), extra.begin(), extra.end());
    if (preview) {
        out.push_back({"preview", ParamType::Bool, "attach a PNG of the viewport (edit-mode overlay included)", false, {}, false});
        out.push_back({"view", ParamType::String, "preview view", false, mesh::viewNames(), std::string("iso")});
        out.push_back({"yaw", ParamType::Number, "preview orbit: degrees around the vertical axis (overrides view)", false});
        out.push_back({"pitch", ParamType::Number, "preview orbit: degrees up from the horizon", false});
        out.push_back({"zoom", ParamType::Number, "preview zoom (1 fits everything)", false, {}, 1.0});
        out.push_back({"shading", ParamType::String, "preview shading, like Blender's viewport", false, mesh::shadingNames(), std::string("material")});
    }
    return out;
}

bool has(const Params& p, const std::string& key) {
    auto it = p.find(key);
    return it != p.end() && !std::holds_alternative<std::monostate>(it->second);
}

std::shared_ptr<ModelDoc> findDoc(Session& s, const Params& p, std::string& id, std::string& error) {
    if (has(p, "doc")) {
        id = stringParam(p, "doc");
        auto d = s.workspace().getAs<ModelDoc>(id);
        if (!d) error = "no open model called '" + id + "' (model.new or model.open makes one)";
        return d;
    }
    std::shared_ptr<ModelDoc> found;
    int count = 0;
    for (const auto& i : s.workspace().ids()) {
        if (auto d = s.workspace().getAs<ModelDoc>(i)) {
            found = d;
            id = i;
            ++count;
        }
    }
    if (count == 1) return found;
    error = count == 0 ? "no model is open (model.new or model.open makes one)" : "several models are open: give doc=<id>";
    return nullptr;
}

namespace {

Params withId(const Params& p, const std::string& id) {
    Params q = p;
    q["doc"] = id;
    return q;
}

}  // namespace

ActionFactory edit(Session& s, EditFn fn) {
    return [&s, fn](const Params& p) -> std::unique_ptr<Action> {
        std::string id, error;
        auto doc = findDoc(s, p, id, error);
        if (!doc) return std::make_unique<FunctionAction>([error] { return Result::failure(error); });
        return std::make_unique<ModelEdit>(doc, [fn, q = withId(p, id)](ModelDoc& d) { return fn(d, q); });
    };
}

ActionFactory query(Session& s, EditFn fn) {
    return [&s, fn](const Params& p) -> std::unique_ptr<Action> {
        std::string id, error;
        auto doc = findDoc(s, p, id, error);
        if (!doc) return std::make_unique<FunctionAction>([error] { return Result::failure(error); });
        return std::make_unique<FunctionAction>([fn, doc, q = withId(p, id)] {
            try {
                return fn(*doc, q);
            } catch (const std::exception& e) {
                return Result::failure(e.what());
            }
        });
    };
}

Object& target(ModelDoc& doc, const Params& p) {
    const std::string name = has(p, "object") ? stringParam(p, "object") : doc.scene.active;
    if (name.empty()) throw std::runtime_error("no active object: give object=<name>");
    Object* o = doc.object(name);
    if (!o) throw std::runtime_error("no object called '" + name + "'");
    return *o;
}

std::vector<Object*> targets(ModelDoc& doc, const Params& p) {
    std::vector<Object*> out;
    if (has(p, "objects")) {
        for (const auto& n : splitList(stringParam(p, "objects"))) {
            Object* o = doc.object(n);
            if (!o) throw std::runtime_error("no object called '" + n + "'");
            out.push_back(o);
        }
        return out;
    }
    if (has(p, "object")) return {&target(doc, p)};
    for (auto& o : doc.scene.objects)
        if (o.selected) out.push_back(&o);
    if (out.empty())
        if (Object* a = doc.activeObject()) out.push_back(a);
    if (out.empty()) throw std::runtime_error("no objects selected: give objects=<names>");
    return out;
}

EditFn meshEdit(MeshFn fn) {
    return [fn](ModelDoc& d, const Params& p) {
        Object& o = target(d, p);
        if (o.type != "mesh") return Result::failure("'" + o.name + "' is a " + o.type + ", not a mesh");
        PolyMesh& m = d.editMesh(o);
        for (auto& other : d.scene.objects) other.selected = other.name == o.name;
        d.scene.active = o.name;
        d.scene.editMode = true;
        return fn(d, o, m, p);
    };
}

OrbitView viewFrom(const Params& p) {
    OrbitView v;
    presetView(stringParam(p, "view", "iso"), v.yaw, v.pitch);
    if (has(p, "yaw")) v.yaw = numberParam(p, "yaw");
    if (has(p, "pitch")) v.pitch = numberParam(p, "pitch");
    if (has(p, "zoom")) v.zoom = numberParam(p, "zoom", 1);
    if (has(p, "shading")) v.shading = stringParam(p, "shading");
    return v;
}

Result& addPreview(Result& r, const ModelDoc& doc, const Params& p) {
    if (!boolParam(p, "preview", false)) return r;
    std::vector<std::uint8_t> rgba;
    const int size = 512;
    if (!doc.renderView(viewFrom(p), size, size, rgba)) return r;
    raster::Image img(size, size);
    std::copy(rgba.begin(), rgba.end(), img.data());
    r.attach({"image/png", raster::encodePng(img), "preview.png"});
    return r;
}

void selectionFields(Result& r, const PolyMesh& m) {
    const auto c = countSelection(m);
    r.with("selected_verts", static_cast<std::int64_t>(c.verts));
    r.with("selected_edges", static_cast<std::int64_t>(c.edges));
    r.with("selected_faces", static_cast<std::int64_t>(c.faces));
}

Result done(const std::string& message, ModelDoc& doc, const Params& p) {
    doc.refresh();
    Result r = Result::success(message);
    r.with("doc", stringParam(p, "doc"));
    if (const Object* a = doc.activeObject()) {
        r.with("object", a->name);
        if (const PolyMesh* m = doc.meshOf(*a)) {
            const Stats st = stats(*m);
            r.with("verts", static_cast<std::int64_t>(st.verts));
            r.with("edges", static_cast<std::int64_t>(st.edges));
            r.with("faces", static_cast<std::int64_t>(st.faces));
            if (doc.scene.editMode) selectionFields(r, *m);
        }
    }
    r.with("mode", doc.scene.editMode ? std::string("edit") : std::string("object"));
    return addPreview(r, doc, p);
}

Vec3 vec3(const Params& p, const std::string& key, const Vec3& fallback) {
    if (!has(p, key)) return fallback;
    const NumberList l = listParam(p, key);
    if (l.size() == 1) return {l[0], l[0], l[0]};
    if (l.size() != 3) throw std::runtime_error(key + " needs three numbers (x,y,z)");
    return {l[0], l[1], l[2]};
}

Vec3 color(const Params& p, const std::string& key, const Vec3& fallback) {
    if (!has(p, key)) return fallback;
    const auto c = raster::Color::parse(stringParam(p, key));
    if (!c) throw std::runtime_error(key + " is a colour like #cc3322 or red");
    return {c->r / 255.0, c->g / 255.0, c->b / 255.0};
}

std::vector<std::string> splitList(const std::string& s) {
    std::vector<std::string> out;
    std::stringstream ss(s);
    std::string item;
    while (std::getline(ss, item, ',')) {
        item.erase(0, item.find_first_not_of(" \t"));
        item.erase(item.find_last_not_of(" \t") + 1);
        if (!item.empty()) out.push_back(item);
    }
    return out;
}

std::string num(double v) {
    char buf[32];
    std::snprintf(buf, sizeof buf, "%.4g", std::fabs(v) < 1e-12 ? 0.0 : v);
    return buf;
}

std::string many(std::size_t n, const std::string& word) {
    if (n != 1 && word == "vertex") return std::to_string(n) + " vertices";
    return std::to_string(n) + " " + word + (n == 1 ? "" : "s");
}

std::string hex(const Vec3& c) {
    char buf[16];
    auto b = [](double v) { return static_cast<int>(std::clamp(v, 0.0, 1.0) * 255 + 0.5); };
    std::snprintf(buf, sizeof buf, "#%02x%02x%02x", b(c.x), b(c.y), b(c.z));
    return buf;
}

void setLocal(Object& o, const Mat4& m) {
    o.location = {m.at(0, 3), m.at(1, 3), m.at(2, 3)};
    Vec3 c0{m.at(0, 0), m.at(1, 0), m.at(2, 0)}, c1{m.at(0, 1), m.at(1, 1), m.at(2, 1)}, c2{m.at(0, 2), m.at(1, 2), m.at(2, 2)};
    Vec3 s{mesh::length(c0), mesh::length(c1), mesh::length(c2)};
    if (m.determinant3() < 0) s.x = -s.x;
    o.scale = s;
    if (s.x != 0) c0 = c0 / s.x;
    if (s.y != 0) c1 = c1 / s.y;
    if (s.z != 0) c2 = c2 / s.z;
    // R = Rz * Ry * Rx; columns c0 c1 c2
    const double r20 = std::clamp(c0.z, -1.0, 1.0);
    const double deg = 180 / mesh::kPi;
    double a, b, c;
    b = std::asin(-r20);
    if (std::fabs(r20) < 0.999999) {
        a = std::atan2(c1.z, c2.z);
        c = std::atan2(c0.y, c0.x);
    } else {
        a = 0;
        c = std::atan2(-c1.x, c1.y);
    }
    auto clean = [](double v) { return std::fabs(v) < 1e-9 ? 0.0 : v; };
    o.rotation = {clean(a * deg), clean(b * deg), clean(c * deg)};
}

void setWorld(ModelDoc& doc, Object& o, const Mat4& world) {
    const Object* p = doc.object(o.parent);
    setLocal(o, p ? doc.world(*p).inverse() * world : world);
}

void purgeMeshes(ModelDoc& doc) {
    std::set<std::string> used;
    for (const auto& o : doc.scene.objects)
        if (o.type == "mesh") used.insert(o.data);
    for (auto it = doc.scene.meshes.begin(); it != doc.scene.meshes.end();)
        it = used.count(it->first) ? std::next(it) : doc.scene.meshes.erase(it);
}

PolyMesh& singleUser(ModelDoc& doc, Object& o) {
    int users = 0;
    for (const auto& other : doc.scene.objects) users += other.type == "mesh" && other.data == o.data ? 1 : 0;
    if (users > 1 && doc.meshOf(o)) {
        const std::string name = doc.uniqueMeshName(o.data);
        doc.scene.meshes[name] = std::make_shared<PolyMesh>(*doc.meshOf(o));
        o.data = name;
    }
    return doc.editMesh(o);
}

// ---- render scene ----

namespace {

double linear(double c) { return c <= 0.04045 ? c / 12.92 : std::pow((c + 0.055) / 1.055, 2.4); }
Vec3 linear(const Vec3& c) { return {linear(c.x), linear(c.y), linear(c.z)}; }

render::Material renderMaterial(const Material* m) {
    render::Material r;
    if (!m) return r;
    r.color = linear(m->color);
    r.roughness = m->roughness;
    r.metallic = m->metallic;
    r.emission = linear(m->emission);
    r.emissionStrength = m->emissionStrength;
    r.transmission = m->transmission;
    r.ior = m->ior;
    r.opacity = m->alpha;
    r.texture = m->texture;
    r.textureScale = m->textureScale;
    r.normalMap = m->normalMap;
    r.normalStrength = m->normalStrength;
    r.roughnessMap = m->roughnessMap;
    return r;
}

}  // namespace

render::Scene toRenderScene(const ModelDoc& doc, const RenderSetup& setup) {
    render::Scene rs = render::defaultScene();
    rs.objects.clear();
    rs.settings.width = setup.width;
    rs.settings.height = setup.height;
    std::vector<render::Light> lights;
    for (const auto& o : doc.scene.objects) {
        if (!doc.shown(o)) continue;
        const Mat4 w = doc.world(o);
        if (o.type == "light") {
            render::Light l;
            l.name = o.name;
            l.type = o.lightType;
            l.position = w.point({0, 0, 0});
            l.target = l.position + mesh::normalize(w.direction({0, 0, -1})) * 10.0;
            l.color = linear(o.lightColor);
            if (o.lightType == "sun") {
                l.intensity = o.power;
                l.radius = std::max(o.size, 0.0);
            } else {
                l.intensity = o.power / (4 * mesh::kPi) * 2;
                l.radius = o.size;
                l.width = l.height = std::max(o.size, 1e-3);
            }
            lights.push_back(l);
            continue;
        }
        if (o.type != "mesh") continue;
        const Evaluated& ev = doc.evaluated(o.name);
        const mesh::Mesh& tm = ev.tris.mesh;
        if (tm.empty()) continue;
        std::map<int, std::vector<std::size_t>> bySlot;
        for (std::size_t t = 0; t < tm.triangleCount(); ++t) bySlot[t < ev.tris.material.size() ? ev.tris.material[t] : 0].push_back(t);
        for (const auto& [slot, tris] : bySlot) {
            auto part = std::make_shared<mesh::Mesh>();
            std::vector<std::int64_t> remap(tm.positions.size(), -1);
            for (std::size_t t : tris) {
                for (int k = 0; k < 3; ++k) {
                    const std::uint32_t v = tm.indices[t * 3 + static_cast<std::size_t>(k)];
                    if (remap[v] < 0) {
                        remap[v] = static_cast<std::int64_t>(part->positions.size());
                        part->positions.push_back(tm.positions[v]);
                        if (tm.hasNormals()) part->normals.push_back(tm.normals[v]);
                        if (tm.hasUvs()) part->uvs.push_back(tm.uvs[v]);
                    }
                    part->indices.push_back(static_cast<std::uint32_t>(remap[v]));
                }
            }
            render::Object ro;
            ro.name = bySlot.size() > 1 ? o.name + "." + std::to_string(slot) : o.name;
            ro.type = "mesh";
            ro.mesh = part;
            ro.smooth = true;
            const Material* mat =
                slot >= 0 && static_cast<std::size_t>(slot) < o.materials.size() ? doc.material(o.materials[static_cast<std::size_t>(slot)]) : nullptr;
            ro.material = renderMaterial(mat);
            rs.objects.push_back(std::move(ro));
        }
    }
    if (!lights.empty()) rs.lights = lights;
    if (setup.ground) render::addGround(rs);
    {
        const Scene& s = doc.scene;
        render::Sky& k = rs.sky;
        k.strength = s.worldStrength;
        if (s.worldType == "hdri" && !s.hdri.empty()) {
            k.type = "hdri";
            k.hdri = s.hdri;
            k.rotation = s.hdriRotation;
        } else if (s.worldType == "color") {
            k.type = "color";
            k.zenith = k.horizon = linear(s.worldColor);
        } else if (s.worldType == "none") {
            k.type = "none";
        } else if (s.worldType == "gradient") {
            k.zenith = linear(s.worldColor);
        }
        if (s.worldVisible) k.visible = true;
    }

    const Object* cam = nullptr;
    if (setup.camera != "view") {
        if (!setup.camera.empty()) {
            cam = doc.object(setup.camera);
            if (!cam || cam->type != "camera") throw std::runtime_error("no camera object called '" + setup.camera + "'");
        } else {
            for (const auto& o : doc.scene.objects)
                if (o.type == "camera" && (!cam || o.name == doc.scene.active)) cam = &o;
        }
    }
    render::Camera c;
    if (cam) {
        const Mat4 w = doc.world(*cam);
        c.position = w.point({0, 0, 0});
        c.target = c.position + mesh::normalize(w.direction({0, 0, -1})) * 10.0;
        c.up = mesh::normalize(w.direction({0, 1, 0}));
        c.fov = cam->fov;
        c.autoFit = false;
    } else {
        const double rad = mesh::kPi / 180;
        const double y = setup.yaw * rad, pt = std::clamp(setup.pitch, -89.0, 89.0) * rad;
        c.target = {0, 0, 0};
        c.position = Vec3{std::cos(pt) * std::sin(y), -std::cos(pt) * std::cos(y), std::sin(pt)} * 10.0;
        render::fitCamera(rs, c);
        c.autoFit = false;
    }
    rs.camera = c;
    return rs;
}

}  // namespace cmd

void registerCommands(Session& session) {
    cmd::registerDocCommands(session);
    cmd::registerObjectCommands(session);
    cmd::registerObjectCommands2(session);
    cmd::registerSelectCommands(session);
    cmd::registerEditCommands(session);
    cmd::registerEditCommands2(session);
    cmd::registerModifierCommands(session);
    cmd::registerMaterialCommands(session);
    cmd::registerRenderCommands(session);
    cmd::registerRigCommands(session);
}

}  // namespace wetwaresuite::model
