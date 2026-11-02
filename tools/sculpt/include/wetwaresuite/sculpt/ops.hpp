// Sculpting and retopology algorithms on libs/mesh meshes, free of the
// command layer so they can be tested and reused directly. Every function
// expects a welded mesh (one vertex per position): sculpt documents always are.
//
// Memory is bounded on purpose: anything that grows a mesh refuses to go past
// kMaxTriangles, and voxel grids past kMaxResolution cells a side, with an
// error that says what to lower.
#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <vector>

#include "wetwaresuite/mesh/mesh.hpp"
#include "wetwaresuite/mesh/render.hpp"
#include "wetwaresuite/raster/image.hpp"

namespace wetwaresuite::sculpt {

using mesh::Mesh;
using mesh::Vec3;

constexpr std::size_t kMaxTriangles = 1'000'000;  // subdivide, new, remesh and retopo results
constexpr int kMaxResolution = 160;               // voxel cells along the longest side
constexpr std::size_t kMaxDabs = 4000;            // dabs per stroke

// Per-vertex mask: 0 = free, 1 = fully protected from edits. Empty = nothing masked.
using Mask = std::vector<float>;

// ---- primitives ----

enum class Shape { Sphere, Cube, Plane };
std::optional<Shape> parseShape(const std::string& name);
// Triangles a primitive has at `level` (sphere 20*4^L, cube 12*4^L, plane 2*4^L).
std::size_t primitiveTriangles(Shape shape, int level);
// Centred on the origin. Sphere: icosphere of `size` diameter; cube: `size` edge,
// 2^level cells per edge on each face; plane: size x size in XY facing +Z.
// Throws std::invalid_argument past kMaxTriangles or for size <= 0.
Mesh primitive(Shape shape, double size, int level);

// ---- brushes ----

enum class Brush { Draw, Inflate, Smooth, Flatten, Pinch, Grab, Crease };
enum class Falloff { Smooth, Linear, Constant, Sharp };
std::optional<Brush> parseBrush(const std::string& name);
std::optional<Falloff> parseFalloff(const std::string& name);
const std::vector<std::string>& brushNames();
const std::vector<std::string>& falloffNames();
// Weight at distance fraction t (0 = centre, 1 = edge of the radius).
double falloffWeight(Falloff f, double t);

// Mirror bits: 1 = across X (x -> -x), 2 = Y, 4 = Z. Parses "x", "xy", "none".
int parseSymmetry(const std::string& text);

struct BrushSettings {
    Brush brush = Brush::Draw;
    double radius = 1;
    double strength = 0.5;  // 0..1 typical; draw/inflate/crease move up to radius * strength * 0.15 per dab
    Falloff falloff = Falloff::Smooth;
    double spacing = 0.25;  // dab spacing as a fraction of radius
    bool invert = false;    // draw/inflate dig in, pinch pushes out, crease raises a ridge
    int symmetry = 0;       // mirror bits, mirrored about the origin planes
    std::vector<double> pressure;  // optional, one per path point: scales the strength (a pen's pressure, 0..1)
};

struct StrokeStats {
    std::size_t dabs = 0;
    std::size_t moved = 0;    // distinct vertices that moved
    double maxOffset = 0;     // largest total displacement of one vertex
};

// Runs a stroke along `path` (world points). Dab centres are snapped onto the
// surface near them, except grab, whose first point picks the vertices and
// whose later points are where they're dragged to. Masked vertices move less.
StrokeStats stroke(Mesh& m, const Mask& mask, const std::vector<Vec3>& path, const BrushSettings& settings);

// ---- views (to aim strokes from a preview) ----

// Camera axes for a view, optionally turned by yaw (about Z) and pitch (about
// the camera's right axis) in degrees, exactly as preview() draws them.
struct ViewRay {
    mesh::View view = mesh::View::Iso;
    double yaw = 0, pitch = 0;
};
// The mesh as the renderer should draw it for a turned view (yaw/pitch 0 = unchanged).
Mesh turnedForView(const Mesh& m, const ViewRay& v);
// Point on the surface under (u, v) of a square preview (0..1, from the top
// left), or nothing if that pixel shows background.
std::optional<Vec3> screenToSurface(const Mesh& m, const ViewRay& v, double u, double vv);
// Point at (u, v) on the plane through `anchor` facing the camera (for grab targets).
Vec3 screenToPlane(const Mesh& m, const ViewRay& v, double u, double vv, const Vec3& anchor);

// ---- whole-mesh operations ----

// Laplacian (shrinks) or Taubin (lambda/mu, keeps volume) smoothing. Boundary
// vertices stay put. Returns the largest displacement.
double smooth(Mesh& m, const Mask& mask, int iterations, double strength, bool taubin);

// Splits every triangle into four. Loop smooths (limit surface of a subdivision
// surface); midpoint keeps the shape exactly. The mask is carried over
// (new vertices average their edge). Throws std::length_error past kMaxTriangles.
Mesh subdivide(const Mesh& m, bool loop, Mask* mask = nullptr);

// Rebuilds the surface on a voxel grid with `resolution` cells along the longest
// side (surface nets, so faces come out as quads split into triangle pairs:
// triangles 2k and 2k+1 form quad k). Closed meshes give their outside
// (overlapping parts merge); open meshes become a thin shell around the surface.
// Throws std::length_error past kMaxResolution / kMaxTriangles.
Mesh voxelRemesh(const Mesh& m, int resolution, bool* shell = nullptr);

// Quadric edge collapse (Garland-Heckbert) down to about `targetTriangles`.
// Keeps open edges in place and refuses collapses that flip faces. Vertices
// with mask >= 0.5 are locked. `mask` is remapped onto the result.
Mesh decimateQuadric(const Mesh& m, std::size_t targetTriangles, Mask* mask = nullptr);

// Quad-dominant retopology: voxel remesh sized for about `targetQuads`, then
// `relax` rounds of tangential relaxation projected back onto the original
// surface. Output keeps the quad pairing of voxelRemesh.
Mesh retopologize(const Mesh& m, std::size_t targetQuads, int relax = 8);

// Mirrors one half onto the other across the plane through the origin
// perpendicular to `axis` (0 = X). positiveToNegative copies the +side over the
// -side. Triangles crossing the plane are clipped; the seam is welded.
Mesh symmetrize(const Mesh& m, int axis, bool positiveToNegative);

// Heightmap displacement along vertex normals: offset = amount * (luma - midlevel).
enum class Mapping { Auto, Uv, Planar, Cylindrical, Spherical };
std::optional<Mapping> parseMapping(const std::string& name);
struct DisplaceSettings {
    Mapping mapping = Mapping::Auto;  // auto: the mesh's UVs if it has them, else planar
    int axis = 2;                     // planar projection direction / cylinder and sphere pole
    double amount = 1;
    double midlevel = 0.5;
    double tile = 1;                  // repeats of the image across the mapping
};
// Returns the largest offset. Throws std::invalid_argument when mapping=uv and there are no UVs.
double displace(Mesh& m, const Mask& mask, const raster::Image& height, const DisplaceSettings& s);

// ---- mask shapes ----

enum class MaskMode { Set, Add, Subtract, Intersect };
std::optional<MaskMode> parseMaskMode(const std::string& name);
// Sphere: 1 inside `radius` of centre, fading over `softness` (model units) outside.
void maskSphere(const Mesh& m, Mask& mask, const Vec3& center, double radius, double softness, MaskMode mode);
// Plane: 1 on the side `normal` points to, fading over `softness` across the plane.
void maskPlane(const Mesh& m, Mask& mask, const Vec3& point, const Vec3& normal, double softness, MaskMode mode);
// Grows (steps > 0) or shrinks (< 0) the mask by whole edge rings, or blurs it.
void growMask(const Mesh& m, Mask& mask, int steps);
void blurMask(const Mesh& m, Mask& mask, int iterations);

// ---- stats ----

struct Stats {
    std::size_t triangles = 0, vertices = 0, edges = 0, openEdges = 0, nonManifoldEdges = 0;
    double minEdge = 0, meanEdge = 0, maxEdge = 0;
    double area = 0, volume = 0;
    bool watertight = false;
    long long euler = 0;  // V - E + F
};
Stats stats(const Mesh& m);

}  // namespace wetwaresuite::sculpt
