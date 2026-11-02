#include "blend.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <map>
#include <numeric>
#include <stdexcept>

#include "wetwaresuite/mesh/primitives.hpp"
#include "wetwaresuite/mesh/topology.hpp"

namespace wetwaresuite::cad::detail {

using mesh::Mat4;
using mesh::Mesh;
using mesh::Vec2;
using mesh::Vec3;

namespace {

constexpr double kPi = 3.14159265358979323846;

double tolerance(const Mesh& m) { return std::max(mesh::bounds(m).diagonal() * 1e-9, 1e-12); }

Mesh welded(const Mesh& body) {
    Mesh w;
    w.positions = body.positions;
    w.indices = body.indices;
    mesh::weld(w, tolerance(body));
    return w;
}

// Ray parity: is p inside the closed mesh? Two skewed rays must agree, else a third decides.
bool inside(const Mesh& m, const Vec3& p) {
    auto parity = [&](Vec3 dir) {
        dir = mesh::normalize(dir);
        int hits = 0;
        for (std::size_t t = 0; t < m.triangleCount(); ++t) {
            if (mesh::rayTriangle(p, dir, m.corner(t, 0), m.corner(t, 1), m.corner(t, 2)) > 0) ++hits;
        }
        return hits % 2 == 1;
    };
    const bool a = parity({0.5773, 0.5774, 0.5775}), b = parity({-0.4123, 0.8167, -0.4036});
    return a == b ? a : parity({0.7071, -0.3013, 0.6398});
}

struct Segment {
    std::uint32_t a, b;  // welded vertex indices, along the half-edge of the n1 face
    Vec3 n1, n2;
    bool convex;
    double angle;
};

bool same(const Vec3& x, const Vec3& y) { return mesh::dot(x, y) > 1 - 1e-9; }

}  // namespace

std::vector<SharpEdge> sharpEdges(const Mesh& body, double minAngle) {
    const Mesh w = welded(body);
    const std::size_t nt = w.triangleCount();
    std::vector<Vec3> fn(nt);
    for (std::size_t t = 0; t < nt; ++t) fn[t] = mesh::normalize(w.faceNormal(t));
    // half-edge (a, b) -> triangle
    std::map<std::pair<std::uint32_t, std::uint32_t>, std::vector<std::size_t>> half;
    for (std::size_t t = 0; t < nt; ++t) {
        if (mesh::length(w.faceNormal(t)) == 0) continue;
        for (int k = 0; k < 3; ++k) {
            const std::uint32_t a = w.indices[t * 3 + static_cast<std::size_t>(k)], b = w.indices[t * 3 + static_cast<std::size_t>((k + 1) % 3)];
            half[{a, b}].push_back(t);
        }
    }
    const double cosMin = std::cos(minAngle * kPi / 180);
    std::vector<Segment> segs;
    for (const auto& [key, tris] : half) {
        if (key.first > key.second || tris.size() != 1) continue;
        auto other = half.find({key.second, key.first});
        if (other == half.end() || other->second.size() != 1) continue;
        const std::size_t t1 = tris[0], t2 = other->second[0];
        const double c = mesh::dot(fn[t1], fn[t2]);
        if (c > cosMin) continue;
        std::uint32_t opp = 0;
        for (int k = 0; k < 3; ++k) {
            const std::uint32_t v = w.indices[t2 * 3 + static_cast<std::size_t>(k)];
            if (v != key.first && v != key.second) opp = v;
        }
        const bool convex = mesh::dot(fn[t1], w.positions[opp] - w.positions[key.first]) < 0;
        segs.push_back({key.first, key.second, fn[t1], fn[t2], convex, std::acos(std::clamp(c, -1.0, 1.0)) * 180 / kPi});
    }
    // join collinear pieces of one edge (T-junctions from booleans split them)
    std::vector<std::size_t> parent(segs.size());
    std::iota(parent.begin(), parent.end(), 0);
    auto find = [&](std::size_t i) {
        while (parent[i] != i) i = parent[i] = parent[parent[i]];
        return i;
    };
    std::map<std::uint32_t, std::vector<std::size_t>> byVertex;
    for (std::size_t i = 0; i < segs.size(); ++i) {
        byVertex[segs[i].a].push_back(i);
        byVertex[segs[i].b].push_back(i);
    }
    for (const auto& [v, list] : byVertex) {
        if (list.size() != 2) continue;
        const Segment& s = segs[list[0]];
        const Segment& t = segs[list[1]];
        const Vec3 ds = mesh::normalize(w.positions[s.b] - w.positions[s.a]), dt = mesh::normalize(w.positions[t.b] - w.positions[t.a]);
        const bool sameFaces = (same(s.n1, t.n1) && same(s.n2, t.n2)) || (same(s.n1, t.n2) && same(s.n2, t.n1));
        if (sameFaces && std::abs(mesh::dot(ds, dt)) > 1 - 1e-9) parent[find(list[0])] = find(list[1]);
    }
    std::map<std::size_t, SharpEdge> merged;
    for (std::size_t i = 0; i < segs.size(); ++i) {
        const Segment& s = segs[i];
        const Vec3 pa = w.positions[s.a], pb = w.positions[s.b];
        auto it = merged.find(find(i));
        if (it == merged.end()) {
            merged[find(i)] = {pa, pb, s.n1, s.n2, s.convex, s.angle};
            continue;
        }
        SharpEdge& e = it->second;
        const Vec3 d = mesh::normalize(e.b - e.a);
        for (const Vec3& p : {pa, pb}) {
            if (mesh::dot(p - e.a, d) < 0) e.a = p;
            if (mesh::dot(p - e.b, d) > 0) e.b = p;
        }
    }
    std::vector<SharpEdge> out;
    for (auto& [k, e] : merged) out.push_back(e);
    // a stable order: by midpoint
    std::sort(out.begin(), out.end(), [](const SharpEdge& x, const SharpEdge& y) {
        const Vec3 mx = (x.a + x.b) * 0.5, my = (y.a + y.b) * 0.5;
        if (std::abs(mx.z - my.z) > 1e-9) return mx.z < my.z;
        if (std::abs(mx.y - my.y) > 1e-9) return mx.y < my.y;
        return mx.x < my.x;
    });
    return out;
}

std::vector<SharpEdge> pickEdges(const Mesh& body, const std::vector<SharpEdge>& edges, const std::vector<std::string>& names,
                                 const std::vector<Vec3>& near) {
    std::vector<SharpEdge> out;
    if (!near.empty()) {
        std::vector<bool> taken(edges.size(), false);
        for (const Vec3& p : near) {
            double best = 1e300;
            std::size_t pick = edges.size();
            for (std::size_t i = 0; i < edges.size(); ++i) {
                const Vec3 ab = edges[i].b - edges[i].a;
                const double t = std::clamp(mesh::dot(p - edges[i].a, ab) / std::max(mesh::dot(ab, ab), 1e-300), 0.0, 1.0);
                const double dist = mesh::length(edges[i].a + ab * t - p);
                if (dist < best) best = dist, pick = i;
            }
            if (pick < edges.size() && !taken[pick]) {
                taken[pick] = true;
                out.push_back(edges[pick]);
            }
        }
        return out;
    }
    const mesh::Bounds bb = mesh::bounds(body);
    const double tol = std::max(bb.diagonal() * 1e-6, 1e-9);
    for (const auto& e : edges) {
        const Vec3 d = mesh::normalize(e.b - e.a);
        bool keep = false;
        for (const auto& n : names) {
            if (n == "all") keep = true;
            else if (n == "outer" || n == "convex") keep = keep || e.convex;
            else if (n == "inner" || n == "concave") keep = keep || !e.convex;
            else if (n == "top") keep = keep || (std::abs(e.a.z - bb.max.z) < tol && std::abs(e.b.z - bb.max.z) < tol);
            else if (n == "bottom") keep = keep || (std::abs(e.a.z - bb.min.z) < tol && std::abs(e.b.z - bb.min.z) < tol);
            else if (n == "vertical" || n == "z") keep = keep || std::abs(d.z) > 1 - 1e-9;
            else if (n == "x") keep = keep || std::abs(d.x) > 1 - 1e-9;
            else if (n == "y") keep = keep || std::abs(d.y) > 1 - 1e-9;
            else if (n == "horizontal") keep = keep || std::abs(d.z) < 1e-9;
            else throw std::invalid_argument("unknown edge choice '" + n + "' (all, outer, inner, top, bottom, vertical, horizontal, x, y, z)");
        }
        if (keep) out.push_back(e);
    }
    return out;
}

namespace {

// The cutter's cross-section across one edge: a 2D ring in the frame (ex, ey) at the edge.
struct Section {
    Vec3 ex, ey;
    std::vector<Vec2> ring;
    bool ok = false;
};

Section sectionFor(const Vec3& d, const Vec3& n1, const Vec3& n2, bool convex, double size, bool chamfer, int arcPoints, double margin) {
    Section out;
    const double s = convex ? 1 : -1;
    Vec3 u1 = mesh::normalize(mesh::cross(d, n1)), u2 = mesh::normalize(mesh::cross(d, n2));
    if (mesh::dot(u1, n2) * s > 0) u1 = -u1;  // along face 1, away from the edge
    if (mesh::dot(u2, n1) * s > 0) u2 = -u2;
    const double alpha = std::acos(std::clamp(mesh::dot(u1, u2), -1.0, 1.0));  // the wedge between the faces
    if (alpha < 1e-3 || alpha > kPi - 1e-3) return out;
    out.ex = u1;
    out.ey = mesh::normalize(u2 - u1 * mesh::dot(u1, u2));
    const Vec2 b2{std::cos(alpha / 2), std::sin(alpha / 2)};
    const Vec2 w1{0, -1}, w2{-std::sin(alpha), std::cos(alpha)};  // away from the wedge
    const double t = chamfer ? size : size / std::tan(alpha / 2);
    const Vec2 t1{t, 0}, t2{t * std::cos(alpha), t * std::sin(alpha)};
    out.ring.push_back(t1);
    if (!chamfer) {
        const Vec2 c = b2 * (size / std::sin(alpha / 2));
        const double p1 = std::atan2(t1.y - c.y, t1.x - c.x);
        double sweep = std::atan2(t2.y - c.y, t2.x - c.x) - p1;
        while (sweep > kPi) sweep -= 2 * kPi;
        while (sweep < -kPi) sweep += 2 * kPi;
        for (int k = 1; k < arcPoints; ++k) {
            const double a = p1 + sweep * k / arcPoints;
            out.ring.push_back({c.x + size * std::cos(a), c.y + size * std::sin(a)});
        }
    }
    out.ring.push_back(t2);
    out.ring.push_back(t2 + w2 * margin);
    out.ring.push_back(b2 * (-2 * margin));
    out.ring.push_back(t1 + w1 * margin);
    out.ok = true;
    return out;
}

// Selected edges joined end to end into runs that turn gently (a round rim is one run).
struct Chain {
    std::vector<SharpEdge> edges;  // oriented along the run
    bool closed = false;
};

std::vector<Chain> chains(const std::vector<SharpEdge>& edges, double tol) {
    const std::size_t n = edges.size();
    auto near = [&](const Vec3& a, const Vec3& b) { return mesh::length(a - b) < tol; };
    auto dir = [](const SharpEdge& e) { return mesh::normalize(e.b - e.a); };
    // ends[i][0] = neighbour at a, ends[i][1] = neighbour at b (or n)
    std::vector<std::array<std::size_t, 2>> link(n, {n, n});
    for (std::size_t i = 0; i < n; ++i) {
        for (int end = 0; end < 2; ++end) {
            const Vec3 p = end == 0 ? edges[i].a : edges[i].b;
            std::vector<std::size_t> touching;
            for (std::size_t j = 0; j < n; ++j) {
                if (j != i && (near(edges[j].a, p) || near(edges[j].b, p))) touching.push_back(j);
            }
            if (touching.size() != 1 || edges[touching[0]].convex != edges[i].convex) continue;
            const std::size_t j = touching[0];
            // directions leaving p along each edge
            const Vec3 di = end == 0 ? dir(edges[i]) : -dir(edges[i]);
            const Vec3 dj = near(edges[j].a, p) ? dir(edges[j]) : -dir(edges[j]);
            if (mesh::dot(di, dj) < -std::cos(kPi / 3)) link[i][static_cast<std::size_t>(end)] = j;  // turns less than 60 degrees
        }
    }
    // keep only links both sides agree on
    for (std::size_t i = 0; i < n; ++i) {
        for (auto& l : link[i]) {
            if (l < n && link[l][0] != i && link[l][1] != i) l = n;
        }
    }
    std::vector<bool> used(n, false);
    std::vector<Chain> out;
    auto walk = [&](std::size_t first, int fromEnd) {
        Chain c;
        std::size_t cur = first;
        int enter = fromEnd;  // the end of cur we came in through
        while (cur < n && !used[cur]) {
            used[cur] = true;
            SharpEdge e = edges[cur];
            if (enter == 1) {  // walk it backwards: swapping the ends swaps which face is on which side
                std::swap(e.a, e.b);
                std::swap(e.n1, e.n2);
            }
            c.edges.push_back(e);
            const std::size_t next = link[cur][enter == 0 ? 1 : 0];
            if (next >= n) break;
            if (next == first) {
                c.closed = true;
                break;
            }
            enter = near(edges[next].a, e.b) ? 0 : 1;
            cur = next;
        }
        return c;
    };
    for (std::size_t i = 0; i < n; ++i) {  // open runs start at a free end
        if (used[i]) continue;
        if (link[i][0] >= n) out.push_back(walk(i, 0));
        else if (link[i][1] >= n) out.push_back(walk(i, 1));
    }
    for (std::size_t i = 0; i < n; ++i) {  // what's left are loops
        if (!used[i]) out.push_back(walk(i, 0));
    }
    return out;
}

// Sweeps the cross-section along a run as one closed solid (mitred at the joints).
Mesh sweepChain(const Chain& c, const Mesh& body, double size, bool chamfer, int segments, double margin) {
    const std::size_t k = c.edges.size();
    // arc points from the sharpest wedge in the run, so every section has the same count
    int arcPoints = 2;
    for (const auto& e : c.edges) {
        const double turn = kPi - std::acos(std::clamp(mesh::dot(e.n1, e.n2), -1.0, 1.0));  // the arc's sweep is pi minus the wedge
        arcPoints = std::max(arcPoints, static_cast<int>(std::ceil(segments * (kPi - turn) / (kPi / 2))));
    }
    struct Placed {
        Vec3 p;
        Vec3 d;
        std::vector<Vec3> pts;
    };
    std::vector<Placed> sections;
    const std::size_t count = c.closed ? k : k + 1;
    for (std::size_t j = 0; j < count; ++j) {
        const SharpEdge& in = c.edges[(j + k - 1) % k];
        const SharpEdge& out = c.edges[j % k];
        const bool start = !c.closed && j == 0, end = !c.closed && j == k;
        const SharpEdge& only = start ? c.edges[0] : c.edges[k - 1];
        const Vec3 da = mesh::normalize(in.b - in.a), db = mesh::normalize(out.b - out.a);
        Vec3 d, n1, n2, p;
        if (start || end) {
            d = mesh::normalize(only.b - only.a);
            n1 = only.n1;
            n2 = only.n2;
            p = start ? only.a : only.b;
        } else {
            d = mesh::normalize(da + db);
            n1 = mesh::normalize(in.n1 + out.n1);
            n2 = mesh::normalize(in.n2 + out.n2);
            p = out.a;
        }
        // keep the averaged normals square to the run
        n1 = mesh::normalize(n1 - d * mesh::dot(n1, d));
        n2 = mesh::normalize(n2 - d * mesh::dot(n2, d));
        Section sec = sectionFor(d, n1, n2, only.convex, size, chamfer, arcPoints, margin);
        if (!sec.ok) return {};
        Placed pl{p, d, {}};
        double stretch = 1;
        Vec3 kdir;
        if (!start && !end) {
            const double cosHalf = std::sqrt(std::max(0.0, (1 + mesh::dot(da, db)) / 2));
            if (cosHalf > 1e-3) stretch = 1 / cosHalf;
            kdir = db - da;
            kdir = mesh::length(kdir) > 1e-12 ? mesh::normalize(kdir - d * mesh::dot(kdir, d)) : Vec3{};
        }
        for (const Vec2& q : sec.ring) {
            Vec3 v = sec.ex * q.x + sec.ey * q.y;
            v = v + kdir * (mesh::dot(v, kdir) * (stretch - 1));
            pl.pts.push_back(p + v);
        }
        sections.push_back(std::move(pl));
    }
    // ends: run on past an open end only where the body doesn't continue (keeps a boss's cutter out of its base)
    if (!c.closed && c.edges[0].convex) {
        for (int which = 0; which < 2; ++which) {
            Placed& s = which == 0 ? sections.front() : sections.back();
            const Vec3 outward = which == 0 ? -s.d : s.d;
            Vec3 centre;
            for (const auto& q : s.pts) centre = centre + q;
            centre = centre / static_cast<double>(s.pts.size());
            // a point just past the end, a little inside the corner
            const Vec3 into = mesh::normalize(s.pts[0] + s.pts[s.pts.size() - 4] - s.p * 2) * (size * 0.25);
            const double len = mesh::length(c.edges[which == 0 ? 0 : k - 1].b - c.edges[which == 0 ? 0 : k - 1].a);
            const double probe = std::min(margin, len) * 0.5;
            if (!inside(body, s.p + outward * probe + into)) {
                for (auto& q : s.pts) q = q + outward * margin;
            }
        }
    }
    // the ring's winding about the direction of travel
    const Placed& s0 = sections[0];
    const std::size_t m = s0.pts.size();
    const Vec3 ex = mesh::normalize(s0.pts[0] - s0.p), ey = mesh::cross(s0.d, ex);
    std::vector<Vec2> flat;
    for (const auto& q : s0.pts) flat.push_back({mesh::dot(q - s0.p, ex), mesh::dot(q - s0.p, ey)});
    double area = 0;
    for (std::size_t i = 0; i < m; ++i) area += mesh::cross(flat[i], flat[(i + 1) % m]);
    const bool reverse = area < 0;
    Mesh tool;
    for (auto& sec : sections) {
        if (reverse) std::reverse(sec.pts.begin(), sec.pts.end());
        for (const auto& q : sec.pts) tool.addVertex(q);
    }
    if (reverse) std::reverse(flat.begin(), flat.end());
    const std::size_t ns = sections.size();
    const std::size_t spans = c.closed ? ns : ns - 1;
    for (std::size_t j = 0; j < spans; ++j) {
        const auto base0 = static_cast<std::uint32_t>(j * m), base1 = static_cast<std::uint32_t>(((j + 1) % ns) * m);
        for (std::size_t i = 0; i < m; ++i) {
            const auto i0 = static_cast<std::uint32_t>(i), i1 = static_cast<std::uint32_t>((i + 1) % m);
            tool.addTriangle(base0 + i0, base0 + i1, base1 + i1);
            tool.addTriangle(base0 + i0, base1 + i1, base1 + i0);
        }
    }
    if (!c.closed) {
        const auto tris = mesh::triangulate(mesh::Polygon2{flat, {}});
        const auto last = static_cast<std::uint32_t>((ns - 1) * m);
        for (std::size_t t = 0; t + 2 < tris.size(); t += 3) {
            tool.addTriangle(tris[t], tris[t + 2], tris[t + 1]);                       // start cap faces back
            tool.addTriangle(last + tris[t], last + tris[t + 1], last + tris[t + 2]);  // end cap faces forward
        }
    }
    return tool;
}


// Where three rounded outer edges meet, trims the corner to a ball: the block
// between the corner and the ball's centre, minus the ball, comes off.
Mesh roundCorners(Mesh body, const std::vector<SharpEdge>& edges, double r, int segments, double tol, double margin, const Csg& csg) {
    std::vector<std::pair<Vec3, std::vector<std::size_t>>> corners;
    for (std::size_t i = 0; i < edges.size(); ++i) {
        if (!edges[i].convex) continue;
        for (const Vec3& p : {edges[i].a, edges[i].b}) {
            bool found = false;
            for (auto& [q, list] : corners) {
                if (mesh::length(q - p) < tol) {
                    list.push_back(i);
                    found = true;
                }
            }
            if (!found) corners.push_back({p, {i}});
        }
    }
    for (const auto& [v, list] : corners) {
        if (list.size() != 3) continue;
        std::vector<Vec3> normals;
        for (std::size_t i : list) {
            for (const Vec3& n : {edges[i].n1, edges[i].n2}) {
                bool dup = false;
                for (const auto& m : normals) dup = dup || same(m, n);
                if (!dup) normals.push_back(n);
            }
        }
        if (normals.size() != 3) continue;
        // centre: r inside all three faces
        const Vec3 &a = normals[0], &b = normals[1], &c = normals[2];
        const double det = mesh::dot(a, mesh::cross(b, c));
        if (std::abs(det) < 1e-6) continue;
        const Vec3 centre = v + (mesh::cross(b, c) * -r + mesh::cross(c, a) * -r + mesh::cross(a, b) * -r) / det;
        Vec3 span[3];
        bool ok = true;
        for (int k = 0; k < 3; ++k) {
            const SharpEdge& e = edges[list[static_cast<std::size_t>(k)]];
            const Vec3 far = mesh::length(e.a - v) < tol ? e.b : e.a;
            const Vec3 dir = mesh::normalize(far - v);
            const double reach = mesh::dot(centre - v, dir);
            if (!(reach > 0) || reach > mesh::length(far - v)) ok = false;
            span[k] = dir * reach;
        }
        if (!ok) continue;
        // the block from the corner along the three edges, pushed out past the faces through the corner
        Vec3 origin = v;
        for (const auto& sp : span) origin = origin - mesh::normalize(sp) * margin;
        Vec3 axes[3];
        for (int k = 0; k < 3; ++k) axes[k] = span[k] + mesh::normalize(span[k]) * margin;
        if (mesh::dot(axes[0], mesh::cross(axes[1], axes[2])) < 0) std::swap(axes[1], axes[2]);
        Mesh block = mesh::box({1, 1, 1}, false);
        Mat4 m;
        for (int row = 0; row < 3; ++row) {
            for (int col = 0; col < 3; ++col) m.at(row, col) = axes[col][row];
            m.at(row, 3) = origin[row];
        }
        mesh::transform(block, m);
        Mesh ball = mesh::sphere(r, std::max(8, segments * 4), std::max(4, segments * 2));
        mesh::transform(ball, Mat4::translate(centre));
        Mesh cutter = csg(block, ball, mesh::BooleanOp::Difference);
        if (cutter.empty()) continue;
        Mesh next = csg(body, cutter, mesh::BooleanOp::Difference);
        if (!next.empty()) body = std::move(next);
    }
    return body;
}

}  // namespace

Mesh blendEdges(const Mesh& body, const std::vector<SharpEdge>& edges, double size, bool chamfer, int segments, const Csg& csg) {
    if (!(size > 0)) throw std::runtime_error(chamfer ? "distance must be positive" : "radius must be positive");
    const mesh::Bounds bb = mesh::bounds(body);
    const double startVolume = mesh::volume(body);
    const double tol = std::max(bb.diagonal() * 1e-7, 1e-9);
    Mesh out = body;
    for (const auto& c : chains(edges, tol)) {
        double margin = 0;
        for (const auto& e : c.edges) {
            const double alpha = std::acos(std::clamp(-mesh::dot(e.n1, e.n2), -1.0, 1.0));
            margin = std::max(margin, chamfer ? size : size / std::tan(std::max(alpha, 1e-3) / 2));
        }
        margin = margin * 0.5 + bb.diagonal() * 1e-4;
        // the round must end on the faces: its tangent lines have to lie on the body
        for (const auto& e : c.edges) {
            if (!e.convex) continue;
            const Vec3 d = mesh::normalize(e.b - e.a), mid = (e.a + e.b) * 0.5;
            const double wedge = std::acos(std::clamp(-mesh::dot(e.n1, e.n2), -1.0, 1.0));
            const double t = chamfer ? size : size / std::tan(std::max(wedge, 1e-3) / 2);
            for (const auto& [n, other] : {std::pair{e.n1, e.n2}, std::pair{e.n2, e.n1}}) {
                Vec3 u = mesh::normalize(mesh::cross(d, n));
                if (mesh::dot(u, other) > 0) u = -u;
                if (!inside(out, mid + u * (t * 0.999) - n * (tol * 100))) {
                    throw std::runtime_error(std::string(chamfer ? "distance " : "radius ") + "is too large for the edge at " +
                                             std::to_string(mid.x) + "," + std::to_string(mid.y) + "," + std::to_string(mid.z));
                }
            }
        }
        Mesh tool = sweepChain(c, out, size, chamfer, segments, margin);
        if (tool.empty()) continue;
        const SharpEdge& e = c.edges[0];
        Mesh next = csg(out, tool, e.convex ? mesh::BooleanOp::Difference : mesh::BooleanOp::Union);
        if (next.empty()) {
            throw std::runtime_error("the edge at " + std::to_string((e.a.x + e.b.x) / 2) + "," + std::to_string((e.a.y + e.b.y) / 2) + "," +
                                     std::to_string((e.a.z + e.b.z) / 2) + " fell apart; try a smaller size");
        }
        out = std::move(next);
    }
    if (!chamfer) out = roundCorners(out, edges, size, segments, tol, bb.diagonal() * 1e-4, csg);
    const double v = mesh::volume(out);
    if (!(v > 0) || (startVolume > 0 && v < startVolume * 0.05)) throw std::runtime_error("the size is too large for these edges");
    return out;
}

bool directionByName(const std::string& name, Vec3& out) {
    static const std::map<std::string, Vec3> dirs = {
        {"top", {0, 0, 1}},  {"bottom", {0, 0, -1}}, {"front", {0, -1, 0}}, {"back", {0, 1, 0}},  {"left", {-1, 0, 0}},
        {"right", {1, 0, 0}}, {"+z", {0, 0, 1}},     {"-z", {0, 0, -1}},    {"+y", {0, 1, 0}},    {"-y", {0, -1, 0}},
        {"+x", {1, 0, 0}},    {"-x", {-1, 0, 0}},    {"z", {0, 0, 1}},      {"y", {0, 1, 0}},     {"x", {1, 0, 0}}};
    auto it = dirs.find(name);
    if (it == dirs.end()) return false;
    out = it->second;
    return true;
}

Mesh shell(const Mesh& body, double thickness, const std::vector<Vec3>& open, const Csg& csg) {
    if (!(thickness > 0)) throw std::runtime_error("thickness must be positive");
    Mesh w = welded(body);
    const std::size_t nt = w.triangleCount(), nv = w.positions.size();
    const mesh::Bounds bb = mesh::bounds(w);
    const double tol = std::max(bb.diagonal() * 1e-6, 1e-9);
    std::vector<Vec3> fn(nt);
    std::vector<bool> isOpen(nt, false);
    std::vector<double> extreme(open.size(), -1e300);
    for (std::size_t i = 0; i < open.size(); ++i) {
        for (const auto& p : w.positions) extreme[i] = std::max(extreme[i], mesh::dot(p, open[i]));
    }
    for (std::size_t t = 0; t < nt; ++t) {
        fn[t] = mesh::normalize(w.faceNormal(t));
        for (std::size_t i = 0; i < open.size(); ++i) {
            if (mesh::dot(fn[t], open[i]) > 1 - 1e-6 && std::abs(mesh::dot(w.corner(t, 0), open[i]) - extreme[i]) < tol) isOpen[t] = true;
        }
    }
    // each vertex moves so every face plane around it moves by -thickness (or +thickness when open)
    std::vector<std::vector<std::pair<Vec3, double>>> planes(nv);
    for (std::size_t t = 0; t < nt; ++t) {
        if (mesh::length(w.faceNormal(t)) == 0) continue;
        const double o = isOpen[t] ? thickness : -thickness;
        for (int k = 0; k < 3; ++k) {
            auto& list = planes[w.indices[t * 3 + static_cast<std::size_t>(k)]];
            bool dup = false;
            for (const auto& [n, off] : list) dup = dup || (mesh::dot(n, fn[t]) > 1 - 1e-7 && off == o);
            if (!dup) list.push_back({fn[t], o});
        }
    }
    Mesh inner = w;
    for (std::size_t v = 0; v < nv; ++v) {
        // least squares with a little damping: exact where 1-3 planes meet, sensible on curved surfaces
        double a[3][3] = {{1e-9, 0, 0}, {0, 1e-9, 0}, {0, 0, 1e-9}}, rhs[3] = {0, 0, 0};
        for (const auto& [n, off] : planes[v]) {
            for (int i = 0; i < 3; ++i) {
                for (int j = 0; j < 3; ++j) a[i][j] += n[i] * n[j];
                rhs[i] += n[i] * off;
            }
        }
        // Cramer's rule
        auto det3 = [](double m[3][3]) {
            return m[0][0] * (m[1][1] * m[2][2] - m[1][2] * m[2][1]) - m[0][1] * (m[1][0] * m[2][2] - m[1][2] * m[2][0]) +
                   m[0][2] * (m[1][0] * m[2][1] - m[1][1] * m[2][0]);
        };
        const double det = det3(a);
        if (std::abs(det) < 1e-300) continue;
        Vec3 dsp;
        for (int c = 0; c < 3; ++c) {
            double m[3][3];
            for (int i = 0; i < 3; ++i) {
                for (int j = 0; j < 3; ++j) m[i][j] = j == c ? rhs[i] : a[i][j];
            }
            dsp[c] = det3(m) / det;
        }
        inner.positions[v] = w.positions[v] + dsp;
    }
    for (std::size_t t = 0; t < nt; ++t) {
        const Vec3 n = inner.faceNormal(t);
        if (mesh::length(w.faceNormal(t)) > 0 && mesh::dot(n, fn[t]) <= 0) {
            throw std::runtime_error("thickness is too large for this shape (the inside surface turns inside out)");
        }
    }
    Mesh out = csg(body, inner, mesh::BooleanOp::Difference);
    const double v0 = mesh::volume(body), v1 = mesh::volume(out);
    if (out.empty() || !(v1 > 0) || v1 > v0 * (1 - 1e-9)) throw std::runtime_error("the shell came out empty; check thickness");
    return out;
}


// ---- offset face (push/pull on any face) ----

namespace {

double pointToTriangle(const Vec3& p, const Vec3& a, const Vec3& b, const Vec3& c) {
    const Vec3 n = mesh::normalize(mesh::cross(b - a, c - a));
    const Vec3 foot = p - n * mesh::dot(p - a, n);
    auto inside = [&](const Vec3& x, const Vec3& y) { return mesh::dot(mesh::cross(y - x, foot - x), n) >= -1e-12; };
    if (inside(a, b) && inside(b, c) && inside(c, a)) return std::fabs(mesh::dot(p - a, n));
    auto seg = [&](const Vec3& x, const Vec3& y) {
        const Vec3 d = y - x;
        const double l2 = mesh::dot(d, d);
        const double t = l2 > 0 ? std::clamp(mesh::dot(p - x, d) / l2, 0.0, 1.0) : 0;
        return mesh::length(p - (x + d * t));
    };
    return std::min({seg(a, b), seg(b, c), seg(c, a)});
}

}  // namespace

Mesh offsetFace(const Mesh& body, const Vec3& at, double distance, const Csg& csg) {
    const std::size_t nt = body.triangleCount();
    if (nt == 0) throw std::runtime_error("the body is empty");
    if (!(std::fabs(distance) > 0)) throw std::runtime_error("distance can't be 0");
    // welded vertex ids, so triangles that share a corner position share an id
    std::map<std::array<double, 3>, std::uint32_t> weld;
    std::vector<Vec3> pos;
    std::vector<std::array<std::uint32_t, 3>> tri(nt);
    for (std::size_t t = 0; t < nt; ++t)
        for (int k = 0; k < 3; ++k) {
            const Vec3& p = body.corner(t, k);
            auto [it, fresh] = weld.try_emplace({p.x, p.y, p.z}, static_cast<std::uint32_t>(pos.size()));
            if (fresh) pos.push_back(p);
            tri[t][static_cast<std::size_t>(k)] = it->second;
        }
    std::vector<Vec3> normal(nt);
    for (std::size_t t = 0; t < nt; ++t) normal[t] = mesh::normalize(body.faceNormal(t));
    std::size_t start = nt;
    double best = 1e300;
    for (std::size_t t = 0; t < nt; ++t) {
        if (mesh::length(body.faceNormal(t)) == 0) continue;
        const double d = pointToTriangle(at, body.corner(t, 0), body.corner(t, 1), body.corner(t, 2));
        if (d < best) best = d, start = t;
    }
    if (start == nt) throw std::runtime_error("no face there");
    {
        const mesh::Bounds b = mesh::bounds(body);
        if (best > 0.05 * b.diagonal() + 1e-9) throw std::runtime_error("no face near that point (cad.pick gives a point on one)");
    }
    // edges -> triangles, then grow the smooth patch
    std::map<std::pair<std::uint32_t, std::uint32_t>, std::vector<std::size_t>> edges;
    for (std::size_t t = 0; t < nt; ++t)
        for (int k = 0; k < 3; ++k) {
            std::uint32_t a = tri[t][static_cast<std::size_t>(k)], b = tri[t][static_cast<std::size_t>((k + 1) % 3)];
            if (b < a) std::swap(a, b);
            edges[{a, b}].push_back(t);
        }
    const double smooth = std::cos(20 * mesh::kPi / 180);
    std::vector<char> in(nt, 0);
    std::vector<std::size_t> todo{start}, region;
    in[start] = 1;
    while (!todo.empty()) {
        const std::size_t t = todo.back();
        todo.pop_back();
        region.push_back(t);
        for (int k = 0; k < 3; ++k) {
            std::uint32_t a = tri[t][static_cast<std::size_t>(k)], b = tri[t][static_cast<std::size_t>((k + 1) % 3)];
            if (b < a) std::swap(a, b);
            for (std::size_t u : edges[{a, b}])
                if (!in[u] && mesh::length(body.faceNormal(u)) > 0 && mesh::dot(normal[u], normal[t]) >= smooth) {
                    in[u] = 1;
                    todo.push_back(u);
                }
        }
    }
    // each patch corner moves along the area-weighted normal of the patch triangles around it
    std::map<std::uint32_t, Vec3> vn;
    for (std::size_t t : region)
        for (int k = 0; k < 3; ++k) vn[tri[t][static_cast<std::size_t>(k)]] = vn[tri[t][static_cast<std::size_t>(k)]] + body.faceNormal(t);
    for (auto& [v, n] : vn) n = mesh::normalize(n);
    // the slab runs from a little inside the face to `distance` out (or from a little outside to
    // `distance` in), so the boolean overlaps instead of meeting face to face
    const mesh::Bounds bb = mesh::bounds(body);
    const double overlap = std::min(std::fabs(distance) * 0.25, 1e-3 * (mesh::length(bb.max - bb.min) + 1));
    const double lo = distance > 0 ? -overlap : distance, hi = distance > 0 ? distance : overlap;
    Mesh slab;
    std::map<std::uint32_t, std::uint32_t> low, high;
    for (const auto& [v, n] : vn) {
        low[v] = slab.addVertex(pos[v] + n * lo);
        high[v] = slab.addVertex(pos[v] + n * hi);
    }
    std::map<std::pair<std::uint32_t, std::uint32_t>, int> uses;
    for (std::size_t t : region) {
        const auto& f = tri[t];
        slab.addTriangle(high[f[0]], high[f[1]], high[f[2]]);
        slab.addTriangle(low[f[0]], low[f[2]], low[f[1]]);
        for (int k = 0; k < 3; ++k) {
            std::uint32_t a = f[static_cast<std::size_t>(k)], b = f[static_cast<std::size_t>((k + 1) % 3)];
            ++uses[{std::min(a, b), std::max(a, b)}];
        }
    }
    for (std::size_t t : region) {  // walls along the patch's outline, facing out
        const auto& f = tri[t];
        for (int k = 0; k < 3; ++k) {
            const std::uint32_t a = f[static_cast<std::size_t>(k)], b = f[static_cast<std::size_t>((k + 1) % 3)];
            if (uses[{std::min(a, b), std::max(a, b)}] != 1) continue;
            slab.addTriangle(low[a], low[b], high[b]);
            slab.addTriangle(low[a], high[b], high[a]);
        }
    }
    Mesh out = csg(body, slab, distance > 0 ? mesh::BooleanOp::Union : mesh::BooleanOp::Difference);
    if (out.empty()) throw std::runtime_error("moving that face by " + std::to_string(distance) + " leaves nothing");
    return out;
}

}  // namespace wetwaresuite::cad::detail
