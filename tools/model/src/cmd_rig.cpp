// Rigging commands: armatures and bones, posing (and keying poses), skinning with automatic
// weights, and painting weights by hand.
#include <algorithm>
#include <cmath>

#include "cmd.hpp"

namespace wetwaresuite::model::cmd {

namespace {

Object& armatureOf(ModelDoc& d, const Params& p) {
    const std::string name = has(p, "armature") ? stringParam(p, "armature") : d.scene.active;
    Object* a = d.object(name);
    if (!a) {
        for (auto& o : d.scene.objects)
            if (o.type == "armature") return o;  // the only one, usually
        throw std::runtime_error("no armature (model.armature_add makes one)");
    }
    if (a->type != "armature") {
        // a mesh skinned by one: its armature
        for (const auto& m : a->modifiers)
            if (m.type == "armature")
                if (Object* arm = d.object(m.str("object", ""))) return *arm;
        throw std::runtime_error(a->name + " isn't an armature (armature= names one)");
    }
    return *a;
}

Bone& boneOf(Object& a, const Params& p) {
    const std::string name = stringParam(p, "bone");
    if (name.empty() && !a.bones.empty()) return a.bones.back();
    Bone* b = findBone(a, name);
    if (!b) throw std::runtime_error(a.name + " has no bone '" + name + "'");
    return *b;
}

std::string boneList(const Object& a) {
    std::string s;
    for (const auto& b : a.bones) s += (s.empty() ? "" : ",") + b.name;
    return s;
}

Result rigDone(const std::string& message, ModelDoc& d, const Params& p, const Object& a) {
    Result r = done(message, d, p);
    r.with("armature", a.name).with("bones", boneList(a));
    return r;
}

// Bone channels keyed at a frame.
void keyBone(Object& a, const Bone& b, int frame, const std::string& interp) {
    for (const char* g : {"rotation", "location"}) {
        const Vec3 v = std::string(g) == "rotation" ? b.rotation : b.location;
        const double vals[3] = {v.x, v.y, v.z};
        const char* axes[3] = {"x", "y", "z"};
        for (int i = 0; i < 3; ++i) {
            auto& keys = a.keys["pose." + b.name + "." + g + "." + axes[i]];
            std::erase_if(keys, [&](const Keyframe& k) { return k.frame == frame; });
            keys.push_back({frame, vals[i], interp});
            std::sort(keys.begin(), keys.end(), [](const Keyframe& x, const Keyframe& y) { return x.frame < y.frame; });
        }
    }
}

}  // namespace

void registerRigCommands(Session& session) {
    auto& reg = session.registry();

    reg.add({"model.armature_add",
             "Add armature",
             "Add an armature (a skeleton) at the 3D cursor, with one bone 1 m up, as Blender does (Shift+A, Armature). "
             "Add more with model.bone_add, skin a mesh to it with model.skin, pose it with model.pose.",
             withDoc({{"name", ParamType::String, "object name", false, {}, std::string("Armature")},
                      {"location", ParamType::NumberList, "x,y,z (default: the 3D cursor)", false},
                      {"bone", ParamType::String, "first bone's name", false, {}, std::string("Bone")},
                      {"length", ParamType::Number, "first bone's length", false, {}, 1.0}}),
             true},
            edit(session, [](ModelDoc& d, const Params& p) {
                Object o;
                o.type = "armature";
                o.name = d.uniqueName(stringParam(p, "name", "Armature"));
                o.location = vec3(p, "location", d.scene.cursor);
                const double len = numberParam(p, "length", 1.0);
                if (!(len > 0)) return Result::failure("length must be positive");
                o.bones.push_back({stringParam(p, "bone", "Bone"), "", {0, 0, 0}, {0, 0, len}});
                for (auto& other : d.scene.objects) other.selected = false;
                o.selected = true;
                d.scene.editMode = false;
                d.scene.objects.push_back(o);
                d.scene.active = o.name;
                return rigDone("added armature " + o.name, d, p, d.scene.objects.back());
            }));

    reg.add({"model.bone_add",
             "Add bone",
             "Add a bone to an armature: head and tail in the armature's space. from=<bone> starts it at that bone's "
             "tail and parents it there (Blender's extrude, E), so tail alone is enough. Give a chain in one go with "
             "chain=x,y,z,x,y,z,... (each point a new bone's tail).",
             withDoc({{"armature", ParamType::String, "armature (default: the active one)", false},
                      {"name", ParamType::String, "bone name (default Bone.001, ...)", false},
                      {"head", ParamType::NumberList, "x,y,z", false},
                      {"tail", ParamType::NumberList, "x,y,z", false},
                      {"parent", ParamType::String, "parent bone", false},
                      {"from", ParamType::String, "start at this bone's tail, as its child", false},
                      {"chain", ParamType::NumberList, "several bones end to end: tails x,y,z,...", false}}),
             true},
            edit(session, [](ModelDoc& d, const Params& p) {
                Object& a = armatureOf(d, p);
                auto unique = [&](std::string base) {
                    if (base.empty()) base = "Bone";
                    if (!findBone(a, base)) return base;
                    for (int n = 1;; ++n) {
                        char buf[16];
                        std::snprintf(buf, sizeof buf, ".%03d", n);
                        if (!findBone(a, base + buf)) return base + buf;
                    }
                };
                Bone b;
                b.name = unique(stringParam(p, "name", "Bone"));
                if (has(p, "from")) {
                    const Bone* from = findBone(a, stringParam(p, "from"));
                    if (!from) return Result::failure(a.name + " has no bone '" + stringParam(p, "from") + "'");
                    b.head = from->tail;
                    b.parent = from->name;
                    b.tail = b.head + (from->tail - from->head);
                }
                b.head = vec3(p, "head", b.head);
                b.tail = vec3(p, "tail", has(p, "head") ? b.head + Vec3{0, 0, 1} : b.tail);
                if (has(p, "parent")) {
                    b.parent = stringParam(p, "parent");
                    if (!b.parent.empty() && !findBone(a, b.parent)) return Result::failure(a.name + " has no bone '" + b.parent + "'");
                }
                std::size_t added = 0;
                if (has(p, "chain")) {
                    const NumberList c = listParam(p, "chain");
                    if (c.size() < 3 || c.size() % 3) return Result::failure("chain is x,y,z triples");
                    Vec3 head = has(p, "head") || has(p, "from") ? b.head : a.bones.empty() ? Vec3{0, 0, 0} : a.bones.back().tail;
                    std::string parent = has(p, "parent") || has(p, "from") ? b.parent : a.bones.empty() ? "" : a.bones.back().name;
                    const std::string stem = stringParam(p, "name", "Bone");
                    for (std::size_t i = 0; i + 2 < c.size(); i += 3) {
                        Bone k;
                        k.name = unique(stem);
                        k.head = head;
                        k.tail = {c[i], c[i + 1], c[i + 2]};
                        k.parent = parent;
                        if (mesh::length(k.tail - k.head) < 1e-9) return Result::failure("a bone needs some length");
                        a.bones.push_back(k);
                        head = k.tail;
                        parent = k.name;
                        ++added;
                    }
                } else {
                    if (mesh::length(b.tail - b.head) < 1e-9) return Result::failure("a bone needs some length (head and tail apart)");
                    a.bones.push_back(b);
                    added = 1;
                }
                return rigDone("added " + many(added, "bone") + " to " + a.name, d, p, a);
            }));

    reg.add({"model.bone_set",
             "Edit bone",
             "Change a bone's head, tail, roll, parent or name (rest pose; the pose is kept).",
             withDoc({{"armature", ParamType::String, "armature (default: the active one)", false},
                      {"bone", ParamType::String, "bone", true},
                      {"head", ParamType::NumberList, "x,y,z", false},
                      {"tail", ParamType::NumberList, "x,y,z", false},
                      {"roll", ParamType::Number, "degrees about its own axis", false},
                      {"parent", ParamType::String, "parent bone (empty: none)", false},
                      {"rename", ParamType::String, "new name", false}}),
             true},
            edit(session, [](ModelDoc& d, const Params& p) {
                Object& a = armatureOf(d, p);
                Bone& b = boneOf(a, p);
                b.head = vec3(p, "head", b.head);
                b.tail = vec3(p, "tail", b.tail);
                if (has(p, "roll")) b.roll = numberParam(p, "roll");
                if (has(p, "parent")) {
                    const std::string parent = stringParam(p, "parent");
                    for (std::string at = parent; !at.empty();) {  // no loops
                        if (at == b.name) return Result::failure("a bone can't be its own ancestor");
                        const Bone* up = findBone(a, at);
                        if (!up) return Result::failure(a.name + " has no bone '" + at + "'");
                        at = up->parent;
                    }
                    b.parent = parent;
                }
                if (mesh::length(b.tail - b.head) < 1e-9) return Result::failure("a bone needs some length");
                if (has(p, "rename")) {
                    const std::string to = stringParam(p, "rename"), from = b.name;
                    if (to.empty() || (findBone(a, to) && to != from)) return Result::failure("give a new, unused name");
                    for (auto& o : a.bones)
                        if (o.parent == from) o.parent = to;
                    for (auto& o : d.scene.objects) {  // vertex groups and keys follow
                        auto w = o.weights.find(from);
                        if (w != o.weights.end()) o.weights[to] = std::move(w->second), o.weights.erase(from);
                    }
                    std::map<std::string, std::vector<Keyframe>> keys;
                    for (auto& [ch, k] : a.keys)
                        keys[ch.rfind("pose." + from + ".", 0) == 0 ? "pose." + to + ch.substr(5 + from.size()) : ch] = k;
                    a.keys = keys;
                    findBone(a, from)->name = to;
                }
                return rigDone("changed bone " + (has(p, "rename") ? stringParam(p, "rename") : stringParam(p, "bone")), d, p, a);
            }));

    reg.add({"model.bone_delete",
             "Delete bone",
             "Remove a bone: its children move up to its parent, its vertex groups and keys go.",
             withDoc({{"armature", ParamType::String, "armature (default: the active one)", false}, {"bone", ParamType::String, "bone", true}}),
             true},
            edit(session, [](ModelDoc& d, const Params& p) {
                Object& a = armatureOf(d, p);
                const Bone b = boneOf(a, p);
                for (auto& o : a.bones)
                    if (o.parent == b.name) o.parent = b.parent;
                std::erase_if(a.bones, [&](const Bone& x) { return x.name == b.name; });
                for (auto& o : d.scene.objects) o.weights.erase(b.name);
                std::erase_if(a.keys, [&](const auto& kv) { return kv.first.rfind("pose." + b.name + ".", 0) == 0; });
                return rigDone("deleted bone " + b.name, d, p, a);
            }));

    reg.add({"model.pose",
             "Pose bone",
             "Turn (rotation, degrees about the bone's own X, Y, Z: Y runs along the bone) or move a bone; children "
             "follow and skinned meshes bend. key=true keys it at frame (I in pose mode), so poses play back and export "
             "as an animation. reset=true puts every bone back in its rest pose.",
             withDoc({{"armature", ParamType::String, "armature (default: the active one)", false},
                      {"bone", ParamType::String, "bone (default: the last one)", false},
                      {"rotation", ParamType::NumberList, "degrees about x,y,z of the bone", false},
                      {"location", ParamType::NumberList, "move x,y,z in the bone's frame", false},
                      {"relative", ParamType::Bool, "add to the pose instead of setting it", false, {}, false},
                      {"reset", ParamType::Bool, "every bone back to rest (keys stay)", false, {}, false},
                      {"key", ParamType::Bool, "key this bone's pose at frame", false, {}, false},
                      {"frame", ParamType::Int, "frame to key at (default: the current one)", false},
                      {"interp", ParamType::String, "interpolation to the next key", false, {"bezier", "linear", "constant"}, std::string("bezier")}}),
             true},
            edit(session, [](ModelDoc& d, const Params& p) {
                Object& a = armatureOf(d, p);
                if (boolParam(p, "reset", false)) {
                    for (auto& b : a.bones) b.rotation = b.location = {0, 0, 0};
                    return rigDone("reset the pose of " + a.name, d, p, a);
                }
                if (a.bones.empty()) return Result::failure(a.name + " has no bones");
                Bone& b = boneOf(a, p);
                const bool rel = boolParam(p, "relative", false);
                if (has(p, "rotation")) b.rotation = rel ? b.rotation + vec3(p, "rotation", {0, 0, 0}) : vec3(p, "rotation", b.rotation);
                if (has(p, "location")) b.location = rel ? b.location + vec3(p, "location", {0, 0, 0}) : vec3(p, "location", b.location);
                std::string msg = "posed " + b.name;
                if (boolParam(p, "key", false)) {
                    const int frame = static_cast<int>(intParam(p, "frame", d.scene.frame));
                    keyBone(a, b, frame, stringParam(p, "interp", "bezier"));
                    d.scene.frame = frame;
                    msg += ", keyed at frame " + std::to_string(frame);
                }
                return rigDone(msg, d, p, a);
            }));

    reg.add({"model.skin",
             "Skin to armature",
             "Bind a mesh to an armature (Ctrl+P, With Automatic Weights): adds an armature modifier first in its stack "
             "and gives every vertex weights from its distance to the bones (up to 4 bones each). weights=empty binds "
             "with no weights, to paint them yourself; weights=keep keeps the ones it has.",
             withDoc({objectParam(),
                      {"armature", ParamType::String, "armature (default: the only one)", false},
                      {"weights", ParamType::String, "how to start the weights", false, {"auto", "empty", "keep"}, std::string("auto")}}),
             true},
            edit(session, [](ModelDoc& d, const Params& p) {
                Object& o = target(d, p);
                if (o.type != "mesh" || !d.meshOf(o)) return Result::failure(o.name + " isn't a mesh");
                Params q = p;
                q.erase("object");
                Object* arm = nullptr;
                if (has(p, "armature")) {
                    arm = d.object(stringParam(p, "armature"));
                } else {
                    for (auto& x : d.scene.objects)
                        if (x.type == "armature") arm = &x;
                }
                if (!arm || arm->type != "armature") return Result::failure("no armature to skin to (model.armature_add makes one)");
                const std::string mode = stringParam(p, "weights", "auto");
                if (mode == "auto") autoWeights(d, o, *arm);
                if (mode == "empty") {
                    o.weights.clear();
                    for (const auto& b : arm->bones) o.weights[b.name].assign(d.meshOf(o)->verts.size(), 0.0f);
                }
                std::erase_if(o.modifiers, [](const Modifier& m) { return m.type == "armature"; });
                Modifier m = makeModifier("armature");
                m.text["object"] = arm->name;
                o.modifiers.insert(o.modifiers.begin(), m);
                Result r = done("skinned " + o.name + " to " + arm->name + " (" + many(arm->bones.size(), "bone") + ")", d, p);
                return r.with("armature", arm->name).with("bones", boneList(*arm));
            }));

    reg.add({"model.weight_paint",
             "Paint weights",
             "Paint how much a bone moves a skinned mesh, like Blender's weight paint: at=x,y,z (world; or a ray "
             "x,y,z,dx,dy,dz that hits the mesh) with radius, strength and falloff. mode add (towards weight), subtract, "
             "replace (set to weight) or smooth (blend with the neighbours). The viewport shows that bone's weights "
             "while you paint (model.weight_show).",
             withDoc({objectParam(),
                      {"bone", ParamType::String, "the bone (vertex group)", true},
                      {"at", ParamType::NumberList, "x,y,z on the surface, or several x,y,z,... for a stroke", false},
                      {"ray", ParamType::NumberList, "instead: x,y,z,dx,dy,dz from the view (where each hits is painted; several for a stroke)", false},
                      {"radius", ParamType::Number, "brush radius (world)", false, {}, 0.25},
                      {"strength", ParamType::Number, "0..1 per dab", false, {}, 0.5},
                      {"weight", ParamType::Number, "the weight add and replace paint towards", false, {}, 1.0},
                      {"mode", ParamType::String, "how it paints", false, {"add", "subtract", "replace", "smooth"}, std::string("add")}}),
             true},
            edit(session, [](ModelDoc& d, const Params& p) {
                Object& o = target(d, p);
                const PolyMesh* m = d.meshOf(o);
                if (!m) return Result::failure(o.name + " isn't a mesh");
                const std::string bone = stringParam(p, "bone");
                for (const auto& mod : o.modifiers) {
                    if (mod.type != "armature") continue;
                    const Object* arm = d.object(mod.str("object", ""));
                    if (arm && !findBone(*arm, bone)) return Result::failure(arm->name + " has no bone called '" + bone + "'");
                }
                auto& w = o.weights[bone];
                w.resize(m->verts.size(), 0.0f);
                const Mat4 ow = d.world(o);
                std::vector<Vec3> centres;
                const NumberList rays = has(p, "ray") ? listParam(p, "ray") : NumberList{};
                if (has(p, "ray") && (rays.empty() || rays.size() % 6)) return Result::failure("ray is x,y,z,dx,dy,dz (several for a stroke)");
                for (std::size_t ri = 0; ri + 5 < rays.size(); ri += 6) {
                    const Vec3 org{rays[ri], rays[ri + 1], rays[ri + 2]}, dir = mesh::normalize(Vec3{rays[ri + 3], rays[ri + 4], rays[ri + 5]});
                    double best = 1e300;
                    for (const auto& f : m->faces) {
                        const auto tri = triangulateFace(*m, f);
                        for (std::size_t k = 0; k + 2 < tri.size(); k += 3) {
                            const Vec3 a = ow.point(m->verts[f.v[tri[k]]].p), b = ow.point(m->verts[f.v[tri[k + 1]]].p),
                                       c = ow.point(m->verts[f.v[tri[k + 2]]].p);
                            const Vec3 e1 = b - a, e2 = c - a, pv = mesh::cross(dir, e2);
                            const double det = mesh::dot(e1, pv);
                            if (std::fabs(det) < 1e-14) continue;
                            const Vec3 tv = org - a;
                            const double u = mesh::dot(tv, pv) / det;
                            if (u < 0 || u > 1) continue;
                            const Vec3 qv = mesh::cross(tv, e1);
                            const double v = mesh::dot(dir, qv) / det, t = mesh::dot(e2, qv) / det;
                            if (v < 0 || u + v > 1 || t < 0 || t >= best) continue;
                            best = t;
                        }
                    }
                    if (best < 1e300) centres.push_back(org + dir * best);
                }
                if (has(p, "ray")) {
                    if (centres.empty()) return Result::failure("the ray misses " + o.name);
                } else {
                    const NumberList at = listParam(p, "at");
                    if (at.size() < 3 || at.size() % 3) return Result::failure("give at=x,y,z (or ray=)");
                    for (std::size_t i = 0; i + 2 < at.size(); i += 3) centres.push_back({at[i], at[i + 1], at[i + 2]});
                }
                const double radius = numberParam(p, "radius", 0.25), strength = std::clamp(numberParam(p, "strength", 0.5), 0.0, 1.0);
                const double target = std::clamp(numberParam(p, "weight", 1.0), 0.0, 1.0);
                const std::string mode = stringParam(p, "mode", "add");
                if (!(radius > 0)) return Result::failure("radius must be positive");
                // neighbours for smoothing
                std::vector<std::vector<std::uint32_t>> nb;
                if (mode == "smooth") {
                    nb.resize(m->verts.size());
                    for (const auto& f : m->faces)
                        for (std::size_t k = 0; k < f.v.size(); ++k) {
                            nb[f.v[k]].push_back(f.v[(k + 1) % f.v.size()]);
                            nb[f.v[(k + 1) % f.v.size()]].push_back(f.v[k]);
                        }
                }
                std::size_t touched = 0;
                for (const Vec3& c : centres) {
                    const std::vector<float> before = w;
                    for (std::size_t v = 0; v < m->verts.size(); ++v) {
                        const double dist = mesh::length(ow.point(m->verts[v].p) - c);
                        if (dist >= radius) continue;
                        const double x = dist / radius, fall = 1 - x * x * (3 - 2 * x);
                        const double k = strength * fall;
                        double cur = before[v];
                        if (mode == "add") cur += (target - cur) * k;
                        else if (mode == "subtract") cur -= cur * k;
                        else if (mode == "replace") cur = cur + (target - cur) * std::min(1.0, k * 2);
                        else if (!nb[v].empty()) {
                            double avg = 0;
                            for (auto u : nb[v]) avg += before[u];
                            cur += (avg / nb[v].size() - cur) * k;
                        }
                        w[v] = static_cast<float>(std::clamp(cur, 0.0, 1.0));
                        ++touched;
                    }
                }
                d.scene.paintBone = bone;
                Result r = done("painted " + bone + " on " + many(touched, "vertex"), d, p);
                double total = 0;
                for (float x : w) total += x;
                return r.with("bone", bone).with("touched", static_cast<std::int64_t>(touched)).with("average", w.empty() ? 0.0 : total / w.size());
            }));

    reg.add({"model.weight_show",
             "Show weights",
             "Colour meshes by one bone's weights in the viewport (blue 0, green half, red 1), as in weight paint mode; "
             "an empty bone goes back to the materials.",
             withDoc({{"bone", ParamType::String, "bone to show (empty: none)", false, {}, std::string()}}),
             true},
            edit(session, [](ModelDoc& d, const Params& p) {
                d.scene.paintBone = stringParam(p, "bone");
                return done(d.scene.paintBone.empty() ? "showing materials" : "showing the weights of " + d.scene.paintBone, d, p);
            }));

    reg.add({"model.rig_info",
             "Rig info",
             "An armature's bones (head, tail, parent, pose) and, for a skinned mesh, how many vertices each bone moves.",
             withDoc({{"armature", ParamType::String, "armature (default: the active one, or the one the active mesh is skinned to)", false}}, false),
             false,
             true},
            query(session, [](ModelDoc& d, const Params& p) {
                Object& a = armatureOf(d, p);
                std::string text = a.name + ": " + many(a.bones.size(), "bone");
                for (const auto& b : a.bones) {
                    text += "\n  " + b.name + (b.parent.empty() ? "" : " (child of " + b.parent + ")") + ": head " + num(b.head.x) + "," + num(b.head.y) +
                            "," + num(b.head.z) + " tail " + num(b.tail.x) + "," + num(b.tail.y) + "," + num(b.tail.z);
                    if (b.rotation.x || b.rotation.y || b.rotation.z) text += ", turned " + num(b.rotation.x) + "," + num(b.rotation.y) + "," + num(b.rotation.z);
                }
                std::string skinned;
                for (const auto& o : d.scene.objects) {
                    bool bound = false;
                    for (const auto& m : o.modifiers) bound = bound || (m.type == "armature" && m.str("object", "") == a.name);
                    if (!bound) continue;
                    skinned += (skinned.empty() ? "" : ",") + o.name;
                    text += "\n" + o.name + " is skinned to it:";
                    for (const auto& [bone, w] : o.weights) {
                        std::size_t n = 0;
                        for (float x : w) n += x > 0.001f;
                        text += " " + bone + " " + std::to_string(n);
                    }
                }
                Result r = Result::success(text);
                return r.with("armature", a.name).with("bones", boneList(a)).with("skinned", skinned);
            }));
}

}  // namespace wetwaresuite::model::cmd
