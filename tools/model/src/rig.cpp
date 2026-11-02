// Rigging, the Blender way at its core: armature objects hold bones (head, tail, roll, parent),
// meshes hold vertex groups (a weight per vertex for each bone), and the armature modifier skins
// the mesh by the posed bones (linear blend skinning). Poses are keyed like any channel
// (pose.<bone>.rotation.x ...), and a skinned scene exports to glTF with its skin, bone
// hierarchy and keyed poses as an animation, so it drops straight into Godot or any engine.
#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <fstream>
#include <functional>
#include <set>
#include <stdexcept>

#include <nlohmann/json.hpp>

#include "wetwaresuite/model/model.hpp"

namespace wetwaresuite::model {

using json = nlohmann::json;

const Bone* findBone(const Object& a, const std::string& name) {
    for (const auto& b : a.bones)
        if (b.name == name) return &b;
    return nullptr;
}

Bone* findBone(Object& a, const std::string& name) {
    for (auto& b : a.bones)
        if (b.name == name) return &b;
    return nullptr;
}

namespace {

Mat4 fromColumns(const Vec3& x, const Vec3& y, const Vec3& z, const Vec3& t) {
    Mat4 m;
    m.m[0] = x.x, m.m[1] = y.x, m.m[2] = z.x, m.m[3] = t.x;
    m.m[4] = x.y, m.m[5] = y.y, m.m[6] = z.y, m.m[7] = t.y;
    m.m[8] = x.z, m.m[9] = y.z, m.m[10] = z.z, m.m[11] = t.z;
    m.m[12] = 0, m.m[13] = 0, m.m[14] = 0, m.m[15] = 1;
    return m;
}

Mat4 poseLocal(const Bone& b) { return Mat4::translate(b.location) * Mat4::rotate(b.rotation); }

double segmentDistance(const Vec3& p, const Vec3& a, const Vec3& b) {
    const Vec3 ab = b - a;
    const double l2 = mesh::dot(ab, ab);
    const double t = l2 > 0 ? std::clamp(mesh::dot(p - a, ab) / l2, 0.0, 1.0) : 0.0;
    return mesh::length(p - (a + ab * t));
}

}  // namespace

Mat4 boneRest(const Bone& b) {
    Vec3 y = b.tail - b.head;
    y = mesh::length(y) > 1e-12 ? mesh::normalize(y) : Vec3{0, 0, 1};
    const Vec3 ref = std::fabs(y.z) < 0.999 ? Vec3{0, 0, 1} : Vec3{0, -1, 0};
    Vec3 x = mesh::normalize(mesh::cross(y, ref));
    Vec3 z = mesh::cross(x, y);
    if (b.roll != 0) {
        const Mat4 r = Mat4::rotateAxis(y, b.roll);
        x = r.direction(x);
        z = r.direction(z);
    }
    return fromColumns(x, y, z, b.head);
}

std::map<std::string, Mat4> posedBones(const Object& a) {
    std::map<std::string, Mat4> out;
    std::set<std::string> visiting;
    std::function<Mat4(const Bone&)> posed = [&](const Bone& b) -> Mat4 {
        auto it = out.find(b.name);
        if (it != out.end()) return it->second;
        Mat4 g = boneRest(b) * poseLocal(b);
        const Bone* parent = b.parent.empty() ? nullptr : findBone(a, b.parent);
        if (parent && !visiting.count(b.name)) {
            visiting.insert(b.name);
            g = posed(*parent) * boneRest(*parent).inverse() * boneRest(b) * poseLocal(b);
            visiting.erase(b.name);
        }
        out[b.name] = g;
        return g;
    };
    for (const auto& b : a.bones) posed(b);
    return out;
}

std::map<std::string, Mat4> skinMatrices(const ModelDoc& doc, const Object& a) {
    const Mat4 w = doc.world(a), wi = w.inverse();
    std::map<std::string, Mat4> out;
    for (const auto& [name, g] : posedBones(a)) {
        const Bone* b = findBone(a, name);
        out[name] = w * g * boneRest(*b).inverse() * wi;
    }
    return out;
}

void autoWeights(const ModelDoc& doc, Object& o, const Object& a) {
    const PolyMesh* m = doc.meshOf(o);
    if (!m) throw std::runtime_error(o.name + " has no mesh");
    if (a.bones.empty()) throw std::runtime_error(a.name + " has no bones");
    const Mat4 ow = doc.world(o), aw = doc.world(a);
    struct Seg {
        std::string name;
        Vec3 a, b;
        double soft;
    };
    std::vector<Seg> segs;
    for (const auto& b : a.bones) {
        const Vec3 h = aw.point(b.head), t = aw.point(b.tail);
        segs.push_back({b.name, h, t, std::max(1e-6, mesh::length(t - h) * 0.08)});
    }
    o.weights.clear();
    for (const auto& s : segs) o.weights[s.name].assign(m->verts.size(), 0.0f);
    std::vector<std::pair<double, std::size_t>> w(segs.size());
    for (std::size_t v = 0; v < m->verts.size(); ++v) {
        const Vec3 p = ow.point(m->verts[v].p);
        for (std::size_t i = 0; i < segs.size(); ++i) {
            const double d = segmentDistance(p, segs[i].a, segs[i].b) + segs[i].soft;
            w[i] = {1.0 / (d * d * d * d), i};
        }
        std::sort(w.begin(), w.end(), [](const auto& x, const auto& y) { return x.first > y.first; });
        const std::size_t keep = std::min<std::size_t>(4, w.size());
        double total = 0;
        for (std::size_t k = 0; k < keep; ++k)
            if (w[k].first >= w[0].first * 0.02) total += w[k].first;
        for (std::size_t k = 0; k < keep; ++k)
            if (w[k].first >= w[0].first * 0.02) o.weights[segs[w[k].second].name][v] = static_cast<float>(w[k].first / total);
    }
}

PolyMesh applyArmature(const ModelDoc& doc, const Object& o, const PolyMesh& base, const Modifier& mod) {
    const std::string name = mod.str("object", "");
    const Object* a = doc.object(name);
    if (!a || a->type != "armature") throw std::runtime_error("object= must name an armature" + (name.empty() ? std::string() : " ('" + name + "' isn't one)"));
    PolyMesh m = base;
    if (o.weights.empty()) return m;
    const auto skin = skinMatrices(doc, *a);
    const Mat4 ow = doc.world(o), owi = ow.inverse();
    std::vector<std::pair<const Mat4*, const std::vector<float>*>> groups;
    for (const auto& [bone, w] : o.weights) {
        auto it = skin.find(bone);
        if (it != skin.end()) groups.push_back({&it->second, &w});
    }
    for (std::size_t v = 0; v < m.verts.size(); ++v) {
        const Vec3 p = ow.point(m.verts[v].p);
        Vec3 acc{0, 0, 0};
        double total = 0;
        for (const auto& [mat, w] : groups) {
            const double wv = v < w->size() ? (*w)[v] : 0.0;
            if (wv <= 0) continue;
            acc = acc + mat->point(p) * wv;
            total += wv;
        }
        if (total > 1e-9) m.verts[v].p = owi.point(acc / total);
    }
    return m;
}

bool hasSkin(const ModelDoc& doc) {
    for (const auto& o : doc.scene.objects)
        for (const auto& mod : o.modifiers)
            if (mod.type == "armature" && mod.enabled && !o.weights.empty()) return true;
    return false;
}

// ---- glTF with a skin ----

namespace {

std::array<double, 4> quatOf(const Mat4& m) {
    const double r00 = m.m[0], r01 = m.m[1], r02 = m.m[2], r10 = m.m[4], r11 = m.m[5], r12 = m.m[6], r20 = m.m[8], r21 = m.m[9], r22 = m.m[10];
    const double tr = r00 + r11 + r22;
    double x, y, z, w;
    if (tr > 0) {
        const double s = std::sqrt(tr + 1.0) * 2;
        w = 0.25 * s, x = (r21 - r12) / s, y = (r02 - r20) / s, z = (r10 - r01) / s;
    } else if (r00 > r11 && r00 > r22) {
        const double s = std::sqrt(1.0 + r00 - r11 - r22) * 2;
        w = (r21 - r12) / s, x = 0.25 * s, y = (r01 + r10) / s, z = (r02 + r20) / s;
    } else if (r11 > r22) {
        const double s = std::sqrt(1.0 + r11 - r00 - r22) * 2;
        w = (r02 - r20) / s, x = (r01 + r10) / s, y = 0.25 * s, z = (r12 + r21) / s;
    } else {
        const double s = std::sqrt(1.0 + r22 - r00 - r11) * 2;
        w = (r10 - r01) / s, x = (r02 + r20) / s, y = (r12 + r21) / s, z = 0.25 * s;
    }
    const double n = std::sqrt(x * x + y * y + z * z + w * w);
    return {x / n, y / n, z / n, w / n};
}

json columnMajor(const Mat4& m) {
    json a = json::array();
    for (int c = 0; c < 4; ++c)
        for (int r = 0; r < 4; ++r) a.push_back(m.m[r * 4 + c]);
    return a;
}

struct Buffer {
    std::string bin;
    json views = json::array(), accessors = json::array();
    template <class T>
    int add(const std::vector<T>& data, int componentType, const char* type, std::size_t count, int target, json minv = {}, json maxv = {}) {
        while (bin.size() % 4) bin.push_back('\0');
        const std::size_t offset = bin.size();
        bin.append(reinterpret_cast<const char*>(data.data()), data.size() * sizeof(T));
        json v{{"buffer", 0}, {"byteOffset", offset}, {"byteLength", data.size() * sizeof(T)}};
        if (target) v["target"] = target;
        views.push_back(v);
        json a{{"bufferView", views.size() - 1}, {"componentType", componentType}, {"count", count}, {"type", type}};
        if (!minv.is_null()) a["min"] = minv, a["max"] = maxv;
        accessors.push_back(a);
        return static_cast<int>(accessors.size()) - 1;
    }
};

}  // namespace

std::string exportSkinnedGltf(const ModelDoc& doc, const std::filesystem::path& path, bool modifiers) {
    std::string ext = path.extension().string();
    for (auto& c : ext) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    const bool binary = ext == ".glb";
    Buffer buf;
    json nodes = json::array(), meshes = json::array(), skins = json::array(), materials = json::array(), animations = json::array();
    json sceneNodes = json::array();
    std::map<std::string, int> materialIndex;
    auto materialFor = [&](const Object& o) -> int {
        if (o.materials.empty()) return -1;
        const Material* mat = doc.material(o.materials.front());
        if (!mat) return -1;
        auto it = materialIndex.find(mat->name);
        if (it != materialIndex.end()) return it->second;
        materials.push_back({{"name", mat->name},
                             {"pbrMetallicRoughness",
                              {{"baseColorFactor", {mat->color.x, mat->color.y, mat->color.z, mat->alpha}}, {"metallicFactor", mat->metallic},
                               {"roughnessFactor", mat->roughness}}}});
        return materialIndex[mat->name] = static_cast<int>(materials.size()) - 1;
    };
    // armatures: a node with the armature's world matrix, and a node per bone under it
    std::map<std::string, std::map<std::string, int>> boneNode;  // armature -> bone -> node
    std::size_t skinned = 0, plain = 0, bonesOut = 0;
    for (const auto& a : doc.scene.objects) {
        if (a.type != "armature" || a.bones.empty()) continue;
        const int root = static_cast<int>(nodes.size());
        nodes.push_back({{"name", a.name}, {"matrix", columnMajor(doc.world(a))}, {"children", json::array()}});
        sceneNodes.push_back(root);
        auto& ids = boneNode[a.name];
        for (const auto& b : a.bones) {
            ids[b.name] = static_cast<int>(nodes.size());
            nodes.push_back({{"name", b.name}});
            ++bonesOut;
        }
        for (const auto& b : a.bones) {
            const Bone* parent = b.parent.empty() ? nullptr : findBone(a, b.parent);
            const Mat4 local = parent ? boneRest(*parent).inverse() * boneRest(b) : boneRest(b);
            json& n = nodes[static_cast<std::size_t>(ids[b.name])];
            const auto q = quatOf(local);
            n["translation"] = {local.m[3], local.m[7], local.m[11]};
            n["rotation"] = {q[0], q[1], q[2], q[3]};
            const int at = parent ? ids[parent->name] : root;
            if (!nodes[static_cast<std::size_t>(at)].contains("children")) nodes[static_cast<std::size_t>(at)]["children"] = json::array();
            nodes[static_cast<std::size_t>(at)]["children"].push_back(ids[b.name]);
        }
        // keyed poses -> one animation per armature, sampled every frame
        std::set<std::string> keyed;
        for (const auto& [ch, keys] : a.keys)
            if (ch.rfind("pose.", 0) == 0 && !keys.empty()) keyed.insert(ch.substr(5, ch.find('.', 5) - 5));
        if (!keyed.empty()) {
            const int f0 = doc.scene.frameStart, f1 = std::max(doc.scene.frameEnd, f0);
            std::vector<float> times;
            for (int f = f0; f <= f1; ++f) times.push_back(static_cast<float>((f - f0) / std::max(1.0, doc.scene.fps)));
            const int input = buf.add(times, 5126, "SCALAR", times.size(), 0, json::array({times.front()}), json::array({times.back()}));
            json samplers = json::array(), channels = json::array();
            for (const auto& bn : keyed) {
                const Bone* b = findBone(a, bn);
                if (!b) continue;
                const Bone* parent = b->parent.empty() ? nullptr : findBone(a, b->parent);
                const Mat4 restLocal = parent ? boneRest(*parent).inverse() * boneRest(*b) : boneRest(*b);
                std::vector<float> rot, loc;
                for (int f = f0; f <= f1; ++f) {
                    Bone posed = *b;
                    auto at = [&](const std::string& c, double fallback) { return channelAt(a, "pose." + bn + "." + c, f, fallback); };
                    posed.rotation = {at("rotation.x", b->rotation.x), at("rotation.y", b->rotation.y), at("rotation.z", b->rotation.z)};
                    posed.location = {at("location.x", b->location.x), at("location.y", b->location.y), at("location.z", b->location.z)};
                    const Mat4 local = restLocal * poseLocal(posed);
                    const auto q = quatOf(local);
                    rot.insert(rot.end(), {static_cast<float>(q[0]), static_cast<float>(q[1]), static_cast<float>(q[2]), static_cast<float>(q[3])});
                    loc.insert(loc.end(), {static_cast<float>(local.m[3]), static_cast<float>(local.m[7]), static_cast<float>(local.m[11])});
                }
                const int r = buf.add(rot, 5126, "VEC4", rot.size() / 4, 0);
                const int l = buf.add(loc, 5126, "VEC3", loc.size() / 3, 0);
                samplers.push_back({{"input", input}, {"output", r}, {"interpolation", "LINEAR"}});
                channels.push_back({{"sampler", samplers.size() - 1}, {"target", {{"node", ids[bn]}, {"path", "rotation"}}}});
                samplers.push_back({{"input", input}, {"output", l}, {"interpolation", "LINEAR"}});
                channels.push_back({{"sampler", samplers.size() - 1}, {"target", {{"node", ids[bn]}, {"path", "translation"}}}});
            }
            animations.push_back({{"name", a.name + "Action"}, {"samplers", samplers}, {"channels", channels}});
        }
    }
    // meshes: world-space rest positions, one vertex per face corner (keeps uvs and flat faces simple)
    for (const auto& o : doc.scene.objects) {
        if (o.type != "mesh" || !doc.shown(o)) continue;
        const Modifier* arm = nullptr;
        for (const auto& mod : o.modifiers)
            if (mod.type == "armature" && mod.enabled) arm = &mod;
        const Object* a = arm ? doc.object(arm->str("object", "")) : nullptr;
        const bool skin = a && a->type == "armature" && !o.weights.empty() && boneNode.count(a->name);
        PolyMesh pm;
        if (skin || !modifiers) {
            const PolyMesh* base = doc.meshOf(o);
            if (!base) continue;
            pm = *base;  // skinned: the rest mesh, the weights index its vertices
        } else {
            pm = evaluate(doc, o);
        }
        const Mat4 w = doc.world(o);
        std::vector<float> pos, nrm, uv;
        std::vector<std::uint16_t> joints;
        std::vector<float> weights;
        std::vector<std::uint32_t> idx;
        std::vector<std::string> boneOrder;
        if (skin)
            for (const auto& b : a->bones) boneOrder.push_back(b.name);
        Vec3 lo{1e300, 1e300, 1e300}, hi{-1e300, -1e300, -1e300};
        const bool hasUv = pm.hasUvs();
        for (const auto& f : pm.faces) {
            const auto tri = triangulateFace(pm, f);
            Vec3 n{0, 0, 0};
            for (std::size_t k = 0; k < f.v.size(); ++k) {
                const Vec3 p0 = pm.verts[f.v[k]].p, p1 = pm.verts[f.v[(k + 1) % f.v.size()]].p;
                n = n + Vec3{(p0.y - p1.y) * (p0.z + p1.z), (p0.z - p1.z) * (p0.x + p1.x), (p0.x - p1.x) * (p0.y + p1.y)};
            }
            n = mesh::normalize(w.direction(n));
            const auto first = static_cast<std::uint32_t>(pos.size() / 3);
            for (std::size_t k = 0; k < f.v.size(); ++k) {
                const Vec3 p = w.point(pm.verts[f.v[k]].p);
                pos.insert(pos.end(), {static_cast<float>(p.x), static_cast<float>(p.y), static_cast<float>(p.z)});
                lo = {std::min(lo.x, p.x), std::min(lo.y, p.y), std::min(lo.z, p.z)};
                hi = {std::max(hi.x, p.x), std::max(hi.y, p.y), std::max(hi.z, p.z)};
                nrm.insert(nrm.end(), {static_cast<float>(n.x), static_cast<float>(n.y), static_cast<float>(n.z)});
                if (hasUv) {
                    const Vec2 t = k < f.uv.size() ? f.uv[k] : Vec2{0, 0};
                    uv.insert(uv.end(), {static_cast<float>(t.x), static_cast<float>(1 - t.y)});
                }
                if (skin) {
                    std::vector<std::pair<float, std::uint16_t>> wv;
                    for (std::size_t bi = 0; bi < boneOrder.size(); ++bi) {
                        auto it = o.weights.find(boneOrder[bi]);
                        const float x = it != o.weights.end() && f.v[k] < it->second.size() ? it->second[f.v[k]] : 0.0f;
                        if (x > 0) wv.push_back({x, static_cast<std::uint16_t>(bi)});
                    }
                    std::sort(wv.begin(), wv.end(), [](const auto& x, const auto& y) { return x.first > y.first; });
                    wv.resize(std::min<std::size_t>(4, wv.size()));
                    float total = 0;
                    for (const auto& x : wv) total += x.first;
                    for (std::size_t j = 0; j < 4; ++j) {
                        joints.push_back(j < wv.size() ? wv[j].second : 0);
                        weights.push_back(j < wv.size() && total > 0 ? wv[j].first / total : (j == 0 && wv.empty() ? 1.0f : 0.0f));
                    }
                }
            }
            for (auto t : tri) idx.push_back(first + t);
        }
        if (idx.empty()) continue;
        const std::size_t nv = pos.size() / 3;
        json attrs;
        attrs["POSITION"] = buf.add(pos, 5126, "VEC3", nv, 34962, json::array({lo.x, lo.y, lo.z}), json::array({hi.x, hi.y, hi.z}));
        attrs["NORMAL"] = buf.add(nrm, 5126, "VEC3", nv, 34962);
        if (hasUv) attrs["TEXCOORD_0"] = buf.add(uv, 5126, "VEC2", nv, 34962);
        if (skin) {
            attrs["JOINTS_0"] = buf.add(joints, 5123, "VEC4", nv, 34962);
            attrs["WEIGHTS_0"] = buf.add(weights, 5126, "VEC4", nv, 34962);
        }
        json prim{{"attributes", attrs}, {"indices", buf.add(idx, 5125, "SCALAR", idx.size(), 34963)}};
        const int mat = materialFor(o);
        if (mat >= 0) prim["material"] = mat;
        meshes.push_back({{"name", o.name}, {"primitives", json::array({prim})}});
        json node{{"name", o.name}, {"mesh", meshes.size() - 1}};
        if (skin) {
            json jointList = json::array();
            std::vector<float> ibm;
            const Mat4 aw = doc.world(*a);
            for (const auto& b : a->bones) {
                jointList.push_back(boneNode[a->name][b.name]);
                const Mat4 inv = (aw * boneRest(b)).inverse();
                for (int c = 0; c < 4; ++c)
                    for (int r = 0; r < 4; ++r) ibm.push_back(static_cast<float>(inv.m[r * 4 + c]));
            }
            skins.push_back({{"name", a->name}, {"joints", jointList}, {"inverseBindMatrices", buf.add(ibm, 5126, "MAT4", a->bones.size(), 0)}});
            node["skin"] = skins.size() - 1;
            ++skinned;
        } else {
            ++plain;
        }
        sceneNodes.push_back(static_cast<int>(nodes.size()));
        nodes.push_back(node);
    }
    if (meshes.empty()) throw std::runtime_error("nothing to export (no visible mesh objects)");
    while (buf.bin.size() % 4) buf.bin.push_back('\0');
    json g{{"asset", {{"version", "2.0"}, {"generator", "wetwaresuite model"}}},
           {"scene", 0},
           {"scenes", json::array({{{"nodes", sceneNodes}}})},
           {"nodes", nodes},
           {"meshes", meshes},
           {"accessors", buf.accessors},
           {"bufferViews", buf.views}};
    if (!skins.empty()) g["skins"] = skins;
    if (!materials.empty()) g["materials"] = materials;
    if (!animations.empty()) g["animations"] = animations;
    std::ofstream out(path, std::ios::binary);
    if (!out) throw std::runtime_error("can't write " + path.string());
    if (binary) {
        g["buffers"] = json::array({{{"byteLength", buf.bin.size()}}});
        std::string text = g.dump();
        while (text.size() % 4) text.push_back(' ');
        auto u32 = [&](std::uint32_t v) { out.write(reinterpret_cast<const char*>(&v), 4); };
        u32(0x46546C67);  // glTF
        u32(2);
        u32(static_cast<std::uint32_t>(12 + 8 + text.size() + 8 + buf.bin.size()));
        u32(static_cast<std::uint32_t>(text.size()));
        u32(0x4E4F534A);  // JSON
        out.write(text.data(), static_cast<std::streamsize>(text.size()));
        u32(static_cast<std::uint32_t>(buf.bin.size()));
        u32(0x004E4942);  // BIN
        out.write(buf.bin.data(), static_cast<std::streamsize>(buf.bin.size()));
    } else {
        // .gltf with the buffer beside it
        std::filesystem::path binPath = path;
        binPath.replace_extension(".bin");
        std::ofstream b(binPath, std::ios::binary);
        b.write(buf.bin.data(), static_cast<std::streamsize>(buf.bin.size()));
        g["buffers"] = json::array({{{"byteLength", buf.bin.size()}, {"uri", binPath.filename().string()}}});
        out << g.dump(1);
    }
    if (!out) throw std::runtime_error("can't write " + path.string());
    return "wrote " + path.string() + ": " + std::to_string(skinned) + " skinned and " + std::to_string(plain) + " plain meshes, " +
           std::to_string(bonesOut) + " bones" + (animations.empty() ? std::string() : ", " + std::to_string(animations.size()) + " animations");
}

}  // namespace wetwaresuite::model
