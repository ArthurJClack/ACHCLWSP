// Play test for 2D scenes: see scene_play.hpp for what is and isn't simulated.
#include "wetwaresuite/levels/scene_play.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <fstream>
#include <functional>
#include <map>
#include <sstream>

#include "wetwaresuite/levels/levels.hpp"
#include "wetwaresuite/raster/ops.hpp"

namespace wetwaresuite::scene {

namespace {

constexpr double kPi = 3.14159265358979323846;

std::string slurp(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

// ExtResource("a") / SubResource("b") references renamed through the maps.
std::string remapRefs(const std::string& v, const std::map<std::string, std::string>& ext, const std::map<std::string, std::string>& sub) {
    if (v.find("Resource(\"") == std::string::npos) return v;
    std::string out;
    std::size_t i = 0;
    while (i < v.size()) {
        const std::size_t e = v.find("ExtResource(\"", i), s = v.find("SubResource(\"", i);
        const std::size_t at = std::min(e, s);
        if (at == std::string::npos) {
            out.append(v, i, std::string::npos);
            break;
        }
        const bool isExt = at == e;
        const std::size_t open = at + 13;
        const std::size_t close = v.find('"', open);
        if (close == std::string::npos) {
            out.append(v, i, std::string::npos);
            break;
        }
        out.append(v, i, open - i);
        const std::string id = v.substr(open, close - open);
        const auto& m = isExt ? ext : sub;
        const auto it = m.find(id);
        out += it == m.end() ? id : it->second;
        i = close;
    }
    return out;
}

void remapNode(Node& n, const std::map<std::string, std::string>& ext, const std::map<std::string, std::string>& sub) {
    for (auto& [k, v] : n.props) v = remapRefs(v, ext, sub);
    if (!n.instance.empty()) {
        if (auto it = ext.find(n.instance); it != ext.end()) n.instance = it->second;
    }
    for (auto& c : n.children) remapNode(c, ext, sub);
}

Xform2 inverse(const Xform2& m) {
    const double det = m.a * m.d - m.b * m.c;
    if (std::abs(det) < 1e-12) return {};
    Xform2 r;
    r.a = m.d / det;
    r.b = -m.b / det;
    r.c = -m.c / det;
    r.d = m.a / det;
    r.e = -(r.a * m.e + r.c * m.f);
    r.f = -(r.b * m.e + r.d * m.f);
    return r;
}

bool is2d(const Node& n) { return inherits(n.type, "Node2D") || inherits(n.type, "Control"); }

double num(const Node& n, const char* key, double def) {
    const auto* v = n.prop(key);
    if (!v) return def;
    const auto k = numbers(*v);
    return k.empty() ? def : k[0];
}

// A script constant, e.g. "const SPEED = 300.0" or "const SPEED: float = 300.0".
double scriptConst(const std::string& code, const std::string& name, double def) {
    std::size_t at = 0;
    while ((at = code.find("const " + name, at)) != std::string::npos) {
        const std::size_t after = at + 6 + name.size();
        at = after;
        if (after < code.size() && (std::isalnum(static_cast<unsigned char>(code[after])) || code[after] == '_')) continue;  // SPEED_UP
        const std::size_t eq = code.find('=', after), nl = code.find('\n', after);
        if (eq == std::string::npos || (nl != std::string::npos && eq > nl)) continue;
        const auto k = numbers(code.substr(eq + 1, (nl == std::string::npos ? code.size() : nl) - eq - 1));
        if (!k.empty()) return k[0];
    }
    return def;
}

// The indented body of `func name(...)`.
std::string funcBody(const std::string& code, const std::string& name) {
    const std::size_t at = code.find("func " + name + "(");
    if (at == std::string::npos) return {};
    std::size_t i = code.find('\n', at);
    std::string out;
    while (i != std::string::npos && i + 1 < code.size()) {
        const std::size_t nl = code.find('\n', i + 1);
        const std::string line = code.substr(i + 1, (nl == std::string::npos ? code.size() : nl) - i - 1);
        const bool blank = line.find_first_not_of(" \t\r") == std::string::npos;
        if (!blank && line[0] != '\t' && line[0] != ' ') break;
        out += line + "\n";
        i = nl;
    }
    return out;
}

// The nodes a method's queue_free() calls take away: "@body" for body.queue_free(), node paths
// for $Name.queue_free() / get_node("Name").queue_free() (from the script's node), and the
// script's own node for a bare queue_free().
void freedBy(const std::string& body, const std::string& owner, std::vector<std::string>& out) {
    auto rel = [&](std::string name) {
        while (!name.empty() && (name.back() == '"' || name.back() == '\'' || name.back() == ')')) name.pop_back();
        if (!name.empty() && (name.front() == '"' || name.front() == '\'')) name.erase(0, 1);
        if (name.rfind("../", 0) == 0) {  // a sibling of the script's node
            const auto slash = owner.rfind('/');
            const std::string up = owner == "." || slash == std::string::npos ? "." : owner.substr(0, slash);
            name = name.substr(3);
            return up == "." ? name : up + "/" + name;
        }
        return owner == "." ? name : owner + "/" + name;
    };
    for (std::size_t at = body.find("queue_free("); at != std::string::npos; at = body.find("queue_free(", at + 1)) {
        std::size_t lineStart = body.rfind('\n', at);
        lineStart = lineStart == std::string::npos ? 0 : lineStart + 1;
        std::string before = body.substr(lineStart, at - lineStart);
        const auto first = before.find_first_not_of(" \t");
        before = first == std::string::npos ? std::string() : before.substr(first);
        if (before.empty() || before == "self.") {
            out.push_back(owner);
        } else if (before == "body." || before == "area.") {
            out.push_back("@body");
        } else if (before.front() == '$' && before.back() == '.') {
            out.push_back(rel(before.substr(1, before.size() - 2)));
        } else if (before.rfind("get_node(", 0) == 0 && before.back() == '.') {
            out.push_back(rel(before.substr(9, before.size() - 10)));
        }
    }
}

std::string scriptOf(const Scene& s, const Node& n) {
    const auto* sc = n.prop("script");
    if (!sc) return {};
    const ExtResource* e = s.extById(refId(*sc, "ExtResource"));
    if (!e) return {};
    std::error_code ec;
    return fs::exists(e->file, ec) ? slurp(e->file) : std::string();
}

// Collision shape points in the shape node's own space.
std::vector<std::array<double, 2>> shapePoints(const Scene& s, const Node& n) {
    std::vector<std::array<double, 2>> pts;
    if (n.prop("disabled") && *n.prop("disabled") == "true") return pts;
    if (n.type == "CollisionPolygon2D") {
        const auto v = n.prop("polygon") ? numbers(*n.prop("polygon")) : std::vector<double>{};
        for (std::size_t i = 0; i + 1 < v.size(); i += 2) pts.push_back({v[i], v[i + 1]});
        return pts;
    }
    const auto* sv = n.prop("shape");
    if (!sv) return pts;
    const SubResource* r = s.subById(refId(*sv, "SubResource"));
    if (!r) return pts;
    auto nums = [&](const char* k) { return r->prop(k) ? numbers(*r->prop(k)) : std::vector<double>{}; };
    if (r->type == "RectangleShape2D") {
        auto sz = nums("size");
        if (sz.size() < 2) sz = {20, 20};
        pts = {{-sz[0] / 2, -sz[1] / 2}, {sz[0] / 2, sz[1] / 2}, {-sz[0] / 2, sz[1] / 2}, {sz[0] / 2, -sz[1] / 2}};
    } else if (r->type == "CircleShape2D" || r->type == "CapsuleShape2D") {
        const double rad = nums("radius").empty() ? 10 : nums("radius")[0];
        const double h = r->type == "CapsuleShape2D" ? (nums("height").empty() ? 30 : nums("height")[0]) : 2 * rad;
        const double hy = std::max(rad, h / 2);
        for (int i = 0; i < 8; ++i) {
            const double a = 2 * kPi * i / 8;
            pts.push_back({rad * std::cos(a), (std::sin(a) >= 0 ? hy - rad : rad - hy) + rad * std::sin(a)});
        }
        pts.push_back({0, hy});
        pts.push_back({0, -hy});
        pts.push_back({rad, 0});
        pts.push_back({-rad, 0});
    } else if (r->type == "SegmentShape2D") {
        const auto a = nums("a"), b = nums("b");
        if (a.size() == 2 && b.size() == 2) pts = {{a[0], a[1]}, {b[0], b[1]}};
    } else if (r->type == "WorldBoundaryShape2D") {
        // an endless floor: a very wide box below the line
        pts = {{-1e6, 0}, {1e6, 1e6}};
    }
    return pts;
}

std::map<std::string, std::string> projectSettings(const fs::path& project) {
    std::map<std::string, std::string> out;
    if (project.empty()) return out;
    std::ifstream in(project / "project.godot");
    std::string line, section;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty() || line[0] == ';') continue;
        if (line[0] == '[') {
            section = line.substr(1, line.find(']') - 1);
            continue;
        }
        const auto eq = line.find('=');
        if (eq == std::string::npos) continue;
        out[section + "/" + line.substr(0, eq)] = line.substr(eq + 1);
    }
    return out;
}

}  // namespace

Scene inlineInstances(const Scene& s) {
    Scene out = s;
    std::function<void(Node&, const std::string&, int)> expand = [&](Node& n, const std::string& path, int depth) {
        if (!n.instance.empty() && depth < 8) {
            const ExtResource* e = out.extById(n.instance);
            Scene inst;
            bool loaded = false;
            if (e) {
                try {
                    std::error_code ec;
                    if (fs::exists(e->file, ec)) {
                        inst = inlineInstances(readTscn(slurp(e->file), e->file));
                        loaded = true;
                    }
                } catch (const std::exception&) {
                }
            }
            if (loaded) {
                std::map<std::string, std::string> extMap, subMap;
                for (const auto& x : inst.ext) extMap[x.id] = out.addExt(x.type, x.file);
                std::vector<std::size_t> added;
                for (const auto& x : inst.sub) {
                    subMap[x.id] = out.addSub(x.type);
                    added.push_back(out.sub.size() - 1);
                }
                for (std::size_t i = 0; i < inst.sub.size(); ++i) {
                    auto& dst = out.sub[added[i]];
                    for (const auto& [k, v] : inst.sub[i].props) dst.props.emplace_back(k, remapRefs(v, extMap, subMap));
                }
                Node m = inst.root;
                remapNode(m, extMap, subMap);
                for (const auto& [k, v] : n.props) m.set(k, v);
                m.name = n.name;
                for (const auto& g : n.groups) m.groups.push_back(g);
                m.instance.clear();
                for (auto& c : n.children) m.children.push_back(std::move(c));
                for (auto c : inst.connections) {
                    auto prefix = [&](const std::string& p) { return p == "." ? path : path + "/" + p; };
                    c.from = prefix(c.from);
                    c.to = prefix(c.to);
                    out.connections.push_back(c);
                }
                n = std::move(m);
            } else {
                n.instance.clear();
            }
        }
        for (auto& c : n.children) expand(c, path == "." ? c.name : path + "/" + c.name, depth + 1);
    };
    expand(out.root, ".", 0);
    return out;
}

Play::Play(const Scene& source) : scene_(inlineInstances(source)) {
    const auto settings = projectSettings(scene_.project);
    auto setting = [&](const std::string& key, double def) {
        const auto it = settings.find(key);
        if (it == settings.end()) return def;
        const auto k = numbers(it->second);
        return k.empty() ? def : k[0];
    };
    winW_ = static_cast<int>(std::clamp(setting("display/window/size/viewport_width", 1152), 16.0, 4096.0));
    winH_ = static_cast<int>(std::clamp(setting("display/window/size/viewport_height", 648), 16.0, 4096.0));
    gravity_ = setting("physics/2d/default_gravity", 980);

    double bottom = -1e18;
    auto addSolid = [&](const std::vector<std::array<double, 2>>& pts, const Xform2& xf, bool oneWay) {
        if (pts.empty()) return;
        Box b{1e18, 1e18, -1e18, -1e18};
        for (const auto& p : pts) {
            const auto q = xf.apply(p[0], p[1]);
            b.x0 = std::min(b.x0, q[0]), b.y0 = std::min(b.y0, q[1]), b.x1 = std::max(b.x1, q[0]), b.y1 = std::max(b.y1, q[1]);
        }
        if (b.y1 - b.y0 < 1) b.y1 = b.y0 + 1;  // segments and flat polygons still stop things
        if (b.x1 - b.x0 < 1) b.x1 = b.x0 + 1;
        solids_.push_back({b, oneWay});
        if (b.y1 < 1e5) bottom = std::max(bottom, b.y1);
    };

    // a body's shapes, relative to the body (nested bodies have their own)
    std::function<void(const Node&, const Xform2&, std::vector<std::pair<std::vector<std::array<double, 2>>, Xform2>>&, bool&)> shapesOf =
        [&](const Node& n, const Xform2& rel, auto& outShapes, bool& oneWay) {
            for (const auto& c : n.children) {
                if (inherits(c.type, "CollisionObject2D")) continue;
                const Xform2 x = is2d(c) ? rel * localXform2(c) : rel;
                if (c.type == "CollisionShape2D" || c.type == "CollisionPolygon2D") {
                    auto pts = shapePoints(scene_, c);
                    if (!pts.empty()) outShapes.push_back({std::move(pts), x});
                    if (c.prop("one_way_collision") && *c.prop("one_way_collision") == "true") oneWay = true;
                }
                shapesOf(c, x, outShapes, oneWay);
            }
        };

    bool godotTiles = false;
    std::function<void(const Node&, const std::string&, const Xform2&)> walk = [&](const Node& n, const std::string& path, const Xform2& parent) {
        const Xform2 world = is2d(n) ? parent * localXform2(n) : parent;
        const std::string& t = n.type;
        if (inherits(t, "CollisionObject2D")) {
            std::vector<std::pair<std::vector<std::array<double, 2>>, Xform2>> shapes;
            bool oneWay = false;
            shapesOf(n, Xform2{}, shapes, oneWay);
            const bool frozen = n.prop("freeze") && *n.prop("freeze") == "true";
            if (t == "StaticBody2D" || t == "AnimatableBody2D" || (t == "RigidBody2D" && frozen)) {
                for (const auto& [pts, rel] : shapes) addSolid(pts, world * rel, oneWay);
            } else if (t == "Area2D") {
                Area a;
                a.path = path;
                a.box = {1e18, 1e18, -1e18, -1e18};
                for (const auto& [pts, rel] : shapes) {
                    for (const auto& p : pts) {
                        const auto q = (world * rel).apply(p[0], p[1]);
                        a.box.x0 = std::min(a.box.x0, q[0]), a.box.y0 = std::min(a.box.y0, q[1]);
                        a.box.x1 = std::max(a.box.x1, q[0]), a.box.y1 = std::max(a.box.y1, q[1]);
                    }
                }
                if (shapes.empty()) notes_.push_back(path + " (Area2D) has no collision shape, so nothing can enter it");
                // what entering does: the connected methods (and the area's own _on_body_entered)
                for (const auto& c : scene_.connections) {
                    if (c.signal != "body_entered" || c.from != path) continue;
                    const Node* target = scene_.find(c.to);
                    if (!target) continue;
                    freedBy(funcBody(scriptOf(scene_, *target), c.method), scene_.pathOf(target), a.frees);
                    a.method = c.method;
                }
                const std::string own = scriptOf(scene_, n);
                if (own.find("body_entered.connect(") != std::string::npos) {
                    const auto at = own.find("body_entered.connect(") + 21;
                    const std::string m = own.substr(at, own.find(')', at) - at);
                    freedBy(funcBody(own, m), path, a.frees);
                    if (a.method.empty()) a.method = m;
                }
                if (!shapes.empty()) areas_.push_back(a);
            } else if (t == "CharacterBody2D" || t == "RigidBody2D") {
                PlayBody b;
                b.path = path;
                b.type = t;
                b.x = world.e;
                b.y = world.f;
                Mover m;
                m.body = bodies_.size();
                m.parentInv = inverse(parent);
                for (const auto& [pts, rel] : shapes) {
                    Box bx{1e18, 1e18, -1e18, -1e18};
                    // the shape box in world axes, relative to the body's origin
                    const Xform2 rot{world.a, world.b, world.c, world.d, 0, 0};
                    for (const auto& p : pts) {
                        const auto q = (rot * rel).apply(p[0], p[1]);
                        bx.x0 = std::min(bx.x0, q[0]), bx.y0 = std::min(bx.y0, q[1]), bx.x1 = std::max(bx.x1, q[0]), bx.y1 = std::max(bx.y1, q[1]);
                    }
                    m.shapes.push_back(bx);
                }
                if (t == "RigidBody2D") {
                    m.kind = Mover::Rigid;
                    m.gravityScale = num(n, "gravity_scale", 1);
                    if (const auto* lv = n.prop("linear_velocity")) {
                        const auto k = numbers(*lv);
                        if (k.size() >= 2) b.vx = k[0], b.vy = k[1];
                    }
                } else {
                    const std::string code = scriptOf(scene_, n);
                    if (code.empty()) {
                        m.kind = Mover::Still;
                        notes_.push_back(path + " has no script, so it doesn't move (in Godot neither): attach one with scene.script template=character2d");
                    } else if (code.find("get_vector(") != std::string::npos) {
                        m.kind = Mover::TopDown;
                        m.speed = scriptConst(code, "SPEED", 200);
                    } else if (code.find("is_on_floor") != std::string::npos || code.find("gravity") != std::string::npos ||
                               code.find("get_axis(") != std::string::npos) {
                        m.kind = Mover::Platformer;
                        m.speed = scriptConst(code, "SPEED", 300);
                        m.jump = scriptConst(code, "JUMP_VELOCITY", -400);
                    } else {
                        m.kind = Mover::Still;
                        notes_.push_back(path + "'s script isn't a mover the play test knows (platformer or top-down starter), so it stays put");
                    }
                }
                if (shapes.empty()) notes_.push_back(path + " has no collision shape, so it falls through everything (in Godot too)");
                bodies_.push_back(b);
                movers_.push_back(m);
            }
        } else if ((t == "TileMap" || t == "TileMapLayer")) {
            if (const auto* src = n.prop("metadata/wetwaresuite_source")) {
                try {
                    const levels::Map map = levels::loadMap(fromRes(unquote(*src).value_or(""), scene_.project));
                    // tiles whose TileSet tile has physics (solid=true or its own shapes) are solid, as in Godot;
                    // collision layers and collision objects came over as StaticBody2D nodes already
                    std::vector<bool> solid(static_cast<std::size_t>(map.width) * map.height, false);
                    for (const auto& l : map.layers) {
                        if (l.kind != levels::LayerKind::Tiles || l.collision) continue;
                        for (std::size_t i = 0; i < l.cells.size() && i < solid.size(); ++i) {
                            const std::uint32_t raw = l.cells[i] & levels::kGidMask;
                            if (!raw) continue;
                            const levels::Tileset* ts = map.tilesetFor(raw);
                            if (!ts) continue;
                            const int id = static_cast<int>(raw - ts->firstGid);
                            bool hard = ts->tileShapes.count(id) && !ts->tileShapes.at(id).empty();
                            if (auto it = ts->tileProps.find(id); it != ts->tileProps.end()) {
                                if (auto q = it->second.find("solid"); q != it->second.end() && (q->second.value == "true" || q->second.value == "1")) hard = true;
                            }
                            if (hard) solid[i] = true;
                        }
                    }
                    for (const auto& r : levels::mergeRects(map.width, map.height, solid)) {
                        const double x0 = r.x * map.tileWidth, y0 = r.y * map.tileHeight;
                        addSolid({{x0, y0}, {x0 + r.w * map.tileWidth, y0 + r.h * map.tileHeight}}, world, false);
                    }
                } catch (const std::exception& e) {
                    notes_.push_back(path + ": couldn't read its level (" + std::string(e.what()) + ")");
                }
            } else {
                godotTiles = true;
            }
        }
        for (const auto& c : n.children) walk(c, path == "." ? c.name : path + "/" + c.name, world);
    };
    walk(scene_.root, ".", Xform2{});
    if (solids_.empty() && !movers_.empty())
        notes_.push_back("nothing in the scene is solid, so bodies fall forever (in Godot too): give the level collision with levels.collision, or add a StaticBody2D");
    if (godotTiles) notes_.push_back("tiles painted in Godot's TileSet editor aren't solid in the play test (levels made here are)");
    // falling below everything ends a body's run (Godot would keep it falling)
    killY_ = bottom > -1e17 ? bottom + 2000 : 1e5;
}

Play::Box Play::worldBox(const Mover& m) const {
    const PlayBody& b = bodies_[m.body];
    Box out{1e18, 1e18, -1e18, -1e18};
    for (const auto& s : m.shapes) {
        out.x0 = std::min(out.x0, b.x + s.x0), out.y0 = std::min(out.y0, b.y + s.y0);
        out.x1 = std::max(out.x1, b.x + s.x1), out.y1 = std::max(out.y1, b.y + s.y1);
    }
    return out;
}

namespace {
bool overlap(double ax0, double ay0, double ax1, double ay1, double bx0, double by0, double bx1, double by1) {
    return ax0 < bx1 - 1e-6 && ax1 > bx0 + 1e-6 && ay0 < by1 - 1e-6 && ay1 > by0 + 1e-6;
}
}  // namespace

void Play::moveAxis(Mover& m, double d, bool xAxis) {
    PlayBody& b = bodies_[m.body];
    if (m.shapes.empty()) {
        (xAxis ? b.x : b.y) += d;
        return;
    }
    // small steps so fast bodies don't pass through thin floors
    const int n = std::max(1, static_cast<int>(std::ceil(std::abs(d) / 4)));
    const double part = d / n;
    for (int i = 0; i < n; ++i) {
        const Box before = worldBox(m);
        (xAxis ? b.x : b.y) += part;
        const Box now = worldBox(m);
        bool blocked = false;
        double fix = 0;
        auto test = [&](const Box& s, bool oneWay) {
            if (!overlap(now.x0, now.y0, now.x1, now.y1, s.x0, s.y0, s.x1, s.y1)) return;
            if (oneWay && (xAxis || part <= 0 || before.y1 > s.y0 + 0.5)) return;  // one-way: only landing on top
            double push = 0;
            if (xAxis) push = part > 0 ? s.x0 - now.x1 : s.x1 - now.x0;
            else push = part > 0 ? s.y0 - now.y1 : s.y1 - now.y0;
            if (!blocked || std::abs(push) > std::abs(fix)) fix = push;
            blocked = true;
        };
        for (const auto& s : solids_) test(s.box, s.oneWay);
        for (std::size_t k = 0; k < movers_.size(); ++k) {
            const Mover& o = movers_[k];
            if (o.body == m.body || bodies_[o.body].gone || o.shapes.empty()) continue;
            test(worldBox(o), false);
        }
        if (blocked) {
            (xAxis ? b.x : b.y) += fix;
            if (xAxis) {
                b.vx = 0;
            } else {
                if (part > 0) b.onFloor = true;
                b.vy = 0;
            }
            return;
        }
    }
}

void Play::steps(const PlayInput& in, double dt) {
    const bool jumpNow = in.jump && !jumpHeld_;
    jumpHeld_ = in.jump;
    const double dir = (in.right ? 1.0 : 0.0) - (in.left ? 1.0 : 0.0);
    for (auto& m : movers_) {
        PlayBody& b = bodies_[m.body];
        if (b.gone) continue;
        switch (m.kind) {
            case Mover::Platformer:
                if (!b.onFloor) b.vy += gravity_ * dt;
                if (jumpNow && b.onFloor) b.vy = m.jump;
                if (dir != 0) b.vx = dir * m.speed;
                else b.vx = 0;  // move_toward(velocity.x, 0, SPEED) stops in one step
                break;
            case Mover::TopDown: {
                double vx = dir, vy = (in.down ? 1.0 : 0.0) - (in.up ? 1.0 : 0.0);
                const double len = std::hypot(vx, vy);
                if (len > 1) vx /= len, vy /= len;
                b.vx = vx * m.speed;
                b.vy = vy * m.speed;
                break;
            }
            case Mover::Rigid:
                b.vy += gravity_ * m.gravityScale * dt;
                if (b.onFloor) b.vx *= std::pow(0.02, dt);  // friction
                break;
            case Mover::Still: continue;
        }
        const bool wasOnFloor = b.onFloor;
        b.onFloor = false;
        moveAxis(m, b.vx * dt, true);
        moveAxis(m, b.vy * dt, false);
        // standing still on a floor: probe a little down so is_on_floor stays true
        if (!b.onFloor && wasOnFloor && b.vy >= 0 && m.kind != Mover::TopDown) {
            const double y = b.y;
            moveAxis(m, 1, false);
            if (!b.onFloor) b.y = y;
        }
        if (b.y > killY_ && !b.gone) {
            b.gone = true;
            events_.push_back({time_, b.path + " fell out of the level"});
        }
    }
    // areas
    for (auto& a : areas_) {
        if (a.gone) continue;
        for (const auto& m : movers_) {
            PlayBody& b = bodies_[m.body];
            if (b.gone || m.shapes.empty()) continue;
            const Box w = worldBox(m);
            const bool in = overlap(w.x0, w.y0, w.x1, w.y1, a.box.x0, a.box.y0, a.box.x1, a.box.y1);
            const auto it = std::find(a.inside.begin(), a.inside.end(), b.path);
            if (in && it == a.inside.end()) {
                a.inside.push_back(b.path);
                std::string what = b.path + " entered " + a.path;
                if (!a.method.empty()) what += " -> " + a.method + "()";
                for (const auto& f : a.frees) {
                    const std::string gonePath = f == "@body" ? b.path : f;
                    what += ", " + (gonePath == "." ? scene_.root.name : gonePath) + " freed";
                    if (gonePath == ".") {
                        what += " (the whole scene: the game would end here)";
                        continue;
                    }
                    if (Node* n = scene_.find(gonePath)) n->set("visible", "false");
                    auto under = [&](const std::string& q) { return q == gonePath || q.rfind(gonePath + "/", 0) == 0; };
                    for (auto& o : areas_) {
                        if (!o.gone && under(o.path)) o.gone = true, ++freed_;
                    }
                    for (auto& ob : bodies_) {
                        if (!ob.gone && under(ob.path)) ob.gone = true;
                    }
                }
                if (events_.size() < 500) events_.push_back({time_, what});
                if (a.gone) break;
            } else if (!in && it != a.inside.end()) {
                a.inside.erase(it);
            }
        }
    }
    time_ += dt;
}

void Play::step(const PlayInput& in, double seconds) {
    constexpr double dt = 1.0 / 60;
    carry_ += std::max(0.0, seconds);
    int guard = 0;
    while (carry_ >= dt - 1e-9 && guard++ < 600) {
        steps(in, dt);
        carry_ -= dt;
    }
    if (guard >= 600) carry_ = 0;
    // write positions back for drawing
    for (const auto& m : movers_) {
        const PlayBody& b = bodies_[m.body];
        if (Node* n = scene_.find(b.path)) {
            const auto local = m.parentInv.apply(b.x, b.y);
            n->set("position", "Vector2(" + gdNum(local[0]) + ", " + gdNum(local[1]) + ")");
        }
    }
}

raster::Image Play::frame(double scale) const {
    scale = std::clamp(scale, 0.1, 8.0);
    // the camera: the first enabled Camera2D, followed through its parents
    double cx = winW_ / 2.0, cy = winH_ / 2.0, zx = 1, zy = 1;
    bool centred = false, haveCam = false;
    double limits[4] = {-1e7, -1e7, 1e7, 1e7};
    std::function<void(const Node&, const Xform2&)> findCam = [&](const Node& n, const Xform2& parent) {
        if (haveCam) return;
        const Xform2 world = is2d(n) ? parent * localXform2(n) : parent;
        if (n.type == "Camera2D" && !(n.prop("enabled") && *n.prop("enabled") == "false")) {
            haveCam = true;
            double ox = 0, oy = 0;
            if (const auto* o = n.prop("offset")) {
                const auto k = numbers(*o);
                if (k.size() >= 2) ox = k[0], oy = k[1];
            }
            cx = world.e + ox;
            cy = world.f + oy;
            if (const auto* z = n.prop("zoom")) {
                const auto k = numbers(*z);
                if (k.size() >= 2) zx = std::max(0.01, k[0]), zy = std::max(0.01, k[1]);
            }
            centred = !n.prop("anchor_mode") || *n.prop("anchor_mode") != "0";
            limits[0] = num(n, "limit_left", -1e7);
            limits[1] = num(n, "limit_top", -1e7);
            limits[2] = num(n, "limit_right", 1e7);
            limits[3] = num(n, "limit_bottom", 1e7);
            return;
        }
        for (const auto& c : n.children) findCam(c, world);
    };
    findCam(scene_.root, Xform2{});
    const double vw = winW_ / zx, vh = winH_ / zy;
    double ox = haveCam ? (centred ? cx - vw / 2 : cx) : 0, oy = haveCam ? (centred ? cy - vh / 2 : cy) : 0;
    if (haveCam) {
        if (limits[2] - limits[0] >= vw) ox = std::clamp(ox, limits[0], limits[2] - vw);
        if (limits[3] - limits[1] >= vh) oy = std::clamp(oy, limits[1], limits[3] - vh);
    }
    // the world without the HUD, then the HUD on top in screen space
    Scene world = scene_;
    Scene hud = scene_;
    hud.root = Node{};
    hud.root.name = "HUD";
    hud.root.type = "Node";
    std::function<void(Node&)> pull = [&](Node& n) {
        for (auto it = n.children.begin(); it != n.children.end();) {
            if (it->type == "CanvasLayer") {
                if (!(it->prop("visible") && *it->prop("visible") == "false")) hud.root.children.push_back(*it);
                it = n.children.erase(it);
            } else {
                pull(*it);
                ++it;
            }
        }
    };
    pull(world.root);
    RenderOptions2D o;
    o.helpers = false;
    o.axes = false;
    o.background = raster::Color{77, 77, 77, 255};
    View2D v;
    v.originX = ox;
    v.originY = oy;
    v.scale = zx * scale;
    v.width = std::max(16, static_cast<int>(std::lround(winW_ * scale)));
    v.height = std::max(16, static_cast<int>(std::lround(winH_ * scale)));
    o.view = v;
    raster::Image img = render2D(world, o);
    if (!hud.root.children.empty()) {
        RenderOptions2D h = o;
        h.background = raster::Color{0, 0, 0, 0};
        View2D hv = v;
        hv.originX = 0;
        hv.originY = 0;
        hv.scale = scale;
        h.view = hv;
        const raster::Image top = render2D(hud, h);
        raster::blendOnto(img, top, 0, 0, raster::Blend::Normal, 1.0);
    }
    return img;
}

}  // namespace wetwaresuite::scene
