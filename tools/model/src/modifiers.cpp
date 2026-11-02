// The modifier stack: non-destructive operations evaluated on a copy of an
// object's mesh, in order, every time the scene is drawn, rendered or
// exported (the evaluation is cached until the mesh, the stack or the
// transforms change).
#include <algorithm>
#include <cmath>
#include <map>
#include <memory>
#include <stdexcept>

#include "wetwaresuite/mesh/csg.hpp"
#include "wetwaresuite/model/model.hpp"
#include "wetwaresuite/raster/codec.hpp"
#include "wetwaresuite/sculpt/ops.hpp"

namespace wetwaresuite::model {

const std::vector<std::string>& modifierTypes() {
    static const std::vector<std::string> types{"mirror", "array",  "subsurf", "solidify", "bevel",      "boolean",
                                                "decimate", "smooth", "displace", "weld",   "triangulate", "armature"};
    return types;
}

double Modifier::get(const std::string& key, double fallback) const {
    auto it = num.find(key);
    return it == num.end() ? fallback : it->second;
}

std::string Modifier::str(const std::string& key, const std::string& fallback) const {
    auto it = text.find(key);
    return it == text.end() ? fallback : it->second;
}

Modifier makeModifier(const std::string& type) {
    Modifier m;
    if (std::find(modifierTypes().begin(), modifierTypes().end(), type) == modifierTypes().end()) return m;
    m.type = type;
    m.name = type;
    m.name[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(m.name[0])));
    if (type == "mirror") {
        m.text["axis"] = "x";
        m.num = {{"merge", 1}, {"distance", 0.001}, {"bisect", 0}};
    } else if (type == "array") {
        m.num = {{"count", 2}, {"relative_x", 1}, {"relative_y", 0}, {"relative_z", 0}, {"constant_x", 0},
                 {"constant_y", 0}, {"constant_z", 0}, {"merge", 0}, {"distance", 0.01}};
    } else if (type == "subsurf") {
        m.num = {{"levels", 1}};
    } else if (type == "solidify") {
        m.num = {{"thickness", 0.1}, {"offset", -1}, {"rim", 1}};
    } else if (type == "bevel") {
        m.num = {{"width", 0.1}, {"segments", 1}, {"angle", 30}};
        m.text["limit"] = "angle";
    } else if (type == "boolean") {
        m.text = {{"object", ""}, {"operation", "difference"}};
    } else if (type == "decimate") {
        m.num = {{"ratio", 0.5}, {"angle", 5}};
        m.text["mode"] = "collapse";
    } else if (type == "smooth") {
        m.num = {{"factor", 0.5}, {"repeat", 1}};
    } else if (type == "displace") {
        m.num = {{"strength", 0.2}, {"midlevel", 0.5}, {"scale", 1}, {"seed", 0}};
        m.text = {{"texture", ""}, {"direction", "normal"}};
    } else if (type == "weld") {
        m.num = {{"distance", 0.001}};
    } else if (type == "armature") {
        m.text = {{"object", ""}};
    }
    return m;
}

namespace {

constexpr std::size_t kMaxFaces = 1'000'000;

void checkSize(const PolyMesh& m) {
    if (m.faces.size() > kMaxFaces) throw std::runtime_error("more than 1000000 faces");
}

int axisIndex(char c) { return c == 'x' ? 0 : c == 'y' ? 1 : c == 'z' ? 2 : -1; }

PolyMesh solidify(const PolyMesh& in, double thickness, double offset, bool rim) {
    const auto normals = vertexNormals(in);
    const double outer = thickness * (offset + 1) / 2, inner = thickness * (offset - 1) / 2;
    PolyMesh m;
    m.colors = in.colors;
    const auto n = static_cast<std::uint32_t>(in.verts.size());
    for (std::uint32_t i = 0; i < n; ++i) {
        Vertex v = in.verts[i];
        v.sel = false;
        v.p = in.verts[i].p + normals[i] * outer;
        m.verts.push_back(v);
    }
    for (std::uint32_t i = 0; i < n; ++i) {
        Vertex v = in.verts[i];
        v.sel = false;
        v.p = in.verts[i].p + normals[i] * inner;
        m.verts.push_back(v);
    }
    for (const auto& f : in.faces) {
        Face a = f;
        a.sel = false;
        m.faces.push_back(a);
        Face b = f;
        b.sel = false;
        std::reverse(b.v.begin(), b.v.end());
        std::reverse(b.uv.begin(), b.uv.end());
        for (auto& v : b.v) v += n;
        m.faces.push_back(std::move(b));
    }
    if (rim) {
        const Topology t(in);
        for (std::size_t e = 0; e < t.edges.size(); ++e) {
            if (t.edgeFaces[e].size() != 1) continue;
            const HalfRef h = t.edgeFaces[e][0];
            const Face& f = in.faces[h.face];
            const std::uint32_t a = f.v[h.corner], b = f.v[(h.corner + 1) % f.v.size()];
            Face q;
            q.v = {b, a, a + n, b + n};
            q.material = f.material;
            q.smooth = f.smooth;
            if (!f.uv.empty()) {
                const Vec2 ua = f.uv[h.corner], ub = f.uv[(h.corner + 1) % f.v.size()];
                q.uv = {ub, ua, ua, ub};
            }
            m.faces.push_back(std::move(q));
        }
    }
    for (auto k : in.seams) {
        m.seams.insert(k);
        m.seams.insert(edgeKey(keyA(k) + n, keyB(k) + n));
    }
    for (auto k : in.sharp) {
        m.sharp.insert(k);
        m.sharp.insert(edgeKey(keyA(k) + n, keyB(k) + n));
    }
    cleanup(m);
    return m;
}

// Smooth 3D value noise in 0..1, three octaves.
double hash3(int x, int y, int z, int seed) {
    unsigned h = static_cast<unsigned>(x) * 73856093u ^ static_cast<unsigned>(y) * 19349663u ^ static_cast<unsigned>(z) * 83492791u ^
                 static_cast<unsigned>(seed) * 2654435761u;
    h ^= h >> 13;
    h *= 0x5bd1e995u;
    h ^= h >> 15;
    return (h & 0xffffff) / static_cast<double>(0xffffff);
}

double valueNoise(const Vec3& p, int seed) {
    const int x = static_cast<int>(std::floor(p.x)), y = static_cast<int>(std::floor(p.y)), z = static_cast<int>(std::floor(p.z));
    auto fade = [](double t) { return t * t * (3 - 2 * t); };
    const double fx = fade(p.x - x), fy = fade(p.y - y), fz = fade(p.z - z);
    double c[2][2][2];
    for (int i = 0; i < 2; ++i)
        for (int j = 0; j < 2; ++j)
            for (int k = 0; k < 2; ++k) c[i][j][k] = hash3(x + i, y + j, z + k, seed);
    auto lerp = [](double a, double b, double t) { return a + (b - a) * t; };
    const double x00 = lerp(c[0][0][0], c[1][0][0], fx), x10 = lerp(c[0][1][0], c[1][1][0], fx);
    const double x01 = lerp(c[0][0][1], c[1][0][1], fx), x11 = lerp(c[0][1][1], c[1][1][1], fx);
    return lerp(lerp(x00, x10, fy), lerp(x01, x11, fy), fz);
}

double fbm(const Vec3& p, int seed) {
    double sum = 0, amp = 0.5, total = 0;
    Vec3 q = p;
    for (int o = 0; o < 3; ++o) {
        sum += valueNoise(q, seed + o) * amp;
        total += amp;
        amp *= 0.5;
        q = q * 2.03;
    }
    return sum / total;
}

std::shared_ptr<const raster::Image> loadTexture(const std::string& path) {
    static std::map<std::string, std::shared_ptr<const raster::Image>> cache;
    auto it = cache.find(path);
    if (it != cache.end()) return it->second;
    auto img = std::make_shared<raster::Image>(raster::load(path));
    if (cache.size() > 16) cache.clear();
    cache[path] = img;
    return img;
}

PolyMesh displace(const PolyMesh& in, const Modifier& mod) {
    PolyMesh m = in;
    const auto normals = vertexNormals(m);
    const double strength = mod.get("strength", 0.2), mid = mod.get("midlevel", 0.5), scale = mod.get("scale", 1);
    const int seed = static_cast<int>(mod.get("seed", 0));
    const std::string tex = mod.str("texture", ""), dir = mod.str("direction", "normal");
    std::shared_ptr<const raster::Image> img;
    std::vector<Vec2> uv;
    if (!tex.empty()) {
        img = loadTexture(tex);
        if (img->empty()) throw std::runtime_error("texture '" + tex + "' is empty");
        if (!m.hasUvs()) throw std::runtime_error("displace with a texture needs UVs (model.uv_unwrap)");
        uv.assign(m.verts.size(), Vec2{0, 0});
        std::vector<char> seen(m.verts.size(), 0);
        for (const auto& f : m.faces)
            for (std::size_t c = 0; c < f.v.size() && c < f.uv.size(); ++c)
                if (!seen[f.v[c]]) {
                    seen[f.v[c]] = 1;
                    uv[f.v[c]] = f.uv[c];
                }
    }
    for (std::size_t i = 0; i < m.verts.size(); ++i) {
        double value;
        if (img) {
            const double u = uv[i].x - std::floor(uv[i].x), v = uv[i].y - std::floor(uv[i].y);
            const int x = std::clamp(static_cast<int>(u * img->width()), 0, img->width() - 1);
            const int y = std::clamp(static_cast<int>((1 - v) * img->height()), 0, img->height() - 1);
            const auto* p = img->pixel(x, y);
            value = (0.2126 * p[0] + 0.7152 * p[1] + 0.0722 * p[2]) / 255.0;
        } else {
            value = fbm(in.verts[i].p * scale, seed);
        }
        const double d = (value - mid) * strength;
        Vec3 n = normals[i];
        if (dir == "x") n = {1, 0, 0};
        if (dir == "y") n = {0, 1, 0};
        if (dir == "z") n = {0, 0, 1};
        m.verts[i].p = m.verts[i].p + n * d;
    }
    return m;
}

thread_local int booleanDepth = 0;

PolyMesh booleanMod(const ModelDoc& doc, const Object& o, const PolyMesh& in, const Modifier& mod) {
    const std::string name = mod.str("object", "");
    const Object* other = doc.object(name);
    if (name.empty()) throw std::runtime_error("no object set (object=<name>)");
    if (!other) throw std::runtime_error("no object called '" + name + "'");
    if (other == &o) throw std::runtime_error("an object can't cut itself");
    if (!doc.meshOf(*other)) throw std::runtime_error("'" + name + "' isn't a mesh");
    const std::string op = mod.str("operation", "difference");
    mesh::BooleanOp bop = mesh::BooleanOp::Difference;
    if (op == "union") bop = mesh::BooleanOp::Union;
    else if (op == "intersect" || op == "intersection") bop = mesh::BooleanOp::Intersection;
    else if (op != "difference") throw std::runtime_error("operation is union, difference or intersect");
    if (booleanDepth > 4) throw std::runtime_error("booleans nested too deep (or in a loop)");
    ++booleanDepth;
    PolyMesh cutter;
    try {
        cutter = evaluate(doc, *other, nullptr);
    } catch (...) {
        --booleanDepth;
        throw;
    }
    --booleanDepth;
    transform(cutter, doc.world(o).inverse() * doc.world(*other));
    auto solid = [](const PolyMesh& p) {
        TriangulateOptions t;
        t.uvs = false;
        mesh::Mesh tm = triangulate(p, t).mesh;
        tm.normals.clear();
        tm.uvs.clear();
        mesh::weld(tm, 0);
        return tm;
    };
    const mesh::Mesh result = mesh::booleanOp(solid(in), solid(cutter), bop);
    PolyMesh out = fromTriangles(result);
    limitedDissolve(out, 0.5, false);
    const bool smooth = !in.faces.empty() && in.faces.front().smooth;
    for (auto& f : out.faces) f.smooth = smooth;
    return out;
}

PolyMesh decimateMod(const PolyMesh& in, const Modifier& mod) {
    PolyMesh m = in;
    if (mod.str("mode", "collapse") == "planar") {
        limitedDissolve(m, mod.get("angle", 5), false);
        return m;
    }
    TriangulateOptions t;
    t.uvs = false;
    mesh::Mesh tm = triangulate(in, t).mesh;
    tm.normals.clear();
    tm.uvs.clear();
    mesh::weld(tm, 0);
    const double ratio = std::clamp(mod.get("ratio", 0.5), 0.0, 1.0);
    const auto target = std::max<std::size_t>(4, static_cast<std::size_t>(std::llround(ratio * tm.triangleCount())));
    PolyMesh out = fromTriangles(sculpt::decimateQuadric(tm, target));
    const bool smooth = !in.faces.empty() && in.faces.front().smooth;
    for (auto& f : out.faces) f.smooth = smooth;
    return out;
}

}  // namespace

PolyMesh applyModifier(const ModelDoc& doc, const Object& o, const PolyMesh& base, const Modifier& mod) {
    PolyMesh m = base;
    selectAll(m, false);
    const std::string& type = mod.type;
    if (type == "mirror") {
        const std::string axes = mod.str("axis", "x");
        const double dist = mod.get("merge", 1) != 0 ? std::max(mod.get("distance", 0.001), 0.0) : 0.0;
        for (char c : axes) {
            const int axis = axisIndex(c);
            if (axis < 0) throw std::runtime_error("axis is made of x, y and z");
            if (mod.get("bisect", 0) != 0) {
                Vec3 n{0, 0, 0};
                (axis == 0 ? n.x : axis == 1 ? n.y : n.z) = 1;
                bisect(m, {0, 0, 0}, n, true, false, false);
            }
            selectAll(m, false);
            mirror(m, axis, {0, 0, 0}, true, dist);
            checkSize(m);
        }
    } else if (type == "array") {
        const int count = static_cast<int>(mod.get("count", 2));
        if (count < 1 || count > 1000) throw std::runtime_error("count is 1 to 1000");
        const mesh::Bounds b = bounds(base);
        const Vec3 size = b.empty() ? Vec3{0, 0, 0} : b.max - b.min;
        const Vec3 off{mod.get("relative_x", 1) * size.x + mod.get("constant_x", 0),
                       mod.get("relative_y", 0) * size.y + mod.get("constant_y", 0),
                       mod.get("relative_z", 0) * size.z + mod.get("constant_z", 0)};
        if (base.faces.size() * static_cast<std::size_t>(count) > kMaxFaces) throw std::runtime_error("more than 1000000 faces");
        selectAll(m, false);
        PolyMesh one = m;
        for (int i = 1; i < count; ++i) append(m, one, Mat4::translate(off * i));
        if (mod.get("merge", 0) != 0) merge(m, MergeMode::Distance, std::max(mod.get("distance", 0.01), 0.0), {}, true);
    } else if (type == "subsurf") {
        const int levels = static_cast<int>(mod.get("levels", 1));
        if (levels < 0 || levels > 6) throw std::runtime_error("levels is 0 to 6");
        if (levels > 0) m = catmullClark(m, levels, kMaxFaces);
    } else if (type == "solidify") {
        m = solidify(m, mod.get("thickness", 0.1), mod.get("offset", -1), mod.get("rim", 1) != 0);
    } else if (type == "bevel") {
        const double width = mod.get("width", 0.1);
        const int segments = static_cast<int>(mod.get("segments", 1));
        if (width <= 0) throw std::runtime_error("width must be positive");
        if (segments < 1 || segments > 32) throw std::runtime_error("segments is 1 to 32");
        if (mod.str("limit", "angle") == "angle") {
            selectSharpEdges(m, mod.get("angle", 30));
            for (auto k : base.sharp) m.esel.insert(k);
        } else {
            const Topology t(m);
            for (std::size_t e = 0; e < t.edges.size(); ++e)
                if (t.edgeFaces[e].size() == 2) m.esel.insert(t.edges[e]);
        }
        flushSelection(m, SelectMode::Edge);
        if (!m.esel.empty()) bevel(m, width, segments, false);
    } else if (type == "boolean") {
        m = booleanMod(doc, o, m, mod);
    } else if (type == "decimate") {
        m = decimateMod(m, mod);
    } else if (type == "smooth") {
        smoothVertices(m, mod.get("factor", 0.5), static_cast<int>(mod.get("repeat", 1)), false);
    } else if (type == "displace") {
        m = displace(m, mod);
    } else if (type == "weld") {
        merge(m, MergeMode::Distance, std::max(mod.get("distance", 0.001), 0.0), {}, true);
    } else if (type == "triangulate") {
        triangulateFaces(m, false);
    } else if (type == "armature") {
        m = applyArmature(doc, o, m, mod);
    } else {
        throw std::runtime_error("unknown modifier type '" + type + "'");
    }
    selectAll(m, false);
    return m;
}

PolyMesh evaluate(const ModelDoc& doc, const Object& o, std::vector<std::string>* errors) {
    const PolyMesh* base = doc.meshOf(o);
    if (!base) return {};
    PolyMesh m = *base;
    for (const auto& mod : o.modifiers) {
        if (!mod.enabled) continue;
        try {
            m = applyModifier(doc, o, m, mod);
        } catch (const std::exception& e) {
            if (errors) errors->push_back(mod.name + ": " + e.what());
        }
    }
    return m;
}

}  // namespace wetwaresuite::model
