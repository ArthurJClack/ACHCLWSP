// A 3D-ish view of the assembled board: the board as a slab with its solder mask, pads, silkscreen
// and holes, and every part as a box (or cylinder) of a typical height for its package, drawn with
// flat shading and the painter's algorithm. It is a preview to check placement, not a model.
#include <algorithm>
#include <cctype>
#include <cmath>
#include <map>

#include "wetwaresuite/circuit/circuit.hpp"
#include "wetwaresuite/mesh/io.hpp"
#include "wetwaresuite/mesh/primitives.hpp"
#include "wetwaresuite/raster/paint.hpp"
#include "geom.hpp"

namespace wetwaresuite::circuit {

using raster::Color;

namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr double kThick = 1.6;

struct V3 {
    double x = 0, y = 0, z = 0;
};

V3 operator-(V3 a, V3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
double dot(V3 a, V3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
V3 unit(V3 a) {
    const double l = std::sqrt(dot(a, a));
    return l > 1e-12 ? V3{a.x / l, a.y / l, a.z / l} : V3{0, 0, 1};
}

Color rgb(unsigned v) {
    return Color{static_cast<std::uint8_t>(v >> 16), static_cast<std::uint8_t>((v >> 8) & 0xff), static_cast<std::uint8_t>(v & 0xff), 255};
}

std::string lower(std::string s) {
    for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

struct Face {
    std::vector<V3> pts;
    V3 normal;
    Color color;
    int group = 0;  // faces of one solid are sorted together
    double depth = 0;
    bool flat = false;  // lies on the board: drawn before any solid, in order
};

// What a part looks like: a box of the footprint's body size and a height and colour by package.
struct Body {
    enum Kind { Box, Cylinder, Board } kind = Box;
    double x0 = 0, y0 = 0, x1 = 0, y1 = 0;  // footprint space
    double z0 = 0, z1 = 1;                   // above the board surface
    Color color;
};

std::vector<Body> bodiesFor(const Part& part, const Footprint& fp) {
    const std::string n = lower(fp.name);
    const std::string sym = lower(part.symbol);
    auto starts = [&](const char* p) { return n.rfind(p, 0) == 0; };
    // the body: courtyard without its margin, or the pads' extent for tiny SMD parts
    double x0 = fp.x0 + 0.25, y0 = fp.y0 + 0.25, x1 = fp.x1 - 0.25, y1 = fp.y1 - 0.25;
    double px0 = 1e9, py0 = 1e9, px1 = -1e9, py1 = -1e9;
    for (const auto& p : fp.pads) {
        px0 = std::min(px0, p.pos.x - p.w / 2), px1 = std::max(px1, p.pos.x + p.w / 2);
        py0 = std::min(py0, p.pos.y - p.h / 2), py1 = std::max(py1, p.pos.y + p.h / 2);
    }
    std::vector<Body> out;
    auto box = [&](double a, double b, double c, double d, double z0, double z1, unsigned col) {
        out.push_back({Body::Box, a, b, c, d, z0, z1, rgb(col)});
    };
    auto cyl = [&](double cx, double cy, double r, double z0, double z1, unsigned col) {
        out.push_back({Body::Cylinder, cx - r, cy - r, cx + r, cy + r, z0, z1, rgb(col)});
    };
    if (fp.pads.empty() || starts("mountinghole") || starts("testpoint")) return out;
    const bool tiny = starts("r_0") || starts("c_0") || starts("l_0") || starts("led_0") || starts("d_0") || starts("fuse_");
    if (tiny) {
        // chip parts: the body sits between and over the inner halves of the pads
        const double len = px1 - px0, wid = py1 - py0;
        const double h = len < 1.5 ? 0.35 : len < 2.5 ? 0.5 : len < 3.5 ? 0.6 : 0.7;
        unsigned col = 0x262626;
        if (starts("c_")) col = 0xb89a6a;
        if (starts("l_")) col = 0x4a4a4a;
        if (starts("led_")) {
            const std::string v = lower(part.value);
            col = v.find("green") != std::string::npos ? 0x40e060 : v.find("blue") != std::string::npos ? 0x4080ff
                  : v.find("yellow") != std::string::npos ? 0xf0e040 : v.find("white") != std::string::npos ? 0xf4f4f4 : 0xf04030;
        }
        box(px0 + len * 0.12, py0 + wid * 0.12, px1 - len * 0.12, py1 - wid * 0.12, 0, h, col);
        // tinned ends
        box(px0 + len * 0.1, py0 + wid * 0.1, px0 + len * 0.25, py1 - wid * 0.1, 0, h + 0.01, 0xc8c8c8);
        box(px1 - len * 0.25, py0 + wid * 0.1, px1 - len * 0.1, py1 - wid * 0.1, 0, h + 0.01, 0xc8c8c8);
        return out;
    }
    if (starts("pinheader") || starts("pinsocket")) {
        box(x0 + 0.1, y0 + 0.1, x1 - 0.1, y1 - 0.1, 0, 2.5, 0x202020);
        for (const auto& p : fp.pads) box(p.pos.x - 0.32, p.pos.y - 0.32, p.pos.x + 0.32, p.pos.y + 0.32, 2.5, 8.5, 0xd8b040);
        return out;
    }
    if (starts("terminalblock")) {
        box(x0, y0, x1, y1, 0, 10, 0x2a8a3a);
        for (const auto& p : fp.pads) cyl(p.pos.x, p.pos.y - 0.5, 1.4, 10, 10.2, 0xb0b0b0);
        return out;
    }
    if (starts("arduino") || starts("esp32_devkit") || starts("bluepill") || starts("rpi_pico")) {
        // a dev board on headers
        unsigned col = starts("rpi_pico") ? 0x1e7a3a : starts("esp32") ? 0x202020 : 0x1a4a9a;
        box(x0, y0, x1, y1, 8.5, 10.1, col);
        for (const auto& p : fp.pads) box(p.pos.x - 1.25, p.pos.y - 1.25, p.pos.x + 1.25, p.pos.y + 1.25, 0, 8.5, 0x202020);
        const double cx = (x0 + x1) / 2, cy = (y0 + y1) / 2, s = std::min(x1 - x0, y1 - y0) * 0.22;
        box(cx - s, cy - s, cx + s, cy + s, 10.1, 11.2, 0x161616);
        return out;
    }
    if (starts("esp32-wroom")) {
        box(-9, -12.75, 9, 12.75, 0, 0.8, 0x203a7a);
        box(-8.1, -5.7, 8.1, 11.6, 0.8, 3.1, 0xc4c4c4);
        return out;
    }
    if (starts("usb_micro")) {
        box(-3.9, -2.4, 3.9, 2.6, 0, 2.7, 0xc0c0c0);
        return out;
    }
    if (starts("cp_tht")) {
        const double r = std::max(x1 - x0, y1 - y0) / 2 - 0.2;
        cyl((x0 + x1) / 2, (y0 + y1) / 2, r, 0, 7, 0x203a90);
        return out;
    }
    if (starts("buzzer")) {
        cyl(0, 0, 6, 0, 9.5, 0x1a1a1a);
        return out;
    }
    if (starts("led_tht")) {
        cyl(0, 0, 2.5, 0, 8.6, 0xe04030);
        return out;
    }
    if (starts("sw_push")) {
        box(-3, -3, 3, 3, 0, 3.5, 0x202020);
        cyl(0, 0, 1.75, 3.5, 5, 0x606060);
        return out;
    }
    if (starts("hc49")) {
        box(x0 + 0.3, y0 + 0.4, x1 - 0.3, y1 - 0.4, 0, 3.5, 0xc8c8c8);
        return out;
    }
    if (starts("to-220")) {
        box(x0, y0 + 1.4, x1, y1 - 0.4, 0, 4.4, 0x1c1c1c);
        box(x0, y0 + 0.2, x1, y0 + 1.4, 0, 1.3, 0xc0c0c0);
        return out;
    }
    if (starts("to-92")) {
        box(-2.4, -1.0, 2.4, 1.6, 0, 4.8, 0x1c1c1c);
        return out;
    }
    if (starts("r_tht") || starts("l_tht") || starts("d_tht")) {
        const double cy = (y0 + y1) / 2;
        box(x0 + 1.4, cy - 1.1, x1 - 1.4, cy + 1.1, 0.2, 2.4, starts("r_") ? 0xd8c090 : starts("d_") ? 0x202020 : 0x406040);
        return out;
    }
    double h = 1.2;
    unsigned col = 0x1c1c1c;
    if (starts("dip-")) h = 3.5;
    else if (starts("soic") || starts("tssop")) h = 1.5;
    else if (starts("qfn")) h = 0.85;
    else if (starts("tqfp")) h = 1.2;
    else if (starts("sot-223")) h = 1.7;
    else if (starts("sot-23")) h = 1.1;
    else if (starts("d_")) h = 1.1;
    else if (starts("sw_spdt")) h = 3.5, col = 0x303030;
    else if (starts("pot_tht")) h = 4.5, col = 0x2a50a0;
    else if (starts("c_tht")) h = 5, col = 0xd08a30;
    else if (sym == "crystal") h = 1.0, col = 0xb0b0b0;
    auto innerX = [&] {
        double v = 1e9;
        for (const auto& p : fp.pads) v = std::min(v, std::abs(p.pos.x) - p.w / 2);
        return v;
    };
    auto innerY = [&] {
        double v = 1e9;
        for (const auto& p : fp.pads) v = std::min(v, std::abs(p.pos.y) - p.h / 2);
        return v;
    };
    if (starts("soic") || starts("tssop") || starts("dip-")) {
        // two rows of pads left and right: the body sits between them (a DIP's over them)
        const double half = innerX() + (starts("dip-") ? 0.2 : 0.05);
        x0 = -half, x1 = half, y0 = py0 - 0.2, y1 = py1 + 0.2;
    } else if (starts("sot")) {
        const double half = std::max(0.3, innerY() + 0.1);
        x0 = px0, x1 = px1, y0 = -half, y1 = half;
    } else if (starts("d_")) {
        const double half = std::max(0.3, innerX() + 0.15);
        x0 = -half, x1 = half, y0 = py0 - 0.15, y1 = py1 + 0.15;
    }
    if (starts("qfn") || starts("tqfp")) {
        const double s = starts("qfn") ? (px1 - px0) / 2 - 0.1 : (px1 - px0) / 2 - 1.5;
        x0 = y0 = -s, x1 = y1 = s;
    }
    box(x0, y0, x1, y1, starts("dip-") ? 0.5 : 0, h, col);
    if (starts("dip-") || starts("soic") || starts("qfn") || starts("tqfp") || starts("tssop")) {
        // pin 1 dot
        const double d = std::min(x1 - x0, y1 - y0) * 0.08 + 0.2;
        box(x0 + d * 0.6, y0 + d * 0.6, x0 + d * 1.6, y0 + d * 1.6, h, h + 0.02, 0x8a8a8a);
    }
    return out;
}

class Scene {
public:
    Scene(double yawDeg, double elevDeg, bool below) : below_(below) {
        yaw_ = yawDeg * kPi / 180;
        elev_ = std::clamp(elevDeg, 5.0, 90.0) * kPi / 180;
        light_ = unit(view({-0.35, 0.45, 0.85}));
    }
    // board space (mm, y down, z up from the bottom of the board) -> camera space (x right, y up, z towards the viewer)
    V3 view(V3 p) const {
        double X = p.x, Y = -p.y, Z = p.z;
        if (below_) X = -X, Z = kThick - Z;
        const double xr = X * std::cos(yaw_) - Y * std::sin(yaw_);
        const double yr = X * std::sin(yaw_) + Y * std::cos(yaw_);
        return {xr, yr * std::sin(elev_) + Z * std::cos(elev_), -yr * std::cos(elev_) + Z * std::sin(elev_)};
    }
    V3 viewDir(V3 n) const {
        V3 a = view(n), o = view({0, 0, 0});
        return unit(a - o);
    }
    void add(std::vector<V3> pts, V3 normal, Color col, int group, bool flat = false) {
        Face f;
        f.normal = viewDir(normal);
        if (f.normal.z <= 1e-6) return;  // facing away
        double d = 0;
        for (auto& p : pts) {
            p = view(p);
            d += p.z;
        }
        f.pts = std::move(pts);
        f.depth = d / static_cast<double>(f.pts.size());
        f.color = col;
        f.group = group;
        f.flat = flat;
        faces_.push_back(std::move(f));
    }
    void prism(const std::vector<Pt>& base, double z0, double z1, Color col, int group) {
        if (base.size() < 3) return;
        // orientation of the base (board space, y down)
        double area = 0;
        for (std::size_t i = 0; i < base.size(); ++i) {
            const Pt a = base[i], b = base[(i + 1) % base.size()];
            area += a.x * b.y - b.x * a.y;
        }
        std::vector<V3> top, bottom;
        for (const auto& p : base) top.push_back({p.x, p.y, z1}), bottom.push_back({p.x, p.y, z0});
        add(top, {0, 0, 1}, col, group);
        add(bottom, {0, 0, -1}, col, group);
        for (std::size_t i = 0; i < base.size(); ++i) {
            const Pt a = base[i], b = base[(i + 1) % base.size()];
            // outward normal in board space (y down): for a clockwise-on-screen polygon (area > 0) it's (dy, -dx)
            double nx = b.y - a.y, ny = -(b.x - a.x);
            if (area < 0) nx = -nx, ny = -ny;
            add({{a.x, a.y, z0}, {b.x, b.y, z0}, {b.x, b.y, z1}, {a.x, a.y, z1}}, {nx, ny, 0}, col, group);
        }
    }
    void flat(const std::vector<Pt>& poly, double z, Color col) {
        std::vector<V3> pts;
        for (const auto& p : poly) pts.push_back({p.x, p.y, z});
        add(pts, {0, 0, z >= kThick / 2 ? 1.0 : -1.0}, col, flatOrder_++, true);
    }
    raster::Image render(int width, int height, Color background) {
        raster::Image img(width, height, background);
        if (faces_.empty()) return img;
        double x0 = 1e18, y0 = 1e18, x1 = -1e18, y1 = -1e18;
        for (const auto& f : faces_) {
            for (const auto& p : f.pts) x0 = std::min(x0, p.x), x1 = std::max(x1, p.x), y0 = std::min(y0, p.y), y1 = std::max(y1, p.y);
        }
        const double margin = 0.05;
        const double s = std::min(width * (1 - 2 * margin) / std::max(1e-6, x1 - x0), height * (1 - 2 * margin) / std::max(1e-6, y1 - y0));
        const double ox = (width - (x1 - x0) * s) / 2 - x0 * s, oy = (height - (y1 - y0) * s) / 2 + y1 * s;
        // solids sorted far to near by their mean depth; flat board artwork first, in order
        std::map<int, std::pair<double, int>> groupDepth;  // group -> (sum, count)
        for (const auto& f : faces_) {
            if (f.flat) continue;
            auto& g = groupDepth[f.group];
            g.first += f.depth, g.second += 1;
        }
        std::vector<const Face*> order;
        for (const auto& f : faces_) {
            if (f.group == 0) order.push_back(&f);  // the board slab
        }
        std::vector<const Face*> flats;
        for (const auto& f : faces_) {
            if (f.flat) flats.push_back(&f);
        }
        std::sort(flats.begin(), flats.end(), [](const Face* a, const Face* b) { return a->group < b->group; });
        for (const auto* f : flats) order.push_back(f);
        std::vector<const Face*> solids;
        for (const auto& f : faces_) {
            if (!f.flat && f.group != 0) solids.push_back(&f);
        }
        auto gd = [&](const Face* f) {
            const auto& g = groupDepth[f->group];
            return g.first / std::max(1, g.second);
        };
        std::stable_sort(solids.begin(), solids.end(), [&](const Face* a, const Face* b) {
            const double da = gd(a), db = gd(b);
            if (std::abs(da - db) > 1e-9) return da < db;
            if (a->group != b->group) return a->group < b->group;
            return a->depth < b->depth;
        });
        for (const auto* f : solids) order.push_back(f);
        for (const Face* f : order) {
            const double lam = std::max(0.0, dot(f->normal, light_));
            const double k = 0.42 + 0.58 * lam;
            Color c{static_cast<std::uint8_t>(std::clamp(f->color.r * k, 0.0, 255.0)), static_cast<std::uint8_t>(std::clamp(f->color.g * k, 0.0, 255.0)),
                    static_cast<std::uint8_t>(std::clamp(f->color.b * k, 0.0, 255.0)), 255};
            std::vector<raster::Point> pts;
            for (const auto& p : f->pts) pts.push_back({ox + p.x * s, oy - p.y * s});
            raster::fillPolygon(img, pts, c);
        }
        return img;
    }

private:
    double yaw_ = 0, elev_ = 0;
    bool below_ = false;
    V3 light_;
    std::vector<Face> faces_;
    int flatOrder_ = 1;
};

std::vector<Pt> circlePts(Pt c, double r, int n = 20) {
    std::vector<Pt> out;
    for (int i = 0; i < n; ++i) out.push_back({c.x + r * std::cos(2 * kPi * i / n), c.y + r * std::sin(2 * kPi * i / n)});
    return out;
}

std::vector<Pt> capsulePts(Pt a, Pt b, double r) {
    const double ang = std::atan2(b.y - a.y, b.x - a.x);
    std::vector<Pt> out;
    const int n = 8;
    for (int i = 0; i <= n; ++i) {
        const double t = ang + kPi / 2 + kPi * i / n;
        out.push_back({b.x + r * std::cos(t) * -1, b.y + r * std::sin(t) * -1});
    }
    for (int i = 0; i <= n; ++i) {
        const double t = ang + kPi / 2 + kPi * i / n;
        out.push_back({a.x + r * std::cos(t), a.y + r * std::sin(t)});
    }
    return out;
}

std::vector<Pt> padPts(const WorldPad& p) {
    if (p.angle != 0) return padOutline(p);
    if (p.shape == PadShape::Rect) {
        return {{p.pos.x - p.w / 2, p.pos.y - p.h / 2}, {p.pos.x + p.w / 2, p.pos.y - p.h / 2}, {p.pos.x + p.w / 2, p.pos.y + p.h / 2}, {p.pos.x - p.w / 2, p.pos.y + p.h / 2}};
    }
    const auto s = geom::padShape(p);
    if (std::abs(s.a.x - s.b.x) < 1e-9 && std::abs(s.a.y - s.b.y) < 1e-9) return circlePts(s.a, s.r);
    return capsulePts(s.a, s.b, s.r);
}

}  // namespace

raster::Image renderBoard3d(const Circuit& c, const Netlist& nl, int width, int height, const View3d& v) {
    Scene scene(v.yaw, v.elevation, v.fromBelow);
    const Color mask = rgb(v.maskColor), silk = rgb(0xf0f0f0), finish = rgb(0xd8c070), hole = rgb(0x111111);
    std::vector<Pt> outline = c.board.outline;
    if (outline.size() < 3) {
        double x0, y0, x1, y1;
        boardBounds(c.board, x0, y0, x1, y1);
        outline = {{x0 - 5, y0 - 5}, {x1 + 5, y0 - 5}, {x1 + 5, y1 + 5}, {x0 - 5, y1 + 5}};
    }
    scene.prism(outline, 0, kThick, mask, 0);
    const auto pads = worldPads(c, nl);
    // copper under the mask shows as a lighter green; pads, holes and silkscreen on top
    const bool top = !v.fromBelow;
    const double zs = top ? kThick + 0.002 : -0.002;
    const int side = top ? 0 : 1;
    Color traceCol{static_cast<std::uint8_t>(std::min(255, mask.r + 30)), static_cast<std::uint8_t>(std::min(255, mask.g + 40)),
                   static_cast<std::uint8_t>(std::min(255, mask.b + 25)), 255};
    for (const auto& t : c.board.traces) {
        if (t.layer == side) scene.flat(capsulePts(t.a, t.b, t.width / 2), zs, traceCol);
    }
    for (const auto& p : pads) {
        if (!(p.layers & layerBit(side)) || !p.plated) continue;
        scene.flat(padPts(p), top ? kThick + 0.004 : -0.004, finish);
    }
    for (const auto& vi : c.board.vias) scene.flat(circlePts(vi.pos, vi.diameter / 2, 12), top ? kThick + 0.004 : -0.004, traceCol);
    for (const auto& p : pads) {
        if (p.drill > 0) scene.flat(circlePts(p.pos, p.drill / 2, 14), top ? kThick + 0.006 : -0.006, hole);
    }
    for (const auto& vi : c.board.vias) scene.flat(circlePts(vi.pos, vi.drill / 2, 10), top ? kThick + 0.006 : -0.006, hole);
    int group = 1;
    for (const auto& part : c.parts) {
        if (!part.pcb.placed) continue;
        auto fp = footprintOf(part);
        if (!fp) continue;
        if (part.pcb.bottom == top) {
            // silkscreen of the far side isn't visible; its parts still are, through the board edge
        } else {
            for (const auto& [a, b] : fp->silk) scene.flat(capsulePts(toBoard(part.pcb, a), toBoard(part.pcb, b), 0.08), top ? kThick + 0.008 : -0.008, silk);
        }
        for (const auto& body : bodiesFor(part, *fp)) {
            std::vector<Pt> base;
            if (body.kind == Body::Cylinder) {
                for (const auto& p : circlePts({(body.x0 + body.x1) / 2, (body.y0 + body.y1) / 2}, (body.x1 - body.x0) / 2, 18)) base.push_back(toBoard(part.pcb, p));
            } else {
                for (Pt p : {Pt{body.x0, body.y0}, Pt{body.x1, body.y0}, Pt{body.x1, body.y1}, Pt{body.x0, body.y1}}) base.push_back(toBoard(part.pcb, p));
            }
            const double z0 = part.pcb.bottom ? -body.z1 : kThick + body.z0;
            const double z1 = part.pcb.bottom ? -body.z0 : kThick + body.z1;
            scene.prism(base, z0, z1, body.color, group++);
        }
    }
    return scene.render(width, height, rgb(v.background));
}

// The assembled board as solids for a CAD app: the board (1.6 mm, with its mounting and large
// holes) and every part as the same boxes and cylinders the 3D view draws. Millimetres, y up
// (the board file's y flipped), the top of the board at z = 1.6.
std::string boardModel(const Circuit& c, const Netlist& nl, const std::string& ext) {
    mesh::Mesh all;
    auto add = [&](const std::vector<Pt>& base, std::vector<std::vector<Pt>> holes, double z0, double z1) {
        if (base.size() < 3 || z1 - z0 < 1e-6) return;
        mesh::Polygon2 poly;
        for (const auto& p : base) poly.outer.push_back({p.x, -p.y});
        for (const auto& h : holes) {
            poly.holes.emplace_back();
            for (const auto& p : h) poly.holes.back().push_back({p.x, -p.y});
        }
        const mesh::Mesh m = mesh::extrude(poly, z1 - z0);
        const auto offset = static_cast<std::uint32_t>(all.positions.size());
        for (auto v : m.positions) all.positions.push_back({v.x, v.y, v.z + z0});
        for (auto i : m.indices) all.indices.push_back(i + offset);
    };
    std::vector<Pt> outline = c.board.outline;
    if (outline.size() < 3) {
        double x0, y0, x1, y1;
        boardBounds(c.board, x0, y0, x1, y1);
        outline = {{x0 - 5, y0 - 5}, {x1 + 5, y0 - 5}, {x1 + 5, y1 + 5}, {x0 - 5, y1 + 5}};
    }
    std::vector<std::vector<Pt>> holes;  // mounting holes and anything big a screw or post goes through
    for (const auto& p : worldPads(c, nl)) {
        if (p.drill >= 2.0 || (!p.plated && p.drill > 0)) holes.push_back(circlePts(p.pos, p.drill / 2, 24));
    }
    add(outline, holes, 0, kThick);
    for (const auto& part : c.parts) {
        if (!part.pcb.placed) continue;
        auto fp = footprintOf(part);
        if (!fp) continue;
        for (const auto& body : bodiesFor(part, *fp)) {
            std::vector<Pt> base;
            if (body.kind == Body::Cylinder) {
                for (const auto& p : circlePts({(body.x0 + body.x1) / 2, (body.y0 + body.y1) / 2}, (body.x1 - body.x0) / 2, 24)) base.push_back(toBoard(part.pcb, p));
            } else {
                for (Pt p : {Pt{body.x0, body.y0}, Pt{body.x1, body.y0}, Pt{body.x1, body.y1}, Pt{body.x0, body.y1}}) base.push_back(toBoard(part.pcb, p));
            }
            const double z0 = part.pcb.bottom ? -body.z1 : kThick + body.z0;
            const double z1 = part.pcb.bottom ? -body.z0 : kThick + body.z1;
            add(base, {}, z0, z1);
        }
    }
    std::string e = ext;
    if (!e.empty() && e[0] == '.') e.erase(0, 1);
    if (e == "stp") e = "step";
    return mesh::encode(all, e);
}

}  // namespace wetwaresuite::circuit
