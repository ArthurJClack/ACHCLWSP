// The flattened world both renderers share: triangles, materials, textures,
// lights, a binned-SAH BVH, shading and the camera.
#include <algorithm>
#include <atomic>
#include <chrono>
#include <map>
#include <stdexcept>
#include <thread>

#include "wetwaresuite/raster/codec.hpp"
#include "internal.hpp"

namespace wetwaresuite::render::detail {

// ---- textures ----

V3 Texture::sample(float u, float v) const {
    if (w <= 0 || h <= 0) return {1, 1, 1};
    float fx = (u - std::floor(u)) * static_cast<float>(w) - 0.5f;
    float fy = (1.0f - (v - std::floor(v))) * static_cast<float>(h) - 0.5f;  // v = 0 is the bottom row
    const float x0f = std::floor(fx), y0f = std::floor(fy);
    const float tx = fx - x0f, ty = fy - y0f;
    auto wrap = [](int i, int n) { return ((i % n) + n) % n; };
    const int x0 = wrap(static_cast<int>(x0f), w), x1 = wrap(static_cast<int>(x0f) + 1, w);
    const int y0 = wrap(static_cast<int>(y0f), h), y1 = wrap(static_cast<int>(y0f) + 1, h);
    auto at = [&](int x, int y) { return px[static_cast<std::size_t>(y) * static_cast<std::size_t>(w) + static_cast<std::size_t>(x)]; };
    return lerp(lerp(at(x0, y0), at(x1, y0), tx), lerp(at(x0, y1), at(x1, y1), tx), ty);
}

// ---- building ----

namespace {

Mat toMat(const Material& m) {
    Mat r;
    r.color = V3(m.color);
    r.roughness = static_cast<float>(std::clamp(m.roughness, 0.0, 1.0));
    r.metallic = static_cast<float>(std::clamp(m.metallic, 0.0, 1.0));
    r.emission = V3(m.emission) * static_cast<float>(std::max(0.0, m.emissionStrength));
    r.transmission = static_cast<float>(std::clamp(m.transmission, 0.0, 1.0));
    r.ior = static_cast<float>(std::clamp(m.ior, 1.0, 3.0));
    r.opacity = static_cast<float>(std::clamp(m.opacity, 0.0, 1.0));
    r.textureScale = static_cast<float>(m.textureScale);
    return r;
}

// Texture coordinates for meshes that have none: spherical for spheres,
// cylindrical sides for cylinders and cones, otherwise each face from its
// closest axis, all in object space so the texture sticks to the object.
void generatedUvs(const Object& o, const mesh::Mesh& m, std::size_t t, float u[3], float v[3]) {
    const mesh::Bounds b = mesh::bounds(m);
    const mesh::Vec3 size = b.size();
    const double longest = std::max({size.x, size.y, size.z, 1e-9});
    const mesh::Vec3 c = b.center();
    const mesh::Vec3 fn = mesh::normalize(m.faceNormal(t));
    const bool round = o.type == "sphere" || ((o.type == "cylinder" || o.type == "cone") && std::fabs(fn.z) < 0.7);
    if (round) {
        for (int k = 0; k < 3; ++k) {
            const mesh::Vec3 d = m.corner(t, k) - c;
            u[k] = static_cast<float>(std::atan2(d.y, d.x) / (2 * mesh::kPi) + 0.5);
            if (o.type == "sphere") {
                const double len = std::max(mesh::length(d), 1e-12);
                v[k] = static_cast<float>(1.0 - std::acos(std::clamp(d.z / len, -1.0, 1.0)) / mesh::kPi);
            } else {
                v[k] = static_cast<float>(size.z > 0 ? (m.corner(t, k).z - b.min.z) / size.z : 0.5);
            }
        }
        const float lo = std::min({u[0], u[1], u[2]}), hi = std::max({u[0], u[1], u[2]});
        if (hi - lo > 0.5f) {
            for (int k = 0; k < 3; ++k) if (u[k] < 0.5f) u[k] += 1.0f;
        }
        return;
    }
    const double ax = std::fabs(fn.x), ay = std::fabs(fn.y), az = std::fabs(fn.z);
    const int a = ax >= ay && ax >= az ? 0 : ay >= az ? 1 : 2;
    const int iu = a == 0 ? 1 : 0, iv = a == 2 ? 1 : 2;
    const bool flip = (a == 0 && fn.x < 0) || (a == 1 && fn.y > 0);
    for (int k = 0; k < 3; ++k) {
        const mesh::Vec3& p = m.corner(t, k);
        double uu = (p[iu] - b.min[iu]) / longest;
        if (flip) uu = (b.max[iu] - p[iu]) / longest;
        u[k] = static_cast<float>(uu);
        v[k] = static_cast<float>((p[iv] - b.min[iv]) / longest);
    }
}

}  // namespace

void World::build(const Scene& scene) {
    sky = scene.sky;
    skyType = sky.type == "color" ? 1 : sky.type == "none" ? 2 : sky.type == "hdri" ? 3 : 0;
    std::map<std::string, int> texIndex;
    mesh::Bounds all;

    // Loads an image once; colour textures go from sRGB to linear, data maps stay as they are.
    auto loadTexture = [&](const std::string& file, bool colour, const std::string& owner) -> int {
        if (file.empty()) return -1;
        const std::string key = (colour ? "c:" : "d:") + file;
        auto it = texIndex.find(key);
        if (it != texIndex.end()) return it->second;
        int index = -1;
        try {
            raster::Image img = raster::load(file);
            Texture tex;
            tex.w = img.width();
            tex.h = img.height();
            tex.px.resize(static_cast<std::size_t>(tex.w) * static_cast<std::size_t>(tex.h));
            float lut[256];
            for (int i = 0; i < 256; ++i) lut[i] = colour ? srgbToLinear(static_cast<float>(i) / 255.0f) : static_cast<float>(i) / 255.0f;
            for (int y = 0; y < tex.h; ++y) {
                for (int x = 0; x < tex.w; ++x) {
                    const std::uint8_t* px = img.pixel(x, y);
                    tex.px[static_cast<std::size_t>(y) * static_cast<std::size_t>(tex.w) + static_cast<std::size_t>(x)] = {lut[px[0]], lut[px[1]], lut[px[2]]};
                }
            }
            index = static_cast<int>(textures.size());
            textures.push_back(std::move(tex));
        } catch (const std::exception& e) {
            warnings.push_back("image for '" + owner + "' not used: " + e.what());
        }
        texIndex.emplace(key, index);
        return index;
    };
    auto makeMat = [&](const Material& m, const std::string& owner) {
        Mat mat = toMat(m);
        mat.texture = loadTexture(m.texture, true, owner);
        mat.normalMap = loadTexture(m.normalMap, false, owner);
        mat.roughnessMap = loadTexture(m.roughnessMap, false, owner);
        mat.normalStrength = static_cast<float>(m.normalStrength);
        return mat;
    };

    if (skyType == 3) {
        int hi = -1;
        try {
            raster::Image img = raster::load(sky.hdri);
            hdri.w = img.width();
            hdri.h = img.height();
            hdri.px.resize(static_cast<std::size_t>(hdri.w) * static_cast<std::size_t>(hdri.h));
            for (int y = 0; y < hdri.h; ++y) {
                for (int x = 0; x < hdri.w; ++x) {
                    const std::uint8_t* px = img.pixel(x, y);
                    // 8-bit panoramas clip the sun; lift the brightest values so it still casts light
                    V3 c{srgbToLinear(px[0] / 255.0f), srgbToLinear(px[1] / 255.0f), srgbToLinear(px[2] / 255.0f)};
                    const float l = maxComponent(c);
                    if (l > 0.9f) c *= 1.0f + (l - 0.9f) * 40.0f;
                    hdri.px[static_cast<std::size_t>(y) * static_cast<std::size_t>(hdri.w) + static_cast<std::size_t>(x)] = c;
                }
            }
            hi = 0;
        } catch (const std::exception& e) {
            warnings.push_back("hdri not used: " + std::string(e.what()));
        }
        if (hi < 0) {
            skyType = 0;
        } else {
            hdriTurn = static_cast<float>(sky.rotation * kPiF / 180.0);
            // average incoming light per axis direction (cosine weighted), for the preview's ambient term
            const V3 axes[6] = {{1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}};
            float wsum[6] = {0, 0, 0, 0, 0, 0};
            for (auto& a : hdriAmbient) a = {0, 0, 0};
            const int sy = 32, sx = 64;
            for (int j = 0; j < sy; ++j) {
                const float theta = (static_cast<float>(j) + 0.5f) / sy * kPiF;
                for (int i = 0; i < sx; ++i) {
                    const float phi = (static_cast<float>(i) + 0.5f) / sx * 2 * kPiF;
                    const V3 d{std::sin(theta) * std::cos(phi), std::sin(theta) * std::sin(phi), std::cos(theta)};
                    const V3 c = skyColor(d);
                    const float solid = std::sin(theta);
                    for (int k = 0; k < 6; ++k) {
                        const float w = std::max(0.0f, dot(d, axes[k])) * solid;
                        hdriAmbient[k] += c * w;
                        wsum[k] += w;
                    }
                }
            }
            for (int k = 0; k < 6; ++k) hdriAmbient[k] = wsum[k] > 0 ? hdriAmbient[k] / wsum[k] : V3{0, 0, 0};
        }
    }

    // non-planes first, so auto-sized ground planes can fit around them
    std::vector<std::pair<const Object*, mesh::Mesh>> meshes;
    mesh::Bounds solids;
    for (const auto& o : scene.objects) {
        if (!o.visible || o.type == "plane") continue;
        mesh::Mesh m = objectSpaceMesh(o, 1);
        mesh::Mesh wm = m;
        mesh::transform(wm, o.matrix());
        solids.merge(mesh::bounds(wm));
        meshes.emplace_back(&o, std::move(m));
    }
    double plane = 100;
    if (!solids.empty()) {
        const double ext = std::max({std::fabs(solids.min.x), std::fabs(solids.max.x), std::fabs(solids.min.y),
                                     std::fabs(solids.max.y), solids.diagonal()});
        plane = std::max(100.0, ext * 60);
    }
    for (const auto& o : scene.objects) {
        if (o.visible && o.type == "plane") meshes.emplace_back(&o, objectSpaceMesh(o, plane));
    }

    for (auto& [obj, m] : meshes) {
        const Object& o = *obj;
        const int mi = static_cast<int>(mats.size());
        mats.push_back(makeMat(o.material, o.name));
        const bool byPart = !o.parts.empty() && o.partOf && o.partOf->size() == m.triangleCount();
        if (byPart) {
            for (const auto& part : o.parts) mats.push_back(makeMat(part, o.name));
        }

        const mesh::Mat4 xf = o.matrix();
        const bool uvs = m.hasUvs();
        const std::size_t nt = m.triangleCount();
        tris.reserve(tris.size() + nt);
        for (std::size_t t = 0; t < nt; ++t) {
            Tri tri;
            tri.material = byPart ? mi + 1 + std::clamp((*o.partOf)[t], 0, static_cast<int>(o.parts.size()) - 1) : mi;
            if (!uvs) generatedUvs(o, m, t, tri.u, tri.v);
            for (int k = 0; k < 3; ++k) {
                const std::uint32_t vi = m.indices[t * 3 + static_cast<std::size_t>(k)];
                const mesh::Vec3 wp = xf.point(m.positions[vi]);
                all.add(wp);
                tri.p[k] = V3(wp);
                tri.n[k] = V3(xf.normal(m.normals[vi]));
                if (uvs) {
                    tri.u[k] = static_cast<float>(m.uvs[vi].x);
                    tri.v[k] = static_cast<float>(m.uvs[vi].y);
                }
            }
            if (length(cross(tri.p[1] - tri.p[0], tri.p[2] - tri.p[0])) <= 0) continue;
            {
                const V3 e1 = tri.p[1] - tri.p[0], e2 = tri.p[2] - tri.p[0];
                const float du1 = tri.u[1] - tri.u[0], dv1 = tri.v[1] - tri.v[0];
                const float du2 = tri.u[2] - tri.u[0], dv2 = tri.v[2] - tri.v[0];
                const float det = du1 * dv2 - du2 * dv1;
                if (std::fabs(det) > 1e-12f) {
                    const float r = 1.0f / det;
                    tri.tangent = (e1 * dv2 - e2 * dv1) * r;
                    tri.bitangent = (e2 * du1 - e1 * du2) * r;
                }
            }
            tris.push_back(tri);
        }
    }
    double reach = 1;
    if (!solids.empty() || !all.empty()) {
        const mesh::Bounds& b = solids.empty() ? all : solids;
        reach = std::max({b.diagonal(), std::fabs(b.min.x), std::fabs(b.min.y), std::fabs(b.min.z), std::fabs(b.max.x),
                          std::fabs(b.max.y), std::fabs(b.max.z), 1e-3});
    }
    eps = static_cast<float>(std::max(1e-5, reach * 2e-5));

    for (const auto& l : scene.lights) {
        LightF f;
        f.type = l.type == "sun" ? 1 : l.type == "area" ? 2 : 0;
        f.position = V3(l.position);
        f.dir = normalize(V3(l.target - l.position));
        if (length(V3(l.target - l.position)) <= 0) f.dir = {0, 0, -1};
        f.color = V3(l.color) * static_cast<float>(std::max(0.0, l.intensity));
        f.radius = static_cast<float>(std::max(0.0, l.radius));
        f.shadows = l.shadows;
        if (f.type == 1) f.cosAngle = std::cos(std::min(f.radius, 45.0f) * kPiF / 180.0f);
        if (f.type == 2) {
            V3 side = cross(f.dir, V3{0, 0, 1});
            if (length(side) < 1e-4f) side = V3{1, 0, 0};
            side = normalize(side);
            const V3 upv = normalize(cross(side, f.dir));
            f.ax = side * static_cast<float>(l.width / 2);
            f.ay = upv * static_cast<float>(l.height / 2);
            f.area = static_cast<float>(std::max(1e-9, l.width * l.height));
        }
        lights.push_back(f);
    }
}

// ---- BVH ----

namespace {

struct Box {
    V3 lo{1e30f, 1e30f, 1e30f}, hi{-1e30f, -1e30f, -1e30f};
    void add(const V3& p) { lo = vmin(lo, p); hi = vmax(hi, p); }
    void add(const Box& b) { lo = vmin(lo, b.lo); hi = vmax(hi, b.hi); }
    float area() const {
        if (hi.x < lo.x) return 0;
        const V3 d = hi - lo;
        return 2 * (d.x * d.y + d.y * d.z + d.z * d.x);
    }
};

}  // namespace

void World::buildBvh() {
    nodes.clear();
    const int n = static_cast<int>(tris.size());
    if (n == 0) return;
    std::vector<Box> boxes(static_cast<std::size_t>(n));
    std::vector<V3> centers(static_cast<std::size_t>(n));
    for (int i = 0; i < n; ++i) {
        Box b;
        for (const auto& p : tris[static_cast<std::size_t>(i)].p) b.add(p);
        boxes[static_cast<std::size_t>(i)] = b;
        centers[static_cast<std::size_t>(i)] = (b.lo + b.hi) * 0.5f;
    }
    std::vector<int> order(static_cast<std::size_t>(n));
    for (int i = 0; i < n; ++i) order[static_cast<std::size_t>(i)] = i;

    nodes.reserve(static_cast<std::size_t>(2 * n));
    nodes.push_back({});
    struct Task {
        int node, first, count, depth;
    };
    std::vector<Task> stack{{0, 0, n, 0}};
    constexpr int kBins = 12;
    while (!stack.empty()) {
        Task task = stack.back();
        stack.pop_back();
        Box bounds, cbounds;
        for (int i = task.first; i < task.first + task.count; ++i) {
            const int t = order[static_cast<std::size_t>(i)];
            bounds.add(boxes[static_cast<std::size_t>(t)]);
            cbounds.add(centers[static_cast<std::size_t>(t)]);
        }
        Node& node = nodes[static_cast<std::size_t>(task.node)];
        node.bmin = bounds.lo;
        node.bmax = bounds.hi;
        auto makeLeaf = [&] {
            Node& nd = nodes[static_cast<std::size_t>(task.node)];
            nd.first = task.first;
            nd.count = task.count;
        };
        if (task.count <= 2 || task.depth > 60) {
            makeLeaf();
            continue;
        }
        const V3 ext = cbounds.hi - cbounds.lo;
        int axis = ext.x >= ext.y && ext.x >= ext.z ? 0 : ext.y >= ext.z ? 1 : 2;
        const float extent = ext[axis];
        if (extent <= 0) {
            makeLeaf();
            continue;
        }
        Box binBox[kBins];
        int binCount[kBins] = {};
        const float lo = cbounds.lo[axis];
        auto binOf = [&](int t) {
            int b = static_cast<int>((centers[static_cast<std::size_t>(t)][axis] - lo) / extent * kBins);
            return std::clamp(b, 0, kBins - 1);
        };
        for (int i = task.first; i < task.first + task.count; ++i) {
            const int t = order[static_cast<std::size_t>(i)];
            const int b = binOf(t);
            binBox[b].add(boxes[static_cast<std::size_t>(t)]);
            ++binCount[b];
        }
        float leftArea[kBins - 1], rightArea[kBins - 1];
        int leftCount[kBins - 1], rightCount[kBins - 1];
        Box acc;
        int cnt = 0;
        for (int i = 0; i < kBins - 1; ++i) {
            acc.add(binBox[i]);
            cnt += binCount[i];
            leftArea[i] = acc.area();
            leftCount[i] = cnt;
        }
        acc = Box{};
        cnt = 0;
        for (int i = kBins - 1; i > 0; --i) {
            acc.add(binBox[i]);
            cnt += binCount[i];
            rightArea[i - 1] = acc.area();
            rightCount[i - 1] = cnt;
        }
        int best = -1;
        float bestCost = 1e30f;
        for (int i = 0; i < kBins - 1; ++i) {
            if (leftCount[i] == 0 || rightCount[i] == 0) continue;
            const float cost = leftArea[i] * static_cast<float>(leftCount[i]) + rightArea[i] * static_cast<float>(rightCount[i]);
            if (cost < bestCost) {
                bestCost = cost;
                best = i;
            }
        }
        const float leafCost = bounds.area() * static_cast<float>(task.count);
        if (best < 0 || (task.count <= 4 && bestCost >= leafCost)) {
            makeLeaf();
            continue;
        }
        auto mid = std::partition(order.begin() + task.first, order.begin() + task.first + task.count,
                                  [&](int t) { return binOf(t) <= best; });
        const int leftN = static_cast<int>(mid - (order.begin() + task.first));
        if (leftN == 0 || leftN == task.count) {
            makeLeaf();
            continue;
        }
        const int left = static_cast<int>(nodes.size());
        nodes.push_back({});
        nodes.push_back({});
        nodes[static_cast<std::size_t>(task.node)].first = left;
        nodes[static_cast<std::size_t>(task.node)].count = 0;
        stack.push_back({left, task.first, leftN, task.depth + 1});
        stack.push_back({left + 1, task.first + leftN, task.count - leftN, task.depth + 1});
    }
    std::vector<Tri> sorted;
    sorted.reserve(tris.size());
    for (int i : order) sorted.push_back(tris[static_cast<std::size_t>(i)]);
    tris = std::move(sorted);
}

namespace {

inline bool slab(const Node& n, const V3& o, const V3& inv, float tmax, float& tnear) {
    float t0 = (n.bmin.x - o.x) * inv.x, t1 = (n.bmax.x - o.x) * inv.x;
    float lo = std::min(t0, t1), hi = std::max(t0, t1);
    t0 = (n.bmin.y - o.y) * inv.y, t1 = (n.bmax.y - o.y) * inv.y;
    lo = std::max(lo, std::min(t0, t1)), hi = std::min(hi, std::max(t0, t1));
    t0 = (n.bmin.z - o.z) * inv.z, t1 = (n.bmax.z - o.z) * inv.z;
    lo = std::max(lo, std::min(t0, t1)), hi = std::min(hi, std::max(t0, t1));
    tnear = lo;
    return hi >= std::max(lo, 0.0f) && lo <= tmax;
}

// Moller-Trumbore. Two-sided.
inline bool hitTri(const Tri& tr, const V3& o, const V3& d, float tmin, float tmax, float& t, float& b1, float& b2) {
    const V3 e1 = tr.p[1] - tr.p[0], e2 = tr.p[2] - tr.p[0];
    const V3 pv = cross(d, e2);
    const float det = dot(e1, pv);
    if (std::fabs(det) < 1e-20f) return false;
    const float inv = 1.0f / det;
    const V3 tv = o - tr.p[0];
    const float u = dot(tv, pv) * inv;
    if (u < 0 || u > 1) return false;
    const V3 qv = cross(tv, e1);
    const float v = dot(d, qv) * inv;
    if (v < 0 || u + v > 1) return false;
    const float tt = dot(e2, qv) * inv;
    if (tt <= tmin || tt >= tmax) return false;
    t = tt, b1 = u, b2 = v;
    return true;
}

inline V3 inverse(const V3& d) {
    auto inv = [](float x) { return std::fabs(x) > 1e-30f ? 1.0f / x : std::copysign(1e30f, x); };
    return {inv(d.x), inv(d.y), inv(d.z)};
}

}  // namespace

bool World::intersect(const V3& o, const V3& d, float tmax, Hit& hit) const {
    if (nodes.empty()) return false;
    const V3 inv = inverse(d);
    int stack[64];
    int sp = 0;
    stack[sp++] = 0;
    bool found = false;
    float best = tmax;
    while (sp > 0) {
        const Node& n = nodes[static_cast<std::size_t>(stack[--sp])];
        float tn;
        if (!slab(n, o, inv, best, tn)) continue;
        if (n.count > 0) {
            for (int i = n.first; i < n.first + n.count; ++i) {
                float t, b1, b2;
                if (hitTri(tris[static_cast<std::size_t>(i)], o, d, 0.0f, best, t, b1, b2)) {
                    best = t;
                    hit = {t, i, b1, b2};
                    found = true;
                }
            }
        } else {
            float ta, tb;
            const bool ha = slab(nodes[static_cast<std::size_t>(n.first)], o, inv, best, ta);
            const bool hb = slab(nodes[static_cast<std::size_t>(n.first + 1)], o, inv, best, tb);
            if (ha && hb) {
                if (sp + 2 > 64) continue;
                if (ta <= tb) {
                    stack[sp++] = n.first + 1;
                    stack[sp++] = n.first;
                } else {
                    stack[sp++] = n.first;
                    stack[sp++] = n.first + 1;
                }
            } else if (ha) {
                stack[sp++] = n.first;
            } else if (hb) {
                stack[sp++] = n.first + 1;
            }
        }
    }
    return found;
}

V3 World::transmittance(const V3& o, const V3& d, float tmax) const {
    V3 T{1, 1, 1};
    if (nodes.empty()) return T;
    const V3 inv = inverse(d);
    int stack[64];
    int sp = 0;
    stack[sp++] = 0;
    while (sp > 0) {
        const Node& n = nodes[static_cast<std::size_t>(stack[--sp])];
        float tn;
        if (!slab(n, o, inv, tmax, tn)) continue;
        if (n.count > 0) {
            for (int i = n.first; i < n.first + n.count; ++i) {
                const Tri& tr = tris[static_cast<std::size_t>(i)];
                float t, b1, b2;
                if (!hitTri(tr, o, d, 0.0f, tmax, t, b1, b2)) continue;
                const Mat& m = mats[static_cast<std::size_t>(tr.material)];
                if (m.opaque()) return {0, 0, 0};
                const V3 filter = V3{1, 1, 1} * (1 - m.opacity) + m.color * (m.opacity * m.transmission * 0.92f);
                T *= filter;
                if (maxComponent(T) < 1e-3f) return {0, 0, 0};
            }
        } else if (sp + 2 <= 64) {
            stack[sp++] = n.first;
            stack[sp++] = n.first + 1;
        }
    }
    return T;
}

Surface World::surface(const Hit& h) const {
    const Tri& tr = tris[static_cast<std::size_t>(h.tri)];
    const float b0 = 1 - h.b1 - h.b2;
    Surface s;
    s.p = tr.p[0] * b0 + tr.p[1] * h.b1 + tr.p[2] * h.b2;
    s.ng = normalize(cross(tr.p[1] - tr.p[0], tr.p[2] - tr.p[0]));
    s.n = normalize(tr.n[0] * b0 + tr.n[1] * h.b1 + tr.n[2] * h.b2);
    if (dot(s.n, s.n) < 0.5f) s.n = s.ng;
    s.u = tr.u[0] * b0 + tr.u[1] * h.b1 + tr.u[2] * h.b2;
    s.v = tr.v[0] * b0 + tr.v[1] * h.b1 + tr.v[2] * h.b2;
    s.mat = &mats[static_cast<std::size_t>(tr.material)];
    s.base = s.mat->color;
    s.rough = s.mat->roughness;
    const float su = s.u * s.mat->textureScale, sv = s.v * s.mat->textureScale;
    if (s.mat->texture >= 0) s.base *= textures[static_cast<std::size_t>(s.mat->texture)].sample(su, sv);
    if (s.mat->roughnessMap >= 0) s.rough = std::clamp(s.rough * textures[static_cast<std::size_t>(s.mat->roughnessMap)].sample(su, sv).y, 0.0f, 1.0f);
    if (s.mat->normalMap >= 0 && dot(tr.tangent, tr.tangent) > 0) {
        const V3 c = textures[static_cast<std::size_t>(s.mat->normalMap)].sample(su, sv);
        const float k = s.mat->normalStrength;
        const V3 tn{(c.x * 2 - 1) * k, (c.y * 2 - 1) * k, std::max(c.z * 2 - 1, 0.05f)};
        V3 T = tr.tangent - s.n * dot(s.n, tr.tangent);
        if (dot(T, T) > 1e-20f) {
            T = normalize(T);
            V3 B = cross(s.n, T);
            if (dot(B, tr.bitangent) < 0) B = -B;
            s.n = normalize(T * tn.x + B * tn.y + s.n * tn.z);
        }
    }
    return s;
}

V3 World::skyColor(const V3& d) const {
    const float k = static_cast<float>(sky.strength);
    if (skyType == 2) return {0, 0, 0};
    if (skyType == 3) {
        // equirectangular: u around z (u = 0.5 looks down +x), v from the bottom (-z) to the top (+z)
        float u = std::atan2(d.y, d.x) - hdriTurn;
        u = 0.5f - u / (2 * kPiF);
        const float v = 0.5f + std::asin(std::clamp(d.z, -1.0f, 1.0f)) / kPiF;
        return hdri.sample(u, v) * k;
    }
    if (skyType == 1) return V3(sky.zenith) * k;
    const V3 z(sky.zenith), h(sky.horizon), g(sky.ground);
    if (d.z >= 0) return lerp(h, z, std::sqrt(std::min(d.z, 1.0f))) * k;
    return lerp(h, g, std::min(1.0f, -d.z * 6)) * k;
}

V3 World::skyAmbient(const V3& n) const {
    const float k = static_cast<float>(sky.strength);
    if (skyType == 2) return {0, 0, 0};
    if (skyType == 3) {
        const V3 a = n.x >= 0 ? hdriAmbient[0] : hdriAmbient[1];
        const V3 b = n.y >= 0 ? hdriAmbient[2] : hdriAmbient[3];
        const V3 c = n.z >= 0 ? hdriAmbient[4] : hdriAmbient[5];
        return (a * (n.x * n.x) + b * (n.y * n.y) + c * (n.z * n.z)) * 1.0f;  // already scaled by strength
    }
    if (skyType == 1) return V3(sky.zenith) * k;
    const V3 up = (V3(sky.zenith) + V3(sky.horizon)) * 0.5f;
    const V3 down = (V3(sky.ground) + V3(sky.horizon)) * 0.5f;
    return lerp(down, up, std::clamp(n.z * 0.5f + 0.5f, 0.0f, 1.0f)) * k;
}

// ---- camera ----

void CameraF::ray(float sx, float sy, V3& origin, V3& dir) const {
    if (ortho) {
        origin = pos + right * (sx * halfH * aspect) + up * (sy * halfH);
        dir = fwd;
    } else {
        origin = pos;
        dir = normalize(fwd + right * (sx * tanHalf * aspect) + up * (sy * tanHalf));
    }
}

CameraF makeCamera(const Camera& c, int width, int height) {
    CameraF f;
    f.pos = V3(c.position);
    V3 fwd = V3(c.target - c.position);
    if (length(fwd) <= 0) throw std::runtime_error("the camera's position and target are the same point");
    f.fwd = normalize(fwd);
    V3 upHint = V3(c.up);
    if (length(upHint) <= 0) upHint = {0, 0, 1};
    V3 right = cross(f.fwd, normalize(upHint));
    if (length(right) < 1e-4f) right = cross(f.fwd, std::fabs(f.fwd.y) < 0.9f ? V3{0, 1, 0} : V3{1, 0, 0});
    f.right = normalize(right);
    f.up = normalize(cross(f.right, f.fwd));
    f.aspect = static_cast<float>(width) / static_cast<float>(std::max(height, 1));
    f.tanHalf = static_cast<float>(std::tan(std::clamp(c.fov, 1.0, 170.0) * mesh::kPi / 360.0));
    f.ortho = c.orthographic;
    f.halfH = static_cast<float>(std::max(c.orthoSize, 1e-9) / 2);
    return f;
}

// ---- shading ----

namespace {

inline float ggxAlpha(float rough) { return std::max(rough * rough, 0.01f); }
constexpr float kMirror = 0.02f;

inline V3 schlick(const V3& f0, float cosTheta) {
    const float m = pow5(1 - std::clamp(cosTheta, 0.0f, 1.0f));
    return f0 + (V3{1, 1, 1} - f0) * m;
}

}  // namespace

V3 evalBrdf(const Surface& s, const V3& n, const V3& wo, const V3& wi) {
    const float nl = dot(n, wi), nv = dot(n, wo);
    if (nl <= 0 || nv <= 0) return {0, 0, 0};
    const Mat& m = *s.mat;
    const V3 diffuse = s.base * ((1 - m.metallic) / kPiF);
    if (s.rough < kMirror) return diffuse;
    const V3 f0 = lerp(V3{0.04f, 0.04f, 0.04f}, s.base, m.metallic);
    const float a = ggxAlpha(s.rough), a2 = a * a;
    const V3 h = normalize(wo + wi);
    const float nh = std::max(dot(n, h), 0.0f), vh = std::max(dot(wo, h), 0.0f);
    const float dd = nh * nh * (a2 - 1) + 1;
    const float D = a2 / (kPiF * dd * dd);
    auto g1 = [&](float x) { return 2 * x / (x + std::sqrt(a2 + (1 - a2) * x * x)); };
    const float G = g1(nv) * g1(nl);
    const V3 F = schlick(f0, vh);
    return diffuse + F * (D * G / (4 * nl * nv));
}

V3 directLight(const World& w, const Surface& s, const V3& n, const V3& wo, Rng* rng, bool shadows) {
    V3 sum{0, 0, 0};
    const V3 origin = s.p + s.ng * (dot(s.ng, wo) >= 0 ? w.eps : -w.eps);
    const bool soft = rng != nullptr;
    for (const auto& L : w.lights) {
        V3 wi, Li;
        float dist = 1e30f;
        if (L.type == 0) {
            V3 lp = L.position;
            if (soft && L.radius > 0) {
                // a random point in the bulb
                V3 r;
                do {
                    r = {rng->uniform() * 2 - 1, rng->uniform() * 2 - 1, rng->uniform() * 2 - 1};
                } while (dot(r, r) > 1);
                lp += r * L.radius;
            }
            const V3 d = lp - s.p;
            const float d2 = std::max(dot(d, d), 1e-12f);
            dist = std::sqrt(d2);
            wi = d / dist;
            Li = L.color / d2;
        } else if (L.type == 1) {
            wi = -L.dir;
            if (soft && L.cosAngle < 1) {
                V3 t, b;
                basis(wi, t, b);
                const float cosT = 1 - rng->uniform() * (1 - L.cosAngle);
                const float sinT = std::sqrt(std::max(0.0f, 1 - cosT * cosT));
                const float phi = 2 * kPiF * rng->uniform();
                wi = normalize(t * (std::cos(phi) * sinT) + b * (std::sin(phi) * sinT) + wi * cosT);
            }
            Li = L.color;
        } else {
            const V3 q = soft ? L.position + L.ax * (rng->uniform() * 2 - 1) + L.ay * (rng->uniform() * 2 - 1) : L.position;
            const V3 d = q - s.p;
            const float d2 = std::max(dot(d, d), 1e-12f);
            dist = std::sqrt(d2);
            wi = d / dist;
            const float cosL = dot(-wi, L.dir);
            if (cosL <= 0) continue;
            Li = L.color * (cosL / d2);  // radiance x area = intensity, so on-axis it matches a point light
        }
        const float nl = dot(n, wi);
        if (nl <= 0 || dot(s.ng, wi) * dot(s.ng, wo) < 0) continue;
        const V3 f = evalBrdf(s, n, wo, wi);
        if (maxComponent(f) <= 0) continue;
        V3 vis{1, 1, 1};
        if (shadows && L.shadows) {
            vis = w.transmittance(origin, wi, dist < 1e29f ? dist * (1 - 1e-4f) - w.eps : 1e30f);
            if (maxComponent(vis) <= 0) continue;
        }
        sum += f * Li * vis * nl;
    }
    return sum;
}

// ---- output ----

float filmic(float x) {
    x = std::max(0.0f, x) * 0.8f;
    return std::clamp(x * (2.51f * x + 0.03f) / (x * (2.43f * x + 0.59f) + 0.14f), 0.0f, 1.0f);
}

std::uint8_t toByte(float linear) {
    return static_cast<std::uint8_t>(std::lround(linearToSrgb(linear) * 255.0f));
}

int threadCount(int requested) {
    if (requested > 0) return std::min(requested, 256);
    const unsigned hc = std::thread::hardware_concurrency();
    return hc == 0 ? 4 : static_cast<int>(std::min(hc, 256u));
}

void parallelRows(int rows, int threads, const std::function<void(int)>& fn) {
    threads = std::max(1, std::min(threads, rows));
    std::atomic<int> next{0};
    auto worker = [&] {
        for (int r = next++; r < rows; r = next++) fn(r);
    };
    if (threads == 1) {
        worker();
        return;
    }
    std::vector<std::thread> pool;
    pool.reserve(static_cast<std::size_t>(threads - 1));
    for (int i = 0; i < threads - 1; ++i) pool.emplace_back(worker);
    worker();
    for (auto& t : pool) t.join();
}

}  // namespace wetwaresuite::render::detail

namespace wetwaresuite::render {

RenderOptions optionsFrom(const Scene& s) {
    RenderOptions o;
    o.mode = s.settings.mode;
    o.width = s.settings.width;
    o.height = s.settings.height;
    o.samples = s.settings.samples;
    o.bounces = s.settings.bounces;
    o.denoise = s.settings.denoise;
    o.exposure = s.settings.exposure;
    return o;
}

raster::Image renderScene(const Scene& scene, const RenderOptions& o, RenderStats* stats) {
    using namespace detail;
    const auto start = std::chrono::steady_clock::now();
    if (o.width < 1 || o.height < 1 || o.width > 16384 || o.height > 16384) throw std::runtime_error("image size must be 1..16384 pixels a side");
    if (o.mode != "path" && o.mode != "preview") throw std::runtime_error("mode is path or preview");
    if (o.samples < 1) throw std::runtime_error("samples must be at least 1");
    if (o.bounces < 0) throw std::runtime_error("bounces can't be negative");

    Camera cam = o.camera ? *o.camera : scene.camera;
    if (!o.camera && scene.camera.autoFit) {
        Scene tmp;
        tmp.objects = scene.objects;
        tmp.settings.width = o.width;
        tmp.settings.height = o.height;
        fitCamera(tmp, cam);
    }
    World world;
    world.build(scene);
    if (o.mode == "path" || o.shadows) world.buildBvh();
    const CameraF cf = makeCamera(cam, o.width, o.height);
    raster::Image img = o.mode == "path" ? pathTrace(world, cf, o) : rasterize(world, cf, o);
    if (stats) {
        stats->seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
        stats->triangles = world.tris.size();
        stats->threads = threadCount(o.threads);
        stats->warnings = world.warnings;
    }
    return img;
}

}  // namespace wetwaresuite::render
