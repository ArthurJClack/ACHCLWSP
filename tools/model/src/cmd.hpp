// Shared pieces of the model.* commands (internal).
#pragma once

#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "wetwaresuite/model/model.hpp"
#include "wetwaresuite/render/render.hpp"

namespace wetwaresuite::model::cmd {

using EditFn = std::function<Result(ModelDoc&, const Params&)>;
using MeshFn = std::function<Result(ModelDoc&, Object&, PolyMesh&, const Params&)>;

ParamSpec docParam();     // "doc": the model (optional when one is open)
ParamSpec objectParam();  // "object": defaults to the active object
// doc + extra + preview/view/yaw/pitch
std::vector<ParamSpec> withDoc(std::vector<ParamSpec> extra, bool preview = true);

// The model the params name (or the only one open); null and an error otherwise.
std::shared_ptr<ModelDoc> findDoc(Session& s, const Params& p, std::string& id, std::string& error);
// Undoable command on a model (a ModelEdit), or a plain one.
ActionFactory edit(Session& s, EditFn fn);
ActionFactory query(Session& s, EditFn fn);
// Edit-mode mesh operators: the object (object= or the active one) must be a
// mesh; runs fn on its own copy of the mesh and enters edit mode.
EditFn meshEdit(MeshFn fn);

Object& target(ModelDoc& doc, const Params& p);  // object= or active; throws std::runtime_error
// Objects named in "objects" (comma separated), else the selected ones, else the active one.
std::vector<Object*> targets(ModelDoc& doc, const Params& p);

// Success with doc/object/counts fields, plus a preview when preview=true.
Result done(const std::string& message, ModelDoc& doc, const Params& p);
Result& addPreview(Result& r, const ModelDoc& doc, const Params& p);
OrbitView viewFrom(const Params& p);  // view=/yaw=/pitch=/zoom=

// Params
Vec3 vec3(const Params& p, const std::string& key, const Vec3& fallback);  // throws when not 3 numbers
bool has(const Params& p, const std::string& key);
Vec3 color(const Params& p, const std::string& key, const Vec3& fallback);  // "#rrggbb" or names -> sRGB 0..1
std::vector<std::string> splitList(const std::string& s);
std::string num(double v);
std::string many(std::size_t n, const std::string& word);  // "1 object", "3 objects"
std::string hex(const Vec3& c);
void selectionFields(Result& r, const PolyMesh& m);

// Sets location/rotation/scale from a matrix (no shear), and from a world matrix under the object's parent.
void setLocal(Object& o, const Mat4& local);
void setWorld(ModelDoc& doc, Object& o, const Mat4& world);
// Meshes no object uses any more are dropped.
void purgeMeshes(ModelDoc& doc);
// Gives the object a mesh of its own when other objects share it; returns it.
PolyMesh& singleUser(ModelDoc& doc, Object& o);

// Turns the model into a render scene: evaluated meshes split by material,
// its lights (or default ones), and a camera from a camera object or the view.
struct RenderSetup {
    std::string camera;  // camera object, "view" (yaw/pitch), or empty for the scene camera / fitted view
    double yaw = 45, pitch = 25;
    bool ground = false;
    int width = 640, height = 480;
};
render::Scene toRenderScene(const ModelDoc& doc, const RenderSetup& setup);

void registerDocCommands(Session& s);
void registerObjectCommands(Session& s);
void registerObjectCommands2(Session& s);
void registerSelectCommands(Session& s);
void registerEditCommands(Session& s);
void registerEditCommands2(Session& s);
void registerModifierCommands(Session& s);
void registerMaterialCommands(Session& s);
void registerRenderCommands(Session& s);
void registerRigCommands(Session& s);

}  // namespace wetwaresuite::model::cmd
