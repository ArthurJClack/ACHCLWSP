// render: 3D scenes and renders, headless. A Scene is a list of objects
// (primitives or meshes from libs/mesh files and open mesh documents) with
// transforms and materials, lights, a sky and a camera. It renders two ways:
// a fast z-buffer rasterizer for previews (optionally with ray-traced
// shadows) and a multithreaded CPU path tracer (BVH, next-event estimation,
// GGX metals and plastics, glass, emission, image textures, a simple
// edge-aware denoiser). Z is up, like the rest of the suite.
//
// Every action is a render.* command; edits snapshot the scene for undo.
// Scenes save as JSON (.scene.json); meshes that came from open documents are
// written next to it as .glb files.
#pragma once

#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "wetwaresuite/core/command.hpp"
#include "wetwaresuite/mesh/math.hpp"
#include "wetwaresuite/mesh/mesh.hpp"
#include "wetwaresuite/raster/image.hpp"

namespace wetwaresuite::render {

using mesh::Vec3;

// Colours are linear RGB; files and params use sRGB hex ("#cc3322").
struct Material {
    Vec3 color{0.8, 0.8, 0.8};
    double roughness = 0.5;      // 0 = mirror-smooth, 1 = fully rough
    double metallic = 0.0;       // 0 = plastic/paint, 1 = metal (tinted reflections)
    Vec3 emission{0, 0, 0};      // glow colour
    double emissionStrength = 0; // 0 = no glow
    double transmission = 0.0;   // 1 = clear glass (tinted by color)
    double ior = 1.5;            // index of refraction for glass
    double opacity = 1.0;        // < 1 lets light pass straight through (cut-outs, ghosts)
    std::string texture;         // image file multiplied into color; empty = none
    double textureScale = 1.0;   // UV repeat count
    std::string normalMap;       // tangent-space normal map (OpenGL style, green up); empty = none
    double normalStrength = 1.0; // 0 = flat .. 1 = as painted .. 2 = exaggerated
    std::string roughnessMap;    // grey (or glTF green channel) image multiplied into roughness
    std::string name;            // parts only: the material's name in the file
};

const std::vector<std::string>& objectTypes();     // box sphere cylinder cone torus plane mesh
const std::vector<std::string>& materialPresets();  // matte plastic metal gold copper chrome mirror glass rubber emissive ...
// Applies a named preset (keeps the texture). False when the name is unknown.
bool applyPreset(Material& m, const std::string& preset);

struct Object {
    std::string name;
    std::string type = "box";  // see objectTypes()
    // primitive dimensions (model units)
    Vec3 size{1, 1, 1};        // box: x, y, z; plane: x, y
    double radius = 0.5;       // sphere, cylinder, cone (bottom), torus (major)
    double radius2 = 0.0;      // cone top radius; torus tube radius (0 = radius / 4)
    double height = 1.0;       // cylinder, cone
    int segments = 48;         // round primitives
    std::string path;          // type mesh: file it came from (may be empty for document meshes)
    std::shared_ptr<const mesh::Mesh> mesh;  // type mesh: the geometry
    bool centered = false;     // type mesh: the file's geometry was moved so the middle of its base is the origin
    Vec3 position{0, 0, 0};
    Vec3 rotation{0, 0, 0};    // degrees about x, then y, then z
    Vec3 scale{1, 1, 1};
    bool visible = true;
    bool smooth = true;        // smooth normals (false = flat facets)
    Material material;
    // type mesh from a file with several materials (OBJ + .mtl): one material per
    // part, and which part each triangle belongs to. Empty = `material` everywhere.
    std::vector<Material> parts;
    std::shared_ptr<const std::vector<int>> partOf;

    mesh::Mat4 matrix() const;  // translate * rotate * scale
};

const std::vector<std::string>& lightTypes();  // point sun area

struct Light {
    std::string name;
    std::string type = "point";
    Vec3 position{4, -4, 6};
    Vec3 target{0, 0, 0};       // sun and area lights shine from position towards target
    Vec3 color{1, 1, 1};
    double intensity = 100;     // point/area: power (falls off with distance squared); sun: brightness
    double radius = 0.1;        // point: bulb radius (soft shadows); sun: angular radius in degrees
    double width = 1, height = 1;  // area: rectangle size
    bool shadows = true;
};

struct Sky {
    std::string type = "gradient";  // gradient, color, none (black), hdri (an equirectangular image)
    std::string hdri;               // type hdri: panorama image (2:1, any readable format)
    double rotation = 0;            // type hdri: degrees to turn it about z
    Vec3 zenith{0.30, 0.48, 0.80};
    Vec3 horizon{0.80, 0.86, 0.95};
    Vec3 ground{0.22, 0.20, 0.18};
    double strength = 0.5;          // how much light the sky gives (and how bright it looks)
    bool visible = true;            // false: the camera sees `background` instead (still lights the scene)
    Vec3 background{0.95, 0.95, 0.96};
    bool transparent = false;       // PNG with alpha where the sky shows
};

struct Camera {
    Vec3 position{6, -8, 5};
    Vec3 target{0, 0, 0.5};
    Vec3 up{0, 0, 1};
    double fov = 40;           // vertical field of view, degrees
    bool orthographic = false;
    double orthoSize = 5;      // orthographic: visible height in model units
    bool autoFit = true;       // frame every object at render time until a camera is set
};

struct Settings {
    int width = 640, height = 480;
    int samples = 16;   // path tracer samples per pixel
    int bounces = 4;    // path tracer max bounces
    bool denoise = true;
    double exposure = 0;  // stops
    std::string mode = "path";  // default render mode: path or preview
};

struct Scene {
    std::vector<Object> objects;
    std::vector<Light> lights;
    Sky sky;
    Camera camera;
    Settings settings;

    Object* findObject(const std::string& name);
    Light* findLight(const std::string& name);
    const Object* findObject(const std::string& name) const;
    const Light* findLight(const std::string& name) const;
    // A free name like "sphere2": not used by any object or light.
    std::string uniqueName(const std::string& base) const;
};

// The default scene render.new makes: a sun, a fill light and a gradient sky.
Scene defaultScene();
// Adds a ground plane named "ground" under everything (size from the bounds).
void addGround(Scene& scene, double size = 0);

// Moves a mesh so the middle of its bounding box's base is at the origin.
void centerMesh(mesh::Mesh& m);

// Reads a model file. For OBJ files with several `usemtl` groups it also
// fills `parts` (from the .mtl: Kd, Ns/Pr, Pm, Ke, d, Ni, map_Kd, map_Bump/norm,
// map_Pr) and `partOf` (one part index per triangle); otherwise they stay empty.
mesh::Mesh loadMeshFile(const std::filesystem::path& path, std::vector<Material>& parts, std::vector<int>& partOf);

// Geometry of one object in world space, with normals (and UVs when it had them).
mesh::Mesh worldMesh(const Object& object);
// Bounds of all visible objects (ground planes only when nothing else exists).
mesh::Bounds sceneBounds(const Scene& scene);
// Points the camera at the scene from its current direction (or the default one).
void fitCamera(const Scene& scene, Camera& camera);  // uses the scene's width:height

// ---- JSON (.scene.json) ----
// Relative mesh and texture paths resolve against `baseDir`.
std::string toJson(const Scene& scene, const std::filesystem::path& baseDir = {});
Scene fromJson(const std::string& text, const std::filesystem::path& baseDir = {});  // throws std::runtime_error
// Saves the scene; meshes without a file are written as <stem>.<name>.glb beside it.
void saveScene(Scene& scene, const std::filesystem::path& path);
Scene loadScene(const std::filesystem::path& path);

// ---- rendering ----
struct RenderOptions {
    std::string mode = "path";  // "path" (path tracer) or "preview" (rasterizer)
    int width = 640, height = 480;
    int samples = 16;
    int bounces = 4;
    bool denoise = true;
    double exposure = 0;
    bool shadows = true;   // preview mode: ray-traced shadows
    bool antialias = true; // preview mode: 2x supersampling
    int threads = 0;       // 0 = all cores
    unsigned seed = 1;
    std::optional<Camera> camera;  // overrides the scene's
};

struct RenderStats {
    double seconds = 0;
    std::size_t triangles = 0;
    int threads = 1;
    std::vector<std::string> warnings;  // missing textures and the like
};

// Renders the scene. Throws std::runtime_error for bad options.
raster::Image renderScene(const Scene& scene, const RenderOptions& options, RenderStats* stats = nullptr);

// Options from the scene's own settings.
RenderOptions optionsFrom(const Scene& scene);

// ---- documents and commands ----
class SceneDoc : public Document {
public:
    Scene scene;
    std::string kind() const override { return "scene"; }
    std::string summary() const override;
    std::unique_ptr<Attachment> preview() const override;  // fast rasterized PNG
};

// Undoable scene edit: snapshots the scene and restores it on undo or failure.
class SceneEdit : public Action {
public:
    SceneEdit(std::shared_ptr<SceneDoc> doc, std::function<Result(SceneDoc&)> fn) : doc_(std::move(doc)), fn_(std::move(fn)) {}
    Result apply() override;
    void revert() override;

private:
    std::shared_ptr<SceneDoc> doc_;
    std::function<Result(SceneDoc&)> fn_;
    Scene before_;
};

// Registers every render.* command. render.add mesh= takes ids of meshes opened
// with forge, cad or any tool that keeps a mesh document.
void registerCommands(Session& session);

}  // namespace wetwaresuite::render
