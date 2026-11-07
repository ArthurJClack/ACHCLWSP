// Texture baking: normal and ambient occlusion maps from a high-poly mesh onto a
// low-poly mesh's UVs. A small BVH over the high mesh answers the rays.
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <random>
#include <stdexcept>
#include <thread>
#include <vector>

#include "wetwaresuite/forge/meshops.hpp"

namespace wetwaresuite::forge {

using mesh::Mesh;
using mesh::Vec2;
using mesh::Vec3;

namespace {

constexpr double kPi = 3.14159265358979323846;

struct Box {
    Vec3 lo{1e300, 1e300, 1e300}, hi{-1e300, -1e300, -1e300};
    void add(const Vec3& p) { lo = mesh::min(lo, p), hi = mesh::max(hi, p); }
    void add(const Box& b) { lo = mesh::min(lo, b.lo), hi = mesh::max(hi, b.hi); }
};

class Bvh {
public:
    explicit Bvh(const Mesh& m) : m_(m) {
        const std::size_t n = m.triangleCount();
        tris_.resize(n);
        for (std::size_t i = 0; i < n; ++i) tris_[i] = static_cast<std::uint32_t>(i);
        centres_.resize(n);
        for (std::size_t i = 0; i < n; ++i) centres_[i] = (m.corner(i, 0) + m.corner(i, 1) + m.corner(i, 2)) / 3.0;
        nodes_.reserve(n * 2 + 1);
        if (n) build(0, n);
    }

    // Nearest hit along the ray within (tmin, tmax): triangle and barycentrics.
    bool hit(const Vec3& o, const Vec3& d, double tmin, double tmax, double& t, std::uint32_t& tri, double& bu, double& bv) const {
        if (nodes_.empty()) return false;
        const Vec3 inv{1 / (d.x == 0 ? 1e-30 : d.x), 1 / (d.y == 0 ? 1e-30 : d.y), 1 / (d.z == 0 ? 1e-30 : d.z)};
        std::uint32_t stack[96];
        int sp = 0;
        stack[sp++] = 0;
        bool found = false;
        while (sp) {
            const Node& nd = nodes_[stack[--sp]];
            if (!slab(nd.box, o, inv, tmin, tmax)) continue;
            if (nd.count) {
                for (std::uint32_t k = 0; k < nd.count; ++k) {
                    const std::uint32_t f = tris_[nd.first + k];
                    double tt, u, v;
                    if (triangle(f, o, d, tt, u, v) && tt > tmin && tt < tmax) {
                        tmax = tt, t = tt, tri = f, bu = u, bv = v, found = true;
                    }
                }
            } else if (sp + 2 <= 96) {
                stack[sp++] = nd.first;
                stack[sp++] = nd.first + 1;
            }
        }
        return found;
    }

private:
    struct Node {
        Box box;
        std::uint32_t first = 0, count = 0;  // leaf: triangles [first, first+count); inner: children first, first+1
    };
    const Mesh& m_;
    std::vector<std::uint32_t> tris_;
    std::vector<Vec3> centres_;
    std::vector<Node> nodes_;

    std::uint32_t build(std::size_t begin, std::size_t end) {
        const auto idx = static_cast<std::uint32_t>(nodes_.size());
        nodes_.push_back({});
        Box box, cb;
        for (std::size_t i = begin; i < end; ++i) {
            for (int k = 0; k < 3; ++k) box.add(m_.corner(tris_[i], k));
            cb.add(centres_[tris_[i]]);
        }
        nodes_[idx].box = box;
        if (end - begin <= 4) {
            nodes_[idx].first = static_cast<std::uint32_t>(begin);
            nodes_[idx].count = static_cast<std::uint32_t>(end - begin);
            return idx;
        }
        const Vec3 ext = cb.hi - cb.lo;
        const int axis = ext.x > ext.y ? (ext.x > ext.z ? 0 : 2) : (ext.y > ext.z ? 1 : 2);
        const std::size_t mid = (begin + end) / 2;
        std::nth_element(tris_.begin() + static_cast<std::ptrdiff_t>(begin), tris_.begin() + static_cast<std::ptrdiff_t>(mid),
                         tris_.begin() + static_cast<std::ptrdiff_t>(end), [&](std::uint32_t a, std::uint32_t b) { return centres_[a][axis] < centres_[b][axis]; });
        // children sit next to each other
        const auto left = static_cast<std::uint32_t>(nodes_.size());
        nodes_.push_back({});
        nodes_.push_back({});
        nodes_[idx].first = left;
        buildInto(left, begin, mid);
        buildInto(left + 1, mid, end);
        return idx;
    }
    void buildInto(std::uint32_t slot, std::size_t begin, std::size_t end) {
        const std::uint32_t made = build(begin, end);
        nodes_[slot] = nodes_[made];
    }

    static bool slab(const Box& b, const Vec3& o, const Vec3& inv, double tmin, double tmax) {
        for (int a = 0; a < 3; ++a) {
            double t0 = (b.lo[a] - o[a]) * inv[a], t1 = (b.hi[a] - o[a]) * inv[a];
            if (t0 > t1) std::swap(t0, t1);
            tmin = std::max(tmin, t0);
            tmax = std::min(tmax, t1);
            if (tmax < tmin) return false;
        }
        return true;
    }
    bool triangle(std::uint32_t f, const Vec3& o, const Vec3& d, double& t, double& u, double& v) const {
        const Vec3 &a = m_.corner(f, 0), &b = m_.corner(f, 1), &c = m_.corner(f, 2);
        const Vec3 e1 = b - a, e2 = c - a, pv = mesh::cross(d, e2);
        const double det = mesh::dot(e1, pv);
        if (std::abs(det) < 1e-14) return false;
        const double id = 1 / det;
        const Vec3 tv = o - a;
        u = mesh::dot(tv, pv) * id;
        if (u < 0 || u > 1) return false;
        const Vec3 qv = mesh::cross(tv, e1);
        v = mesh::dot(d, qv) * id;
        if (v < 0 || u + v > 1) return false;
        t = mesh::dot(e2, qv) * id;
        return true;
    }
};

Vec3 surfaceNormal(const Mesh& m, std::uint32_t f, double u, double v) {
    if (m.hasNormals()) {
        const auto* ix = &m.indices[static_cast<std::size_t>(f) * 3];
        const Vec3 n = m.normals[ix[0]] * (1 - u - v) + m.normals[ix[1]] * u + m.normals[ix[2]] * v;
        if (mesh::length(n) > 1e-12) return mesh::normalize(n);
    }
    return mesh::normalize(m.faceNormal(f));
}

// Per-vertex tangents from the UVs (summed over the faces around each vertex), so
// the normal map reads the same way engines build their tangent frames.
std::vector<Vec3> vertexTangents(const Mesh& m, const std::vector<Vec3>& normals, std::vector<double>& handedness) {
    std::vector<Vec3> tan(m.positions.size()), bit(m.positions.size());
    for (std::size_t f = 0; f < m.triangleCount(); ++f) {
        const auto* ix = &m.indices[f * 3];
        const Vec3 e1 = m.positions[ix[1]] - m.positions[ix[0]], e2 = m.positions[ix[2]] - m.positions[ix[0]];
        const Vec2 d1 = m.uvs[ix[1]] - m.uvs[ix[0]], d2 = m.uvs[ix[2]] - m.uvs[ix[0]];
        const double r = d1.x * d2.y - d2.x * d1.y;
        if (std::abs(r) < 1e-14) continue;
        const Vec3 t = (e1 * d2.y - e2 * d1.y) / r, b = (e2 * d1.x - e1 * d2.x) / r;
        for (int k = 0; k < 3; ++k) tan[ix[k]] += t, bit[ix[k]] += b;
    }
    handedness.assign(m.positions.size(), 1.0);
    for (std::size_t i = 0; i < tan.size(); ++i) {
        const Vec3& n = normals[i];
        Vec3 t = tan[i] - n * mesh::dot(n, tan[i]);  // Gram-Schmidt
        if (mesh::length(t) < 1e-12) {
            t = std::abs(n.x) < 0.9 ? mesh::cross(n, Vec3{1, 0, 0}) : mesh::cross(n, Vec3{0, 1, 0});
        }
        tan[i] = mesh::normalize(t);
        handedness[i] = mesh::dot(mesh::cross(n, tan[i]), bit[i]) < 0 ? -1.0 : 1.0;
    }
    return tan;
}

}  // namespace

BakeResult bakeMaps(const Mesh& low, const Mesh& high, const BakeOptions& o) {
    if (low.empty()) throw std::runtime_error("the low-poly mesh has no triangles");
    if (high.empty()) throw std::runtime_error("the high-poly mesh has no triangles");
    if (!low.hasUvs()) throw std::runtime_error("the low-poly mesh has no UVs: unwrap it first (model.uv_unwrap, or forge.mesh_uv)");
    const int size = std::clamp(o.size, 16, 8192);
    const mesh::Bounds lb = mesh::bounds(low), hb = mesh::bounds(high);
    const double diag = std::max(mesh::length(lb.max - lb.min), mesh::length(hb.max - hb.min));
    const double dist = o.distance > 0 ? o.distance : diag * 0.04;
    const double aoDist = o.aoDistance > 0 ? o.aoDistance : diag * 0.15;

    // smooth normals on the low mesh, if it has none
    std::vector<Vec3> ln(low.positions.size());
    if (low.hasNormals()) {
        ln = low.normals;
    } else {
        for (std::size_t f = 0; f < low.triangleCount(); ++f)
            for (int k = 0; k < 3; ++k) ln[low.indices[f * 3 + static_cast<std::size_t>(k)]] += low.faceNormal(f);
    }
    for (auto& n : ln) n = mesh::length(n) > 1e-14 ? mesh::normalize(n) : Vec3{0, 0, 1};
    std::vector<double> hand;
    const std::vector<Vec3> lt = vertexTangents(low, ln, hand);

    const Bvh bvh(high);
    BakeResult out;
    std::vector<float> nrm(static_cast<std::size_t>(size) * size * 3, 0.0f), occ(static_cast<std::size_t>(size) * size, 1.0f);
    std::vector<std::uint8_t> covered(static_cast<std::size_t>(size) * size, 0);

    // which triangle covers each texel (rasterised in UV space, image v down)
    std::vector<std::int32_t> owner(static_cast<std::size_t>(size) * size, -1);
    std::vector<std::array<float, 2>> bary(owner.size());
    for (std::size_t f = 0; f < low.triangleCount(); ++f) {
        const auto* ix = &low.indices[f * 3];
        Vec2 p[3];
        for (int k = 0; k < 3; ++k) p[k] = {low.uvs[ix[k]].x * size, (1 - low.uvs[ix[k]].y) * size};
        if (std::abs(mesh::cross(p[1] - p[0], p[2] - p[0])) < 1e-12) continue;
        const int x0 = std::max(0, static_cast<int>(std::floor(std::min({p[0].x, p[1].x, p[2].x}))));
        const int x1 = std::min(size - 1, static_cast<int>(std::ceil(std::max({p[0].x, p[1].x, p[2].x}))));
        const int y0 = std::max(0, static_cast<int>(std::floor(std::min({p[0].y, p[1].y, p[2].y}))));
        const int y1 = std::min(size - 1, static_cast<int>(std::ceil(std::max({p[0].y, p[1].y, p[2].y}))));
        for (int y = y0; y <= y1; ++y)
            for (int x = x0; x <= x1; ++x) {
                const Vec2 c{x + 0.5, y + 0.5};
                // barycentrics of corners 1 and 2 (either winding)
                const double b1 = mesh::cross(c - p[0], p[2] - p[0]) / mesh::cross(p[1] - p[0], p[2] - p[0]);
                const double b2 = mesh::cross(p[1] - p[0], c - p[0]) / mesh::cross(p[1] - p[0], p[2] - p[0]);
                const double eps = -1e-6;
                if (b1 < eps || b2 < eps || b1 + b2 > 1 - eps) continue;
                const std::size_t i = static_cast<std::size_t>(y) * size + x;
                owner[i] = static_cast<std::int32_t>(f);
                bary[i] = {static_cast<float>(b1), static_cast<float>(b2)};
            }
    }

    std::atomic<std::size_t> missed{0}, texels{0};
    auto work = [&](int row0, int step) {
        std::mt19937 rng;
        std::uniform_real_distribution<double> uni(0.0, 1.0);
        for (int y = row0; y < size; y += step) {
            // seeded per row, so the bake is the same whatever the number of cores
            rng.seed(static_cast<unsigned>(y * 7919 + 17));
            for (int x = 0; x < size; ++x) {
                const std::size_t i = static_cast<std::size_t>(y) * size + x;
                if (owner[i] < 0) continue;
                const auto f = static_cast<std::size_t>(owner[i]);
                const auto* ix = &low.indices[f * 3];
                const double b1 = bary[i][0], b2 = bary[i][1], b0 = 1 - b1 - b2;
                const Vec3 P = low.positions[ix[0]] * b0 + low.positions[ix[1]] * b1 + low.positions[ix[2]] * b2;
                const Vec3 N = mesh::normalize(ln[ix[0]] * b0 + ln[ix[1]] * b1 + ln[ix[2]] * b2);
                Vec3 T = lt[ix[0]] * b0 + lt[ix[1]] * b1 + lt[ix[2]] * b2;
                T = mesh::normalize(T - N * mesh::dot(N, T));
                const Vec3 B = mesh::cross(N, T) * hand[ix[0]];
                ++texels;
                // the high surface: the nearest crossing along the normal, outside first then inside
                double t = 0, u = 0, v = 0;
                std::uint32_t tri = 0;
                Vec3 Nh = N, Ph = P;
                if (bvh.hit(P + N * dist, -N, 0, dist * 2, t, tri, u, v)) {
                    Ph = P + N * dist - N * t;
                    Nh = surfaceNormal(high, tri, u, v);
                } else {
                    ++missed;
                }
                covered[i] = 1;
                if (o.normal) {
                    Vec3 ts{mesh::dot(Nh, T), mesh::dot(Nh, B), mesh::dot(Nh, N)};
                    if (o.directXGreen) ts.y = -ts.y;
                    nrm[i * 3] = static_cast<float>(ts.x * 0.5 + 0.5);
                    nrm[i * 3 + 1] = static_cast<float>(ts.y * 0.5 + 0.5);
                    nrm[i * 3 + 2] = static_cast<float>(ts.z * 0.5 + 0.5);
                }
                if (o.ao) {
                    // cosine-weighted rays over the hemisphere around the high normal
                    const Vec3 a = std::abs(Nh.x) < 0.9 ? Vec3{1, 0, 0} : Vec3{0, 1, 0};
                    const Vec3 tx = mesh::normalize(mesh::cross(Nh, a)), ty = mesh::cross(Nh, tx);
                    const Vec3 start = Ph + Nh * (diag * 1e-4);
                    int blocked = 0;
                    const int n = std::max(1, o.aoSamples);
                    for (int s = 0; s < n; ++s) {
                        const double r1 = (s + uni(rng)) / n, r2 = uni(rng);
                        const double r = std::sqrt(r1), phi = 2 * kPi * r2;
                        const Vec3 d = tx * (r * std::cos(phi)) + ty * (r * std::sin(phi)) + Nh * std::sqrt(std::max(0.0, 1 - r1));
                        double tt, uu, vv;
                        std::uint32_t ff;
                        if (bvh.hit(start, d, 0, aoDist, tt, ff, uu, vv)) ++blocked;
                    }
                    occ[i] = 1.0f - static_cast<float>(blocked) / static_cast<float>(n);
                }
            }
        }
    };
    const int threads = static_cast<int>(std::clamp(std::thread::hardware_concurrency(), 1u, 16u));
    std::vector<std::thread> pool;
    for (int k = 0; k < threads; ++k) pool.emplace_back(work, k, threads);
    for (auto& th : pool) th.join();
    out.texels = texels;
    out.missed = missed;

    // grow each island's colours outwards so mipmaps and filtering don't pull in the background
    for (int pass = 0; pass < std::max(0, o.padding); ++pass) {
        std::vector<std::uint8_t> next = covered;
        for (int y = 0; y < size; ++y)
            for (int x = 0; x < size; ++x) {
                const std::size_t i = static_cast<std::size_t>(y) * size + x;
                if (covered[i]) continue;
                float sum[4] = {0, 0, 0, 0};
                int cnt = 0;
                for (int dy = -1; dy <= 1; ++dy)
                    for (int dx = -1; dx <= 1; ++dx) {
                        const int nx = x + dx, ny = y + dy;
                        if (nx < 0 || ny < 0 || nx >= size || ny >= size) continue;
                        const std::size_t j = static_cast<std::size_t>(ny) * size + nx;
                        if (!covered[j]) continue;
                        for (int c = 0; c < 3; ++c) sum[c] += nrm[j * 3 + static_cast<std::size_t>(c)];
                        sum[3] += occ[j];
                        ++cnt;
                    }
                if (!cnt) continue;
                for (int c = 0; c < 3; ++c) nrm[i * 3 + static_cast<std::size_t>(c)] = sum[c] / static_cast<float>(cnt);
                occ[i] = sum[3] / static_cast<float>(cnt);
                next[i] = 1;
            }
        covered.swap(next);
    }

    auto byte = [](float v) { return static_cast<std::uint8_t>(std::lround(std::clamp(v, 0.0f, 1.0f) * 255)); };
    if (o.normal) {
        out.normalMap = raster::Image(size, size, {128, 128, 255, 255});
        for (int y = 0; y < size; ++y)
            for (int x = 0; x < size; ++x) {
                const std::size_t i = static_cast<std::size_t>(y) * size + x;
                if (!covered[i]) continue;
                std::uint8_t* px = out.normalMap.pixel(x, y);
                px[0] = byte(nrm[i * 3]), px[1] = byte(nrm[i * 3 + 1]), px[2] = byte(nrm[i * 3 + 2]), px[3] = 255;
            }
    }
    if (o.ao) {
        out.aoMap = raster::Image(size, size, {255, 255, 255, 255});
        for (int y = 0; y < size; ++y)
            for (int x = 0; x < size; ++x) {
                const std::size_t i = static_cast<std::size_t>(y) * size + x;
                if (!covered[i]) continue;
                const std::uint8_t g = byte(occ[i]);
                std::uint8_t* px = out.aoMap.pixel(x, y);
                px[0] = px[1] = px[2] = g;
            }
    }
    return out;
}

}  // namespace wetwaresuite::forge
