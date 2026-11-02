#include "wetwaresuite/mesh/render.hpp"

#include <algorithm>
#include <array>
#include <map>
#include <cctype>
#include <cmath>
#include <limits>
#include <stdexcept>

namespace wetwaresuite::mesh {

const std::vector<std::string>& viewNames() {
    static const std::vector<std::string> v = {"iso", "front", "back", "left", "right", "top", "bottom"};
    return v;
}

std::optional<View> parseView(const std::string& in) {
    std::string s;
    for (char c : in) s += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (s == "iso" || s == "isometric") return View::Iso;
    if (s == "front") return View::Front;
    if (s == "back") return View::Back;
    if (s == "left") return View::Left;
    if (s == "right" || s == "side") return View::Right;
    if (s == "top") return View::Top;
    if (s == "bottom") return View::Bottom;
    return std::nullopt;
}

const std::vector<std::string>& shadingNames() {
    static const std::vector<std::string> v = {"material", "solid", "matcap", "wireframe", "xray"};
    return v;
}

std::optional<Shading> parseShading(const std::string& in) {
    std::string s;
    for (char c : in) s += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (s == "material" || s.empty()) return Shading::Material;
    if (s == "solid") return Shading::Solid;
    if (s == "matcap" || s == "clay") return Shading::Matcap;
    if (s == "wireframe" || s == "wire") return Shading::Wireframe;
    if (s == "xray" || s == "x-ray") return Shading::XRay;
    return std::nullopt;
}

namespace {

struct Camera {
    Vec3 forward, right, up;
};

Camera camera(View v) {
    Vec3 f, worldUp{0, 0, 1};
    switch (v) {
        case View::Iso: f = {-1, 1, -1}; break;
        case View::Front: f = {0, 1, 0}; break;
        case View::Back: f = {0, -1, 0}; break;
        case View::Left: f = {1, 0, 0}; break;
        case View::Right: f = {-1, 0, 0}; break;
        case View::Top: f = {0, 0, -1}; worldUp = {0, 1, 0}; break;
        case View::Bottom: f = {0, 0, 1}; worldUp = {0, -1, 0}; break;
    }
    Camera c;
    c.forward = normalize(f);
    c.right = normalize(cross(c.forward, worldUp));
    c.up = cross(c.right, c.forward);
    return c;
}

Camera camera(const Orbit& o) {
    const double rad = 3.14159265358979323846 / 180.0;
    const double yaw = o.yaw * rad, pitch = std::clamp(o.pitch, -89.9, 89.9) * rad;
    const Vec3 eye{std::cos(pitch) * std::sin(yaw), -std::cos(pitch) * std::cos(yaw), std::sin(pitch)};
    Camera c;
    c.forward = normalize(-eye);
    c.right = normalize(cross(c.forward, Vec3{0, 0, 1}));
    c.up = cross(c.right, c.forward);
    return c;
}

std::uint8_t clampByte(double v) { return static_cast<std::uint8_t>(std::min(255.0, std::max(0.0, v) + 0.5)); }

}  // namespace

Projection fitProjection(const Mesh& m, int W, int H, const RenderOptions& o) {
    const Camera cam = o.orbit ? camera(*o.orbit) : camera(o.view);
    Projection out;
    out.forward = cam.forward;
    out.right = cam.right;
    out.up = cam.up;
    double minX = 1e300, maxX = -1e300, minY = 1e300, maxY = -1e300;
    auto add = [&](const Vec3& p) {
        const double x = dot(p, cam.right), y = dot(p, cam.up);
        minX = std::min(minX, x); maxX = std::max(maxX, x);
        minY = std::min(minY, y); maxY = std::max(maxY, y);
    };
    for (const Vec3& p : m.positions) add(p);
    for (const auto& ln : o.lines)
        for (const Vec3& p : ln.points) add(p);
    if (minX > maxX) minX = maxX = minY = maxY = 0;
    const double ex = std::max(maxX - minX, 1e-12), ey = std::max(maxY - minY, 1e-12);
    const double avail = 1.0 - 2.0 * std::clamp(o.margin, 0.0, 0.45);
    out.scale = std::min(std::max(W, 1) * avail / ex, std::max(H, 1) * avail / ey);
    out.cx = (minX + maxX) / 2;
    out.cy = (minY + maxY) / 2;
    if (o.orbit) {
        // fit the bounding sphere, not this view's outline, so orbiting doesn't change the size
        Vec3 lo{1e300, 1e300, 1e300}, hi{-1e300, -1e300, -1e300};
        auto grow = [&](const Vec3& p) {
            lo = {std::min(lo.x, p.x), std::min(lo.y, p.y), std::min(lo.z, p.z)};
            hi = {std::max(hi.x, p.x), std::max(hi.y, p.y), std::max(hi.z, p.z)};
        };
        for (const Vec3& p : m.positions) grow(p);
        for (const auto& ln : o.lines)
            for (const Vec3& p : ln.points) grow(p);
        if (lo.x > hi.x) lo = hi = {0, 0, 0};
        const Vec3 centre = (lo + hi) * 0.5;
        const double radius = std::max(length(hi - lo) / 2, 1e-12);
        out.scale = std::min(std::max(W, 1), std::max(H, 1)) * avail / (2 * radius) * std::clamp(o.orbit->zoom, 1e-3, 1e4);
        out.cx = dot(centre, cam.right) - o.orbit->panX * radius;
        out.cy = dot(centre, cam.up) - o.orbit->panY * radius;
    }
    return out;
}

raster::Image render(const Mesh& input, int width, int height, const RenderOptions& o) {
    if (width <= 0 || height <= 0) throw std::invalid_argument("render size must be positive");
    raster::Image out(width, height, o.background);
    if (input.empty() && o.lines.empty()) return out;

    const int ss = o.antialias ? 2 : 1;
    const int W = width * ss, H = height * ss;

    Mesh smoothMesh;
    const Mesh* mp = &input;
    if (o.smooth && !input.hasNormals()) {
        smoothMesh = input;
        computeNormals(smoothMesh, 30.0);
        mp = &smoothMesh;
    }
    const Mesh& m = *mp;
    const bool useVertexNormals = o.smooth && m.hasNormals();

    // Project (fitted to the mesh and lines unless a projection is given).
    const Projection proj = o.projection ? *o.projection : fitProjection(input, width, height, o);
    const Camera cam{proj.forward, proj.right, proj.up};
    const double scale = proj.scale * ss, cx = proj.cx, cy = proj.cy;
    std::vector<Vec3> scr(m.positions.size());
    double minD = 1e300, maxD = -1e300;
    for (std::size_t i = 0; i < m.positions.size(); ++i) {
        const Vec3& p = m.positions[i];
        const double d = dot(p, cam.forward);
        scr[i] = {dot(p, cam.right), dot(p, cam.up), d};
        minD = std::min(minD, d);
        maxD = std::max(maxD, d);
    }
    for (auto& s : scr) {
        s.x = (s.x - cx) * scale + W / 2.0;
        s.y = H / 2.0 - (s.y - cy) * scale;
    }

    // Lighting in world space: key light from the upper left of the viewer, plus a soft fill.
    const Vec3 toEye = -cam.forward;
    const Vec3 key = normalize(toEye * 1.0 + cam.up * 0.75 - cam.right * 0.45);
    const Vec3 fill = normalize(toEye * 0.6 - cam.up * 0.3 + cam.right * 0.7);
    const Vec3 half = normalize(key + toEye);
    const bool matcap = o.shading == Shading::Matcap;
    auto shade = [&](Vec3 n) {
        if (dot(n, toEye) < 0) n = -n;  // two-sided
        if (matcap) {
            // a clay ball lit from the top left in view space, with a soft rim: the same on every model
            const double u = dot(n, cam.right), v = dot(n, cam.up), z = std::max(0.0, dot(n, toEye));
            const double lightN = std::max(0.0, -0.45 * u + 0.6 * v + 0.66 * z);
            const double rim = std::pow(1.0 - z, 3.0);
            const double spec = std::pow(std::max(0.0, -0.3 * u + 0.45 * v + 0.84 * z), 60.0);
            return Vec3{0.22 + 0.78 * lightN + 0.35 * rim, spec * 0.35, 0};
        }
        const double diff = std::max(0.0, dot(n, key)), f = std::max(0.0, dot(n, fill));
        const double spec = std::pow(std::max(0.0, dot(n, half)), 40.0);
        return Vec3{0.34 + 0.58 * diff + 0.22 * f, spec * 0.22, 0};  // (diffuse factor, specular)
    };

    const std::size_t npx = static_cast<std::size_t>(W) * static_cast<std::size_t>(H);
    std::vector<float> depth(npx, std::numeric_limits<float>::infinity());
    std::vector<std::int32_t> faceId(npx, -1);
    std::vector<float> lit(npx * 2, 0.f);
    std::vector<Vec3> faceN(m.triangleCount());

    for (std::size_t t = 0; t < m.triangleCount(); ++t) {
        const std::uint32_t i0 = m.indices[t * 3], i1 = m.indices[t * 3 + 1], i2 = m.indices[t * 3 + 2];
        const Vec3 &a = scr[i0], &b = scr[i1], &c = scr[i2];
        faceN[t] = normalize(m.faceNormal(t));
        const double area = (b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x);
        if (std::fabs(area) < 1e-12) continue;
        Vec3 s0, s1, s2;
        if (useVertexNormals) {
            s0 = shade(m.normals[i0]);
            s1 = shade(m.normals[i1]);
            s2 = shade(m.normals[i2]);
            // Smooth normals can face away on silhouettes; fall back to the face for those.
            const Vec3 sf = shade(faceN[t]);
            if (dot(m.normals[i0], faceN[t]) < 0.2) s0 = sf;
            if (dot(m.normals[i1], faceN[t]) < 0.2) s1 = sf;
            if (dot(m.normals[i2], faceN[t]) < 0.2) s2 = sf;
        } else {
            s0 = s1 = s2 = shade(faceN[t]);
        }
        const int x0 = std::max(0, static_cast<int>(std::floor(std::min({a.x, b.x, c.x}))));
        const int x1 = std::min(W - 1, static_cast<int>(std::ceil(std::max({a.x, b.x, c.x}))));
        const int y0 = std::max(0, static_cast<int>(std::floor(std::min({a.y, b.y, c.y}))));
        const int y1 = std::min(H - 1, static_cast<int>(std::ceil(std::max({a.y, b.y, c.y}))));
        if (x0 > x1 || y0 > y1) continue;
        const double inv = 1.0 / area;
        for (int y = y0; y <= y1; ++y) {
            const double py = y + 0.5;
            for (int x = x0; x <= x1; ++x) {
                const double px = x + 0.5;
                double w0 = ((b.x - px) * (c.y - py) - (b.y - py) * (c.x - px)) * inv;
                double w1 = ((c.x - px) * (a.y - py) - (c.y - py) * (a.x - px)) * inv;
                double w2 = 1.0 - w0 - w1;
                if (w0 < 0 || w1 < 0 || w2 < 0) continue;
                const float d = static_cast<float>(w0 * a.z + w1 * b.z + w2 * c.z);
                const std::size_t k = static_cast<std::size_t>(y) * static_cast<std::size_t>(W) + static_cast<std::size_t>(x);
                if (d >= depth[k]) continue;
                depth[k] = d;
                faceId[k] = static_cast<std::int32_t>(t);
                lit[k * 2] = static_cast<float>(w0 * s0.x + w1 * s1.x + w2 * s2.x);
                lit[k * 2 + 1] = static_cast<float>(w0 * s0.y + w1 * s1.y + w2 * s2.y);
            }
        }
    }

    // Outlines: silhouettes, sharp creases and depth jumps.
    std::vector<std::uint8_t> edge(npx, 0);
    if (o.outline) {
        const double cosCrease = std::cos(35.0 * kPi / 180.0);
        const float jump = static_cast<float>((maxD - minD) * 0.04 + 1e-9);
        auto check = [&](std::size_t k, std::size_t q) {
            const std::int32_t f = faceId[k], g = faceId[q];
            if (f == g) return;
            bool e = (f < 0) != (g < 0);
            if (!e && f >= 0) {
                e = dot(faceN[static_cast<std::size_t>(f)], faceN[static_cast<std::size_t>(g)]) < cosCrease ||
                    std::fabs(depth[k] - depth[q]) > jump;
            }
            if (e) {
                if (f >= 0) edge[k] = 1;
                if (g >= 0) edge[q] = 1;
            }
        };
        for (int y = 0; y < H; ++y) {
            for (int x = 0; x < W; ++x) {
                const std::size_t k = static_cast<std::size_t>(y) * static_cast<std::size_t>(W) + static_cast<std::size_t>(x);
                if (x + 1 < W) check(k, k + 1);
                if (y + 1 < H) check(k, k + static_cast<std::size_t>(W));
            }
        }
    }

    // Lines: depth-tested against the model with a little slack, so a sketch on a face shows.
    std::vector<std::int32_t> lineId(o.lines.empty() ? 0 : npx, -1);
    if (!o.lines.empty()) {
        const float slack = static_cast<float>((maxD > minD ? maxD - minD : 0.0) * 0.01 + 1e-6) + static_cast<float>(1.5 / scale);
        auto toScreen = [&](const Vec3& p) {
            return Vec3{(dot(p, cam.right) - cx) * scale + W / 2.0, H / 2.0 - (dot(p, cam.up) - cy) * scale, dot(p, cam.forward)};
        };
        const int thick = ss;  // about one output pixel either side
        for (std::size_t li = 0; li < o.lines.size(); ++li) {
            const auto& ln = o.lines[li];
            const std::size_t n = ln.points.size();
            for (std::size_t i = 0; i + 1 < n + (ln.closed && n > 2 ? 1 : 0); ++i) {
                const Vec3 a = toScreen(ln.points[i]), b = toScreen(ln.points[(i + 1) % n]);
                const double len = std::max(std::fabs(b.x - a.x), std::fabs(b.y - a.y));
                const int steps = std::max(1, static_cast<int>(std::ceil(len)));
                for (int k = 0; k <= steps; ++k) {
                    const double t = static_cast<double>(k) / steps;
                    const int x = static_cast<int>(a.x + (b.x - a.x) * t), y = static_cast<int>(a.y + (b.y - a.y) * t);
                    const float d = static_cast<float>(a.z + (b.z - a.z) * t);
                    for (int dy = -thick / 2; dy <= thick / 2; ++dy) {
                        for (int dx = -thick / 2; dx <= thick / 2; ++dx) {
                            const int px = x + dx, py = y + dy;
                            if (px < 0 || py < 0 || px >= W || py >= H) continue;
                            const std::size_t q = static_cast<std::size_t>(py) * static_cast<std::size_t>(W) + static_cast<std::size_t>(px);
                            if (d <= depth[q] + slack) lineId[q] = static_cast<std::int32_t>(li);
                        }
                    }
                }
            }
        }
    }

    // wireframe and x-ray: the mesh's edges (not the diagonals of flat quads), drawn over everything
    const bool wire = o.shading == Shading::Wireframe || o.shading == Shading::XRay;
    std::vector<std::uint8_t> wireHit(wire ? npx : 0, 0);
    if (wire) {
        std::map<std::pair<std::uint32_t, std::uint32_t>, std::vector<std::size_t>> edges;
        // weld by position so split normals don't turn every edge into a boundary
        std::map<std::array<double, 3>, std::uint32_t> weld;
        std::vector<std::uint32_t> id(m.positions.size());
        for (std::size_t i = 0; i < m.positions.size(); ++i)
            id[i] = weld.try_emplace({m.positions[i].x, m.positions[i].y, m.positions[i].z}, static_cast<std::uint32_t>(weld.size())).first->second;
        std::vector<std::uint32_t> rep(weld.size());
        for (std::size_t i = 0; i < m.positions.size(); ++i) rep[id[i]] = static_cast<std::uint32_t>(i);
        for (std::size_t t = 0; t < m.triangleCount(); ++t)
            for (int k = 0; k < 3; ++k) {
                std::uint32_t a = id[m.indices[t * 3 + static_cast<std::size_t>(k)]], b = id[m.indices[t * 3 + static_cast<std::size_t>((k + 1) % 3)]];
                if (b < a) std::swap(a, b);
                edges[{a, b}].push_back(t);
            }
        for (const auto& [e, tris] : edges) {
            if (tris.size() == 2 && dot(faceN[tris[0]], faceN[tris[1]]) > 0.9999) continue;  // inside a flat polygon
            const Vec3 &a = scr[rep[e.first]], &b = scr[rep[e.second]];
            const double len = std::max(std::fabs(b.x - a.x), std::fabs(b.y - a.y));
            const int steps = std::max(1, static_cast<int>(std::ceil(len)));
            for (int k = 0; k <= steps; ++k) {
                const double t = static_cast<double>(k) / steps;
                const int x = static_cast<int>(a.x + (b.x - a.x) * t), y = static_cast<int>(a.y + (b.y - a.y) * t);
                if (x < 0 || y < 0 || x >= W || y >= H) continue;
                const std::size_t q = static_cast<std::size_t>(y) * static_cast<std::size_t>(W) + static_cast<std::size_t>(x);
                const float d = static_cast<float>(a.z + (b.z - a.z) * t);
                // wireframe: hidden edges fainter; x-ray: every edge the same
                wireHit[q] = std::max<std::uint8_t>(wireHit[q], (o.shading == Shading::XRay || d <= depth[q] + (maxD - minD) * 0.002 + 1e-9) ? 2 : 1);
            }
        }
    }
    const bool perFace = o.faceColors.size() == m.triangleCount() && o.shading != Shading::Solid && o.shading != Shading::Matcap;
    const double ink = 0.25;
    const double ss2 = static_cast<double>(ss * ss);
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            double r = 0, g = 0, b = 0, a = 0;
            for (int sy = 0; sy < ss; ++sy) {
                for (int sx = 0; sx < ss; ++sx) {
                    const std::size_t k = static_cast<std::size_t>(y * ss + sy) * static_cast<std::size_t>(W) + static_cast<std::size_t>(x * ss + sx);
                    double pr, pg, pb, pa;
                    if (!lineId.empty() && lineId[k] >= 0) {
                        const raster::Color& lc = o.lines[static_cast<std::size_t>(lineId[k])].color;
                        pr = lc.r;
                        pg = lc.g;
                        pb = lc.b;
                        pa = 1.0;
                    } else if (wire && wireHit[k]) {
                        const double strength = wireHit[k] == 2 ? 1.0 : 0.35;
                        const double base = faceId[k] >= 0 && o.shading == Shading::XRay ? 200 : o.background.r;
                        pr = base + (40 - base) * strength;
                        pg = base + (44 - base) * strength;
                        pb = base + (56 - base) * strength;
                        pa = 1.0;
                    } else if (faceId[k] < 0 || o.shading == Shading::Wireframe) {
                        pa = o.background.a / 255.0;
                        pr = o.background.r * pa;
                        pg = o.background.g * pa;
                        pb = o.background.b * pa;
                    } else if (o.shading == Shading::XRay) {
                        // see-through: the surface tinted lightly over the background
                        const raster::Color& fc = perFace ? o.faceColors[static_cast<std::size_t>(faceId[k])] : o.color;
                        const double t = 0.3 * lit[k * 2];
                        pr = o.background.r * (1 - t) + fc.r * t;
                        pg = o.background.g * (1 - t) + fc.g * t;
                        pb = o.background.b * (1 - t) + fc.b * t;
                        pa = 1.0;
                    } else {
                        const double diff = lit[k * 2], spec = lit[k * 2 + 1] * 255.0;
                        const raster::Color clay{196, 188, 178, 255};
                        const raster::Color& fc = perFace ? o.faceColors[static_cast<std::size_t>(faceId[k])] : matcap ? clay : o.color;
                        const double br = fc.r, bg = fc.g, bb = fc.b;
                        pr = br * diff + spec;
                        pg = bg * diff + spec;
                        pb = bb * diff + spec;
                        if (edge[k]) {
                            pr = pr * ink + 30 * (1 - ink);
                            pg = pg * ink + 36 * (1 - ink);
                            pb = pb * ink + 48 * (1 - ink);
                        }
                        pa = 1.0;
                    }
                    r += pr; g += pg; b += pb; a += pa;
                }
            }
            raster::Color c;
            if (a <= 0) {
                c = {o.background.r, o.background.g, o.background.b, 0};
            } else {
                c = {clampByte(r / a), clampByte(g / a), clampByte(b / a), clampByte(a / ss2 * 255.0)};
            }
            out.set(x, y, c);
        }
    }
    return out;
}

}  // namespace wetwaresuite::mesh
