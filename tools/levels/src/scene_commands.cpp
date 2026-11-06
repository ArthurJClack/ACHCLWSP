// scene.* commands: build Godot scenes node by node, from scripts, Claude or the scene editor.
// Every edit is one SceneEdit, so it undoes in one step and a failed edit changes nothing.
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <functional>
#include <map>
#include <optional>
#include <set>
#include <sstream>

#include "wetwaresuite/levels/levels.hpp"
#include "wetwaresuite/levels/scene.hpp"
#include "wetwaresuite/levels/scene_play.hpp"
#include "wetwaresuite/raster/codec.hpp"

namespace wetwaresuite::scene {

namespace {

using Edit = std::function<Result(SceneDoc&, const Params&)>;

bool has(const Params& p, const std::string& key) {
    auto it = p.find(key);
    return it != p.end() && !std::holds_alternative<std::monostate>(it->second);
}

std::string trim(const std::string& s) {
    const auto a = s.find_first_not_of(" \t\r\n"), b = s.find_last_not_of(" \t\r\n");
    return a == std::string::npos ? std::string() : s.substr(a, b - a + 1);
}

std::string readAll(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    if (!in) throw std::runtime_error("can't read " + p.string());
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

void writeAll(const fs::path& p, const std::string& data) {
    std::error_code ec;
    if (p.has_parent_path()) fs::create_directories(p.parent_path(), ec);
    std::ofstream out(p, std::ios::binary);
    if (!out) throw std::runtime_error("can't write " + p.string());
    out << data;
    if (!out) throw std::runtime_error("can't write " + p.string());
}

ParamSpec docParam() { return {"doc", ParamType::String, "id of an open scene, e.g. scn1", true}; }
ParamSpec nodeParam(const std::string& what = "node path from the root: . is the root, Player/Sprite a child's child") {
    return {"node", ParamType::String, what, false, {}, std::string(".")};
}
ParamSpec propsParam() {
    return {"props", ParamType::String,
            "properties, semicolon-separated: position=16,32; rotation=45; texture=art/hero.png; shape=rect:16x32 "
            "(scene.inspect lists a node's properties)",
            false};
}
ParamSpec previewParam() { return {"preview", ParamType::Bool, "attach a picture of the scene", false, {}, false}; }

// "position=16,32; texture=a.png" -> pairs (values may hold commas; ; or newlines separate)
std::vector<std::pair<std::string, std::string>> parseProps(const std::string& text) {
    std::vector<std::pair<std::string, std::string>> out;
    std::string item;
    int depth = 0;
    bool str = false;
    auto flush = [&] {
        const std::string t = trim(item);
        item.clear();
        if (t.empty()) return;
        const auto eq = t.find('=');
        if (eq == std::string::npos) throw std::runtime_error("'" + t + "' needs a value: name=value");
        out.push_back({trim(t.substr(0, eq)), trim(t.substr(eq + 1))});
    };
    for (char c : text) {
        if (str) {
            if (c == '"') str = false;
            item += c;
            continue;
        }
        if (c == '"') str = true;
        if (c == '(' || c == '[') ++depth;
        if (c == ')' || c == ']') --depth;
        if ((c == ';' || c == '\n') && depth <= 0) flush();
        else item += c;
    }
    flush();
    return out;
}

// a value set on a property the inspector doesn't know: Godot syntax if it looks like it, else text
std::string rawValue(const std::string& v) {
    const std::string t = trim(v);
    if (t.empty()) return t;
    if (t == "true" || t == "false" || t.front() == '"' || t.front() == '&' || t.front() == '^') return t;
    if (std::isdigit(static_cast<unsigned char>(t.front())) || t.front() == '-' || t.front() == '.') {
        char* end = nullptr;
        std::strtod(t.c_str(), &end);
        if (!*end) return t;
    }
    if (std::isupper(static_cast<unsigned char>(t.front())) && t.find('(') != std::string::npos && t.back() == ')') return t;
    return gdString(t);
}

fs::path baseDir(const SceneDoc& doc) {
    if (!doc.scene.project.empty()) return doc.scene.project;
    if (!doc.file.empty()) return fs::absolute(doc.file).parent_path();
    return fs::current_path();
}

// relative file names in commands are read from the current folder, like every other tool
fs::path cwd() { return fs::current_path(); }

void applyProps(SceneDoc& doc, Node& n, const std::string& text) {
    for (const auto& [key, value] : parseProps(text)) {
        if (key == "name") throw std::runtime_error("rename with scene.rename");
        if (key == "script") {
            if (value.empty() || value == "none") {
                n.set("script", "");
                continue;
            }
            fs::path f = value;
            if (f.is_relative()) f = cwd() / f;
            if (!fs::exists(f)) throw std::runtime_error("no script " + f.string() + " (scene.script makes one)");
            n.set("script", "ExtResource(\"" + doc.scene.addExt("Script", f) + "\")");
            continue;
        }
        if (key == "groups") {
            n.groups.clear();
            std::stringstream ss(value);
            std::string g;
            while (std::getline(ss, g, ',')) {
                g = trim(g);
                if (!g.empty()) n.groups.push_back(g);
            }
            continue;
        }
        if (const PropInfo* info = propInfo(n.type, key)) {
            if (info->kind == PropKind::Color && key == "material_override" && n.type == "MeshInstance3D") {
                if (value.empty() || value == "none") {
                    n.set(key, "");
                    continue;
                }
                const std::string id = doc.scene.addSub("StandardMaterial3D");
                PropInfo c = *info;
                doc.scene.subById(id)->set("albedo_color", toGodot(doc.scene, n.type, c, value, cwd()));
                n.set(key, "SubResource(\"" + id + "\")");
                continue;
            }
            n.set(key, toGodot(doc.scene, n.type, *info, value, cwd()));
        } else {
            n.set(key, rawValue(value));
        }
    }
    // resources nothing uses any more are dropped when saving (the node may not be in the tree yet)
    (void)doc;
}

std::string cleanName(std::string name) {
    for (auto& c : name)
        if (std::string(".:@/\"%").find(c) != std::string::npos) c = '_';
    return trim(name);
}

std::string uniqueName(const Node& parent, const std::string& want, const Node* self = nullptr) {
    std::string base = cleanName(want);
    if (base.empty()) base = "Node";
    auto taken = [&](const std::string& n) {
        return std::any_of(parent.children.begin(), parent.children.end(), [&](const Node& c) { return &c != self && c.name == n; });
    };
    if (!taken(base)) return base;
    // Godot counts up: Sprite2D, Sprite2D2, Sprite2D3 ...
    std::string stem = base;
    while (!stem.empty() && std::isdigit(static_cast<unsigned char>(stem.back()))) stem.pop_back();
    for (int i = 2;; ++i)
        if (!taken(stem + std::to_string(i))) return stem + std::to_string(i);
}

std::string treeText(const Scene& s) {
    std::string out;
    std::function<void(const Node&, const std::string&, bool, bool)> walk = [&](const Node& n, const std::string& indent, bool last, bool isRoot) {
        out += indent + (isRoot ? "" : (last ? "└─ " : "├─ ")) + n.name + " (" + (n.type.empty() ? "instance" : n.type);
        if (!n.instance.empty()) {
            if (const ExtResource* e = s.extById(n.instance)) out += " of " + e->file.filename().string();
        }
        out += ")";
        if (n.prop("script")) {
            if (const ExtResource* e = s.extById(refId(*n.prop("script"), "ExtResource"))) out += " script " + e->file.filename().string();
        }
        if (!n.groups.empty()) {
            out += " groups";
            for (const auto& g : n.groups) out += " " + g;
        }
        out += "\n";
        for (std::size_t i = 0; i < n.children.size(); ++i)
            walk(n.children[i], isRoot ? "" : indent + (last ? "   " : "│  "), i + 1 == n.children.size(), false);
    };
    walk(s.root, "", true, true);
    if (!out.empty()) out.pop_back();
    return out;
}

Result& attachPreview(Result& r, const SceneDoc& doc, const Params& p) {
    if (!r.ok || !boolParam(p, "preview", false)) return r;
    if (auto a = doc.preview()) r.attach(*a);
    return r;
}

ActionFactory sceneCommand(Session& session, Edit edit, bool undoable = true) {
    return [&session, edit, undoable](const Params& p) -> std::unique_ptr<Action> {
        const std::string id = stringParam(p, "doc");
        auto doc = session.workspace().getAs<SceneDoc>(id);
        if (!doc) {
            return std::make_unique<FunctionAction>([id] { return Result::failure("no open scene called '" + id + "' (scene.new or scene.open first)"); });
        }
        auto run = [edit](SceneDoc& d, const Params& q) {
            Result r = edit(d, q);
            r.with("doc", stringParam(q, "doc"));
            return attachPreview(r, d, q);
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
        return std::make_unique<SceneEdit>(doc, [run, p](SceneDoc& d) { return run(d, p); });
    };
}

Node* need(SceneDoc& doc, const Params& p, const std::string& key = "node") {
    const std::string path = stringParam(p, key, ".");
    Node* n = doc.scene.find(path);
    if (!n) throw std::runtime_error("no node '" + path + "' (scene.tree lists them)");
    return n;
}

Result created(Session& session, std::shared_ptr<SceneDoc> doc, const std::string& verb, const Params& p) {
    const std::string id = session.workspace().add(doc, "scn");
    Result r = Result::success(verb + " " + id + ": " + doc->summary() + "\n" + treeText(doc->scene), id);
    r.with("doc", id).with("dims", static_cast<std::int64_t>(dimsOf(doc->scene)));
    return attachPreview(r, *doc, p);
}

class CloseAction : public Action {
public:
    CloseAction(Session& session, std::string id) : session_(session), id_(std::move(id)) {}
    Result apply() override {
        doc_ = session_.workspace().take(id_);
        if (!doc_) return Result::failure("no open document called '" + id_ + "'");
        return Result::success("closed " + id_);
    }
    void revert() override {
        if (doc_) session_.workspace().put(id_, doc_);
    }

private:
    Session& session_;
    std::string id_;
    std::shared_ptr<Document> doc_;
};

std::string classList(int dims, const std::string& base) {
    std::string out;
    for (const auto& c : classes()) {
        if (c.doc.empty()) continue;  // abstract bases
        if (dims && c.dims != dims && c.dims != 0) continue;
        if (!base.empty() && !inherits(c.name, base)) continue;
        out += "\n  " + c.name + ": " + c.doc;
    }
    return out;
}

void registerFileCommands(Session& session) {
    auto& reg = session.registry();
    reg.add({"scene.new", "New Scene",
             "Start a Godot scene with one root node. 2D games usually start from Node2D, 3D from Node3D, a player from "
             "CharacterBody2D/3D, a menu from Control. Save it as .tscn to open in Godot.",
             {{"root", ParamType::String, "the root node's class (scene.classes lists them)", false, {}, std::string("Node2D")},
              {"name", ParamType::String, "the root node's name (default: the class name)", false},
              {"path", ParamType::String, "where it will be saved (.tscn); sets the Godot project folder (the one with project.godot above it)", false},
              previewParam()},
             false, true},
            [&session](const Params& p) -> std::unique_ptr<Action> {
                return std::make_unique<FunctionAction>([&session, p] {
                    const std::string type = stringParam(p, "root", "Node2D");
                    const ClassInfo* c = classInfo(type);
                    if (!c) return Result::failure("no class '" + type + "' (scene.classes lists them)");
                    auto doc = std::make_shared<SceneDoc>();
                    doc->scene.root.type = c->name;
                    doc->scene.root.name = cleanName(has(p, "name") ? stringParam(p, "name") : c->name);
                    if (has(p, "path")) {
                        doc->setFile(fs::absolute(stringParam(p, "path")));
                        doc->scene.project = projectRoot(doc->file);
                    }
                    return created(session, doc, "created", p);
                });
            });

    reg.add({"scene.open", "Open Scene", "Open a Godot 4 scene (.tscn). Resources are found through the project folder (the one holding project.godot).",
             {{"path", ParamType::String, ".tscn file", true}, previewParam()}, false, true},
            [&session](const Params& p) -> std::unique_ptr<Action> {
                return std::make_unique<FunctionAction>([&session, p] {
                    try {
                        const fs::path path = fs::absolute(stringParam(p, "path"));
                        auto doc = std::make_shared<SceneDoc>();
                        doc->scene = readTscn(readAll(path), path);
                        doc->setFile(path);
                        std::vector<std::string> missing;
                        for (const auto& e : doc->scene.ext)
                            if (!fs::exists(e.file)) missing.push_back(e.file.string());
                        Result r = created(session, doc, "opened", p);
                        if (!missing.empty()) {
                            r.message += "\nwarning: files not found:";
                            for (const auto& m : missing) r.message += " " + m;
                        }
                        return r;
                    } catch (const std::exception& e) {
                        return Result::failure(e.what());
                    }
                });
            });

    reg.add({"scene.save", "Save Scene",
             "Save as a Godot 4 .tscn. File paths become res:// paths when they are inside the Godot project folder "
             "(the one with project.godot, or the scene's own folder), so save scenes inside the project.",
             {docParam(), {"path", ParamType::String, "the .tscn file (default: where it was opened or last saved)", false}}, false},
            sceneCommand(
                session,
                [](SceneDoc& doc, const Params& p) {
                    fs::path path = has(p, "path") ? fs::absolute(stringParam(p, "path")) : doc.file;
                    if (path.empty()) return Result::failure("give path= (a .tscn file)");
                    if (path.extension() != ".tscn") path += ".tscn";
                    if (has(p, "path")) doc.scene.project = projectRoot(path);
                    if (doc.scene.project.empty()) doc.scene.project = projectRoot(path);
                    writeAll(path, writeTscn(doc.scene, path));
                    doc.setFile(path);
                    std::vector<std::string> outside;
                    for (const auto& e : doc.scene.ext)
                        if (resPath(e.file, doc.scene.project).rfind("res://", 0) != 0) outside.push_back(e.file.filename().string());
                    std::string msg = "saved " + path.string();
                    if (!outside.empty()) {
                        msg += "\nwarning: outside the project folder " + doc.scene.project.string() + ", so Godot won't find them:";
                        for (const auto& o : outside) msg += " " + o;
                    }
                    return Result::success(msg).with("path", path.string());
                },
                false));

    reg.add({"scene.close", "Close Scene", "Close an open scene (undo reopens it).", {docParam()}, true},
            [&session](const Params& p) -> std::unique_ptr<Action> { return std::make_unique<CloseAction>(session, stringParam(p, "doc")); });

    reg.add({"scene.project", "Make Godot Project",
             "Write a project.godot in a folder so it opens in Godot, with the main scene, window size, the renderer and "
             "the scene's folder as its resources. Existing project files are updated, not replaced.",
             {{"dir", ParamType::String, "the project folder", true},
              {"name", ParamType::String, "the game's name", false, {}, std::string("New Game")},
              {"main_scene", ParamType::String, "the scene the game starts with (a .tscn inside the folder)", false},
              {"width", ParamType::Int, "window width", false, {}, std::int64_t{1152}},
              {"height", ParamType::Int, "window height", false, {}, std::int64_t{648}},
              {"pixel_art", ParamType::Bool, "sharp pixels (nearest filtering) and integer scaling", false, {}, false},
              {"renderer", ParamType::String, "forward_plus (desktop), mobile, or gl_compatibility (oldest machines, web)", false,
               {"forward_plus", "mobile", "gl_compatibility"}, std::string("gl_compatibility")}},
             false},
            [](const Params& p) -> std::unique_ptr<Action> {
                return std::make_unique<FunctionAction>([p] {
                    try {
                        const fs::path dir = fs::absolute(stringParam(p, "dir"));
                        std::ostringstream o;
                        o << "; Engine configuration file.\n; Written by wetwaresuite; Godot keeps it up to date from here on.\n\nconfig_version=5\n\n";
                        o << "[application]\n\nconfig/name=" << gdString(stringParam(p, "name", "New Game")) << "\n";
                        if (has(p, "main_scene")) {
                            fs::path m = stringParam(p, "main_scene");
                            if (m.is_relative()) m = dir / m;
                            o << "run/main_scene=" << gdString(resPath(m, dir)) << "\n";
                        }
                        o << "config/features=PackedStringArray(\"4.3\")\n\n";
                        o << "[display]\n\nwindow/size/viewport_width=" << intParam(p, "width", 1152) << "\nwindow/size/viewport_height=" << intParam(p, "height", 648)
                          << "\n";
                        const bool pixel = boolParam(p, "pixel_art", false);
                        if (pixel) o << "window/stretch/mode=\"viewport\"\nwindow/stretch/scale_mode=\"integer\"\n";
                        o << "\n";
                        if (pixel) o << "[rendering]\n\ntextures/canvas_textures/default_texture_filter=0\n";
                        const std::string renderer = stringParam(p, "renderer", "gl_compatibility");
                        o << (pixel ? "" : "[rendering]\n\n") << "renderer/rendering_method=" << gdString(renderer) << "\n";
                        if (renderer == "gl_compatibility") o << "renderer/rendering_method.mobile=\"gl_compatibility\"\n";
                        writeAll(dir / "project.godot", o.str());
                        return Result::success("wrote " + (dir / "project.godot").string() + ": open the folder in Godot (Import) and press Play")
                            .with("path", (dir / "project.godot").string());
                    } catch (const std::exception& e) {
                        return Result::failure(e.what());
                    }
                });
            });
}

void registerTreeCommands(Session& session) {
    auto& reg = session.registry();
    reg.add({"scene.tree", "Scene Tree", "The node tree, like Godot's Scene dock: names, classes, instanced scenes, scripts and groups.",
             {docParam(), previewParam()}, false, true},
            sceneCommand(
                session,
                [](SceneDoc& doc, const Params&) {
                    std::vector<std::string> paths;
                    std::function<void(const Node&, const std::string&)> walk = [&](const Node& n, const std::string& path) {
                        paths.push_back(path);
                        for (const auto& c : n.children) walk(c, path == "." ? c.name : path + "/" + c.name);
                    };
                    walk(doc.scene.root, ".");
                    std::string types, all;
                    std::function<void(const Node&)> collect = [&](const Node& n) {
                        types += (types.empty() ? "" : ",") + (n.type.empty() ? std::string("instance") : n.type);
                        for (const auto& c : n.children) collect(c);
                    };
                    collect(doc.scene.root);
                    for (const auto& x : paths) all += (all.empty() ? "" : ",") + x;
                    return Result::success(treeText(doc.scene))
                        .with("nodes", all)
                        .with("types", types)
                        .with("count", static_cast<std::int64_t>(paths.size()))
                        .with("dims", static_cast<std::int64_t>(dimsOf(doc.scene)))
                        .with("path", doc.file.string());
                },
                false));

    reg.add({"scene.classes", "Node Classes", "The node classes you can add, with what each is for (Godot's Create New Node list).",
             {{"dims", ParamType::Int, "2 or 3 for just 2D or 3D nodes (0: all)", false, {}, std::int64_t{0}},
              {"base", ParamType::String, "only classes inheriting this one, e.g. PhysicsBody2D", false}},
             false, true},
            [](const Params& p) -> std::unique_ptr<Action> {
                return std::make_unique<FunctionAction>([p] {
                    const auto list = classList(static_cast<int>(intParam(p, "dims", 0)), stringParam(p, "base"));
                    std::string names;
                    for (const auto& c : classes())
                        if (!c.doc.empty() && list.find("\n  " + c.name + ":") != std::string::npos) names += (names.empty() ? "" : ",") + c.name;
                    return Result::success("node classes:" + list).with("classes", names);
                });
            });

    reg.add({"scene.add", "Add Node",
             "Add a child node, like Godot's Add Child Node. Give the class, where (parent) and properties in one go, e.g. "
             "type=Sprite2D parent=Player props=\"texture=art/hero.png; position=0,-8\". A body needs a CollisionShape2D/3D "
             "child (shape=rect:16x32) to collide.",
             {docParam(),
              {"type", ParamType::String, "node class (scene.classes)", true},
              {"name", ParamType::String, "node name (default: the class name)", false},
              {"parent", ParamType::String, "parent node path (default: the root)", false, {}, std::string(".")},
              propsParam(),
              {"index", ParamType::Int, "position among the parent's children (default: last)", false},
              previewParam()},
             true},
            sceneCommand(session, [](SceneDoc& doc, const Params& p) {
                const ClassInfo* c = classInfo(stringParam(p, "type"));
                if (!c) return Result::failure("no class '" + stringParam(p, "type") + "' (scene.classes lists them)");
                Node* parent = need(doc, p, "parent");
                Node n;
                n.type = c->name;
                n.name = uniqueName(*parent, has(p, "name") ? stringParam(p, "name") : c->name);
                if (has(p, "props")) applyProps(doc, n, stringParam(p, "props"));
                const std::string parentPath = doc.scene.pathOf(parent);
                auto at = parent->children.end();
                if (has(p, "index")) at = parent->children.begin() + std::clamp<std::int64_t>(intParam(p, "index"), 0, static_cast<std::int64_t>(parent->children.size()));
                parent->children.insert(at, n);
                const std::string path = parentPath == "." ? n.name : parentPath + "/" + n.name;
                std::string hint;
                if ((inherits(c->name, "CollisionObject2D") || inherits(c->name, "CollisionObject3D")))
                    hint = "\nnext: give it a shape, scene.add type=" + std::string(c->dims == 3 ? "CollisionShape3D" : "CollisionShape2D") + " parent=" + path +
                           " props=\"shape=" + (c->dims == 3 ? "box:1x2x1" : "rect:16x32") + "\"";
                return Result::success("added " + path + " (" + c->name + ")" + hint).with("node", path);
            }));

    reg.add({"scene.set", "Set Properties",
             "Change a node's properties, like Godot's Inspector: props=\"position=100,50; scale=2; modulate=#ff8080\". "
             "Angles are degrees, colours #rrggbb or names, files relative to the current folder, shapes rect:WxH / "
             "circle:R / capsule:RxH (2D) or box:WxHxD / sphere:R (3D), meshes box: / sphere: / cylinder: / plane: or a "
             ".obj file. script=player.gd attaches a script, groups=enemies,hurtbox sets groups, prop=default resets one.",
             {docParam(), nodeParam(), propsParam(), previewParam()}, true},
            sceneCommand(session, [](SceneDoc& doc, const Params& p) {
                Node* n = need(doc, p);
                if (!has(p, "props")) return Result::failure("give props=\"name=value; ...\" (scene.inspect lists them)");
                applyProps(doc, *n, stringParam(p, "props"));
                return Result::success("set " + stringParam(p, "props") + " on " + doc.scene.pathOf(n)).with("node", doc.scene.pathOf(n));
            }));

    reg.add({"scene.inspect", "Inspect Node",
             "A node's properties, like Godot's Inspector: every property this editor knows for its class (with the "
             "class it comes from, its value or default, and what it does), then any others it has.",
             {docParam(), nodeParam()}, false, true},
            sceneCommand(
                session,
                [](SceneDoc& doc, const Params& p) {
                    const Node* n = need(doc, p);
                    std::string s = doc.scene.pathOf(n) + " (" + (n->type.empty() ? std::string("instance") : n->type) + ")";
                    Result r = Result::success("");
                    std::set<std::string> shown;
                    std::string names, values, kinds, sections, choices;
                    auto kindName = [](PropKind k) {
                        switch (k) {
                            case PropKind::Bool: return "bool";
                            case PropKind::Int: return "int";
                            case PropKind::Float: return "float";
                            case PropKind::Angle: return "angle";
                            case PropKind::Vec2: return "vec2";
                            case PropKind::Vec3: return "vec3";
                            case PropKind::Angle3: return "angle3";
                            case PropKind::Color: return "color";
                            case PropKind::String: return "string";
                            case PropKind::Texture: return "texture";
                            case PropKind::Mesh: return "mesh";
                            case PropKind::Shape2D: return "shape2d";
                            case PropKind::Shape3D: return "shape3d";
                            case PropKind::Stream: return "stream";
                            case PropKind::Points: return "points";
                            case PropKind::Enum: return "enum";
                        }
                        return "string";
                    };
                    const auto chain = lineage(n->type);
                    for (auto it = chain.rbegin(); it != chain.rend(); ++it) {
                        const ClassInfo* c = *it;
                        if (c->props.empty()) continue;
                        s += "\n" + c->name + ":";
                        for (const auto& pi : c->props) {
                            if (shown.count(pi.name)) continue;
                            shown.insert(pi.name);
                            const std::string* v = n->prop(pi.name);
                            std::string shownValue = v ? fromGodot(doc.scene, pi, *v) : (pi.def.empty() ? std::string("(none)") : fromGodot(doc.scene, pi, pi.def));
                            if (pi.kind == PropKind::Color && pi.name == "material_override" && v) {
                                if (const SubResource* m = doc.scene.subById(refId(*v, "SubResource"))) {
                                    if (m->prop("albedo_color")) shownValue = fromGodot(doc.scene, pi, *m->prop("albedo_color"));
                                }
                            }
                            s += "\n  " + pi.name + " = " + shownValue + (v ? "" : "  (default)");
                            if (!pi.doc.empty()) s += "  - " + pi.doc;
                            if (pi.kind == PropKind::Enum) {
                                s += " [";
                                for (std::size_t i = 0; i < pi.choices.size(); ++i) s += (i ? ", " : "") + pi.choices[i];
                                s += "]";
                            }
                            names += (names.empty() ? "" : "|") + pi.name;
                            values += (values.empty() ? "" : "|") + shownValue;
                            kinds += (kinds.empty() ? "" : "|") + std::string(kindName(pi.kind)) + (v ? "" : "*");
                            sections += (sections.empty() ? "" : "|") + c->name;
                            std::string ch;
                            for (const auto& x : pi.choices) ch += (ch.empty() ? "" : ";") + x;
                            choices += (names.find('|') == std::string::npos ? "" : "|") + ch;
                        }
                    }
                    std::string extra;
                    for (const auto& [k, v] : n->props) {
                        if (shown.count(k)) continue;
                        if (k == "script") {
                            if (const ExtResource* e = doc.scene.extById(refId(v, "ExtResource"))) extra += "\n  script = " + e->file.string();
                            continue;
                        }
                        extra += "\n  " + k + " = " + v;
                    }
                    if (!extra.empty()) s += "\nother:" + extra;
                    if (!n->groups.empty()) {
                        s += "\ngroups:";
                        for (const auto& g : n->groups) s += " " + g;
                    }
                    r.message = s;
                    std::string script;
                    if (n->prop("script")) {
                        if (const ExtResource* e = doc.scene.extById(refId(*n->prop("script"), "ExtResource"))) script = e->file.string();
                    }
                    std::string groups;
                    for (const auto& g : n->groups) groups += (groups.empty() ? "" : ",") + g;
                    return r.with("node", doc.scene.pathOf(n)).with("type", n->type).with("names", names).with("values", values).with("kinds", kinds)
                        .with("sections", sections).with("choices", choices).with("script", script).with("groups", groups);
                },
                false));

    reg.add({"scene.remove", "Delete Node", "Delete a node and everything under it (not the root).", {docParam(), nodeParam("the node to delete"), previewParam()}, true},
            sceneCommand(session, [](SceneDoc& doc, const Params& p) {
                const std::string path = stringParam(p, "node");
                Node* parent = doc.scene.parentOf(path);
                if (!parent) return Result::failure(doc.scene.find(path) ? "the root can't be deleted (scene.new starts another)" : "no node '" + path + "'");
                Node* target = doc.scene.find(path);
                const std::string full = doc.scene.pathOf(target);
                std::erase_if(parent->children, [&](const Node& c) { return &c == target; });
                std::erase_if(doc.scene.connections, [&](const Connection& c) { return c.from == full || c.to == full || c.from.rfind(full + "/", 0) == 0 || c.to.rfind(full + "/", 0) == 0; });
                doc.scene.prune();
                return Result::success("deleted " + full);
            }));

    reg.add({"scene.rename", "Rename Node", "Rename a node (names are unique among siblings; . : @ / \" % aren't allowed).",
             {docParam(), nodeParam(), {"name", ParamType::String, "the new name", true}}, true},
            sceneCommand(session, [](SceneDoc& doc, const Params& p) {
                Node* n = need(doc, p);
                const std::string old = doc.scene.pathOf(n);
                Node* parent = doc.scene.parentOf(old);
                const std::string name = parent ? uniqueName(*parent, stringParam(p, "name"), n) : cleanName(stringParam(p, "name"));
                if (name.empty()) return Result::failure("give a name");
                n->name = name;
                const std::string now = doc.scene.pathOf(n);
                for (auto& c : doc.scene.connections) {
                    if (c.from == old) c.from = now;
                    if (c.to == old) c.to = now;
                }
                return Result::success("renamed " + old + " to " + now).with("node", now);
            }));

    reg.add({"scene.move", "Move Node",
             "Give a node a new parent or a new place among its siblings (drag and drop in Godot's Scene dock). Its local "
             "position stays as it is.",
             {docParam(), nodeParam(), {"parent", ParamType::String, "the new parent (default: the same one)", false},
              {"index", ParamType::Int, "place among the siblings, 0 = first (default: last)", false}, previewParam()},
             true},
            sceneCommand(session, [](SceneDoc& doc, const Params& p) {
                const std::string path = stringParam(p, "node");
                Node* from = doc.scene.parentOf(path);
                if (!from) return Result::failure("the root can't move");
                Node* target = doc.scene.find(path);
                Node copy = *target;
                const std::string oldPath = doc.scene.pathOf(target);
                const std::string newParentPath = has(p, "parent") ? stringParam(p, "parent") : doc.scene.pathOf(from);
                Node* np = doc.scene.find(newParentPath);
                if (!np) return Result::failure("no node '" + newParentPath + "'");
                const std::string npPath = doc.scene.pathOf(np);
                if (npPath == oldPath || npPath.rfind(oldPath + "/", 0) == 0) return Result::failure("a node can't move inside itself");
                const auto oldIndex = static_cast<std::size_t>(std::find_if(from->children.begin(), from->children.end(), [&](const Node& c) { return &c == target; }) - from->children.begin());
                from->children.erase(from->children.begin() + static_cast<std::ptrdiff_t>(oldIndex));
                np = doc.scene.find(npPath);  // pointers moved
                copy.name = uniqueName(*np, copy.name);
                std::size_t at = np->children.size();
                if (has(p, "index")) at = static_cast<std::size_t>(std::clamp<std::int64_t>(intParam(p, "index"), 0, static_cast<std::int64_t>(np->children.size())));
                np->children.insert(np->children.begin() + static_cast<std::ptrdiff_t>(at), copy);
                const std::string now = npPath == "." ? copy.name : npPath + "/" + copy.name;
                for (auto& c : doc.scene.connections) {
                    auto fix = [&](std::string& s) {
                        if (s == oldPath) s = now;
                        else if (s.rfind(oldPath + "/", 0) == 0) s = now + s.substr(oldPath.size());
                    };
                    fix(c.from);
                    fix(c.to);
                }
                return Result::success("moved " + oldPath + " to " + now).with("node", now);
            }));

    reg.add({"scene.duplicate", "Duplicate Node", "Copy a node and its children next to it (Ctrl+D in Godot).",
             {docParam(), nodeParam(), {"name", ParamType::String, "the copy's name", false}, propsParam(), previewParam()}, true},
            sceneCommand(session, [](SceneDoc& doc, const Params& p) {
                const std::string path = stringParam(p, "node");
                Node* parent = doc.scene.parentOf(path);
                if (!parent) return Result::failure("the root can't be duplicated");
                Node copy = *doc.scene.find(path);
                copy.name = uniqueName(*parent, has(p, "name") ? stringParam(p, "name") : copy.name);
                if (has(p, "props")) applyProps(doc, copy, stringParam(p, "props"));
                const std::string pp = doc.scene.pathOf(parent);
                const Node* orig = doc.scene.find(path);
                auto it = std::find_if(parent->children.begin(), parent->children.end(), [&](const Node& c) { return &c == orig; });
                parent->children.insert(it + 1, copy);
                const std::string now = pp == "." ? copy.name : pp + "/" + copy.name;
                // like Godot's Ctrl+D, the copy keeps the signal connections of the node and its children
                const std::string from = doc.scene.pathOf(doc.scene.find(path));
                std::vector<Connection> more;
                for (const auto& c : doc.scene.connections) {
                    auto moved = [&](const std::string& q) -> std::optional<std::string> {
                        if (q == from) return now;
                        if (q.rfind(from + "/", 0) == 0) return now + q.substr(from.size());
                        return std::nullopt;
                    };
                    const auto f = moved(c.from), t = moved(c.to);
                    if (!f && !t) continue;
                    Connection d = c;
                    if (f) d.from = *f;
                    if (t) d.to = *t;
                    more.push_back(d);
                }
                for (auto& c : more) doc.scene.connections.push_back(c);
                return Result::success("duplicated " + path + " as " + now + (more.empty() ? "" : " with its " + std::to_string(more.size()) + " signal connection" + (more.size() == 1 ? "" : "s")))
                    .with("node", now);
            }));
}

void registerResourceCommands(Session& session) {
    auto& reg = session.registry();
    reg.add({"scene.instance", "Instance Scene",
             "Put another scene inside this one as a child (Godot's Instantiate Child Scene): a .tscn (a player, an enemy, "
             "a level), or a .glb/.gltf model. Change its properties on the instance with scene.set.",
             {docParam(), {"path", ParamType::String, "the .tscn, .glb or .gltf file", true}, {"parent", ParamType::String, "parent node", false, {}, std::string(".")},
              {"name", ParamType::String, "node name (default: the file's root name)", false}, propsParam(), previewParam()},
             true},
            sceneCommand(session, [](SceneDoc& doc, const Params& p) {
                fs::path f = stringParam(p, "path");
                if (f.is_relative()) f = cwd() / f;
                if (!fs::exists(f)) return Result::failure("no file " + f.string());
                Node* parent = need(doc, p, "parent");
                const std::string parentPath = doc.scene.pathOf(parent);
                std::string name = f.stem().string();
                std::string type;
                if (f.extension() == ".tscn") {
                    const Scene inner = readTscn(readAll(f), f);
                    name = inner.root.name;
                    type = inner.root.type;
                }
                Node n;
                n.name = uniqueName(*parent, has(p, "name") ? stringParam(p, "name") : name);
                n.instance = doc.scene.addExt("PackedScene", f);
                // the inspector needs a class to know properties: use the instanced root's (not written to the file)
                Node probe = n;
                probe.type = type.empty() ? "Node3D" : type;
                if (has(p, "props")) {
                    applyProps(doc, probe, stringParam(p, "props"));
                    n.props = probe.props;
                }
                parent = doc.scene.find(parentPath);
                parent->children.push_back(n);
                const std::string path = parentPath == "." ? n.name : parentPath + "/" + n.name;
                return Result::success("instanced " + f.filename().string() + " as " + path).with("node", path);
            }));

    reg.add({"scene.add_level", "Add Level",
             "Put a tile level from levels (an open level or a .tmx/.tmj/.json/.ldtk file) into the scene: the level is "
             "exported as its own Godot scene (TileMap, TileSet with collision, image layers, objects) next to the file "
             "you name, and instanced here, so editing the level and adding it again updates the game.",
             {docParam(), {"level", ParamType::String, "an open level id (lvl1) or a map file", true},
              {"path", ParamType::String, "where the level's .tscn goes (inside the Godot project)", true},
              {"parent", ParamType::String, "parent node", false, {}, std::string(".")}, {"name", ParamType::String, "node name", false}, previewParam()},
             true},
            [&session](const Params& p) -> std::unique_ptr<Action> {
                // the level is read now, from the workspace or a file
                std::shared_ptr<levels::Map> map;
                fs::path source;
                const std::string level = stringParam(p, "level");
                if (auto ld = session.workspace().getAs<levels::LevelDoc>(level)) {
                    map = std::make_shared<levels::Map>(ld->map);
                } else {
                    source = fs::absolute(level);
                    try {
                        map = std::make_shared<levels::Map>(levels::loadMap(source));
                    } catch (const std::exception& e) {
                        const std::string msg = "no open level or readable map '" + level + "': " + e.what();
                        return std::make_unique<FunctionAction>([msg] { return Result::failure(msg); });
                    }
                }
                return sceneCommand(session, [map, source](SceneDoc& doc, const Params& q) {
                    const fs::path out = fs::absolute(stringParam(q, "path"));
                    std::vector<std::string> warnings;
                    const std::string text = levels::toGodotScene(*map, out.parent_path(), out.stem().string(), &warnings);
                    Scene lvl = readTscn(text, out);
                    // keep where the tiles came from, so previews draw them; an open level is saved beside the scene
                    fs::path src = source;
                    if (src.empty()) {
                        src = out;
                        src.replace_extension(".tmj");
                        levels::saveMap(*map, src, "tmj");
                    }
                    std::function<void(Node&)> mark = [&](Node& n) {
                        if (n.type == "TileMap" || n.type == "TileMapLayer") n.set("metadata/wetwaresuite_source", gdString(resPath(src, projectRoot(out))));
                        for (auto& c : n.children) mark(c);
                    };
                    mark(lvl.root);
                    writeAll(out, writeTscn(lvl, out));
                    Node* parent = need(doc, q, "parent");
                    Node n;
                    n.name = uniqueName(*parent, has(q, "name") ? stringParam(q, "name") : lvl.root.name);
                    n.instance = doc.scene.addExt("PackedScene", out);
                    const std::string pp = doc.scene.pathOf(parent);
                    doc.scene.find(pp)->children.push_back(n);
                    std::string msg = "wrote " + out.string() + " and instanced it as " + (pp == "." ? n.name : pp + "/" + n.name);
                    for (const auto& w : warnings) msg += "\nwarning: " + w;
                    return Result::success(msg).with("node", pp == "." ? n.name : pp + "/" + n.name).with("path", out.string());
                })(p);
            });

    reg.add({"scene.script", "Attach Script",
             "Make a GDScript for a node and attach it (or attach one that exists). Templates: empty (_ready/_process), "
             "character2d (platformer movement with gravity and jump), topdown2d (8-way movement), character3d, area "
             "(reacts to bodies entering).",
             {docParam(), nodeParam(), {"path", ParamType::String, "the .gd file (made when missing)", true},
              {"template", ParamType::String, "what the new script starts with", false, {"empty", "character2d", "topdown2d", "character3d", "area"}, std::string("empty")}},
             true},
            sceneCommand(session, [](SceneDoc& doc, const Params& p) {
                Node* n = need(doc, p);
                fs::path f = stringParam(p, "path");
                if (f.extension() != ".gd") f += ".gd";
                if (f.is_relative()) f = cwd() / f;
                std::string made;
                if (!fs::exists(f)) {
                    const std::string base = n->type.empty() ? "Node" : n->type;
                    writeAll(f, scriptTemplate(base, stringParam(p, "template", "empty")));
                    made = "wrote " + f.string() + " and ";
                }
                n->set("script", "ExtResource(\"" + doc.scene.addExt("Script", f) + "\")");
                return Result::success(made + "attached it to " + (n == &doc.scene.root ? n->name : doc.scene.pathOf(n))).with("path", f.string());
            }));

    reg.add({"scene.connect", "Connect Signal",
             "Connect a node's signal to a method (Godot's Node > Signals dock), e.g. signal=body_entered from=Coin to=. "
             "method=_on_coin_body_entered. remove=true disconnects. Write the method in the target's script.",
             {docParam(), {"signal", ParamType::String, "e.g. body_entered, timeout, pressed", true}, {"from", ParamType::String, "the node sending it", true},
              {"to", ParamType::String, "the node with the method", false, {}, std::string(".")}, {"method", ParamType::String, "default: _on_<from>_<signal>", false},
              {"remove", ParamType::Bool, "disconnect instead", false, {}, false}},
             true},
            sceneCommand(session, [](SceneDoc& doc, const Params& p) {
                const Node* from = need(doc, p, "from");
                const Node* to = need(doc, p, "to");
                Connection c;
                c.signal = stringParam(p, "signal");
                c.from = doc.scene.pathOf(from);
                c.to = doc.scene.pathOf(to);
                std::string snake;
                for (char ch : from->name) {
                    if (std::isupper(static_cast<unsigned char>(ch)) && !snake.empty() && snake.back() != '_') snake += '_';
                    snake += std::isalnum(static_cast<unsigned char>(ch)) ? static_cast<char>(std::tolower(static_cast<unsigned char>(ch))) : '_';
                }
                c.method = has(p, "method") ? stringParam(p, "method") : "_on_" + snake + "_" + c.signal;
                auto same = [&](const Connection& o) { return o.signal == c.signal && o.from == c.from && o.to == c.to && (o.method == c.method || !has(p, "method")); };
                if (boolParam(p, "remove", false)) {
                    const auto before = doc.scene.connections.size();
                    std::erase_if(doc.scene.connections, same);
                    if (before == doc.scene.connections.size()) return Result::failure("no such connection");
                    return Result::success("disconnected " + c.signal + " from " + c.from);
                }
                if (std::any_of(doc.scene.connections.begin(), doc.scene.connections.end(), same)) return Result::failure("already connected");
                doc.scene.connections.push_back(c);
                // like Godot, give the target's script the method when it doesn't have it yet
                std::string stub;
                if (const std::string* sc = to->prop("script")) {
                    if (const ExtResource* e = doc.scene.extById(refId(*sc, "ExtResource"))) {
                        std::error_code ec;
                        const std::string code = fs::exists(e->file, ec) ? readAll(e->file) : std::string();
                        if (!code.empty() && code.find("func " + c.method + "(") == std::string::npos) {
                            static const std::map<std::string, std::string> args = {
                                {"body_entered", "body: Node"}, {"body_exited", "body: Node"},       {"area_entered", "area: Area2D"},
                                {"area_exited", "area: Area2D"}, {"input_event", "viewport: Node, event: InputEvent, shape_idx: int"},
                                {"animation_finished", "anim_name: StringName"}, {"toggled", "toggled_on: bool"}, {"value_changed", "value: float"},
                                {"text_changed", "new_text: String"}, {"text_submitted", "new_text: String"}};
                            const auto it = args.find(c.signal);
                            std::string add = std::string(code.back() == '\n' ? "" : "\n") + "\n\nfunc " + c.method + "(" + (it == args.end() ? "" : it->second) + ") -> void:\n\tpass\n";
                            std::ofstream out(e->file, std::ios::binary | std::ios::app);
                            out << add;
                            if (out) stub = "\nadded func " + c.method + "() to " + e->file.filename().string();
                        }
                    }
                }
                return Result::success("connected " + (c.from == "." ? doc.scene.root.name : c.from) + "." + c.signal + " to " + (c.to == "." ? doc.scene.root.name : c.to) + "." + c.method + "()" + stub).with("method", c.method);
            }));
}

void registerViewCommands(Session& session) {
    auto& reg = session.registry();
    reg.add({"scene.preview", "Scene Picture",
             "A picture of the scene like Godot's editor viewport: 2D scenes with sprites, polygons, tiles and UI boxes "
             "plus collision shapes, cameras and markers; 3D scenes from an orbiting view or the Camera3D. Reports where "
             "the 2D world landed (origin_x, origin_y, scale), so clicks map back with scene.hit.",
             {docParam(), {"view", ParamType::String, "auto, 2d or 3d", false, {"auto", "2d", "3d"}, std::string("auto")},
              {"selected", ParamType::String, "node drawn highlighted", false},
              {"size", ParamType::Int, "longest side, px", false, {}, std::int64_t{1200}},
              {"helpers", ParamType::Bool, "draw collision shapes, cameras and markers", false, {}, true},
              {"origin_x", ParamType::Number, "2D: world x at the left edge (with origin_y, scale, width, height for a fixed view)", false},
              {"origin_y", ParamType::Number, "2D: world y at the top edge", false},
              {"scale", ParamType::Number, "2D: pixels per world unit", false},
              {"width", ParamType::Int, "fixed view width, px", false},
              {"height", ParamType::Int, "fixed view height, px", false},
              {"angle", ParamType::Number, "3D: orbit angle, degrees", false, {}, 35.0},
              {"tilt", ParamType::Number, "3D: tilt, degrees", false, {}, 25.0},
              {"camera", ParamType::Bool, "3D: look through the Camera3D", false, {}, false},
              {"path", ParamType::String, "also write the PNG here", false}},
             false, true},
            sceneCommand(
                session,
                [](SceneDoc& doc, const Params& p) {
                    std::string view = stringParam(p, "view", "auto");
                    if (view == "auto") view = dimsOf(doc.scene) == 3 ? "3d" : "2d";
                    const int size = static_cast<int>(std::clamp<std::int64_t>(intParam(p, "size", 1200), 64, 8192));
                    raster::Image img;
                    Result r = Result::success("");
                    if (view == "3d") {
                        const int w = static_cast<int>(has(p, "width") ? intParam(p, "width") : size);
                        const int h = static_cast<int>(has(p, "height") ? intParam(p, "height") : size * 2 / 3);
                        img = render3D(doc.scene, std::clamp(w, 16, 8192), std::clamp(h, 16, 8192), numberParam(p, "angle", 35), numberParam(p, "tilt", 25),
                                       boolParam(p, "camera", false), stringParam(p, "selected"));
                        r.message = "3D view " + std::to_string(img.width()) + "x" + std::to_string(img.height());
                    } else {
                        RenderOptions2D o;
                        o.maxSize = size;
                        o.helpers = boolParam(p, "helpers", true);
                        o.selected = stringParam(p, "selected");
                        if (has(p, "scale") && has(p, "width") && has(p, "height")) {
                            View2D v;
                            v.originX = numberParam(p, "origin_x", 0);
                            v.originY = numberParam(p, "origin_y", 0);
                            v.scale = std::max(1e-4, numberParam(p, "scale", 1));
                            v.width = static_cast<int>(std::clamp<std::int64_t>(intParam(p, "width"), 16, 8192));
                            v.height = static_cast<int>(std::clamp<std::int64_t>(intParam(p, "height"), 16, 8192));
                            o.view = v;
                        }
                        View2D used;
                        std::vector<Hit> hits;
                        img = render2D(doc.scene, o, &used, &hits);
                        for (const auto& h : hits) {
                            if (h.path != o.selected) continue;
                            r.with("sel_x0", h.x0).with("sel_y0", h.y0).with("sel_x1", h.x1).with("sel_y1", h.y1);
                        }
                        r.message = "2D view " + std::to_string(img.width()) + "x" + std::to_string(img.height()) + ", world " + gdNum(used.originX) + "," +
                                    gdNum(used.originY) + " at the top left, " + gdNum(used.scale) + " px per unit";
                        r.with("origin_x", used.originX).with("origin_y", used.originY).with("scale", used.scale);
                    }
                    const std::string png = raster::encodePng(img);
                    if (has(p, "path")) {
                        writeAll(stringParam(p, "path"), png);
                        r.message += "\nwrote " + stringParam(p, "path");
                    }
                    r.attach({"image/png", png, "scene.png"});
                    return r.with("width", static_cast<std::int64_t>(img.width())).with("height", static_cast<std::int64_t>(img.height())).with("view", view);
                },
                false));

    reg.add({"scene.play", "Play Test",
             "Run a 2D scene for a few seconds without Godot, like F6: platformer and top-down starter scripts move "
             "(SPEED and JUMP_VELOCITY are read from the script), rigid bodies fall, static bodies and tile levels are "
             "solid, areas report bodies entering and go away when the connected method calls queue_free(). Keys are "
             "held over times, e.g. input=\"right 0-1.5; jump 0.4; left 2-3\" (a single time taps the key). Reports "
             "where every body ended up and what happened; path= writes a .gif of the run or a .png of the last frame.",
             {docParam(), {"seconds", ParamType::Number, "how long to run", false, {}, 3.0},
              {"input", ParamType::String, "keys over time: left, right, up, down, jump, each with a time or from-to range in seconds", false},
              {"fps", ParamType::Int, "GIF frames per second", false, {}, std::int64_t{20}},
              {"scale", ParamType::Number, "picture size against the game window", false, {}, 0.5},
              {"path", ParamType::String, "write a .gif of the run or a .png of the last frame", false}},
             false, true},
            sceneCommand(
                session,
                [](SceneDoc& doc, const Params& p) {
                    struct Hold {
                        std::string key;
                        double from, to;
                    };
                    std::vector<Hold> holds;
                    {
                        std::string spec = stringParam(p, "input");
                        for (char& c : spec) {
                            if (c == ',') c = ';';
                        }
                        std::stringstream ss(spec);
                        std::string part;
                        while (std::getline(ss, part, ';')) {
                            part = trim(part);
                            if (part.empty()) continue;
                            const auto sp = part.find_first_of(" \t=");
                            const std::string key = trim(part.substr(0, sp));
                            if (key != "left" && key != "right" && key != "up" && key != "down" && key != "jump")
                                return Result::failure("unknown key '" + key + "' in input (use left, right, up, down, jump)");
                            const std::string when = sp == std::string::npos ? "0" : trim(part.substr(sp + 1));
                            const auto dash = when.find('-', 1);
                            Hold h{key, std::atof(when.c_str()), 0};
                            h.to = dash == std::string::npos ? h.from + 0.1 : std::atof(when.substr(dash + 1).c_str());
                            if (h.to < h.from) std::swap(h.from, h.to);
                            holds.push_back(h);
                        }
                    }
                    const double seconds = std::clamp(numberParam(p, "seconds", 3), 0.0, 120.0);
                    const int fps = static_cast<int>(std::clamp<std::int64_t>(intParam(p, "fps", 20), 1, 60));
                    const double scale = std::clamp(numberParam(p, "scale", 0.5), 0.1, 4.0);
                    const std::string path = stringParam(p, "path");
                    const bool gif = !path.empty() && fs::path(path).extension() == ".gif";
                    Play play(doc.scene);
                    std::vector<raster::Image> frames;
                    if (gif) frames.push_back(play.frame(scale));
                    const double dt = 1.0 / fps;
                    for (double t = 0; t < seconds - 1e-9; t += dt) {
                        PlayInput in;
                        for (const auto& h : holds) {
                            if (t + 1e-9 < h.from || t >= h.to) continue;
                            if (h.key == "left") in.left = true;
                            if (h.key == "right") in.right = true;
                            if (h.key == "up") in.up = true;
                            if (h.key == "down") in.down = true;
                            if (h.key == "jump") in.jump = true;
                        }
                        play.step(in, std::min(dt, seconds - t));
                        if (gif && frames.size() < 1200) frames.push_back(play.frame(scale));
                    }
                    std::ostringstream msg;
                    msg << "played " << gdNum(std::round(play.time() * 100) / 100) << " s";
                    Result r = Result::success("");
                    for (const auto& b : play.bodies()) {
                        msg << "\n" << b.path << " (" << b.type << "): " << (b.gone ? "gone, last at " : "at ") << gdNum(std::round(b.x * 10) / 10) << ", "
                            << gdNum(std::round(b.y * 10) / 10) << (b.onFloor ? ", on the floor" : "");
                        r.with(b.path + ".x", b.x).with(b.path + ".y", b.y).with(b.path + ".on_floor", b.onFloor);
                    }
                    for (const auto& e : play.events()) msg << "\n" << gdNum(std::round(e.time * 100) / 100) << " s: " << e.text;
                    for (const auto& n : play.notes()) msg << "\nnote: " << n;
                    const raster::Image last = play.frame(scale);
                    const std::string png = raster::encodePng(last);
                    if (!path.empty()) {
                        writeAll(path, gif ? raster::encodeGif(frames, static_cast<int>(std::lround(100.0 / fps))) : png);
                        msg << "\nwrote " << path;
                    }
                    r.message = msg.str();
                    r.attach({"image/png", png, "play.png"});
                    return r.with("events", static_cast<std::int64_t>(play.events().size())).with("freed", static_cast<std::int64_t>(play.freed()));
                },
                false));

    reg.add({"scene.play_step", "Play (Live)",
             "Play a 2D scene step by step, as the editor's Play button does: the first call starts it from the scene as "
             "it is, each call holds the given keys for `seconds` and returns the game window's picture and what "
             "happened. restart=true starts over, stop=true ends it. Same rules as scene.play.",
             {docParam(), {"keys", ParamType::String, "keys held this step, comma-separated: left, right, up, down, jump", false},
              {"seconds", ParamType::Number, "how long to run this step", false, {}, 1.0 / 30},
              {"scale", ParamType::Number, "picture size against the game window", false, {}, 1.0},
              {"restart", ParamType::Bool, "start again from the scene", false, {}, false},
              {"stop", ParamType::Bool, "end the play test", false, {}, false},
              {"raw", ParamType::Bool, "attach raw RGBA pixels (image/x-rgba) instead of a PNG, for live views", false, {}, false}},
             false, true},
            sceneCommand(
                session,
                [](SceneDoc& doc, const Params& p) {
                    if (boolParam(p, "stop", false)) {
                        doc.play.reset();
                        return Result::success("stopped");
                    }
                    if (!doc.play || boolParam(p, "restart", false)) {
                        if (dimsOf(doc.scene) == 3) return Result::failure("the play test runs 2D scenes; press Play in Godot for 3D ones");
                        doc.play = std::make_shared<Play>(doc.scene);
                        doc.playEventsSeen = 0;
                    }
                    PlayInput in;
                    const std::string keys = stringParam(p, "keys");
                    in.left = keys.find("left") != std::string::npos;
                    in.right = keys.find("right") != std::string::npos;
                    in.up = keys.find("up") != std::string::npos;
                    in.down = keys.find("down") != std::string::npos;
                    in.jump = keys.find("jump") != std::string::npos;
                    Play& play = *doc.play;
                    play.step(in, std::clamp(numberParam(p, "seconds", 1.0 / 30), 0.0, 1.0));
                    const raster::Image img = play.frame(std::clamp(numberParam(p, "scale", 1), 0.1, 4.0));
                    std::ostringstream msg;
                    msg << "t=" << gdNum(std::round(play.time() * 100) / 100) << " s";
                    Result r = Result::success("");
                    for (const auto& b : play.bodies()) {
                        msg << "\n" << b.path << ": " << (b.gone ? "gone" : gdNum(std::round(b.x)) + ", " + gdNum(std::round(b.y)));
                        r.with(b.path + ".x", b.x).with(b.path + ".y", b.y);
                    }
                    std::string fresh;
                    for (std::size_t i = doc.playEventsSeen; i < play.events().size(); ++i) {
                        msg << "\n" << play.events()[i].text;
                        fresh += (fresh.empty() ? "" : "\n") + play.events()[i].text;
                    }
                    doc.playEventsSeen = play.events().size();
                    if (doc.playEventsSeen == 0 && play.time() < 0.1) {
                        for (const auto& n : play.notes()) msg << "\nnote: " << n;
                    }
                    std::string notes;
                    for (const auto& n : play.notes()) notes += (notes.empty() ? "" : "\n") + n;
                    r.message = msg.str();
                    if (boolParam(p, "raw", false)) {
                        std::string bytes(reinterpret_cast<const char*>(img.data()), static_cast<std::size_t>(img.width()) * img.height() * 4);
                        r.attach({"image/x-rgba", std::move(bytes), "play.rgba"});
                    } else {
                        r.attach({"image/png", raster::encodePng(img), "play.png"});
                    }
                    return r.with("width", static_cast<std::int64_t>(img.width()))
                        .with("height", static_cast<std::int64_t>(img.height()))
                        .with("time", play.time())
                        .with("events", fresh)
                        .with("notes", notes)
                        .with("freed", static_cast<std::int64_t>(play.freed()));
                },
                false));

    reg.add({"scene.transform", "Move, Turn or Scale Node",
             "Move a node by a world-space distance (what dragging it in the viewport does), turn it by some degrees and "
             "scale it by a factor, keeping its parent's transform in mind. 2D: dx, dy in pixels; 3D: dx, dy, dz in metres "
             "in the parent's space, rotate about axis (y by default).",
             {docParam(), nodeParam(), {"dx", ParamType::Number, "move right by", false, {}, 0.0}, {"dy", ParamType::Number, "move down (2D) / up (3D) by", false, {}, 0.0},
              {"dz", ParamType::Number, "3D: move along z by", false, {}, 0.0}, {"rotate", ParamType::Number, "turn by, degrees (2D: clockwise)", false, {}, 0.0},
              {"axis", ParamType::String, "3D: axis to turn about", false, {"x", "y", "z"}, std::string("y")},
              {"scale", ParamType::Number, "multiply the scale by", false, {}, 1.0}, previewParam()},
             true},
            sceneCommand(session, [](SceneDoc& doc, const Params& p) {
                Node* n = need(doc, p);
                const double dx = numberParam(p, "dx", 0), dy = numberParam(p, "dy", 0), dz = numberParam(p, "dz", 0);
                const double rot = numberParam(p, "rotate", 0), k = numberParam(p, "scale", 1);
                const std::string path = doc.scene.pathOf(n);
                if (k <= 0) return Result::failure("scale must be above 0");
                const bool is3d = inherits(n->type, "Node3D") || (n->type.empty() && dimsOf(doc.scene) == 3);
                const bool isControl = inherits(n->type, "Control");
                auto vec = [&](const char* key, std::size_t count, double def) {
                    std::vector<double> v = n->prop(key) ? numbers(*n->prop(key)) : std::vector<double>{};
                    v.resize(count, def);
                    return v;
                };
                if (is3d) {
                    auto pos = vec("position", 3, 0);
                    pos[0] += dx, pos[1] += dy, pos[2] += dz;
                    n->set("position", "Vector3(" + gdNum(pos[0]) + ", " + gdNum(pos[1]) + ", " + gdNum(pos[2]) + ")");
                    if (rot != 0) {
                        auto r = vec("rotation", 3, 0);
                        const std::string ax = stringParam(p, "axis", "y");
                        r[ax == "x" ? 0 : ax == "z" ? 2 : 1] += rot * 3.14159265358979323846 / 180;
                        n->set("rotation", "Vector3(" + gdNum(r[0]) + ", " + gdNum(r[1]) + ", " + gdNum(r[2]) + ")");
                    }
                    if (k != 1) {
                        auto sc = vec("scale", 3, 1);
                        n->set("scale", "Vector3(" + gdNum(sc[0] * k) + ", " + gdNum(sc[1] * k) + ", " + gdNum(sc[2] * k) + ")");
                    }
                } else {
                    if (dx != 0 || dy != 0) {
                        // the world move in the parent's own space
                        Xform2 parent;
                        std::string acc;
                        std::vector<std::string> parts;
                        std::stringstream ss(path == "." ? std::string() : path);
                        for (std::string part; std::getline(ss, part, '/');) parts.push_back(part);
                        const Node* cur = &doc.scene.root;
                        if (!parts.empty()) {
                            parent = inherits(cur->type, "Node2D") || inherits(cur->type, "Control") ? localXform2(*cur) : Xform2{};
                            for (std::size_t i = 0; i + 1 < parts.size(); ++i) {
                                acc += (acc.empty() ? "" : "/") + parts[i];
                                cur = doc.scene.find(acc);
                                if (cur && (inherits(cur->type, "Node2D") || inherits(cur->type, "Control"))) parent = parent * localXform2(*cur);
                            }
                        }
                        const double det = parent.a * parent.d - parent.b * parent.c;
                        double lx = dx, ly = dy;
                        if (std::abs(det) > 1e-12) {
                            lx = (parent.d * dx - parent.c * dy) / det;
                            ly = (-parent.b * dx + parent.a * dy) / det;
                        }
                        auto pos = vec("position", 2, 0);
                        n->set("position", "Vector2(" + gdNum(pos[0] + lx) + ", " + gdNum(pos[1] + ly) + ")");
                    }
                    if (rot != 0) n->set("rotation", gdNum(vec("rotation", 1, 0)[0] + rot * 3.14159265358979323846 / 180));
                    if (k != 1) {
                        if (isControl && n->prop("size")) {
                            auto sz = vec("size", 2, 0);
                            n->set("size", "Vector2(" + gdNum(sz[0] * k) + ", " + gdNum(sz[1] * k) + ")");
                        } else {
                            auto sc = vec("scale", 2, 1);
                            n->set("scale", "Vector2(" + gdNum(sc[0] * k) + ", " + gdNum(sc[1] * k) + ")");
                        }
                    }
                }
                return Result::success("moved " + path).with("node", path);
            }));

    reg.add({"scene.hit", "Node At Point", "Which 2D node is drawn at a world point (the topmost), for clicking nodes in a picture.",
             {docParam(), {"x", ParamType::Number, "world x", true}, {"y", ParamType::Number, "world y", true},
              {"radius", ParamType::Number, "how close counts, world units (markers and thin lines)", false, {}, 4.0}},
             false, true},
            sceneCommand(
                session,
                [](SceneDoc& doc, const Params& p) {
                    std::vector<Hit> hits;
                    RenderOptions2D o;
                    o.maxSize = 64;  // only the boxes are wanted
                    render2D(doc.scene, o, nullptr, &hits);
                    const double x = numberParam(p, "x"), y = numberParam(p, "y"), rad = numberParam(p, "radius", 4);
                    // topmost first; a camera frame only counts near its edge, so it doesn't swallow every click
                    for (auto it = hits.rbegin(); it != hits.rend(); ++it) {
                        const bool inBox = x >= it->x0 - rad && x <= it->x1 + rad && y >= it->y0 - rad && y <= it->y1 + rad;
                        if (!inBox) continue;
                        if (it->type == "Camera2D") {
                            const double edge = std::min({std::abs(x - it->x0), std::abs(x - it->x1), std::abs(y - it->y0), std::abs(y - it->y1)});
                            if (edge > rad * 2) continue;
                        }
                        return Result::success(it->path + " (" + it->type + ")")
                            .with("node", it->path)
                            .with("type", it->type)
                            .with("x0", it->x0)
                            .with("y0", it->y0)
                            .with("x1", it->x1)
                            .with("y1", it->y1);
                    }
                    return Result::success("nothing there").with("node", std::string());
                },
                false));
}

}  // namespace

void registerCommands(Session& session) {
    registerFileCommands(session);
    registerTreeCommands(session);
    registerResourceCommands(session);
    registerViewCommands(session);
}

}  // namespace wetwaresuite::scene
