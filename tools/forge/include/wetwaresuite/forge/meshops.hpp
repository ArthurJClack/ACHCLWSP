// Mesh helpers for game assets that libs/mesh doesn't have: UV projection,
// up-axis conversion, ratio-based decimation.
#pragma once

#include <cstddef>
#include <optional>
#include <string>

#include "wetwaresuite/mesh/mesh.hpp"
#include "wetwaresuite/raster/image.hpp"

namespace wetwaresuite::forge {

enum class Projection { Planar, Box, Cylindrical, Spherical };

// Projects simple UVs. `axis` (0 = X, 1 = Y, 2 = Z) is the projection direction
// for planar and the pole axis for cylindrical and spherical; box picks each
// triangle's dominant axis. UVs fill 0..1 on the longest side (box keeps one
// texel density on every side) and are then multiplied by `scale`. Vertices
// are split where triangles need different UVs (box sides, the wrap seam).
void generateUvs(mesh::Mesh& m, Projection projection, int axis = 2, double scale = 1.0);

enum class AxisConversion { None, YUpToZUp, ZUpToYUp };

// Rotates the mesh between Y-up (most game engines, OBJ/FBX exports) and
// Z-up (this library, CAD, Blender). Y-up -> Z-up turns +Y into +Z.
void convertAxes(mesh::Mesh& m, AxisConversion conversion);
std::optional<AxisConversion> parseAxes(const std::string& name);  // none, y_up_to_z_up, z_up_to_y_up

// Smooth vertex normals where faces meet at less than `creaseDegrees`, split
// (hard) edges where they meet more sharply. Vertices are matched by position,
// so it smooths across UV seams and unwelded files without merging vertices
// (UVs survive).
void recomputeNormals(mesh::Mesh& m, double creaseDegrees = 60.0);

// Decimates to about `ratio` of the triangles (0..1], then recomputes normals
// with `creaseDegrees` when `normals` is set (decimation drops normals and UVs).
mesh::Mesh decimateRatio(const mesh::Mesh& m, double ratio, bool normals = true, double creaseDegrees = 60.0);
mesh::Mesh decimateTo(const mesh::Mesh& m, std::size_t triangles, bool normals = true, double creaseDegrees = 60.0);

// Texture baking: details of a high-poly mesh painted into a low-poly mesh's UV layout,
// as in Blender, Substance or xNormal. For every texel the low mesh covers, a ray along
// the low surface's normal finds the high surface; the normal map stores the high
// normal in the low mesh's tangent space (OpenGL style, green up, what Godot and Blender
// read), the AO map how much of the sky the high surface sees.
struct BakeOptions {
    int size = 512;            // texture width and height, px
    bool normal = true, ao = true;
    int aoSamples = 24;        // rays per texel
    double distance = 0;       // how far from the low surface to look for the high one (0: 4% of the size)
    double aoDistance = 0;     // how far a ray can be blocked for AO (0: 15% of the size)
    int padding = 4;           // px of colour grown past each UV island's edge, against seams
    bool directXGreen = false; // flip green (DirectX / Unreal style)
};
struct BakeResult {
    raster::Image normalMap, aoMap;
    std::size_t texels = 0, missed = 0;  // texels covered, and those where no high surface was found
};
// The low mesh needs UVs (inside 0..1); throws std::runtime_error with a readable message.
BakeResult bakeMaps(const mesh::Mesh& low, const mesh::Mesh& high, const BakeOptions& options);

}  // namespace wetwaresuite::forge
