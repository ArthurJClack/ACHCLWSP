// Geometry operations: point in polygon, simplify, clip, buffer (distance
// field + marching squares), nearest point, filter expressions, GPX track
// statistics and heatmaps.
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <map>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>
#include <unordered_map>

#include "wetwaresuite/maps/maps.hpp"

namespace wetwaresuite::maps {

namespace {

struct P2 {
    double x = 0, y = 0;
};

double segDist2(const P2& p, const P2& a, const P2& b, P2* closest = nullptr) {
    const double dx = b.x - a.x, dy = b.y - a.y;
    const double len2 = dx * dx + dy * dy;
    double t = len2 > 0 ? ((p.x - a.x) * dx + (p.y - a.y) * dy) / len2 : 0;
    t = std::clamp(t, 0.0, 1.0);
    const P2 c{a.x + t * dx, a.y + t * dy};
    if (closest) *closest = c;
    return (p.x - c.x) * (p.x - c.x) + (p.y - c.y) * (p.y - c.y);
}

Coord lerp(const Coord& a, const Coord& b, double t) {
    Coord c;
    c.x = a.x + (b.x - a.x) * t;
    c.y = a.y + (b.y - a.y) * t;
    c.z = a.z + (b.z - a.z) * t;
    c.t = a.t + (b.t - a.t) * t;
    return c;
}

}  // namespace

// ---- planar helpers ---------------------------------------------------------------------

double signedArea(const Ring& r) {
    double s = 0;
    for (std::size_t i = 0; i < r.size(); ++i) {
        const Coord& a = r[i];
        const Coord& b = r[(i + 1) % r.size()];
        s += a.x * b.y - b.x * a.y;
    }
    return s / 2;
}

bool pointInRing(const Ring& r, double x, double y) {
    bool in = false;
    for (std::size_t i = 0, j = r.size() - 1; i < r.size(); j = i++) {
        const Coord& a = r[i];
        const Coord& b = r[j];
        if (((a.y > y) != (b.y > y)) && (x < (b.x - a.x) * (y - a.y) / (b.y - a.y) + a.x)) in = !in;
    }
    return in;
}

bool pointInPolygon(const Polygon& p, double x, double y) {
    if (p.empty() || !pointInRing(p[0], x, y)) return false;
    for (std::size_t i = 1; i < p.size(); ++i) {
        if (pointInRing(p[i], x, y)) return false;
    }
    return true;
}

bool pointInGeometry(const Geometry& g, double lon, double lat) {
    for (const auto& p : g.polygons) {
        if (pointInPolygon(p, lon, lat)) return true;
    }
    return false;
}

// ---- simplify -----------------------------------------------------------------------------

namespace {

void dpMark(const std::vector<P2>& pts, std::size_t a, std::size_t b, double tol2, std::vector<char>& keep) {
    // iterative to stay safe on huge tracks
    std::vector<std::pair<std::size_t, std::size_t>> stack{{a, b}};
    while (!stack.empty()) {
        auto [i, j] = stack.back();
        stack.pop_back();
        if (j <= i + 1) continue;
        double best = -1;
        std::size_t idx = i;
        for (std::size_t k = i + 1; k < j; ++k) {
            const double d = segDist2(pts[k], pts[i], pts[j]);
            if (d > best) {
                best = d;
                idx = k;
            }
        }
        if (best > tol2) {
            keep[idx] = 1;
            stack.push_back({i, idx});
            stack.push_back({idx, j});
        }
    }
}

std::vector<P2> projectAll(const std::vector<Coord>& cs, const Crs& crs) {
    std::vector<P2> out;
    out.reserve(cs.size());
    for (const auto& c : cs) {
        const Coord p = crs.forward(c);
        out.push_back({p.x, p.y});
    }
    return out;
}

Line simplifyLine(const Line& l, double tol, const Crs& crs, bool closed) {
    if (l.size() <= (closed ? 3u : 2u)) return l;
    std::vector<Coord> work = l;
    if (closed) work.push_back(l.front());
    const auto pts = projectAll(work, crs);
    std::vector<char> keep(work.size(), 0);
    keep.front() = keep.back() = 1;
    if (closed) {
        // split at the point farthest from the start so both halves are proper chains
        std::size_t far = 0;
        double best = -1;
        for (std::size_t k = 1; k + 1 < pts.size(); ++k) {
            const double d = (pts[k].x - pts[0].x) * (pts[k].x - pts[0].x) + (pts[k].y - pts[0].y) * (pts[k].y - pts[0].y);
            if (d > best) {
                best = d;
                far = k;
            }
        }
        keep[far] = 1;
        dpMark(pts, 0, far, tol * tol, keep);
        dpMark(pts, far, pts.size() - 1, tol * tol, keep);
    } else {
        dpMark(pts, 0, pts.size() - 1, tol * tol, keep);
    }
    Line out;
    for (std::size_t i = 0; i < work.size(); ++i) {
        if (keep[i]) out.push_back(work[i]);
    }
    if (closed) out.pop_back();
    return out;
}

}  // namespace

Geometry simplify(const Geometry& g, double tol) {
    if (tol <= 0 || g.empty()) return g;
    const BBox b = bboxOf(g);
    const Crs crs = Crs::localTm((b.minX + b.maxX) / 2);
    Geometry out;
    out.type = g.type;
    out.points = g.points;
    for (const auto& l : g.lines) {
        Line s = simplifyLine(l, tol, crs, false);
        if (s.size() >= 2) out.lines.push_back(std::move(s));
    }
    for (const auto& p : g.polygons) {
        Polygon np;
        for (std::size_t i = 0; i < p.size(); ++i) {
            Ring r = simplifyLine(p[i], tol, crs, true);
            if (r.size() < 3) {
                if (i == 0) break;
                continue;
            }
            np.push_back(std::move(r));
        }
        if (!np.empty()) out.polygons.push_back(std::move(np));
    }
    if (out.empty()) out.type = GeomType::None;
    return out;
}

// ---- clip -------------------------------------------------------------------------------

namespace {

Ring clipRing(const Ring& ring, const BBox& b) {
    Ring cur = ring;
    for (int edge = 0; edge < 4 && !cur.empty(); ++edge) {
        auto inside = [&](const Coord& c) {
            switch (edge) {
                case 0: return c.x >= b.minX;
                case 1: return c.x <= b.maxX;
                case 2: return c.y >= b.minY;
                default: return c.y <= b.maxY;
            }
        };
        auto cross = [&](const Coord& p, const Coord& q) {
            double t = 0;
            switch (edge) {
                case 0: t = (b.minX - p.x) / (q.x - p.x); break;
                case 1: t = (b.maxX - p.x) / (q.x - p.x); break;
                case 2: t = (b.minY - p.y) / (q.y - p.y); break;
                default: t = (b.maxY - p.y) / (q.y - p.y); break;
            }
            return lerp(p, q, t);
        };
        Ring next;
        for (std::size_t i = 0; i < cur.size(); ++i) {
            const Coord& p = cur[i];
            const Coord& q = cur[(i + 1) % cur.size()];
            const bool pin = inside(p), qin = inside(q);
            if (pin) next.push_back(p);
            if (pin != qin) next.push_back(cross(p, q));
        }
        cur = std::move(next);
    }
    return cur;
}

}  // namespace

Geometry clip(const Geometry& g, const BBox& b) {
    Geometry out;
    out.type = g.type;
    for (const auto& p : g.points) {
        if (b.contains(p.x, p.y)) out.points.push_back(p);
    }
    for (const auto& l : g.lines) {
        Line cur;
        for (std::size_t i = 1; i < l.size(); ++i) {
            // Liang-Barsky on segment l[i-1] -> l[i]
            const Coord& a = l[i - 1];
            const Coord& c = l[i];
            double t0 = 0, t1 = 1;
            const double dx = c.x - a.x, dy = c.y - a.y;
            bool visible = true;
            auto test = [&](double p, double q) {
                if (p == 0) {
                    if (q < 0) visible = false;
                    return;
                }
                const double r = q / p;
                if (p < 0) {
                    if (r > t1) visible = false;
                    else if (r > t0) t0 = r;
                } else {
                    if (r < t0) visible = false;
                    else if (r < t1) t1 = r;
                }
            };
            test(-dx, a.x - b.minX);
            test(dx, b.maxX - a.x);
            test(-dy, a.y - b.minY);
            test(dy, b.maxY - a.y);
            if (!visible) {
                if (cur.size() >= 2) out.lines.push_back(cur);
                cur.clear();
                continue;
            }
            const Coord s = t0 > 0 ? lerp(a, c, t0) : a;
            const Coord e = t1 < 1 ? lerp(a, c, t1) : c;
            if (cur.empty() || t0 > 0) {
                if (cur.size() >= 2) out.lines.push_back(cur);
                cur.clear();
                cur.push_back(s);
            }
            cur.push_back(e);
            if (t1 < 1) {
                if (cur.size() >= 2) out.lines.push_back(cur);
                cur.clear();
            }
        }
        if (cur.size() >= 2) out.lines.push_back(cur);
    }
    for (const auto& p : g.polygons) {
        Polygon np;
        for (std::size_t i = 0; i < p.size(); ++i) {
            Ring r = clipRing(p[i], b);
            if (r.size() < 3 || std::abs(signedArea(r)) < 1e-18) {
                if (i == 0) break;
                continue;
            }
            np.push_back(std::move(r));
        }
        if (!np.empty()) out.polygons.push_back(std::move(np));
    }
    if (out.empty()) out.type = GeomType::None;
    return out;
}

// ---- buffer -----------------------------------------------------------------------------

namespace {

Polygon geodesicCircle(const Coord& c, double r, int n) {
    Ring ring;
    for (int i = 0; i < n; ++i) ring.push_back(geodesicDirect(c.x, c.y, -360.0 * i / n, r));  // counter-clockwise
    return {ring};
}

struct FieldBuilder {
    std::vector<std::pair<P2, P2>> segs;
    std::vector<std::vector<std::vector<P2>>> polys;  // planar rings per polygon
    std::vector<P2> pts;
};

std::vector<P2> ringPlanar(const Ring& r, const Crs& crs) { return projectAll(r, crs); }

}  // namespace

namespace {

struct FieldGrid {
    Crs crs;
    double ox = 0, oy = 0, cell = 1;
    int W = 0, H = 0;
};

// The geometries as planar pieces on crs; returns their planar box.
BBox collectField(const std::vector<const Geometry*>& gs, const Crs& crs, FieldBuilder& fb) {
    BBox pb;
    for (const Geometry* g : gs) {
        for (const auto& c : g->points) {
            const Coord p = crs.forward(c);
            fb.pts.push_back({p.x, p.y});
            pb.add(p.x, p.y);
        }
        for (const auto& l : g->lines) {
            const auto pts = projectAll(l, crs);
            for (std::size_t i = 0; i < pts.size(); ++i) {
                pb.add(pts[i].x, pts[i].y);
                if (i) fb.segs.push_back({pts[i - 1], pts[i]});
            }
            if (pts.size() == 1) fb.pts.push_back(pts[0]);
        }
        for (const auto& poly : g->polygons) {
            std::vector<std::vector<P2>> rings;
            for (const auto& r : poly) {
                auto pts = ringPlanar(r, crs);
                for (std::size_t i = 0; i < pts.size(); ++i) {
                    pb.add(pts[i].x, pts[i].y);
                    fb.segs.push_back({pts[i], pts[(i + 1) % pts.size()]});
                }
                rings.push_back(std::move(pts));
            }
            fb.polys.push_back(std::move(rings));
        }
    }
    return pb;
}

FieldGrid makeGrid(const Crs& crs, const BBox& pb, double pad, double cell) {
    FieldGrid g;
    g.crs = crs;
    g.cell = cell;
    g.ox = pb.minX - pad;
    g.oy = pb.minY - pad;
    g.W = static_cast<int>(std::ceil((pb.width() + 2 * pad) / cell)) + 1;
    g.H = static_cast<int>(std::ceil((pb.height() + 2 * pad) / cell)) + 1;
    return g;
}

// Signed distance from every grid node to the geometries (planar metres): negative inside polygons.
std::vector<double> signedDistance(const FieldBuilder& fb, const FieldGrid& grid) {
    const double ox = grid.ox, oy = grid.oy, cell = grid.cell;
    const int W = grid.W, H = grid.H;
    const std::size_t N = static_cast<std::size_t>(W) * H;
    auto nodeX = [&](int i) { return ox + i * cell; };
    auto nodeY = [&](int j) { return oy + j * cell; };

    // seed nodes, each remembering one segment (or point) that passes by it
    std::vector<int> seedOf(N, -1);  // index into fb.segs, or segs.size() + point index
    auto mark = [&](const P2& p, int id) {
        const int i = static_cast<int>(std::lround((p.x - ox) / cell)), j = static_cast<int>(std::lround((p.y - oy) / cell));
        if (i < 0 || j < 0 || i >= W || j >= H) return;
        seedOf[static_cast<std::size_t>(j) * W + i] = id;
    };
    for (std::size_t s = 0; s < fb.segs.size(); ++s) {
        const auto& [a, b] = fb.segs[s];
        const double len = std::hypot(b.x - a.x, b.y - a.y);
        const int steps = std::max(1, static_cast<int>(std::ceil(len / (cell * 0.5))));
        for (int k = 0; k <= steps; ++k) {
            const double t = static_cast<double>(k) / steps;
            mark({a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t}, static_cast<int>(s));
        }
    }
    for (std::size_t k = 0; k < fb.pts.size(); ++k) mark(fb.pts[k], static_cast<int>(fb.segs.size() + k));

    // exact Euclidean distance transform (Felzenszwalb), keeping the nearest seed node
    constexpr double kInf = 1e30;
    std::vector<double> f(N, kInf);
    std::vector<int> srcRow(N, -1);
    for (int i = 0; i < W; ++i) {
        int last = -1;
        for (int j = 0; j < H; ++j) {
            if (seedOf[static_cast<std::size_t>(j) * W + i] >= 0) last = j;
            if (last >= 0) {
                f[static_cast<std::size_t>(j) * W + i] = static_cast<double>(j - last) * (j - last);
                srcRow[static_cast<std::size_t>(j) * W + i] = last;
            }
        }
        last = -1;
        for (int j = H - 1; j >= 0; --j) {
            if (seedOf[static_cast<std::size_t>(j) * W + i] >= 0) last = j;
            if (last >= 0) {
                const double v = static_cast<double>(last - j) * (last - j);
                if (v < f[static_cast<std::size_t>(j) * W + i]) {
                    f[static_cast<std::size_t>(j) * W + i] = v;
                    srcRow[static_cast<std::size_t>(j) * W + i] = last;
                }
            }
        }
    }
    std::vector<double> dist(N, kInf);
    {
        std::vector<int> v(static_cast<std::size_t>(W));
        std::vector<double> z(static_cast<std::size_t>(W) + 1);
        for (int j = 0; j < H; ++j) {
            const double* g = f.data() + static_cast<std::size_t>(j) * W;
            int k = -1;
            for (int q = 0; q < W; ++q) {
                if (g[q] >= kInf) continue;
                if (k < 0) {
                    k = 0;
                    v[0] = q;
                    z[0] = -kInf;
                    z[1] = kInf;
                    continue;
                }
                double s = 0;
                while (true) {
                    const int p = v[static_cast<std::size_t>(k)];
                    s = ((g[q] + static_cast<double>(q) * q) - (g[p] + static_cast<double>(p) * p)) / (2.0 * (q - p));
                    if (s <= z[static_cast<std::size_t>(k)] && k > 0) {
                        --k;
                        continue;
                    }
                    break;
                }
                if (s <= z[static_cast<std::size_t>(k)]) {
                    // k == 0 and the new parabola dominates everywhere
                    v[0] = q;
                    z[0] = -kInf;
                    z[1] = kInf;
                    continue;
                }
                ++k;
                v[static_cast<std::size_t>(k)] = q;
                z[static_cast<std::size_t>(k)] = s;
                z[static_cast<std::size_t>(k) + 1] = kInf;
            }
            if (k < 0) continue;
            int kk = 0;
            for (int x = 0; x < W; ++x) {
                while (z[static_cast<std::size_t>(kk) + 1] < x) ++kk;
                const int q = v[static_cast<std::size_t>(kk)];
                const int row = srcRow[static_cast<std::size_t>(j) * W + q];
                const int id = seedOf[static_cast<std::size_t>(row) * W + q];
                const P2 node{nodeX(x), nodeY(j)};
                double d2 = 0;
                if (id < static_cast<int>(fb.segs.size())) {
                    d2 = segDist2(node, fb.segs[static_cast<std::size_t>(id)].first, fb.segs[static_cast<std::size_t>(id)].second);
                } else {
                    const P2& p = fb.pts[static_cast<std::size_t>(id) - fb.segs.size()];
                    d2 = (node.x - p.x) * (node.x - p.x) + (node.y - p.y) * (node.y - p.y);
                }
                // the seed's own geometry is never farther than the seed node itself (+ half a cell)
                const double approx = std::sqrt(static_cast<double>(x - q) * (x - q) + g[q]) * cell;
                dist[static_cast<std::size_t>(j) * W + x] = std::min(std::sqrt(d2), approx + cell * 0.71);
            }
        }
    }
    // inside polygons: even-odd per polygon, union over polygons
    std::vector<char> inside(N, 0);
    std::vector<double> xs;
    for (const auto& rings : fb.polys) {
        for (int j = 0; j < H; ++j) {
            const double y = nodeY(j);
            xs.clear();
            for (const auto& r : rings) {
                for (std::size_t i = 0; i < r.size(); ++i) {
                    const P2& a = r[i];
                    const P2& b = r[(i + 1) % r.size()];
                    if ((a.y <= y && b.y > y) || (b.y <= y && a.y > y)) xs.push_back(a.x + (y - a.y) / (b.y - a.y) * (b.x - a.x));
                }
            }
            std::sort(xs.begin(), xs.end());
            for (std::size_t k = 0; k + 1 < xs.size(); k += 2) {
                const int i0 = std::max(0, static_cast<int>(std::ceil((xs[k] - ox) / cell)));
                const int i1 = std::min(W - 1, static_cast<int>(std::floor((xs[k + 1] - ox) / cell)));
                for (int i = i0; i <= i1; ++i) inside[static_cast<std::size_t>(j) * W + i] = 1;
            }
        }
    }
    std::vector<double> out(N);
    for (std::size_t n = 0; n < N; ++n) out[n] = inside[n] ? -dist[n] : dist[n];
    return out;
}

// Traces where val crosses zero (positive = inside) back into lon/lat polygons with holes.
Geometry traceField(const FieldGrid& grid, std::vector<double> val) {
    Geometry out;
    out.type = GeomType::Polygon;
    const double ox = grid.ox, oy = grid.oy, cell = grid.cell;
    const int W = grid.W, H = grid.H;
    auto nodeX = [&](int i) { return ox + i * cell; };
    auto nodeY = [&](int j) { return oy + j * cell; };
    for (auto& v : val) {
        if (v == 0) v = -1e-9;
    }
    // marching squares; segments go from an inside->outside crossing to an outside->inside one (inside on the left)
    auto edgeId = [&](int i, int j, bool vertical) { return (static_cast<std::int64_t>(j) * W + i) * 2 + (vertical ? 1 : 0); };
    auto at = [&](int i, int j) { return val[static_cast<std::size_t>(j) * W + i]; };
    struct Seg {
        std::int64_t from, to;
        P2 a;
    };
    std::vector<Seg> segs;
    std::unordered_map<std::int64_t, std::size_t> startAt;
    for (int j = 0; j + 1 < H; ++j) {
        for (int i = 0; i + 1 < W; ++i) {
            const double c[4] = {at(i, j), at(i + 1, j), at(i + 1, j + 1), at(i, j + 1)};
            const int ci[4][2] = {{i, j}, {i + 1, j}, {i + 1, j + 1}, {i, j + 1}};
            const int code = (c[0] > 0) | ((c[1] > 0) << 1) | ((c[2] > 0) << 2) | ((c[3] > 0) << 3);
            if (code == 0 || code == 15) continue;
            struct Cross {
                std::int64_t id;
                P2 p;
                bool exit;  // inside -> outside along the counter-clockwise walk
            };
            Cross cr[4];
            int n = 0;
            for (int e = 0; e < 4; ++e) {
                const int e2 = (e + 1) % 4;
                const bool in0 = c[e] > 0, in1 = c[e2] > 0;
                if (in0 == in1) continue;
                const double t = c[e] / (c[e] - c[e2]);
                const P2 p{nodeX(ci[e][0]) + (nodeX(ci[e2][0]) - nodeX(ci[e][0])) * t, nodeY(ci[e][1]) + (nodeY(ci[e2][1]) - nodeY(ci[e][1])) * t};
                std::int64_t id = 0;
                switch (e) {
                    case 0: id = edgeId(i, j, false); break;
                    case 1: id = edgeId(i + 1, j, true); break;
                    case 2: id = edgeId(i, j + 1, false); break;
                    default: id = edgeId(i, j, true); break;
                }
                cr[n++] = {id, p, in0};
            }
            auto addSeg = [&](const Cross& x, const Cross& to) {
                startAt[x.id] = segs.size();
                segs.push_back({x.id, to.id, x.p});
            };
            if (n == 2) {
                const Cross& x = cr[0].exit ? cr[0] : cr[1];
                const Cross& e = cr[0].exit ? cr[1] : cr[0];
                addSeg(x, e);
            } else if (n == 4) {
                const bool centerIn = (c[0] + c[1] + c[2] + c[3]) / 4 > 0;
                for (int k = 0; k < 4; ++k) {
                    if (!cr[k].exit) continue;
                    // centre inside: pair with the next entry; outside: with the previous one
                    const Cross& e = centerIn ? cr[(k + 1) % 4] : cr[(k + 3) % 4];
                    addSeg(cr[k], e);
                }
            }
        }
    }
    std::vector<char> used(segs.size(), 0);
    std::vector<std::vector<P2>> rings;
    for (std::size_t s = 0; s < segs.size(); ++s) {
        if (used[s]) continue;
        std::vector<P2> ring;
        std::size_t cur = s;
        while (!used[cur]) {
            used[cur] = 1;
            ring.push_back(segs[cur].a);
            auto it = startAt.find(segs[cur].to);
            if (it == startAt.end()) break;
            cur = it->second;
        }
        if (ring.size() >= 3) rings.push_back(std::move(ring));
    }
    // tidy: drop near-collinear points, back to lon/lat
    std::vector<Ring> outers, holes;
    const Crs local = grid.crs;
    for (auto& r : rings) {
        Ring planar;
        for (const auto& p : r) planar.push_back({p.x, p.y});
        Ring simple;
        {
            std::vector<P2> pts(r);
            pts.push_back(r.front());
            std::vector<char> keep(pts.size(), 0);
            keep.front() = keep.back() = 1;
            const std::size_t mid = pts.size() / 2;
            keep[mid] = 1;
            dpMark(pts, 0, mid, cell * cell * 0.01, keep);
            dpMark(pts, mid, pts.size() - 1, cell * cell * 0.01, keep);
            for (std::size_t i = 0; i + 1 < pts.size(); ++i) {
                if (keep[i]) simple.push_back({pts[i].x, pts[i].y});
            }
        }
        if (simple.size() < 3) continue;
        const double a = signedArea(simple);
        Ring geo;
        for (const auto& p : simple) {
            Coord q = local.inverse(p);
            geo.push_back({q.x, q.y});
        }
        (a > 0 ? outers : holes).push_back(std::move(geo));
    }
    std::vector<Polygon> polys;
    std::vector<double> areas;
    for (auto& o : outers) {
        areas.push_back(std::abs(signedArea(o)));
        polys.push_back({std::move(o)});
    }
    for (auto& h : holes) {
        int best = -1;
        for (std::size_t k = 0; k < polys.size(); ++k) {
            if (pointInRing(polys[k][0], h[0].x, h[0].y) && (best < 0 || areas[k] < areas[static_cast<std::size_t>(best)])) best = static_cast<int>(k);
        }
        if (best >= 0) polys[static_cast<std::size_t>(best)].push_back(std::move(h));
    }
    out.polygons = std::move(polys);
    if (out.polygons.empty()) out.type = GeomType::None;
    return out;
}

}  // namespace

Geometry bufferUnion(const std::vector<const Geometry*>& gs, double d, int resolution) {
    if (d != 0 && exactFits(gs, 48)) return exactBuffer(gs, d, 48);
    Geometry out;
    out.type = GeomType::Polygon;
    BBox ll;
    bool anyPolygon = false;
    for (const Geometry* g : gs) {
        ll.add(bboxOf(*g));
        anyPolygon = anyPolygon || g->type == GeomType::Polygon;
    }
    if (!ll.valid() || (d <= 0 && !anyPolygon) || d == 0) {
        if (d == 0) {
            for (const Geometry* g : gs) {
                for (const auto& p : g->polygons) out.polygons.push_back(p);
            }
        }
        if (out.polygons.empty()) out.type = GeomType::None;
        return out;
    }
    const Crs crs = Crs::localTm((ll.minX + ll.maxX) / 2);
    FieldBuilder fb;
    const BBox pb = collectField(gs, crs, fb);
    const double grow = std::max(d, 0.0);
    const int res = std::clamp(resolution, 50, 4000);
    const double extent = std::max(pb.width(), pb.height()) + 2 * grow;
    double cell = extent / res;
    if (cell <= 0) cell = std::abs(d) / 20;
    const FieldGrid grid = makeGrid(crs, pb, grow + 3 * cell, cell);
    std::vector<double> val = signedDistance(fb, grid);
    for (auto& v : val) v = d - v;
    return traceField(grid, std::move(val));
}

Geometry overlay(const std::vector<const Geometry*>& a, const std::vector<const Geometry*>& b, OverlayOp op, int resolution) {
    if (exactFits(a, b)) return exactOverlay(a, b, op);
    BBox ll;
    for (const Geometry* g : a) ll.add(bboxOf(*g));
    for (const Geometry* g : b) ll.add(bboxOf(*g));
    if (!ll.valid()) return {};
    const Crs crs = Crs::localTm((ll.minX + ll.maxX) / 2);
    FieldBuilder fa, fb;
    BBox pb = collectField(a, crs, fa);
    pb.add(collectField(b, crs, fb));
    const double extent = std::max(pb.width(), pb.height());
    if (!(extent > 0)) return {};
    const double cell = extent / std::clamp(resolution, 50, 4000);
    const FieldGrid grid = makeGrid(crs, pb, 3 * cell, cell);
    const std::vector<double> sa = signedDistance(fa, grid), sb = signedDistance(fb, grid);
    std::vector<double> val(sa.size());
    for (std::size_t n = 0; n < val.size(); ++n) {
        const double inA = -sa[n], inB = -sb[n];
        switch (op) {
            case OverlayOp::Intersection: val[n] = std::min(inA, inB); break;
            case OverlayOp::Union: val[n] = std::max(inA, inB); break;
            case OverlayOp::Difference: val[n] = std::min(inA, -inB); break;
            case OverlayOp::SymDifference: val[n] = std::max(std::min(inA, -inB), std::min(inB, -inA)); break;
        }
    }
    return traceField(grid, std::move(val));
}

Geometry buffer(const Geometry& g, double d, int resolution, int circleSegments) {
    if (g.type == GeomType::Point && d > 0 && g.points.size() == 1) {
        Geometry out;
        out.type = GeomType::Polygon;
        out.polygons.push_back(geodesicCircle(g.points[0], d, std::clamp(circleSegments, 8, 720)));
        return out;
    }
    if (d != 0 && exactFits({&g}, circleSegments)) return exactBuffer({&g}, d, circleSegments);
    return bufferUnion({&g}, d, resolution);
}

// ---- nearest --------------------------------------------------------------------------------

Nearest nearestPoint(const Geometry& g, double lon, double lat) {
    Nearest best;
    if (g.empty()) return best;
    if (pointInGeometry(g, lon, lat)) {
        best.distance = 0;
        best.point = {lon, lat};
        return best;
    }
    const Crs crs = Crs::localTm(lon);
    const Coord qp = crs.forward({lon, lat});
    const P2 q{qp.x, qp.y};
    double bestD2 = std::numeric_limits<double>::infinity();
    P2 bestP;
    auto consider = [&](const P2& p) {
        const double d2 = (p.x - q.x) * (p.x - q.x) + (p.y - q.y) * (p.y - q.y);
        if (d2 < bestD2) {
            bestD2 = d2;
            bestP = p;
        }
    };
    auto chain = [&](const std::vector<Coord>& cs, bool closed) {
        const auto pts = projectAll(cs, crs);
        if (pts.size() == 1) consider(pts[0]);
        const std::size_t n = closed ? pts.size() : pts.size() - 1;
        for (std::size_t i = 0; i < n && pts.size() > 1; ++i) {
            P2 c;
            segDist2(q, pts[i], pts[(i + 1) % pts.size()], &c);
            consider(c);
        }
    };
    for (const auto& p : g.points) {
        const Coord c = crs.forward(p);
        consider({c.x, c.y});
    }
    for (const auto& l : g.lines) {
        if (!l.empty()) chain(l, false);
    }
    for (const auto& p : g.polygons) {
        for (const auto& r : p) {
            if (!r.empty()) chain(r, true);
        }
    }
    const Coord ll = crs.inverse({bestP.x, bestP.y});
    best.point = {ll.x, ll.y};
    best.distance = geodesicDistance(lon, lat, ll.x, ll.y);
    return best;
}

// ---- expressions ------------------------------------------------------------------------------

struct Expr::Node {
    enum Op { Lit, Field, Var, Neg, Not, And, Or, Cmp, Arith, In, Contains, StartsWith, Like, IsNull, Call } op = Lit;
    Prop lit;
    std::string name;  // field, variable, comparison or arithmetic operator, function
    bool negate = false;
    std::vector<std::shared_ptr<const Node>> kids;
};

namespace {

using NodeP = std::shared_ptr<const Expr::Node>;
using Node = Expr::Node;

struct Tok {
    enum Kind { End, Num, Str, Ident, Var, Sym } kind = End;
    std::string text;
    double num = 0;
    std::size_t pos = 0;
};

std::vector<Tok> tokenize(const std::string& s) {
    std::vector<Tok> out;
    std::size_t i = 0;
    while (i < s.size()) {
        const char c = s[i];
        if (std::isspace(static_cast<unsigned char>(c))) {
            ++i;
            continue;
        }
        Tok t;
        t.pos = i;
        if (std::isdigit(static_cast<unsigned char>(c)) || (c == '.' && i + 1 < s.size() && std::isdigit(static_cast<unsigned char>(s[i + 1])))) {
            char* end = nullptr;
            t.kind = Tok::Num;
            t.num = std::strtod(s.c_str() + i, &end);
            i = static_cast<std::size_t>(end - s.c_str());
        } else if (c == '\'' || c == '"') {
            t.kind = Tok::Str;
            ++i;
            while (i < s.size()) {
                if (s[i] == c) {
                    if (i + 1 < s.size() && s[i + 1] == c) {  // '' escapes a quote
                        t.text += c;
                        i += 2;
                        continue;
                    }
                    break;
                }
                if (s[i] == '\\' && i + 1 < s.size()) ++i;
                t.text += s[i++];
            }
            if (i >= s.size()) throw std::runtime_error("unterminated string starting at position " + std::to_string(t.pos + 1));
            ++i;
        } else if (c == '`' || c == '[') {
            const char close = c == '`' ? '`' : ']';
            t.kind = Tok::Ident;
            const auto e = s.find(close, i + 1);
            if (e == std::string::npos) throw std::runtime_error("unterminated field name at position " + std::to_string(i + 1));
            t.text = s.substr(i + 1, e - i - 1);
            i = e + 1;
        } else if (std::isalpha(static_cast<unsigned char>(c)) || c == '_' || c == '$' || static_cast<unsigned char>(c) >= 0x80) {
            t.kind = c == '$' ? Tok::Var : Tok::Ident;
            std::size_t j = i + 1;
            while (j < s.size() && (std::isalnum(static_cast<unsigned char>(s[j])) || s[j] == '_' || s[j] == '.' || static_cast<unsigned char>(s[j]) >= 0x80)) ++j;
            t.text = s.substr(c == '$' ? i + 1 : i, j - (c == '$' ? i + 1 : i));
            i = j;
        } else {
            t.kind = Tok::Sym;
            static const char* two[] = {"==", "!=", "<>", "<=", ">=", "&&", "||"};
            bool matched = false;
            for (const char* op : two) {
                if (s.compare(i, 2, op) == 0) {
                    t.text = op;
                    i += 2;
                    matched = true;
                    break;
                }
            }
            if (!matched) {
                if (std::string("=<>+-*/%(),!").find(c) == std::string::npos)
                    throw std::runtime_error(std::string("unexpected '") + c + "' at position " + std::to_string(i + 1));
                t.text = std::string(1, c);
                ++i;
            }
        }
        out.push_back(std::move(t));
    }
    Tok end;
    end.pos = s.size();
    out.push_back(end);
    return out;
}

std::string lowerStr(std::string s) {
    for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

class Parser {
public:
    Parser(std::vector<Tok> toks, std::vector<std::string>& fields) : t_(std::move(toks)), fields_(fields) {}

    NodeP parse() {
        NodeP n = orExpr();
        if (peek().kind != Tok::End) fail("unexpected '" + peek().text + "'");
        return n;
    }

private:
    std::vector<Tok> t_;
    std::size_t i_ = 0;
    std::vector<std::string>& fields_;

    const Tok& peek() const { return t_[i_]; }
    [[noreturn]] void fail(const std::string& what) const {
        throw std::runtime_error(what + " at position " + std::to_string(peek().pos + 1));
    }
    bool word(const char* w) {
        if (peek().kind == Tok::Ident && lowerStr(peek().text) == w) {
            ++i_;
            return true;
        }
        return false;
    }
    bool isWord(const char* w, std::size_t ahead = 0) const {
        const Tok& t = t_[std::min(i_ + ahead, t_.size() - 1)];
        return t.kind == Tok::Ident && lowerStr(t.text) == w;
    }
    bool sym(const char* s) {
        if (peek().kind == Tok::Sym && peek().text == s) {
            ++i_;
            return true;
        }
        return false;
    }
    static NodeP make(Node::Op op, std::vector<NodeP> kids, std::string name = {}) {
        auto n = std::make_shared<Node>();
        n->op = op;
        n->kids = std::move(kids);
        n->name = std::move(name);
        return n;
    }
    NodeP orExpr() {
        NodeP l = andExpr();
        while (word("or") || sym("||")) l = make(Node::Or, {l, andExpr()});
        return l;
    }
    NodeP andExpr() {
        NodeP l = notExpr();
        while (word("and") || sym("&&")) l = make(Node::And, {l, notExpr()});
        return l;
    }
    NodeP notExpr() {
        if (word("not") || sym("!")) return make(Node::Not, {notExpr()});
        return cmp();
    }
    NodeP cmp() {
        NodeP l = add();
        for (const char* op : {"==", "!=", "<>", "<=", ">=", "=", "<", ">"}) {
            if (sym(op)) {
                std::string o = op;
                if (o == "=") o = "==";
                if (o == "<>") o = "!=";
                return make(Node::Cmp, {l, add()}, o);
            }
        }
        bool negate = false;
        if (isWord("not") && (isWord("in", 1) || isWord("like", 1) || isWord("contains", 1))) {
            ++i_;
            negate = true;
        }
        auto finish = [&](NodeP n) {
            if (!negate) return n;
            return make(Node::Not, {n});
        };
        if (word("in")) {
            if (!sym("(")) fail("expected '(' after in");
            std::vector<NodeP> kids{l};
            if (!sym(")")) {
                do kids.push_back(add());
                while (sym(","));
                if (!sym(")")) fail("expected ')'");
            }
            return finish(make(Node::In, kids));
        }
        if (word("contains")) return finish(make(Node::Contains, {l, add()}));
        if (word("startswith")) return finish(make(Node::StartsWith, {l, add()}));
        if (word("like")) return finish(make(Node::Like, {l, add()}));
        if (word("is")) {
            const bool no = word("not");
            if (!word("null")) fail("expected null after is");
            NodeP n = make(Node::IsNull, {l});
            return no ? make(Node::Not, {n}) : n;
        }
        return l;
    }
    NodeP add() {
        NodeP l = mul();
        while (true) {
            if (sym("+")) l = make(Node::Arith, {l, mul()}, "+");
            else if (sym("-")) l = make(Node::Arith, {l, mul()}, "-");
            else return l;
        }
    }
    NodeP mul() {
        NodeP l = unary();
        while (true) {
            if (sym("*")) l = make(Node::Arith, {l, unary()}, "*");
            else if (sym("/")) l = make(Node::Arith, {l, unary()}, "/");
            else if (sym("%")) l = make(Node::Arith, {l, unary()}, "%");
            else return l;
        }
    }
    NodeP unary() {
        if (sym("-")) return make(Node::Neg, {unary()});
        if (sym("+")) return unary();
        return primary();
    }
    NodeP primary() {
        const Tok t = peek();
        if (t.kind == Tok::Num) {
            ++i_;
            auto n = std::make_shared<Node>();
            n->lit = Prop::number(t.num);
            return n;
        }
        if (t.kind == Tok::Str) {
            ++i_;
            auto n = std::make_shared<Node>();
            n->lit = Prop::string(t.text);
            return n;
        }
        if (t.kind == Tok::Var) {
            ++i_;
            static const std::vector<std::string> vars = {"area", "length", "id", "x", "y", "type", "vertices"};
            const std::string v = lowerStr(t.text);
            if (std::find(vars.begin(), vars.end(), v) == vars.end())
                throw std::runtime_error("unknown variable $" + t.text + " (use $area, $length, $id, $x, $y, $type or $vertices)");
            return make(Node::Var, {}, v);
        }
        if (sym("(")) {
            NodeP n = orExpr();
            if (!sym(")")) fail("expected ')'");
            return n;
        }
        if (t.kind == Tok::Ident) {
            ++i_;
            const std::string w = lowerStr(t.text);
            if (w == "true" || w == "false") {
                auto n = std::make_shared<Node>();
                n->lit = Prop::boolean(w == "true");
                return n;
            }
            if (w == "null") return std::make_shared<Node>();
            if (peek().kind == Tok::Sym && peek().text == "(") {
                static const std::vector<std::string> fns = {"lower", "upper", "len", "abs", "round", "floor", "ceil", "sqrt", "log10", "min", "max",
                                                             "coalesce", "str", "num"};
                if (std::find(fns.begin(), fns.end(), w) == fns.end()) throw std::runtime_error("unknown function " + t.text + "()");
                ++i_;
                std::vector<NodeP> args;
                if (!sym(")")) {
                    do args.push_back(orExpr());
                    while (sym(","));
                    if (!sym(")")) fail("expected ')'");
                }
                return make(Node::Call, args, w);
            }
            if (std::find(fields_.begin(), fields_.end(), t.text) == fields_.end()) fields_.push_back(t.text);
            return make(Node::Field, {}, t.text);
        }
        if (t.kind == Tok::End) fail("expression ends too early");
        fail("unexpected '" + t.text + "'");
    }
};

int compare(const Prop& a, const Prop& b) {
    auto x = a.asNumber(), y = b.asNumber();
    if (x && y && !(a.kind == Prop::String && b.kind == Prop::String && a.str != a.text())) return *x < *y ? -1 : (*x > *y ? 1 : 0);
    const std::string s = a.text(), t = b.text();
    return s < t ? -1 : (s > t ? 1 : 0);
}

bool likeMatch(const std::string& s, const std::string& pat) {
    // % = any run, _ = one character, case-insensitive
    const std::string a = lowerStr(s), p = lowerStr(pat);
    std::size_t si = 0, pi = 0, star = std::string::npos, mark = 0;
    while (si < a.size()) {
        if (pi < p.size() && (p[pi] == '_' || p[pi] == a[si])) {
            ++si;
            ++pi;
        } else if (pi < p.size() && p[pi] == '%') {
            star = pi++;
            mark = si;
        } else if (star != std::string::npos) {
            pi = star + 1;
            si = ++mark;
        } else {
            return false;
        }
    }
    while (pi < p.size() && p[pi] == '%') ++pi;
    return pi == p.size();
}

Prop evalNode(const Node& n, const Feature& f, std::size_t index) {
    auto kid = [&](std::size_t i) { return evalNode(*n.kids[i], f, index); };
    switch (n.op) {
        case Node::Lit: return n.lit;
        case Node::Field: {
            auto it = f.props.find(n.name);
            if (it != f.props.end()) return it->second;
            for (const auto& [k, v] : f.props) {
                if (lowerStr(k) == lowerStr(n.name)) return v;
            }
            return {};
        }
        case Node::Var:
            if (n.name == "area") return Prop::number(area(f.geom));
            if (n.name == "length") return Prop::number(length(f.geom));
            if (n.name == "id") return Prop::number(static_cast<double>(index));
            if (n.name == "x") return Prop::number(centroid(f.geom).x);
            if (n.name == "y") return Prop::number(centroid(f.geom).y);
            if (n.name == "vertices") return Prop::number(static_cast<double>(f.geom.vertexCount()));
            return Prop::string(toString(f.geom.type));
        case Node::Neg: {
            auto v = kid(0).asNumber();
            return v ? Prop::number(-*v) : Prop::null();
        }
        case Node::Not: return Prop::boolean(!kid(0).truthy());
        case Node::And: return Prop::boolean(kid(0).truthy() && kid(1).truthy());
        case Node::Or: return Prop::boolean(kid(0).truthy() || kid(1).truthy());
        case Node::Cmp: {
            const Prop a = kid(0), b = kid(1);
            if (a.isNull() || b.isNull()) {
                if (n.name == "==") return Prop::boolean(a.isNull() && b.isNull());
                if (n.name == "!=") return Prop::boolean(a.isNull() != b.isNull());
                return Prop::boolean(false);
            }
            const int c = compare(a, b);
            if (n.name == "==") return Prop::boolean(c == 0);
            if (n.name == "!=") return Prop::boolean(c != 0);
            if (n.name == "<") return Prop::boolean(c < 0);
            if (n.name == "<=") return Prop::boolean(c <= 0);
            if (n.name == ">") return Prop::boolean(c > 0);
            return Prop::boolean(c >= 0);
        }
        case Node::Arith: {
            const Prop a = kid(0), b = kid(1);
            auto x = a.asNumber(), y = b.asNumber();
            if (n.name == "+" && (!x || !y) && (a.kind == Prop::String || b.kind == Prop::String)) return Prop::string(a.text() + b.text());
            if (!x || !y) return {};
            if (n.name == "+") return Prop::number(*x + *y);
            if (n.name == "-") return Prop::number(*x - *y);
            if (n.name == "*") return Prop::number(*x * *y);
            if (*y == 0) return {};
            if (n.name == "/") return Prop::number(*x / *y);
            return Prop::number(std::fmod(*x, *y));
        }
        case Node::In: {
            const Prop a = kid(0);
            if (a.isNull()) return Prop::boolean(false);
            for (std::size_t i = 1; i < n.kids.size(); ++i) {
                if (compare(a, kid(i)) == 0) return Prop::boolean(true);
            }
            return Prop::boolean(false);
        }
        case Node::Contains: return Prop::boolean(lowerStr(kid(0).text()).find(lowerStr(kid(1).text())) != std::string::npos);
        case Node::StartsWith: return Prop::boolean(lowerStr(kid(0).text()).rfind(lowerStr(kid(1).text()), 0) == 0);
        case Node::Like: return Prop::boolean(likeMatch(kid(0).text(), kid(1).text()));
        case Node::IsNull: return Prop::boolean(kid(0).isNull());
        case Node::Call: {
            std::vector<Prop> a;
            for (std::size_t i = 0; i < n.kids.size(); ++i) a.push_back(kid(i));
            auto numArg = [&](std::size_t i) -> std::optional<double> { return i < a.size() ? a[i].asNumber() : std::nullopt; };
            const std::string& fn = n.name;
            if (fn == "coalesce") {
                for (const auto& v : a) {
                    if (!v.isNull()) return v;
                }
                return {};
            }
            if (a.empty()) return {};
            if (fn == "lower" || fn == "upper") {
                std::string s = a[0].text();
                for (auto& c : s) c = static_cast<char>(fn == "lower" ? std::tolower(static_cast<unsigned char>(c)) : std::toupper(static_cast<unsigned char>(c)));
                return a[0].isNull() ? Prop::null() : Prop::string(s);
            }
            if (fn == "len") return Prop::number(static_cast<double>(a[0].text().size()));
            if (fn == "str") return Prop::string(a[0].text());
            if (fn == "num") {
                auto v = a[0].asNumber();
                return v ? Prop::number(*v) : Prop::null();
            }
            if (fn == "min" || fn == "max") {
                std::optional<double> best;
                for (std::size_t i = 0; i < a.size(); ++i) {
                    auto v = numArg(i);
                    if (v && (!best || (fn == "min" ? *v < *best : *v > *best))) best = v;
                }
                return best ? Prop::number(*best) : Prop::null();
            }
            auto v = numArg(0);
            if (!v) return {};
            if (fn == "abs") return Prop::number(std::abs(*v));
            if (fn == "floor") return Prop::number(std::floor(*v));
            if (fn == "ceil") return Prop::number(std::ceil(*v));
            if (fn == "sqrt") return *v >= 0 ? Prop::number(std::sqrt(*v)) : Prop::null();
            if (fn == "log10") return *v > 0 ? Prop::number(std::log10(*v)) : Prop::null();
            if (fn == "round") {
                const double k = std::pow(10.0, numArg(1).value_or(0));
                return Prop::number(std::round(*v * k) / k);
            }
            return {};
        }
    }
    return {};
}

}  // namespace

Expr Expr::compile(const std::string& text) {
    Expr e;
    e.text_ = text;
    Parser p(tokenize(text), e.fields_);
    e.root_ = p.parse();
    return e;
}

Prop Expr::eval(const Feature& f, std::size_t index) const { return root_ ? evalNode(*root_, f, index) : Prop::boolean(true); }

// ---- tracks ---------------------------------------------------------------------------------

TrackStats trackStats(const std::vector<const Line*>& segments, double hysteresis) {
    TrackStats s;
    double movingDist = 0;
    for (const Line* seg : segments) {
        const Line& l = *seg;
        s.points += l.size();
        double ref = kNaN;
        std::vector<double> cum(l.size(), 0);
        for (std::size_t i = 0; i < l.size(); ++i) {
            const Coord& c = l[i];
            if (!std::isnan(c.z)) {
                s.hasEle = true;
                s.minEle = std::isnan(s.minEle) ? c.z : std::min(s.minEle, c.z);
                s.maxEle = std::isnan(s.maxEle) ? c.z : std::max(s.maxEle, c.z);
                if (std::isnan(ref)) {
                    ref = c.z;
                } else if (c.z - ref >= hysteresis) {
                    s.gain += c.z - ref;
                    ref = c.z;
                } else if (ref - c.z >= hysteresis) {
                    s.loss += ref - c.z;
                    ref = c.z;
                }
            }
            if (!std::isnan(c.t)) {
                s.hasTime = true;
                s.startTime = std::isnan(s.startTime) ? c.t : std::min(s.startTime, c.t);
                s.endTime = std::isnan(s.endTime) ? c.t : std::max(s.endTime, c.t);
            }
            if (i) {
                const double d = geodesicDistance(l[i - 1].x, l[i - 1].y, c.x, c.y);
                cum[i] = cum[i - 1] + d;
                s.distance += d;
                const double dt = c.t - l[i - 1].t;
                if (std::isfinite(dt) && dt > 0 && d / dt > 0.5 && dt < 600) {
                    s.movingTime += dt;
                    movingDist += d;
                }
            }
        }
        // max speed over a short window (+-2 points) to tame GPS jitter
        for (std::size_t i = 0; i < l.size(); ++i) {
            const std::size_t a = i >= 2 ? i - 2 : 0, b = std::min(l.size() - 1, i + 2);
            const double dt = l[b].t - l[a].t;
            if (b > a && std::isfinite(dt) && dt > 0) s.maxSpeed = std::max(s.maxSpeed, (cum[b] - cum[a]) / dt);
        }
    }
    if (s.hasTime) s.duration = s.endTime - s.startTime;
    if (s.duration > 0) s.avgSpeed = s.distance / s.duration;
    if (s.movingTime > 0) s.movingSpeed = movingDist / s.movingTime;
    return s;
}

// ---- heatmap ----------------------------------------------------------------------------------

std::shared_ptr<Grid> heatmap(const std::vector<Feature>& features, const HeatmapOptions& o) {
    const Crs wm = Crs::webMercator();
    struct Pt {
        double x, y, w;
    };
    std::vector<Pt> pts;
    BBox ll;
    // lines are sampled along their length: explicitly, or (spacing < 0) every quarter radius
    double spacing = o.lineSpacing;
    if (spacing < 0) {
        BBox fb;
        for (const auto& f : features) fb.add(bboxOf(f.geom));
        const double ext = fb.valid() ? std::max(geodesicDistance(fb.minX, fb.minY, fb.maxX, fb.minY), geodesicDistance(fb.minX, fb.minY, fb.minX, fb.maxY)) : 0;
        spacing = std::max((o.radius > 0 ? o.radius : std::max(ext / 20, 50.0)) / 4, 1.0);
    }
    for (const auto& f : features) {
        double w = 1;
        if (!o.weightField.empty()) {
            auto it = f.props.find(o.weightField);
            if (it == f.props.end()) continue;
            auto v = it->second.asNumber();
            if (!v) continue;
            w = *v;
        }
        auto addPt = [&](const Coord& c) {
            if (o.bbox && !o.bbox->contains(c.x, c.y)) return;
            const Coord p = wm.forward(c);
            pts.push_back({p.x, p.y, w});
            ll.add(c.x, c.y);
        };
        if (f.geom.type == GeomType::Point) {
            for (const auto& c : f.geom.points) addPt(c);
        } else if (f.geom.type == GeomType::Line && spacing > 0) {
            // tracks: every stretch of line counts, so busy routes glow
            for (const auto& l : f.geom.lines) {
                for (const auto& c : samplePoints(l, spacing)) addPt(c);
            }
        } else if (f.geom.type == GeomType::Polygon && o.lineSpacing > 0) {
            for (const auto& poly : f.geom.polygons) {
                for (const auto& r : poly) {
                    Line closed = r;
                    if (!r.empty()) closed.push_back(r.front());
                    for (const auto& c : samplePoints(closed, o.lineSpacing)) addPt(c);
                }
            }
        } else if (!f.geom.empty()) {
            addPt(centroid(f.geom));
        }
    }
    if (pts.empty()) throw std::runtime_error("no points to make a heatmap from");
    BBox box = o.bbox ? *o.bbox : ll;
    const double midLat = (box.minY + box.maxY) / 2;
    const double scale = 1 / std::cos(std::clamp(midLat, -85.0, 85.0) * kDeg);  // Web Mercator units per ground metre
    double extentM = std::max(geodesicDistance(box.minX, midLat, box.maxX, midLat), geodesicDistance(box.minX, box.minY, box.minX, box.maxY));
    double radius = o.radius > 0 ? o.radius : std::max(extentM / 20, 50.0);
    const double r = radius * scale;
    Coord lo = wm.forward({box.minX, box.minY}), hi = wm.forward({box.maxX, box.maxY});
    if (!o.bbox) {
        lo.x -= r;
        lo.y -= r;
        hi.x += r;
        hi.y += r;
    }
    const int res = std::clamp(o.resolution, 16, 4096);
    const double cell = std::max(hi.x - lo.x, hi.y - lo.y) / res;
    auto g = std::make_shared<Grid>();
    g->width = std::max(1, static_cast<int>(std::ceil((hi.x - lo.x) / cell)));
    g->height = std::max(1, static_cast<int>(std::ceil((hi.y - lo.y) / cell)));
    g->values.assign(static_cast<std::size_t>(g->width) * g->height, 0.0f);
    g->crs = wm;
    g->geo = {cell, 0, lo.x, 0, -cell, lo.y + g->height * cell};
    const int rc = static_cast<int>(std::ceil(r / cell));
    for (const auto& p : pts) {
        const double col = (p.x - g->geo.c) / cell, row = (g->geo.f - p.y) / cell;
        const int c0 = static_cast<int>(std::floor(col)), r0 = static_cast<int>(std::floor(row));
        for (int rr = std::max(0, r0 - rc); rr <= std::min(g->height - 1, r0 + rc); ++rr) {
            for (int cc = std::max(0, c0 - rc); cc <= std::min(g->width - 1, c0 + rc); ++cc) {
                const double dx = (cc + 0.5 - col) * cell, dy = (rr + 0.5 - row) * cell;
                const double u = (dx * dx + dy * dy) / (r * r);
                if (u >= 1) continue;
                g->values[static_cast<std::size_t>(rr) * g->width + cc] += static_cast<float>(p.w * (1 - u) * (1 - u));
            }
        }
    }
    g->updateRange();
    return g;
}

}  // namespace wetwaresuite::maps
