// The scene model: presets, names, world geometry, camera fitting, JSON.
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <fstream>
#include <map>
#include <sstream>
#include <stdexcept>

#include <nlohmann/json.hpp>

#include "wetwaresuite/mesh/io.hpp"
#include "wetwaresuite/mesh/primitives.hpp"
#include "wetwaresuite/raster/image.hpp"
#include "internal.hpp"

namespace wetwaresuite::render {

namespace fs = std::filesystem;
using json = nlohmann::ordered_json;

namespace detail {

float srgbToLinear(float c) {
    c = std::clamp(c, 0.0f, 1.0f);
    return c <= 0.04045f ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f);
}

float linearToSrgb(float c) {
    c = std::clamp(c, 0.0f, 1.0f);
    return c <= 0.0031308f ? c * 12.92f : 1.055f * std::pow(c, 1.0f / 2.4f) - 0.055f;
}

std::optional<Vec3> parseColor(const std::string& text) {
    if (auto c = raster::Color::parse(text)) {
        return Vec3{srgbToLinear(c->r / 255.0f), srgbToLinear(c->g / 255.0f), srgbToLinear(c->b / 255.0f)};
    }
    // "r,g,b" as linear floats
    double v[3];
    char tail = 0;
    if (std::sscanf(text.c_str(), "%lf,%lf,%lf%c", &v[0], &v[1], &v[2], &tail) == 3) {
        if (v[0] < 0 || v[1] < 0 || v[2] < 0) return std::nullopt;
        return Vec3{v[0], v[1], v[2]};
    }
    return std::nullopt;
}

std::string hexColor(const Vec3& c) {
    auto byte = [](double v) { return static_cast<int>(std::lround(linearToSrgb(static_cast<float>(v)) * 255.0f)); };
    char buf[16];
    std::snprintf(buf, sizeof buf, "#%02x%02x%02x", byte(c.x), byte(c.y), byte(c.z));
    return buf;
}

}  // namespace detail

using detail::hexColor;
using detail::parseColor;

const std::vector<std::string>& objectTypes() {
    static const std::vector<std::string> t = {"box", "sphere", "cylinder", "cone", "torus", "plane", "mesh"};
    return t;
}

const std::vector<std::string>& lightTypes() {
    static const std::vector<std::string> t = {"point", "sun", "area"};
    return t;
}

const std::vector<std::string>& materialPresets() {
    static const std::vector<std::string> p = {"matte",  "plastic", "glossy", "metal", "brushed", "chrome", "mirror", "gold",
                                               "copper", "glass",   "frosted", "rubber", "emissive", "ceramic", "clay"};
    return p;
}

bool applyPreset(Material& m, const std::string& name) {
    const std::string tex = m.texture;
    const double texScale = m.textureScale;
    const Vec3 color = m.color;
    Material d;  // defaults
    if (name == "matte") {
        d.roughness = 0.9;
        d.color = color;
    } else if (name == "plastic") {
        d.roughness = 0.35;
        d.color = color;
    } else if (name == "glossy") {
        d.roughness = 0.08;
        d.color = color;
    } else if (name == "metal") {
        d.metallic = 1, d.roughness = 0.3, d.color = color;
    } else if (name == "brushed") {
        d.metallic = 1, d.roughness = 0.5, d.color = {0.75, 0.75, 0.75};
    } else if (name == "chrome") {
        d.metallic = 1, d.roughness = 0.05, d.color = {0.9, 0.9, 0.9};
    } else if (name == "mirror") {
        d.metallic = 1, d.roughness = 0, d.color = {0.95, 0.95, 0.95};
    } else if (name == "gold") {
        d.metallic = 1, d.roughness = 0.2, d.color = {1.0, 0.71, 0.29};
    } else if (name == "copper") {
        d.metallic = 1, d.roughness = 0.25, d.color = {0.95, 0.64, 0.54};
    } else if (name == "glass") {
        d.transmission = 1, d.roughness = 0, d.ior = 1.5, d.color = {1, 1, 1};
    } else if (name == "frosted") {
        d.transmission = 1, d.roughness = 0.3, d.ior = 1.5, d.color = {1, 1, 1};
    } else if (name == "rubber") {
        d.roughness = 0.7, d.color = {0.05, 0.05, 0.05};
    } else if (name == "emissive") {
        d.color = color, d.emission = color, d.emissionStrength = 5;
    } else if (name == "ceramic") {
        d.roughness = 0.15, d.color = {0.9, 0.9, 0.88};
    } else if (name == "clay") {
        d.roughness = 1.0, d.color = {0.8, 0.8, 0.8};
    } else {
        return false;
    }
    const std::string normal = m.normalMap, rough = m.roughnessMap, nm = m.name;
    const double ns = m.normalStrength;
    m = d;
    m.texture = tex;
    m.textureScale = texScale;
    m.normalMap = normal;
    m.normalStrength = ns;
    m.roughnessMap = rough;
    m.name = nm;
    return true;
}

mesh::Mat4 Object::matrix() const {
    return mesh::Mat4::translate(position) * mesh::Mat4::rotate(rotation) * mesh::Mat4::scale(scale);
}

Object* Scene::findObject(const std::string& name) {
    for (auto& o : objects) if (o.name == name) return &o;
    return nullptr;
}
Light* Scene::findLight(const std::string& name) {
    for (auto& l : lights) if (l.name == name) return &l;
    return nullptr;
}
const Object* Scene::findObject(const std::string& name) const {
    for (const auto& o : objects) if (o.name == name) return &o;
    return nullptr;
}
const Light* Scene::findLight(const std::string& name) const {
    for (const auto& l : lights) if (l.name == name) return &l;
    return nullptr;
}

std::string Scene::uniqueName(const std::string& base) const {
    for (int i = 1;; ++i) {
        std::string n = base + std::to_string(i);
        if (!findObject(n) && !findLight(n)) return n;
    }
}

Scene defaultScene() {
    Scene s;
    Light sun;
    sun.name = "sun";
    sun.type = "sun";
    sun.position = {-5, -2, 6};
    sun.target = {0, 0, 0};
    sun.color = {1.0, 0.95, 0.88};
    sun.intensity = 4;
    sun.radius = 2;
    s.lights.push_back(sun);
    addGround(s);
    return s;
}

void addGround(Scene& s, double size) {
    Object g;
    g.name = s.findObject("ground") ? s.uniqueName("ground") : "ground";
    g.type = "plane";
    g.size = {size, size, 0};  // 0 = sized to the scene when rendering
    g.material.color = {0.4, 0.4, 0.4};
    g.material.roughness = 0.85;
    s.objects.insert(s.objects.begin(), g);
}

namespace {

// Geometry in object space, origin at the middle of the base.
mesh::Mesh objectMesh(const Object& o, double autoPlane) {
    using namespace mesh;
    const int seg = std::clamp(o.segments, 3, 512);
    Mesh m;
    if (o.type == "box") {
        m = box(o.size, true);
        transform(m, Mat4::translate({0, 0, o.size.z / 2}));
    } else if (o.type == "sphere") {
        m = sphere(o.radius, seg, std::max(seg / 2, 2));
        transform(m, Mat4::translate({0, 0, o.radius}));
    } else if (o.type == "cylinder") {
        m = cylinder(o.radius, o.height, seg, false);
    } else if (o.type == "cone") {
        m = cone(o.radius, std::max(o.radius2, 0.0), o.height, seg, false);
    } else if (o.type == "torus") {
        const double minor = o.radius2 > 0 ? o.radius2 : o.radius / 4;
        m = torus(o.radius, minor, seg, std::max(seg / 2, 3));
        transform(m, Mat4::translate({0, 0, minor}));
    } else if (o.type == "plane") {
        const double w = o.size.x > 0 ? o.size.x : autoPlane, d = o.size.y > 0 ? o.size.y : autoPlane;
        return plane(w, d);
    } else if (o.type == "mesh") {
        if (!o.mesh) return {};
        m = *o.mesh;
    } else {
        throw std::runtime_error("unknown object type '" + o.type + "'");
    }
    if (!m.hasNormals() || !o.smooth) {
        if (o.smooth) {
            computeNormals(m, o.type == "mesh" ? 45.0 : 50.0);
        } else {
            m = unweld(m);
            m.normals.clear();
            computeNormals(m, 0.0);
        }
    }
    return m;
}

}  // namespace

namespace {

std::string trimmed(std::string s) {
    while (!s.empty() && (s.back() == '\r' || s.back() == ' ' || s.back() == '\t')) s.pop_back();
    std::size_t i = 0;
    while (i < s.size() && (s[i] == ' ' || s[i] == '\t')) ++i;
    return s.substr(i);
}

// The file name at the end of a map_* line (options like -bm 0.5 come first).
std::string mapFile(const std::string& rest, double* bumpScale) {
    std::istringstream in(rest);
    std::vector<std::string> words;
    std::string w;
    while (in >> w) words.push_back(w);
    for (std::size_t i = 0; i + 1 < words.size(); ++i) {
        if (words[i] == "-bm" && bumpScale) bumpScale[0] = std::atof(words[i + 1].c_str());
    }
    return words.empty() ? std::string() : words.back();
}

double srgb(double c) { return c <= 0.04045 ? c / 12.92 : std::pow((c + 0.055) / 1.055, 2.4); }

std::map<std::string, Material> readMtl(const fs::path& file) {
    std::map<std::string, Material> out;
    std::ifstream in(file, std::ios::binary);
    if (!in) return out;
    const fs::path dir = file.parent_path();
    auto path = [&](const std::string& f) {
        if (f.empty()) return std::string();
        fs::path p(f);
        if (p.is_relative()) p = dir / p;
        return p.lexically_normal().string();
    };
    Material* m = nullptr;
    std::string line;
    while (std::getline(in, line)) {
        line = trimmed(line);
        if (line.empty() || line[0] == '#') continue;
        const std::size_t sp = line.find_first_of(" \t");
        const std::string key = line.substr(0, sp);
        const std::string rest = sp == std::string::npos ? "" : trimmed(line.substr(sp));
        std::istringstream v(rest);
        if (key == "newmtl") {
            m = &out[rest];
            m->name = rest;
            continue;
        }
        if (!m) continue;
        double a = 0, b = 0, c = 0;
        if (key == "Kd" && (v >> a >> b >> c)) m->color = {srgb(a), srgb(b), srgb(c)};
        else if (key == "Ke" && (v >> a >> b >> c)) {
            const double s = std::max({a, b, c});
            if (s > 0) m->emission = {srgb(a / s), srgb(b / s), srgb(c / s)}, m->emissionStrength = s;
        } else if (key == "Ns" && (v >> a)) m->roughness = std::clamp(std::pow(2.0 / (std::max(a, 0.0) + 2), 0.25), 0.0, 1.0);
        else if (key == "Pr" && (v >> a)) m->roughness = std::clamp(a, 0.0, 1.0);
        else if (key == "Pm" && (v >> a)) m->metallic = std::clamp(a, 0.0, 1.0);
        else if (key == "Ni" && (v >> a)) m->ior = std::clamp(a, 1.0, 3.0);
        else if (key == "d" && (v >> a)) m->opacity = std::clamp(a, 0.0, 1.0);
        else if (key == "Tr" && (v >> a)) m->opacity = std::clamp(1.0 - a, 0.0, 1.0);
        else if (key == "map_Kd") m->texture = path(mapFile(rest, nullptr));
        else if (key == "map_Pr") m->roughnessMap = path(mapFile(rest, nullptr));
        else if (key == "map_Bump" || key == "map_bump" || key == "bump" || key == "norm" || key == "map_Kn") {
            double scale[1] = {1.0};
            m->normalMap = path(mapFile(rest, scale));
            m->normalStrength = std::clamp(scale[0], 0.0, 4.0);
        }
    }
    return out;
}

}  // namespace

mesh::Mesh loadMeshFile(const fs::path& file, std::vector<Material>& parts, std::vector<int>& partOf) {
    parts.clear();
    partOf.clear();
    std::string ext = file.extension().string();
    for (auto& ch : ext) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    if (ext != ".obj") return mesh::load(file);
    std::ifstream in(file, std::ios::binary);
    if (!in) throw std::runtime_error("can't open " + file.string());
    std::stringstream ss;
    ss << in.rdbuf();
    const std::string text = ss.str();
    // split faces by usemtl, keeping every vertex line so indices still match
    std::string shared;
    std::vector<std::string> groupNames;
    std::vector<std::string> groupFaces;
    std::vector<std::string> libs;
    int cur = -1;
    std::istringstream lines(text);
    std::string line;
    while (std::getline(lines, line)) {
        const std::string tl = trimmed(line);
        if (tl.rfind("usemtl", 0) == 0) {
            const std::string name = trimmed(tl.substr(6));
            auto it = std::find(groupNames.begin(), groupNames.end(), name);
            if (it == groupNames.end()) {
                groupNames.push_back(name);
                groupFaces.emplace_back();
                cur = static_cast<int>(groupNames.size()) - 1;
            } else {
                cur = static_cast<int>(it - groupNames.begin());
            }
            continue;
        }
        if (tl.rfind("mtllib", 0) == 0) {
            libs.push_back(trimmed(tl.substr(6)));
            continue;
        }
        if (tl.rfind("f ", 0) == 0 || tl.rfind("f\t", 0) == 0) {
            if (cur < 0) {
                groupNames.emplace_back();
                groupFaces.emplace_back();
                cur = 0;
            }
            groupFaces[static_cast<std::size_t>(cur)] += tl + "\n";
            shared += "#\n";  // keeps negative indices pointing at the same vertices
            continue;
        }
        shared += tl + "\n";
    }
    if (groupNames.size() < 2) return mesh::load(file);
    std::map<std::string, Material> lib;
    for (const auto& l : libs) {
        fs::path p(l);
        if (p.is_relative()) p = file.parent_path() / p;
        for (auto& [k, m] : readMtl(p)) lib.emplace(k, m);
    }
    // negative (relative) indices need the face in its original place, so they fall back to one material
    if (text.find("f -") != std::string::npos) return mesh::load(file);
    mesh::Mesh all;
    for (std::size_t g = 0; g < groupNames.size(); ++g) {
        mesh::Mesh part = mesh::readObj(shared + groupFaces[g]);
        if (part.empty()) continue;
        const auto base = static_cast<std::uint32_t>(all.positions.size());
        const bool keepN = (all.empty() || all.hasNormals()) && part.hasNormals();
        const bool keepT = (all.empty() || all.hasUvs()) && part.hasUvs();
        if (!keepN) all.normals.clear();
        if (!keepT) all.uvs.clear();
        all.positions.insert(all.positions.end(), part.positions.begin(), part.positions.end());
        if (keepN) all.normals.insert(all.normals.end(), part.normals.begin(), part.normals.end());
        if (keepT) all.uvs.insert(all.uvs.end(), part.uvs.begin(), part.uvs.end());
        for (std::uint32_t i : part.indices) all.indices.push_back(base + i);
        Material m;
        auto it = lib.find(groupNames[g]);
        if (it != lib.end()) m = it->second;
        m.name = groupNames[g].empty() ? "default" : groupNames[g];
        partOf.insert(partOf.end(), part.triangleCount(), static_cast<int>(parts.size()));
        parts.push_back(m);
    }
    if (all.empty()) throw std::runtime_error("that mesh has no triangles");
    if (parts.size() < 2) {
        if (parts.size() == 1) {
            parts.clear();
            partOf.clear();
        }
    }
    return all;
}

void centerMesh(mesh::Mesh& m) {
    const mesh::Bounds b = mesh::bounds(m);
    if (b.empty()) return;
    const Vec3 c = b.center();
    mesh::transform(m, mesh::Mat4::translate({-c.x, -c.y, -b.min.z}));
}

mesh::Mesh worldMesh(const Object& o) {
    mesh::Mesh m = objectMesh(o, 20);
    mesh::transform(m, o.matrix());
    return m;
}

namespace detail {
mesh::Mesh objectSpaceMesh(const Object& o, double autoPlane) { return objectMesh(o, autoPlane); }
}  // namespace detail

mesh::Bounds sceneBounds(const Scene& s) {
    mesh::Bounds b, planes;
    for (const auto& o : s.objects) {
        if (!o.visible) continue;
        if (o.type == "plane") {
            if (o.size.x > 0) planes.merge(mesh::bounds(worldMesh(o)));
            continue;
        }
        b.merge(mesh::bounds(worldMesh(o)));
    }
    return b.empty() ? planes : b;
}

void fitCamera(const Scene& s, Camera& cam) {
    mesh::Bounds b = sceneBounds(s);
    if (b.empty()) {
        b.add({-1, -1, 0});
        b.add({1, 1, 1});
    }
    Vec3 dir = mesh::normalize(cam.position - cam.target);
    if (mesh::length(dir) == 0) dir = mesh::normalize(Vec3{6, -8, 5});
    const double aspect = s.settings.height > 0 ? static_cast<double>(s.settings.width) / s.settings.height : 1.0;
    const double r = std::max(b.diagonal() / 2, 1e-6);
    cam.target = b.center();
    if (cam.orthographic) {
        cam.orthoSize = 2 * r * 1.08 / std::min(1.0, aspect);
        cam.position = cam.target + dir * (r * 4);
    } else {
        const double halfV = cam.fov * mesh::kPi / 360.0;
        const double halfH = std::atan(std::tan(halfV) * aspect);
        const double half = std::min(halfV, halfH);
        cam.position = cam.target + dir * (r / std::sin(half) * 1.04);
    }
}

// ---- JSON ----

namespace {

json vec(const Vec3& v) { return json::array({v.x, v.y, v.z}); }

Vec3 getVec(const json& j, const char* key, const Vec3& fallback) {
    if (!j.contains(key)) return fallback;
    const auto& a = j.at(key);
    if (a.is_number()) {
        const double s = a.get<double>();
        return {s, s, s};
    }
    if (!a.is_array() || a.size() != 3) throw std::runtime_error(std::string("'") + key + "' should be [x, y, z]");
    return {a[0].get<double>(), a[1].get<double>(), a[2].get<double>()};
}

Vec3 getColor(const json& j, const char* key, const Vec3& fallback) {
    if (!j.contains(key)) return fallback;
    const auto& a = j.at(key);
    if (a.is_string()) {
        auto c = parseColor(a.get<std::string>());
        if (!c) throw std::runtime_error("bad colour '" + a.get<std::string>() + "'");
        return *c;
    }
    return getVec(j, key, fallback);
}

template <typename T>
T get(const json& j, const char* key, T fallback) {
    return j.contains(key) ? j.at(key).get<T>() : fallback;
}

std::string relPath(const std::string& p, const fs::path& base) {
    if (p.empty() || base.empty()) return p;
    std::error_code ec;
    fs::path abs = fs::absolute(p, ec);
    fs::path b = fs::absolute(base, ec);
    fs::path rel = abs.lexically_relative(b);
    if (rel.empty() || rel.native().rfind(fs::path("..").native(), 0) == 0) return abs.generic_string();
    return rel.generic_string();
}

std::string resolve(const std::string& p, const fs::path& base) {
    if (p.empty()) return p;
    fs::path f(p);
    if (f.is_absolute() || base.empty()) return f.string();
    return (base / f).lexically_normal().string();
}

json materialJson(const Material& m, const fs::path& base) {
    json j = {{"color", hexColor(m.color)}, {"roughness", m.roughness}, {"metallic", m.metallic}};
    if (m.emissionStrength > 0) j["emission"] = hexColor(m.emission), j["emission_strength"] = m.emissionStrength;
    if (m.transmission > 0) j["transmission"] = m.transmission, j["ior"] = m.ior;
    if (m.opacity < 1) j["opacity"] = m.opacity;
    if (!m.texture.empty()) j["texture"] = relPath(m.texture, base), j["texture_scale"] = m.textureScale;
    if (!m.normalMap.empty()) j["normal_map"] = relPath(m.normalMap, base), j["normal_strength"] = m.normalStrength;
    if (!m.roughnessMap.empty()) j["roughness_map"] = relPath(m.roughnessMap, base);
    if (!m.name.empty()) j["name"] = m.name;
    return j;
}

Material materialFrom(const json& j, const fs::path& base) {
    Material m;
    if (j.contains("preset")) applyPreset(m, j.at("preset").get<std::string>());
    m.color = getColor(j, "color", m.color);
    m.roughness = std::clamp(get(j, "roughness", m.roughness), 0.0, 1.0);
    m.metallic = std::clamp(get(j, "metallic", m.metallic), 0.0, 1.0);
    m.emission = getColor(j, "emission", m.emission);
    m.emissionStrength = std::max(0.0, get(j, "emission_strength", m.emissionStrength));
    m.transmission = std::clamp(get(j, "transmission", m.transmission), 0.0, 1.0);
    m.ior = std::clamp(get(j, "ior", m.ior), 1.0, 3.0);
    m.opacity = std::clamp(get(j, "opacity", m.opacity), 0.0, 1.0);
    m.texture = resolve(get<std::string>(j, "texture", ""), base);
    m.textureScale = get(j, "texture_scale", m.textureScale);
    m.normalMap = resolve(get<std::string>(j, "normal_map", ""), base);
    m.normalStrength = std::clamp(get(j, "normal_strength", m.normalStrength), 0.0, 4.0);
    m.roughnessMap = resolve(get<std::string>(j, "roughness_map", ""), base);
    m.name = get<std::string>(j, "name", "");
    return m;
}

}  // namespace

namespace {

// dump(2) puts every number of [x, y, z] on its own line; put short number arrays back on one.
std::string compactArrays(const std::string& in) {
    std::string out;
    out.reserve(in.size());
    for (std::size_t i = 0; i < in.size(); ++i) {
        if (in[i] == '[') {
            const std::size_t close = in.find(']', i);
            if (close != std::string::npos) {
                const std::string inner = in.substr(i + 1, close - i - 1);
                if (inner.find_first_not_of("0123456789.-+eE, \n") == std::string::npos) {
                    std::string flat = "[";
                    bool space = false;
                    for (char c : inner) {
                        if (c == '\n' || c == ' ') {
                            space = true;
                            continue;
                        }
                        if (space && flat.size() > 1 && flat.back() == ',') flat += ' ';
                        space = false;
                        flat += c;
                    }
                    out += flat + "]";
                    i = close;
                    continue;
                }
            }
        }
        out += in[i];
    }
    return out;
}

}  // namespace

std::string toJson(const Scene& s, const fs::path& base) {
    json j;
    j["format"] = "wetwaresuite-scene";
    j["version"] = 1;
    const Camera& c = s.camera;
    j["camera"] = {{"position", vec(c.position)}, {"target", vec(c.target)}, {"up", vec(c.up)},   {"fov", c.fov},
                   {"orthographic", c.orthographic}, {"ortho_size", c.orthoSize},           {"auto", c.autoFit}};
    const Settings& st = s.settings;
    j["settings"] = {{"width", st.width},     {"height", st.height},     {"samples", st.samples}, {"bounces", st.bounces},
                     {"denoise", st.denoise}, {"exposure", st.exposure}, {"mode", st.mode}};
    const Sky& k = s.sky;
    j["sky"] = {{"type", k.type},         {"zenith", hexColor(k.zenith)}, {"horizon", hexColor(k.horizon)},
                {"ground", hexColor(k.ground)}, {"strength", k.strength},  {"visible", k.visible},
                {"background", hexColor(k.background)}, {"transparent", k.transparent}};
    if (!k.hdri.empty()) j["sky"]["hdri"] = relPath(k.hdri, base), j["sky"]["rotation"] = k.rotation;
    json objs = json::array();
    for (const auto& o : s.objects) {
        json jo = {{"name", o.name}, {"type", o.type}};
        if (o.type == "box" || o.type == "plane") jo["size"] = vec(o.size);
        if (o.type == "sphere" || o.type == "cylinder" || o.type == "cone" || o.type == "torus") jo["radius"] = o.radius;
        if (o.type == "cone" || o.type == "torus") jo["radius2"] = o.radius2;
        if (o.type == "cylinder" || o.type == "cone") jo["height"] = o.height;
        if (o.type != "box" && o.type != "plane" && o.type != "mesh") jo["segments"] = o.segments;
        if (o.type == "mesh") jo["path"] = relPath(o.path, base), jo["center"] = o.centered;
        jo["position"] = vec(o.position);
        jo["rotation"] = vec(o.rotation);
        jo["scale"] = vec(o.scale);
        if (!o.visible) jo["visible"] = false;
        if (!o.smooth) jo["smooth"] = false;
        jo["material"] = materialJson(o.material, base);
        if (!o.parts.empty()) {
            json parts = json::array();
            for (const auto& m : o.parts) parts.push_back(materialJson(m, base));
            jo["parts"] = parts;
        }
        objs.push_back(jo);
    }
    j["objects"] = objs;
    json lights = json::array();
    for (const auto& l : s.lights) {
        json jl = {{"name", l.name}, {"type", l.type}, {"position", vec(l.position)}};
        if (l.type != "point") jl["target"] = vec(l.target);
        jl["color"] = hexColor(l.color);
        jl["intensity"] = l.intensity;
        jl["radius"] = l.radius;
        if (l.type == "area") jl["width"] = l.width, jl["height"] = l.height;
        if (!l.shadows) jl["shadows"] = false;
        lights.push_back(jl);
    }
    j["lights"] = lights;
    return compactArrays(j.dump(2)) + "\n";
}

Scene fromJson(const std::string& text, const fs::path& base) {
    json j;
    try {
        j = json::parse(text);
    } catch (const std::exception& e) {
        throw std::runtime_error(std::string("not valid JSON: ") + e.what());
    }
    if (!j.is_object()) throw std::runtime_error("a scene file is a JSON object");
    Scene s;
    try {
        if (j.contains("camera")) {
            const auto& c = j["camera"];
            s.camera.position = getVec(c, "position", s.camera.position);
            s.camera.target = getVec(c, "target", s.camera.target);
            s.camera.up = getVec(c, "up", s.camera.up);
            s.camera.fov = std::clamp(get(c, "fov", s.camera.fov), 1.0, 170.0);
            s.camera.orthographic = get(c, "orthographic", false);
            s.camera.orthoSize = get(c, "ortho_size", s.camera.orthoSize);
            s.camera.autoFit = get(c, "auto", !c.contains("position"));
        }
        if (j.contains("settings")) {
            const auto& st = j["settings"];
            s.settings.width = std::clamp(get(st, "width", s.settings.width), 8, 8192);
            s.settings.height = std::clamp(get(st, "height", s.settings.height), 8, 8192);
            s.settings.samples = std::clamp(get(st, "samples", s.settings.samples), 1, 100000);
            s.settings.bounces = std::clamp(get(st, "bounces", s.settings.bounces), 0, 64);
            s.settings.denoise = get(st, "denoise", s.settings.denoise);
            s.settings.exposure = get(st, "exposure", s.settings.exposure);
            s.settings.mode = get<std::string>(st, "mode", s.settings.mode);
            if (s.settings.mode != "path" && s.settings.mode != "preview") s.settings.mode = "path";
        }
        if (j.contains("sky")) {
            const auto& k = j["sky"];
            s.sky.type = get<std::string>(k, "type", s.sky.type);
            if (s.sky.type != "gradient" && s.sky.type != "color" && s.sky.type != "none" && s.sky.type != "hdri") {
                throw std::runtime_error("sky type is gradient, color, none or hdri");
            }
            s.sky.hdri = resolve(get<std::string>(k, "hdri", ""), base);
            s.sky.rotation = get(k, "rotation", 0.0);
            if (s.sky.type == "hdri" && s.sky.hdri.empty()) s.sky.type = "gradient";
            if (k.contains("color")) s.sky.zenith = s.sky.horizon = getColor(k, "color", s.sky.zenith);
            s.sky.zenith = getColor(k, "zenith", s.sky.zenith);
            s.sky.horizon = getColor(k, "horizon", s.sky.horizon);
            s.sky.ground = getColor(k, "ground", s.sky.ground);
            s.sky.strength = std::max(0.0, get(k, "strength", s.sky.strength));
            s.sky.visible = get(k, "visible", s.sky.visible);
            s.sky.background = getColor(k, "background", s.sky.background);
            s.sky.transparent = get(k, "transparent", s.sky.transparent);
        }
        for (const auto& jo : j.value("objects", json::array())) {
            Object o;
            o.name = get<std::string>(jo, "name", "");
            o.type = get<std::string>(jo, "type", "box");
            if (std::find(objectTypes().begin(), objectTypes().end(), o.type) == objectTypes().end()) {
                throw std::runtime_error("unknown object type '" + o.type + "'");
            }
            if (o.name.empty() || s.findObject(o.name) || s.findLight(o.name)) o.name = s.uniqueName(o.type);
            o.size = getVec(jo, "size", o.size);
            o.radius = get(jo, "radius", o.radius);
            o.radius2 = get(jo, "radius2", o.radius2);
            o.height = get(jo, "height", o.height);
            o.segments = std::clamp(get(jo, "segments", o.segments), 3, 512);
            o.position = getVec(jo, "position", o.position);
            o.rotation = getVec(jo, "rotation", o.rotation);
            o.scale = getVec(jo, "scale", o.scale);
            o.visible = get(jo, "visible", true);
            o.smooth = get(jo, "smooth", true);
            if (jo.contains("material")) o.material = materialFrom(jo["material"], base);
            if (o.type == "mesh") {
                o.path = resolve(get<std::string>(jo, "path", ""), base);
                if (o.path.empty()) throw std::runtime_error("mesh object '" + o.name + "' has no path");
                try {
                    std::vector<int> partOf;
                    mesh::Mesh m = loadMeshFile(o.path, o.parts, partOf);
                    o.centered = get(jo, "center", false);
                    if (o.centered) centerMesh(m);
                    o.mesh = std::make_shared<const mesh::Mesh>(std::move(m));
                    if (!o.parts.empty()) {
                        o.partOf = std::make_shared<const std::vector<int>>(std::move(partOf));
                        // saved edits win over the .mtl, matched by name
                        for (const auto& jp : jo.value("parts", json::array())) {
                            Material pm = materialFrom(jp, base);
                            for (auto& part : o.parts) {
                                if (part.name == pm.name) part = pm;
                            }
                        }
                    }
                } catch (const std::exception& e) {
                    throw std::runtime_error("object '" + o.name + "': " + e.what());
                }
            }
            s.objects.push_back(std::move(o));
        }
        for (const auto& jl : j.value("lights", json::array())) {
            Light l;
            l.type = get<std::string>(jl, "type", "point");
            if (std::find(lightTypes().begin(), lightTypes().end(), l.type) == lightTypes().end()) {
                throw std::runtime_error("unknown light type '" + l.type + "'");
            }
            l.name = get<std::string>(jl, "name", "");
            if (l.name.empty() || s.findObject(l.name) || s.findLight(l.name)) l.name = s.uniqueName(l.type);
            l.position = getVec(jl, "position", l.position);
            l.target = getVec(jl, "target", l.target);
            if (jl.contains("direction")) l.target = l.position + getVec(jl, "direction", {0, 0, -1});
            l.color = getColor(jl, "color", l.color);
            l.intensity = std::max(0.0, get(jl, "intensity", l.type == "sun" ? 4.0 : l.intensity));
            l.radius = std::max(0.0, get(jl, "radius", l.type == "sun" ? 2.0 : l.radius));
            l.width = std::max(1e-6, get(jl, "width", l.width));
            l.height = std::max(1e-6, get(jl, "height", l.height));
            l.shadows = get(jl, "shadows", true);
            s.lights.push_back(l);
        }
    } catch (const json::exception& e) {
        throw std::runtime_error(std::string("bad scene file: ") + e.what());
    }
    return s;
}

void saveScene(Scene& s, const fs::path& path) {
    const fs::path dir = path.parent_path();
    std::error_code ec;
    if (!dir.empty()) fs::create_directories(dir, ec);
    std::string stem = path.filename().string();
    for (const char* suffix : {".scene.json", ".json"}) {
        const std::string suf = suffix;
        if (stem.size() > suf.size() && stem.compare(stem.size() - suf.size(), suf.size(), suf) == 0) {
            stem.resize(stem.size() - suf.size());
            break;
        }
    }
    for (auto& o : s.objects) {
        if (o.type != "mesh" || !o.mesh) continue;
        if (!o.path.empty() && fs::exists(o.path, ec)) continue;
        const fs::path side = (dir.empty() ? fs::path(".") : dir) / (stem + "." + o.name + ".glb");
        mesh::save(side, *o.mesh);
        o.path = side.string();
        o.centered = false;
    }
    std::ofstream out(path, std::ios::binary);
    if (!out) throw std::runtime_error("can't write " + path.string());
    out << toJson(s, dir.empty() ? fs::current_path(ec) : dir);
    if (!out) throw std::runtime_error("couldn't finish writing " + path.string());
}

Scene loadScene(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) throw std::runtime_error("can't open " + path.string());
    std::stringstream ss;
    ss << in.rdbuf();
    std::error_code ec;
    fs::path dir = path.parent_path();
    if (dir.empty()) dir = fs::current_path(ec);
    return fromJson(ss.str(), dir);
}

}  // namespace wetwaresuite::render
