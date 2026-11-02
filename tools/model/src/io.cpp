// Model files: .wwmodel (JSON, everything kept), OBJ with polygons, UVs,
// smoothing, objects and materials, and the other mesh formats through
// libs/mesh (triangulated).
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <stdexcept>

#include <nlohmann/json.hpp>

#include "wetwaresuite/mesh/io.hpp"
#include "wetwaresuite/model/model.hpp"

namespace wetwaresuite::model {

namespace fs = std::filesystem;
using json = nlohmann::ordered_json;

namespace {

std::string lowerExt(const fs::path& p) {
    std::string e = p.extension().string();
    if (!e.empty() && e[0] == '.') e.erase(0, 1);
    std::transform(e.begin(), e.end(), e.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return e;
}

std::string readFile(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) throw std::runtime_error("can't open " + path.string());
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

void writeFile(const fs::path& path, const std::string& bytes) {
    std::ofstream out(path, std::ios::binary);
    if (!out) throw std::runtime_error("can't write " + path.string());
    out << bytes;
    if (!out) throw std::runtime_error("failed writing " + path.string());
}

json vec(const Vec3& v) { return json::array({v.x, v.y, v.z}); }
Vec3 getVec(const json& j, const char* key, const Vec3& fallback) {
    if (!j.contains(key)) return fallback;
    const auto& a = j.at(key);
    if (!a.is_array() || a.size() != 3) throw std::runtime_error(std::string("'") + key + "' needs three numbers");
    return {a[0].get<double>(), a[1].get<double>(), a[2].get<double>()};
}

json edgeList(const std::set<EdgeKey>& s) {
    json a = json::array();
    for (auto k : s) {
        a.push_back(keyA(k));
        a.push_back(keyB(k));
    }
    return a;
}

std::set<EdgeKey> readEdges(const json& j, const char* key, std::size_t nverts) {
    std::set<EdgeKey> s;
    if (!j.contains(key)) return s;
    const auto& a = j.at(key);
    for (std::size_t i = 0; i + 1 < a.size(); i += 2) {
        const auto x = a[i].get<std::uint32_t>(), y = a[i + 1].get<std::uint32_t>();
        if (x < nverts && y < nverts && x != y) s.insert(edgeKey(x, y));
    }
    return s;
}

json meshJson(const PolyMesh& m) {
    json j;
    json verts = json::array(), vsel = json::array(), colors = json::array();
    for (std::size_t i = 0; i < m.verts.size(); ++i) {
        const auto& v = m.verts[i];
        verts.push_back(v.p.x);
        verts.push_back(v.p.y);
        verts.push_back(v.p.z);
        if (v.sel) vsel.push_back(i);
        if (m.colors) {
            colors.push_back(v.color.x);
            colors.push_back(v.color.y);
            colors.push_back(v.color.z);
        }
    }
    j["verts"] = verts;
    if (!vsel.empty()) j["vsel"] = vsel;
    if (m.colors) j["colors"] = colors;
    json faces = json::array();
    for (const auto& f : m.faces) {
        json jf;
        jf["v"] = f.v;
        if (!f.uv.empty()) {
            json uv = json::array();
            for (const auto& t : f.uv) {
                uv.push_back(t.x);
                uv.push_back(t.y);
            }
            jf["uv"] = uv;
        }
        if (f.material) jf["m"] = f.material;
        if (f.smooth) jf["s"] = true;
        if (f.sel) jf["sel"] = true;
        faces.push_back(jf);
    }
    j["faces"] = faces;
    if (!m.loose.empty()) {
        json l = json::array();
        for (const auto& e : m.loose) {
            l.push_back(e[0]);
            l.push_back(e[1]);
        }
        j["loose"] = l;
    }
    if (!m.esel.empty()) j["esel"] = edgeList(m.esel);
    if (!m.seams.empty()) j["seams"] = edgeList(m.seams);
    if (!m.sharp.empty()) j["sharp"] = edgeList(m.sharp);
    return j;
}

PolyMesh meshFrom(const json& j) {
    PolyMesh m;
    const auto& verts = j.at("verts");
    for (std::size_t i = 0; i + 2 < verts.size(); i += 3)
        m.addVertex({verts[i].get<double>(), verts[i + 1].get<double>(), verts[i + 2].get<double>()});
    const std::size_t n = m.verts.size();
    if (j.contains("vsel"))
        for (const auto& i : j.at("vsel"))
            if (i.get<std::size_t>() < n) m.verts[i.get<std::size_t>()].sel = true;
    if (j.contains("colors")) {
        m.colors = true;
        const auto& c = j.at("colors");
        for (std::size_t i = 0; i < n && i * 3 + 2 < c.size(); ++i)
            m.verts[i].color = {c[i * 3].get<double>(), c[i * 3 + 1].get<double>(), c[i * 3 + 2].get<double>()};
    }
    for (const auto& jf : j.at("faces")) {
        Face f;
        f.v = jf.at("v").get<std::vector<std::uint32_t>>();
        for (auto v : f.v)
            if (v >= n) throw std::runtime_error("face refers to a missing vertex");
        if (jf.contains("uv")) {
            const auto& uv = jf.at("uv");
            for (std::size_t i = 0; i + 1 < uv.size(); i += 2) f.uv.push_back({uv[i].get<double>(), uv[i + 1].get<double>()});
            if (f.uv.size() != f.v.size()) f.uv.clear();
        }
        f.material = jf.value("m", 0);
        f.smooth = jf.value("s", false);
        f.sel = jf.value("sel", false);
        m.faces.push_back(std::move(f));
    }
    if (j.contains("loose")) {
        const auto& l = j.at("loose");
        for (std::size_t i = 0; i + 1 < l.size(); i += 2) {
            const auto a = l[i].get<std::uint32_t>(), b = l[i + 1].get<std::uint32_t>();
            if (a < n && b < n) m.loose.push_back({a, b});
        }
    }
    m.esel = readEdges(j, "esel", n);
    m.seams = readEdges(j, "seams", n);
    m.sharp = readEdges(j, "sharp", n);
    std::string why;
    if (!valid(m, &why)) throw std::runtime_error("bad mesh: " + why);
    return m;
}

json keysJson(const Object& o) {
    json k = json::object();
    for (const auto& [ch, keys] : o.keys) {
        json a = json::array();
        for (const auto& key : keys) a.push_back({{"frame", key.frame}, {"value", key.value}, {"interp", key.interp}});
        k[ch] = a;
    }
    return k;
}

}  // namespace

std::string toJson(const ModelDoc& doc) {
    const Scene& s = doc.scene;
    json j;
    j["format"] = "wwmodel";
    j["version"] = 1;
    j["scene"] = {{"active", s.active},       {"selectMode", toString(s.selectMode)}, {"editMode", s.editMode},
                  {"cursor", vec(s.cursor)},  {"frame", s.frame},                     {"frameStart", s.frameStart},
                  {"frameEnd", s.frameEnd},   {"fps", s.fps}};
    j["world"] = {{"type", s.worldType}, {"color", vec(s.worldColor)}, {"hdri", s.hdri}, {"rotation", s.hdriRotation},
                  {"strength", s.worldStrength}, {"visible", s.worldVisible}};
    json cols = json::array();
    for (const auto& c : s.collections) cols.push_back({{"name", c.name}, {"visible", c.visible}});
    j["collections"] = cols;
    json mats = json::array();
    for (const auto& m : s.materials) {
        mats.push_back({{"name", m.name},
                        {"color", vec(m.color)},
                        {"roughness", m.roughness},
                        {"metallic", m.metallic},
                        {"emission", vec(m.emission)},
                        {"emissionStrength", m.emissionStrength},
                        {"transmission", m.transmission},
                        {"ior", m.ior},
                        {"alpha", m.alpha},
                        {"texture", m.texture},
                        {"textureScale", m.textureScale},
                        {"normalMap", m.normalMap},
                        {"normalStrength", m.normalStrength},
                        {"roughnessMap", m.roughnessMap}});
    }
    j["materials"] = mats;
    json meshes = json::object();
    for (const auto& [name, m] : s.meshes)
        if (m) meshes[name] = meshJson(*m);
    j["meshes"] = meshes;
    json objs = json::array();
    for (const auto& o : s.objects) {
        json jo{{"name", o.name},          {"type", o.type},         {"data", o.data},
                {"parent", o.parent},      {"collection", o.collection}, {"location", vec(o.location)},
                {"rotation", vec(o.rotation)}, {"scale", vec(o.scale)}, {"visible", o.visible},
                {"selected", o.selected},  {"materials", o.materials}, {"autoSmooth", o.autoSmooth}};
        json mods = json::array();
        for (const auto& m : o.modifiers)
            mods.push_back({{"type", m.type}, {"name", m.name}, {"enabled", m.enabled}, {"num", m.num}, {"text", m.text}});
        jo["modifiers"] = mods;
        if (!o.keys.empty()) jo["keys"] = keysJson(o);
        if (o.type == "light") {
            jo["lightType"] = o.lightType;
            jo["lightColor"] = vec(o.lightColor);
            jo["power"] = o.power;
            jo["size"] = o.size;
        }
        if (o.type == "camera") jo["fov"] = o.fov;
        if (!o.bones.empty()) {
            json bones = json::array();
            for (const auto& b : o.bones)
                bones.push_back({{"name", b.name}, {"parent", b.parent}, {"head", vec(b.head)}, {"tail", vec(b.tail)}, {"roll", b.roll},
                                 {"rotation", vec(b.rotation)}, {"location", vec(b.location)}});
            jo["bones"] = bones;
        }
        if (!o.weights.empty()) jo["weights"] = o.weights;
        objs.push_back(jo);
    }
    j["objects"] = objs;
    return j.dump(1);
}

void fromJson(ModelDoc& doc, const std::string& text) {
    json j;
    try {
        j = json::parse(text);
    } catch (const std::exception& e) {
        throw std::runtime_error(std::string("not a model file: ") + e.what());
    }
    if (!j.is_object() || j.value("format", "") != "wwmodel") throw std::runtime_error("not a .wwmodel file");
    try {
        Scene s;
        const json& js = j.value("scene", json::object());
        s.active = js.value("active", "");
        s.selectMode = parseSelectMode(js.value("selectMode", "vert")).value_or(SelectMode::Vertex);
        s.editMode = js.value("editMode", false);
        s.cursor = getVec(js, "cursor", {0, 0, 0});
        s.frame = js.value("frame", 1);
        s.frameStart = js.value("frameStart", 1);
        s.frameEnd = js.value("frameEnd", 48);
        s.fps = js.value("fps", 24.0);
        if (j.contains("world")) {
            const json& w = j["world"];
            s.worldType = w.value("type", s.worldType);
            s.worldColor = getVec(w, "color", s.worldColor);
            s.hdri = w.value("hdri", "");
            s.hdriRotation = w.value("rotation", 0.0);
            s.worldStrength = w.value("strength", s.worldStrength);
            s.worldVisible = w.value("visible", false);
        }
        if (j.contains("collections")) {
            s.collections.clear();
            for (const auto& c : j.at("collections")) s.collections.push_back({c.value("name", "Collection"), c.value("visible", true)});
        }
        for (const auto& jm : j.value("materials", json::array())) {
            Material m;
            m.name = jm.value("name", "Material");
            m.color = getVec(jm, "color", m.color);
            m.roughness = jm.value("roughness", m.roughness);
            m.metallic = jm.value("metallic", m.metallic);
            m.emission = getVec(jm, "emission", m.emission);
            m.emissionStrength = jm.value("emissionStrength", m.emissionStrength);
            m.transmission = jm.value("transmission", m.transmission);
            m.ior = jm.value("ior", m.ior);
            m.alpha = jm.value("alpha", m.alpha);
            m.texture = jm.value("texture", "");
            m.textureScale = jm.value("textureScale", 1.0);
            m.normalMap = jm.value("normalMap", "");
            m.normalStrength = jm.value("normalStrength", 1.0);
            m.roughnessMap = jm.value("roughnessMap", "");
            s.materials.push_back(m);
        }
        const json meshes = j.value("meshes", json::object());
        for (const auto& [name, jm] : meshes.items())
            s.meshes[name] = std::make_shared<PolyMesh>(meshFrom(jm));
        for (const auto& jo : j.value("objects", json::array())) {
            Object o;
            o.name = jo.at("name").get<std::string>();
            o.type = jo.value("type", "mesh");
            o.data = jo.value("data", "");
            o.parent = jo.value("parent", "");
            o.collection = jo.value("collection", "Collection");
            o.location = getVec(jo, "location", o.location);
            o.rotation = getVec(jo, "rotation", o.rotation);
            o.scale = getVec(jo, "scale", o.scale);
            o.visible = jo.value("visible", true);
            o.selected = jo.value("selected", false);
            o.materials = jo.value("materials", std::vector<std::string>{});
            o.autoSmooth = jo.value("autoSmooth", 180.0);
            for (const auto& jm : jo.value("modifiers", json::array())) {
                Modifier m;
                m.type = jm.value("type", "");
                m.name = jm.value("name", m.type);
                m.enabled = jm.value("enabled", true);
                m.num = jm.value("num", std::map<std::string, double>{});
                m.text = jm.value("text", std::map<std::string, std::string>{});
                o.modifiers.push_back(m);
            }
            const json keyed = jo.value("keys", json::object());
            for (const auto& [ch, keys] : keyed.items()) {
                for (const auto& k : keys) o.keys[ch].push_back({k.value("frame", 1), k.value("value", 0.0), k.value("interp", "bezier")});
                std::sort(o.keys[ch].begin(), o.keys[ch].end(), [](const Keyframe& a, const Keyframe& b) { return a.frame < b.frame; });
            }
            o.lightType = jo.value("lightType", "point");
            o.lightColor = getVec(jo, "lightColor", o.lightColor);
            o.power = jo.value("power", 1000.0);
            o.size = jo.value("size", 0.1);
            o.fov = jo.value("fov", 40.0);
            for (const auto& jb : jo.value("bones", json::array())) {
                Bone b;
                b.name = jb.value("name", "Bone");
                b.parent = jb.value("parent", "");
                b.head = getVec(jb, "head", b.head);
                b.tail = getVec(jb, "tail", b.tail);
                b.roll = jb.value("roll", 0.0);
                b.rotation = getVec(jb, "rotation", b.rotation);
                b.location = getVec(jb, "location", b.location);
                o.bones.push_back(b);
            }
            o.weights = jo.value("weights", std::map<std::string, std::vector<float>>{});
            if (o.type == "mesh" && !s.meshes.count(o.data)) throw std::runtime_error("object '" + o.name + "' uses a missing mesh");
            s.objects.push_back(std::move(o));
        }
        doc.scene = std::move(s);
    } catch (const nlohmann::json::exception& e) {
        throw std::runtime_error(std::string("bad model file: ") + e.what());
    }
    doc.refresh();
}

void saveModel(ModelDoc& doc, const fs::path& path) {
    writeFile(path, toJson(doc));
    doc.path = path.string();
}

std::shared_ptr<ModelDoc> loadModel(const fs::path& path) {
    auto doc = std::make_shared<ModelDoc>();
    fromJson(*doc, readFile(path));
    doc->path = path.string();
    return doc;
}

// ---- export ----

namespace {

std::vector<const Object*> exported(const ModelDoc& doc, const ExportOptions& o) {
    std::vector<const Object*> out;
    for (const auto& obj : doc.scene.objects) {
        if (!doc.meshOf(obj)) continue;
        if (!o.objects.empty()) {
            if (std::find(o.objects.begin(), o.objects.end(), obj.name) == o.objects.end()) continue;
        } else {
            if (!doc.shown(obj)) continue;
            if (o.selectedOnly && !obj.selected) continue;
        }
        out.push_back(&obj);
    }
    for (const auto& name : o.objects)
        if (!doc.meshOf(doc.object(name) ? *doc.object(name) : Object{}))
            throw std::runtime_error("no mesh object called '" + name + "'");
    return out;
}

PolyMesh worldPoly(const ModelDoc& doc, const Object& o, bool modifiers) {
    PolyMesh m = modifiers ? evaluate(doc, o) : *doc.meshOf(o);
    transform(m, doc.world(o));
    return m;
}

std::string fmt(double v) {
    char buf[32];
    std::snprintf(buf, sizeof buf, "%.6g", std::fabs(v) < 1e-12 ? 0.0 : v);
    return buf;
}

std::string objName(const std::string& s) {
    std::string out = s;
    for (auto& c : out)
        if (std::isspace(static_cast<unsigned char>(c))) c = '_';
    return out.empty() ? "None" : out;
}

std::string writeMtl(const ModelDoc& doc, const std::set<std::string>& used) {
    std::string out = "# wetwaresuite model materials\n";
    for (const auto& m : doc.scene.materials) {
        if (!used.count(m.name)) continue;
        out += "\nnewmtl " + objName(m.name) + "\n";
        out += "Kd " + fmt(m.color.x) + " " + fmt(m.color.y) + " " + fmt(m.color.z) + "\n";
        const double ns = std::clamp(2.0 / std::max(1e-4, m.roughness * m.roughness * m.roughness * m.roughness) - 2, 0.0, 1000.0);
        out += "Ns " + fmt(ns) + "\nKs 0.5 0.5 0.5\n";
        out += "Pr " + fmt(m.roughness) + "\nPm " + fmt(m.metallic) + "\n";
        if (m.emissionStrength > 0)
            out += "Ke " + fmt(m.emission.x * m.emissionStrength) + " " + fmt(m.emission.y * m.emissionStrength) + " " +
                   fmt(m.emission.z * m.emissionStrength) + "\n";
        if (m.alpha < 1) out += "d " + fmt(m.alpha) + "\n";
        if (m.transmission > 0) out += "Ni " + fmt(m.ior) + "\nillum 7\n";
        if (!m.texture.empty()) out += "map_Kd " + m.texture + "\n";
        if (!m.normalMap.empty()) out += "norm " + m.normalMap + "\n";
        if (!m.roughnessMap.empty()) out += "map_Pr " + m.roughnessMap + "\n";
    }
    return out;
}

std::string exportObj(const ModelDoc& doc, const fs::path& path, const std::vector<const Object*>& objs, const ExportOptions& opt,
                      std::size_t& faces) {
    std::string out = "# wetwaresuite model\n";
    std::set<std::string> used;
    for (const Object* o : objs)
        for (const auto& m : o->materials)
            if (doc.material(m)) used.insert(m);
    const fs::path mtl = fs::path(path).replace_extension(".mtl");
    if (!used.empty()) out += "mtllib " + mtl.filename().string() + "\n";
    std::size_t vbase = 1, tbase = 1;
    for (const Object* o : objs) {
        const PolyMesh m = worldPoly(doc, *o, opt.modifiers);
        out += "o " + objName(o->name) + "\n";
        for (const auto& v : m.verts) {
            out += "v " + fmt(v.p.x) + " " + fmt(v.p.y) + " " + fmt(v.p.z);
            if (m.colors) out += " " + fmt(v.color.x) + " " + fmt(v.color.y) + " " + fmt(v.color.z);
            out += "\n";
        }
        std::size_t tcount = 0;
        for (const auto& f : m.faces)
            for (const auto& t : f.uv) {
                out += "vt " + fmt(t.x) + " " + fmt(t.y) + "\n";
                ++tcount;
            }
        int slot = -1;
        int smooth = -1;
        std::size_t t = tbase;
        for (const auto& f : m.faces) {
            if (f.material != slot) {
                slot = f.material;
                const std::string name = slot >= 0 && static_cast<std::size_t>(slot) < o->materials.size() &&
                                                 doc.material(o->materials[static_cast<std::size_t>(slot)])
                                             ? o->materials[static_cast<std::size_t>(slot)]
                                             : std::string();
                if (!name.empty()) out += "usemtl " + objName(name) + "\n";
            }
            if (static_cast<int>(f.smooth) != smooth) {
                smooth = f.smooth;
                out += smooth ? "s 1\n" : "s off\n";
            }
            out += "f";
            for (std::size_t c = 0; c < f.v.size(); ++c) {
                out += " " + std::to_string(f.v[c] + vbase);
                if (!f.uv.empty()) out += "/" + std::to_string(t++);
            }
            out += "\n";
        }
        for (const auto& e : m.loose) out += "l " + std::to_string(e[0] + vbase) + " " + std::to_string(e[1] + vbase) + "\n";
        vbase += m.verts.size();
        tbase += tcount;
        faces += m.faces.size();
    }
    if (!used.empty()) writeFile(mtl, writeMtl(doc, used));
    return out;
}

}  // namespace

std::string exportFile(const ModelDoc& doc, const fs::path& path, const ExportOptions& opt) {
    const std::string ext = lowerExt(path);
    if (ext == "wwmodel") {
        writeFile(path, toJson(doc));
        return "wrote " + path.string() + " (the whole scene)";
    }
    if ((ext == "glb" || ext == "gltf") && hasSkin(doc) && opt.objects.empty() && !opt.selectedOnly) return exportSkinnedGltf(doc, path, opt.modifiers);
    const auto objs = exported(doc, opt);
    if (objs.empty()) throw std::runtime_error("nothing to export (no visible mesh objects)");
    if (ext == "obj") {
        std::size_t faces = 0;
        writeFile(path, exportObj(doc, path, objs, opt, faces));
        return "wrote " + path.string() + ": " + std::to_string(objs.size()) + (objs.size() == 1 ? " object, " : " objects, ") + std::to_string(faces) + " polygons";
    }
    if (!mesh::canWrite(path)) {
        std::string list = "wwmodel";
        for (const auto& e : mesh::writableExtensions()) list += ", " + e;
        throw std::runtime_error("can't write ." + ext + " (use " + list + ")");
    }
    mesh::Mesh all;
    for (const Object* o : objs) {
        TriangulateOptions to;
        to.autoSmooth = o->autoSmooth;
        mesh::append(all, triangulate(worldPoly(doc, *o, opt.modifiers), to).mesh);
    }
    mesh::SaveOptions so;
    so.ascii = opt.ascii;
    mesh::save(path, all, so);
    return "wrote " + path.string() + ": " + std::to_string(objs.size()) + (objs.size() == 1 ? " object, " : " objects, ") + std::to_string(all.triangleCount()) + " triangles";
}

// ---- import ----

namespace {

struct ObjPart {
    std::string name;
    std::vector<std::vector<std::pair<long, long>>> faces;  // (vertex, uv) global indices, 0-based, uv -1 = none
    std::vector<int> faceMat;
    std::vector<bool> faceSmooth;
    std::vector<std::pair<long, long>> lines;
    std::vector<std::string> mats;
};

void readMtl(ModelDoc& doc, const fs::path& path) {
    std::ifstream in(path);
    if (!in) return;
    std::string line;
    Material* cur = nullptr;
    std::vector<Material> fresh;
    while (std::getline(in, line)) {
        std::istringstream ls(line);
        std::string tag;
        ls >> tag;
        if (tag == "newmtl") {
            std::string name;
            ls >> name;
            fresh.push_back({});
            fresh.back().name = name;
            cur = &fresh.back();
        } else if (cur && tag == "Kd") {
            ls >> cur->color.x >> cur->color.y >> cur->color.z;
        } else if (cur && tag == "Pr") {
            ls >> cur->roughness;
        } else if (cur && tag == "Ns") {
            double ns = 0;
            ls >> ns;
            cur->roughness = std::clamp(std::pow(2.0 / (ns + 2), 0.25), 0.0, 1.0);
        } else if (cur && tag == "Pm") {
            ls >> cur->metallic;
        } else if (cur && tag == "d") {
            ls >> cur->alpha;
        } else if (cur && tag == "Ke") {
            Vec3 e;
            ls >> e.x >> e.y >> e.z;
            const double s = std::max({e.x, e.y, e.z});
            if (s > 0) {
                cur->emission = e / s;
                cur->emissionStrength = s;
            }
        } else if (cur && (tag == "map_Kd" || tag == "norm" || tag == "map_Bump" || tag == "map_bump" || tag == "bump" || tag == "map_Pr")) {
            std::string rest;
            std::getline(ls, rest);
            // options (-bm 0.5 ...) come before the file name
            std::istringstream words(rest);
            std::vector<std::string> w;
            for (std::string s; words >> s;) w.push_back(s);
            double scale = 1;
            for (std::size_t i = 0; i + 1 < w.size(); ++i)
                if (w[i] == "-bm") scale = std::atof(w[i + 1].c_str());
            if (!w.empty()) {
                const fs::path p(w.back());
                const std::string file = (p.is_absolute() ? p : path.parent_path() / p).string();
                if (tag == "map_Kd") cur->texture = file;
                else if (tag == "map_Pr") cur->roughnessMap = file;
                else cur->normalMap = file, cur->normalStrength = std::clamp(scale, 0.0, 4.0);
            }
        }
    }
    for (auto& m : fresh)
        if (!doc.material(m.name)) doc.scene.materials.push_back(m);
}

std::vector<std::string> importObj(ModelDoc& doc, const fs::path& path) {
    std::ifstream in(path);
    if (!in) throw std::runtime_error("can't open " + path.string());
    std::vector<Vec3> pos, col;
    std::vector<Vec2> uvs;
    std::vector<ObjPart> parts(1);
    parts[0].name = path.stem().string();
    int mat = -1;
    bool smooth = false;
    std::string line;
    auto idx = [](long i, std::size_t n) -> long { return i > 0 ? i - 1 : i < 0 ? static_cast<long>(n) + i : -1; };
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        std::istringstream ls(line);
        std::string tag;
        ls >> tag;
        if (tag == "v") {
            Vec3 p, c{1, 1, 1};
            ls >> p.x >> p.y >> p.z;
            if (ls >> c.x >> c.y >> c.z) col.resize(pos.size(), Vec3{1, 1, 1}), col.push_back(c);
            pos.push_back(p);
        } else if (tag == "vt") {
            Vec2 t;
            ls >> t.x >> t.y;
            uvs.push_back(t);
        } else if (tag == "o" || tag == "g") {
            std::string name;
            std::getline(ls, name);
            name.erase(0, name.find_first_not_of(" \t"));
            if (name.empty()) continue;
            if (!parts.back().faces.empty() || !parts.back().lines.empty()) {
                parts.push_back({});
                mat = -1;
            } else if (tag == "g" && parts.size() > 1) {
                continue;
            }
            parts.back().name = name;
        } else if (tag == "mtllib") {
            std::string rest;
            std::getline(ls, rest);
            rest.erase(0, rest.find_first_not_of(" \t"));
            readMtl(doc, path.parent_path() / rest);
        } else if (tag == "usemtl") {
            std::string name;
            ls >> name;
            auto& m = parts.back().mats;
            auto it = std::find(m.begin(), m.end(), name);
            if (it == m.end()) {
                m.push_back(name);
                mat = static_cast<int>(m.size()) - 1;
            } else {
                mat = static_cast<int>(it - m.begin());
            }
        } else if (tag == "s") {
            std::string v;
            ls >> v;
            smooth = !(v == "off" || v == "0");
        } else if (tag == "f") {
            std::vector<std::pair<long, long>> f;
            std::string tok;
            while (ls >> tok) {
                const auto s1 = tok.find('/');
                const long v = idx(std::stol(tok.substr(0, s1)), pos.size());
                long t = -1;
                if (s1 != std::string::npos) {
                    const auto s2 = tok.find('/', s1 + 1);
                    const std::string ts = tok.substr(s1 + 1, s2 == std::string::npos ? std::string::npos : s2 - s1 - 1);
                    if (!ts.empty()) t = idx(std::stol(ts), uvs.size());
                }
                if (v < 0 || v >= static_cast<long>(pos.size())) throw std::runtime_error("face refers to a missing vertex: " + line);
                f.push_back({v, t});
            }
            if (f.size() >= 3) {
                parts.back().faces.push_back(f);
                parts.back().faceMat.push_back(std::max(mat, 0));
                parts.back().faceSmooth.push_back(smooth);
            }
        } else if (tag == "l") {
            std::vector<long> vs;
            std::string tok;
            while (ls >> tok) vs.push_back(idx(std::stol(tok.substr(0, tok.find('/'))), pos.size()));
            for (std::size_t i = 0; i + 1 < vs.size(); ++i) parts.back().lines.push_back({vs[i], vs[i + 1]});
        }
    }
    std::vector<std::string> names;
    for (const auto& part : parts) {
        if (part.faces.empty() && part.lines.empty()) continue;
        PolyMesh m;
        m.colors = !col.empty();
        std::map<long, std::uint32_t> local;
        auto vert = [&](long g) {
            auto it = local.find(g);
            if (it != local.end()) return it->second;
            const auto i = m.addVertex(pos[static_cast<std::size_t>(g)]);
            if (m.colors && static_cast<std::size_t>(g) < col.size()) m.verts[i].color = col[static_cast<std::size_t>(g)];
            local[g] = i;
            return i;
        };
        for (std::size_t i = 0; i < part.faces.size(); ++i) {
            Face f;
            bool allUv = true;
            for (const auto& [v, t] : part.faces[i]) {
                f.v.push_back(vert(v));
                allUv = allUv && t >= 0 && t < static_cast<long>(uvs.size());
            }
            if (allUv)
                for (const auto& c : part.faces[i]) f.uv.push_back(uvs[static_cast<std::size_t>(c.second)]);
            f.material = part.faceMat[i];
            f.smooth = part.faceSmooth[i];
            m.faces.push_back(std::move(f));
        }
        for (const auto& [a, b] : part.lines)
            if (a >= 0 && b >= 0 && a < static_cast<long>(pos.size()) && b < static_cast<long>(pos.size()))
                m.loose.push_back({vert(a), vert(b)});
        // faces without uvs in a mesh that has some would confuse the uv tools: give them zeros
        if (m.hasUvs())
            for (auto& f : m.faces)
                if (f.uv.empty()) f.uv.assign(f.v.size(), Vec2{0, 0});
        cleanup(m);
        const std::string name = addMeshObject(doc, part.name, std::move(m));
        doc.object(name)->materials = part.mats;
        for (const auto& mn : part.mats)
            if (!doc.material(mn)) {
                Material mt;
                mt.name = mn;
                doc.scene.materials.push_back(mt);
            }
        names.push_back(name);
    }
    if (names.empty()) throw std::runtime_error(path.string() + " has no faces or lines");
    return names;
}

std::vector<std::string> importModel(ModelDoc& doc, const fs::path& path) {
    auto other = loadModel(path);
    std::map<std::string, std::string> objRename, meshRename;
    for (const auto& [name, m] : other->scene.meshes) {
        const std::string n = doc.uniqueMeshName(name);
        meshRename[name] = n;
        doc.scene.meshes[n] = m;
    }
    std::vector<std::string> names;
    for (const auto& o : other->scene.objects) {
        std::string n = doc.uniqueName(o.name);
        while (std::find(names.begin(), names.end(), n) != names.end()) n = doc.uniqueName(n + ".001");
        objRename[o.name] = n;
        names.push_back(n);
    }
    for (auto& c : other->scene.collections)
        if (std::none_of(doc.scene.collections.begin(), doc.scene.collections.end(), [&](const Collection& x) { return x.name == c.name; }))
            doc.scene.collections.push_back(c);
    for (const auto& m : other->scene.materials)
        if (!doc.material(m.name)) doc.scene.materials.push_back(m);
    for (auto& o : doc.scene.objects) o.selected = false;
    for (Object o : other->scene.objects) {
        o.name = objRename[o.name];
        if (!o.data.empty()) o.data = meshRename.count(o.data) ? meshRename[o.data] : o.data;
        if (!o.parent.empty()) o.parent = objRename.count(o.parent) ? objRename[o.parent] : "";
        for (auto& mod : o.modifiers)
            if (mod.text.count("object") && objRename.count(mod.text["object"])) mod.text["object"] = objRename[mod.text["object"]];
        o.selected = true;
        doc.scene.objects.push_back(std::move(o));
    }
    if (!names.empty()) doc.scene.active = names.front();
    return names;
}

}  // namespace

std::vector<std::string> importFile(ModelDoc& doc, const fs::path& path, bool quads) {
    const std::string ext = lowerExt(path);
    std::vector<std::string> names;
    if (ext == "wwmodel") {
        names = importModel(doc, path);
    } else if (ext == "obj") {
        names = importObj(doc, path);
    } else {
        if (!mesh::canRead(path)) {
            std::string list = "wwmodel";
            for (const auto& e : mesh::readableExtensions()) list += ", " + e;
            throw std::runtime_error("can't read ." + ext + " (use " + list + ")");
        }
        mesh::Mesh tm = mesh::load(path);
        const bool smooth = tm.hasNormals();
        tm.normals.clear();
        PolyMesh m = fromTriangles(tm);
        if (quads) trisToQuads(m, 40, false);
        for (auto& f : m.faces) f.smooth = smooth;
        selectAll(m, false);
        names.push_back(addMeshObject(doc, path.stem().string(), std::move(m)));
    }
    doc.refresh();
    return names;
}

}  // namespace wetwaresuite::model
