// Software preview renderer: orthographic, z-buffered, lambert-shaded, with
// optional crease/silhouette outlines and 2x supersampling. The camera
// auto-fits the mesh. Used for MCP previews and thumbnails, so it favours
// speed and a clean CAD look over realism.
#pragma once

#include <optional>
#include <string>
#include <vector>

#include "wetwaresuite/mesh/mesh.hpp"
#include "wetwaresuite/raster/image.hpp"

namespace wetwaresuite::mesh {

// Iso looks from the front-right-top; front looks along +Y, right along -X, top down -Z.
enum class View { Iso, Front, Back, Left, Right, Top, Bottom };

const std::vector<std::string>& viewNames();  // iso front back left right top bottom ("side" parses as right)
std::optional<View> parseView(const std::string& name);

// A free camera for orbiting: yaw turns around the vertical (Z) axis, pitch tilts
// up and down (degrees). yaw 0 pitch 0 is the front view; yaw 45 pitch 35.26 is iso.
// The model's bounding sphere is fitted, so its size stays put while orbiting.
struct Orbit {
    double yaw = 45;
    double pitch = 35.26;
    double zoom = 1;  // 2 = twice as big
    double panX = 0;  // shift in model radii, right and up on screen
    double panY = 0;
};

// A line drawn over the shaded model (sketches, axes); hidden where the model is in front of it.
struct Line3 {
    std::vector<Vec3> points;
    raster::Color color{230, 110, 30, 255};
    bool closed = false;
};

// The orthographic projection a picture is drawn with (in output pixels): p lands on
// x = (dot(p, right) - cx) * scale + width / 2, y = height / 2 - (dot(p, up) - cy) * scale.
struct Projection {
    Vec3 right{1, 0, 0}, up{0, 0, 1}, forward{0, 1, 0};
    double cx = 0, cy = 0, scale = 1;
    Vec2 toScreen(const Vec3& p, int width, int height) const {
        return {(dot(p, right) - cx) * scale + width / 2.0, height / 2.0 - (dot(p, up) - cy) * scale};
    }
};

// How surfaces are drawn, like Blender's viewport shading: solid (one colour), material (the
// per-face colours), matcap (a studio clay look from the view, no lights), wireframe (edges only)
// and xray (see-through faces with every edge, so hidden parts show).
enum class Shading { Material, Solid, Matcap, Wireframe, XRay };
const std::vector<std::string>& shadingNames();  // material solid matcap wireframe xray
std::optional<Shading> parseShading(const std::string& name);

struct RenderOptions {
    View view = View::Iso;
    Shading shading = Shading::Material;
    std::optional<Orbit> orbit;  // overrides view
    std::optional<Projection> projection;  // overrides view and orbit: no fitting
    raster::Color background{246, 247, 249, 255};  // alpha 0 gives a transparent background
    raster::Color color{122, 158, 204, 255};       // surface colour
    bool smooth = true;     // smooth normals across shallow edges; false = flat facets
    bool outline = true;    // dark lines on silhouettes and sharp edges
    bool antialias = true;  // render at 2x and downsample
    double margin = 0.06;   // empty border, as a fraction of the image size
    // one colour per triangle (overrides `color`), for highlighting parts or
    // gap maps; ignored unless it has exactly triangleCount() entries
    std::vector<raster::Color> faceColors;
    // lines over the model; they count when fitting the camera, so lines alone still show
    std::vector<Line3> lines;
};

raster::Image render(const Mesh& mesh, int width, int height, const RenderOptions& options = {});
// The projection render() would use for this mesh, size and options (it fits the camera to the mesh and lines).
Projection fitProjection(const Mesh& mesh, int width, int height, const RenderOptions& options = {});

}  // namespace wetwaresuite::mesh
