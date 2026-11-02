// Edge blends (fillet, chamfer) and shells on the mesh kernel. A fillet or
// chamfer is a boolean with a cutter built along each sharp edge (subtracted
// on convex edges, added on concave ones); a shell subtracts the body offset
// inwards by the wall thickness, with chosen faces pushed out so they open.
#pragma once

#include <functional>
#include <string>
#include <vector>

#include "wetwaresuite/mesh/csg.hpp"
#include "wetwaresuite/mesh/mesh.hpp"

namespace wetwaresuite::cad::detail {

using Csg = std::function<mesh::Mesh(const mesh::Mesh&, const mesh::Mesh&, mesh::BooleanOp)>;

// A straight edge where two flat faces meet at an angle (collinear pieces merged).
struct SharpEdge {
    mesh::Vec3 a, b;
    mesh::Vec3 n1, n2;  // outward normals of the two faces
    bool convex = true;
    double angle = 0;  // between the face normals, degrees (90 for a box edge)
};

// Edges whose faces meet at minAngle degrees or more.
std::vector<SharpEdge> sharpEdges(const mesh::Mesh& body, double minAngle);

// Picks edges by name (all, outer/convex, inner/concave, top, bottom, vertical,
// horizontal, x, y, z; several are combined) or, when `near` is given, the
// edge closest to each point. Throws std::invalid_argument on an unknown name.
std::vector<SharpEdge> pickEdges(const mesh::Mesh& body, const std::vector<SharpEdge>& edges, const std::vector<std::string>& names,
                                 const std::vector<mesh::Vec3>& near);

// Rounds (or bevels, chamfer=true) the given edges by `size`. Throws
// std::runtime_error when the result falls apart (size too large).
mesh::Mesh blendEdges(const mesh::Mesh& body, const std::vector<SharpEdge>& edges, double size, bool chamfer, int segments,
                      const Csg& csg);

// Hollows the body to `thickness` walls; faces facing one of `open` (unit
// directions) on the body's outside in that direction are removed.
mesh::Mesh shell(const mesh::Mesh& body, double thickness, const std::vector<mesh::Vec3>& open, const Csg& csg);

// A face of the body moved along its own normals by distance (out when positive, in when negative),
// flat or curved: the smooth patch around the triangle nearest `at` (neighbours bending less than
// 20 degrees), thickened into a closed slab and added to or cut from the body. Throws
// std::runtime_error when there is no face there or the result falls apart.
mesh::Mesh offsetFace(const mesh::Mesh& body, const mesh::Vec3& at, double distance, const Csg& csg);

// Direction for an open-face name: top bottom front back left right, +x -x +y -y +z -z.
bool directionByName(const std::string& name, mesh::Vec3& out);

}  // namespace wetwaresuite::cad::detail
