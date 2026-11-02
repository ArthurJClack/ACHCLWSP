// The path tracer: next-event estimation at every diffuse/glossy hit, GGX
// specular and cosine-weighted diffuse sampling, glass with Fresnel, cut-out
// transparency, Russian roulette, a firefly clamp, and an a-trous denoiser
// guided by first-hit albedo, normals and depth.
#include "internal.hpp"

#include <algorithm>

namespace wetwaresuite::render::detail {

namespace {

constexpr float kMirror = 0.02f;

inline V3 schlick(const V3& f0, float c) {
    const float m = pow5(1 - std::clamp(c, 0.0f, 1.0f));
    return f0 + (V3{1, 1, 1} - f0) * m;
}

inline float ggxAlpha(float rough) { return std::max(rough * rough, 0.01f); }

inline V3 sampleGgx(const V3& n, float a, Rng& rng) {
    const float u1 = rng.uniform(), u2 = rng.uniform();
    const float phi = 2 * kPiF * u1;
    const float cosT = std::sqrt((1 - u2) / (1 + (a * a - 1) * u2));
    const float sinT = std::sqrt(std::max(0.0f, 1 - cosT * cosT));
    V3 t, b;
    basis(n, t, b);
    return normalize(t * (std::cos(phi) * sinT) + b * (std::sin(phi) * sinT) + n * cosT);
}

inline V3 sampleCosine(const V3& n, Rng& rng) {
    const float u1 = rng.uniform(), u2 = rng.uniform();
    const float r = std::sqrt(u1), phi = 2 * kPiF * u2;
    V3 t, b;
    basis(n, t, b);
    return normalize(t * (r * std::cos(phi)) + b * (r * std::sin(phi)) + n * std::sqrt(std::max(0.0f, 1 - u1)));
}

inline V3 clampFirefly(V3 c, int bounce) {
    if (bounce == 0) return c;
    const float m = maxComponent(c);
    return m > 3.0f ? c * (3.0f / m) : c;
}

float fresnelDielectric(float cosi, float cost, float eta) {
    const float rs = (eta * cosi - cost) / (eta * cosi + cost);
    const float rp = (cosi - eta * cost) / (cosi + eta * cost);
    return std::clamp(0.5f * (rs * rs + rp * rp), 0.0f, 1.0f);
}

struct PathResult {
    V3 L{0, 0, 0};
    bool hit = false;
    V3 albedo{1, 1, 1}, normal{0, 0, 0};
    float depth = 1e30f;
};

PathResult tracePath(const World& w, V3 o, V3 d, int maxBounces, Rng& rng) {
    PathResult r;
    V3 T{1, 1, 1};
    int bounce = 0, passes = 0;
    bool straight = true;  // only cut-outs and refraction so far: the camera still "sees" the background
    for (;;) {
        Hit h;
        if (!w.intersect(o, d, 1e30f, h)) {
            if (r.hit) {
                const V3 env = straight && !w.sky.visible ? V3(w.sky.background) : w.skyColor(d);
                r.L += clampFirefly(T * env, bounce);
            } else if (w.sky.visible) {
                r.L += w.skyColor(d);
            }
            break;
        }
        const Surface s = w.surface(h);
        if (!r.hit) {
            r.hit = true;
            r.albedo = s.base;
            r.normal = s.n;
            r.depth = h.t;
        }
        const V3 wo = -d;
        const bool entering = dot(s.ng, d) < 0;
        V3 ng = entering ? s.ng : -s.ng;
        V3 n = dot(s.n, ng) >= 0 ? s.n : -s.n;
        if (dot(n, wo) <= 0) n = ng;
        const Mat& m = *s.mat;
        if (maxComponent(m.emission) > 0) r.L += clampFirefly(T * m.emission, bounce);

        if (m.opacity < 1 && rng.uniform() >= m.opacity) {
            o = s.p - ng * w.eps;
            if (++passes > 32) break;
            continue;
        }
        if (m.transmission > 0 && rng.uniform() < m.transmission) {
            if (bounce >= maxBounces) break;
            V3 hn = n;
            if (s.rough >= kMirror) {
                hn = sampleGgx(n, ggxAlpha(s.rough), rng);
                if (dot(hn, wo) <= 0) hn = n;
            }
            const float eta = entering ? 1.0f / m.ior : m.ior;
            const float cosi = std::clamp(dot(wo, hn), 0.0f, 1.0f);
            const float sin2t = eta * eta * (1 - cosi * cosi);
            float F = 1;
            float cost = 0;
            if (sin2t < 1) {
                cost = std::sqrt(1 - sin2t);
                F = fresnelDielectric(cosi, cost, eta);
            }
            if (rng.uniform() < F) {
                d = normalize(reflect(d, hn));
                o = s.p + ng * w.eps;
                straight = false;
            } else {
                d = normalize(d * eta + hn * (eta * cosi - cost));
                o = s.p - ng * w.eps;
                if (entering) T *= s.base;
            }
            ++bounce;
            continue;
        }
        straight = false;
        r.L += clampFirefly(T * directLight(w, s, n, wo, &rng, true), bounce);
        if (bounce >= maxBounces) break;

        const float nv = std::max(dot(n, wo), 1e-4f);
        const V3 f0 = lerp(V3{0.04f, 0.04f, 0.04f}, s.base, m.metallic);
        const bool mirror = s.rough < kMirror;
        const float ps = m.metallic >= 0.999f ? 1.0f
                                              : m.metallic + (1 - m.metallic) * std::clamp(luminance(schlick(f0, nv)), 0.05f, 0.95f);
        V3 wi;
        V3 weight;
        if (rng.uniform() < ps && mirror) {
            wi = normalize(reflect(d, n));
            weight = schlick(f0, nv) / ps;
        } else {
            const float a = ggxAlpha(s.rough);
            const bool spec = !mirror && rng.uniform() < ps;
            if (spec) {
                const V3 hv = sampleGgx(n, a, rng);
                wi = normalize(reflect(d, hv));
            } else {
                wi = sampleCosine(n, rng);
            }
            const float nl = dot(n, wi);
            if (nl <= 0) break;
            const V3 f = evalBrdf(s, n, wo, wi);
            const float pdfD = nl / kPiF;
            float pdf;
            if (mirror) {
                pdf = (1 - ps) * pdfD;
            } else {
                const V3 hv = normalize(wo + wi);
                const float nh = std::max(dot(n, hv), 0.0f), vh = std::max(dot(wo, hv), 1e-4f);
                const float a2 = a * a, dd = nh * nh * (a2 - 1) + 1;
                const float D = a2 / (kPiF * dd * dd);
                pdf = ps * D * nh / (4 * vh) + (1 - ps) * pdfD;
            }
            if (pdf <= 1e-8f) break;
            weight = f * (nl / pdf);
        }
        if (dot(ng, wi) <= 0) break;
        T *= weight;
        if (maxComponent(T) <= 0) break;
        d = wi;
        o = s.p + ng * w.eps;
        ++bounce;
        if (bounce >= 3) {
            const float p = std::clamp(maxComponent(T), 0.05f, 0.95f);
            if (rng.uniform() > p) break;
            T *= 1.0f / p;
        }
    }
    return r;
}

std::uint64_t hash(std::uint64_t x) {
    x ^= x >> 33;
    x *= 0xff51afd7ed558ccdULL;
    x ^= x >> 33;
    x *= 0xc4ceb9fe1a85ec53ULL;
    x ^= x >> 33;
    return x;
}

void denoise(std::vector<V3>& color, const std::vector<V3>& albedo, const std::vector<V3>& normal, const std::vector<float>& depth,
             int W, int H, int spp, int threads) {
    const std::size_t n = color.size();
    std::vector<V3> illum(n), out(n);
    for (std::size_t i = 0; i < n; ++i) {
        const V3 a = vmax(albedo[i], V3{0.02f, 0.02f, 0.02f});
        illum[i] = {color[i].x / a.x, color[i].y / a.y, color[i].z / a.z};
    }
    static const float k[5] = {1.0f / 16, 1.0f / 4, 3.0f / 8, 1.0f / 4, 1.0f / 16};
    const float noise = std::clamp(std::sqrt(16.0f / static_cast<float>(spp)), 0.3f, 2.0f);
    for (int pass = 0; pass < 3; ++pass) {
        const int step = 1 << pass;
        const float sigma = 1.5f * noise / static_cast<float>(step);
        parallelRows(H, threads, [&](int y) {
            for (int x = 0; x < W; ++x) {
                const std::size_t i = static_cast<std::size_t>(y) * static_cast<std::size_t>(W) + static_cast<std::size_t>(x);
                const bool skyI = dot(normal[i], normal[i]) < 0.25f;
                const float li = luminance(illum[i]);
                V3 sum{0, 0, 0};
                float wsum = 0;
                for (int dy = -2; dy <= 2; ++dy) {
                    const int yy = y + dy * step;
                    if (yy < 0 || yy >= H) continue;
                    for (int dx = -2; dx <= 2; ++dx) {
                        const int xx = x + dx * step;
                        if (xx < 0 || xx >= W) continue;
                        const std::size_t j = static_cast<std::size_t>(yy) * static_cast<std::size_t>(W) + static_cast<std::size_t>(xx);
                        const bool skyJ = dot(normal[j], normal[j]) < 0.25f;
                        float wgt = k[dx + 2] * k[dy + 2];
                        if (skyI != skyJ) continue;
                        if (!skyI) {
                            const float nd = std::max(0.0f, dot(normal[i], normal[j]));
                            const float n2 = nd * nd, n4 = n2 * n2, n8 = n4 * n4;
                            wgt *= n8 * n8;
                            const float dz = std::fabs(depth[i] - depth[j]) / (0.02f * std::max(depth[i], depth[j]) + 1e-12f);
                            wgt *= fastFalloff(dz);
                        }
                        const float lj = luminance(illum[j]);
                        const float dl = std::fabs(li - lj) / (sigma * 0.5f * (li + lj) + 0.02f);
                        wgt *= fastFalloff(dl * dl);
                        sum += illum[j] * wgt;
                        wsum += wgt;
                    }
                }
                out[i] = wsum > 0 ? sum / wsum : illum[i];
            }
        });
        std::swap(illum, out);
    }
    for (std::size_t i = 0; i < n; ++i) color[i] = illum[i] * vmax(albedo[i], V3{0.02f, 0.02f, 0.02f});
}

}  // namespace

raster::Image toImage(const std::vector<V3>& color, const std::vector<float>& coverage, int W, int H, const World& w, double exposure) {
    raster::Image img(W, H);
    const float scale = static_cast<float>(std::pow(2.0, exposure));
    const bool bgMode = !w.sky.visible || w.sky.transparent;
    const V3 bg(w.sky.background);
    const float bgc[3] = {linearToSrgb(bg.x) * 255.0f, linearToSrgb(bg.y) * 255.0f, linearToSrgb(bg.z) * 255.0f};
    for (int y = 0; y < H; ++y) {
        for (int x = 0; x < W; ++x) {
            const std::size_t i = static_cast<std::size_t>(y) * static_cast<std::size_t>(W) + static_cast<std::size_t>(x);
            const V3 c = color[i] * scale;
            std::uint8_t* p = img.pixel(x, y);
            const std::uint8_t rgb[3] = {toByte(filmic(c.x)), toByte(filmic(c.y)), toByte(filmic(c.z))};
            if (!bgMode) {
                p[0] = rgb[0], p[1] = rgb[1], p[2] = rgb[2], p[3] = 255;
                continue;
            }
            const float a = std::clamp(coverage[i], 0.0f, 1.0f);
            if (w.sky.transparent) {
                p[0] = rgb[0], p[1] = rgb[1], p[2] = rgb[2];
                p[3] = static_cast<std::uint8_t>(std::lround(a * 255.0f));
            } else {
                for (int k = 0; k < 3; ++k) p[k] = static_cast<std::uint8_t>(std::lround(rgb[k] * a + bgc[k] * (1 - a)));
                p[3] = 255;
            }
        }
    }
    return img;
}

raster::Image pathTrace(const World& w, const CameraF& cam, const RenderOptions& o) {
    const int W = o.width, H = o.height, spp = o.samples;
    const std::size_t n = static_cast<std::size_t>(W) * static_cast<std::size_t>(H);
    const bool bgMode = !w.sky.visible || w.sky.transparent;
    std::vector<V3> color(n), albedo(n, V3{1, 1, 1}), normal(n);
    std::vector<float> depth(n, 1e30f), cov(n, 1.0f);
    const int threads = threadCount(o.threads);
    parallelRows(H, threads, [&](int y) {
        for (int x = 0; x < W; ++x) {
            const std::size_t i = static_cast<std::size_t>(y) * static_cast<std::size_t>(W) + static_cast<std::size_t>(x);
            Rng rng(hash((static_cast<std::uint64_t>(o.seed) << 40) ^ (static_cast<std::uint64_t>(y) << 20) ^ static_cast<std::uint64_t>(x)));
            V3 acc{0, 0, 0}, accA{0, 0, 0}, accN{0, 0, 0};
            float accD = 0;
            int hits = 0;
            for (int s = 0; s < spp; ++s) {
                const float sx = ((static_cast<float>(x) + rng.uniform()) / static_cast<float>(W)) * 2 - 1;
                const float sy = 1 - ((static_cast<float>(y) + rng.uniform()) / static_cast<float>(H)) * 2;
                V3 ro, rd;
                cam.ray(sx, sy, ro, rd);
                const PathResult pr = tracePath(w, ro, rd, o.bounces, rng);
                if (pr.hit || !bgMode) acc += pr.L;
                if (pr.hit) {
                    ++hits;
                    accA += pr.albedo;
                    accN += pr.normal;
                    accD += pr.depth;
                }
            }
            if (bgMode) {
                cov[i] = static_cast<float>(hits) / static_cast<float>(spp);
                color[i] = hits ? acc / static_cast<float>(hits) : V3{0, 0, 0};
            } else {
                color[i] = acc / static_cast<float>(spp);
            }
            if (hits) {
                albedo[i] = accA / static_cast<float>(hits);
                normal[i] = length(accN) > 0 ? normalize(accN) : V3{0, 0, 1};
                depth[i] = accD / static_cast<float>(hits);
            }
        }
    });
    if (o.denoise && spp < 4096) denoise(color, albedo, normal, depth, W, H, spp, threads);
    return toImage(color, cov, W, H, w, o.exposure);
}

}  // namespace wetwaresuite::render::detail
