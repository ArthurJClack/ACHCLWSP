// Exact polygon booleans (intersection, union, difference, symmetric difference) and exact
// buffers, done on the edges themselves instead of a grid:
//   1. every ring edge of both sides is split where it meets another edge (a bucket grid keeps
//      that near-linear), with points snapped to a fine tolerance so shared vertices match;
//   2. each piece is classified by the nonzero winding of both sides just to its left and right,
//      and kept when the result's inside/outside differs across it (pointing so inside is left);
//   3. the kept pieces are walked into rings (turning most left at shared corners, so touching
//      shapes come out as separate rings) and holes are put in the smallest outer ring around them.
// Planar on whatever coordinates come in: lon/lat for overlays (straight edges in lon/lat, just as
// the map draws them), metres on a local transverse Mercator grid for buffers.
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <unordered_map>
#include <vector>

#include "wetwaresuite/maps/maps.hpp"

namespace wetwaresuite::maps {

namespace {

struct P {
    double x = 0, y = 0;
};

P operator-(P a, P b) { return {a.x - b.x, a.y - b.y}; }
P operator+(P a, P b) { return {a.x + b.x, a.y + b.y}; }
P operator*(P a, double k) { return {a.x * k, a.y * k}; }
double cross(P a, P b) { return a.x * b.y - a.y * b.x; }
double dot(P a, P b) { return a.x * b.x + a.y * b.y; }

struct Key {
    std::int64_t x = 0, y = 0;
    bool operator==(const Key& o) const { return x == o.x && y == o.y; }
    bool operator<(const Key& o) const { return x != o.x ? x < o.x : y < o.y; }
};
struct KeyHash {
    std::size_t operator()(const Key& k) const { return std::hash<std::int64_t>()(k.x * 0x9E3779B97F4A7C15ll ^ k.y); }
};
struct SegKey {
    Key a, b;
    bool operator==(const SegKey& o) const { return a == o.a && b == o.b; }
};
struct SegKeyHash {
    std::size_t operator()(const SegKey& k) const { return KeyHash()(k.a) * 31 ^ KeyHash()(k.b); }
};

struct Seg {
    P a, b;
    int side = 0;  // 0 = A, 1 = B
};

using RingP = std::vector<P>;

double ringArea(const RingP& r) {
    double s = 0;
    for (std::size_t i = 0, n = r.size(); i < n; ++i) s += cross(r[i], r[(i + 1) % n]);
    return s / 2;
}

bool insideRing(const RingP& r, P p) {
    bool in = false;
    for (std::size_t i = 0, j = r.size() - 1; i < r.size(); j = i++) {
        if ((r[i].y > p.y) != (r[j].y > p.y) && p.x < (r[j].x - r[i].x) * (p.y - r[i].y) / (r[j].y - r[i].y) + r[i].x) in = !in;
    }
    return in;
}

class Snapper {
public:
    explicit Snapper(double tol) : tol_(tol) {}
    Key key(P p) const { return {std::llround(p.x / tol_), std::llround(p.y / tol_)}; }
    P snap(P p) const { return {std::round(p.x / tol_) * tol_, std::round(p.y / tol_) * tol_}; }
    double tol() const { return tol_; }

private:
    double tol_;
};

// Rings of one side, outer rings counter-clockwise and holes clockwise, so nonzero winding
// treats overlapping polygons of the same side as one area.
void addSide(const std::vector<RingP>& outerRings, const std::vector<std::vector<RingP>>& holes, int side, const Snapper& sn, std::vector<Seg>& segs) {
    auto add = [&](RingP r, bool outer) {
        RingP clean;
        for (const P& p : r) {
            const P s = sn.snap(p);
            if (clean.empty() || !(sn.key(clean.back()) == sn.key(s))) clean.push_back(s);
        }
        while (clean.size() > 1 && sn.key(clean.front()) == sn.key(clean.back())) clean.pop_back();
        if (clean.size() < 3) return;
        const double a = ringArea(clean);
        if (a == 0) return;
        if ((a > 0) != outer) std::reverse(clean.begin(), clean.end());
        for (std::size_t i = 0; i < clean.size(); ++i) segs.push_back({clean[i], clean[(i + 1) % clean.size()], side});
    };
    for (std::size_t i = 0; i < outerRings.size(); ++i) {
        add(outerRings[i], true);
        for (const auto& h : holes[i]) add(h, false);
    }
}

// Splits every segment where it meets another one.
std::vector<Seg> splitAll(const std::vector<Seg>& segs, const Snapper& sn) {
    const std::size_t n = segs.size();
    double minX = std::numeric_limits<double>::infinity(), minY = minX, maxX = -minX, maxY = -minX;
    for (const auto& s : segs) {
        minX = std::min({minX, s.a.x, s.b.x});
        maxX = std::max({maxX, s.a.x, s.b.x});
        minY = std::min({minY, s.a.y, s.b.y});
        maxY = std::max({maxY, s.a.y, s.b.y});
    }
    const int cells = static_cast<int>(std::clamp(std::sqrt(static_cast<double>(n)) / 1.5, 1.0, 1024.0));
    const double cw = std::max((maxX - minX) / cells, sn.tol()), ch = std::max((maxY - minY) / cells, sn.tol());
    auto cx = [&](double x) { return std::clamp(static_cast<int>((x - minX) / cw), 0, cells - 1); };
    auto cy = [&](double y) { return std::clamp(static_cast<int>((y - minY) / ch), 0, cells - 1); };
    std::vector<std::vector<std::uint32_t>> grid(static_cast<std::size_t>(cells) * cells);
    for (std::size_t i = 0; i < n; ++i) {
        const Seg& s = segs[i];
        for (int y = cy(std::min(s.a.y, s.b.y)); y <= cy(std::max(s.a.y, s.b.y)); ++y)
            for (int x = cx(std::min(s.a.x, s.b.x)); x <= cx(std::max(s.a.x, s.b.x)); ++x) grid[static_cast<std::size_t>(y) * cells + x].push_back(static_cast<std::uint32_t>(i));
    }
    std::vector<std::vector<P>> cuts(n);
    const double tol = sn.tol();
    auto onInterior = [&](const Seg& s, P q, std::size_t idx) {
        const P d = s.b - s.a;
        const double len2 = dot(d, d);
        if (len2 <= 0) return;
        const double t = dot(q - s.a, d) / len2;
        if (t <= 0 || t >= 1) return;
        const P foot = s.a + d * t;
        const P off = q - foot;
        if (dot(off, off) <= tol * tol * 4) cuts[idx].push_back(sn.snap(q));
    };
    for (int gy = 0; gy < cells; ++gy) {
        for (int gx = 0; gx < cells; ++gx) {
            const auto& cell = grid[static_cast<std::size_t>(gy) * cells + gx];
            for (std::size_t ii = 0; ii < cell.size(); ++ii) {
                const Seg& s = segs[cell[ii]];
                for (std::size_t jj = ii + 1; jj < cell.size(); ++jj) {
                    const Seg& t = segs[cell[jj]];
                    // the pair is looked at once: in the cell holding the low corner of their boxes' overlap
                    const double ox = std::max(std::min(s.a.x, s.b.x), std::min(t.a.x, t.b.x)), oy = std::max(std::min(s.a.y, s.b.y), std::min(t.a.y, t.b.y));
                    if (ox > std::min(std::max(s.a.x, s.b.x), std::max(t.a.x, t.b.x)) + tol || oy > std::min(std::max(s.a.y, s.b.y), std::max(t.a.y, t.b.y)) + tol) continue;
                    if (cx(ox) != gx || cy(oy) != gy) continue;
                    const std::size_t i = cell[ii], j = cell[jj];
                    // endpoints lying on the other (T joins, overlaps)
                    onInterior(s, t.a, i);
                    onInterior(s, t.b, i);
                    onInterior(t, s.a, j);
                    onInterior(t, s.b, j);
                    const P r = s.b - s.a, q = t.b - t.a;
                    const double den = cross(r, q);
                    if (std::abs(den) <= 1e-300) continue;
                    const double u = cross(t.a - s.a, q) / den, v = cross(t.a - s.a, r) / den;
                    if (u <= 0 || u >= 1 || v <= 0 || v >= 1) continue;
                    const P x = sn.snap(s.a + r * u);
                    cuts[i].push_back(x);
                    cuts[j].push_back(x);
                }
            }
        }
    }
    std::vector<Seg> out;
    out.reserve(n * 2);
    for (std::size_t i = 0; i < n; ++i) {
        const Seg& s = segs[i];
        auto& c = cuts[i];
        const P d = s.b - s.a;
        std::sort(c.begin(), c.end(), [&](P p, P q) { return dot(p - s.a, d) < dot(q - s.a, d); });
        P prev = s.a;
        for (const P& p : c) {
            if (sn.key(p) == sn.key(prev) || sn.key(p) == sn.key(s.b)) continue;
            out.push_back({prev, p, s.side});
            prev = p;
        }
        if (!(sn.key(prev) == sn.key(s.b))) out.push_back({prev, s.b, s.side});
    }
    return out;
}

// Nonzero winding of both sides at points, with the segments bucketed in horizontal bands.
class Winding {
public:
    explicit Winding(const std::vector<Seg>& segs) : segs_(segs) {
        minY_ = std::numeric_limits<double>::infinity();
        double maxY = -minY_;
        for (const auto& s : segs) {
            minY_ = std::min({minY_, s.a.y, s.b.y});
            maxY = std::max({maxY, s.a.y, s.b.y});
        }
        bands_ = static_cast<int>(std::clamp(std::sqrt(static_cast<double>(segs.size())) * 2, 1.0, 8192.0));
        bh_ = std::max((maxY - minY_) / bands_, 1e-300);
        band_.resize(static_cast<std::size_t>(bands_));
        for (std::size_t i = 0; i < segs.size(); ++i) {
            const auto& s = segs[i];
            if (s.a.y == s.b.y) continue;  // horizontal edges never cross a horizontal ray
            for (int b = band(std::min(s.a.y, s.b.y)); b <= band(std::max(s.a.y, s.b.y)); ++b) band_[static_cast<std::size_t>(b)].push_back(static_cast<std::uint32_t>(i));
        }
    }
    void at(P p, int& wa, int& wb) const {
        wa = wb = 0;
        if (p.y < minY_ || band(p.y) < 0) return;
        for (auto i : band_[static_cast<std::size_t>(band(p.y))]) {
            const Seg& s = segs_[i];
            if ((s.a.y <= p.y) == (s.b.y <= p.y)) continue;
            const double x = s.a.x + (p.y - s.a.y) / (s.b.y - s.a.y) * (s.b.x - s.a.x);
            if (x <= p.x) continue;
            const int dir = s.b.y > s.a.y ? 1 : -1;
            (s.side == 0 ? wa : wb) += dir;
        }
    }

private:
    int band(double y) const { return std::clamp(static_cast<int>((y - minY_) / bh_), 0, bands_ - 1); }
    const std::vector<Seg>& segs_;
    double minY_ = 0, bh_ = 1;
    int bands_ = 1;
    std::vector<std::vector<std::uint32_t>> band_;
};

bool keep(OverlayOp op, bool a, bool b) {
    switch (op) {
        case OverlayOp::Intersection: return a && b;
        case OverlayOp::Union: return a || b;
        case OverlayOp::Difference: return a && !b;
        case OverlayOp::SymDifference: return a != b;
    }
    return false;
}

// Kept edges (inside on the left) walked into rings; holes put into outer rings.
std::vector<std::vector<RingP>> assemble(const std::vector<Seg>& edges, const Snapper& sn) {
    std::unordered_map<Key, std::vector<std::uint32_t>, KeyHash> from;
    for (std::size_t i = 0; i < edges.size(); ++i) from[sn.key(edges[i].a)].push_back(static_cast<std::uint32_t>(i));
    std::vector<char> used(edges.size(), 0);
    std::vector<RingP> outers, holes;
    for (std::size_t start = 0; start < edges.size(); ++start) {
        if (used[start]) continue;
        RingP ring;
        std::size_t e = start;
        const Key home = sn.key(edges[start].a);
        bool closed = false;
        for (std::size_t guard = 0; guard <= edges.size(); ++guard) {
            used[e] = 1;
            ring.push_back(edges[e].a);
            const Key at = sn.key(edges[e].b);
            if (at == home) {
                closed = true;
                break;
            }
            const P in = edges[e].b - edges[e].a;
            auto it = from.find(at);
            if (it == from.end()) break;
            // most left turn first: touching shapes stay separate rings
            double best = -10;
            std::size_t next = edges.size();
            for (auto c : it->second) {
                if (used[c]) continue;
                const P out = edges[c].b - edges[c].a;
                const double ang = std::atan2(cross(in, out), dot(in, out));
                if (ang > best) {
                    best = ang;
                    next = c;
                }
            }
            if (next == edges.size()) break;
            e = next;
        }
        if (!closed || ring.size() < 3) continue;
        const double a = ringArea(ring);
        if (std::abs(a) <= sn.tol() * sn.tol()) continue;
        (a > 0 ? outers : holes).push_back(std::move(ring));
    }
    std::vector<std::vector<RingP>> polys;
    std::vector<double> areas;
    for (auto& o : outers) {
        areas.push_back(ringArea(o));
        polys.push_back({std::move(o)});
    }
    for (auto& h : holes) {
        const P probe = (h[0] + h[1]) * 0.5;
        std::size_t owner = polys.size();
        for (std::size_t i = 0; i < polys.size(); ++i) {
            if ((owner == polys.size() || areas[i] < areas[owner]) && insideRing(polys[i][0], probe)) owner = i;
        }
        if (owner < polys.size()) polys[owner].push_back(std::move(h));
    }
    return polys;
}

std::vector<std::vector<RingP>> booleanRings(const std::vector<std::vector<RingP>>& a, const std::vector<std::vector<RingP>>& b, OverlayOp op, double tol) {
    const Snapper sn(tol);
    std::vector<Seg> segs;
    for (int side = 0; side < 2; ++side) {
        const auto& polys = side == 0 ? a : b;
        std::vector<RingP> outer;
        std::vector<std::vector<RingP>> holes;
        for (const auto& p : polys) {
            if (p.empty()) continue;
            outer.push_back(p[0]);
            holes.emplace_back(p.begin() + 1, p.end());
        }
        addSide(outer, holes, side, sn, segs);
    }
    if (segs.empty()) return {};
    std::vector<Seg> pieces = splitAll(segs, sn);
    // one copy of each piece (shared borders appear once per side)
    std::unordered_map<SegKey, std::uint32_t, SegKeyHash> seen;
    std::vector<Seg> unique;
    for (const auto& s : pieces) {
        Key ka = sn.key(s.a), kb = sn.key(s.b);
        P pa = s.a, pb = s.b;
        if (kb < ka) {
            std::swap(ka, kb);
            std::swap(pa, pb);
        }
        if (seen.emplace(SegKey{ka, kb}, 0).second) unique.push_back({pa, pb, 0});
    }
    const Winding w(pieces);
    std::vector<Seg> kept;
    for (const auto& s : unique) {
        const P d = s.b - s.a;
        const double len = std::sqrt(dot(d, d));
        if (len <= 0) continue;
        const double eps = std::clamp(len * 1e-4, tol * 2, tol * 1000);
        const P nrm{-d.y / len * eps, d.x / len * eps};
        const P mid = (s.a + s.b) * 0.5;
        int la = 0, lb = 0, ra = 0, rb = 0;
        w.at(mid + nrm, la, lb);
        w.at(mid - nrm, ra, rb);
        const bool left = keep(op, la != 0, lb != 0), right = keep(op, ra != 0, rb != 0);
        if (left == right) continue;
        kept.push_back(left ? Seg{s.a, s.b, 0} : Seg{s.b, s.a, 0});
    }
    return assemble(kept, sn);
}

std::vector<std::vector<RingP>> polysOf(const std::vector<const Geometry*>& gs, const std::function<P(const Coord&)>& to) {
    std::vector<std::vector<RingP>> out;
    for (const Geometry* g : gs) {
        if (g->type != GeomType::Polygon) continue;
        for (const auto& poly : g->polygons) {
            std::vector<RingP> rings;
            for (const auto& r : poly) {
                RingP rp;
                rp.reserve(r.size());
                for (const auto& c : r) rp.push_back(to(c));
                rings.push_back(std::move(rp));
            }
            out.push_back(std::move(rings));
        }
    }
    return out;
}

Geometry toGeometry(const std::vector<std::vector<RingP>>& polys, const std::function<Coord(P)>& back) {
    Geometry g;
    for (const auto& poly : polys) {
        Polygon pg;
        for (const auto& r : poly) {
            Ring ring;
            ring.reserve(r.size());
            for (const P& p : r) ring.push_back(back(p));
            pg.push_back(std::move(ring));
        }
        g.polygons.push_back(std::move(pg));
    }
    g.type = g.polygons.empty() ? GeomType::None : GeomType::Polygon;
    return g;
}

std::size_t edgeCount(const std::vector<const Geometry*>& gs) {
    std::size_t n = 0;
    for (const Geometry* g : gs) n += g->vertexCount();
    return n;
}

RingP circle(P c, double r, int segs) {
    RingP out;
    for (int i = 0; i < segs; ++i) {
        const double a = 2 * kPi * i / segs;
        out.push_back({c.x + r * std::cos(a), c.y + r * std::sin(a)});
    }
    return out;
}

constexpr std::size_t kExactEdgeBudget = 600000;

}  // namespace

Geometry exactOverlay(const std::vector<const Geometry*>& a, const std::vector<const Geometry*>& b, OverlayOp op) {
    BBox box;
    for (const Geometry* g : a) box.add(bboxOf(*g));
    for (const Geometry* g : b) box.add(bboxOf(*g));
    if (!box.valid()) return {};
    const double extent = std::max({std::abs(box.minX), std::abs(box.maxX), std::abs(box.minY), std::abs(box.maxY), 1.0});
    // about 0.02 mm at lon 180, a power of two so round numbers (0.25, 10.5) stay exact
    const double tol = std::ldexp(1.0, static_cast<int>(std::floor(std::log2(extent * 1e-12))));
    auto to = [](const Coord& c) { return P{c.x, c.y}; };
    auto back = [](P p) { return Coord{p.x, p.y}; };
    return toGeometry(booleanRings(polysOf(a, to), polysOf(b, to), op, tol), back);
}

Geometry exactBuffer(const std::vector<const Geometry*>& gs, double d, int circleSegments) {
    BBox box;
    for (const Geometry* g : gs) box.add(bboxOf(*g));
    if (!box.valid() || d == 0) return {};
    const Crs crs = Crs::localTm((box.minX + box.maxX) / 2);
    auto to = [&crs](const Coord& c) {
        const Coord m = crs.forward(c);
        return P{m.x, m.y};
    };
    auto back = [&crs](P p) { return crs.inverse({p.x, p.y}); };
    const double r = std::abs(d);
    const int segs = std::clamp(circleSegments, 8, 720);
    // the shapes swept by a disc of radius r along every edge: a box per edge, a disc per vertex
    std::vector<std::vector<RingP>> sweep;
    auto edge = [&](P a, P b) {
        const P dd = b - a;
        const double len = std::sqrt(dot(dd, dd));
        if (len <= 0) return;
        const P n{-dd.y / len * r, dd.x / len * r};
        sweep.push_back({{a - n, b - n, b + n, a + n}});
    };
    auto chain = [&](const std::vector<Coord>& cs, bool closed) {
        std::vector<P> pts;
        for (const auto& c : cs) pts.push_back(to(c));
        for (const P& p : pts) sweep.push_back({circle(p, r, segs)});
        const std::size_t n = closed ? pts.size() : pts.size() - 1;
        for (std::size_t i = 0; i < n && pts.size() > 1; ++i) edge(pts[i], pts[(i + 1) % pts.size()]);
    };
    for (const Geometry* g : gs) {
        if (d > 0) {
            for (const auto& p : g->points) chain({p}, false);
            for (const auto& l : g->lines) chain(l, false);
        }
        for (const auto& poly : g->polygons)
            for (const auto& ring : poly) chain(ring, true);
    }
    std::vector<std::vector<RingP>> own = polysOf(gs, to);
    std::vector<std::vector<RingP>> result;
    if (d > 0) {
        own.insert(own.end(), sweep.begin(), sweep.end());
        result = booleanRings(own, {}, OverlayOp::Union, std::ldexp(1.0, -20));  // about 1 micrometre
    } else {
        result = booleanRings(own, sweep, OverlayOp::Difference, std::ldexp(1.0, -20));
    }
    return toGeometry(result, back);
}

bool exactFits(const std::vector<const Geometry*>& gs, int circleSegments) {
    return edgeCount(gs) * static_cast<std::size_t>(std::max(circleSegments, 8) + 4) <= kExactEdgeBudget;
}

bool exactFits(const std::vector<const Geometry*>& a, const std::vector<const Geometry*>& b) { return edgeCount(a) + edgeCount(b) <= kExactEdgeBudget; }

}  // namespace wetwaresuite::maps
