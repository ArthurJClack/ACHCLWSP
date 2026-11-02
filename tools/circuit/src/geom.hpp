// Copper geometry shared by connectivity, DRC, pours, the router and Gerber output.
#pragma once

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

#include "wetwaresuite/circuit/circuit.hpp"

namespace wetwaresuite::circuit::geom {

// A capsule (segment a-b grown by r; a == b gives a disc) or an axis-aligned box (centre a, half sizes hx, hy).
struct Shape {
    bool box = false;
    Pt a, b;
    double r = 0;
    double hx = 0, hy = 0;
    double x0() const { return box ? a.x - hx : std::min(a.x, b.x) - r; }
    double x1() const { return box ? a.x + hx : std::max(a.x, b.x) + r; }
    double y0() const { return box ? a.y - hy : std::min(a.y, b.y) - r; }
    double y1() const { return box ? a.y + hy : std::max(a.y, b.y) + r; }
};

inline Shape capsule(Pt a, Pt b, double r) {
    Shape s;
    s.a = a, s.b = b, s.r = r;
    return s;
}
inline Shape boxShape(Pt c, double hx, double hy) {
    Shape s;
    s.box = true;
    s.a = c, s.hx = hx, s.hy = hy;
    return s;
}

inline double pointSeg(Pt p, Pt a, Pt b) {
    const double dx = b.x - a.x, dy = b.y - a.y;
    const double l2 = dx * dx + dy * dy;
    double t = l2 > 0 ? ((p.x - a.x) * dx + (p.y - a.y) * dy) / l2 : 0;
    t = std::clamp(t, 0.0, 1.0);
    return std::hypot(p.x - (a.x + t * dx), p.y - (a.y + t * dy));
}

inline bool segsCross(Pt a, Pt b, Pt c, Pt d) {
    auto orient = [](Pt p, Pt q, Pt r) { return (q.x - p.x) * (r.y - p.y) - (q.y - p.y) * (r.x - p.x); };
    const double d1 = orient(c, d, a), d2 = orient(c, d, b), d3 = orient(a, b, c), d4 = orient(a, b, d);
    return ((d1 > 0) != (d2 > 0)) && ((d3 > 0) != (d4 > 0)) && d1 != 0 && d2 != 0 && d3 != 0 && d4 != 0;
}

inline double segSeg(Pt a, Pt b, Pt c, Pt d) {
    if (segsCross(a, b, c, d)) return 0;
    return std::min({pointSeg(a, c, d), pointSeg(b, c, d), pointSeg(c, a, b), pointSeg(d, a, b)});
}

inline double pointBox(Pt p, Pt c, double hx, double hy) {
    const double dx = std::max(0.0, std::abs(p.x - c.x) - hx), dy = std::max(0.0, std::abs(p.y - c.y) - hy);
    return std::hypot(dx, dy);
}

inline double segBox(Pt a, Pt b, Pt c, double hx, double hy) {
    if (pointBox(a, c, hx, hy) == 0 || pointBox(b, c, hx, hy) == 0) return 0;
    const Pt q[4] = {{c.x - hx, c.y - hy}, {c.x + hx, c.y - hy}, {c.x + hx, c.y + hy}, {c.x - hx, c.y + hy}};
    double d = std::min(pointBox(a, c, hx, hy), pointBox(b, c, hx, hy));
    for (int i = 0; i < 4; ++i) {
        if (segsCross(a, b, q[i], q[(i + 1) % 4])) return 0;
        d = std::min(d, pointSeg(q[i], a, b));
    }
    return d;
}

// Gap between two shapes (0 when they touch or overlap).
inline double distance(const Shape& s, const Shape& t) {
    double d;
    if (s.box && t.box) {
        const double dx = std::max(0.0, std::abs(s.a.x - t.a.x) - s.hx - t.hx);
        const double dy = std::max(0.0, std::abs(s.a.y - t.a.y) - s.hy - t.hy);
        return std::hypot(dx, dy);
    }
    if (s.box) d = segBox(t.a, t.b, s.a, s.hx, s.hy) - t.r;
    else if (t.box) d = segBox(s.a, s.b, t.a, t.hx, t.hy) - s.r;
    else d = segSeg(s.a, s.b, t.a, t.b) - s.r - t.r;
    return std::max(0.0, d);
}

inline double distance(const Shape& s, Pt p) {
    if (s.box) return pointBox(p, s.a, s.hx, s.hy);
    return std::max(0.0, pointSeg(p, s.a, s.b) - s.r);
}

// signed: negative inside the shape
inline double signedDistance(const Shape& s, Pt p) {
    if (s.box) {
        const double dx = std::abs(p.x - s.a.x) - s.hx, dy = std::abs(p.y - s.a.y) - s.hy;
        if (dx <= 0 && dy <= 0) return std::max(dx, dy);
        return std::hypot(std::max(dx, 0.0), std::max(dy, 0.0));
    }
    return pointSeg(p, s.a, s.b) - s.r;
}

inline Shape padShape(const WorldPad& p) {
    if (p.angle != 0 && p.shape != PadShape::Rect) {  // a turned oval is still a capsule, just slanted
        const double r = std::min(p.rw, p.rh) / 2, half = std::max(p.rw, p.rh) / 2 - r;
        const double a = p.angle * 3.14159265358979323846 / 180 + (p.rw >= p.rh ? 0 : 3.14159265358979323846 / 2);
        const Pt d{half * std::cos(a), -half * std::sin(a)};
        return capsule({p.pos.x - d.x, p.pos.y - d.y}, {p.pos.x + d.x, p.pos.y + d.y}, r);
    }
    if (p.shape == PadShape::Rect) return boxShape(p.pos, p.w / 2, p.h / 2);  // turned: its upright bounding box
    if (p.shape == PadShape::Circle || std::abs(p.w - p.h) < 1e-9) return capsule(p.pos, p.pos, std::min(p.w, p.h) / 2);
    const double r = std::min(p.w, p.h) / 2;
    if (p.w > p.h) return capsule({p.pos.x - (p.w / 2 - r), p.pos.y}, {p.pos.x + (p.w / 2 - r), p.pos.y}, r);
    return capsule({p.pos.x, p.pos.y - (p.h / 2 - r)}, {p.pos.x, p.pos.y + (p.h / 2 - r)}, r);
}

struct Item {
    enum Kind { Pad, Trace, Via } kind = Pad;
    int index = 0;  // into pads / traces / vias
    Shape shape;
    int layers = 1;
    std::string net;
    std::string owner;  // part ref for pads
};

std::vector<Item> copperItems(const Circuit& c, const std::vector<WorldPad>& pads);

struct Uf {
    std::vector<int> p;
    explicit Uf(std::size_t n = 0) : p(n) {
        for (std::size_t i = 0; i < n; ++i) p[i] = static_cast<int>(i);
    }
    int add() {
        p.push_back(static_cast<int>(p.size()));
        return p.back();
    }
    int find(int x) {
        while (p[static_cast<std::size_t>(x)] != x) {
            p[static_cast<std::size_t>(x)] = p[static_cast<std::size_t>(p[static_cast<std::size_t>(x)])];
            x = p[static_cast<std::size_t>(x)];
        }
        return x;
    }
    void unite(int a, int b) {
        a = find(a), b = find(b);
        if (a != b) p[static_cast<std::size_t>(std::max(a, b))] = std::min(a, b);
    }
};

// Calls fn(i, j) for item pairs whose boxes, grown by margin, overlap (sweep over x).
template <typename Fn>
void forNearPairs(const std::vector<Item>& items, double margin, Fn fn) {
    std::vector<int> order(items.size());
    for (std::size_t i = 0; i < order.size(); ++i) order[i] = static_cast<int>(i);
    std::sort(order.begin(), order.end(), [&](int a, int b) { return items[static_cast<std::size_t>(a)].shape.x0() < items[static_cast<std::size_t>(b)].shape.x0(); });
    for (std::size_t i = 0; i < order.size(); ++i) {
        const Item& a = items[static_cast<std::size_t>(order[i])];
        const double ax1 = a.shape.x1() + margin;
        for (std::size_t j = i + 1; j < order.size(); ++j) {
            const Item& b = items[static_cast<std::size_t>(order[j])];
            if (b.shape.x0() > ax1) break;
            if (b.shape.y0() > a.shape.y1() + margin || a.shape.y0() > b.shape.y1() + margin) continue;
            fn(order[i], order[j]);
        }
    }
}

bool pointInPolygon(const std::vector<Pt>& poly, Pt p);
double distanceToOutline(const std::vector<Pt>& poly, Pt p);

// Connectivity of a board: island id per item (pads, traces, vias) after joining same-net copper
// that touches, including through filled zones.
struct Islands {
    std::vector<Item> items;
    std::vector<int> island;  // per item
};
Islands islands(const Circuit& c, const std::vector<WorldPad>& pads, const std::vector<ZoneFill>& fills);

}  // namespace wetwaresuite::circuit::geom
