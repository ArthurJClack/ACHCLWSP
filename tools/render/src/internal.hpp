// Shared internals of the renderer: float vectors, the flattened world
// (triangles, materials, textures), the BVH, camera rays and shading.
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "wetwaresuite/render/render.hpp"

namespace wetwaresuite::render::detail {

struct V3 {
    float x = 0, y = 0, z = 0;
    V3() = default;
    constexpr V3(float a, float b, float c) : x(a), y(b), z(c) {}
    explicit V3(const Vec3& v) : x(static_cast<float>(v.x)), y(static_cast<float>(v.y)), z(static_cast<float>(v.z)) {}
    V3 operator+(const V3& o) const { return {x + o.x, y + o.y, z + o.z}; }
    V3 operator-(const V3& o) const { return {x - o.x, y - o.y, z - o.z}; }
    V3 operator-() const { return {-x, -y, -z}; }
    V3 operator*(float s) const { return {x * s, y * s, z * s}; }
    V3 operator*(const V3& o) const { return {x * o.x, y * o.y, z * o.z}; }
    V3 operator/(float s) const { return {x / s, y / s, z / s}; }
    V3& operator+=(const V3& o) { x += o.x; y += o.y; z += o.z; return *this; }
    V3& operator*=(const V3& o) { x *= o.x; y *= o.y; z *= o.z; return *this; }
    V3& operator*=(float s) { x *= s; y *= s; z *= s; return *this; }
    float operator[](int i) const { return i == 0 ? x : i == 1 ? y : z; }
};

inline V3 operator*(float s, const V3& v) { return v * s; }
inline float dot(const V3& a, const V3& b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
inline V3 cross(const V3& a, const V3& b) { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }
inline float length(const V3& v) { return std::sqrt(dot(v, v)); }
inline V3 normalize(const V3& v) {
    const float l = length(v);
    return l > 0 ? v / l : V3{0, 0, 1};
}
inline V3 vmin(const V3& a, const V3& b) { return {std::min(a.x, b.x), std::min(a.y, b.y), std::min(a.z, b.z)}; }
inline V3 vmax(const V3& a, const V3& b) { return {std::max(a.x, b.x), std::max(a.y, b.y), std::max(a.z, b.z)}; }
inline float luminance(const V3& c) { return 0.2126f * c.x + 0.7152f * c.y + 0.0722f * c.z; }
inline float maxComponent(const V3& c) { return std::max({c.x, c.y, c.z}); }
inline V3 lerp(const V3& a, const V3& b, float t) { return a + (b - a) * t; }
inline V3 reflect(const V3& d, const V3& n) { return d - n * (2 * dot(d, n)); }
// Two unit vectors perpendicular to n.
inline void basis(const V3& n, V3& t, V3& b) {
    const float sign = std::copysign(1.0f, n.z);
    const float a = -1.0f / (sign + n.z);
    const float c = n.x * n.y * a;
    t = {1.0f + sign * n.x * n.x * a, sign * c, -sign * n.x};
    b = {c, sign + n.y * n.y * a, -n.y};
}

constexpr float kPiF = 3.14159265358979f;

inline float pow5(float x) {
    const float x2 = x * x;
    return x2 * x2 * x;
}
// Cheap stand-in for exp(-x), x >= 0, for filter weights.
inline float fastFalloff(float x) { return 1.0f / (1.0f + x + 0.5f * x * x + 0.1667f * x * x * x); }

// sRGB <-> linear for one channel in 0..1.
float srgbToLinear(float c);
float linearToSrgb(float c);
// "#rrggbb", names, rgb(...) or "r,g,b" floats (0..1, linear) -> linear colour.
std::optional<Vec3> parseColor(const std::string& text);
std::string hexColor(const Vec3& linear);

// Small fast RNG (PCG32).
struct Rng {
    std::uint64_t state;
    explicit Rng(std::uint64_t seed) : state(seed * 6364136223846793005ULL + 1442695040888963407ULL) { next(); }
    std::uint32_t next() {
        const std::uint64_t old = state;
        state = old * 6364136223846793005ULL + 1442695040888963407ULL;
        const auto xorshifted = static_cast<std::uint32_t>(((old >> 18u) ^ old) >> 27u);
        const auto rot = static_cast<std::uint32_t>(old >> 59u);
        return (xorshifted >> rot) | (xorshifted << ((32 - rot) & 31));
    }
    float uniform() { return static_cast<float>(next() >> 8) * (1.0f / 16777216.0f); }  // [0, 1)
};

struct Texture {
    int w = 0, h = 0;
    std::vector<V3> px;  // linear
    V3 sample(float u, float v) const;  // bilinear, repeating
};

struct Mat {
    V3 color{0.8f, 0.8f, 0.8f};
    float roughness = 0.5f, metallic = 0;
    V3 emission{0, 0, 0};  // colour x strength
    float transmission = 0, ior = 1.5f, opacity = 1;
    int texture = -1;
    int normalMap = -1, roughnessMap = -1;  // linear textures
    float normalStrength = 1;
    float textureScale = 1;
    bool opaque() const { return transmission <= 0 && opacity >= 1; }
};

struct Tri {
    V3 p[3];
    V3 n[3];
    float u[3], v[3];
    V3 tangent, bitangent;  // world-space dP/du and dP/dv, for normal maps
    int material = 0;
};

struct Node {
    V3 bmin, bmax;
    int first = 0;  // leaf: first triangle; inner: index of the left child (right = left + 1)
    int count = 0;  // > 0 for leaves
};

struct Hit {
    float t = 0;
    int tri = -1;
    float b1 = 0, b2 = 0;  // barycentrics of corners 1 and 2
};

struct Surface {
    V3 p, n, ng;  // position, shading normal, geometric normal (both unit, unflipped)
    float u = 0, v = 0;
    const Mat* mat = nullptr;
    V3 base;  // colour x texture
    float rough = 0.5f;  // roughness x roughness map
};

struct LightF {
    int type = 0;  // 0 point, 1 sun, 2 area
    V3 position, dir;  // dir: where the light points (sun, area)
    V3 color;  // colour x intensity
    float radius = 0, cosAngle = 1;
    V3 ax, ay;  // area: half-extent axes
    float area = 1;
    bool shadows = true;
};

struct CameraF {
    V3 pos, fwd, right, up;
    float tanHalf = 0.36f, aspect = 1, halfH = 1;
    bool ortho = false;
    // sx, sy in [-1, 1], +y up
    void ray(float sx, float sy, V3& origin, V3& dir) const;
};

class World {
public:
    std::vector<Tri> tris;
    std::vector<Mat> mats;
    std::vector<Texture> textures;
    std::vector<LightF> lights;
    std::vector<Node> nodes;
    Sky sky;
    int skyType = 0;  // 0 gradient, 1 colour, 2 none, 3 hdri
    Texture hdri;     // equirectangular environment, linear
    V3 hdriAmbient[6];  // average light from +x -x +y -y +z -z, for preview shading
    float hdriTurn = 0;  // radians about z
    float eps = 1e-4f;  // ray offset, scaled to the scene
    std::vector<std::string> warnings;

    void build(const Scene& scene);
    void buildBvh();
    bool intersect(const V3& o, const V3& d, float tmax, Hit& hit) const;
    // Light that gets through from o along d up to tmax: 0 when blocked, tinted by glass.
    V3 transmittance(const V3& o, const V3& d, float tmax) const;
    Surface surface(const Hit& hit) const;
    V3 skyColor(const V3& d) const;
    V3 skyAmbient(const V3& n) const;  // rough irradiance estimate for preview shading
};

// Object-space geometry (origin at the middle of the base) with normals; planes
// with no size get `autoPlane`.
mesh::Mesh objectSpaceMesh(const Object& o, double autoPlane);

CameraF makeCamera(const Camera& cam, int width, int height);

// BRDF (diffuse + GGX specular), evaluated for directions wo (to the viewer) and wi (to the light).
V3 evalBrdf(const Surface& s, const V3& n, const V3& wo, const V3& wi);
// Direct light at a surface from every light, with shadows. With an rng, lights
// are sampled over their size (soft shadows); without, from their centre.
V3 directLight(const World& w, const Surface& s, const V3& n, const V3& wo, Rng* rng, bool shadows);

raster::Image rasterize(const World& w, const CameraF& cam, const RenderOptions& o);
raster::Image pathTrace(const World& w, const CameraF& cam, const RenderOptions& o);

// Tone-maps linear colours into the final image; coverage (0..1 per pixel) blends
// with the background colour, or becomes alpha for transparent skies.
raster::Image toImage(const std::vector<V3>& color, const std::vector<float>& coverage, int W, int H, const World& w, double exposure);

// Linear HDR -> 8-bit sRGB, with exposure in stops and a filmic curve.
std::uint8_t toByte(float linear);
float filmic(float x);

int threadCount(int requested);
// Runs fn(row) for rows 0..rows-1 across `threads` threads.
void parallelRows(int rows, int threads, const std::function<void(int)>& fn);

}  // namespace wetwaresuite::render::detail
