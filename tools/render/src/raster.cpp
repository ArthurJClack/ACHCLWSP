// The preview rasterizer: near-plane clipping, perspective-correct z-buffer
// in horizontal bands (one thread each), deferred shading with the same BRDF
// and lights as the path tracer, sky ambient and reflections, optional
// ray-traced shadows, a blended pass for glass and cut-outs, 2x supersampling.
#include <algorithm>
#include <array>

#include "internal.hpp"

namespace wetwaresuite::render::detail {

namespace {

constexpr float kMirror = 0.02f;

struct ClipVert {
    V3 c;           // camera space: x right, y up, z forward
    float b1, b2;   // barycentrics in the original triangle
};

struct ScreenVert {
    float x, y;  // pixels
    float z;     // camera depth
    float iz;    // 1 / z (perspective)
    float b1, b2;
};

V3 shade(const World& w, const Surface& s, const V3& d, bool shadows) {
    const V3 wo = -d;
    const bool entering = dot(s.ng, d) < 0;
    const V3 ng = entering ? s.ng : -s.ng;
    V3 n = dot(s.n, ng) >= 0 ? s.n : -s.n;
    if (dot(n, wo) <= 0) n = ng;
    const Mat& m = *s.mat;
    const float nv = std::max(dot(n, wo), 0.0f);
    const V3 f0 = lerp(V3{0.04f, 0.04f, 0.04f}, s.base, m.metallic);
    const float fr = pow5(1 - nv);
    const V3 F = f0 + (V3{1, 1, 1} - f0) * fr * (1 - s.rough * 0.7f);
    V3 c = m.emission;
    const float diffuseShare = (1 - m.metallic) * (1 - m.transmission);
    c += s.base * w.skyAmbient(n) * diffuseShare;
    const V3 r = reflect(d, n);
    // a little studio fill so metals and glass never go black under a dark sky
    const V3 env = vmax(lerp(w.skyColor(r), w.skyAmbient(r), std::min(1.0f, s.rough * 1.4f)), V3{0.06f, 0.06f, 0.06f});
    c += F * env * (m.transmission > 0 ? 1.0f : (m.metallic > 0 ? 1.0f : 1.0f - s.rough * 0.8f));
    Surface sd = s;
    if (m.transmission > 0) sd.base = sd.base * (1 - m.transmission);
    c += directLight(w, sd, n, wo, nullptr, shadows && !w.nodes.empty());
    if (s.rough < kMirror && m.transmission <= 0) {
        // smooth surfaces show a soft highlight for point-like lights
        for (const auto& L : w.lights) {
            const V3 wi = L.type == 1 ? -L.dir : normalize(L.position - s.p);
            const float sp = std::pow(std::max(0.0f, dot(reflect(-wi, n), wo)), 400.0f);
            const V3 Li = L.type == 1 ? L.color : L.color / std::max(dot(L.position - s.p, L.position - s.p), 1e-6f);
            c += F * Li * sp;
        }
    }
    return c;
}

}  // namespace

raster::Image rasterize(const World& w, const CameraF& cam, const RenderOptions& o) {
    const int ss = o.antialias ? 2 : 1;
    const int W = o.width * ss, H = o.height * ss;
    const std::size_t npx = static_cast<std::size_t>(W) * static_cast<std::size_t>(H);
    const int threads = threadCount(o.threads);
    const bool bgMode = !w.sky.visible || w.sky.transparent;
    const float nearZ = std::max(1e-6f, w.eps * 5);

    // camera-space corners
    const std::size_t nt = w.tris.size();
    std::vector<std::array<V3, 3>> cs(nt);
    std::vector<int> opaque, blended;
    for (std::size_t t = 0; t < nt; ++t) {
        const Tri& tr = w.tris[t];
        for (int k = 0; k < 3; ++k) {
            const V3 d = tr.p[k] - cam.pos;
            cs[t][static_cast<std::size_t>(k)] = {dot(d, cam.right), dot(d, cam.up), dot(d, cam.fwd)};
        }
        (w.mats[static_cast<std::size_t>(tr.material)].opaque() ? opaque : blended).push_back(static_cast<int>(t));
    }

    auto project = [&](const ClipVert& v) {
        ScreenVert s;
        float sx, sy;
        if (cam.ortho) {
            sx = v.c.x / (cam.halfH * cam.aspect);
            sy = v.c.y / cam.halfH;
        } else {
            sx = v.c.x / (v.c.z * cam.tanHalf * cam.aspect);
            sy = v.c.y / (v.c.z * cam.tanHalf);
        }
        s.x = (sx + 1) * 0.5f * static_cast<float>(W);
        s.y = (1 - sy) * 0.5f * static_cast<float>(H);
        s.z = v.c.z;
        s.iz = 1.0f / v.c.z;
        s.b1 = v.b1;
        s.b2 = v.b2;
        return s;
    };

    // Calls fn(pixel index, z, b1, b2) for every covered pixel of triangle t in rows [y0, y1).
    auto rasterTri = [&](int t, int y0, int y1, auto&& fn) {
        const auto& c = cs[static_cast<std::size_t>(t)];
        ClipVert in[3] = {{c[0], 0, 0}, {c[1], 1, 0}, {c[2], 0, 1}};
        ClipVert poly[4];
        int np = 0;
        for (int k = 0; k < 3; ++k) {
            const ClipVert& a = in[k];
            const ClipVert& b = in[(k + 1) % 3];
            const bool ain = a.c.z > nearZ, bin = b.c.z > nearZ;
            if (ain) poly[np++] = a;
            if (ain != bin && np < 4) {
                const float f = (nearZ - a.c.z) / (b.c.z - a.c.z);
                poly[np++] = {lerp(a.c, b.c, f), a.b1 + (b.b1 - a.b1) * f, a.b2 + (b.b2 - a.b2) * f};
            }
        }
        if (np < 3) return;
        ScreenVert sv[4];
        for (int k = 0; k < np; ++k) sv[k] = project(poly[k]);
        for (int f = 1; f + 1 < np; ++f) {
            const ScreenVert& A = sv[0];
            const ScreenVert& B = sv[f];
            const ScreenVert& C = sv[f + 1];
            const float area = (B.x - A.x) * (C.y - A.y) - (B.y - A.y) * (C.x - A.x);
            if (std::fabs(area) < 1e-12f) continue;
            const float minX = std::min({A.x, B.x, C.x}), maxX = std::max({A.x, B.x, C.x});
            const float minY = std::min({A.y, B.y, C.y}), maxY = std::max({A.y, B.y, C.y});
            const int px0 = std::max(0, static_cast<int>(std::floor(minX))), px1 = std::min(W - 1, static_cast<int>(std::ceil(maxX)));
            const int py0 = std::max(y0, static_cast<int>(std::floor(minY))), py1 = std::min(y1 - 1, static_cast<int>(std::ceil(maxY)));
            if (px0 > px1 || py0 > py1) continue;
            const float inv = 1.0f / area;
            for (int py = py0; py <= py1; ++py) {
                const float y = static_cast<float>(py) + 0.5f;
                for (int px = px0; px <= px1; ++px) {
                    const float x = static_cast<float>(px) + 0.5f;
                    const float l0 = ((B.x - x) * (C.y - y) - (B.y - y) * (C.x - x)) * inv;
                    const float l1 = ((C.x - x) * (A.y - y) - (C.y - y) * (A.x - x)) * inv;
                    const float l2 = 1 - l0 - l1;
                    if (l0 < 0 || l1 < 0 || l2 < 0) continue;
                    float z, w0, w1, w2;
                    if (cam.ortho) {
                        z = l0 * A.z + l1 * B.z + l2 * C.z;
                        w0 = l0, w1 = l1, w2 = l2;
                    } else {
                        const float iz = l0 * A.iz + l1 * B.iz + l2 * C.iz;
                        z = 1.0f / iz;
                        w0 = l0 * A.iz * z, w1 = l1 * B.iz * z, w2 = l2 * C.iz * z;
                    }
                    const float b1 = w0 * A.b1 + w1 * B.b1 + w2 * C.b1;
                    const float b2 = w0 * A.b2 + w1 * B.b2 + w2 * C.b2;
                    fn(static_cast<std::size_t>(py) * static_cast<std::size_t>(W) + static_cast<std::size_t>(px), z, b1, b2);
                }
            }
        }
    };

    auto pixelDir = [&](std::size_t i) {
        const int px = static_cast<int>(i % static_cast<std::size_t>(W)), py = static_cast<int>(i / static_cast<std::size_t>(W));
        const float sx = (static_cast<float>(px) + 0.5f) / static_cast<float>(W) * 2 - 1;
        const float sy = 1 - (static_cast<float>(py) + 0.5f) / static_cast<float>(H) * 2;
        V3 ro, rd;
        cam.ray(sx, sy, ro, rd);
        return rd;
    };

    std::vector<float> depth(npx, 1e30f);
    std::vector<Hit> vis(npx);
    std::vector<V3> color(npx);
    std::vector<float> cov(npx, bgMode ? 0.0f : 1.0f);

    const int bandH = std::max(8, (H + threads * 4 - 1) / (threads * 4));
    const int bands = (H + bandH - 1) / bandH;
    parallelRows(bands, threads, [&](int b) {
        const int y0 = b * bandH, y1 = std::min(H, y0 + bandH);
        for (int t : opaque) {
            rasterTri(t, y0, y1, [&](std::size_t i, float z, float b1, float b2) {
                if (z < depth[i]) {
                    depth[i] = z;
                    vis[i] = {z, t, b1, b2};
                }
            });
        }
    });
    parallelRows(H, threads, [&](int y) {
        for (int x = 0; x < W; ++x) {
            const std::size_t i = static_cast<std::size_t>(y) * static_cast<std::size_t>(W) + static_cast<std::size_t>(x);
            const V3 d = pixelDir(i);
            if (vis[i].tri < 0) {
                color[i] = bgMode ? V3{0, 0, 0} : w.skyColor(d);
                continue;
            }
            color[i] = shade(w, w.surface(vis[i]), d, o.shadows);
            cov[i] = 1;
        }
    });
    if (!blended.empty()) {
        parallelRows(bands, threads, [&](int b) {
            const int y0 = b * bandH, y1 = std::min(H, y0 + bandH);
            for (int t : blended) {
                rasterTri(t, y0, y1, [&](std::size_t i, float z, float b1, float b2) {
                    if (z >= depth[i]) return;
                    const Surface s = w.surface({z, t, b1, b2});
                    const Mat& m = *s.mat;
                    const float a = std::clamp(m.opacity * (1 - 0.85f * m.transmission), 0.05f, 1.0f);
                    const V3 c = shade(w, s, pixelDir(i), o.shadows);
                    if (cov[i] <= 0) {
                        color[i] = c;
                    } else {
                        color[i] = lerp(color[i], c, a);
                    }
                    cov[i] = std::max(cov[i], a);
                });
            }
        });
    }
    if (ss == 1) return toImage(color, cov, W, H, w, o.exposure);
    const int ow = o.width, oh = o.height;
    std::vector<V3> c2(static_cast<std::size_t>(ow) * static_cast<std::size_t>(oh));
    std::vector<float> v2(c2.size());
    for (int y = 0; y < oh; ++y) {
        for (int x = 0; x < ow; ++x) {
            V3 sum{0, 0, 0};
            float cs2 = 0;
            for (int dy = 0; dy < 2; ++dy) {
                for (int dx = 0; dx < 2; ++dx) {
                    const std::size_t i = static_cast<std::size_t>(y * 2 + dy) * static_cast<std::size_t>(W) + static_cast<std::size_t>(x * 2 + dx);
                    sum += bgMode ? color[i] * cov[i] : color[i];
                    cs2 += cov[i];
                }
            }
            const std::size_t j = static_cast<std::size_t>(y) * static_cast<std::size_t>(ow) + static_cast<std::size_t>(x);
            v2[j] = cs2 / 4;
            c2[j] = bgMode ? (cs2 > 0 ? sum / cs2 : V3{0, 0, 0}) : sum / 4.0f;
        }
    }
    return toImage(c2, v2, ow, oh, w, o.exposure);
}

}  // namespace wetwaresuite::render::detail
