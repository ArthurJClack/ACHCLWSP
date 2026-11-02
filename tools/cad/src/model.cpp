// The feature tree: feature types, regeneration with per-feature caching,
// snapshot undo and the .wwcad file format.
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <sstream>

#include <nlohmann/json.hpp>

#include "wetwaresuite/cad/cad.hpp"
#include "wetwaresuite/mesh/csg.hpp"
#include "wetwaresuite/mesh/io.hpp"
#include "wetwaresuite/mesh/primitives.hpp"
#include "blend.hpp"
#include "build.hpp"
#include "sheetmetal.hpp"
#include "repair.hpp"

namespace wetwaresuite::cad {

using mesh::Mat4;
using mesh::Mesh;
using mesh::Vec2;
using mesh::Vec3;
using json = nlohmann::json;

// ---- feature types ------------------------------------------------------------

namespace {

FieldDef num(std::string name, std::string help, std::string fallback = {}, bool required = false) {
    return {std::move(name), FieldKind::Number, std::move(help), std::move(fallback), required, {}};
}
FieldDef integer(std::string name, std::string help, std::string fallback = {}, bool required = false) {
    return {std::move(name), FieldKind::Integer, std::move(help), std::move(fallback), required, {}};
}
FieldDef ref(std::string name, std::string help) { return {std::move(name), FieldKind::Ref, std::move(help), {}, true, {}}; }
FieldDef choice(std::string name, std::string help, std::vector<std::string> choices, std::string fallback) {
    return {std::move(name), FieldKind::Choice, std::move(help), std::move(fallback), false, std::move(choices)};
}
FieldDef flag(std::string name, std::string help, bool fallback) {
    return {std::move(name), FieldKind::Bool, std::move(help), fallback ? "true" : "false", false, {}};
}
FieldDef words(std::string name, std::string help, std::string fallback) {
    return {std::move(name), FieldKind::Text, std::move(help), std::move(fallback), false, {}};
}
FieldDef optRef(std::string name, std::string help) { return {std::move(name), FieldKind::Ref, std::move(help), {}, false, {}}; }
FieldDef refs(std::string name, std::string help) { return {std::move(name), FieldKind::Refs, std::move(help), {}, true, {}}; }
FieldDef points(std::string name, std::string help, bool required) {
    return {std::move(name), FieldKind::Points, std::move(help), {}, required, {}};
}

std::vector<FieldDef> with(std::vector<FieldDef> a, const std::vector<FieldDef>& b) {
    a.insert(a.end(), b.begin(), b.end());
    return a;
}

std::vector<FieldDef> placement() {
    return {num("x", "move along X", "0"),         num("y", "move along Y", "0"),
            num("z", "move along Z", "0"),         num("rx", "rotate about X, degrees (applied first)", "0"),
            num("ry", "rotate about Y, degrees", "0"), num("rz", "rotate about Z, degrees (applied last)", "0")};
}

FieldDef planeField() {
    return choice("plane", "sketch plane: xy extrudes along +Z, xz (x->X, y->Z) along +Y, yz (x->Y, y->Z) along +X",
                  {"xy", "xz", "yz"}, "xy");
}

std::vector<FieldDef> combine() {
    return {choice("operation", "new body, join (add to target), cut (remove from target) or intersect (keep the overlap)",
                   {"join", "cut", "new", "intersect"}, "join"),
            optRef("target", "body to join, cut or intersect (default: the last body in the tree)")};
}

FieldDef faceField(const std::string& of) {
    return words("face", "which flat face of " + of + ": top, bottom, front, back, left, right (+x -x +y -y +z -z), or x,y,z near it", "top");
}

// Sketches, datums and the features built from sketches (built in features.cpp).
void addSketchTypes(std::vector<FeatureType>& t) {
    t.push_back({"sketch", "Sketch",
                 "Start a 2D sketch: on a standard plane (xy, xz, yz) shifted by offset, on a flat face of a body (on=body "
                 "face=top) or on a datum plane (on=datum_plane1). Add geometry with cad.sketch_line, sketch_rect, sketch_circle, "
                 "sketch_arc, sketch_polygon, sketch_spline; constrain it with cad.sketch_constrain and cad.sketch_dimension. "
                 "Sketch x/y follow the world axes the plane is closest to (top face: X and Y). Then cad.sketch_extrude, "
                 "sketch_revolve, loft or sweep it.",
                 {choice("plane", "standard plane: xy (normal +Z), xz (x->X, y->Z, normal +Y), yz (x->Y, y->Z, normal +X)", {"xy", "xz", "yz"}, "xy"),
                  num("offset", "shift the plane along its normal", "0"),
                  optRef("on", "sketch on a flat face of this body, or on this datum plane"), faceField("on"),
                  integer("segments", "segments per full circle when arcs and circles become profiles", "64")}});
    t.push_back({"datum_plane", "Datum Plane",
                 "A reference plane for sketches and mates: a standard plane or a face of a body (on, face), shifted by offset "
                 "along its normal and tilted by angle about its own u or v axis; or through three points.",
                 {choice("plane", "standard plane to start from", {"xy", "xz", "yz"}, "xy"), num("offset", "shift along the normal", "0"),
                  optRef("on", "start from a flat face of this body (or another datum plane)"), faceField("on"),
                  num("angle", "tilt in degrees about the axis below, through the plane's origin", "0"),
                  choice("axis", "tilt axis: the plane's own u (sketch x) or v (sketch y)", {"u", "v"}, "u"),
                  words("points", "instead: three points x,y,z,x,y,z,x,y,z the plane passes through", "")}});
    t.push_back({"datum_axis", "Datum Axis",
                 "A reference axis for revolves, patterns and concentric mates: through cx/cy/cz along x, y or z; through two "
                 "points; or the axis of a hole, cylinder or revolve (on, index).",
                 {choice("axis", "direction", {"z", "x", "y"}, "z"), num("cx", "a point on it: X", "0"), num("cy", "Y", "0"),
                  num("cz", "Z", "0"), words("points", "instead: two points x,y,z,x,y,z", ""),
                  optRef("on", "instead: take the axis of this feature (hole, cylinder, revolve, sketch circle)"),
                  integer("index", "which of its axes (patterns of holes have several)", "1")}});
    t.push_back({"sketch_extrude", "Extrude Sketch Profile",
                 "Extrude a sketch's closed profiles (inner loops become holes) as a new body, or join, cut or intersect it with "
                 "a body. end: blind (depth), symmetric (depth split across the plane), through_all, up_to_face (the plane of "
                 "a face of the target, up_to). depth2 adds a second direction; draft tapers the walls; reverse flips the "
                 "direction. A cut on a face sketch goes into the body.",
                 with({ref("sketch", "the sketch to extrude"), num("depth", "extrusion distance", "10"),
                       choice("end", "end condition", {"blind", "symmetric", "through_all", "up_to_face"}, "blind"),
                       num("depth2", "blind: also extrude this far the other way", "0"),
                       words("up_to", "up_to_face: face of the target (top, bottom, ..., or x,y,z) to stop at", "top"),
                       flag("reverse", "extrude the other way", false), num("draft", "taper angle in degrees (inwards)", "0"),
                       words("regions", "which profile regions, 1,2,... (cad.sketch_info lists them; empty = all)", "")},
                      combine())});
    t.push_back({"sketch_revolve", "Revolve Sketch Profile",
                 "Revolve a sketch's closed profiles about an axis in the sketch: a line of the sketch (often a construction "
                 "centerline) or the sketch's own x or y axis. angle < 360 leaves flat ends; symmetric spreads it both ways.",
                 with({ref("sketch", "the sketch to revolve"),
                       words("axis", "a line id in the sketch (e.g. l5), or x / y for the sketch's axes", "y"),
                       num("angle", "degrees, (0, 360]", "360"), flag("symmetric", "spread the angle both ways", false),
                       flag("reverse", "revolve the other way", false), integer("segments", "segments for a full turn", "64")},
                      combine())});
    t.push_back({"loft", "Loft",
                 "Blend between two or more sketch profiles (one closed outline each) in order: straight between two, smooth "
                 "through three or more. The outlines are matched point by point from where they are closest.",
                 with({refs("sections", "sketches in order, comma-separated (at least 2)"),
                       flag("ruled", "straight between every pair of sections", false),
                       integer("samples", "points around each section, 0 = automatic", "0")},
                      combine())});
    t.push_back({"sweep", "Sweep",
                 "Sweep a sketch's closed profile along the path drawn in another sketch (one chain of lines, arcs and "
                 "splines, open or closed). The profile keeps its angle to the path; twist turns it along the way.",
                 with({ref("profile", "sketch with the closed profile"), ref("path", "sketch with the path"),
                       num("twist", "total twist in degrees", "0")},
                      combine())});
    t.push_back({"draft", "Draft",
                 "Tilt the walls of a body (faces parallel to the pull direction) by angle, so it releases from a mould: "
                 "walls lean in going along pull, holes open up. neutral is where the walls keep their size (default: the "
                 "body's start along pull).",
                 {ref("target", "body to draft"), num("angle", "draft angle in degrees", {}, true),
                  choice("pull", "pull direction", {"+z", "-z", "+x", "-x", "+y", "-y"}, "+z"),
                  num("neutral", "position of the neutral plane along pull (default: the body's start)")}});
    t.push_back({"rib", "Rib",
                 "Add a rib (gusset) to a body from an open line in a sketch: the line is thickened across the sketch plane "
                 "and filled towards the body until it meets it. flip fills to the other side of the line.",
                 {ref("sketch", "sketch with one open chain of lines"), ref("target", "body the rib joins"),
                  num("thickness", "rib thickness", {}, true), flag("flip", "fill to the other side of the line", false)}});
    t.push_back({"thread", "Thread",
                 "Cut a modeled thread into a body along an axis: external (on a shaft: cuts the grooves) or internal (in a "
                 "hole: the hole should be the minor diameter). Metric 60 degree profile. Drawings show it as a callout "
                 "(M8x1.25); cosmetic=true only adds the callout and axis and leaves the solid plain (fast, and what "
                 "most printed and machined parts want).",
                 {ref("target", "body to thread"), num("diameter", "major (nominal) diameter", {}, true),
                  num("pitch", "distance per turn", {}, true), num("length", "threaded length", {}, true),
                  num("x", "start of the thread on its axis: X", "0"), num("y", "Y", "0"), num("z", "Z", "0"),
                  choice("axis", "direction the thread runs from the start point", {"z", "-z", "x", "-x", "y", "-y"}, "z"),
                  flag("internal", "an internal thread (in a hole)", false), flag("left", "left-handed", false),
                  flag("cosmetic", "callout only: don't cut the thread into the solid", false),
                  integer("segments", "segments per turn", "32")}});
}

std::vector<FeatureType> buildTypes() {
    const std::string pos = " Placed by x/y/z and rotated by rx/ry/rz (degrees, about its own origin).";
    std::vector<FeatureType> t;
    t.push_back({"box", "Box", "Add a box (cuboid) with exact sizes: width along X, depth along Y, height along Z. By default "
                 "its min corner sits at x/y/z; center=true centres it there instead." + pos,
                 with({num("width", "size along X", {}, true), num("depth", "size along Y", {}, true),
                       num("height", "size along Z", {}, true), flag("center", "centre the box on x/y/z", false)},
                      placement())});
    t.push_back({"cylinder", "Cylinder",
                 "Add a cylinder along Z. Give diameter or radius. Its base circle is centred on x/y/z (center=true "
                 "centres it in height too)." + pos,
                 with({num("diameter", "diameter (or give radius)"), num("radius", "radius (or give diameter)"),
                       num("height", "height along Z", {}, true), integer("segments", "sides of the polygon", "64"),
                       flag("center", "centre in height as well", false)},
                      placement())});
    t.push_back({"cone", "Cone",
                 "Add a cone or frustum along Z from bottom diameter d1 to top diameter d2 (either may be 0; or give "
                 "r1/r2). Base centred on x/y/z." + pos,
                 with({num("d1", "bottom diameter"), num("d2", "top diameter"), num("r1", "bottom radius (instead of d1)"),
                       num("r2", "top radius (instead of d2)"), num("height", "height along Z", {}, true),
                       integer("segments", "sides of the polygon", "64"), flag("center", "centre in height as well", false)},
                      placement())});
    t.push_back({"sphere", "Sphere", "Add a sphere centred on x/y/z. Give diameter or radius." + pos,
                 with({num("diameter", "diameter (or give radius)"), num("radius", "radius (or give diameter)"),
                       integer("segments", "segments around Z", "64"), integer("rings", "rings from pole to pole", "32")},
                      placement())});
    t.push_back({"torus", "Torus", "Add a torus (ring) around Z centred on x/y/z: major = distance to the tube centre, "
                 "minor = tube radius." + pos,
                 with({num("major", "ring radius to the tube centre", {}, true), num("minor", "tube radius", {}, true),
                       integer("segments", "segments around the ring", "64"),
                       integer("tube_segments", "segments around the tube", "24")},
                      placement())});
    t.push_back({"extrude", "Extrude Sketch",
                 "Extrude a 2D polygon sketch by height. points is x,y,x,y,... (each may be an expression, e.g. "
                 "\"0,0, w,0, w,h, 0,h\"); hole_points cuts one hole, cad.add_hole adds more." + pos,
                 with({points("points", "outer outline x,y,x,y,... (at least 3 points)", true),
                       num("height", "extrusion distance", {}, true),
                       points("hole_points", "optional hole outline x,y,x,y,...", false), planeField()},
                      placement())});
    t.push_back({"revolve", "Revolve Sketch",
                 "Revolve a closed 2D profile about the Z axis: points are r,z pairs (r >= 0 is the distance from the "
                 "axis). angle < 360 gives a partial revolve with flat ends." + pos,
                 with({points("points", "profile r,z,r,z,... (at least 3 points)", true),
                       num("angle", "sweep in degrees, (0, 360]", "360"), integer("segments", "segments for a full turn", "64")},
                      placement())});
    t.push_back({"extrude_rect", "Extrude Rectangle",
                 "Extrude a rectangle (radius > 0 rounds its corners) by height. Its min corner sits at x/y (or its "
                 "centre with center=true)." + pos,
                 with({num("width", "size along sketch x", {}, true), num("depth", "size along sketch y", {}, true),
                       num("height", "extrusion distance", {}, true), num("radius", "corner radius, 0 = sharp", "0"),
                       integer("segments", "segments per rounded corner", "8"),
                       flag("center", "centre the rectangle on x/y", false), planeField()},
                      placement())});
    t.push_back({"extrude_circle", "Extrude Circle",
                 "Extrude a circle (or a ring with inner_diameter) centred on x/y by height." + pos,
                 with({num("diameter", "outer diameter (or give radius)"), num("radius", "outer radius (or give diameter)"),
                       num("height", "extrusion distance", {}, true), num("inner_diameter", "makes a tube; 0 = solid", "0"),
                       integer("segments", "sides of the polygon", "64"), planeField()},
                      placement())});
    t.push_back({"extrude_slot", "Extrude Slot",
                 "Extrude a slot (stadium: a rectangle with round ends) centred on x/y, long along sketch x, by height. "
                 "length is the overall length, width the slot width." + pos,
                 with({num("length", "overall length along sketch x", {}, true), num("width", "slot width", {}, true),
                       num("height", "extrusion distance", {}, true), integer("segments", "segments per round end", "32"),
                       planeField()},
                      placement())});
    t.push_back({"imported", "Imported Mesh", "A fixed mesh body imported from a file (cad.import or cad.open)." + pos,
                 with({num("scale", "uniform scale", "1")}, placement())});
    t.push_back({"boolean", "Boolean",
                 "Combine two earlier features into one body: union, difference (a minus b) or intersection. Both "
                 "inputs stop being separate bodies.",
                 {choice("op", "union, difference (a - b) or intersection", {"union", "difference", "intersection"}, "union"),
                  ref("a", "first feature (the one kept for difference)"), ref("b", "second feature (the cutter for difference)")}});
    t.push_back({"hole", "Hole",
                 "Drill a round hole into a feature: diameter, depth (0 = through all), along axis (-z drills "
                 "downwards). x/y/z is the entry point (0 if left out), except that the coordinate along the axis "
                 "defaults to the entry face of the target. Optional countersink (countersink_diameter, countersink_angle) or "
                 "counterbore (counterbore_diameter, counterbore_depth). Patterning or mirroring a hole repeats the hole.",
                 {ref("target", "feature to drill"), num("diameter", "hole diameter", {}, true),
                  num("depth", "hole depth, 0 = through all", "0"),
                  choice("axis", "drilling direction", {"-z", "z", "-x", "x", "-y", "y"}, "-z"),
                  num("x", "entry point X"), num("y", "entry point Y"), num("z", "entry point Z"),
                  num("countersink_diameter", "countersink top diameter, 0 = none", "0"),
                  num("countersink_angle", "countersink included angle in degrees", "90"),
                  num("counterbore_diameter", "counterbore diameter, 0 = none", "0"),
                  num("counterbore_depth", "counterbore depth", "0"), integer("segments", "sides of the hole polygon", "64")}});
    t.push_back({"pattern_linear", "Linear Pattern",
                 "Repeat a feature count times, each copy offset by dx/dy/dz; count2 with dx2/dy2/dz2 makes a grid. "
                 "On a hole feature it repeats the hole.",
                 {ref("target", "feature to repeat"), integer("count", "number of copies including the original", {}, true),
                  num("dx", "step along X", "0"), num("dy", "step along Y", "0"), num("dz", "step along Z", "0"),
                  integer("count2", "copies in the second direction (grid)", "1"), num("dx2", "second step X", "0"),
                  num("dy2", "second step Y", "0"), num("dz2", "second step Z", "0")}});
    t.push_back({"pattern_circular", "Circular Pattern",
                 "Repeat a feature count times around an axis through cx/cy/cz. angle=360 spaces them evenly; a smaller "
                 "angle spreads them from the original to angle. On a hole feature it repeats the hole.",
                 {ref("target", "feature to repeat"), integer("count", "number of copies including the original", {}, true),
                  num("angle", "total sweep in degrees", "360"), choice("axis", "rotation axis", {"z", "x", "y"}, "z"),
                  num("cx", "axis point X", "0"), num("cy", "axis point Y", "0"), num("cz", "axis point Z", "0")}});
    t.push_back({"mirror", "Mirror",
                 "Mirror a feature across a plane (yz flips X, xz flips Y, xy flips Z) at offset along its normal. "
                 "keep=true keeps the original too and joins them. On a hole feature it mirrors the hole.",
                 {ref("target", "feature to mirror"), choice("plane", "mirror plane", {"yz", "xz", "xy"}, "yz"),
                  num("offset", "plane position along its normal", "0"), flag("keep", "keep the original as well", true)}});
    t.push_back({"transform", "Transform",
                 "Move, rotate and scale an earlier feature's body: scale and rotate (rx, ry, rz degrees) about the "
                 "pivot cx/cy/cz, then move by x/y/z.",
                 {ref("target", "feature to transform"), num("x", "move X", "0"), num("y", "move Y", "0"),
                  num("z", "move Z", "0"), num("rx", "rotate about X", "0"), num("ry", "rotate about Y", "0"),
                  num("rz", "rotate about Z", "0"), num("scale", "uniform scale", "1"), num("cx", "pivot X", "0"),
                  num("cy", "pivot Y", "0"), num("cz", "pivot Z", "0")}});
    t.push_back({"fillet", "Fillet",
                 "Round the sharp edges of a feature with radius: outer edges are cut round, inner edges filled round. "
                 "Pick edges by kind (edges=top,vertical) or by points near them (near=0,0,5). Where three rounded outer edges "
                 "meet, the corner becomes a ball.",
                 {ref("target", "feature to round"), num("radius", "fillet radius", {}, true), words("edges", "which edges: all, outer, inner, top, bottom, vertical, horizontal, x, y, z (comma-separated, combined)", "all"),
                  words("near", "instead of edges: points x,y,z,x,y,z,... picking the edge closest to each (cad.edges lists them)", ""),
                  num("angle", "only edges whose faces meet at this many degrees or more", "30"),
                  integer("segments", "segments per quarter circle", "8")}});
    t.push_back({"chamfer", "Chamfer",
                 "Bevel the sharp edges of a feature: distance is measured along both faces from the edge. Outer edges are cut, "
                 "inner edges filled. Pick edges like fillet does.",
                 {ref("target", "feature to bevel"), num("distance", "chamfer size along each face", {}, true), words("edges", "which edges: all, outer, inner, top, bottom, vertical, horizontal, x, y, z (comma-separated, combined)", "all"),
                  words("near", "instead of edges: points x,y,z,x,y,z,... picking the edge closest to each (cad.edges lists them)", ""),
                  num("angle", "only edges whose faces meet at this many degrees or more", "30")}});
    t.push_back({"offset_face", "Offset Face",
                 "Move one face of a feature along its own normals by distance: out (positive) adds material, in "
                 "(negative) cuts it. Works on curved faces too (a hole's wall, a boss's side, a fillet), which push/pull "
                 "can't. face is a point x,y,z on the face (cad.pick gives one).",
                 {ref("target", "feature whose face moves"), words("face", "a point x,y,z on the face", ""),
                  num("distance", "how far: positive out, negative in", {}, true)}});
    t.push_back({"shell", "Shell",
                 "Hollow a feature out, leaving walls of thickness. open removes the outermost faces facing those ways "
                 "(top, bottom, front, back, left, right or +x -x +y -y +z -z; comma-separated), e.g. open=top makes a box "
                 "into a tray.",
                 {ref("target", "feature to hollow"), num("thickness", "wall thickness", {}, true),
                  words("open", "faces to remove: top, bottom, front, back, left, right, +x, -x, ...", "")}});
    t.push_back({"sheet_base", "Sheet Metal Base",
                 "A flat plate of sheet metal: width x depth from the origin (or an outline of points), thickness thick. "
                 "Flanges bend off its edges; cad.sheet_flat unfolds the part for a laser cutter.",
                 {num("width", "plate width (x)", "100"), num("depth", "plate depth (y)", "60"),
                  points("points", "instead of width/depth: the outline x,y,x,y,... (a straight-edged shape)", false),
                  num("thickness", "sheet thickness", "1.5"), num("radius", "default inside bend radius (default: the thickness)", ""),
                  num("k_factor", "where the neutral line sits in a bend, 0 (inside) to 1 (outside); 0.44 suits most steel", "0.44")}});
    t.push_back({"sheet_flange", "Sheet Metal Flange",
                 "Bend a flange off a straight edge of the sheet's base plate: up or down by angle, through an inside "
                 "radius, then straight on for length. target is the base or another flange of the same part.",
                 {ref("target", "the sheet_base (or a flange of it)"),
                  words("edge", "which edge: front, back, left, right, or a point x,y near it", "front"),
                  num("length", "flange length past the bend", "20"), num("angle", "bend angle in degrees (90 = square)", "90"),
                  choice("direction", "bend up or down", {"up", "down"}, "up"),
                  num("radius", "inside bend radius (default: the base's)", "")}});
    addSketchTypes(t);
    return t;
}

std::string fmtExact(double v) {
    char buf[40];
    std::snprintf(buf, sizeof buf, "%.17g", v);
    return buf;
}

bool isHolePointsKey(const std::string& key) {
    if (key.rfind("hole_points_", 0) != 0 || key.size() == 12) return false;
    return std::all_of(key.begin() + 12, key.end(), [](char c) { return c >= '0' && c <= '9'; });
}

}  // namespace

const FieldDef* FeatureType::field(const std::string& name) const {
    for (const auto& f : fields) {
        if (f.name == name) return &f;
    }
    if (type == "extrude" && isHolePointsKey(name)) return field("hole_points");
    return nullptr;
}

const std::vector<FeatureType>& featureTypes() {
    static const std::vector<FeatureType> types = buildTypes();
    return types;
}

const FeatureType* featureType(const std::string& type) {
    for (const auto& t : featureTypes()) {
        if (t.type == type) return &t;
    }
    return nullptr;
}

const std::vector<std::string>& unitNames() {
    static const std::vector<std::string> names = {"mm", "cm", "m", "in"};
    return names;
}

double unitsToCm(const std::string& units) {
    if (units == "cm") return 1.0;
    if (units == "m") return 100.0;
    if (units == "in") return 2.54;
    return 0.1;
}

// ---- document -----------------------------------------------------------------

std::string CadDoc::summary() const {
    std::string s = std::to_string(features.size()) + " features, " + std::to_string(params.size()) + " params, " +
                    std::to_string(bodies.size()) + " bodies, " + units;
    if (!mesh.empty()) s += ", " + mesh::describe(mesh);
    if (!path.empty()) s += ", " + path;
    return s;
}

std::vector<mesh::Line3> CadDoc::sketchLines() const {
    std::vector<mesh::Line3> lines;
    for (std::size_t i = 0; i < features.size(); ++i) {
        const Feature& f = features[i];
        if (f.type != "sketch" || f.suppressed) continue;
        const FeatureExtra* x = featureExtra(f.name);
        if (!x || !x->sketch) continue;
        // a sketch a later feature already uses has done its job: hide it, as Onshape does
        bool used = false;
        for (std::size_t j = i + 1; j < features.size() && !used; ++j) {
            if (features[j].suppressed) continue;
            for (const auto& [field, value] : features[j].values)
                if (value == f.name) used = true;
        }
        if (used) continue;
        const SolvedSketch& sk = *x->sketch;
        auto at = [&](const std::string& id) {
            auto it = sk.points.find(id);
            const mesh::Vec2 p = it == sk.points.end() ? mesh::Vec2{} : it->second;
            return sk.frame.at(p.x, p.y);
        };
        auto point2 = [&](const std::string& id) {
            auto it = sk.points.find(id);
            return it == sk.points.end() ? mesh::Vec2{} : it->second;
        };
        for (const SketchEntity& e : f.sketch.entities) {
            mesh::Line3 ln;
            ln.color = e.construction ? raster::Color{150, 150, 160, 255} : raster::Color{230, 110, 30, 255};
            if (e.kind == "line" && e.points.size() >= 2) {
                ln.points = {at(e.points[0]), at(e.points[1])};
            } else if (e.kind == "circle" && !e.points.empty()) {
                auto r = sk.radii.find(e.id);
                const double radius = r != sk.radii.end() ? r->second : e.radius;
                const mesh::Vec2 c = point2(e.points[0]);
                for (int k = 0; k < 64; ++k) {
                    const double a = 2 * mesh::kPi * k / 64;
                    ln.points.push_back(sk.frame.at(c.x + radius * std::cos(a), c.y + radius * std::sin(a)));
                }
                ln.closed = true;
            } else if (e.kind == "arc" && e.points.size() >= 3) {
                const mesh::Vec2 c = point2(e.points[0]), a = point2(e.points[1]), b = point2(e.points[2]);
                const double radius = std::hypot(a.x - c.x, a.y - c.y);
                const double a0 = std::atan2(a.y - c.y, a.x - c.x);
                double a1 = std::atan2(b.y - c.y, b.x - c.x);
                if (a1 <= a0) a1 += 2 * mesh::kPi;  // counter-clockwise from start to end
                for (int k = 0; k <= 48; ++k) {
                    const double t = a0 + (a1 - a0) * k / 48;
                    ln.points.push_back(sk.frame.at(c.x + radius * std::cos(t), c.y + radius * std::sin(t)));
                }
            } else if (e.kind == "spline" && e.points.size() >= 2) {
                for (const auto& id : e.points) ln.points.push_back(at(id));
                ln.closed = e.closed;
            }
            if (!ln.points.empty()) lines.push_back(std::move(ln));
        }
    }
    return lines;
}

mesh::RenderOptions CadDoc::viewOptions() const {
    mesh::RenderOptions o = previewOptions;
    o.lines = sketchLines();
    if (mesh.empty()) {
        // an empty part: the axes show which way is up and where the sketch plane is
        double size = 10;
        for (const auto& ln : o.lines)
            for (const auto& p : ln.points) size = std::max({size, std::fabs(p.x), std::fabs(p.y), std::fabs(p.z)});
        const double len = o.lines.empty() ? size : size * 0.3;
        o.lines.push_back({{{0, 0, 0}, {len, 0, 0}}, {220, 60, 60, 255}});
        o.lines.push_back({{{0, 0, 0}, {0, len, 0}}, {60, 170, 80, 255}});
        o.lines.push_back({{{0, 0, 0}, {0, 0, len}}, {60, 110, 220, 255}});
    }
    return o;
}

bool CadDoc::renderView(const OrbitView& view, int width, int height, std::vector<std::uint8_t>& rgba) const {
    return mesh::renderOrbit(mesh, view, width, height, rgba, viewOptions());
}

bool CadDoc::viewCamera(const OrbitView& view, int width, int height, ViewCamera& camera) const {
    camera = mesh::orbitCamera(mesh, view, width, height, viewOptions());
    return true;
}

const Parameter* CadDoc::findParam(const std::string& name) const {
    for (const auto& p : params) {
        if (p.name == name) return &p;
    }
    return nullptr;
}

int CadDoc::featureIndex(const std::string& name) const {
    for (std::size_t i = 0; i < features.size(); ++i) {
        if (features[i].name == name) return static_cast<int>(i);
    }
    return -1;
}

Feature* CadDoc::findFeature(const std::string& name) {
    int i = featureIndex(name);
    return i < 0 ? nullptr : &features[static_cast<std::size_t>(i)];
}

const Feature* CadDoc::findFeature(const std::string& name) const {
    int i = featureIndex(name);
    return i < 0 ? nullptr : &features[static_cast<std::size_t>(i)];
}

std::string CadDoc::newFeatureName(const std::string& type) {
    for (;;) {
        std::string name = type + std::to_string(++counters[type]);
        if (featureIndex(name) < 0) return name;
    }
}

namespace {

double evalWith(const CadDoc& doc, const std::string& expr, int depth) {
    if (depth > 64) throw ExprError("parameters refer to each other in a loop");
    return evaluate(expr, [&doc, depth](const std::string& name) -> std::optional<double> {
        const Parameter* p = doc.findParam(name);
        if (!p) return std::nullopt;
        try {
            return evalWith(doc, p->expr, depth + 1);
        } catch (const ExprError& e) {
            const std::string what = e.what();
            if (what.rfind("parameter ", 0) == 0 || what.find("loop") != std::string::npos) throw;
            throw ExprError("parameter " + name + ": " + what);
        }
    });
}

}  // namespace

double CadDoc::paramValue(const std::string& name) const {
    const Parameter* p = findParam(name);
    if (!p) throw ExprError("no parameter called '" + name + "'");
    return evalWith(*this, p->expr, 0);
}

double CadDoc::eval(const std::string& expr) const { return evalWith(*this, expr, 0); }

const Mesh* CadDoc::featureMesh(const std::string& name) const {
    auto it = results.find(name);
    return it == results.end() || !it->second.mesh ? nullptr : it->second.mesh.get();
}

const FeatureExtra* CadDoc::featureExtra(const std::string& name) const {
    auto it = results.find(name);
    return it == results.end() || !it->second.extra ? nullptr : it->second.extra.get();
}

std::string CadDoc::featureStatus(const std::string& name) const {
    const Feature* f = findFeature(name);
    if (!f) return "missing";
    if (f->suppressed) return "suppressed";
    if (!featureMesh(name)) {
        const FeatureExtra* x = featureExtra(name);
        if (x && x->sketch) return "sketch, " + x->sketch->status();
        if (x && x->plane) return "plane";
        if (x && !x->axes.empty()) return "axis";
        return "empty";
    }
    auto c = consumedBy.find(name);
    return c == consumedBy.end() ? "body" : "used by " + c->second;
}

// ---- regeneration ---------------------------------------------------------------

namespace detail {

void Ctx::fail(const std::string& why) const { throw std::runtime_error("feature " + f.name + ": " + why); }

bool Ctx::has(const std::string& key) const {
    auto it = f.values.find(key);
    return it != f.values.end() && !it->second.empty();
}
std::string Ctx::text(const std::string& key) const {
    auto it = f.values.find(key);
    if (it != f.values.end() && !it->second.empty()) return it->second;
    const FieldDef* d = t.field(key);
    return d ? d->fallback : std::string();
}
double Ctx::number(const std::string& key) const {
    std::string s = text(key);
    if (s.empty()) fail(key + " is required");
    try {
        return doc.eval(s);
    } catch (const ExprError& e) {
        fail(key + ": " + e.what());
    }
}
int Ctx::whole(const std::string& key, int lo, int hi) const {
    double v = number(key);
    long long n = std::llround(v);
    if (n < lo || n > hi) fail(key + " must be between " + std::to_string(lo) + " and " + std::to_string(hi));
    return static_cast<int>(n);
}
bool Ctx::flag(const std::string& key) const { return text(key) == "true"; }
std::vector<Vec2> Ctx::pts(const std::string& key, std::size_t minPoints) const {
    std::vector<std::string> items = splitList(text(key));
    if (items.size() % 2 != 0 || items.size() / 2 < minPoints) {
        fail(key + " must be x,y pairs, at least " + std::to_string(minPoints) + " points (got " +
             std::to_string(items.size()) + " numbers)");
    }
    std::vector<Vec2> out;
    for (std::size_t i = 0; i < items.size(); i += 2) {
        try {
            out.push_back({doc.eval(items[i]), doc.eval(items[i + 1])});
        } catch (const ExprError& e) {
            fail(key + ": " + e.what());
        }
    }
    return out;
}
std::vector<Vec3> Ctx::points3(const std::string& key) const {
    std::vector<std::string> items = splitList(text(key));
    if (items.size() % 3 != 0) fail(key + " must be x,y,z triples");
    std::vector<Vec3> out;
    for (std::size_t i = 0; i < items.size(); i += 3) {
        try {
            out.push_back({doc.eval(items[i]), doc.eval(items[i + 1]), doc.eval(items[i + 2])});
        } catch (const ExprError& e) {
            fail(key + ": " + e.what());
        }
    }
    return out;
}
double Ctx::size(const std::string& diameter, const std::string& radius, bool required) const {
    if (has(radius)) return number(radius);
    if (has(diameter)) return number(diameter) / 2;
    if (required) fail("give " + diameter + " or " + radius);
    return 0;
}
Mat4 Ctx::place() const {
    return Mat4::translate({number("x"), number("y"), number("z")}) * Mat4::rotate({number("rx"), number("ry"), number("rz")});
}
Mat4 Ctx::sketchPlane() const {
    Mat4 m;
    const std::string plane = text("plane");
    if (plane == "xz") {  // x->X, y->Z, extrusion->Y
        m.m[0] = 1; m.m[1] = 0; m.m[2] = 0;
        m.m[4] = 0; m.m[5] = 0; m.m[6] = 1;
        m.m[8] = 0; m.m[9] = 1; m.m[10] = 0;
    } else if (plane == "yz") {  // x->Y, y->Z, extrusion->X
        m.m[0] = 0; m.m[1] = 0; m.m[2] = 1;
        m.m[4] = 1; m.m[5] = 0; m.m[6] = 0;
        m.m[8] = 0; m.m[9] = 1; m.m[10] = 0;
    }
    return m;
}

}  // namespace detail

namespace {

using detail::Built;
using detail::Ctx;
using detail::MeshPtr;
using detail::placed;
using detail::share;
using detail::csg;
using detail::input;

// CSG can leave zero-volume "fins": triangles on the same three vertices with
// opposite winding. They enclose nothing, so dropping each opposite pair keeps
// the solid and makes it watertight again.
bool dropFins(Mesh& m) {
    std::map<std::array<std::uint32_t, 3>, std::vector<std::size_t>> byCorners;
    for (std::size_t t = 0; t < m.triangleCount(); ++t) {
        std::array<std::uint32_t, 3> k{m.indices[t * 3], m.indices[t * 3 + 1], m.indices[t * 3 + 2]};
        std::sort(k.begin(), k.end());
        byCorners[k].push_back(t);
    }
    std::vector<bool> drop(m.triangleCount(), false);
    bool any = false;
    for (const auto& [k, tris] : byCorners) {
        if (tris.size() < 2) continue;
        // split by winding: same cyclic order as the first = same side
        std::vector<std::size_t> same, flipped;
        const auto* a0 = &m.indices[tris[0] * 3];
        for (std::size_t t : tris) {
            const auto* b0 = &m.indices[t * 3];
            bool s = false;
            for (int r = 0; r < 3; ++r) s = s || (a0[0] == b0[r] && a0[1] == b0[(r + 1) % 3] && a0[2] == b0[(r + 2) % 3]);
            (s ? same : flipped).push_back(t);
        }
        for (std::size_t i = 0; i < std::min(same.size(), flipped.size()); ++i) {
            drop[same[i]] = drop[flipped[i]] = true;
            any = true;
        }
    }
    if (!any) return false;
    std::vector<std::uint32_t> kept;
    for (std::size_t t = 0; t < m.triangleCount(); ++t) {
        if (!drop[t]) kept.insert(kept.end(), {m.indices[t * 3], m.indices[t * 3 + 1], m.indices[t * 3 + 2]});
    }
    m.indices = std::move(kept);
    mesh::cleanup(m);
    return true;
}

Mesh csgOnce(const Mesh& a, const Mesh& b, mesh::BooleanOp op) {
    Mesh m = mesh::booleanOp(a, b, op);
    dropFins(m);
    return m;
}


// Booleans of closed solids should be closed. The BSP engine can still leave a
// sliver overlap when split lines nearly coincide, so if a result comes out
// open, retry with the second solid nudged by far less than any printable or
// machinable size (1e-7 of the model) and keep the first closed result.
// Closed, and no triangle folded back onto its neighbour (overlapping slivers
// still pass the edge test but break the next boolean).
bool clean(const Mesh& m) {
    if (!mesh::isWatertight(m)) return false;
    std::map<std::pair<std::uint32_t, std::uint32_t>, std::size_t> owner;
    for (std::size_t t = 0; t < m.triangleCount(); ++t) {
        for (int k = 0; k < 3; ++k) owner[{m.indices[t * 3 + k], m.indices[t * 3 + (k + 1) % 3]}] = t;
    }
    for (const auto& [edge, t] : owner) {
        auto it = owner.find({edge.second, edge.first});
        if (it == owner.end()) continue;
        const Vec3 n1 = m.faceNormal(t), n2 = m.faceNormal(it->second);
        if (dot(n1, n2) < -0.999 * length(n1) * length(n2)) return false;
    }
    return true;
}

// Flat faces come back from the BSP cut into slivers, which make the next
// boolean fragile: merge them.
Mesh tidy(Mesh m) {
    if (auto merged = mergeCoplanar(m)) return std::move(*merged);
    return m;
}

// Long sliver triangles (flat faces read from STEP or other programs) can
// leave crumbs a hundred-thousandth of a millimetre across where a cut crosses
// them; merging points that close and dropping the fins that leaves closes it.
bool mend(Mesh& m, double d) {
    mesh::weld(m, d * 1e-6);
    dropFins(m);
    m = tidy(std::move(m));
    return !m.empty() && clean(m);
}

Mesh csgImpl(const Mesh& a, const Mesh& b, mesh::BooleanOp op) {
    Mesh m = tidy(csgOnce(a, b, op));
    if (m.empty() || clean(m) || !mesh::isWatertight(a) || !mesh::isWatertight(b)) return m;
    const mesh::Bounds bb = mesh::bounds(b);
    const double d = std::max(mesh::bounds(a).diagonal(), bb.diagonal());
    if (Mesh r = m; mend(r, d)) return r;
    const Vec3 c = bb.center(), skew = normalize(Vec3{1, 0.618, 0.382});
    const Mat4 nudges[] = {Mat4::translate(c) * Mat4::rotateAxis(skew, 1e-5) * Mat4::translate(-c),
                           Mat4::translate(skew * (d * 1e-7)), Mat4::translate(Vec3{-0.382, 1, -0.618} * (d * 3e-7)),
                           Mat4::translate(c) * Mat4::rotateAxis(Vec3{-0.5, 0.2, 1}, -3e-5) * Mat4::translate(-c)};
    for (const auto& n : nudges) {
        Mesh moved = b;
        mesh::transform(moved, n);
        Mesh r = tidy(csgOnce(a, moved, op));
        if (!r.empty() && clean(r)) return r;
    }
    return m;
}

bool boxesTouch(const mesh::Bounds& a, const mesh::Bounds& b) {
    const double eps = 1e-9 * (1 + a.diagonal() + b.diagonal());
    return a.min.x <= b.max.x + eps && b.min.x <= a.max.x + eps && a.min.y <= b.max.y + eps && b.min.y <= a.max.y + eps &&
           a.min.z <= b.max.z + eps && b.min.z <= a.max.z + eps;
}

// Joins solids: plain append when their boxes are apart, CSG union when they might touch.
Mesh joinAll(const std::vector<Mesh>& parts) {
    std::vector<mesh::Bounds> boxes;
    for (const auto& p : parts) boxes.push_back(mesh::bounds(p));
    bool apart = true;
    for (std::size_t i = 0; i < parts.size() && apart; ++i) {
        for (std::size_t j = i + 1; j < parts.size() && apart; ++j) apart = !boxesTouch(boxes[i], boxes[j]);
    }
    Mesh out;
    if (apart) {
        for (const auto& p : parts) mesh::append(out, p);
        return out;
    }
    for (std::size_t i = 0; i < parts.size(); ++i) out = i == 0 ? parts[0] : csg(out, parts[i], mesh::BooleanOp::Union);
    return out;
}

std::vector<Vec2> roundedRect(double w, double h, double r, int seg, bool center) {
    const double x0 = center ? -w / 2 : 0, y0 = center ? -h / 2 : 0;
    if (r <= 0) return mesh::rectangle2(w, h, center);
    std::vector<Vec2> out;
    const Vec2 corners[4] = {{x0 + w - r, y0 + r}, {x0 + w - r, y0 + h - r}, {x0 + r, y0 + h - r}, {x0 + r, y0 + r}};
    for (int c = 0; c < 4; ++c) {
        const double start = -90.0 + 90.0 * c;
        for (int i = 0; i <= seg; ++i) {
            const double a = (start + 90.0 * i / seg) * mesh::kPi / 180.0;
            Vec2 p{corners[c].x + r * std::cos(a), corners[c].y + r * std::sin(a)};
            if (out.empty() || length(out.back() - p) > 1e-12) out.push_back(p);
        }
    }
    if (length(out.front() - out.back()) < 1e-12) out.pop_back();
    return out;
}

std::vector<Vec2> slot2(double len, double width, int seg) {
    const double r = width / 2, c = len / 2 - r;
    std::vector<Vec2> out;
    if (c <= 1e-12) return mesh::circle2(r, std::max(3, seg * 2));
    for (int end = 0; end < 2; ++end) {
        const double cx = end == 0 ? c : -c, start = end == 0 ? -90.0 : 90.0;
        for (int i = 0; i <= seg; ++i) {
            const double a = (start + 180.0 * i / seg) * mesh::kPi / 180.0;
            out.push_back({cx + r * std::cos(a), r * std::sin(a)});
        }
    }
    return out;
}

Vec3 axisVector(const std::string& axis) {
    const double s = axis[0] == '-' ? -1.0 : 1.0;
    const char a = axis.back();
    return a == 'x' ? Vec3{s, 0, 0} : a == 'y' ? Vec3{0, s, 0} : Vec3{0, 0, s};
}

// Rotation taking local -Z (the drilling direction) onto dir.
Mat4 alignDown(const Vec3& dir) {
    const Vec3 down{0, 0, -1};
    if (length(dir - down) < 1e-12) return {};
    if (length(dir + down) < 1e-12) return Mat4::rotateX(180);
    const Vec3 axis = cross(down, dir);
    Mat4 r = Mat4::rotateAxis(axis, 90);
    if (length(r.direction(down) - dir) > 1e-9) r = Mat4::rotateAxis(axis, -90);
    return r;
}

}  // namespace

namespace detail {

MeshPtr share(Mesh m) {
    if (m.empty()) return nullptr;
    return std::make_shared<const Mesh>(std::move(m));
}

Mesh placed(Mesh m, const Mat4& t) {
    mesh::transform(m, t);
    return m;
}

Mesh csg(const Mesh& a, const Mesh& b, mesh::BooleanOp op) { return csgImpl(a, b, op); }

const CadDoc::Cached* input(const Done& done, const Ctx& c, const std::string& key) {
    const std::string name = c.text(key);
    auto it = done.find(name);
    if (it == done.end()) {
        if (c.doc.featureIndex(name) < 0) c.fail(key + " refers to '" + name + "', which doesn't exist");
        c.fail(key + " refers to '" + name + "', which comes later in the tree");
    }
    return &it->second;
}

}  // namespace detail

namespace {

Built buildPrimitive(const Ctx& c) {
    const std::string& type = c.f.type;
    Mesh m;
    try {
        if (type == "box") {
            m = mesh::box({c.number("width"), c.number("depth"), c.number("height")}, c.flag("center"));
        } else if (type == "cylinder") {
            m = mesh::cylinder(c.size("diameter", "radius"), c.number("height"), c.whole("segments", 3, 1024), c.flag("center"));
        } else if (type == "cone") {
            double r1 = c.has("r1") ? c.number("r1") : c.has("d1") ? c.number("d1") / 2 : -1;
            double r2 = c.has("r2") ? c.number("r2") : c.has("d2") ? c.number("d2") / 2 : -1;
            if (r1 < 0 || r2 < 0) c.fail("give d1 and d2 (or r1 and r2)");
            m = mesh::cone(r1, r2, c.number("height"), c.whole("segments", 3, 1024), c.flag("center"));
        } else if (type == "sphere") {
            m = mesh::sphere(c.size("diameter", "radius"), c.whole("segments", 3, 1024), c.whole("rings", 2, 512));
        } else if (type == "torus") {
            m = mesh::torus(c.number("major"), c.number("minor"), c.whole("segments", 3, 1024), c.whole("tube_segments", 3, 512));
        } else if (type == "revolve") {
            m = mesh::revolve(c.pts("points", 3), c.number("angle"), c.whole("segments", 3, 1024));
        } else if (type == "imported") {
            if (!c.f.imported || c.f.imported->empty()) c.fail("has no mesh");
            m = placed(*c.f.imported, Mat4::scale(c.number("scale")));
        } else {
            mesh::Polygon2 shape;
            if (type == "extrude") {
                shape.outer = c.pts("points", 3);
                for (const auto& [key, value] : c.f.values) {
                    if ((key == "hole_points" || isHolePointsKey(key)) && !value.empty()) shape.holes.push_back(c.pts(key, 3));
                }
            } else if (type == "extrude_rect") {
                const double w = c.number("width"), d = c.number("depth"), r = c.number("radius");
                if (r < 0 || r * 2 > std::min(w, d) + 1e-12) c.fail("radius must be between 0 and half the shorter side");
                shape.outer = roundedRect(w, d, std::min(r, std::min(w, d) / 2), c.whole("segments", 1, 256), c.flag("center"));
            } else if (type == "extrude_circle") {
                const double r = c.size("diameter", "radius");
                const int seg = c.whole("segments", 3, 1024);
                shape.outer = mesh::circle2(r, seg);
                const double inner = c.number("inner_diameter") / 2;
                if (inner < 0 || inner >= r) {
                    if (inner != 0) c.fail("inner_diameter must be smaller than the diameter");
                }
                if (inner > 0) shape.holes.push_back(mesh::circle2(inner, seg));
            } else if (type == "extrude_slot") {
                const double len = c.number("length"), w = c.number("width");
                if (!(w > 0) || len < w) c.fail("slot needs width > 0 and length >= width");
                shape.outer = slot2(len, w, c.whole("segments", 1, 512));
            } else {
                c.fail("unknown feature type " + type);
            }
            m = placed(mesh::extrude(shape, c.number("height")), c.sketchPlane());
        }
    } catch (const std::invalid_argument& e) {
        c.fail(e.what());
    }
    return {share(placed(std::move(m), c.place())), {}};
}


Built buildHole(const Ctx& c, const CadDoc::Cached& target) {
    if (!target.mesh) return {};
    const Mesh& body = *target.mesh;
    const mesh::Bounds b = mesh::bounds(body);
    const Vec3 dir = axisVector(c.text("axis"));
    const int along = dir.x != 0 ? 0 : dir.y != 0 ? 1 : 2;
    const char* names[3] = {"x", "y", "z"};
    Vec3 pos;
    for (int i = 0; i < 3; ++i) {
        if (c.has(names[i])) pos[i] = c.number(names[i]);
        else if (i == along) pos[i] = dir[i] < 0 ? b.max[i] : b.min[i];  // entry face
    }
    const double r = c.number("diameter") / 2;
    if (!(r > 0)) c.fail("diameter must be positive");
    const double diag = b.diagonal();
    const double ext = std::max(diag * 0.05, 1e-3);
    double depth = c.number("depth");
    if (depth < 0) c.fail("depth can't be negative");
    if (depth == 0) depth = diag + length(pos - b.center()) + ext;
    const double csd = c.number("countersink_diameter") / 2, cbd = c.number("counterbore_diameter") / 2;
    std::vector<Vec2> prof{{0, -depth}, {r, -depth}};
    if (csd > 0 && cbd > 0) c.fail("choose a countersink or a counterbore, not both");
    if (cbd > 0) {
        const double cbDepth = c.number("counterbore_depth");
        if (cbd <= r) c.fail("counterbore_diameter must be larger than the diameter");
        if (!(cbDepth > 0) || cbDepth >= depth) c.fail("counterbore_depth must be between 0 and the hole depth");
        prof.push_back({r, -cbDepth});
        prof.push_back({cbd, -cbDepth});
        prof.push_back({cbd, ext});
    } else if (csd > 0) {
        const double angle = c.number("countersink_angle");
        if (csd <= r) c.fail("countersink_diameter must be larger than the diameter");
        if (!(angle > 0 && angle < 180)) c.fail("countersink_angle must be between 0 and 180 degrees");
        const double h = (csd - r) / std::tan(angle / 2 * mesh::kPi / 180.0);
        if (h >= depth) c.fail("the countersink is deeper than the hole");
        prof.push_back({r, -h});
        prof.push_back({csd, 0});
        prof.push_back({csd, ext});
    } else {
        prof.push_back({r, ext});
    }
    prof.push_back({0, ext});
    Mesh tool;
    try {
        tool = mesh::revolve(prof, 360, c.whole("segments", 3, 1024));
    } catch (const std::invalid_argument& e) {
        c.fail(e.what());
    }
    mesh::transform(tool, Mat4::translate(pos) * alignDown(dir));
    Built out;
    out.mesh = share(csg(body, tool, mesh::BooleanOp::Difference));
    out.tools.push_back(std::make_shared<const Mesh>(std::move(tool)));
    auto extra = std::make_shared<FeatureExtra>();
    extra->axes.push_back({pos, dir * -1.0, r});
    out.extra = extra;
    return out;
}

// Copies of the target under each transform (the first should be the identity).
Built repeat(const CadDoc::Cached& target, const std::vector<Mat4>& copies, bool includeFirst) {
    if (!target.mesh) return {};
    Built out;
    if (target.extra && !target.extra->axes.empty()) {
        auto extra = std::make_shared<FeatureExtra>();
        for (std::size_t k = 0; k < copies.size(); ++k) {
            if (k == 0 && !includeFirst) continue;
            for (const auto& a : target.extra->axes) {
                extra->axes.push_back({copies[k].point(a.point), mesh::normalize(copies[k].direction(a.dir)), a.radius});
            }
        }
        out.extra = extra;
    }
    if (!target.tools.empty()) {
        // a hole (or a pattern of holes): repeat the cut on the same body
        Mesh body = *target.mesh;
        out.tools = includeFirst ? target.tools : std::vector<MeshPtr>{};
        for (std::size_t k = 0; k < copies.size(); ++k) {
            if (k == 0 && includeFirst) continue;
            for (const auto& tool : target.tools) {
                Mesh t = placed(*tool, copies[k]);
                body = csg(body, t, mesh::BooleanOp::Difference);
                out.tools.push_back(std::make_shared<const Mesh>(std::move(t)));
            }
        }
        out.mesh = share(std::move(body));
        return out;
    }
    std::vector<Mesh> parts;
    for (const auto& m : copies) parts.push_back(placed(*target.mesh, m));
    out.mesh = share(joinAll(parts));
    return out;
}

Built buildOperation(const Ctx& c, const std::map<std::string, CadDoc::Cached>& done) {
    const std::string& type = c.f.type;
    if (type == "boolean") {
        const auto& a = *input(done, c, "a");
        const auto& b = *input(done, c, "b");
        const std::string op = c.text("op");
        if (c.text("a") == c.text("b")) c.fail("a and b must be different features");
        if (!b.mesh) return {a.mesh, {}};
        if (!a.mesh) return {op == "union" ? b.mesh : nullptr, {}};
        const auto kind = op == "difference" ? mesh::BooleanOp::Difference
                          : op == "intersection" ? mesh::BooleanOp::Intersection
                                                 : mesh::BooleanOp::Union;
        return {share(csg(*a.mesh, *b.mesh, kind)), {}};
    }
    const auto& target = *input(done, c, "target");
    if (type == "hole") return buildHole(c, target);
    if (type == "pattern_linear") {
        const int n = c.whole("count", 1, 1000), n2 = c.whole("count2", 1, 1000);
        if (n * n2 > 2000) c.fail("count x count2 is limited to 2000 copies");
        const Vec3 d{c.number("dx"), c.number("dy"), c.number("dz")}, d2{c.number("dx2"), c.number("dy2"), c.number("dz2")};
        std::vector<Mat4> copies;
        for (int j = 0; j < n2; ++j) {
            for (int i = 0; i < n; ++i) copies.push_back(Mat4::translate(d * i + d2 * j));
        }
        return repeat(target, copies, true);
    }
    if (type == "pattern_circular") {
        const int n = c.whole("count", 1, 1000);
        const double angle = c.number("angle");
        if (!(angle > 0 && angle <= 360)) c.fail("angle must be in (0, 360]");
        const double step = std::fabs(angle - 360) < 1e-9 ? 360.0 / n : n > 1 ? angle / (n - 1) : 0;
        const Vec3 center{c.number("cx"), c.number("cy"), c.number("cz")};
        const Vec3 axis = axisVector(c.text("axis"));
        std::vector<Mat4> copies;
        for (int i = 0; i < n; ++i) {
            copies.push_back(Mat4::translate(center) * Mat4::rotateAxis(axis, step * i) * Mat4::translate(-center));
        }
        return repeat(target, copies, true);
    }
    if (type == "mirror") {
        const std::string plane = c.text("plane");
        const int i = plane == "yz" ? 0 : plane == "xz" ? 1 : 2;
        Vec3 s{1, 1, 1}, t{};
        s[i] = -1;
        t[i] = 2 * c.number("offset");
        const Mat4 m = Mat4::translate(t) * Mat4::scale(s);
        if (!c.flag("keep")) {
            if (!target.mesh) return {};
            Built out{share(placed(*target.mesh, m)), {}};
            return out;
        }
        return repeat(target, {Mat4{}, m}, true);
    }
    if (type == "transform") {
        if (!target.mesh) return {};
        const double s = c.number("scale");
        if (!(s > 0)) c.fail("scale must be positive");
        const Vec3 pivot{c.number("cx"), c.number("cy"), c.number("cz")};
        const Mat4 m = Mat4::translate(Vec3{c.number("x"), c.number("y"), c.number("z")} + pivot) *
                       Mat4::rotate({c.number("rx"), c.number("ry"), c.number("rz")}) * Mat4::scale(s) *
                       Mat4::translate(-pivot);
        return {share(placed(*target.mesh, m)), {}};
    }
    if (type == "fillet" || type == "chamfer") {
        if (!target.mesh) return {};
        const double angle = c.number("angle");
        if (!(angle > 0 && angle < 180)) c.fail("angle must be between 0 and 180 degrees");
        std::vector<Vec3> near;
        const auto items = splitList(c.text("near"));
        if (items.size() % 3 != 0) c.fail("near must be x,y,z triples");
        for (std::size_t i = 0; i < items.size(); i += 3) {
            try {
                near.push_back({c.doc.eval(items[i]), c.doc.eval(items[i + 1]), c.doc.eval(items[i + 2])});
            } catch (const ExprError& e) {
                c.fail(std::string("near: ") + e.what());
            }
        }
        try {
            const auto all = detail::sharpEdges(*target.mesh, angle);
            const auto edges = detail::pickEdges(*target.mesh, all, splitList(c.text("edges")), near);
            if (edges.empty()) c.fail("no edges matched (cad.edges lists the sharp edges)");
            const bool chamfer = type == "chamfer";
            return {share(detail::blendEdges(*target.mesh, edges, c.number(chamfer ? "distance" : "radius"), chamfer,
                                             chamfer ? 1 : c.whole("segments", 1, 64), csg)),
                    {}};
        } catch (const std::invalid_argument& e) {
            c.fail(e.what());
        } catch (const std::runtime_error& e) {
            const std::string what = e.what();
            if (what.rfind("feature ", 0) == 0) throw;
            c.fail(what);
        }
    }
    if (type == "offset_face") {
        if (!target.mesh) return {};
        const auto items = splitList(c.text("face"));
        if (items.size() != 3) c.fail("face is a point x,y,z on the face");
        Vec3 at;
        try {
            at = {c.doc.eval(items[0]), c.doc.eval(items[1]), c.doc.eval(items[2])};
        } catch (const ExprError& e) {
            c.fail(std::string("face: ") + e.what());
        }
        try {
            return {share(detail::offsetFace(*target.mesh, at, c.number("distance"), csg)), {}};
        } catch (const std::runtime_error& e) {
            const std::string what = e.what();
            if (what.rfind("feature ", 0) == 0) throw;
            c.fail(what);
        }
    }
    if (type == "shell") {
        if (!target.mesh) return {};
        std::vector<Vec3> open;
        for (const auto& name : splitList(c.text("open"))) {
            Vec3 d;
            if (!detail::directionByName(name, d)) c.fail("open: unknown side '" + name + "' (top, bottom, front, back, left, right, +x, -x, ...)");
            open.push_back(d);
        }
        try {
            return {share(detail::shell(*target.mesh, c.number("thickness"), open, csg)), {}};
        } catch (const std::runtime_error& e) {
            const std::string what = e.what();
            if (what.rfind("feature ", 0) == 0) throw;
            c.fail(what);
        }
    }
    c.fail("unknown feature type " + type);
}

// Everything the result depends on, evaluated, so unchanged features reuse their cache.
std::string signature(const Ctx& c, const std::map<std::string, CadDoc::Cached>& done) {
    std::string sig = c.f.type;
    auto add = [&](const std::string& key) {
        const FieldDef* d = c.t.field(key);
        std::string v = c.text(key);
        sig += "|" + key + "=";
        if (!d || v.empty()) {
            sig += v;
            return;
        }
        try {
            if (d->kind == FieldKind::Number || d->kind == FieldKind::Integer) {
                sig += fmtExact(c.doc.eval(v));
            } else if (d->kind == FieldKind::Points) {
                for (const auto& item : splitList(v)) sig += fmtExact(c.doc.eval(item)) + ",";
            } else if (d->kind == FieldKind::Ref) {
                auto it = done.find(v);
                sig += "{" + (it == done.end() ? std::string("?") : it->second.signature) + "}";
            } else if (d->kind == FieldKind::Refs) {
                for (const auto& item : splitList(v)) {
                    auto it = done.find(item);
                    sig += "{" + (it == done.end() ? std::string("?") : it->second.signature) + "}";
                }
            } else {
                sig += v;
            }
        } catch (const ExprError&) {
            sig += "!" + v;  // the build reports the error
        }
    };
    for (const auto& field : c.t.fields) add(field.name);
    for (const auto& [key, value] : c.f.values) {
        if (isHolePointsKey(key)) add(key);
    }
    if (c.f.imported) {
        char buf[32];
        std::snprintf(buf, sizeof buf, "%p", static_cast<const void*>(c.f.imported.get()));
        sig += "|mesh=" + std::string(buf);
    }
    if (detail::isAdvanced(c.f.type)) sig += detail::advancedSignature(c);
    return sig;
}

}  // namespace

void CadDoc::regenerate() {
    std::map<std::string, Cached> done;
    std::map<std::string, std::string> consumed;
    for (const auto& f : features) {
        const FeatureType* t = featureType(f.type);
        if (!t) throw std::runtime_error("feature " + f.name + ": unknown type " + f.type);
        if (f.suppressed) {
            done[f.name] = Cached{"suppressed", nullptr, {}};
            continue;
        }
        Ctx c{*this, f, *t};
        std::string sig = signature(c, done);
        Cached result;
        auto old = results.find(f.name);
        if (old != results.end() && (old->second.key.empty() ? old->second.signature : old->second.key) == sig) {
            result = old->second;
        } else {
            Built b;
            if (detail::isAdvanced(f.type)) {
                b = detail::buildAdvanced(c, done);
            } else if (detail::isSheetMetal(f.type)) {
                b = detail::buildSheetMetal(c, done);
            } else {
                bool isOp = false;
                for (const auto& field : t->fields) isOp = isOp || field.kind == FieldKind::Ref;
                b = isOp ? buildOperation(c, done) : buildPrimitive(c);
                auto axes = detail::primitiveAxes(c);
                if (!axes.empty() && !b.extra) {
                    auto extra = std::make_shared<FeatureExtra>();
                    extra->axes = std::move(axes);
                    b.extra = extra;
                }
            }
            result = Cached{b.signature.empty() ? sig : b.signature, b.mesh, b.tools, nullptr, b.extra, b.signature.empty() ? std::string() : sig};
        }
        if (result.mesh) {
            for (const auto& field : t->fields) {
                if (field.kind != FieldKind::Ref) continue;
                if (field.name == "target" && c.text("operation") == "new") continue;
                const std::string in = c.text(field.name);
                auto it = done.find(in);
                if (it != done.end() && it->second.mesh) consumed[in] = f.name;
            }
        }
        done[f.name] = std::move(result);
    }
    results = std::move(done);
    consumedBy = std::move(consumed);
    bodies.clear();
    mesh = {};
    for (const auto& f : features) {
        auto it = results.find(f.name);
        if (it == results.end() || !it->second.mesh || consumedBy.count(f.name)) continue;
        // bodies are what gets exported: give their flat faces clean, few triangles
        // (cached; the raw result stays the input of later booleans, which cut it best)
        Cached& r = it->second;
        if (!r.display) {
            auto merged = mergeCoplanar(*r.mesh);
            r.display = merged ? std::make_shared<const Mesh>(std::move(*merged)) : r.mesh;
        }
        bodies.push_back({f.name, *r.display});
        mesh::append(mesh, *r.display);
    }
}

// ---- undo ---------------------------------------------------------------------

Result CadEdit::apply() {
    before_ = {doc_->units, doc_->params, doc_->features, doc_->counters, doc_->configs, doc_->activeConfig};
    Result r;
    try {
        r = fn_(*doc_);
        if (r.ok) doc_->regenerate();
    } catch (const std::exception& e) {
        r = Result::failure(e.what());
    }
    if (!r.ok) revert();
    return r;
}

void CadEdit::revert() {
    doc_->units = before_.units;
    doc_->params = before_.params;
    doc_->features = before_.features;
    doc_->counters = before_.counters;
    doc_->configs = before_.configs;
    doc_->activeConfig = before_.activeConfig;
    try {
        doc_->regenerate();
    } catch (const std::exception&) {
        // the snapshot built before, so this only happens if it never did
    }
}

// ---- files ----------------------------------------------------------------------

std::shared_ptr<CadDoc> newCad(const std::string& units) {
    auto doc = std::make_shared<CadDoc>();
    doc->units = units;
    return doc;
}

namespace {

json sketchJson(const Sketch& sk) {
    json pts = json::array(), ents = json::array(), cons = json::array();
    for (const auto& p : sk.points) pts.push_back({{"id", p.id}, {"x", p.x}, {"y", p.y}});
    for (const auto& e : sk.entities) {
        json je = {{"id", e.id}, {"kind", e.kind}, {"points", e.points}};
        if (e.kind == "circle") je["radius"] = e.radius;
        if (e.construction) je["construction"] = true;
        if (e.closed) je["closed"] = true;
        ents.push_back(je);
    }
    for (const auto& c : sk.constraints) {
        json jc = {{"id", c.id}, {"type", c.type}, {"refs", c.refs}};
        if (!c.value.empty()) jc["value"] = c.value;
        if (c.aux != 0) jc["aux"] = c.aux;
        cons.push_back(jc);
    }
    return {{"points", pts}, {"entities", ents}, {"constraints", cons}, {"counters", sk.counters}};
}

Sketch sketchFromJson(const json& j) {
    Sketch sk;
    for (const auto& p : j.value("points", json::array())) sk.points.push_back({p.at("id").get<std::string>(), p.at("x").get<double>(), p.at("y").get<double>()});
    for (const auto& e : j.value("entities", json::array())) {
        SketchEntity se;
        se.id = e.at("id").get<std::string>();
        se.kind = e.at("kind").get<std::string>();
        se.points = e.at("points").get<std::vector<std::string>>();
        se.radius = e.value("radius", 0.0);
        se.construction = e.value("construction", false);
        se.closed = e.value("closed", false);
        const std::size_t need = se.kind == "line" ? 2 : se.kind == "circle" ? 1 : se.kind == "arc" ? 3 : 2;
        if (se.kind != "line" && se.kind != "circle" && se.kind != "arc" && se.kind != "spline") throw std::runtime_error("unknown sketch entity kind '" + se.kind + "'");
        if (se.kind == "spline" ? se.points.size() < need : se.points.size() != need) throw std::runtime_error("sketch entity " + se.id + " has the wrong number of points");
        for (const auto& pid : se.points) {
            if (!sk.point(pid)) throw std::runtime_error("sketch entity " + se.id + " uses a missing point " + pid);
        }
        sk.entities.push_back(std::move(se));
    }
    for (const auto& c : j.value("constraints", json::array())) {
        SketchConstraint sc;
        sc.id = c.at("id").get<std::string>();
        sc.type = c.at("type").get<std::string>();
        sc.refs = c.at("refs").get<std::vector<std::string>>();
        sc.value = c.value("value", "");
        sc.aux = c.value("aux", 0.0);
        sk.constraints.push_back(std::move(sc));
    }
    sk.counters = j.value("counters", std::map<std::string, int>{});
    return sk;
}

}  // namespace

std::string toJson(const CadDoc& doc) {
    json j;
    j["format"] = "wwcad";
    j["version"] = 1;
    j["units"] = doc.units;
    json params = json::array();
    for (const auto& p : doc.params) params.push_back({{"name", p.name}, {"value", p.expr}});
    j["params"] = params;
    if (!doc.configs.empty()) {
        json configs = json::array();
        for (const auto& c : doc.configs) {
            json values = json::array();
            for (const auto& v : c.values) values.push_back({{"name", v.name}, {"value", v.expr}});
            configs.push_back({{"name", c.name}, {"values", values}});
        }
        j["configurations"] = configs;
        if (!doc.activeConfig.empty()) j["active_configuration"] = doc.activeConfig;
    }
    json features = json::array();
    for (const auto& f : doc.features) {
        json jf = {{"name", f.name}, {"type", f.type}, {"values", f.values}};
        if (f.suppressed) jf["suppressed"] = true;
        if (f.type == "sketch") jf["sketch"] = sketchJson(f.sketch);
        if (f.imported) {
            std::vector<double> pos;
            for (const auto& v : f.imported->positions) pos.insert(pos.end(), {v.x, v.y, v.z});
            jf["mesh"] = {{"positions", pos}, {"indices", f.imported->indices}};
            if (!f.source.empty()) jf["source"] = f.source;
        }
        features.push_back(jf);
    }
    j["features"] = features;
    return j.dump(2) + "\n";
}

std::shared_ptr<CadDoc> fromJson(const std::string& text) {
    json j = json::parse(text, nullptr, false);
    if (j.is_discarded() || !j.is_object()) throw std::runtime_error("not a .wwcad file (invalid JSON)");
    const std::string format = j.value("format", "");
    // "achcad": parts saved before the rename still open
    if (format != "wwcad" && format != "achcad") throw std::runtime_error("not a .wwcad file (format is not \"wwcad\")");
    try {
        auto doc = newCad(j.value("units", "mm"));
        if (std::find(unitNames().begin(), unitNames().end(), doc->units) == unitNames().end()) doc->units = "mm";
        for (const auto& p : j.value("params", json::array())) {
            const std::string name = p.at("name").get<std::string>();
            if (!validParamName(name)) throw std::runtime_error("bad parameter name '" + name + "'");
            doc->params.push_back({name, p.at("value").get<std::string>()});
        }
        for (const auto& c : j.value("configurations", json::array())) {
            Configuration conf{c.at("name").get<std::string>(), {}};
            for (const auto& v : c.value("values", json::array())) conf.values.push_back({v.at("name").get<std::string>(), v.at("value").get<std::string>()});
            doc->configs.push_back(std::move(conf));
        }
        doc->activeConfig = j.value("active_configuration", "");
        for (const auto& jf : j.value("features", json::array())) {
            Feature f;
            f.name = jf.at("name").get<std::string>();
            f.type = jf.at("type").get<std::string>();
            if (!featureType(f.type)) throw std::runtime_error("unknown feature type '" + f.type + "'");
            if (doc->featureIndex(f.name) >= 0) throw std::runtime_error("two features are called '" + f.name + "'");
            f.values = jf.value("values", std::map<std::string, std::string>{});
            f.suppressed = jf.value("suppressed", false);
            f.source = jf.value("source", "");
            if (jf.contains("sketch")) f.sketch = sketchFromJson(jf["sketch"]);
            if (jf.contains("mesh")) {
                Mesh m;
                auto pos = jf["mesh"].at("positions").get<std::vector<double>>();
                for (std::size_t i = 0; i + 2 < pos.size(); i += 3) m.positions.push_back({pos[i], pos[i + 1], pos[i + 2]});
                m.indices = jf["mesh"].at("indices").get<std::vector<std::uint32_t>>();
                if (m.indices.size() % 3) throw std::runtime_error("feature " + f.name + ": bad mesh");
                for (auto idx : m.indices) {
                    if (idx >= m.positions.size()) throw std::runtime_error("feature " + f.name + ": bad mesh index");
                }
                f.imported = std::make_shared<const Mesh>(std::move(m));
            }
            // keep automatic names from colliding with loaded ones
            std::string prefix = f.type;
            if (f.name.rfind(prefix, 0) == 0) {
                const std::string rest = f.name.substr(prefix.size());
                if (!rest.empty() && rest.size() < 9 && std::all_of(rest.begin(), rest.end(), [](char ch) { return ch >= '0' && ch <= '9'; })) {
                    doc->counters[prefix] = std::max(doc->counters[prefix], std::stoi(rest));
                }
            }
            doc->features.push_back(std::move(f));
        }
        doc->regenerate();
        return doc;
    } catch (const json::exception& e) {
        throw std::runtime_error(std::string("bad .wwcad file: ") + e.what());
    }
}

std::shared_ptr<CadDoc> openCad(const std::filesystem::path& path) {
    std::string ext = path.extension().string();
    for (auto& ch : ext) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    std::shared_ptr<CadDoc> doc;
    if (ext == ".wwcad" || ext == ".achcad") {  // .achcad: saved before the rename
        std::ifstream in(path, std::ios::binary);
        if (!in) throw std::runtime_error("can't open " + path.string());
        std::stringstream ss;
        ss << in.rdbuf();
        doc = fromJson(ss.str());
    } else if (mesh::canRead(path)) {
        doc = newCad();
        Feature f;
        f.type = "imported";
        f.name = doc->newFeatureName("imported");
        f.imported = std::make_shared<const Mesh>(mesh::load(path));
        f.source = path.string();
        doc->features.push_back(std::move(f));
        doc->regenerate();
    } else {
        throw std::runtime_error("cad opens .wwcad files, STEP and meshes (step stp stl obj ply off glb gltf 3mf fbx), not " + path.string());
    }
    doc->path = path.string();
    return doc;
}

}  // namespace wetwaresuite::cad
