// The model document: objects, evaluation (with a cache), the viewport
// drawing with Blender's edit-mode overlay, animation channels and undo.
#include <algorithm>
#include <cstring>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <stdexcept>

#include "wetwaresuite/model/model.hpp"
#include "wetwaresuite/raster/codec.hpp"

namespace wetwaresuite::model {

using mesh::cross;
using mesh::dot;
using mesh::length;
using mesh::normalize;

Mat4 Object::local() const { return Mat4::translate(location) * Mat4::rotate(rotation) * Mat4::scale(scale); }

const std::vector<std::string>& channelNames() {
    static const std::vector<std::string> names{"location.x", "location.y", "location.z", "rotation.x", "rotation.y",
                                                "rotation.z", "scale.x",    "scale.y",    "scale.z"};
    return names;
}

// ---- lookups ----

Object* ModelDoc::object(const std::string& name) {
    for (auto& o : scene.objects)
        if (o.name == name) return &o;
    return nullptr;
}

const Object* ModelDoc::object(const std::string& name) const {
    for (const auto& o : scene.objects)
        if (o.name == name) return &o;
    return nullptr;
}

const Material* ModelDoc::material(const std::string& name) const {
    for (const auto& m : scene.materials)
        if (m.name == name) return &m;
    return nullptr;
}

Mat4 ModelDoc::world(const Object& o) const {
    Mat4 w = o.local();
    const Object* p = object(o.parent);
    for (int guard = 0; p && guard < 64; ++guard) {
        w = p->local() * w;
        p = object(p->parent);
    }
    return w;
}

bool ModelDoc::shown(const Object& o) const {
    if (!o.visible) return false;
    for (const auto& c : scene.collections)
        if (c.name == o.collection) return c.visible;
    return true;
}

const PolyMesh* ModelDoc::meshOf(const Object& o) const {
    if (o.type != "mesh") return nullptr;
    auto it = scene.meshes.find(o.data);
    return it == scene.meshes.end() || !it->second ? nullptr : it->second.get();
}

PolyMesh& ModelDoc::editMesh(Object& o) {
    if (o.type != "mesh") throw std::runtime_error("'" + o.name + "' is a " + o.type + ", not a mesh");
    auto it = scene.meshes.find(o.data);
    if (o.data.empty() || it == scene.meshes.end() || !it->second) {
        if (o.data.empty()) o.data = uniqueMeshName(o.name);
        auto fresh = std::make_shared<PolyMesh>();
        scene.meshes[o.data] = fresh;
        return *fresh;
    }
    // Meshes are always created non-const, so a sole owner may write in place;
    // otherwise (an undo snapshot or the cache holds it too) copy first.
    if (it->second.use_count() == 1) return const_cast<PolyMesh&>(*it->second);
    auto copy = std::make_shared<PolyMesh>(*it->second);
    it->second = copy;
    return *copy;
}

namespace {

std::string numbered(const std::string& base, int n) {
    char buf[16];
    std::snprintf(buf, sizeof buf, ".%03d", n);
    return base + buf;
}

std::string stripNumber(const std::string& name) {
    // "Cube.001" -> "Cube", so copies of copies count on from the original
    const auto dot = name.rfind('.');
    if (dot != std::string::npos && dot + 4 == name.size() && std::all_of(name.begin() + static_cast<long>(dot) + 1, name.end(), ::isdigit))
        return name.substr(0, dot);
    return name;
}

}  // namespace

std::string ModelDoc::uniqueName(const std::string& wanted) const {
    const std::string base = wanted.empty() ? "Object" : wanted;
    if (!object(base)) return base;
    const std::string stem = stripNumber(base);
    for (int n = 1;; ++n)
        if (!object(numbered(stem, n))) return numbered(stem, n);
}

std::string ModelDoc::uniqueMeshName(const std::string& wanted) const {
    const std::string base = wanted.empty() ? "Mesh" : wanted;
    if (!scene.meshes.count(base)) return base;
    const std::string stem = stripNumber(base);
    for (int n = 1;; ++n)
        if (!scene.meshes.count(numbered(stem, n))) return numbered(stem, n);
}

// ---- evaluation ----

namespace {

void stampMatrix(std::string& s, const Mat4& m) {
    char buf[40];
    for (double v : m.m) {
        std::snprintf(buf, sizeof buf, "%.17g,", v);
        s += buf;
    }
}

// Everything evaluation depends on, so the cache knows when to redo it.
void stampObject(const ModelDoc& doc, const Object& o, std::string& s, std::vector<std::shared_ptr<const PolyMesh>>& inputs,
                 int depth) {
    char buf[64];
    auto it = doc.scene.meshes.find(o.data);
    if (it != doc.scene.meshes.end()) {
        inputs.push_back(it->second);
        std::snprintf(buf, sizeof buf, "%p|", static_cast<const void*>(it->second.get()));
        s += buf;
    }
    std::snprintf(buf, sizeof buf, "%.17g|", o.autoSmooth);
    s += buf;
    if (!o.weights.empty()) {  // vertex groups: a cheap fingerprint of every weight
        std::uint64_t h = 1469598103934665603ull;
        for (const auto& [bone, w] : o.weights) {
            for (char c : bone) h = (h ^ static_cast<unsigned char>(c)) * 1099511628211ull;
            for (float x : w) {
                std::uint32_t bits;
                std::memcpy(&bits, &x, 4);
                h = (h ^ bits) * 1099511628211ull;
            }
        }
        std::snprintf(buf, sizeof buf, "w%llx|", static_cast<unsigned long long>(h));
        s += buf;
    }
    for (const auto& m : o.modifiers) {
        if (m.type == "armature" && m.enabled) {  // the pose moves the mesh
            if (const Object* a = doc.object(m.str("object", ""))) {
                s += "arm(";
                stampMatrix(s, doc.world(*a));
                stampMatrix(s, doc.world(o));
                for (const auto& b : a->bones) {
                    s += b.name + ":" + b.parent + ":";
                    for (double v : {b.head.x, b.head.y, b.head.z, b.tail.x, b.tail.y, b.tail.z, b.roll, b.rotation.x, b.rotation.y,
                                     b.rotation.z, b.location.x, b.location.y, b.location.z}) {
                        std::snprintf(buf, sizeof buf, "%.17g,", v);
                        s += buf;
                    }
                }
                s += ")";
            }
        }
        s += m.type + (m.enabled ? "+" : "-") + m.name + "{";
        for (const auto& [k, v] : m.num) {
            std::snprintf(buf, sizeof buf, "%.17g", v);
            s += k + "=" + buf + ",";
        }
        for (const auto& [k, v] : m.text) s += k + "=" + v + ",";
        s += "}";
        if (m.type == "boolean" && m.enabled && depth < 4) {
            if (const Object* other = doc.object(m.str("object", "")); other && other != &o) {
                s += "(";
                stampMatrix(s, doc.world(*other));
                stampMatrix(s, doc.world(o));
                stampObject(doc, *other, s, inputs, depth + 1);
                s += ")";
            }
        }
    }
}

// Linear sRGB-ish conversion for the viewport colours.
std::uint8_t byte(double v) { return static_cast<std::uint8_t>(std::clamp(v, 0.0, 1.0) * 255 + 0.5); }

}  // namespace

void ModelDoc::refresh() {
    mesh = {};
    triObject.clear();
    triFace.clear();
    std::vector<raster::Color> colors;
    std::map<std::string, CacheEntry> keep;
    for (std::size_t i = 0; i < scene.objects.size(); ++i) {
        const Object& o = scene.objects[i];
        if (o.type != "mesh" || !shown(o) || !meshOf(o)) continue;
        std::string stamp;
        std::vector<std::shared_ptr<const PolyMesh>> inputs;
        stampObject(*this, o, stamp, inputs, 0);
        stampMatrix(stamp, world(o));
        auto it = cache_.find(o.name);
        CacheEntry entry;
        if (it != cache_.end() && it->second.stamp == stamp) {
            entry = it->second;
        } else {
            auto ev = std::make_shared<Evaluated>();
            PolyMesh pm = evaluate(*this, o, &ev->errors);
            transform(pm, world(o));
            TriangulateOptions to;
            to.autoSmooth = o.autoSmooth;
            ev->tris = triangulate(pm, to);
            ev->faces = pm.faces.size();
            entry.stamp = std::move(stamp);
            entry.value = std::move(ev);
            entry.inputs = std::move(inputs);
        }
        const Evaluated& ev = *entry.value;
        keep[o.name] = entry;
        const auto first = static_cast<std::uint32_t>(mesh.positions.size());
        (void)first;
        mesh::append(mesh, ev.tris.mesh);
        const bool vcol = !ev.tris.color.empty();
        for (std::size_t t = 0; t < ev.tris.face.size(); ++t) {
            triObject.push_back(static_cast<int>(i));
            triFace.push_back(ev.tris.face[t]);
            const int slot = t < ev.tris.material.size() ? ev.tris.material[t] : 0;
            Vec3 c{0.8, 0.8, 0.8};
            if (slot >= 0 && static_cast<std::size_t>(slot) < o.materials.size())
                if (const Material* m = material(o.materials[static_cast<std::size_t>(slot)])) c = m->color;
            if (vcol) {
                Vec3 vc{0, 0, 0};
                for (int k = 0; k < 3; ++k) vc = vc + ev.tris.color[ev.tris.mesh.indices[t * 3 + static_cast<std::size_t>(k)]];
                vc = vc * (1.0 / 3);
                c = {c.x * vc.x, c.y * vc.y, c.z * vc.z};
            }
            if (!scene.paintBone.empty()) {
                // weight paint view: blue (0) through green to red (1), like Blender
                auto wit = o.weights.find(scene.paintBone);
                double wsum = 0;
                if (wit != o.weights.end() && t < ev.tris.face.size()) {
                    const PolyMesh* base = meshOf(o);
                    const std::uint32_t poly = ev.tris.face[t];
                    if (base && poly < base->faces.size()) {
                        const auto& fv = base->faces[poly].v;
                        for (auto v : fv) wsum += v < wit->second.size() ? wit->second[v] : 0.0;
                        wsum /= std::max<std::size_t>(1, fv.size());
                    }
                }
                const double x = std::clamp(wsum, 0.0, 1.0);
                c = x < 0.5 ? Vec3{0.1, 0.2 + 1.4 * x, 0.9 - 1.6 * x} : Vec3{0.2 + 1.6 * (x - 0.5), 0.9 - 1.4 * (x - 0.5), 0.1};
            }
            colors.push_back({byte(c.x), byte(c.y), byte(c.z), 255});
        }
    }
    cache_ = std::move(keep);
    previewOptions.faceColors = std::move(colors);
}

const Evaluated& ModelDoc::evaluated(const std::string& name) const {
    static const Evaluated none;
    auto it = cache_.find(name);
    return it == cache_.end() || !it->second.value ? none : *it->second.value;
}

std::string addMeshObject(ModelDoc& doc, const std::string& name, PolyMesh m) {
    Object o;
    o.name = doc.uniqueName(name);
    o.data = doc.uniqueMeshName(o.name);
    o.location = doc.scene.cursor;
    doc.scene.meshes[o.data] = std::make_shared<PolyMesh>(std::move(m));
    for (auto& other : doc.scene.objects) other.selected = false;
    o.selected = true;
    doc.scene.active = o.name;
    doc.scene.objects.push_back(std::move(o));
    return doc.scene.active;
}

std::shared_ptr<ModelDoc> newModel(bool withCube) {
    auto doc = std::make_shared<ModelDoc>();
    Material mat;
    mat.name = "Material";
    doc->scene.materials.push_back(mat);
    if (withCube) {
        addMeshObject(*doc, "Cube", primitive("cube"));
        doc->scene.objects.back().materials = {"Material"};
    }
    Object light;
    light.name = "Light";
    light.type = "light";
    light.location = {4.076, 1.005, 5.904};
    light.rotation = {37.3, 3.16, 106.9};
    light.power = 1000;
    doc->scene.objects.push_back(light);
    Object cam;
    cam.name = "Camera";
    cam.type = "camera";
    cam.location = {7.359, -6.926, 4.958};
    cam.rotation = {63.559, 0, 46.692};
    cam.fov = 30;
    doc->scene.objects.push_back(cam);
    doc->refresh();
    return doc;
}

std::string ModelDoc::summary() const {
    std::size_t meshObjects = 0, verts = 0, faces = 0;
    for (const auto& o : scene.objects) {
        if (const PolyMesh* m = meshOf(o)) {
            ++meshObjects;
            verts += m->verts.size();
            faces += m->faces.size();
        }
    }
    const auto pl = [](std::size_t n, const char* one, const char* more) { return std::to_string(n) + (n == 1 ? one : more); };
    std::string s = pl(scene.objects.size(), " object (", " objects (") + pl(meshObjects, " mesh): ", " meshes): ") +
                    pl(verts, " vertex, ", " vertices, ") + pl(faces, " face", " faces");
    if (!scene.active.empty()) s += "; active " + scene.active;
    if (scene.editMode) {
        s += "; edit mode (" + std::string(toString(scene.selectMode)) + ")";
        if (const Object* a = activeObject())
            if (const PolyMesh* m = meshOf(*a)) {
                const auto c = countSelection(*m);
                s += ", selected " + std::to_string(c.verts) + "/" + std::to_string(m->verts.size()) + " vertices, " +
                     std::to_string(c.faces) + "/" + std::to_string(m->faces.size()) + " faces";
            }
    }
    if (!path.empty()) s += "; " + path;
    return s;
}

std::unique_ptr<Attachment> ModelDoc::preview() const {
    std::vector<std::uint8_t> rgba;
    const int size = 512;
    if (!renderView(OrbitView{}, size, size, rgba)) return nullptr;
    raster::Image img(size, size);
    std::copy(rgba.begin(), rgba.end(), img.data());
    auto a = std::make_unique<Attachment>();
    a->mime = "image/png";
    a->bytes = raster::encodePng(img);
    a->name = "preview.png";
    return a;
}

// ---- the view ----

namespace {

Projector fit(const Vec3& lo, const Vec3& hi, const OrbitView& view, int width, int height) {
    const double rad = mesh::kPi / 180.0;
    const double yaw = view.yaw * rad, pitch = std::clamp(view.pitch, -89.9, 89.9) * rad;
    const Vec3 eye{std::cos(pitch) * std::sin(yaw), -std::cos(pitch) * std::cos(yaw), std::sin(pitch)};
    Projector p;
    p.forward = normalize(-eye);
    p.right = normalize(cross(p.forward, Vec3{0, 0, 1}));
    p.up = cross(p.right, p.forward);
    p.width = width;
    p.height = height;
    p.centre = (lo + hi) * 0.5;
    p.radius = std::max(length(hi - lo) / 2, 1e-12);
    const double avail = 1.0 - 2.0 * 0.06;
    p.scale = std::min(width, height) * avail / (2 * p.radius) * std::clamp(view.zoom, 1e-3, 1e4);
    p.cx = dot(p.centre, p.right) - view.panX * p.radius;
    p.cy = dot(p.centre, p.up) - view.panY * p.radius;
    if (view.camera) {  // a camera held still by the window
        const ViewCamera& c = *view.camera;
        p.right = {c.right[0], c.right[1], c.right[2]};
        p.up = {c.up[0], c.up[1], c.up[2]};
        p.forward = {c.forward[0], c.forward[1], c.forward[2]};
        p.cx = c.cx;
        p.cy = c.cy;
        p.scale = c.scale;
    }
    return p;
}

void growBounds(const std::vector<Vec3>& pts, Vec3& lo, Vec3& hi) {
    for (const Vec3& q : pts) {
        lo = {std::min(lo.x, q.x), std::min(lo.y, q.y), std::min(lo.z, q.z)};
        hi = {std::max(hi.x, q.x), std::max(hi.y, q.y), std::max(hi.z, q.z)};
    }
}

// Half the floor grid's size: 3 m, doubled until it holds the scene with room
// to spare. It steps rather than following the scene, so moving an object
// doesn't refit the view (it would look as if nothing moved).
double floorExtent(const Vec3& lo, const Vec3& hi) {
    double g = 3;
    if (lo.x > hi.x) return g;
    const double m = std::max({std::fabs(lo.x), std::fabs(hi.x), std::fabs(lo.y), std::fabs(hi.y)});
    while (g < m * 1.2 && g < 1e9) g *= 2;
    return g;
}

void growByFloor(Vec3& lo, Vec3& hi) {
    const double g = floorExtent(lo, hi);
    if (lo.x > hi.x) lo = hi = {0, 0, 0};
    growBounds({{-g, -g, 0}, {g, g, 0}}, lo, hi);
}

// The active object's editable mesh in world space (positions only).
std::vector<Vec3> cagePositions(const ModelDoc& doc, const PolyMesh& m, const Object& o) {
    const Mat4 w = doc.world(o);
    std::vector<Vec3> out(m.verts.size());
    for (std::size_t i = 0; i < m.verts.size(); ++i) out[i] = w.point(m.verts[i].p);
    return out;
}

void rasterize(ViewBuffer& b, const Vec3& a, const Vec3& c1, const Vec3& c2, int object, int face) {
    const Projector& p = b.proj;
    const Vec2 s0 = p.toScreen(a), s1 = p.toScreen(c1), s2 = p.toScreen(c2);
    const double d0 = p.depth(a), d1 = p.depth(c1), d2 = p.depth(c2);
    const double area = (s1.x - s0.x) * (s2.y - s0.y) - (s1.y - s0.y) * (s2.x - s0.x);
    if (std::fabs(area) < 1e-12) return;
    const int x0 = std::max(0, static_cast<int>(std::floor(std::min({s0.x, s1.x, s2.x}))));
    const int x1 = std::min(p.width - 1, static_cast<int>(std::ceil(std::max({s0.x, s1.x, s2.x}))));
    const int y0 = std::max(0, static_cast<int>(std::floor(std::min({s0.y, s1.y, s2.y}))));
    const int y1 = std::min(p.height - 1, static_cast<int>(std::ceil(std::max({s0.y, s1.y, s2.y}))));
    for (int y = y0; y <= y1; ++y) {
        for (int x = x0; x <= x1; ++x) {
            const double px = x + 0.5, py = y + 0.5;
            const double w0 = ((s1.x - px) * (s2.y - py) - (s1.y - py) * (s2.x - px)) / area;
            const double w1 = ((s2.x - px) * (s0.y - py) - (s2.y - py) * (s0.x - px)) / area;
            const double w2 = 1 - w0 - w1;
            if (w0 < -1e-9 || w1 < -1e-9 || w2 < -1e-9) continue;
            const float d = static_cast<float>(w0 * d0 + w1 * d1 + w2 * d2);
            const std::size_t i = static_cast<std::size_t>(y) * p.width + x;
            if (d < b.depth[i]) {
                b.depth[i] = d;
                b.object[i] = object;
                b.face[i] = face;
            }
        }
    }
}

}  // namespace

Vec2 Projector::toScreen(const Vec3& p) const {
    return {(dot(p, right) - cx) * scale + width / 2.0, height / 2.0 - (dot(p, up) - cy) * scale};
}

Vec3 Projector::ray(double x, double y) const {
    const double a = (x - width / 2.0) / scale + cx, b = (height / 2.0 - y) / scale + cy;
    const double c = dot(centre, forward) - 2 * radius - 1;
    return right * a + up * b + forward * c;
}

Projector projector(const mesh::Mesh& sceneMesh, const OrbitView& view, int width, int height) {
    Vec3 lo{1e300, 1e300, 1e300}, hi{-1e300, -1e300, -1e300};
    growBounds(sceneMesh.positions, lo, hi);
    growByFloor(lo, hi);
    return fit(lo, hi, view, width, height);
}

bool presetView(const std::string& name, double& yaw, double& pitch) {
    static const std::map<std::string, std::pair<double, double>> views{
        {"iso", {45, 35.26}}, {"front", {0, 0}},  {"back", {180, 0}},    {"left", {-90, 0}},
        {"right", {90, 0}},   {"side", {90, 0}},  {"top", {0, 89.9}},    {"bottom", {0, -89.9}}};
    auto it = views.find(name);
    if (it == views.end()) return false;
    yaw = it->second.first;
    pitch = it->second.second;
    return true;
}

bool ViewBuffer::visible(const Vec3& world, double slack) const {
    const Vec2 s = proj.toScreen(world);
    const int x = static_cast<int>(std::floor(s.x)), y = static_cast<int>(std::floor(s.y));
    if (at(x, y) < 0) return false;
    // the farthest surface around the point: on a silhouette the pixel itself
    // may be empty, but its neighbours tell whether something is in front
    bool covered = false;
    float far = -1e30f;
    for (int dy = -1; dy <= 1; ++dy)
        for (int dx = -1; dx <= 1; ++dx) {
            const int i = at(x + dx, y + dy);
            if (i < 0 || object[static_cast<std::size_t>(i)] < 0) continue;
            covered = true;
            far = std::max(far, depth[static_cast<std::size_t>(i)]);
        }
    if (!covered) return true;
    if (slack <= 0) slack = proj.radius * 0.012 + 2.0 / proj.scale;
    return proj.depth(world) <= far + slack;
}

ViewBuffer viewBuffer(const ModelDoc& doc, const OrbitView& view, int width, int height, bool cage) {
    ViewBuffer b;
    const Object* active = cage ? doc.activeObject() : nullptr;
    const PolyMesh* cm = active ? doc.meshOf(*active) : nullptr;
    std::vector<Vec3> cagePts;
    if (cm) cagePts = cagePositions(doc, *cm, *active);
    if (doc.mesh.positions.empty() && !cagePts.empty()) {
        Vec3 lo{1e300, 1e300, 1e300}, hi{-1e300, -1e300, -1e300};
        growBounds(cagePts, lo, hi);
        growByFloor(lo, hi);
        b.proj = fit(lo, hi, view, width, height);
    } else {
        b.proj = projector(doc.mesh, view, width, height);
    }
    const std::size_t n = static_cast<std::size_t>(std::max(0, width)) * static_cast<std::size_t>(std::max(0, height));
    b.depth.assign(n, 1e30f);
    b.object.assign(n, -1);
    b.face.assign(n, -1);
    const int activeIndex = active ? static_cast<int>(active - doc.scene.objects.data()) : -1;
    const auto& M = doc.mesh;
    for (std::size_t t = 0; t < M.triangleCount() && t < doc.triObject.size(); ++t) {
        if (cm && doc.triObject[t] == activeIndex) continue;
        rasterize(b, M.corner(t, 0), M.corner(t, 1), M.corner(t, 2), doc.triObject[t], static_cast<int>(doc.triFace[t]));
    }
    if (cm) {
        for (std::size_t f = 0; f < cm->faces.size(); ++f) {
            const auto& face = cm->faces[f];
            const auto tri = triangulateFace(*cm, face);
            for (std::size_t k = 0; k + 2 < tri.size(); k += 3)
                rasterize(b, cagePts[face.v[tri[k]]], cagePts[face.v[tri[k + 1]]], cagePts[face.v[tri[k + 2]]], activeIndex,
                          static_cast<int>(f));
        }
    }
    return b;
}

namespace {

struct Canvas {
    std::vector<std::uint8_t>& px;
    int w, h;
    void set(int x, int y, raster::Color c, double alpha = 1) {
        if (x < 0 || y < 0 || x >= w || y >= h) return;
        std::uint8_t* p = px.data() + (static_cast<std::size_t>(y) * w + x) * 4;
        p[0] = static_cast<std::uint8_t>(p[0] + (c.r - p[0]) * alpha);
        p[1] = static_cast<std::uint8_t>(p[1] + (c.g - p[1]) * alpha);
        p[2] = static_cast<std::uint8_t>(p[2] + (c.b - p[2]) * alpha);
        p[3] = 255;
    }
};

const raster::Color kSelect{255, 160, 40, 255};
const raster::Color kActive{255, 200, 90, 255};
const raster::Color kWire{20, 20, 24, 255};

void drawEdge(Canvas& c, const ViewBuffer& b, const Vec3& a, const Vec3& e, raster::Color col, int thick, bool through) {
    const Vec2 s0 = b.proj.toScreen(a), s1 = b.proj.toScreen(e);
    const int steps = static_cast<int>(std::ceil(std::max(std::fabs(s1.x - s0.x), std::fabs(s1.y - s0.y)))) + 1;
    for (int i = 0; i <= steps; ++i) {
        const double t = static_cast<double>(i) / steps;
        const Vec3 p = a + (e - a) * t;
        if (!through && !b.visible(p)) continue;
        const int x = static_cast<int>(std::floor(s0.x + (s1.x - s0.x) * t)), y = static_cast<int>(std::floor(s0.y + (s1.y - s0.y) * t));
        for (int dy = 0; dy < thick; ++dy)
            for (int dx = 0; dx < thick; ++dx) c.set(x + dx, y + dy, col);
    }
}

}  // namespace

bool ModelDoc::viewCamera(const OrbitView& view, int width, int height, ViewCamera& camera) const {
    const Projector p = projector(mesh, view, width, height);
    camera.right[0] = p.right.x, camera.right[1] = p.right.y, camera.right[2] = p.right.z;
    camera.up[0] = p.up.x, camera.up[1] = p.up.y, camera.up[2] = p.up.z;
    camera.forward[0] = p.forward.x, camera.forward[1] = p.forward.y, camera.forward[2] = p.forward.z;
    camera.cx = p.cx;
    camera.cy = p.cy;
    camera.scale = p.scale;
    return true;
}

bool ModelDoc::renderView(const OrbitView& view, int width, int height, std::vector<std::uint8_t>& rgba) const {
    if (width == 0 && height == 0) return true;
    if (width <= 0 || height <= 0) return false;
    // a floor grid with the red X and green Y axes, as in Blender; it also keeps the view still
    mesh::RenderOptions opts = previewOptions;
    {
        Vec3 lo{1e300, 1e300, 1e300}, hi{-1e300, -1e300, -1e300};
        growBounds(mesh.positions, lo, hi);
        const double g = floorExtent(lo, hi), step = g / 6;
        for (int i = -6; i <= 6; ++i) {
            const double t = i * step;
            const raster::Color grey{205, 207, 212, 255};
            opts.lines.push_back({{{-g, t, 0}, {g, t, 0}}, i == 0 ? raster::Color{225, 110, 110, 255} : grey});
            opts.lines.push_back({{{t, -g, 0}, {t, g, 0}}, i == 0 ? raster::Color{120, 185, 120, 255} : grey});
        }
    }
    if (!mesh::renderOrbit(mesh, view, width, height, rgba, opts)) return false;
    Canvas canvas{rgba, width, height};
    const int thick = std::max(1, std::min(width, height) / 600);
    const Object* active = activeObject();
    const PolyMesh* cm = scene.editMode && active ? meshOf(*active) : nullptr;
    const ViewBuffer b = viewBuffer(*this, view, width, height, cm != nullptr);
    const bool through = view.shading == "xray" || view.shading == "wireframe";  // see (and select) through the mesh
    if (cm) {
        const std::vector<Vec3> pts = cagePositions(*this, *cm, *active);
        const int ai = static_cast<int>(active - scene.objects.data());
        for (std::size_t i = 0; i < b.object.size(); ++i)
            if (b.object[i] == ai && b.face[i] >= 0 && cm->faces[static_cast<std::size_t>(b.face[i])].sel)
                canvas.set(static_cast<int>(i % width), static_cast<int>(i / width), kSelect, 0.28);
        for (EdgeKey k : allEdges(*cm)) {
            const bool sel = cm->esel.count(k) > 0;
            drawEdge(canvas, b, pts[keyA(k)], pts[keyB(k)], sel ? kSelect : kWire, sel ? thick + 1 : thick, through);
        }
        if (scene.selectMode == SelectMode::Vertex) {
            const int r = thick + 1;
            for (std::size_t v = 0; v < pts.size(); ++v) {
                if (!through && !b.visible(pts[v])) continue;
                const Vec2 s = b.proj.toScreen(pts[v]);
                const int x = static_cast<int>(std::floor(s.x)), y = static_cast<int>(std::floor(s.y));
                for (int dy = -r; dy <= r; ++dy)
                    for (int dx = -r; dx <= r; ++dx) canvas.set(x + dx, y + dy, cm->verts[v].sel ? kSelect : kWire);
            }
        }
    } else {
        // object mode: outline selected objects
        for (int y = 0; y < height; ++y) {
            for (int x = 0; x < width; ++x) {
                const int o = b.object[static_cast<std::size_t>(y) * width + x];
                if (o < 0 || !scene.objects[static_cast<std::size_t>(o)].selected) continue;
                bool edge = false;
                for (int d = 1; d <= thick + 1 && !edge; ++d) {
                    const int n[4] = {b.at(x - d, y), b.at(x + d, y), b.at(x, y - d), b.at(x, y + d)};
                    for (int i : n) edge = edge || i < 0 || b.object[static_cast<std::size_t>(i)] != o;
                }
                if (edge) canvas.set(x, y, scene.objects[static_cast<std::size_t>(o)].name == scene.active ? kActive : kSelect);
            }
        }
    }
    // armatures: each bone a tapered stick from head to tail, the posed one where it is now
    for (const auto& o : scene.objects) {
        if (o.type != "armature" || !shown(o)) continue;
        const Mat4 w = world(o);
        const auto posed = posedBones(o);
        for (const auto& bone : o.bones) {
            auto it = posed.find(bone.name);
            if (it == posed.end()) continue;
            const double len = mesh::length(bone.tail - bone.head);
            const Vec3 h = w.point(it->second.point({0, 0, 0})), t = w.point(it->second.point({0, len, 0}));
            const raster::Color col = o.selected ? (bone.name == scene.paintBone ? kActive : kSelect) : raster::Color{70, 80, 100, 255};
            drawEdge(canvas, b, h, t, col, thick + 1, true);
            const Vec2 sh = b.proj.toScreen(h);
            const int r = 2 * thick + 1;
            for (int dy = -r; dy <= r; ++dy)
                for (int dx = -r; dx <= r; ++dx)
                    if (dx * dx + dy * dy <= r * r) canvas.set(static_cast<int>(sh.x) + dx, static_cast<int>(sh.y) + dy, col);
        }
    }
    // empties, lights and cameras as small crosses
    for (const auto& o : scene.objects) {
        if (o.type == "mesh" || o.type == "armature" || !shown(o)) continue;
        const Vec2 s = b.proj.toScreen(world(o).point({0, 0, 0}));
        const int x = static_cast<int>(s.x), y = static_cast<int>(s.y), r = 4 * thick;
        const raster::Color col = o.selected ? kSelect : raster::Color{60, 60, 70, 255};
        for (int i = -r; i <= r; ++i) {
            canvas.set(x + i, y, col);
            canvas.set(x, y + i, col);
        }
    }
    return true;
}

// ---- animation ----

namespace {

double* channelRef(Object& o, const std::string& ch) {
    if (ch.rfind("pose.", 0) == 0) {  // pose.<bone>.rotation.x
        const auto last = ch.rfind('.');
        const auto mid = last == std::string::npos ? std::string::npos : ch.rfind('.', last - 1);
        if (mid == std::string::npos || mid <= 5) return nullptr;
        Bone* b = findBone(o, ch.substr(5, mid - 5));
        if (!b || last + 2 != ch.size()) return nullptr;
        const std::string group = ch.substr(mid + 1, last - mid - 1);
        Vec3* v = group == "rotation" ? &b->rotation : group == "location" ? &b->location : nullptr;
        if (!v) return nullptr;
        const char axis = ch[last + 1];
        return axis == 'x' ? &v->x : axis == 'y' ? &v->y : axis == 'z' ? &v->z : nullptr;
    }
    const auto dot = ch.find('.');
    if (dot == std::string::npos || dot + 2 != ch.size()) return nullptr;
    const std::string group = ch.substr(0, dot);
    const char axis = ch[dot + 1];
    Vec3* v = group == "location" ? &o.location : group == "rotation" ? &o.rotation : group == "scale" ? &o.scale : nullptr;
    if (!v) return nullptr;
    return axis == 'x' ? &v->x : axis == 'y' ? &v->y : axis == 'z' ? &v->z : nullptr;
}

}  // namespace

double channelValue(const Object& o, const std::string& channel) {
    double* p = channelRef(const_cast<Object&>(o), channel);
    if (!p) throw std::invalid_argument("unknown channel '" + channel + "'");
    return *p;
}

void setChannel(Object& o, const std::string& channel, double v) {
    double* p = channelRef(o, channel);
    if (!p) throw std::invalid_argument("unknown channel '" + channel + "'");
    *p = v;
}

double channelAt(const Object& o, const std::string& channel, double frame, double fallback) {
    auto it = o.keys.find(channel);
    if (it == o.keys.end() || it->second.empty()) return fallback;
    const auto& k = it->second;  // kept sorted by frame
    if (frame <= k.front().frame) return k.front().value;
    if (frame >= k.back().frame) return k.back().value;
    for (std::size_t i = 0; i + 1 < k.size(); ++i) {
        if (frame >= k[i].frame && frame <= k[i + 1].frame) {
            const double span = std::max(1, k[i + 1].frame - k[i].frame);
            double t = (frame - k[i].frame) / span;
            if (k[i].interp == "constant") return k[i].value;
            if (k[i].interp != "linear") t = t * t * (3 - 2 * t);
            return k[i].value + (k[i + 1].value - k[i].value) * t;
        }
    }
    return k.back().value;
}

void applyFrame(ModelDoc& doc, int frame) {
    doc.scene.frame = frame;
    for (auto& o : doc.scene.objects)
        for (const auto& [ch, keys] : o.keys)
            if (!keys.empty()) setChannel(o, ch, channelAt(o, ch, frame, channelValue(o, ch)));
}

// ---- undo ----

Result ModelEdit::apply() {
    before_ = doc_->scene;
    Result r;
    try {
        r = fn_(*doc_);
    } catch (const std::exception& e) {
        r = Result::failure(e.what());
    }
    if (!r.ok) doc_->scene = before_;
    doc_->refresh();
    return r;
}

void ModelEdit::revert() {
    doc_->scene = before_;
    doc_->refresh();
}

}  // namespace wetwaresuite::model
