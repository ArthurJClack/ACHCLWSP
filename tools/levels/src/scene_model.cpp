// Godot-style scenes: the node tree, the classes the inspector knows, value conversion,
// .tscn reading and writing, and node transforms.
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <set>
#include <sstream>
#include <stdexcept>

#include "wetwaresuite/levels/scene.hpp"
#include "wetwaresuite/raster/image.hpp"

namespace wetwaresuite::scene {

namespace {

constexpr double kPi = 3.14159265358979323846;

std::string trim(const std::string& s) {
    const auto a = s.find_first_not_of(" \t\r\n"), b = s.find_last_not_of(" \t\r\n");
    return a == std::string::npos ? std::string() : s.substr(a, b - a + 1);
}

std::string lower(std::string s) {
    for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

template <typename T>
const std::string* findProp(const T& list, const std::string& key) {
    for (const auto& [k, v] : list)
        if (k == key) return &v;
    return nullptr;
}

template <typename T>
void setProp(T& list, const std::string& key, const std::string& value) {
    for (auto it = list.begin(); it != list.end(); ++it) {
        if (it->first != key) continue;
        if (value.empty()) list.erase(it);
        else it->second = value;
        return;
    }
    if (!value.empty()) list.push_back({key, value});
}

}  // namespace

const std::string* Node::prop(const std::string& key) const { return findProp(props, key); }
void Node::set(const std::string& key, const std::string& value) { setProp(props, key, value); }
const std::string* SubResource::prop(const std::string& key) const { return findProp(props, key); }
void SubResource::set(const std::string& key, const std::string& value) { setProp(props, key, value); }

// ---- tree -------------------------------------------------------------------------------

Node* Scene::find(const std::string& pathIn) {
    std::string path = trim(pathIn);
    if (path.empty() || path == "." || path == root.name) return &root;
    if (path.rfind("./", 0) == 0) path = path.substr(2);
    if (path.rfind(root.name + "/", 0) == 0) path = path.substr(root.name.size() + 1);
    Node* n = &root;
    std::stringstream ss(path);
    std::string part;
    while (std::getline(ss, part, '/')) {
        if (part.empty()) continue;
        auto it = std::find_if(n->children.begin(), n->children.end(), [&](const Node& c) { return c.name == part; });
        if (it == n->children.end()) {
            // a unique name anywhere below, like Godot's %Name
            if (n == &root && part.find('/') == std::string::npos) {
                Node* found = nullptr;
                int count = 0;
                std::function<void(Node&)> walk = [&](Node& x) {
                    for (auto& c : x.children) {
                        if (c.name == part) found = &c, ++count;
                        walk(c);
                    }
                };
                walk(root);
                if (count == 1 && path.find('/') == std::string::npos) return found;
            }
            return nullptr;
        }
        n = &*it;
    }
    return n;
}

const Node* Scene::find(const std::string& path) const { return const_cast<Scene*>(this)->find(path); }

Node* Scene::parentOf(const std::string& pathIn) {
    Node* target = find(pathIn);
    if (!target || target == &root) return nullptr;
    std::function<Node*(Node&)> walk = [&](Node& x) -> Node* {
        for (auto& c : x.children) {
            if (&c == target) return &x;
            if (Node* r = walk(c)) return r;
        }
        return nullptr;
    };
    return walk(root);
}

std::string Scene::pathOf(const Node* node) const {
    if (node == &root) return ".";
    std::string out;
    std::function<bool(const Node&, const std::string&)> walk = [&](const Node& x, const std::string& prefix) {
        for (const auto& c : x.children) {
            const std::string p = prefix.empty() ? c.name : prefix + "/" + c.name;
            if (&c == node) {
                out = p;
                return true;
            }
            if (walk(c, p)) return true;
        }
        return false;
    };
    walk(root, "");
    return out;
}

const ExtResource* Scene::extById(const std::string& id) const {
    for (const auto& e : ext)
        if (e.id == id) return &e;
    return nullptr;
}
const SubResource* Scene::subById(const std::string& id) const {
    for (const auto& s : sub)
        if (s.id == id) return &s;
    return nullptr;
}
SubResource* Scene::subById(const std::string& id) {
    for (auto& s : sub)
        if (s.id == id) return &s;
    return nullptr;
}

std::string Scene::addExt(const std::string& type, const fs::path& file) {
    std::error_code ec;
    const fs::path abs = fs::weakly_canonical(fs::absolute(file, ec), ec);
    for (const auto& e : ext) {
        std::error_code ec2;
        if (e.type == type && fs::weakly_canonical(fs::absolute(e.file, ec2), ec2) == abs) return e.id;
    }
    std::string stem = file.stem().string();
    std::string clean;
    for (char c : stem) clean += std::isalnum(static_cast<unsigned char>(c)) ? static_cast<char>(std::tolower(static_cast<unsigned char>(c))) : '_';
    int n = static_cast<int>(ext.size()) + 1;
    std::string id;
    do id = std::to_string(n++) + "_" + clean;
    while (extById(id));
    ext.push_back({id, type, abs.empty() ? file : abs, {}});
    return id;
}

std::string Scene::addSub(const std::string& type) {
    int n = 1;
    std::string id;
    do id = type + "_" + std::to_string(n++);
    while (subById(id));
    sub.push_back({id, type, {}});
    return id;
}

void Scene::prune() {
    // a resource is used when a node, an override or another (used) sub resource names it
    for (int pass = 0; pass < 4; ++pass) {
        std::set<std::string> extUsed, subUsed;
        auto scan = [&](const std::string& v) {
            for (const char* kind : {"ExtResource(", "SubResource("}) {
                const bool isExt = kind[0] == 'E';
                for (std::size_t at = v.find(kind); at != std::string::npos; at = v.find(kind, at + 1)) {
                    const std::size_t q0 = v.find('"', at), q1 = q0 == std::string::npos ? q0 : v.find('"', q0 + 1);
                    if (q1 == std::string::npos) continue;
                    (isExt ? extUsed : subUsed).insert(v.substr(q0 + 1, q1 - q0 - 1));
                }
            }
        };
        std::function<void(const Node&)> walk = [&](const Node& n) {
            for (const auto& [k, v] : n.props) scan(v);
            if (!n.instance.empty()) extUsed.insert(n.instance);
            for (const auto& c : n.children) walk(c);
        };
        walk(root);
        for (const auto& [p, n] : overrides) walk(n);
        for (const auto& s : sub)
            for (const auto& [k, v] : s.props) scan(v);
        const auto before = ext.size() + sub.size();
        std::erase_if(ext, [&](const ExtResource& e) { return !extUsed.count(e.id); });
        std::erase_if(sub, [&](const SubResource& s) { return !subUsed.count(s.id); });
        if (ext.size() + sub.size() == before) break;
    }
}

// ---- classes --------------------------------------------------------------------------

const std::vector<ClassInfo>& classes() {
    using K = PropKind;
    static const std::vector<ClassInfo> list = {
        {"Node", "", "the base of everything; groups nodes and runs scripts", 0, {}},
        {"CanvasItem", "Node", "anything drawn in 2D", 2,
         {{"visible", K::Bool, "true", "shown"},
          {"modulate", K::Color, "Color(1, 1, 1, 1)", "tint, children too"},
          {"self_modulate", K::Color, "Color(1, 1, 1, 1)", "tint, this node only"},
          {"z_index", K::Int, "0", "drawing order: higher draws on top"}}},
        {"Node2D", "CanvasItem", "a 2D node with a position, rotation and scale", 2,
         {{"position", K::Vec2, "Vector2(0, 0)", "pixels from the parent, y down"},
          {"rotation", K::Angle, "0.0", "degrees clockwise"},
          {"scale", K::Vec2, "Vector2(1, 1)", "size factor"},
          {"skew", K::Angle, "0.0", "slant, degrees"}}},
        {"Sprite2D", "Node2D", "draws a texture (or one frame of a sprite sheet)", 2,
         {{"texture", K::Texture, "", "the picture: a PNG or other image file"},
          {"centered", K::Bool, "true", "the picture is centred on the node"},
          {"offset", K::Vec2, "Vector2(0, 0)", "picture offset, pixels"},
          {"flip_h", K::Bool, "false", "mirror left to right"},
          {"flip_v", K::Bool, "false", "mirror top to bottom"},
          {"hframes", K::Int, "1", "sprite sheet columns"},
          {"vframes", K::Int, "1", "sprite sheet rows"},
          {"frame", K::Int, "0", "which frame of the sheet"}}},
        {"AnimatedSprite2D", "Node2D", "plays a SpriteFrames animation", 2,
         {{"animation", K::String, "&\"default\"", "animation name"},
          {"autoplay", K::String, "\"\"", "animation to start with"},
          {"speed_scale", K::Float, "1.0", "playback speed"},
          {"centered", K::Bool, "true", ""},
          {"flip_h", K::Bool, "false", ""}}},
        {"Polygon2D", "Node2D", "a filled polygon", 2,
         {{"polygon", K::Points, "PackedVector2Array()", "corners x1,y1 x2,y2 ..."},
          {"color", K::Color, "Color(1, 1, 1, 1)", "fill"},
          {"texture", K::Texture, "", ""}}},
        {"Line2D", "Node2D", "a line through points", 2,
         {{"points", K::Points, "PackedVector2Array()", "x1,y1 x2,y2 ..."},
          {"width", K::Float, "10.0", "pixels"},
          {"default_color", K::Color, "Color(1, 1, 1, 1)", ""}}},
        {"Marker2D", "Node2D", "a named point (spawns, waypoints)", 2, {{"gizmo_extents", K::Float, "10.0", "cross size"}}},
        {"Camera2D", "Node2D", "the 2D view; the current one follows its parent", 2,
         {{"enabled", K::Bool, "true", ""},
          {"zoom", K::Vec2, "Vector2(1, 1)", "2,2 = twice as close"},
          {"offset", K::Vec2, "Vector2(0, 0)", ""},
          {"anchor_mode", K::Enum, "1", "", {"fixed_top_left", "drag_center"}},
          {"position_smoothing_enabled", K::Bool, "false", "follow smoothly"},
          {"limit_left", K::Int, "-10000000", ""},
          {"limit_top", K::Int, "-10000000", ""},
          {"limit_right", K::Int, "10000000", ""},
          {"limit_bottom", K::Int, "10000000", ""}}},
        {"CollisionObject2D", "Node2D", "", 2,
         {{"collision_layer", K::Int, "1", "layers this body is on (bit mask)"}, {"collision_mask", K::Int, "1", "layers it collides with"}}},
        {"PhysicsBody2D", "CollisionObject2D", "", 2, {}},
        {"StaticBody2D", "PhysicsBody2D", "a body that doesn't move (walls, floors)", 2, {}},
        {"AnimatableBody2D", "StaticBody2D", "a moving platform", 2, {}},
        {"CharacterBody2D", "PhysicsBody2D", "a body moved by script (players, enemies)", 2,
         {{"motion_mode", K::Enum, "0", "", {"grounded", "floating"}},
          {"up_direction", K::Vec2, "Vector2(0, -1)", ""},
          {"floor_max_angle", K::Angle, "0.785398", "steepest floor, degrees"}}},
        {"RigidBody2D", "PhysicsBody2D", "a body moved by physics", 2,
         {{"mass", K::Float, "1.0", "kg"}, {"gravity_scale", K::Float, "1.0", ""}, {"lock_rotation", K::Bool, "false", ""}}},
        {"Area2D", "CollisionObject2D", "detects bodies entering (pickups, triggers)", 2,
         {{"monitoring", K::Bool, "true", ""}, {"monitorable", K::Bool, "true", ""}}},
        {"CollisionShape2D", "Node2D", "the shape of its parent body or area", 2,
         {{"shape", K::Shape2D, "", "rect:WxH, circle:R, capsule:RxH or segment:x1,y1,x2,y2"},
          {"disabled", K::Bool, "false", ""},
          {"one_way_collision", K::Bool, "false", "can be jumped through from below"},
          {"debug_color", K::Color, "Color(0, 0.6, 0.7, 0.42)", ""}}},
        {"CollisionPolygon2D", "Node2D", "a polygon shape for its parent body or area", 2,
         {{"polygon", K::Points, "PackedVector2Array()", "x1,y1 x2,y2 ..."}, {"disabled", K::Bool, "false", ""}}},
        {"TileMapLayer", "Node2D", "a layer of tiles (levels makes these)", 2, {}},
        {"TileMap", "Node2D", "tiles (Godot 4.0-4.2 style; levels makes these)", 2, {}},
        {"PointLight2D", "Node2D", "a 2D light", 2,
         {{"color", K::Color, "Color(1, 1, 1, 1)", ""}, {"energy", K::Float, "1.0", ""}, {"texture", K::Texture, "", "light shape"}, {"texture_scale", K::Float, "1.0", ""}}},
        {"AudioStreamPlayer2D", "Node2D", "plays a sound from a place", 2,
         {{"stream", K::Stream, "", "a .wav/.ogg/.mp3 file"}, {"autoplay", K::Bool, "false", ""}, {"volume_db", K::Float, "0.0", ""}}},
        {"GPUParticles2D", "Node2D", "particles", 2, {{"amount", K::Int, "8", ""}, {"emitting", K::Bool, "true", ""}, {"lifetime", K::Float, "1.0", ""}}},
        {"Control", "CanvasItem", "a user interface element", 2,
         {{"position", K::Vec2, "Vector2(0, 0)", "top-left, pixels"},
          {"size", K::Vec2, "Vector2(0, 0)", "pixels"},
          {"rotation", K::Angle, "0.0", "degrees"},
          {"scale", K::Vec2, "Vector2(1, 1)", ""}}},
        {"Label", "Control", "text", 2,
         {{"text", K::String, "\"\"", ""},
          {"horizontal_alignment", K::Enum, "0", "", {"left", "center", "right", "fill"}},
          {"autowrap_mode", K::Enum, "0", "", {"off", "arbitrary", "word", "word_smart"}}}},
        {"Button", "Control", "a button", 2, {{"text", K::String, "\"\"", ""}, {"disabled", K::Bool, "false", ""}}},
        {"ColorRect", "Control", "a filled rectangle", 2, {{"color", K::Color, "Color(1, 1, 1, 1)", ""}}},
        {"TextureRect", "Control", "a picture in a box", 2,
         {{"texture", K::Texture, "", ""}, {"stretch_mode", K::Enum, "0", "", {"scale", "tile", "keep", "keep_centered", "keep_aspect", "keep_aspect_centered", "keep_aspect_covered"}}}},
        {"ProgressBar", "Control", "a bar (health, loading)", 2, {{"value", K::Float, "0.0", ""}, {"max_value", K::Float, "100.0", ""}}},
        {"CanvasLayer", "Node", "draws its children on their own layer (HUDs)", 0, {{"layer", K::Int, "1", ""}}},
        {"Timer", "Node", "counts down and fires timeout", 0,
         {{"wait_time", K::Float, "1.0", "seconds"}, {"one_shot", K::Bool, "false", ""}, {"autostart", K::Bool, "false", ""}}},
        {"AudioStreamPlayer", "Node", "plays a sound (music, UI)", 0,
         {{"stream", K::Stream, "", "a .wav/.ogg/.mp3 file"}, {"autoplay", K::Bool, "false", ""}, {"volume_db", K::Float, "0.0", ""}}},
        {"AnimationPlayer", "Node", "plays animations", 0, {{"autoplay", K::String, "\"\"", ""}}},
        // 3D
        {"Node3D", "Node", "a 3D node with a position, rotation and scale", 3,
         {{"position", K::Vec3, "Vector3(0, 0, 0)", "metres from the parent, y up"},
          {"rotation", K::Angle3, "Vector3(0, 0, 0)", "degrees about x, y, z"},
          {"scale", K::Vec3, "Vector3(1, 1, 1)", ""},
          {"visible", K::Bool, "true", ""}}},
        {"VisualInstance3D", "Node3D", "", 3, {}},
        {"GeometryInstance3D", "VisualInstance3D", "", 3, {{"cast_shadow", K::Enum, "1", "", {"off", "on", "double_sided", "shadows_only"}}}},
        {"MeshInstance3D", "GeometryInstance3D", "draws a mesh", 3,
         {{"mesh", K::Mesh, "", "box:WxHxD, sphere:R, cylinder:RxH, capsule:RxH, plane:WxD or a .obj file"},
          {"material_override", K::Color, "", "a plain colour for the whole mesh"}}},
        {"Camera3D", "Node3D", "the 3D view", 3,
         {{"current", K::Bool, "false", ""},
          {"fov", K::Float, "75.0", "degrees"},
          {"near", K::Float, "0.05", ""},
          {"far", K::Float, "4000.0", ""},
          {"projection", K::Enum, "0", "", {"perspective", "orthogonal", "frustum"}}}},
        {"Light3D", "VisualInstance3D", "", 3,
         {{"light_color", K::Color, "Color(1, 1, 1, 1)", ""}, {"light_energy", K::Float, "1.0", ""}, {"shadow_enabled", K::Bool, "false", ""}}},
        {"DirectionalLight3D", "Light3D", "sunlight", 3, {}},
        {"OmniLight3D", "Light3D", "a bulb", 3, {{"omni_range", K::Float, "5.0", "metres"}}},
        {"SpotLight3D", "Light3D", "a spotlight", 3, {{"spot_range", K::Float, "5.0", ""}, {"spot_angle", K::Float, "45.0", "degrees"}}},
        {"CollisionObject3D", "Node3D", "", 3, {{"collision_layer", K::Int, "1", ""}, {"collision_mask", K::Int, "1", ""}}},
        {"PhysicsBody3D", "CollisionObject3D", "", 3, {}},
        {"StaticBody3D", "PhysicsBody3D", "a body that doesn't move", 3, {}},
        {"AnimatableBody3D", "StaticBody3D", "a moving platform", 3, {}},
        {"CharacterBody3D", "PhysicsBody3D", "a body moved by script", 3, {{"floor_max_angle", K::Angle, "0.785398", "degrees"}}},
        {"RigidBody3D", "PhysicsBody3D", "a body moved by physics", 3, {{"mass", K::Float, "1.0", ""}, {"gravity_scale", K::Float, "1.0", ""}}},
        {"VehicleBody3D", "RigidBody3D", "a car body (with VehicleWheel3D children)", 3, {{"engine_force", K::Float, "0.0", ""}, {"steering", K::Float, "0.0", ""}}},
        {"VehicleWheel3D", "Node3D", "a car wheel", 3,
         {{"use_as_traction", K::Bool, "false", ""}, {"use_as_steering", K::Bool, "false", ""}, {"wheel_radius", K::Float, "0.5", ""}, {"suspension_stiffness", K::Float, "5.88", ""}}},
        {"Area3D", "CollisionObject3D", "detects bodies entering", 3, {{"monitoring", K::Bool, "true", ""}}},
        {"CollisionShape3D", "Node3D", "the shape of its parent body or area", 3,
         {{"shape", K::Shape3D, "", "box:WxHxD, sphere:R, capsule:RxH or cylinder:RxH"}, {"disabled", K::Bool, "false", ""}}},
        {"Marker3D", "Node3D", "a named point", 3, {}},
        {"AudioStreamPlayer3D", "Node3D", "plays a sound from a place", 3, {{"stream", K::Stream, "", ""}, {"autoplay", K::Bool, "false", ""}}},
        {"WorldEnvironment", "Node", "sky, fog and glow for the scene", 0, {}},
    };
    return list;
}

const ClassInfo* classInfo(const std::string& name) {
    for (const auto& c : classes())
        if (c.name == name) return &c;
    // case-insensitive, for people typing "sprite2d"
    for (const auto& c : classes())
        if (lower(c.name) == lower(name)) return &c;
    return nullptr;
}

std::vector<const ClassInfo*> lineage(const std::string& name) {
    std::vector<const ClassInfo*> out;
    for (const ClassInfo* c = classInfo(name); c && out.size() < 16; c = c->parent.empty() ? nullptr : classInfo(c->parent)) out.push_back(c);
    return out;
}

bool inherits(const std::string& type, const std::string& base) {
    for (const auto* c : lineage(type))
        if (c->name == base) return true;
    return false;
}

const PropInfo* propInfo(const std::string& type, const std::string& prop) {
    for (const auto* c : lineage(type))
        for (const auto& p : c->props)
            if (p.name == prop) return &p;
    return nullptr;
}

// ---- values -----------------------------------------------------------------------------

std::string gdString(const std::string& s) {
    std::string out = "\"";
    for (char c : s) {
        if (c == '"' || c == '\\') out += '\\';
        if (c == '\n') {
            out += "\\n";
            continue;
        }
        out += c;
    }
    return out + "\"";
}

std::string gdNum(double v) {
    if (std::abs(v) < 1e-5) v = 0;
    if (std::abs(v - std::round(v)) < 1e-9 && std::abs(v) < 1e15) return std::to_string(static_cast<long long>(std::llround(v)));
    char b[48];
    std::snprintf(b, sizeof b, "%.6g", v);
    return b;
}

std::string gdFloat(double v) {  // floats keep a decimal point, as Godot writes them
    std::string s = gdNum(v);
    if (s.find_first_of(".e") == std::string::npos) s += ".0";
    return s;
}

std::optional<std::string> unquote(const std::string& v) {
    std::string t = trim(v);
    if (!t.empty() && (t[0] == '&' || t[0] == '^')) t = t.substr(1);  // StringName, NodePath
    if (t.size() < 2 || t.front() != '"' || t.back() != '"') return std::nullopt;
    std::string out;
    for (std::size_t i = 1; i + 1 < t.size(); ++i) {
        if (t[i] == '\\' && i + 2 < t.size()) {
            ++i;
            out += t[i] == 'n' ? '\n' : t[i] == 't' ? '\t' : t[i];
        } else {
            out += t[i];
        }
    }
    return out;
}

std::vector<double> numbers(const std::string& v) {
    std::vector<double> out;
    const char* s = v.c_str();
    while (*s) {
        if (std::isdigit(static_cast<unsigned char>(*s)) || ((*s == '-' || *s == '+' || *s == '.') && (std::isdigit(static_cast<unsigned char>(s[1])) || s[1] == '.'))) {
            // skip digits that are part of a name like Vector2 or Transform3D
            if (s != v.c_str() && std::isalpha(static_cast<unsigned char>(s[-1]))) {
                while (std::isalnum(static_cast<unsigned char>(*s))) ++s;
                continue;
            }
            char* end = nullptr;
            const double d = std::strtod(s, &end);
            if (end == s) {
                ++s;
                continue;
            }
            out.push_back(d);
            s = end;
        } else {
            ++s;
        }
    }
    return out;
}

std::string refId(const std::string& v, const char* kind) {
    const std::string k = std::string(kind) + "(";
    const auto at = v.find(k);
    if (at == std::string::npos) return {};
    const auto q0 = v.find('"', at), q1 = q0 == std::string::npos ? q0 : v.find('"', q0 + 1);
    if (q1 == std::string::npos) {
        // Godot 3 style: ExtResource( 1 )
        const auto close = v.find(')', at);
        return close == std::string::npos ? std::string() : trim(v.substr(at + k.size(), close - at - k.size()));
    }
    return v.substr(q0 + 1, q1 - q0 - 1);
}

namespace {

std::vector<double> parseNums(const std::string& text, std::size_t want, const std::string& what) {
    std::string t = text;
    for (auto& c : t)
        if (c == 'x' || c == 'X' || c == ';') c = ',';
    std::vector<double> v;
    std::stringstream ss(t);
    std::string item;
    while (std::getline(ss, item, ',')) {
        item = trim(item);
        if (item.empty()) continue;
        char* end = nullptr;
        const double d = std::strtod(item.c_str(), &end);
        if (end == item.c_str() || *end) throw std::runtime_error(what + ": '" + text + "' isn't " + std::to_string(want) + " numbers");
        v.push_back(d);
    }
    if (want && v.size() == 1) v.resize(want, v[0]);  // "2" -> 2,2 for scales
    if (want && v.size() != want) throw std::runtime_error(what + " takes " + std::to_string(want) + " numbers, e.g. " + (want == 2 ? "16,32" : "1,2,3"));
    return v;
}

std::string colorValue(const std::string& text) {
    std::string t = trim(text);
    if (t.rfind("Color(", 0) == 0) return t;
    if (auto c = raster::Color::parse(t)) {
        return "Color(" + gdNum(c->r / 255.0) + ", " + gdNum(c->g / 255.0) + ", " + gdNum(c->b / 255.0) + ", " + gdNum(c->a / 255.0) + ")";
    }
    const auto n = parseNums(t, 0, "colour");
    if (n.size() == 3 || n.size() == 4) {
        const bool bytes = std::any_of(n.begin(), n.end(), [](double x) { return x > 1.0; });
        auto f = [&](double x) { return gdNum(bytes ? x / 255.0 : x); };
        return "Color(" + f(n[0]) + ", " + f(n[1]) + ", " + f(n[2]) + ", " + (n.size() == 4 ? f(n[3]) : std::string("1")) + ")";
    }
    throw std::runtime_error("colour: give #rrggbb, a name like red, or r,g,b(,a)");
}

std::string shapeKey(const std::string& text) {
    const auto colon = text.find(':');
    return lower(trim(colon == std::string::npos ? text : text.substr(0, colon)));
}
std::string shapeArgs(const std::string& text) {
    const auto colon = text.find(':');
    return colon == std::string::npos ? std::string() : text.substr(colon + 1);
}

std::string textureType(const fs::path& f) {
    const std::string e = lower(f.extension().string());
    if (e == ".tres" || e == ".res") return "Texture2D";
    return "Texture2D";
}

}  // namespace

std::string toGodot(Scene& s, const std::string& type, const PropInfo& info, const std::string& textIn, const fs::path& base) {
    const std::string text = trim(textIn);
    (void)type;
    if (text.empty() || lower(text) == "default" || lower(text) == "none") return "";
    auto file = [&](const std::string& t) {
        fs::path f = t;
        if (t.rfind("res://", 0) == 0) f = fromRes(t, s.project.empty() ? base : s.project);
        else if (f.is_relative()) f = base / f;
        std::error_code ec;
        if (!fs::exists(f, ec)) throw std::runtime_error(info.name + ": no file " + f.string());
        return f;
    };
    switch (info.kind) {
        case PropKind::Bool: {
            const std::string l = lower(text);
            if (l == "true" || l == "1" || l == "yes" || l == "on") return "true";
            if (l == "false" || l == "0" || l == "no" || l == "off") return "false";
            throw std::runtime_error(info.name + " is true or false");
        }
        case PropKind::Int: {
            char* end = nullptr;
            const long long v = std::strtoll(text.c_str(), &end, 10);
            if (*end) throw std::runtime_error(info.name + " is a whole number");
            return std::to_string(v);
        }
        case PropKind::Float: return gdFloat(parseNums(text, 1, info.name)[0]);
        case PropKind::Angle: return gdFloat(parseNums(text, 1, info.name)[0] * kPi / 180);
        case PropKind::Vec2: {
            if (text.rfind("Vector2", 0) == 0) return text;
            const auto v = parseNums(text, 2, info.name);
            return "Vector2(" + gdNum(v[0]) + ", " + gdNum(v[1]) + ")";
        }
        case PropKind::Vec3: {
            if (text.rfind("Vector3", 0) == 0) return text;
            const auto v = parseNums(text, 3, info.name);
            return "Vector3(" + gdNum(v[0]) + ", " + gdNum(v[1]) + ", " + gdNum(v[2]) + ")";
        }
        case PropKind::Angle3: {
            const auto v = parseNums(text, 3, info.name);
            return "Vector3(" + gdNum(v[0] * kPi / 180) + ", " + gdNum(v[1] * kPi / 180) + ", " + gdNum(v[2] * kPi / 180) + ")";
        }
        case PropKind::Color: return colorValue(text);
        case PropKind::String: return text.front() == '"' ? text : gdString(text);
        case PropKind::Enum: {
            for (std::size_t i = 0; i < info.choices.size(); ++i)
                if (lower(text) == info.choices[i]) return std::to_string(i);
            char* end = nullptr;
            const long v = std::strtol(text.c_str(), &end, 10);
            if (!*end && v >= 0 && v < static_cast<long>(info.choices.size())) return std::to_string(v);
            std::string all;
            for (const auto& c : info.choices) all += (all.empty() ? "" : ", ") + c;
            throw std::runtime_error(info.name + " is one of " + all);
        }
        case PropKind::Points: {
            std::string t = text;
            for (auto& c : t)
                if (c == ' ' || c == ';') c = ',';
            const auto v = parseNums(t, 0, info.name);
            if (v.size() % 2) throw std::runtime_error(info.name + ": give x,y pairs");
            std::string out = "PackedVector2Array(";
            for (std::size_t i = 0; i < v.size(); ++i) out += (i ? ", " : "") + gdNum(v[i]);
            return out + ")";
        }
        case PropKind::Texture: return "ExtResource(\"" + s.addExt(textureType(text), file(text)) + "\")";
        case PropKind::Stream: {
            const fs::path f = file(text);
            const std::string e = lower(f.extension().string());
            const std::string t = e == ".wav" ? "AudioStreamWAV" : e == ".ogg" ? "AudioStreamOggVorbis" : e == ".mp3" ? "AudioStreamMP3" : "AudioStream";
            return "ExtResource(\"" + s.addExt(t, f) + "\")";
        }
        case PropKind::Mesh: {
            const std::string k = shapeKey(text);
            const std::string args = shapeArgs(text);
            std::string id;
            if (k == "box" || k == "cube") {
                id = s.addSub("BoxMesh");
                const auto v = parseNums(args.empty() ? "1" : args, 3, "box");
                s.subById(id)->set("size", "Vector3(" + gdNum(v[0]) + ", " + gdNum(v[1]) + ", " + gdNum(v[2]) + ")");
            } else if (k == "sphere") {
                id = s.addSub("SphereMesh");
                const double r = parseNums(args.empty() ? "0.5" : args, 1, "sphere")[0];
                s.subById(id)->set("radius", gdFloat(r));
                s.subById(id)->set("height", gdFloat(2 * r));
            } else if (k == "cylinder" || k == "capsule") {
                id = s.addSub(k == "cylinder" ? "CylinderMesh" : "CapsuleMesh");
                const auto v = parseNums(args.empty() ? "0.5,2" : args, 2, k);
                if (k == "cylinder") {
                    s.subById(id)->set("top_radius", gdFloat(v[0]));
                    s.subById(id)->set("bottom_radius", gdFloat(v[0]));
                } else {
                    s.subById(id)->set("radius", gdFloat(v[0]));
                }
                s.subById(id)->set("height", gdFloat(v[1]));
            } else if (k == "plane") {
                id = s.addSub("PlaneMesh");
                const auto v = parseNums(args.empty() ? "2" : args, 2, "plane");
                s.subById(id)->set("size", "Vector2(" + gdNum(v[0]) + ", " + gdNum(v[1]) + ")");
            } else {
                return "ExtResource(\"" + s.addExt("ArrayMesh", file(text)) + "\")";
            }
            return "SubResource(\"" + id + "\")";
        }
        case PropKind::Shape2D: {
            const std::string k = shapeKey(text);
            const std::string args = shapeArgs(text);
            std::string id;
            if (k == "rect" || k == "rectangle" || k == "box") {
                id = s.addSub("RectangleShape2D");
                const auto v = parseNums(args.empty() ? "20" : args, 2, "rect");
                s.subById(id)->set("size", "Vector2(" + gdNum(v[0]) + ", " + gdNum(v[1]) + ")");
            } else if (k == "circle") {
                id = s.addSub("CircleShape2D");
                s.subById(id)->set("radius", gdFloat(parseNums(args.empty() ? "10" : args, 1, "circle")[0]));
            } else if (k == "capsule") {
                id = s.addSub("CapsuleShape2D");
                const auto v = parseNums(args.empty() ? "10,30" : args, 2, "capsule");
                s.subById(id)->set("radius", gdFloat(v[0]));
                s.subById(id)->set("height", gdFloat(v[1]));
            } else if (k == "segment") {
                id = s.addSub("SegmentShape2D");
                const auto v = parseNums(args, 4, "segment");
                s.subById(id)->set("a", "Vector2(" + gdNum(v[0]) + ", " + gdNum(v[1]) + ")");
                s.subById(id)->set("b", "Vector2(" + gdNum(v[2]) + ", " + gdNum(v[3]) + ")");
            } else if (k == "world_boundary" || k == "floor") {
                id = s.addSub("WorldBoundaryShape2D");
            } else {
                throw std::runtime_error("shape: rect:WxH, circle:R, capsule:RxH, segment:x1,y1,x2,y2 or world_boundary");
            }
            return "SubResource(\"" + id + "\")";
        }
        case PropKind::Shape3D: {
            const std::string k = shapeKey(text);
            const std::string args = shapeArgs(text);
            std::string id;
            if (k == "box" || k == "cube") {
                id = s.addSub("BoxShape3D");
                const auto v = parseNums(args.empty() ? "1" : args, 3, "box");
                s.subById(id)->set("size", "Vector3(" + gdNum(v[0]) + ", " + gdNum(v[1]) + ", " + gdNum(v[2]) + ")");
            } else if (k == "sphere") {
                id = s.addSub("SphereShape3D");
                s.subById(id)->set("radius", gdFloat(parseNums(args.empty() ? "0.5" : args, 1, "sphere")[0]));
            } else if (k == "capsule" || k == "cylinder") {
                id = s.addSub(k == "capsule" ? "CapsuleShape3D" : "CylinderShape3D");
                const auto v = parseNums(args.empty() ? "0.5,2" : args, 2, k);
                s.subById(id)->set("radius", gdFloat(v[0]));
                s.subById(id)->set("height", gdFloat(v[1]));
            } else if (k == "world_boundary" || k == "floor") {
                id = s.addSub("WorldBoundaryShape3D");
            } else {
                throw std::runtime_error("shape: box:WxHxD, sphere:R, capsule:RxH, cylinder:RxH or world_boundary");
            }
            return "SubResource(\"" + id + "\")";
        }
    }
    return text;
}

std::string fromGodot(const Scene& s, const PropInfo& info, const std::string& value) {
    const auto n = numbers(value);
    auto join = [&](double k) {
        std::string out;
        for (std::size_t i = 0; i < n.size(); ++i) out += (i ? ", " : "") + gdNum(n[i] * k);
        return out;
    };
    switch (info.kind) {
        case PropKind::Angle:
        case PropKind::Angle3: return join(180 / kPi);
        case PropKind::Float:
        case PropKind::Vec2:
        case PropKind::Vec3:
        case PropKind::Points: return join(1);
        case PropKind::Color: {
            if (n.size() < 3) return value;
            char b[16];
            auto byte = [](double x) { return static_cast<int>(std::lround(std::clamp(x, 0.0, 1.0) * 255)); };
            if (n.size() > 3 && byte(n[3]) < 255) std::snprintf(b, sizeof b, "#%02x%02x%02x%02x", byte(n[0]), byte(n[1]), byte(n[2]), byte(n[3]));
            else std::snprintf(b, sizeof b, "#%02x%02x%02x", byte(n[0]), byte(n[1]), byte(n[2]));
            return b;
        }
        case PropKind::String: return unquote(value).value_or(value);
        case PropKind::Enum: {
            const auto i = n.empty() ? -1 : static_cast<int>(n[0]);
            return i >= 0 && i < static_cast<int>(info.choices.size()) ? info.choices[static_cast<std::size_t>(i)] : value;
        }
        case PropKind::Texture:
        case PropKind::Stream: {
            if (const ExtResource* e = s.extById(refId(value, "ExtResource"))) return s.project.empty() ? e->file.string() : resPath(e->file, s.project);
            return value;
        }
        case PropKind::Mesh:
        case PropKind::Shape2D:
        case PropKind::Shape3D: {
            if (const ExtResource* e = s.extById(refId(value, "ExtResource"))) return s.project.empty() ? e->file.string() : resPath(e->file, s.project);
            const SubResource* r = s.subById(refId(value, "SubResource"));
            if (!r) return value;
            auto nums = [&](const char* key) { return r->prop(key) ? numbers(*r->prop(key)) : std::vector<double>{}; };
            auto dims = [&](std::vector<double> v) {
                std::string out;
                for (std::size_t i = 0; i < v.size(); ++i) out += (i ? "x" : "") + gdNum(v[i]);
                return out;
            };
            const std::string& t = r->type;
            if (t == "RectangleShape2D") return "rect:" + dims(nums("size").empty() ? std::vector<double>{20, 20} : nums("size"));
            if (t == "CircleShape2D") return "circle:" + dims(nums("radius").empty() ? std::vector<double>{10} : nums("radius"));
            if (t == "CapsuleShape2D" || t == "CapsuleShape3D" || t == "CylinderShape3D" || t == "CapsuleMesh") {
                const auto r0 = nums("radius"), h = nums("height");
                return lower(t.substr(0, t.find_first_of("SM"))) + ":" + gdNum(r0.empty() ? 0.5 : r0[0]) + "x" + gdNum(h.empty() ? 2 : h[0]);
            }
            if (t == "CylinderMesh") {
                const auto r0 = nums("top_radius"), h = nums("height");
                return "cylinder:" + gdNum(r0.empty() ? 0.5 : r0[0]) + "x" + gdNum(h.empty() ? 2 : h[0]);
            }
            if (t == "BoxShape3D" || t == "BoxMesh") return "box:" + dims(nums("size").empty() ? std::vector<double>{1, 1, 1} : nums("size"));
            if (t == "SphereShape3D" || t == "SphereMesh") return "sphere:" + gdNum(nums("radius").empty() ? 0.5 : nums("radius")[0]);
            if (t == "PlaneMesh") return "plane:" + dims(nums("size").empty() ? std::vector<double>{2, 2} : nums("size"));
            if (t == "SegmentShape2D") {
                const auto a = nums("a"), b = nums("b");
                if (a.size() == 2 && b.size() == 2) return "segment:" + gdNum(a[0]) + "," + gdNum(a[1]) + "," + gdNum(b[0]) + "," + gdNum(b[1]);
            }
            if (t == "WorldBoundaryShape2D" || t == "WorldBoundaryShape3D") return "world_boundary";
            return t;
        }
        default: return value;
    }
}

// ---- paths ------------------------------------------------------------------------------

fs::path projectRoot(const fs::path& file) {
    std::error_code ec;
    fs::path dir = fs::absolute(file, ec).parent_path();
    for (fs::path d = dir; !d.empty(); d = d.parent_path()) {
        if (fs::exists(d / "project.godot", ec)) return d;
        if (d == d.parent_path()) break;
    }
    return dir;
}

std::string resPath(const fs::path& file, const fs::path& project) {
    std::error_code ec;
    fs::path rel = fs::relative(fs::absolute(file, ec), fs::absolute(project, ec), ec);
    std::string r = rel.generic_string();
    if (ec || r.empty() || r.rfind("..", 0) == 0) r = fs::absolute(file, ec).generic_string();  // outside the project: Godot will ask
    else r = "res://" + r;
    return r;
}

fs::path fromRes(const std::string& res, const fs::path& project) {
    if (res.rfind("res://", 0) == 0) return project / fs::path(res.substr(6));
    return res;
}

// ---- reading .tscn ------------------------------------------------------------------------

namespace {

// key=value pairs of a [header], values being strings, arrays, calls or bare words
std::vector<std::pair<std::string, std::string>> headerAttrs(const std::string& inside) {
    std::vector<std::pair<std::string, std::string>> out;
    std::size_t i = 0;
    const std::size_t n = inside.size();
    while (i < n) {
        while (i < n && std::isspace(static_cast<unsigned char>(inside[i]))) ++i;
        std::size_t k0 = i;
        while (i < n && inside[i] != '=' && !std::isspace(static_cast<unsigned char>(inside[i]))) ++i;
        std::string key = inside.substr(k0, i - k0);
        while (i < n && std::isspace(static_cast<unsigned char>(inside[i]))) ++i;
        if (i >= n || inside[i] != '=') {
            if (!key.empty()) out.push_back({key, ""});
            continue;
        }
        ++i;
        while (i < n && std::isspace(static_cast<unsigned char>(inside[i]))) ++i;
        std::size_t v0 = i;
        int depth = 0;
        bool str = false;
        for (; i < n; ++i) {
            const char c = inside[i];
            if (str) {
                if (c == '\\') ++i;
                else if (c == '"') str = false;
                continue;
            }
            if (c == '"') str = true;
            else if (c == '(' || c == '[' || c == '{') ++depth;
            else if (c == ')' || c == ']' || c == '}') --depth;
            else if (std::isspace(static_cast<unsigned char>(c)) && depth <= 0) break;
        }
        out.push_back({key, inside.substr(v0, i - v0)});
    }
    return out;
}

std::string attr(const std::vector<std::pair<std::string, std::string>>& a, const std::string& key) {
    for (const auto& [k, v] : a)
        if (k == key) return unquote(v).value_or(v);
    return {};
}

}  // namespace

Scene readTscn(const std::string& text, const fs::path& file) {
    Scene s;
    s.project = projectRoot(file);
    std::istringstream in(text);
    std::string line;
    enum { None, Ext, Sub, NodeSec, Res, Other } section = None;
    SubResource* curSub = nullptr;
    Node* curNode = nullptr;
    bool sawHeader = false;
    std::vector<std::pair<std::string, std::string>>* target = nullptr;
    std::string pendingKey, pendingVal;
    int depth = 0;
    bool inStr = false;
    auto scanDepth = [&](const std::string& t) {
        for (std::size_t i = 0; i < t.size(); ++i) {
            const char c = t[i];
            if (inStr) {
                if (c == '\\') ++i;
                else if (c == '"') inStr = false;
                continue;
            }
            if (c == '"') inStr = true;
            else if (c == '(' || c == '[' || c == '{') ++depth;
            else if (c == ')' || c == ']' || c == '}') --depth;
        }
    };
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (!pendingKey.empty()) {  // a value running over several lines
            pendingVal += "\n" + line;
            scanDepth(line);
            if (depth <= 0 && !inStr) {
                if (target) target->push_back({pendingKey, pendingVal});
                pendingKey.clear();
            }
            continue;
        }
        const std::string t = trim(line);
        if (t.empty() || t[0] == ';') continue;
        if (t.front() == '[' && t.back() == ']') {
            const std::string inside = t.substr(1, t.size() - 2);
            const auto sp = inside.find(' ');
            const std::string tag = inside.substr(0, sp);
            const auto attrs = headerAttrs(sp == std::string::npos ? std::string() : inside.substr(sp + 1));
            target = nullptr;
            curSub = nullptr;
            curNode = nullptr;
            if (tag == "gd_scene") {
                sawHeader = true;
                s.uid = attr(attrs, "uid");
                section = None;
            } else if (tag == "ext_resource") {
                ExtResource e;
                e.type = attr(attrs, "type");
                e.id = attr(attrs, "id");
                e.uid = attr(attrs, "uid");
                {
                    const std::string rp = attr(attrs, "path");
                    fs::path f = fromRes(rp, s.project);
                    if (rp.rfind("res://", 0) != 0 && f.is_relative()) f = fs::absolute(file).parent_path() / f;  // relative to the scene
                    e.file = f.lexically_normal();
                }
                s.ext.push_back(e);
                section = Ext;
            } else if (tag == "sub_resource") {
                s.sub.push_back({attr(attrs, "id"), attr(attrs, "type"), {}});
                curSub = &s.sub.back();
                target = &curSub->props;
                section = Sub;
            } else if (tag == "node") {
                Node n;
                n.name = attr(attrs, "name");
                n.type = attr(attrs, "type");
                std::string parent;
                bool hasParent = false;
                for (const auto& [k, v] : attrs) {
                    if (k == "parent") parent = unquote(v).value_or(v), hasParent = true;
                    else if (k == "instance") n.instance = refId(v, "ExtResource");
                    else if (k == "groups") {
                        std::string g = v;
                        std::size_t at = 0;
                        while ((at = g.find('"', at)) != std::string::npos) {
                            const auto end = g.find('"', at + 1);
                            if (end == std::string::npos) break;
                            n.groups.push_back(g.substr(at + 1, end - at - 1));
                            at = end + 1;
                        }
                    } else if (k != "name" && k != "type") {
                        n.extraAttrs += (n.extraAttrs.empty() ? "" : " ") + k + "=" + v;
                    }
                }
                if (!hasParent) {
                    s.root = n;
                    curNode = &s.root;
                } else if (Node* p = s.find(parent)) {
                    p->children.push_back(n);
                    curNode = &p->children.back();
                } else {
                    s.overrides.push_back({parent, n});
                    curNode = &s.overrides.back().second;
                }
                target = &curNode->props;
                section = NodeSec;
            } else if (tag == "connection") {
                Connection c;
                for (const auto& [k, v] : attrs) {
                    if (k == "signal") c.signal = unquote(v).value_or(v);
                    else if (k == "from") c.from = unquote(v).value_or(v);
                    else if (k == "to") c.to = unquote(v).value_or(v);
                    else if (k == "method") c.method = unquote(v).value_or(v);
                    else c.flags += (c.flags.empty() ? "" : " ") + k + "=" + v;
                }
                s.connections.push_back(c);
                section = Other;
            } else if (tag == "editable") {
                s.editable.push_back(attr(attrs, "path"));
                section = Other;
            } else if (tag == "gd_resource") {
                throw std::runtime_error("this is a Godot resource (.tres), not a scene");
            } else {
                section = Other;
            }
            continue;
        }
        const auto eq = t.find('=');
        if (eq == std::string::npos || !target) continue;
        std::string key = trim(t.substr(0, eq)), val = trim(t.substr(eq + 1));
        depth = 0;
        inStr = false;
        scanDepth(val);
        if (depth > 0 || inStr) {
            pendingKey = key;
            pendingVal = val;
            continue;
        }
        target->push_back({key, val});
    }
    if (!sawHeader) throw std::runtime_error("not a Godot scene (no [gd_scene] header)");
    // 3D transforms become position / rotation / scale, which is what people edit
    std::function<void(Node&)> split = [&](Node& n) {
        if (n.prop("transform") && inherits(n.type, "Node3D")) {
            const auto k = numbers(*n.prop("transform"));
            if (k.size() >= 12) {
                double sc[3];
                double m[9];
                for (int j = 0; j < 3; ++j) {
                    sc[j] = std::sqrt(k[static_cast<std::size_t>(j)] * k[static_cast<std::size_t>(j)] + k[static_cast<std::size_t>(3 + j)] * k[static_cast<std::size_t>(3 + j)] + k[static_cast<std::size_t>(6 + j)] * k[static_cast<std::size_t>(6 + j)]);
                    for (int i = 0; i < 3; ++i) m[i * 3 + j] = sc[j] > 1e-12 ? k[static_cast<std::size_t>(i * 3 + j)] / sc[j] : 0;
                }
                const double x = std::asin(std::clamp(-m[5], -1.0, 1.0));
                double y, z;
                if (std::abs(std::cos(x)) > 1e-6) {
                    y = std::atan2(m[2], m[8]);
                    z = std::atan2(m[3], m[4]);
                } else {
                    y = std::atan2(-m[6], m[0]);
                    z = 0;
                }
                // only when that rebuilds the same matrix (no shear)
                Node probe;
                probe.set("rotation", "Vector3(" + gdNum(x) + ", " + gdNum(y) + ", " + gdNum(z) + ")");
                probe.set("scale", "Vector3(" + gdNum(sc[0]) + ", " + gdNum(sc[1]) + ", " + gdNum(sc[2]) + ")");
                const Xform3 back = localXform3(probe);
                bool same = true;
                for (int i = 0; i < 9; ++i) same = same && std::abs(back.basis[static_cast<std::size_t>(i)] - k[static_cast<std::size_t>(i)]) < 1e-4;
                if (same) {
                    std::vector<std::pair<std::string, std::string>> props;
                    for (const auto& [key, v] : n.props) {
                        if (key != "transform") {
                            props.push_back({key, v});
                            continue;
                        }
                        if (std::abs(k[9]) + std::abs(k[10]) + std::abs(k[11]) > 1e-12)
                            props.push_back({"position", "Vector3(" + gdNum(k[9]) + ", " + gdNum(k[10]) + ", " + gdNum(k[11]) + ")"});
                        if (std::abs(x) + std::abs(y) + std::abs(z) > 1e-6) props.push_back({"rotation", *probe.prop("rotation")});
                        if (std::abs(sc[0] - 1) + std::abs(sc[1] - 1) + std::abs(sc[2] - 1) > 1e-5) props.push_back({"scale", *probe.prop("scale")});
                    }
                    n.props = props;
                }
            }
        }
        for (auto& c : n.children) split(c);
    };
    split(s.root);
    for (auto& [p, n] : s.overrides) split(n);
    if (s.root.name.empty()) throw std::runtime_error("the scene has no root node");
    (void)section;
    // node pointers moved as children vectors grew: nothing keeps them past this point
    return s;
}

// ---- writing .tscn ------------------------------------------------------------------------

std::string writeTscn(const Scene& sIn, const fs::path& file) {
    Scene s = sIn;
    s.prune();
    const fs::path project = s.project.empty() ? projectRoot(file) : s.project;
    std::ostringstream o;
    o << "[gd_scene load_steps=" << (s.ext.size() + s.sub.size() + 1) << " format=3";
    if (!s.uid.empty()) o << " uid=" << gdString(s.uid);
    o << "]\n\n";
    for (const auto& e : s.ext) {
        o << "[ext_resource type=" << gdString(e.type);
        if (!e.uid.empty()) o << " uid=" << gdString(e.uid);
        o << " path=" << gdString(resPath(e.file, project)) << " id=" << gdString(e.id) << "]\n";
    }
    if (!s.ext.empty()) o << "\n";
    for (const auto& r : s.sub) {
        o << "[sub_resource type=" << gdString(r.type) << " id=" << gdString(r.id) << "]\n";
        for (const auto& [k, v] : r.props) o << k << " = " << v << "\n";
        o << "\n";
    }
    std::function<void(const Node&, const std::string&, bool)> node = [&](const Node& n, const std::string& parent, bool isRoot) {
        o << "[node name=" << gdString(n.name);
        if (!n.type.empty()) o << " type=" << gdString(n.type);
        if (!isRoot) o << " parent=" << gdString(parent);
        if (!n.instance.empty()) o << " instance=ExtResource(" << gdString(n.instance) << ")";
        if (!n.groups.empty()) {
            o << " groups=[";
            for (std::size_t i = 0; i < n.groups.size(); ++i) o << (i ? ", " : "") << gdString(n.groups[i]);
            o << "]";
        }
        if (!n.extraAttrs.empty()) o << " " << n.extraAttrs;
        o << "]\n";
        // Godot keeps a 3D node's placement as one transform
        const bool is3d = inherits(n.type, "Node3D") && !n.prop("transform") && (n.prop("position") || n.prop("rotation") || n.prop("scale"));
        bool wroteXform = false;
        for (const auto& [k, v] : n.props) {
            if (is3d && (k == "position" || k == "rotation" || k == "scale")) {
                if (!wroteXform) {
                    const Xform3 t = localXform3(n);
                    o << "transform = Transform3D(";
                    for (int i = 0; i < 9; ++i) o << gdNum(t.basis[static_cast<std::size_t>(i)]) << ", ";
                    o << gdNum(t.origin[0]) << ", " << gdNum(t.origin[1]) << ", " << gdNum(t.origin[2]) << ")\n";
                    wroteXform = true;
                }
                continue;
            }
            o << k << " = " << v << "\n";
        }
        o << "\n";
        const std::string mine = isRoot ? "." : (parent == "." ? n.name : parent + "/" + n.name);
        for (const auto& c : n.children) node(c, mine, false);
    };
    node(s.root, "", true);
    for (const auto& [parent, n] : s.overrides) node(n, parent, false);
    for (const auto& path : s.editable) o << "[editable path=" << gdString(path) << "]\n";
    for (const auto& c : s.connections) {
        o << "[connection signal=" << gdString(c.signal) << " from=" << gdString(c.from) << " to=" << gdString(c.to) << " method=" << gdString(c.method);
        if (!c.flags.empty()) o << " " << c.flags;
        o << "]\n";
    }
    std::string out = o.str();
    while (out.size() > 1 && out[out.size() - 1] == '\n' && out[out.size() - 2] == '\n') out.pop_back();
    return out;
}

// ---- transforms -------------------------------------------------------------------------

Xform2 Xform2::operator*(const Xform2& o) const {
    return {a * o.a + c * o.b, b * o.a + d * o.b, a * o.c + c * o.d, b * o.c + d * o.d, a * o.e + c * o.f + e, b * o.e + d * o.f + f};
}
std::array<double, 2> Xform2::apply(double x, double y) const { return {a * x + c * y + e, b * x + d * y + f}; }

Xform2 localXform2(const Node& n) {
    double px = 0, py = 0, rot = 0, sx = 1, sy = 1, skew = 0;
    if (const auto* v = n.prop("position")) {
        const auto k = numbers(*v);
        if (k.size() >= 2) px = k[0], py = k[1];
    }
    if (const auto* v = n.prop("rotation")) {
        const auto k = numbers(*v);
        if (!k.empty()) rot = k[0];
    }
    if (const auto* v = n.prop("scale")) {
        const auto k = numbers(*v);
        if (k.size() >= 2) sx = k[0], sy = k[1];
    }
    if (const auto* v = n.prop("skew")) {
        const auto k = numbers(*v);
        if (!k.empty()) skew = k[0];
    }
    if (const auto* v = n.prop("transform")) {  // Transform2D(xx, xy, yx, yy, ox, oy)
        const auto k = numbers(*v);
        if (k.size() >= 6) return {k[0], k[1], k[2], k[3], k[4], k[5]};
    }
    // Godot: x axis = (cos r, sin r) * sx, y axis = (-sin(r + skew), cos(r + skew)) * sy
    Xform2 t;
    t.a = std::cos(rot) * sx;
    t.b = std::sin(rot) * sx;
    t.c = -std::sin(rot + skew) * sy;
    t.d = std::cos(rot + skew) * sy;
    t.e = px;
    t.f = py;
    return t;
}

Xform3 Xform3::operator*(const Xform3& o) const {
    Xform3 r;
    const auto& A = basis;
    const auto& B = o.basis;
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j) r.basis[i * 3 + j] = A[i * 3] * B[j] + A[i * 3 + 1] * B[3 + j] + A[i * 3 + 2] * B[6 + j];
    const auto p = apply(o.origin);
    r.origin = p;
    return r;
}

std::array<double, 3> Xform3::apply(const std::array<double, 3>& p) const {
    return {basis[0] * p[0] + basis[1] * p[1] + basis[2] * p[2] + origin[0], basis[3] * p[0] + basis[4] * p[1] + basis[5] * p[2] + origin[1],
            basis[6] * p[0] + basis[7] * p[1] + basis[8] * p[2] + origin[2]};
}

Xform3 localXform3(const Node& n) {
    Xform3 t;
    if (const auto* v = n.prop("transform")) {  // Transform3D(xx, xy, xz, yx, yy, yz, zx, zy, zz, ox, oy, oz): basis rows
        const auto k = numbers(*v);
        if (k.size() >= 12) {
            for (int i = 0; i < 9; ++i) t.basis[static_cast<std::size_t>(i)] = k[static_cast<std::size_t>(i)];
            t.origin = {k[9], k[10], k[11]};
            return t;
        }
    }
    double rx = 0, ry = 0, rz = 0, sx = 1, sy = 1, sz = 1;
    if (const auto* v = n.prop("position")) {
        const auto k = numbers(*v);
        if (k.size() >= 3) t.origin = {k[0], k[1], k[2]};
    }
    if (const auto* v = n.prop("rotation")) {
        const auto k = numbers(*v);
        if (k.size() >= 3) rx = k[0], ry = k[1], rz = k[2];
    }
    if (const auto* v = n.prop("scale")) {
        const auto k = numbers(*v);
        if (k.size() >= 3) sx = k[0], sy = k[1], sz = k[2];
    }
    // Godot's default euler order YXZ: basis = Ry * Rx * Rz, then scaled per column
    const double cx = std::cos(rx), sxr = std::sin(rx), cy = std::cos(ry), syr = std::sin(ry), cz = std::cos(rz), szr = std::sin(rz);
    const std::array<double, 9> Ry{cy, 0, syr, 0, 1, 0, -syr, 0, cy}, Rx{1, 0, 0, 0, cx, -sxr, 0, sxr, cx}, Rz{cz, -szr, 0, szr, cz, 0, 0, 0, 1};
    auto mul = [](const std::array<double, 9>& A, const std::array<double, 9>& B) {
        std::array<double, 9> r{};
        for (int i = 0; i < 3; ++i)
            for (int j = 0; j < 3; ++j) r[static_cast<std::size_t>(i * 3 + j)] = A[static_cast<std::size_t>(i * 3)] * B[static_cast<std::size_t>(j)] + A[static_cast<std::size_t>(i * 3 + 1)] * B[static_cast<std::size_t>(3 + j)] + A[static_cast<std::size_t>(i * 3 + 2)] * B[static_cast<std::size_t>(6 + j)];
        return r;
    };
    auto b = mul(mul(Ry, Rx), Rz);
    for (int i = 0; i < 3; ++i) {
        b[static_cast<std::size_t>(i * 3)] *= sx;
        b[static_cast<std::size_t>(i * 3 + 1)] *= sy;
        b[static_cast<std::size_t>(i * 3 + 2)] *= sz;
    }
    t.basis = b;
    return t;
}

// ---- scripts ----------------------------------------------------------------------------

std::string scriptTemplate(const std::string& base, const std::string& kind) {
    if (kind == "character2d") {
        return "extends " + base +
               "\n\n"
               "const SPEED = 300.0\n"
               "const JUMP_VELOCITY = -400.0\n\n\n"
               "func _physics_process(delta: float) -> void:\n"
               "\t# gravity\n"
               "\tif not is_on_floor():\n"
               "\t\tvelocity += get_gravity() * delta\n\n"
               "\t# jump\n"
               "\tif Input.is_action_just_pressed(\"ui_accept\") and is_on_floor():\n"
               "\t\tvelocity.y = JUMP_VELOCITY\n\n"
               "\t# left and right\n"
               "\tvar direction := Input.get_axis(\"ui_left\", \"ui_right\")\n"
               "\tif direction:\n"
               "\t\tvelocity.x = direction * SPEED\n"
               "\telse:\n"
               "\t\tvelocity.x = move_toward(velocity.x, 0, SPEED)\n\n"
               "\tmove_and_slide()\n";
    }
    if (kind == "topdown2d") {
        return "extends " + base +
               "\n\n"
               "const SPEED = 200.0\n\n\n"
               "func _physics_process(_delta: float) -> void:\n"
               "\tvar direction := Input.get_vector(\"ui_left\", \"ui_right\", \"ui_up\", \"ui_down\")\n"
               "\tvelocity = direction * SPEED\n"
               "\tmove_and_slide()\n";
    }
    if (kind == "character3d") {
        return "extends " + base +
               "\n\n"
               "const SPEED = 5.0\n"
               "const JUMP_VELOCITY = 4.5\n\n\n"
               "func _physics_process(delta: float) -> void:\n"
               "\tif not is_on_floor():\n"
               "\t\tvelocity += get_gravity() * delta\n\n"
               "\tif Input.is_action_just_pressed(\"ui_accept\") and is_on_floor():\n"
               "\t\tvelocity.y = JUMP_VELOCITY\n\n"
               "\tvar input_dir := Input.get_vector(\"ui_left\", \"ui_right\", \"ui_up\", \"ui_down\")\n"
               "\tvar direction := (transform.basis * Vector3(input_dir.x, 0, input_dir.y)).normalized()\n"
               "\tif direction:\n"
               "\t\tvelocity.x = direction.x * SPEED\n"
               "\t\tvelocity.z = direction.z * SPEED\n"
               "\telse:\n"
               "\t\tvelocity.x = move_toward(velocity.x, 0, SPEED)\n"
               "\t\tvelocity.z = move_toward(velocity.z, 0, SPEED)\n\n"
               "\tmove_and_slide()\n";
    }
    if (kind == "area") {
        return "extends " + base +
               "\n\n\n"
               "func _ready() -> void:\n"
               "\tbody_entered.connect(_on_body_entered)\n\n\n"
               "func _on_body_entered(body: Node) -> void:\n"
               "\tprint(body.name, \" entered \", name)\n";
    }
    return "extends " + base +
           "\n\n\n"
           "# Called when the node enters the scene tree for the first time.\n"
           "func _ready() -> void:\n"
           "\tpass\n\n\n"
           "# Called every frame. 'delta' is the elapsed time since the previous frame.\n"
           "func _process(delta: float) -> void:\n"
           "\tpass\n";
}

// ---- documents --------------------------------------------------------------------------

std::string SceneDoc::summary() const {
    int count = 0;
    std::function<void(const Node&)> walk = [&](const Node& n) {
        ++count;
        for (const auto& c : n.children) walk(c);
    };
    walk(scene.root);
    std::string s = scene.root.name + " (" + (scene.root.type.empty() ? std::string("instance") : scene.root.type) + "), " + std::to_string(count) +
                    " node" + (count == 1 ? "" : "s");
    if (!path.empty()) s += ", " + path;
    return s;
}

Result SceneEdit::apply() {
    before_ = doc_->scene;
    beforePath_ = doc_->file;
    Result r;
    try {
        r = fn_(*doc_);
    } catch (const std::exception& e) {
        r = Result::failure(e.what());
    }
    if (!r.ok) revert();
    return r;
}

void SceneEdit::revert() {
    doc_->scene = before_;
    doc_->setFile(beforePath_);
}

}  // namespace wetwaresuite::scene
