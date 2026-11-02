// Primitives, brush strokes and view rays.
#include <algorithm>
#include <cmath>
#include <limits>
#include <map>
#include <stdexcept>

#include "wetwaresuite/mesh/primitives.hpp"
#include "wetwaresuite/mesh/topology.hpp"
#include "wetwaresuite/sculpt/ops.hpp"

namespace wetwaresuite::sculpt {

using mesh::Mat4;

// ---- primitives ----

std::optional<Shape> parseShape(const std::string& n) {
    if (n == "sphere") return Shape::Sphere;
    if (n == "cube" || n == "box") return Shape::Cube;
    if (n == "plane" || n == "grid") return Shape::Plane;
    return std::nullopt;
}

std::size_t primitiveTriangles(Shape shape, int level) {
    if (level < 0 || level > 12) return std::numeric_limits<std::size_t>::max();
    const std::size_t f = std::size_t{1} << (2 * level);
    return (shape == Shape::Sphere ? 20 : shape == Shape::Cube ? 12 : 2) * f;
}

namespace {

Mesh icosphere(double radius, int level) {
    const double t = (1.0 + std::sqrt(5.0)) / 2.0;
    Mesh m;
    const Vec3 v[12] = {{-1, t, 0}, {1, t, 0}, {-1, -t, 0}, {1, -t, 0}, {0, -1, t}, {0, 1, t},
                        {0, -1, -t}, {0, 1, -t}, {t, 0, -1}, {t, 0, 1}, {-t, 0, -1}, {-t, 0, 1}};
    for (const auto& p : v) m.addVertex(mesh::normalize(p));
    m.indices = {0, 11, 5, 0, 5, 1, 0, 1, 7, 0, 7, 10, 0, 10, 11, 1, 5, 9, 5, 11, 4, 11, 10, 2, 10, 7, 6, 7, 1, 8,
                 3, 9, 4, 3, 4, 2, 3, 2, 6, 3, 6, 8, 3, 8, 9, 4, 9, 5, 2, 4, 11, 6, 2, 10, 8, 6, 7, 9, 8, 1};
    for (int l = 0; l < level; ++l) {
        std::map<std::pair<std::uint32_t, std::uint32_t>, std::uint32_t> mid;
        auto midpoint = [&](std::uint32_t a, std::uint32_t b) {
            auto key = std::minmax(a, b);
            auto it = mid.find(key);
            if (it != mid.end()) return it->second;
            const std::uint32_t i = m.addVertex(mesh::normalize(m.positions[a] + m.positions[b]));
            mid.emplace(key, i);
            return i;
        };
        std::vector<std::uint32_t> next;
        next.reserve(m.indices.size() * 4);
        for (std::size_t f = 0; f < m.triangleCount(); ++f) {
            const std::uint32_t a = m.indices[f * 3], b = m.indices[f * 3 + 1], c = m.indices[f * 3 + 2];
            const std::uint32_t ab = midpoint(a, b), bc = midpoint(b, c), ca = midpoint(c, a);
            next.insert(next.end(), {a, ab, ca, b, bc, ab, c, ca, bc, ab, bc, ca});
        }
        m.indices = std::move(next);
    }
    for (auto& p : m.positions) p *= radius;
    return m;
}

Mesh gridCube(double size, int level) {
    const int n = 1 << level;
    std::vector<double> c(static_cast<std::size_t>(n) + 1);
    for (int i = 0; i <= n; ++i) c[static_cast<std::size_t>(i)] = -size / 2 + size * i / n;
    c.back() = size / 2;
    Mesh m;
    for (int axis = 0; axis < 3; ++axis) {
        const int b = (axis + 1) % 3, cc = (axis + 2) % 3;
        for (int side = 0; side < 2; ++side) {
            const std::uint32_t base = static_cast<std::uint32_t>(m.positions.size());
            for (int j = 0; j <= n; ++j) {
                for (int i = 0; i <= n; ++i) {
                    Vec3 p;
                    p[axis] = side ? size / 2 : -size / 2;
                    p[b] = c[static_cast<std::size_t>(i)];
                    p[cc] = c[static_cast<std::size_t>(j)];
                    m.addVertex(p);
                }
            }
            auto at = [&](int i, int j) { return base + static_cast<std::uint32_t>(j * (n + 1) + i); };
            for (int j = 0; j < n; ++j) {
                for (int i = 0; i < n; ++i) {
                    const std::uint32_t q0 = at(i, j), q1 = at(i + 1, j), q2 = at(i + 1, j + 1), q3 = at(i, j + 1);
                    if (side) {
                        m.addTriangle(q0, q1, q2);
                        m.addTriangle(q0, q2, q3);
                    } else {
                        m.addTriangle(q0, q2, q1);
                        m.addTriangle(q0, q3, q2);
                    }
                }
            }
        }
    }
    mesh::weld(m, 0);
    return m;
}

}  // namespace

Mesh primitive(Shape shape, double size, int level) {
    if (!(size > 0) || !std::isfinite(size)) throw std::invalid_argument("size must be a positive number");
    if (level < 0) throw std::invalid_argument("level can't be negative");
    const std::size_t tris = primitiveTriangles(shape, level);
    if (tris > kMaxTriangles) {
        throw std::invalid_argument("level " + std::to_string(level) + " would make " +
                                    (tris == std::numeric_limits<std::size_t>::max() ? std::string("too many") : std::to_string(tris)) +
                                    " triangles; the limit is " + std::to_string(kMaxTriangles) + ", so use a lower level");
    }
    switch (shape) {
        case Shape::Sphere: return icosphere(size / 2, level);
        case Shape::Cube: return gridCube(size, level);
        case Shape::Plane: {
            const int n = 1 << level;
            Mesh m = mesh::plane(size, size, n, n);
            m.normals.clear();
            mesh::weld(m, 0);
            return m;
        }
    }
    return {};
}

// ---- brush names ----

const std::vector<std::string>& brushNames() {
    static const std::vector<std::string> v = {"draw", "inflate", "smooth", "flatten", "pinch", "grab", "crease"};
    return v;
}

const std::vector<std::string>& falloffNames() {
    static const std::vector<std::string> v = {"smooth", "linear", "constant", "sharp"};
    return v;
}

std::optional<Brush> parseBrush(const std::string& n) {
    if (n == "draw" || n == "clay") return Brush::Draw;
    if (n == "inflate" || n == "blob") return Brush::Inflate;
    if (n == "smooth") return Brush::Smooth;
    if (n == "flatten" || n == "planar") return Brush::Flatten;
    if (n == "pinch") return Brush::Pinch;
    if (n == "grab" || n == "move") return Brush::Grab;
    if (n == "crease") return Brush::Crease;
    return std::nullopt;
}

std::optional<Falloff> parseFalloff(const std::string& n) {
    if (n == "smooth") return Falloff::Smooth;
    if (n == "linear") return Falloff::Linear;
    if (n == "constant") return Falloff::Constant;
    if (n == "sharp") return Falloff::Sharp;
    return std::nullopt;
}

double falloffWeight(Falloff f, double t) {
    if (t >= 1) return 0;
    t = std::max(0.0, t);
    switch (f) {
        case Falloff::Smooth: {
            const double s = 1 - t;
            return s * s * (3 - 2 * s);
        }
        case Falloff::Linear: return 1 - t;
        case Falloff::Constant: return 1;
        case Falloff::Sharp: return (1 - t) * (1 - t);
    }
    return 0;
}

int parseSymmetry(const std::string& text) {
    if (text.empty() || text == "none" || text == "off") return 0;
    int bits = 0;
    for (char c : text) {
        if (c == 'x' || c == 'X') bits |= 1;
        else if (c == 'y' || c == 'Y') bits |= 2;
        else if (c == 'z' || c == 'Z') bits |= 4;
        else if (c != ',' && c != ' ') return -1;
    }
    return bits;
}

// ---- strokes ----

namespace {

Vec3 mirror(Vec3 p, int bits) {
    if (bits & 1) p.x = -p.x;
    if (bits & 2) p.y = -p.y;
    if (bits & 4) p.z = -p.z;
    return p;
}

double maskAt(const Mask& mask, std::size_t v) { return v < mask.size() ? std::clamp(static_cast<double>(mask[v]), 0.0, 1.0) : 0.0; }

class StrokeState {
public:
    StrokeState(Mesh& m, const Mask& mask) : m_(m), mask_(mask) {
        nbr_ = mesh::vertexNeighbors(m);
        vtri_ = mesh::vertexTriangles(m);
        fn_.resize(m.triangleCount());
        for (std::size_t t = 0; t < fn_.size(); ++t) fn_[t] = m.faceNormal(t);
        vn_.resize(m.positions.size());
        for (std::size_t v = 0; v < vn_.size(); ++v) vn_[v] = vertexNormal(v);
        triStamp_.assign(fn_.size(), 0);
        vertStamp_.assign(vn_.size(), 0);
    }

    // Vertices within r of c, with their falloff weights.
    void gather(const Vec3& c, double r, Falloff f, std::vector<std::uint32_t>& idx, std::vector<double>& w) const {
        idx.clear();
        w.clear();
        const double r2 = r * r;
        for (std::size_t v = 0; v < m_.positions.size(); ++v) {
            const Vec3 d = m_.positions[v] - c;
            const double d2 = mesh::dot(d, d);
            if (d2 >= r2) continue;
            const double wt = falloffWeight(f, std::sqrt(d2) / r) * (1 - maskAt(mask_, v));
            if (wt <= 0) continue;
            idx.push_back(static_cast<std::uint32_t>(v));
            w.push_back(wt);
        }
    }

    // Closest surface point near c (searching triangles around vertices within r, else around the nearest vertex).
    Vec3 snap(const Vec3& c, double r) const {
        const double r2 = r * r;
        std::size_t nearest = 0;
        double best = std::numeric_limits<double>::infinity();
        std::vector<std::uint32_t> near;
        for (std::size_t v = 0; v < m_.positions.size(); ++v) {
            const Vec3 d = m_.positions[v] - c;
            const double d2 = mesh::dot(d, d);
            if (d2 < best) { best = d2; nearest = v; }
            if (d2 < r2) near.push_back(static_cast<std::uint32_t>(v));
        }
        if (near.empty()) near.push_back(static_cast<std::uint32_t>(nearest));
        Vec3 out = m_.positions[nearest];
        best = std::numeric_limits<double>::infinity();
        for (auto v : near) {
            for (auto it = vtri_.begin(v); it != vtri_.end(v); ++it) {
                const Vec3 q = mesh::closestPointOnTriangle(c, m_.corner(*it, 0), m_.corner(*it, 1), m_.corner(*it, 2));
                const Vec3 d = q - c;
                const double d2 = mesh::dot(d, d);
                if (d2 < best) { best = d2; out = q; }
            }
        }
        return out;
    }

    const Vec3& normal(std::size_t v) const { return vn_[v]; }

    Vec3 neighbourAverage(std::size_t v) const {
        const std::size_t n = nbr_.count(v);
        if (n == 0) return m_.positions[v];
        Vec3 s;
        for (auto it = nbr_.begin(v); it != nbr_.end(v); ++it) s += m_.positions[*it];
        return s / static_cast<double>(n);
    }

    // Moves vertices and refreshes the normals around them.
    void apply(const std::vector<std::uint32_t>& idx, const std::vector<Vec3>& delta) {
        ++stamp_;
        touchedTris_.clear();
        for (std::size_t i = 0; i < idx.size(); ++i) {
            m_.positions[idx[i]] += delta[i];
            for (auto it = vtri_.begin(idx[i]); it != vtri_.end(idx[i]); ++it) {
                if (triStamp_[*it] != stamp_) {
                    triStamp_[*it] = stamp_;
                    touchedTris_.push_back(*it);
                }
            }
        }
        for (auto t : touchedTris_) fn_[t] = m_.faceNormal(t);
        for (auto t : touchedTris_) {
            for (std::size_t k = 0; k < 3; ++k) {
                const std::uint32_t v = m_.indices[t * 3 + k];
                if (vertStamp_[v] != stamp_) {
                    vertStamp_[v] = stamp_;
                    vn_[v] = vertexNormal(v);
                }
            }
        }
    }

private:
    Vec3 vertexNormal(std::size_t v) const {
        Vec3 n;
        for (auto it = vtri_.begin(v); it != vtri_.end(v); ++it) n += fn_[*it];
        return mesh::normalize(n);
    }

    Mesh& m_;
    const Mask& mask_;
    mesh::Adjacency nbr_, vtri_;
    std::vector<Vec3> fn_, vn_;
    std::vector<std::uint32_t> triStamp_, vertStamp_, touchedTris_;
    std::uint32_t stamp_ = 0;
};

std::vector<int> mirrorSets(int symmetry) {
    std::vector<int> sets;
    for (int b = 0; b < 8; ++b) {
        if ((b & ~symmetry) == 0) sets.push_back(b);
    }
    return sets;
}

// One dab of a non-grab brush at surface point c.
void dab(StrokeState& st, const Mesh& m, const Vec3& c, const BrushSettings& s) {
    std::vector<std::uint32_t> idx;
    std::vector<double> w;
    st.gather(c, s.radius, s.falloff, idx, w);
    if (idx.empty()) return;
    Vec3 areaN, areaC;
    double wsum = 0;
    for (std::size_t i = 0; i < idx.size(); ++i) {
        areaN += st.normal(idx[i]) * w[i];
        areaC += m.positions[idx[i]] * w[i];
        wsum += w[i];
    }
    areaN = mesh::normalize(areaN);
    areaC = areaC / wsum;
    const double sign = s.invert ? -1.0 : 1.0;
    const double step = s.radius * s.strength * 0.15;
    std::vector<Vec3> delta(idx.size());
    for (std::size_t i = 0; i < idx.size(); ++i) {
        const Vec3& p = m.positions[idx[i]];
        const double wi = w[i];
        switch (s.brush) {
            case Brush::Draw: delta[i] = areaN * (wi * step * sign); break;
            case Brush::Inflate: delta[i] = st.normal(idx[i]) * (wi * step * sign); break;
            case Brush::Smooth: delta[i] = (st.neighbourAverage(idx[i]) - p) * std::min(1.0, wi * s.strength); break;
            case Brush::Flatten: delta[i] = areaN * (-mesh::dot(p - areaC, areaN) * std::min(1.0, wi * s.strength)); break;
            case Brush::Pinch: {
                Vec3 v = c - p;
                v -= areaN * mesh::dot(v, areaN);
                delta[i] = v * (std::min(1.0, wi * s.strength) * 0.15 * sign);
                break;
            }
            case Brush::Crease: {
                Vec3 v = c - p;
                v -= areaN * mesh::dot(v, areaN);
                delta[i] = v * (std::min(1.0, wi * s.strength) * 0.1) - areaN * (wi * step * sign);
                break;
            }
            case Brush::Grab: break;
        }
    }
    st.apply(idx, delta);
}

}  // namespace

StrokeStats stroke(Mesh& m, const Mask& mask, const std::vector<Vec3>& path, const BrushSettings& s) {
    if (m.empty()) throw std::invalid_argument("the mesh has no triangles to sculpt");
    if (path.empty()) throw std::invalid_argument("a stroke needs at least one point");
    if (!(s.radius > 0)) throw std::invalid_argument("radius must be positive");
    if (!(s.spacing > 0)) throw std::invalid_argument("spacing must be positive");
    StrokeStats stats;
    const std::vector<Vec3> start = m.positions;
    StrokeState st(m, mask);
    const auto sets = mirrorSets(s.symmetry);

    if (s.brush == Brush::Grab) {
        const Vec3 anchor = st.snap(path.front(), s.radius);
        struct Group {
            int bits;
            std::vector<std::uint32_t> idx;
            std::vector<double> w;
        };
        std::vector<Group> groups;
        for (int b : sets) {
            const Vec3 c = mirror(anchor, b);
            if (b != 0 && mesh::length(c - anchor) < s.radius * 1e-6) continue;
            Group g{b, {}, {}};
            st.gather(c, s.radius, s.falloff, g.idx, g.w);
            for (auto& x : g.w) x *= std::min(1.0, std::max(0.0, s.strength));
            groups.push_back(std::move(g));
        }
        Vec3 prev = anchor;
        for (std::size_t k = 1; k < path.size(); ++k) {
            const Vec3 d = path[k] - prev;
            prev = path[k];
            for (const auto& g : groups) {
                const Vec3 md = mirror(d, g.bits);
                std::vector<Vec3> delta(g.idx.size());
                for (std::size_t i = 0; i < g.idx.size(); ++i) delta[i] = md * g.w[i];
                st.apply(g.idx, delta);
            }
            ++stats.dabs;
        }
    } else {
        // Snap the given points, then lay dabs every spacing * radius along the polyline.
        std::vector<Vec3> pts;
        for (const auto& p : path) pts.push_back(st.snap(p, s.radius));
        const double gap = s.radius * s.spacing;
        // Resample the polyline every `gap` along its length (dense input, e.g. from screen
        // strokes, doesn't pile up extra dabs), always ending on the last point.
        std::vector<Vec3> centres{pts.front()};
        auto pressureAt = [&](std::size_t k) { return k < s.pressure.size() ? std::clamp(s.pressure[k], 0.0, 1.0) : 1.0; };
        std::vector<double> centreP{pressureAt(0)};
        double carry = 0;  // distance since the last dab
        for (std::size_t k = 1; k < pts.size(); ++k) {
            const Vec3 a = pts[k - 1], d = pts[k] - a;
            const double len = mesh::length(d);
            double t = gap - carry;
            while (t <= len) {
                centres.push_back(a + d * (t / len));
                centreP.push_back(pressureAt(k - 1) + (pressureAt(k) - pressureAt(k - 1)) * (len > 0 ? t / len : 1));
                if (centres.size() > kMaxDabs) {
                    throw std::invalid_argument("that stroke needs more than " + std::to_string(kMaxDabs) +
                                                " dabs; use a bigger spacing or radius, or split it into several strokes");
                }
                t += gap;
            }
            carry = len - (t - gap);
        }
        if (pts.size() > 1 && carry > gap * 0.25) centres.push_back(pts.back()), centreP.push_back(pressureAt(pts.size() - 1));
        BrushSettings pressed = s;
        for (std::size_t k = 0; k < centres.size(); ++k) {
            const Vec3 c = k == 0 || k + 1 == centres.size() ? centres[k] : st.snap(centres[k], s.radius);
            pressed.strength = s.strength * centreP[k];
            if (pressed.strength <= 0) continue;
            for (int b : sets) {
                const Vec3 mc = mirror(c, b);
                if (b != 0 && mesh::length(mc - c) < s.radius * 1e-6) continue;
                dab(st, m, mc, pressed);
            }
            ++stats.dabs;
        }
    }
    for (std::size_t v = 0; v < m.positions.size(); ++v) {
        const double d = mesh::length(m.positions[v] - start[v]);
        if (d > 1e-12) {
            ++stats.moved;
            stats.maxOffset = std::max(stats.maxOffset, d);
        }
    }
    return stats;
}

// ---- views ----

namespace {

struct Cam {
    Vec3 forward, right, up;
};

// Same axes as libs/mesh render.cpp.
Cam camera(mesh::View v) {
    Vec3 f, worldUp{0, 0, 1};
    switch (v) {
        case mesh::View::Iso: f = {-1, 1, -1}; break;
        case mesh::View::Front: f = {0, 1, 0}; break;
        case mesh::View::Back: f = {0, -1, 0}; break;
        case mesh::View::Left: f = {1, 0, 0}; break;
        case mesh::View::Right: f = {-1, 0, 0}; break;
        case mesh::View::Top: f = {0, 0, -1}; worldUp = {0, 1, 0}; break;
        case mesh::View::Bottom: f = {0, 0, 1}; worldUp = {0, -1, 0}; break;
    }
    Cam c;
    c.forward = mesh::normalize(f);
    c.right = mesh::normalize(mesh::cross(c.forward, worldUp));
    c.up = mesh::cross(c.right, c.forward);
    return c;
}

// Rotation about the bounds centre that turnedForView applies.
Mat4 turnMatrix(const Mesh& m, const ViewRay& v) {
    const Vec3 c = mesh::bounds(m).center();
    const Cam cam = camera(v.view);
    return Mat4::translate(c) * Mat4::rotateAxis(cam.right, v.pitch) * Mat4::rotateZ(v.yaw) * Mat4::translate(-c);
}

struct ScreenFit {
    Cam cam;
    double k = 1, cx = 0, cy = 0, minD = 0;
};

ScreenFit fit(const Mesh& m, mesh::View view) {
    ScreenFit f;
    f.cam = camera(view);
    double minX = 1e300, maxX = -1e300, minY = 1e300, maxY = -1e300, minD = 1e300;
    for (const auto& p : m.positions) {
        const double x = mesh::dot(p, f.cam.right), y = mesh::dot(p, f.cam.up), d = mesh::dot(p, f.cam.forward);
        minX = std::min(minX, x); maxX = std::max(maxX, x);
        minY = std::min(minY, y); maxY = std::max(maxY, y);
        minD = std::min(minD, d);
    }
    const double ex = std::max(maxX - minX, 1e-12), ey = std::max(maxY - minY, 1e-12);
    const double avail = 1.0 - 2.0 * 0.06;  // RenderOptions::margin default
    f.k = std::min(avail / ex, avail / ey);
    f.cx = (minX + maxX) / 2;
    f.cy = (minY + maxY) / 2;
    f.minD = minD;
    return f;
}

Vec3 screenPoint(const ScreenFit& f, double u, double v) {
    return f.cam.right * ((u - 0.5) / f.k + f.cx) + f.cam.up * ((0.5 - v) / f.k + f.cy);
}

}  // namespace

Mesh turnedForView(const Mesh& m, const ViewRay& v) {
    if (v.yaw == 0 && v.pitch == 0) return m;
    Mesh out = m;
    mesh::transform(out, turnMatrix(m, v));
    return out;
}

std::optional<Vec3> screenToSurface(const Mesh& m, const ViewRay& v, double u, double vv) {
    if (m.empty()) return std::nullopt;
    const Mesh turned = turnedForView(m, v);
    const ScreenFit f = fit(turned, v.view);
    const double diag = mesh::bounds(turned).diagonal();
    const Vec3 o = screenPoint(f, u, vv) + f.cam.forward * (f.minD - diag - 1);
    double best = std::numeric_limits<double>::infinity();
    for (std::size_t t = 0; t < turned.triangleCount(); ++t) {
        const double d = mesh::rayTriangle(o, f.cam.forward, turned.corner(t, 0), turned.corner(t, 1), turned.corner(t, 2));
        if (d >= 0 && d < best) best = d;
    }
    if (!std::isfinite(best)) return std::nullopt;
    const Vec3 hit = o + f.cam.forward * best;
    if (v.yaw == 0 && v.pitch == 0) return hit;
    return turnMatrix(m, v).inverse().point(hit);
}

Vec3 screenToPlane(const Mesh& m, const ViewRay& v, double u, double vv, const Vec3& anchor) {
    const Mesh turned = turnedForView(m, v);
    const ScreenFit f = fit(turned, v.view);
    const Mat4 turn = turnMatrix(m, v);
    const Vec3 a = (v.yaw == 0 && v.pitch == 0) ? anchor : turn.point(anchor);
    Vec3 p = screenPoint(f, u, vv);
    p += f.cam.forward * mesh::dot(a, f.cam.forward);
    return (v.yaw == 0 && v.pitch == 0) ? p : turn.inverse().point(p);
}

}  // namespace wetwaresuite::sculpt
