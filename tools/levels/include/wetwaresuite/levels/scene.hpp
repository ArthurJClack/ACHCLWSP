// scene: Godot-style scenes. A scene is a tree of nodes, each with a Godot class (Node2D,
// Sprite2D, CharacterBody2D, MeshInstance3D, Camera3D ...), a name and properties, plus the
// resources they use: files (textures, scripts, meshes, other scenes) and resources made in
// the scene (collision shapes, primitive meshes, materials). Scenes open and save as Godot 4
// .tscn text files, so a scene made here runs in Godot and a Godot scene edits here.
//
// Property values are kept as Godot writes them ("Vector2(16, 32)", "true", "0.5",
// "ExtResource(\"1_tex\")"), so properties this editor knows nothing about survive a round trip.
#pragma once

#include <algorithm>
#include <array>
#include <filesystem>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "wetwaresuite/core/command.hpp"
#include "wetwaresuite/raster/image.hpp"

namespace wetwaresuite::scene {

namespace fs = std::filesystem;

struct Node {
    std::string name, type;      // type is empty for an instanced scene's root
    std::vector<std::pair<std::string, std::string>> props;  // in file order
    std::string instance;        // ext resource id of a PackedScene this node instances
    std::vector<std::string> groups;
    std::string extraAttrs;      // header attributes this editor doesn't use, kept as written
    std::vector<Node> children;

    const std::string* prop(const std::string& key) const;
    void set(const std::string& key, const std::string& value);  // empty value removes it
};

struct ExtResource {
    std::string id, type;
    fs::path file;      // on disk (absolute, or relative to the project when it couldn't be resolved)
    std::string uid;    // kept from Godot files
};

struct SubResource {
    std::string id, type;
    std::vector<std::pair<std::string, std::string>> props;
    const std::string* prop(const std::string& key) const;
    void set(const std::string& key, const std::string& value);
};

struct Connection {
    std::string signal, from, to, method;
    std::string flags;  // extra attributes as written, e.g. "flags=3"
    bool operator==(const Connection&) const = default;
};

struct Scene {
    Node root;
    std::vector<ExtResource> ext;
    std::vector<SubResource> sub;
    std::vector<Connection> connections;
    // nodes inside an instanced scene that this scene overrides (parent path into the instance)
    std::vector<std::pair<std::string, Node>> overrides;
    std::vector<std::string> editable;  // [editable path="..."] entries
    std::string uid;
    fs::path project;  // folder with project.godot (res://); empty: the scene's own folder

    // "." is the root, "Player/Sprite" a path from it, as in Godot's NodePath.
    Node* find(const std::string& path);
    const Node* find(const std::string& path) const;
    Node* parentOf(const std::string& path);
    std::string pathOf(const Node* node) const;

    const ExtResource* extById(const std::string& id) const;
    const SubResource* subById(const std::string& id) const;
    SubResource* subById(const std::string& id);
    // An ExtResource for a file (reused if the scene already has one); returns its id.
    std::string addExt(const std::string& type, const fs::path& file);
    std::string addSub(const std::string& type);
    // Drops resources nothing refers to any more.
    void prune();
};

// ---- classes ----------------------------------------------------------------------------

// What the inspector knows about a property: its kind decides how a value given in a
// command ("16,32", "45", "art/hero.png", "rect:16x32") becomes a Godot value.
enum class PropKind { Bool, Int, Float, Angle, Vec2, Vec3, Angle3, Color, String, Texture, Mesh, Shape2D, Shape3D, Stream, Points, Enum };

struct PropInfo {
    std::string name;
    PropKind kind;
    std::string def;   // Godot's default, in Godot syntax
    std::string doc;
    std::vector<std::string> choices;  // Enum: names in Godot's order (stored as the index)
};

struct ClassInfo {
    std::string name, parent, doc;
    int dims = 0;  // 2 or 3 (0: neither, e.g. Node, Timer)
    std::vector<PropInfo> props;
};

const std::vector<ClassInfo>& classes();
const ClassInfo* classInfo(const std::string& name);
// The class and its parents, nearest first.
std::vector<const ClassInfo*> lineage(const std::string& name);
bool inherits(const std::string& type, const std::string& base);
const PropInfo* propInfo(const std::string& type, const std::string& prop);

// ---- values ---------------------------------------------------------------------------

std::string gdString(const std::string& s);
std::string gdNum(double v);
std::optional<std::string> unquote(const std::string& godotValue);
std::vector<double> numbers(const std::string& godotValue);  // every number inside, in order
std::string refId(const std::string& godotValue, const char* kind);  // ExtResource("x") -> x

// Turns what a person types into a Godot value for that property, adding resources as needed.
// Throws with a readable message when it can't.
std::string toGodot(Scene& s, const std::string& type, const PropInfo& info, const std::string& text, const fs::path& base);
// And back: the value as a person would type it ("16, 32", "45", "art/hero.png", "rect:16x32").
std::string fromGodot(const Scene& s, const PropInfo& info, const std::string& value);

// ---- files ----------------------------------------------------------------------------

Scene readTscn(const std::string& text, const fs::path& file);
std::string writeTscn(const Scene& s, const fs::path& file);
// The folder holding project.godot above `file`'s folder, or the folder itself.
fs::path projectRoot(const fs::path& file);
std::string resPath(const fs::path& file, const fs::path& project);
fs::path fromRes(const std::string& res, const fs::path& project);

// ---- transforms and pictures ------------------------------------------------------------

struct Xform2 {  // x' = a*x + c*y + e, y' = b*x + d*y + f
    double a = 1, b = 0, c = 0, d = 1, e = 0, f = 0;
    Xform2 operator*(const Xform2& o) const;
    std::array<double, 2> apply(double x, double y) const;
};
Xform2 localXform2(const Node& n);

struct Xform3 {  // basis columns x, y, z and origin, Godot's Y-up space
    std::array<double, 9> basis{1, 0, 0, 0, 1, 0, 0, 0, 1};  // row-major
    std::array<double, 3> origin{0, 0, 0};
    Xform3 operator*(const Xform3& o) const;
    std::array<double, 3> apply(const std::array<double, 3>& p) const;
};
Xform3 localXform3(const Node& n);

struct View2D {
    double originX = 0, originY = 0, scale = 1;  // world (x, y) lands at ((x - originX) * scale, (y - originY) * scale)
    int width = 0, height = 0;
};
struct Hit {
    std::string path, type;
    double x0 = 0, y0 = 0, x1 = 0, y1 = 0;  // world box
};
struct RenderOptions2D {
    int maxSize = 1200;
    std::optional<View2D> view;  // fixed view; otherwise fitted to the content
    bool helpers = true;         // collision shapes, cameras, markers
    std::string selected;        // node path drawn with a highlight
};
// Draws a 2D scene (instanced scenes included). `view` and `hits` say where things landed.
raster::Image render2D(const Scene& s, const RenderOptions2D& options, View2D* view = nullptr, std::vector<Hit>* hits = nullptr);
// Draws a 3D scene from an orbiting camera (angle/tilt in degrees) or from its Camera3D.
raster::Image render3D(const Scene& s, int width, int height, double angle, double tilt, bool fromCamera, const std::string& selected);
// 2 when the scene's nodes are mostly 2D, 3 when 3D.
int dimsOf(const Scene& s);

// A starter GDScript for a class: kinds "empty", "character2d", "character3d", "area".
std::string scriptTemplate(const std::string& base, const std::string& kind);

class SceneDoc : public Document {
public:
    Scene scene;
    fs::path file;  // the .tscn (Document::path holds the same, for titles)
    void setFile(const fs::path& f) {
        file = f;
        path = f.string();
    }
    std::string kind() const override { return "scene"; }
    std::string summary() const override;
    std::unique_ptr<Attachment> preview() const override;
};

class SceneEdit : public Action {
public:
    SceneEdit(std::shared_ptr<SceneDoc> doc, std::function<Result(SceneDoc&)> fn) : doc_(std::move(doc)), fn_(std::move(fn)) {}
    Result apply() override;
    void revert() override;

private:
    std::shared_ptr<SceneDoc> doc_;
    std::function<Result(SceneDoc&)> fn_;
    Scene before_;
    fs::path beforePath_;
};

void registerCommands(Session& session);

}  // namespace wetwaresuite::scene
