// Pictures of scenes: 2D scenes drawn like Godot's 2D editor (sprites, polygons, tiles, UI boxes,
// with collision shapes, cameras and markers on top) and 3D scenes through the mesh renderer.
#include <algorithm>
#include <cmath>
#include <fstream>
#include <functional>
#include <map>
#include <sstream>

#include "wetwaresuite/levels/levels.hpp"
#include "wetwaresuite/levels/scene.hpp"
#include "wetwaresuite/mesh/io.hpp"
#include "wetwaresuite/mesh/primitives.hpp"
#include "wetwaresuite/mesh/render.hpp"
#include "wetwaresuite/pdf/page.hpp"
#include "wetwaresuite/raster/codec.hpp"
#include "wetwaresuite/raster/ops.hpp"
#include "wetwaresuite/raster/paint.hpp"

namespace wetwaresuite::scene {

namespace {

using raster::Color;
using raster::Image;
constexpr double kPi = 3.14159265358979323846;

std::string readAll(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

std::shared_ptr<const Image> loadTexture(const fs::path& file) {
    static std::map<std::string, std::pair<fs::file_time_type, std::shared_ptr<const Image>>> cache;
    std::error_code ec;
    const auto stamp = fs::last_write_time(file, ec);
    if (ec) return nullptr;
    auto& slot = cache[file.string()];
    if (slot.second && slot.first == stamp) return slot.second;
    try {
        slot = {stamp, std::make_shared<const Image>(raster::decode(readAll(file)))};
    } catch (const std::exception&) {
        slot = {stamp, nullptr};
    }
    return slot.second;
}

// An instanced scene file, read once per change.
std::shared_ptr<const Scene> loadScene(const fs::path& file) {
    static std::map<std::string, std::pair<fs::file_time_type, std::shared_ptr<const Scene>>> cache;
    std::error_code ec;
    const auto stamp = fs::last_write_time(file, ec);
    if (ec) return nullptr;
    auto& slot = cache[file.string()];
    if (slot.second && slot.first == stamp) return slot.second;
    try {
        slot = {stamp, std::make_shared<const Scene>(readTscn(readAll(file), file))};
    } catch (const std::exception&) {
        slot = {stamp, nullptr};
    }
    return slot.second;
}

std::shared_ptr<const Image> loadLevel(const fs::path& file) {
    static std::map<std::string, std::pair<fs::file_time_type, std::shared_ptr<const Image>>> cache;
    std::error_code ec;
    const auto stamp = fs::last_write_time(file, ec);
    if (ec) return nullptr;
    auto& slot = cache[file.string()];
    if (slot.second && slot.first == stamp) return slot.second;
    try {
        levels::RenderOptions o;
        o.objects = false;
        o.collision = false;
        slot = {stamp, std::make_shared<const Image>(levels::render(levels::loadMap(file), o))};
    } catch (const std::exception&) {
        slot = {stamp, nullptr};
    }
    return slot.second;
}

Color colorOf(const std::string* v, Color def) {
    if (!v) return def;
    const auto n = numbers(*v);
    if (n.size() < 3) return def;
    auto b = [](double x) { return static_cast<std::uint8_t>(std::lround(std::clamp(x, 0.0, 1.0) * 255)); };
    return {b(n[0]), b(n[1]), b(n[2]), n.size() > 3 ? b(n[3]) : std::uint8_t{255}};
}

Color mulColor(Color a, Color b) {
    return {static_cast<std::uint8_t>(a.r * b.r / 255), static_cast<std::uint8_t>(a.g * b.g / 255), static_cast<std::uint8_t>(a.b * b.b / 255),
            static_cast<std::uint8_t>(a.a * b.a / 255)};
}

bool boolOf(const Node& n, const char* key, bool def) {
    const auto* v = n.prop(key);
    return v ? *v == "true" : def;
}
double numOf(const Node& n, const char* key, double def) {
    const auto* v = n.prop(key);
    if (!v) return def;
    const auto k = numbers(*v);
    return k.empty() ? def : k[0];
}

// One thing to draw, in world space.
struct Item {
    enum Kind { Sprite, Poly, Line, Text, Shape, Camera, Marker, Box } kind = Box;
    std::string path, type;
    Xform2 xf;
    std::shared_ptr<const Image> image;
    double sx0 = 0, sy0 = 0, sw = 0, sh = 0;  // source rect in the image
    double lx = 0, ly = 0, lw = 0, lh = 0;    // local rect it is drawn into
    bool flipH = false, flipV = false;
    std::vector<std::array<double, 2>> pts;   // local points
    Color color{255, 255, 255, 255};
    double width = 1;
    std::string text;
    int z = 0;
    int order = 0;
    bool helper = false;
};

struct Walker {
    const Scene* top = nullptr;
    std::vector<Item> items;
    int order = 0;

    void shapeItems(const Scene& s, const Node& n, const std::string& path, const Xform2& xf) {
        Item it;
        it.kind = Item::Shape;
        it.path = path;
        it.type = n.type;
        it.xf = xf;
        it.helper = true;
        it.color = colorOf(n.prop("debug_color"), Color{0, 153, 179, 107});
        if (boolOf(n, "disabled", false)) it.color = {160, 160, 160, 80};
        if (n.type == "CollisionPolygon2D") {
            const auto v = n.prop("polygon") ? numbers(*n.prop("polygon")) : std::vector<double>{};
            for (std::size_t i = 0; i + 1 < v.size(); i += 2) it.pts.push_back({v[i], v[i + 1]});
        } else if (const auto* sv = n.prop("shape")) {
            const SubResource* r = s.subById(refId(*sv, "SubResource"));
            if (!r) return;
            auto nums = [&](const char* k) { return r->prop(k) ? numbers(*r->prop(k)) : std::vector<double>{}; };
            if (r->type == "RectangleShape2D") {
                auto sz = nums("size");
                if (sz.size() < 2) sz = {20, 20};
                it.pts = {{-sz[0] / 2, -sz[1] / 2}, {sz[0] / 2, -sz[1] / 2}, {sz[0] / 2, sz[1] / 2}, {-sz[0] / 2, sz[1] / 2}};
            } else if (r->type == "CircleShape2D" || r->type == "CapsuleShape2D") {
                const double rad = nums("radius").empty() ? 10 : nums("radius")[0];
                const double h = r->type == "CapsuleShape2D" ? (nums("height").empty() ? 30 : nums("height")[0]) : 2 * rad;
                const double half = std::max(0.0, h / 2 - rad);
                for (int i = 0; i <= 32; ++i) {
                    const double a = 2 * kPi * i / 32;
                    const double cy = std::sin(a) >= 0 ? half : -half;
                    it.pts.push_back({rad * std::cos(a), cy + rad * std::sin(a)});
                }
            } else if (r->type == "SegmentShape2D") {
                const auto a = nums("a"), b = nums("b");
                if (a.size() == 2 && b.size() == 2) it.pts = {{a[0], a[1]}, {b[0], b[1]}};
                it.kind = Item::Line;
                it.width = 2;
            } else {
                return;
            }
        }
        if (it.pts.size() >= 2) items.push_back(it);
    }

    std::string instanceRoot;  // set while drawing an instanced scene's insides

    void walk(const Scene& s, const Node& nIn, const std::string& path, const Xform2& parentXf, Color parentMod, int depth, const Node* override_) {
        Node n = nIn;
        if (override_) {  // an instance node: its own settings win over the instanced root's
            for (const auto& [k, v] : override_->props) n.set(k, v);
            n.name = override_->name;
        }
        if (n.prop("visible") && *n.prop("visible") == "false") return;
        const bool canvas = inherits(n.type, "CanvasItem") || n.type.empty();
        const bool node2d = inherits(n.type, "Node2D") || inherits(n.type, "Control");
        Xform2 xf = parentXf;
        if (node2d) xf = parentXf * localXform2(n);
        Color mod = parentMod;
        if (canvas) mod = mulColor(parentMod, colorOf(n.prop("modulate"), Color{255, 255, 255, 255}));
        const Color self = mulColor(mod, colorOf(n.prop("self_modulate"), Color{255, 255, 255, 255}));
        const int z = static_cast<int>(numOf(n, "z_index", 0));
        auto push = [&](Item it) {
            it.path = instanceRoot.empty() ? path : instanceRoot;  // clicks inside an instance pick the instance, as in Godot
            it.type = n.type;
            it.xf = xf;
            it.z = z;
            it.order = order++;
            items.push_back(std::move(it));
        };
        const fs::path project = s.project;
        if (n.type == "Sprite2D" || n.type == "TextureRect" || n.type == "PointLight2D") {
            if (const auto* tv = n.prop("texture")) {
                const ExtResource* e = s.extById(refId(*tv, "ExtResource"));
                if (auto img = e ? loadTexture(e->file) : nullptr; img && img->width() > 0) {
                    Item it;
                    it.kind = Item::Sprite;
                    it.image = img;
                    const int hf = std::max(1, static_cast<int>(numOf(n, "hframes", 1))), vf = std::max(1, static_cast<int>(numOf(n, "vframes", 1)));
                    const int fr = static_cast<int>(numOf(n, "frame", 0));
                    it.sw = static_cast<double>(img->width()) / hf;
                    it.sh = static_cast<double>(img->height()) / vf;
                    it.sx0 = (fr % hf) * it.sw;
                    it.sy0 = ((fr / hf) % vf) * it.sh;
                    if (boolOf(n, "region_enabled", false) && n.prop("region_rect")) {
                        const auto r = numbers(*n.prop("region_rect"));
                        if (r.size() >= 4) it.sx0 = r[0], it.sy0 = r[1], it.sw = r[2], it.sh = r[3];
                    }
                    double ox = 0, oy = 0;
                    if (const auto* ov = n.prop("offset")) {
                        const auto k = numbers(*ov);
                        if (k.size() >= 2) ox = k[0], oy = k[1];
                    }
                    const double ts = n.type == "PointLight2D" ? numOf(n, "texture_scale", 1) : 1;
                    if (n.type == "TextureRect") {
                        const auto sz = n.prop("size") ? numbers(*n.prop("size")) : std::vector<double>{};
                        it.lx = 0, it.ly = 0;
                        it.lw = sz.size() >= 2 && sz[0] > 0 ? sz[0] : it.sw;
                        it.lh = sz.size() >= 2 && sz[1] > 0 ? sz[1] : it.sh;
                    } else {
                        const bool centered = boolOf(n, "centered", true) || n.type == "PointLight2D";
                        it.lw = it.sw * ts, it.lh = it.sh * ts;
                        it.lx = (centered ? -it.lw / 2 : 0) + ox;
                        it.ly = (centered ? -it.lh / 2 : 0) + oy;
                    }
                    it.flipH = boolOf(n, "flip_h", false);
                    it.flipV = boolOf(n, "flip_v", false);
                    it.color = n.type == "PointLight2D" ? mulColor(self, colorOf(n.prop("color"), Color{255, 255, 255, 255})) : self;
                    if (n.type == "PointLight2D") it.color.a = static_cast<std::uint8_t>(it.color.a / 2);
                    push(it);
                }
            }
        } else if (n.type == "Polygon2D" || n.type == "Line2D") {
            const auto v = n.prop(n.type == "Polygon2D" ? "polygon" : "points") ? numbers(*n.prop(n.type == "Polygon2D" ? "polygon" : "points")) : std::vector<double>{};
            Item it;
            it.kind = n.type == "Polygon2D" ? Item::Poly : Item::Line;
            for (std::size_t i = 0; i + 1 < v.size(); i += 2) it.pts.push_back({v[i], v[i + 1]});
            it.color = mulColor(self, colorOf(n.prop(n.type == "Polygon2D" ? "color" : "default_color"), Color{255, 255, 255, 255}));
            it.width = numOf(n, "width", 10);
            if (it.pts.size() >= 2) push(it);
        } else if (n.type == "ColorRect" || n.type == "Label" || n.type == "Button" || n.type == "ProgressBar" || n.type == "Control" || n.type == "Panel") {
            const auto sz = n.prop("size") ? numbers(*n.prop("size")) : std::vector<double>{};
            Item it;
            it.kind = n.type == "ColorRect" ? Item::Poly : Item::Text;
            const double w = sz.size() >= 2 && sz[0] > 0 ? sz[0] : 40, h = sz.size() >= 2 && sz[1] > 0 ? sz[1] : 23;
            it.pts = {{0, 0}, {w, 0}, {w, h}, {0, h}};
            it.lw = w, it.lh = h;
            it.color = n.type == "ColorRect" ? mulColor(self, colorOf(n.prop("color"), Color{255, 255, 255, 255})) : self;
            if (n.prop("text")) it.text = unquote(*n.prop("text")).value_or("");
            if (n.type == "ProgressBar") it.text = gdNum(numOf(n, "value", 0)) + " / " + gdNum(numOf(n, "max_value", 100));
            push(it);
        } else if (n.type == "TileMapLayer" || n.type == "TileMap") {
            // levels keeps the map file it came from, so the tiles draw without decoding Godot's tile data
            if (const auto* src = n.prop("metadata/wetwaresuite_source")) {
                if (auto img = loadLevel(fromRes(unquote(*src).value_or(""), project))) {
                    Item it;
                    it.kind = Item::Sprite;
                    it.image = img;
                    it.sw = it.lw = img->width();
                    it.sh = it.lh = img->height();
                    it.color = self;
                    push(it);
                }
            } else {
                Item it;  // tiles we can't draw: a marker so the node can still be picked
                it.kind = Item::Marker;
                it.helper = true;
                it.width = 12;
                it.color = {140, 140, 140, 200};
                push(it);
            }
        } else if (n.type == "CollisionShape2D" || n.type == "CollisionPolygon2D") {
            const std::size_t at = items.size();
            shapeItems(s, n, path, xf);
            for (std::size_t i = at; i < items.size(); ++i) items[i].z = 1000000, items[i].order = order++;
        } else if (n.type == "Camera2D") {
            Item it;
            it.kind = Item::Camera;
            it.helper = true;
            auto zoom = n.prop("zoom") ? numbers(*n.prop("zoom")) : std::vector<double>{1, 1};
            if (zoom.size() < 2) zoom = {1, 1};
            double vw = 1152, vh = 648;  // Godot's default window
            if (top && !top->project.empty()) {
                std::ifstream pg(top->project / "project.godot");
                std::string line;
                while (std::getline(pg, line)) {
                    if (line.rfind("window/size/viewport_width", 0) == 0) vw = numbers(line).empty() ? vw : numbers(line).back();
                    if (line.rfind("window/size/viewport_height", 0) == 0) vh = numbers(line).empty() ? vh : numbers(line).back();
                }
            }
            const double w = vw / std::max(0.01, zoom[0]), h = vh / std::max(0.01, zoom[1]);
            const bool centre = !n.prop("anchor_mode") || *n.prop("anchor_mode") != "0";
            const double x0 = centre ? -w / 2 : 0, y0 = centre ? -h / 2 : 0;
            it.pts = {{x0, y0}, {x0 + w, y0}, {x0 + w, y0 + h}, {x0, y0 + h}};
            it.color = {200, 110, 255, 230};
            push(it);
            items.back().z = 1000001;
        } else if (n.type == "Marker2D" || n.type == "AudioStreamPlayer2D" || n.type == "GPUParticles2D" || inherits(n.type, "CollisionObject2D") ||
                   n.type == "Node2D") {
            Item it;
            it.kind = Item::Marker;
            it.helper = true;
            it.width = n.type == "Marker2D" ? numOf(n, "gizmo_extents", 10) : 6;
            it.color = n.type == "Marker2D" ? Color{230, 80, 80, 230} : Color{120, 160, 220, 160};
            push(it);
            items.back().z = 1000000;
        }
        // an instanced scene draws its own tree here
        if (!n.instance.empty() && depth < 6) {
            if (const ExtResource* e = s.extById(n.instance)) {
                if (auto inst = loadScene(e->file)) {
                    const bool outer = instanceRoot.empty();
                    if (outer) instanceRoot = path;
                    walk(*inst, inst->root, path, parentXf, parentMod, depth + 1, &n);
                    if (outer) instanceRoot.clear();
                }
            }
        }
        for (const auto& c : n.children) walk(s, c, path == "." ? c.name : path + "/" + c.name, xf, mod, depth, nullptr);
        (void)override_;
    }
};

std::array<double, 4> itemBox(const Item& it) {
    std::vector<std::array<double, 2>> local = it.pts;
    if (it.kind == Item::Sprite || it.kind == Item::Text) local = {{it.lx, it.ly}, {it.lx + it.lw, it.ly}, {it.lx + it.lw, it.ly + it.lh}, {it.lx, it.ly + it.lh}};
    if (it.kind == Item::Marker) local = {{-it.width, -it.width}, {it.width, it.width}};
    if (local.empty()) local = {{0, 0}};
    double x0 = 1e18, y0 = 1e18, x1 = -1e18, y1 = -1e18;
    for (const auto& p : local) {
        const auto w = it.xf.apply(p[0], p[1]);
        x0 = std::min(x0, w[0]), y0 = std::min(y0, w[1]), x1 = std::max(x1, w[0]), y1 = std::max(y1, w[1]);
    }
    if (it.kind == Item::Line) {
        const double h = it.width / 2;
        x0 -= h, y0 -= h, x1 += h, y1 += h;
    }
    return {x0, y0, x1, y1};
}


}  // namespace

Image render2D(const Scene& s, const RenderOptions2D& options, View2D* viewOut, std::vector<Hit>* hits) {
    Walker w;
    w.top = &s;
    w.walk(s, s.root, ".", Xform2{}, Color{255, 255, 255, 255}, 0, nullptr);
    std::stable_sort(w.items.begin(), w.items.end(), [](const Item& a, const Item& b) { return a.z != b.z ? a.z < b.z : a.order < b.order; });
    // the view: fitted to everything drawn (helpers too), or given
    View2D v;
    if (options.view) {
        v = *options.view;
    } else {
        double x0 = 1e18, y0 = 1e18, x1 = -1e18, y1 = -1e18;
        for (const auto& it : w.items) {
            if (it.kind == Item::Camera) continue;  // a camera's frame is usually far bigger than a sketch
            const auto b = itemBox(it);
            x0 = std::min(x0, b[0]), y0 = std::min(y0, b[1]), x1 = std::max(x1, b[2]), y1 = std::max(y1, b[3]);
        }
        if (x0 > x1) x0 = -100, y0 = -100, x1 = 100, y1 = 100;
        const double pad = std::max(16.0, 0.06 * std::max(x1 - x0, y1 - y0));
        x0 -= pad, y0 -= pad, x1 += pad, y1 += pad;
        const double span = std::max(x1 - x0, y1 - y0);
        v.scale = std::min(4.0, options.maxSize / std::max(1.0, span));
        v.width = std::max(16, static_cast<int>(std::ceil((x1 - x0) * v.scale)));
        v.height = std::max(16, static_cast<int>(std::ceil((y1 - y0) * v.scale)));
        v.originX = x0, v.originY = y0;
    }
    Image img(v.width, v.height, Color{77, 77, 82, 255});
    auto toScreen = [&](double x, double y) { return std::array<double, 2>{(x - v.originX) * v.scale, (y - v.originY) * v.scale}; };
    // world axes, like Godot's red and green lines through the origin
    {
        const auto o = toScreen(0, 0);
        if (o[1] >= 0 && o[1] < v.height) raster::drawLine(img, 0, o[1], v.width, o[1], Color{200, 70, 70, 140}, 1);
        if (o[0] >= 0 && o[0] < v.width) raster::drawLine(img, o[0], 0, o[0], v.height, Color{70, 180, 70, 140}, 1);
    }
    for (const auto& it : w.items) {
        if (it.helper && !options.helpers) continue;
        const Xform2 screen{v.scale, 0, 0, v.scale, -v.originX * v.scale, -v.originY * v.scale};
        const Xform2 m = screen * it.xf;
        auto sp = [&](const std::array<double, 2>& p) {
            const auto q = m.apply(p[0], p[1]);
            return raster::Point{q[0], q[1]};
        };
        switch (it.kind) {
            case Item::Sprite: {
                // map output pixels back into the source rect
                double fx = it.flipH ? -1 : 1, fy = it.flipV ? -1 : 1;
                // local (lx..lx+lw) <- source (sx0..sx0+sw)
                const double kx = it.lw / std::max(1e-9, it.sw), ky = it.lh / std::max(1e-9, it.sh);
                Xform2 srcToLocal{kx * fx, 0, 0, ky * fy, it.flipH ? it.lx + it.lw + it.sx0 * kx : it.lx - it.sx0 * kx,
                                  it.flipV ? it.ly + it.lh + it.sy0 * ky : it.ly - it.sy0 * ky};
                const Xform2 full = m * srcToLocal;
                const auto b = itemBox(it);
                const auto p0 = toScreen(b[0], b[1]), p1 = toScreen(b[2], b[3]);
                const int bx = std::max(0, static_cast<int>(std::floor(std::min(p0[0], p1[0])))), by = std::max(0, static_cast<int>(std::floor(std::min(p0[1], p1[1]))));
                const int ex = std::min(v.width, static_cast<int>(std::ceil(std::max(p0[0], p1[0])))), ey = std::min(v.height, static_cast<int>(std::ceil(std::max(p0[1], p1[1]))));
                if (ex <= bx || ey <= by) break;
                raster::Matrix3 fwd{full.a, full.c, full.e - bx, full.b, full.d, full.f - by, 0, 0, 1}, inv{};
                if (!raster::invert(fwd, inv)) break;
                Image src = raster::crop(*it.image, static_cast<int>(it.sx0), static_cast<int>(it.sy0), static_cast<int>(std::ceil(it.sw)), static_cast<int>(std::ceil(it.sh)));
                // the crop starts at the source rect: shift the map to match
                raster::Matrix3 shift{1, 0, -std::floor(it.sx0), 0, 1, -std::floor(it.sy0), 0, 0, 1};
                Image piece = raster::warp(src, raster::multiply(shift, inv), ex - bx, ey - by, v.scale < 2);
                if (it.color.r != 255 || it.color.g != 255 || it.color.b != 255 || it.color.a != 255) {
                    for (int y = 0; y < piece.height(); ++y)
                        for (int x = 0; x < piece.width(); ++x) piece.set(x, y, mulColor(piece.at(x, y), it.color));
                }
                raster::blendOnto(img, piece, bx, by, raster::Blend::Normal, 1.0);
                break;
            }
            case Item::Poly: {
                std::vector<raster::Point> pts;
                for (const auto& p : it.pts) pts.push_back(sp(p));
                raster::fillPolygon(img, pts, it.color);
                break;
            }
            case Item::Line: {
                for (std::size_t i = 1; i < it.pts.size(); ++i) {
                    const auto a = sp(it.pts[i - 1]), b = sp(it.pts[i]);
                    raster::drawLine(img, a.x, a.y, b.x, b.y, it.color, std::max(1.0, it.width * v.scale * (it.helper ? 0.5 : 1)));
                }
                break;
            }
            case Item::Shape: {
                std::vector<raster::Point> pts;
                for (const auto& p : it.pts) pts.push_back(sp(p));
                raster::fillPolygon(img, pts, it.color);
                Color edge = it.color;
                edge.a = 230;
                for (std::size_t i = 0; i < pts.size(); ++i) {
                    const auto& a = pts[i];
                    const auto& b = pts[(i + 1) % pts.size()];
                    raster::drawLine(img, a.x, a.y, b.x, b.y, edge, 1.5);
                }
                break;
            }
            case Item::Camera: {
                for (std::size_t i = 0; i < it.pts.size(); ++i) {
                    const auto a = sp(it.pts[i]), b = sp(it.pts[(i + 1) % it.pts.size()]);
                    raster::drawLine(img, a.x, a.y, b.x, b.y, it.color, 2);
                }
                break;
            }
            case Item::Marker: {
                const auto c = sp({0, 0});
                const double r = std::max(4.0, it.width * std::min(1.0, v.scale));
                raster::drawLine(img, c.x - r, c.y, c.x + r, c.y, it.color, 1.5);
                raster::drawLine(img, c.x, c.y - r, c.x, c.y + r, it.color, 1.5);
                break;
            }
            case Item::Text: {
                std::vector<raster::Point> pts;
                for (const auto& p : it.pts) pts.push_back(sp(p));
                const bool button = it.type == "Button" || it.type == "ProgressBar";
                if (button) raster::fillPolygon(img, pts, Color{58, 64, 76, 230});
                for (std::size_t i = 0; i < pts.size(); ++i) {
                    const auto& a = pts[i];
                    const auto& b = pts[(i + 1) % pts.size()];
                    raster::drawLine(img, a.x, a.y, b.x, b.y, Color{120, 140, 170, button ? std::uint8_t{220} : std::uint8_t{90}}, 1);
                }
                if (!it.text.empty()) {
                    const double size = std::max(6.0, 16 * v.scale * std::sqrt(std::abs(it.xf.a * it.xf.d - it.xf.b * it.xf.c)));
                    const auto at = sp({button ? 6.0 : 2.0, std::min(it.lh, 23.0) * 0.72});
                    std::string line = it.text.substr(0, it.text.find('\n'));
                    pdf::drawText(img, at.x, at.y, line, pdf::Font{}, size, it.color);
                }
                break;
            }
            default: break;
        }
    }
    // the selection: an orange box like Godot's
    std::map<std::string, std::array<double, 4>> boxes;
    for (const auto& it : w.items) {
        if (it.kind == Item::Camera && !options.selected.empty() && it.path != options.selected) continue;
        const auto b = itemBox(it);
        auto [pos, fresh] = boxes.try_emplace(it.path, b);
        if (!fresh) {
            auto& e = pos->second;
            e = {std::min(e[0], b[0]), std::min(e[1], b[1]), std::max(e[2], b[2]), std::max(e[3], b[3])};
        }
        if (hits) hits->push_back({it.path, it.type, b[0], b[1], b[2], b[3]});
    }
    if (!options.selected.empty()) {
        auto sel = boxes.find(options.selected);
        if (sel == boxes.end() && options.selected == ".") sel = boxes.find(".");
        if (sel != boxes.end()) {
            const auto& b = sel->second;
            const auto p0 = toScreen(b[0], b[1]), p1 = toScreen(b[2], b[3]);
            raster::strokeRect(img, p0[0] - 1, p0[1] - 1, p1[0] - p0[0] + 2, p1[1] - p0[1] + 2, Color{255, 160, 40, 255}, 2);
        }
    }
    if (viewOut) *viewOut = v;
    return img;
}

// ---- 3D -----------------------------------------------------------------------------------

namespace {

// Godot's Y-up space into the renderer's Z-up one: (x, y, z) -> (x, -z, y)
mesh::Vec3 toZup(const std::array<double, 3>& p) { return {p[0], -p[2], p[1]}; }

void appendMesh(mesh::Mesh& out, std::vector<Color>& colors, const mesh::Mesh& m, const Xform3& xf, Color c, bool zUpSource) {
    const auto base = static_cast<std::uint32_t>(out.positions.size());
    for (const auto& p : m.positions) {
        // primitives from the mesh library are Z-up already: bring them to Godot's Y-up first
        const std::array<double, 3> g = zUpSource ? std::array<double, 3>{p.x, p.z, -p.y} : std::array<double, 3>{p.x, p.y, p.z};
        out.positions.push_back(toZup(xf.apply(g)));
    }
    for (auto i : m.indices) out.indices.push_back(base + i);
    for (std::size_t t = 0; t < m.triangleCount(); ++t) colors.push_back(c);
}

std::shared_ptr<const mesh::Mesh> loadMesh(const fs::path& file) {
    static std::map<std::string, std::pair<fs::file_time_type, std::shared_ptr<const mesh::Mesh>>> cache;
    std::error_code ec;
    const auto stamp = fs::last_write_time(file, ec);
    if (ec) return nullptr;
    auto& slot = cache[file.string()];
    if (slot.second && slot.first == stamp) return slot.second;
    try {
        slot = {stamp, std::make_shared<const mesh::Mesh>(mesh::load(file))};
    } catch (const std::exception&) {
        slot = {stamp, nullptr};
    }
    return slot.second;
}

struct Walker3 {
    mesh::Mesh all;
    std::vector<Color> colors;
    std::vector<mesh::Line3> lines;
    std::optional<Xform3> camera;
    double fov = 75;
    std::string selected;
    bool hideCameras = false;  // looking through one: its own frame would cover the view

    void box(const Xform3& xf, double hx, double hy, double hz, Color c) {
        const std::array<std::array<double, 3>, 8> k{{{-hx, -hy, -hz}, {hx, -hy, -hz}, {hx, hy, -hz}, {-hx, hy, -hz}, {-hx, -hy, hz}, {hx, -hy, hz}, {hx, hy, hz}, {-hx, hy, hz}}};
        const int e[12][2] = {{0, 1}, {1, 2}, {2, 3}, {3, 0}, {4, 5}, {5, 6}, {6, 7}, {7, 4}, {0, 4}, {1, 5}, {2, 6}, {3, 7}};
        for (const auto& ed : e) lines.push_back({{toZup(xf.apply(k[static_cast<std::size_t>(ed[0])])), toZup(xf.apply(k[static_cast<std::size_t>(ed[1])]))}, c, false});
    }
    void ring(const Xform3& xf, double r, double y, Color c) {
        mesh::Line3 l;
        l.color = c;
        l.closed = true;
        for (int i = 0; i < 32; ++i) l.points.push_back(toZup(xf.apply({r * std::cos(2 * kPi * i / 32), y, r * std::sin(2 * kPi * i / 32)})));
        lines.push_back(l);
    }

    void walk(const Scene& s, const Node& nIn, const std::string& path, const Xform3& parent, int depth, const Node* override_) {
        Node n = nIn;
        if (override_) {
            for (const auto& [k, v] : override_->props) n.set(k, v);
        }
        if (n.prop("visible") && *n.prop("visible") == "false") return;
        Xform3 xf = parent;
        if (inherits(n.type, "Node3D") || n.type.empty()) xf = parent * localXform3(n);
        const bool sel = !selected.empty() && path == selected;
        if (n.type == "MeshInstance3D") {
            Color c{200, 200, 205, 255};
            if (const auto* mo = n.prop("material_override")) {
                if (const SubResource* mat = s.subById(refId(*mo, "SubResource"))) c = colorOf(mat->prop("albedo_color"), c);
            }
            if (sel) c = {255, 170, 70, 255};
            if (const auto* mv = n.prop("mesh")) {
                if (const SubResource* r = s.subById(refId(*mv, "SubResource"))) {
                    auto nums = [&](const char* k) { return r->prop(k) ? numbers(*r->prop(k)) : std::vector<double>{}; };
                    if (const auto* mat = r->prop("material")) {
                        if (const SubResource* m2 = s.subById(refId(*mat, "SubResource"))) c = sel ? c : colorOf(m2->prop("albedo_color"), c);
                    }
                    mesh::Mesh m;
                    if (r->type == "BoxMesh") {
                        auto sz = nums("size");
                        if (sz.size() < 3) sz = {1, 1, 1};
                        m = mesh::box({sz[0], sz[2], sz[1]});
                    } else if (r->type == "SphereMesh") {
                        m = mesh::sphere(nums("radius").empty() ? 0.5 : nums("radius")[0], 24, 12);
                    } else if (r->type == "CylinderMesh" || r->type == "CapsuleMesh") {
                        const double rad = r->type == "CylinderMesh" ? (nums("top_radius").empty() ? 0.5 : nums("top_radius")[0]) : (nums("radius").empty() ? 0.5 : nums("radius")[0]);
                        m = mesh::cylinder(rad, nums("height").empty() ? 2 : nums("height")[0], 24);
                    } else if (r->type == "PlaneMesh") {
                        auto sz = nums("size");
                        if (sz.size() < 2) sz = {2, 2};
                        m = mesh::plane(sz[0], sz[1]);
                    }
                    if (!m.empty()) appendMesh(all, colors, m, xf, c, true);
                } else if (const ExtResource* e = s.extById(refId(*mv, "ExtResource"))) {
                    if (auto m = loadMesh(e->file)) appendMesh(all, colors, *m, xf, c, false);
                }
            }
        } else if (n.type == "CollisionShape3D") {
            const Color c = sel ? Color{255, 160, 40, 255} : Color{0, 160, 190, 220};
            if (const auto* sv = n.prop("shape")) {
                if (const SubResource* r = s.subById(refId(*sv, "SubResource"))) {
                    auto nums = [&](const char* k) { return r->prop(k) ? numbers(*r->prop(k)) : std::vector<double>{}; };
                    if (r->type == "BoxShape3D") {
                        auto sz = nums("size");
                        if (sz.size() < 3) sz = {1, 1, 1};
                        box(xf, sz[0] / 2, sz[1] / 2, sz[2] / 2, c);
                    } else {
                        const double rad = nums("radius").empty() ? 0.5 : nums("radius")[0];
                        const double h = nums("height").empty() ? 2 * rad : nums("height")[0];
                        ring(xf, rad, -h / 2 + (r->type == "SphereShape3D" ? rad : 0), c);
                        ring(xf, rad, h / 2 - (r->type == "SphereShape3D" ? rad : 0), c);
                        ring(xf, rad, 0, c);
                    }
                }
            }
        } else if (n.type == "Camera3D") {
            if (!camera || (n.prop("current") && *n.prop("current") == "true")) {
                camera = xf;
                fov = numOf(n, "fov", 75);
            }
            const Color c = sel ? Color{255, 160, 40, 255} : Color{200, 110, 255, 230};
            const double d = 0.6, w = 0.35, h = 0.22;
            const std::array<double, 3> o{0, 0, 0};
            const std::array<std::array<double, 3>, 4> k{{{-w, -h, -d}, {w, -h, -d}, {w, h, -d}, {-w, h, -d}}};
            for (int i = 0; i < 4 && !hideCameras; ++i) {
                lines.push_back({{toZup(xf.apply(o)), toZup(xf.apply(k[static_cast<std::size_t>(i)]))}, c, false});
                lines.push_back({{toZup(xf.apply(k[static_cast<std::size_t>(i)])), toZup(xf.apply(k[static_cast<std::size_t>((i + 1) % 4)]))}, c, false});
            }
        } else if (inherits(n.type, "Light3D")) {
            const Color c = sel ? Color{255, 160, 40, 255} : Color{255, 220, 90, 230};
            ring(xf, 0.25, 0, c);
            if (n.type == "DirectionalLight3D" || n.type == "SpotLight3D") lines.push_back({{toZup(xf.apply({0, 0, 0})), toZup(xf.apply({0, 0, -1.2}))}, c, false});
        } else if (n.type == "Marker3D" || inherits(n.type, "CollisionObject3D") || n.type == "VehicleWheel3D") {
            const Color c = sel ? Color{255, 160, 40, 255} : Color{230, 80, 80, 200};
            const double r = n.type == "VehicleWheel3D" ? numOf(n, "wheel_radius", 0.5) : 0.25;
            if (n.type == "VehicleWheel3D") {
                mesh::Line3 l;
                l.color = c;
                l.closed = true;
                for (int i = 0; i < 24; ++i) l.points.push_back(toZup(xf.apply({0, r * std::cos(2 * kPi * i / 24), r * std::sin(2 * kPi * i / 24)})));
                lines.push_back(l);
            } else {
                for (int ax = 0; ax < 3; ++ax) {
                    std::array<double, 3> a{0, 0, 0}, b{0, 0, 0};
                    a[static_cast<std::size_t>(ax)] = -r;
                    b[static_cast<std::size_t>(ax)] = r;
                    lines.push_back({{toZup(xf.apply(a)), toZup(xf.apply(b))}, c, false});
                }
            }
        }
        if (!n.instance.empty() && depth < 6) {
            if (const ExtResource* e = s.extById(n.instance)) {
                const std::string ext = e->file.extension().string();
                if (ext == ".tscn") {
                    if (auto inst = loadScene(e->file)) walk(*inst, inst->root, path, parent, depth + 1, &n);
                } else if (auto m = loadMesh(e->file)) {  // a .glb/.gltf/.obj scene
                    appendMesh(all, colors, *m, xf, sel ? Color{255, 170, 70, 255} : Color{200, 200, 205, 255}, false);
                }
            }
        }
        for (const auto& c : n.children) walk(s, c, path == "." ? c.name : path + "/" + c.name, xf, depth, nullptr);
    }
};

}  // namespace

Image render3D(const Scene& s, int width, int height, double angle, double tilt, bool fromCamera, const std::string& selected) {
    Walker3 w;
    w.selected = selected;
    w.hideCameras = fromCamera;
    w.walk(s, s.root, ".", Xform3{}, 0, nullptr);
    // a ground grid around the origin, like Godot's
    for (int i = -5; i <= 5; ++i) {
        const Color c = i == 0 ? Color{200, 80, 80, 120} : Color{150, 150, 155, 70};
        w.lines.push_back({{toZup({-5.0, 0, static_cast<double>(i)}), toZup({5.0, 0, static_cast<double>(i)})}, i == 0 ? Color{200, 80, 80, 120} : c, false});
        w.lines.push_back({{toZup({static_cast<double>(i), 0, -5.0}), toZup({static_cast<double>(i), 0, 5.0})}, i == 0 ? Color{80, 80, 200, 120} : c, false});
    }
    mesh::RenderOptions o;
    o.background = {77, 77, 82, 255};
    o.lines = w.lines;
    if (w.colors.size() == w.all.triangleCount()) o.faceColors = w.colors;
    if (fromCamera && w.camera) {
        // look along the camera's -z, orthographic (the renderer has no perspective)
        const auto& b = w.camera->basis;
        const mesh::Vec3 fwd = toZup({-b[2], -b[5], -b[8]}), up = toZup({b[1], b[4], b[7]}), right = toZup({b[0], b[3], b[6]});
        mesh::Projection pr;
        pr.right = right;
        pr.up = up;
        pr.forward = fwd;
        const mesh::Vec3 eye = toZup(w.camera->origin);
        // frame what is 10 m ahead at the camera's field of view
        const double half = 10 * std::tan(w.fov * kPi / 360);
        pr.scale = height / (2 * std::max(0.1, half));
        pr.cx = mesh::dot(eye + fwd * 10.0, right);
        pr.cy = mesh::dot(eye + fwd * 10.0, up);
        o.projection = pr;
    } else {
        mesh::Orbit orbit;
        orbit.yaw = angle;
        orbit.pitch = tilt;
        o.orbit = orbit;
    }
    return mesh::render(w.all, width, height, o);
}

int dimsOf(const Scene& s) {
    int two = 0, three = 0;
    std::function<void(const Node&)> walk = [&](const Node& n) {
        if (const ClassInfo* c = classInfo(n.type)) {
            if (c->dims == 2) ++two;
            if (c->dims == 3) ++three;
        }
        for (const auto& c : n.children) walk(c);
    };
    walk(s.root);
    return three > two ? 3 : 2;
}

std::unique_ptr<Attachment> SceneDoc::preview() const {
    try {
        Image img = dimsOf(scene) == 3 ? render3D(scene, 900, 600, 35, 25, false, "") : render2D(scene, {}, nullptr, nullptr);
        return std::make_unique<Attachment>(Attachment{"image/png", raster::encodePng(img), "scene.png"});
    } catch (const std::exception&) {
        return nullptr;
    }
}

}  // namespace wetwaresuite::scene
