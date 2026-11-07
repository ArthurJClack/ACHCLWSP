// model.* commands end to end: edit mode with undo, objects, the modifier
// stack, UVs and materials, files, rendering, animation, picking and MCP.
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>

#include <nlohmann/json.hpp>

#include "check.hpp"
#include "wetwaresuite/mcp/server.hpp"
#include "wetwaresuite/mesh/io.hpp"
#include "wetwaresuite/model/model.hpp"
#include "wetwaresuite/raster/codec.hpp"

using namespace wetwaresuite;
using namespace wetwaresuite::model;
using json = nlohmann::ordered_json;
namespace fs = std::filesystem;

namespace {

Result run(Session& s, const std::string& id, const Params& p = {}) {
    Result r;
    try {
        r = s.run(id, p);
    } catch (const std::exception& e) {
        r = Result::failure(std::string("threw: ") + e.what());
    }
    if (!r.ok) std::printf("  %s failed: %s\n", id.c_str(), r.message.c_str());
    return r;
}

bool near(double a, double b, double eps = 1e-6) { return std::fabs(a - b) <= eps; }

std::shared_ptr<ModelDoc> docIn(Session& s, const std::string& id = "model1") { return s.workspace().getAs<ModelDoc>(id); }

const PolyMesh& meshNamed(Session& s, const std::string& object, const std::string& doc = "model1") {
    auto d = docIn(s, doc);
    return *d->meshOf(*d->object(object));
}

bool closedMesh(const PolyMesh& m) {
    std::string why;
    return valid(m, &why) && stats(m).closed;
}

double volumeOf(const PolyMesh& m) { return mesh::volume(triangulate(m).mesh); }

PolyMesh evaluated(Session& s, const std::string& object) {
    auto d = docIn(s);
    return evaluate(*d, *d->object(object));
}

json call(mcp::Server& server, int id, const std::string& tool, const json& args) {
    json req = {{"jsonrpc", "2.0"}, {"id", id}, {"method", "tools/call"}, {"params", {{"name", tool}, {"arguments", args}}}};
    return json::parse(*server.handle(req.dump()))["result"];
}

std::size_t orangePixels(const std::vector<std::uint8_t>& rgba) {
    std::size_t n = 0;
    for (std::size_t i = 0; i + 3 < rgba.size(); i += 4) n += rgba[i] > 230 && rgba[i + 1] > 130 && rgba[i + 1] < 210 && rgba[i + 2] < 100 ? 1 : 0;
    return n;
}

}  // namespace

int main() {
    const fs::path dir = fs::temp_directory_path() / "wetwaresuite_test_model";
    fs::remove_all(dir);
    fs::create_directories(dir);

    // ---- a new scene, edit mode, extrude, undo ----
    {
        Session s;
        registerCommands(s);
        Result r = run(s, "model.new", {{"preview", true}});
        CHECK(r.ok && toText(*r.field("doc")) == "model1" && r.attachments.size() == 1);
        auto d = docIn(s);
        CHECK(d && d->scene.objects.size() == 3 && d->scene.active == "Cube");
        CHECK(d->mesh.triangleCount() == 12 && d->kind() == "model");
        CHECK(run(s, "model.info").ok);

        CHECK(run(s, "model.select", {{"mode", std::string("face")}, {"type", std::string("normal")}, {"normal", NumberList{0, 0, 1}}, {"angle", 10.0}}).ok);
        CHECK(countSelection(meshNamed(s, "Cube")).faces == 1);
        CHECK(d->scene.editMode && d->scene.selectMode == SelectMode::Face);
        r = run(s, "model.extrude", {{"offset", 1.0}, {"preview", true}});
        CHECK(r.ok && r.attachments.size() == 1);
        CHECK(meshNamed(s, "Cube").faces.size() == 10 && closedMesh(meshNamed(s, "Cube")));
        CHECK(near(volumeOf(meshNamed(s, "Cube")), 12, 1e-6));
        CHECK(s.history().undo());
        CHECK(meshNamed(s, "Cube").faces.size() == 6);
        CHECK(s.history().redo());
        CHECK(meshNamed(s, "Cube").faces.size() == 10);

        // move the new top up by 1 in world space: volume grows by 4
        CHECK(run(s, "model.transform", {{"translate", NumberList{0, 0, 1}}}).ok);
        CHECK(near(volumeOf(meshNamed(s, "Cube")), 16, 1e-6));
        // scale it about its median
        CHECK(run(s, "model.transform", {{"scale", NumberList{0.5}}}).ok);
        CHECK(closedMesh(meshNamed(s, "Cube")));

        // inset + extrude inward makes a well
        CHECK(run(s, "model.inset", {{"thickness", 0.1}, {"depth", -0.2}}).ok);
        CHECK(closedMesh(meshNamed(s, "Cube")) && meshNamed(s, "Cube").faces.size() == 14);

        // the edit overlay draws the selection in orange
        std::vector<std::uint8_t> rgba;
        CHECK(d->renderView(OrbitView{}, 200, 200, rgba) && orangePixels(rgba) > 10);
        CHECK(run(s, "model.mode", {{"mode", std::string("object")}}).ok && !d->scene.editMode);
        CHECK(d->renderView(OrbitView{}, 200, 200, rgba) && orangePixels(rgba) > 20);  // selected object outline
        CHECK(run(s, "model.select_object", {{"action", std::string("none")}}).ok);
        CHECK(d->renderView(OrbitView{}, 200, 200, rgba) && orangePixels(rgba) < 5);
        CHECK(!run(s, "model.extrude", {{"object", std::string("Light")}}).ok);
    }

    // ---- bevel, loop cut, subdivide, bisect, knife, merge, dissolve, delete, bridge, fill ----
    {
        Session s;
        registerCommands(s);
        run(s, "model.new");
        CHECK(run(s, "model.mode", {{"mode", std::string("edit")}, {"select_mode", std::string("edge")}}).ok);
        CHECK(run(s, "model.select", {{"type", std::string("all")}}).ok);
        CHECK(run(s, "model.bevel", {{"width", 0.2}, {"segments", 2}}).ok);
        CHECK(closedMesh(meshNamed(s, "Cube")) && meshNamed(s, "Cube").faces.size() > 6);
        s.history().undo();

        const PolyMesh& cube = meshNamed(s, "Cube");
        const EdgeKey e = allEdges(cube).front();
        CHECK(run(s, "model.loop_cut", {{"edge", NumberList{double(keyA(e)), double(keyB(e))}}, {"cuts", std::int64_t{2}}}).ok);
        CHECK(meshNamed(s, "Cube").faces.size() == 6 + 2 * 4 && closedMesh(meshNamed(s, "Cube")));
        CHECK(countSelection(meshNamed(s, "Cube")).edges == 8);
        s.history().undo();

        CHECK(run(s, "model.select", {{"mode", std::string("face")}, {"type", std::string("all")}}).ok);
        CHECK(run(s, "model.subdivide", {{"cuts", std::int64_t{1}}}).ok);
        CHECK(meshNamed(s, "Cube").faces.size() == 24 && closedMesh(meshNamed(s, "Cube")));
        s.history().undo();

        CHECK(run(s, "model.bisect", {{"normal", NumberList{0, 0, 1}}, {"clear_inner", true}, {"fill", true}, {"selected", false}}).ok);
        CHECK(closedMesh(meshNamed(s, "Cube")) && near(volumeOf(meshNamed(s, "Cube")), 4, 1e-6));
        s.history().undo();

        // knife straight down the front view through the middle
        const std::size_t before = meshNamed(s, "Cube").faces.size();
        CHECK(run(s, "model.knife", {{"line", NumberList{0.5, 0.0, 0.5, 1.0}}, {"view", std::string("front")}, {"through", true}}).ok);
        CHECK(meshNamed(s, "Cube").faces.size() > before && closedMesh(meshNamed(s, "Cube")));
        s.history().undo();

        // merge the top four vertices into one: a pyramid
        CHECK(run(s, "model.select", {{"mode", std::string("vert")}, {"type", std::string("box")}, {"box", NumberList{-2, -2, 0.5, 2, 2, 2}}}).ok);
        CHECK(countSelection(meshNamed(s, "Cube")).verts == 4);
        Result r = run(s, "model.merge", {{"mode", std::string("center")}});
        CHECK(r.ok && meshNamed(s, "Cube").verts.size() == 5 && closedMesh(meshNamed(s, "Cube")));
        s.history().undo();

        // delete the top face, fill the hole again
        CHECK(run(s, "model.select", {{"mode", std::string("face")}, {"type", std::string("normal")}, {"normal", NumberList{0, 0, 1}}}).ok);
        CHECK(run(s, "model.delete", {}).ok && meshNamed(s, "Cube").faces.size() == 5 && !stats(meshNamed(s, "Cube")).closed);
        CHECK(run(s, "model.fill", {{"type", std::string("holes")}}).ok && closedMesh(meshNamed(s, "Cube")));
        s.history().undo();
        s.history().undo();

        // bridge top and bottom: a tunnel through the cube
        CHECK(run(s, "model.select", {{"type", std::string("normal")}, {"normal", NumberList{0, 0, 1}}}).ok);
        CHECK(run(s, "model.select", {{"type", std::string("normal")}, {"normal", NumberList{0, 0, -1}}, {"extend", true}}).ok);
        CHECK(countSelection(meshNamed(s, "Cube")).faces == 2);
        CHECK(run(s, "model.inset", {{"thickness", 0.3}, {"individual", true}}).ok);
        CHECK(run(s, "model.bridge").ok);
        CHECK(closedMesh(meshNamed(s, "Cube")) && stats(meshNamed(s, "Cube")).euler == 0);
        s.history().undo();
        s.history().undo();

        // dissolve an edge loop
        CHECK(run(s, "model.select", {{"type", std::string("all")}}).ok);
        CHECK(run(s, "model.subdivide").ok);
        CHECK(run(s, "model.dissolve", {{"type", std::string("limited")}}).ok);
        CHECK(meshNamed(s, "Cube").faces.size() == 6 && closedMesh(meshNamed(s, "Cube")));

        // select loop / ring / grow / linked
        run(s, "model.select", {{"mode", std::string("edge")}, {"type", std::string("none")}});
        CHECK(run(s, "model.select", {{"type", std::string("index")}, {"indices", NumberList{double(keyA(e)), double(keyB(e))}}}).ok);
        CHECK(countSelection(meshNamed(s, "Cube")).edges == 1);
        CHECK(run(s, "model.select_loop", {{"edge", NumberList{double(keyA(e)), double(keyB(e))}}, {"ring", true}}).ok);
        CHECK(countSelection(meshNamed(s, "Cube")).edges == 4);
        CHECK(run(s, "model.select_grow").ok);
        CHECK(run(s, "model.select_linked").ok && countSelection(meshNamed(s, "Cube")).verts == 8);
        CHECK(!run(s, "model.select_loop", {{"edge", NumberList{0, 0}}}).ok);
    }

    // ---- spin, mirror, symmetrize, duplicate, normals, add into the mesh ----
    {
        Session s;
        registerCommands(s);
        run(s, "model.new", {{"empty", true}});
        CHECK(run(s, "model.add", {{"type", std::string("circle")}, {"vertices", std::int64_t{8}}, {"radius", 0.25}, {"fill", std::string("none")},
                                   {"location", NumberList{1, 0, 0}}, {"rotation", NumberList{90, 0, 0}}}).ok);
        CHECK(run(s, "model.apply_transform", {{"location", true}}).ok);
        CHECK(run(s, "model.mode", {{"mode", std::string("edit")}}).ok);
        CHECK(run(s, "model.select", {{"type", std::string("all")}}).ok);
        CHECK(run(s, "model.spin", {{"angle", 360.0}, {"steps", std::int64_t{16}}, {"center", NumberList{0, 0, 0}}}).ok);
        const PolyMesh& ring = meshNamed(s, "Circle");
        CHECK(ring.faces.size() == 128 && closedMesh(ring));

        run(s, "model.new");
        auto cube = [&]() -> const PolyMesh& { return meshNamed(s, "Cube", "model2"); };
        CHECK(run(s, "model.mode", {{"doc", std::string("model2")}, {"mode", std::string("edit")}}).ok);
        CHECK(run(s, "model.select", {{"doc", std::string("model2")}, {"type", std::string("all")}}).ok);
        CHECK(run(s, "model.bisect", {{"doc", std::string("model2")}, {"normal", NumberList{1, 0, 0}}, {"clear_inner", true}}).ok);
        CHECK(!stats(cube()).closed);
        CHECK(run(s, "model.select", {{"doc", std::string("model2")}, {"type", std::string("none")}}).ok);
        CHECK(run(s, "model.mirror", {{"doc", std::string("model2")}, {"duplicate", true}}).ok);
        CHECK(closedMesh(cube()) && near(volumeOf(cube()), 8, 1e-6));
        // symmetrize after pulling one vertex out
        CHECK(run(s, "model.select", {{"doc", std::string("model2")}, {"mode", std::string("vert")}, {"type", std::string("box")}, {"box", NumberList{0.5, 0.5, 0.5, 2, 2, 2}}}).ok);
        CHECK(run(s, "model.transform", {{"doc", std::string("model2")}, {"translate", NumberList{0.5, 0, 0}}}).ok);
        CHECK(run(s, "model.symmetrize", {{"doc", std::string("model2")}, {"side", std::string("+x")}}).ok);
        CHECK(closedMesh(cube()));
        const mesh::Bounds b = bounds(cube());
        CHECK(near(b.min.x, -1.5, 1e-6) && near(b.max.x, 1.5, 1e-6));
        // add a primitive into the edited mesh, duplicate it, flip and fix normals
        CHECK(run(s, "model.add", {{"doc", std::string("model2")}, {"type", std::string("cube")}, {"size", 0.5}, {"location", NumberList{4, 0, 0}}}).ok);
        CHECK(docIn(s, "model2")->scene.objects.size() == 3 && cube().faces.size() > 12);
        const std::size_t faces = cube().faces.size();
        CHECK(run(s, "model.duplicate", {{"doc", std::string("model2")}, {"offset", NumberList{0, 3, 0}}}).ok);
        CHECK(cube().faces.size() == faces + 6);
        CHECK(run(s, "model.normals", {{"doc", std::string("model2")}, {"action", std::string("flip")}}).ok);
        CHECK(volumeOf(extractSelected(cube())) < 0);
        CHECK(run(s, "model.normals", {{"doc", std::string("model2")}}).ok);
        CHECK(volumeOf(extractSelected(cube())) > 0);
        CHECK(run(s, "model.separate", {{"doc", std::string("model2")}, {"type", std::string("loose")}}).ok);
        CHECK(docIn(s, "model2")->scene.objects.size() == 5);
        CHECK(!run(s, "model.close").ok);  // two open: which?
        CHECK(run(s, "model.close", {{"doc", std::string("model1")}}).ok);
        CHECK(run(s, "model.info").ok);  // the only one left
    }

    // ---- the modifier stack ----
    {
        Session s;
        registerCommands(s);
        run(s, "model.new");
        CHECK(run(s, "model.modifier_add", {{"type", std::string("subsurf")}, {"levels", std::int64_t{2}}}).ok);
        PolyMesh ev = evaluated(s, "Cube");
        CHECK(ev.faces.size() == 96 && closedMesh(ev) && meshNamed(s, "Cube").faces.size() == 6);
        CHECK(docIn(s)->mesh.triangleCount() == 192);
        CHECK(run(s, "model.modifier_set", {{"levels", std::int64_t{1}}}).ok && evaluated(s, "Cube").faces.size() == 24);
        CHECK(!run(s, "model.modifier_set", {{"count", std::int64_t{3}}}).ok);  // not an array
        CHECK(run(s, "model.modifier_add", {{"type", std::string("array")}, {"count", std::int64_t{3}}, {"merge", true}}).ok);
        CHECK(evaluated(s, "Cube").faces.size() == 72);  // rounded copies only touch at a point
        CHECK(run(s, "model.modifier_move", {{"modifier", std::string("Array")}, {"direction", std::string("up")}}).ok);
        CHECK(docIn(s)->object("Cube")->modifiers.front().type == "array");
        ev = evaluated(s, "Cube");
        CHECK(closedMesh(ev) && ev.faces.size() == 4 * (6 * 3 - 4));  // the merged bar, then smoothed
        CHECK(run(s, "model.modifier_remove", {{"index", std::int64_t{1}}}).ok);
        CHECK(run(s, "model.modifier_apply").ok);
        CHECK(meshNamed(s, "Cube").faces.size() == 14 && docIn(s)->object("Cube")->modifiers.empty() && closedMesh(meshNamed(s, "Cube")));
        CHECK(near(volumeOf(meshNamed(s, "Cube")), 24, 1e-6));

        // mirror a half
        run(s, "model.new");
        const Params d2{{"doc", std::string("model2")}};
        auto with = [](Params p, const std::string& k, Value v) {
            p[k] = std::move(v);
            return p;
        };
        CHECK(run(s, "model.bisect", with(with(with(d2, "normal", NumberList{1, 0, 0}), "clear_inner", true), "selected", false)).ok);
        CHECK(run(s, "model.mode", with(d2, "mode", std::string("object"))).ok);
        CHECK(run(s, "model.modifier_add", with(d2, "type", std::string("mirror"))).ok);
        {
            auto d = docIn(s, "model2");
            PolyMesh m = evaluate(*d, *d->object("Cube"));
            CHECK(closedMesh(m) && near(volumeOf(m), 8, 1e-6) && m.faces.size() == 6 + 4);
        }
        // solidify a plane, bevel and boolean the cube
        CHECK(run(s, "model.add", with(with(d2, "type", std::string("plane")), "location", NumberList{5, 0, 0})).ok);
        CHECK(run(s, "model.modifier_add", with(with(d2, "type", std::string("solidify")), "thickness", 0.2)).ok);
        {
            auto d = docIn(s, "model2");
            PolyMesh m = evaluate(*d, *d->object("Plane"));
            CHECK(closedMesh(m) && near(volumeOf(m), 0.8, 1e-6));
        }
        run(s, "model.new");
        const Params d3{{"doc", std::string("model3")}};
        CHECK(run(s, "model.modifier_add", with(with(with(d3, "type", std::string("bevel")), "width", 0.2), "segments", std::int64_t{3})).ok);
        {
            auto d = docIn(s, "model3");
            PolyMesh m = evaluate(*d, *d->object("Cube"));
            CHECK(closedMesh(m) && m.faces.size() > 6 && volumeOf(m) < 8 && volumeOf(m) > 7.5);
        }
        CHECK(run(s, "model.modifier_remove", d3).ok);
        CHECK(run(s, "model.add", with(with(with(d3, "type", std::string("cylinder")), "radius", 0.5), "depth", 3.0)).ok);
        CHECK(run(s, "model.select_object", with(with(d3, "objects", std::string("Cube")), "active", std::string("Cube"))).ok);
        CHECK(!run(s, "model.modifier_add", with(d3, "type", std::string("boolean"))).ok);  // no cutter
        CHECK(run(s, "model.modifier_add", with(with(d3, "type", std::string("boolean")), "cutter", std::string("Cylinder"))).ok);
        {
            auto d = docIn(s, "model3");
            CHECK(!d->object("Cylinder")->visible);
            PolyMesh m = evaluate(*d, *d->object("Cube"));
            const double v = volumeOf(m);
            CHECK(closedMesh(m) && v < 8 - 1.4 && v > 8 - 2 * 3.1416 * 0.25 - 0.05);
            CHECK(stats(m).euler == 0);  // a hole through it
            CHECK(m.faces.size() < 80);   // flat parts joined back into n-gons
            CHECK(d->evaluated("Cube").errors.empty());
        }
        // moving the cutter re-evaluates
        const double before = docIn(s, "model3")->mesh.triangleCount();
        CHECK(run(s, "model.object_set", with(with(d3, "object", std::string("Cylinder")), "move", NumberList{10, 0, 0})).ok);
        CHECK(docIn(s, "model3")->mesh.triangleCount() == 12 && before > 12);
        CHECK(run(s, "model.modifier_remove", d3).ok);
        // decimate, smooth, displace, weld, triangulate on a sphere
        CHECK(run(s, "model.add", with(with(d3, "type", std::string("uv_sphere")), "location", NumberList{0, 0, 4})).ok);
        CHECK(run(s, "model.modifier_add", with(with(d3, "type", std::string("decimate")), "ratio", 0.25)).ok);
        {
            auto d = docIn(s, "model3");
            const PolyMesh m = evaluate(*d, *d->object("Sphere"));
            CHECK(stats(m).closed && triangulate(m).mesh.triangleCount() <= triangulate(*d->meshOf(*d->object("Sphere"))).mesh.triangleCount() / 3);
        }
        CHECK(run(s, "model.modifier_set", with(d3, "decimate", std::string("planar"))).ok);
        CHECK(run(s, "model.modifier_add", with(with(d3, "type", std::string("displace")), "strength", 0.3)).ok);
        CHECK(run(s, "model.modifier_add", with(d3, "type", std::string("smooth"))).ok);
        CHECK(run(s, "model.modifier_add", with(d3, "type", std::string("weld"))).ok);
        CHECK(run(s, "model.modifier_add", with(d3, "type", std::string("triangulate"))).ok);
        {
            auto d = docIn(s, "model3");
            const PolyMesh m = evaluate(*d, *d->object("Sphere"));
            CHECK(closedMesh(m) && stats(m).quads == 0 && stats(m).ngons == 0 && d->evaluated("Sphere").errors.empty());
        }
        CHECK(run(s, "model.modifier_apply", with(d3, "all", true)).ok);
        CHECK(docIn(s, "model3")->object("Sphere")->modifiers.empty() && stats(meshNamed(s, "Sphere", "model3")).quads == 0);
        // wireframe: every edge of a cube becomes a beam; screw: a ring of edges spun into a closed tube
        CHECK(run(s, "model.add", with(with(d3, "type", std::string("cube")), "location", NumberList{0, 0, 8})).ok);
        CHECK(run(s, "model.modifier_add", with(with(d3, "type", std::string("wireframe")), "thickness", 0.05)).ok);
        {
            auto d = docIn(s, "model3");
            const PolyMesh m = evaluate(*d, *d->activeObject());
            CHECK(m.faces.size() == 12 * 6 && d->evaluated(d->scene.active).errors.empty());
        }
        CHECK(run(s, "model.add", with(with(with(with(d3, "type", std::string("circle")), "fill", std::string("none")), "vertices", std::int64_t{8}),
                                       "location", NumberList{0, 0, 12})).ok);
        CHECK(run(s, "model.modifier_add", with(with(with(with(d3, "type", std::string("screw")), "axis", std::string("y")), "segments", std::int64_t{12}),
                                       "angle", 180.0)).ok);
        {
            auto d = docIn(s, "model3");
            const PolyMesh m = evaluate(*d, *d->activeObject());
            // half a turn about the circle's own diameter sweeps a sphere once: 8 edges x 12 steps
            CHECK(m.faces.size() == 8 * 12 && d->evaluated(d->scene.active).errors.empty());
        }
        CHECK(!run(s, "model.modifier_set", with(d3, "axis", std::string("w"))).ok);
    }

    // ---- objects: linked duplicates, join, parent, apply transform, origin, collections ----
    {
        Session s;
        registerCommands(s);
        run(s, "model.new");
        CHECK(run(s, "model.duplicate_object", {{"linked", true}, {"offset", NumberList{3, 0, 0}}}).ok);
        auto d = docIn(s);
        CHECK(d->object("Cube.001") && d->object("Cube.001")->data == d->object("Cube")->data);
        CHECK(run(s, "model.select", {{"type", std::string("all")}}).ok);
        CHECK(run(s, "model.subdivide").ok);
        CHECK(meshNamed(s, "Cube").faces.size() == 24 && d->meshOf(*d->object("Cube"))== d->meshOf(*d->object("Cube.001")));
        CHECK(d->mesh.triangleCount() == 96);
        s.history().undo();
        CHECK(run(s, "model.duplicate_object", {{"object", std::string("Cube")}, {"offset", NumberList{0, 3, 0}}}).ok);
        CHECK(d->object("Cube.002")->data != d->object("Cube")->data);
        CHECK(run(s, "model.select_object", {{"objects", std::string("Cube,Cube.001,Cube.002")}, {"active", std::string("Cube")}}).ok);
        CHECK(run(s, "model.join").ok);
        CHECK(d->scene.objects.size() == 3 && meshNamed(s, "Cube").faces.size() == 18 && looseParts(meshNamed(s, "Cube")).size() == 3);
        CHECK(d->scene.meshes.size() == 1);

        CHECK(run(s, "model.add", {{"type", std::string("empty")}, {"location", NumberList{0, 0, 5}}}).ok);
        CHECK(run(s, "model.parent", {{"objects", std::string("Cube")}, {"parent", std::string("Empty")}}).ok);
        CHECK(near(d->world(*d->object("Cube")).point({0, 0, 0}).z, 0, 1e-9) && near(d->object("Cube")->location.z, -5, 1e-9));
        CHECK(run(s, "model.object_set", {{"object", std::string("Empty")}, {"turn", NumberList{0, 0, 90}}}).ok);
        CHECK(near(d->world(*d->object("Cube")).point({3, 0, 0}).y, 3, 1e-9));
        CHECK(!run(s, "model.parent", {{"objects", std::string("Empty")}, {"parent", std::string("Cube")}}).ok);  // a loop
        CHECK(run(s, "model.delete_object", {{"objects", std::string("Empty")}}).ok);
        CHECK(d->object("Cube")->parent.empty() && near(d->world(*d->object("Cube")).point({3, 0, 0}).y, 3, 1e-9));
        CHECK(near(d->object("Cube")->rotation.z, 90, 1e-9));

        CHECK(run(s, "model.object_set", {{"object", std::string("Cube")}, {"scale", NumberList{2}}, {"rotation", NumberList{0, 0, 0}}}).ok);
        CHECK(run(s, "model.apply_transform", {{"object", std::string("Cube")}}).ok);
        const mesh::Bounds b = bounds(meshNamed(s, "Cube"));
        CHECK(near(b.max.x - b.min.x, 2 * 5, 1e-9) && d->object("Cube")->scale.x == 1);
        CHECK(run(s, "model.origin", {{"object", std::string("Cube")}, {"to", std::string("bottom")}}).ok);
        const mesh::Bounds b2 = bounds(meshNamed(s, "Cube"));
        CHECK(near(b2.min.z, 0, 1e-9) && near((b2.min.x + b2.max.x) / 2, 0, 1e-9));
        CHECK(near(d->world(*d->object("Cube")).point(b2.min).x, d->object("Cube")->location.x + b2.min.x, 1e-9));

        CHECK(run(s, "model.collection", {{"name", std::string("Props")}, {"objects", std::string("Cube")}}).ok);
        CHECK(run(s, "model.collection", {{"name", std::string("Props")}, {"action", std::string("hide")}}).ok);
        CHECK(d->mesh.triangleCount() == 0);
        CHECK(run(s, "model.collection", {{"name", std::string("Props")}, {"action", std::string("remove")}}).ok);
        CHECK(d->mesh.triangleCount() == 36 && d->object("Cube")->collection == "Collection");
        CHECK(run(s, "model.shade", {{"type", std::string("auto")}, {"angle", 30.0}, {"object", std::string("Cube")}}).ok);
        CHECK(meshNamed(s, "Cube").faces[0].smooth && d->object("Cube")->autoSmooth == 30);
        CHECK(run(s, "model.cursor", {{"location", NumberList{1, 2, 3}}}).ok && d->scene.cursor.y == 2);
    }

    // ---- UVs, materials and files ----
    {
        Session s;
        registerCommands(s);
        run(s, "model.new");
        CHECK(run(s, "model.uv_unwrap", {{"method", std::string("cube")}}).ok && meshNamed(s, "Cube").hasUvs());
        CHECK(run(s, "model.uv_unwrap", {{"method", std::string("smart")}}).ok);
        CHECK(run(s, "model.mode", {{"select_mode", std::string("edge")}}).ok);
        CHECK(run(s, "model.mark", {{"type", std::string("seam")}, {"angle", 30.0}}).ok && meshNamed(s, "Cube").seams.size() == 12);
        CHECK(run(s, "model.uv_unwrap", {{"method", std::string("unwrap")}}).ok);
        CHECK(run(s, "model.uv_pack").ok);
        const fs::path layout = dir / "layout.png";
        CHECK(run(s, "model.uv_export", {{"path", layout.string()}, {"size", std::int64_t{128}}}).ok && raster::load(layout).width() == 128);
        CHECK(run(s, "model.material", {{"name", std::string("Red")}, {"color", std::string("#cc2222")}, {"roughness", 0.3}, {"assign", false}}).ok);
        CHECK(run(s, "model.select", {{"mode", std::string("face")}, {"type", std::string("normal")}, {"normal", NumberList{0, 0, 1}}}).ok);
        CHECK(run(s, "model.material_assign", {{"material", std::string("Red")}}).ok);
        auto d = docIn(s);
        CHECK(d->object("Cube")->materials.size() == 2);
        std::size_t red = 0;
        for (const auto& f : meshNamed(s, "Cube").faces) red += f.material == 1 ? 1 : 0;
        CHECK(red == 1);
        CHECK(d->previewOptions.faceColors.size() == d->mesh.triangleCount());
        CHECK(run(s, "model.material", {{"name", std::string("Gold")}, {"preset", std::string("gold")}, {"assign", false}}).ok);
        CHECK(d->material("Gold")->metallic > 0.5);
        CHECK(run(s, "model.vertex_color", {{"color", std::string("#00ff00")}}).ok && meshNamed(s, "Cube").colors);
        CHECK(run(s, "model.mode", {{"mode", std::string("object")}}).ok);
        CHECK(run(s, "model.modifier_add", {{"type", std::string("bevel")}, {"width", 0.1}}).ok);
        CHECK(run(s, "model.keyframe", {{"frame", std::int64_t{1}}, {"location", NumberList{0, 0, 0}}}).ok);

        // .wwmodel round trip keeps everything
        const fs::path saved = dir / "scene.wwmodel";
        CHECK(run(s, "model.save", {{"path", saved.string()}}).ok);
        CHECK(run(s, "model.open", {{"path", saved.string()}}).ok);
        auto e = docIn(s, "model2");
        CHECK(e && e->scene.objects.size() == 3 && e->object("Cube")->modifiers.size() == 1 && e->object("Cube")->materials.size() == 2);
        CHECK(meshNamed(s, "Cube", "model2").seams.size() == 12 && meshNamed(s, "Cube", "model2").hasUvs() && meshNamed(s, "Cube", "model2").colors);
        CHECK(e->object("Cube")->keys.size() == 3 && e->mesh.triangleCount() == d->mesh.triangleCount());
        CHECK(!run(s, "model.save", {{"doc", std::string("model2")}, {"path", (dir / "x.obj").string()}}).ok);

        // OBJ keeps quads, UVs, materials and objects
        const fs::path obj = dir / "cube.obj";
        CHECK(run(s, "model.export", {{"doc", std::string("model1")}, {"path", obj.string()}, {"modifiers", false}}).ok);
        CHECK(fs::exists(dir / "cube.mtl"));
        CHECK(run(s, "model.open", {{"path", obj.string()}}).ok);
        auto f = docIn(s, "model3");
        const Object* imported = f ? f->object("Cube") : nullptr;
        CHECK(imported && f->meshOf(*imported)->faces.size() == 6 && f->meshOf(*imported)->hasUvs() && imported->materials.size() == 2);
        CHECK(f->material("Red") && near(f->material("Red")->color.x, 0.8, 0.01));
        CHECK(closedMesh(*f->meshOf(*imported)));
        // with modifiers applied the bevel shows up
        CHECK(run(s, "model.export", {{"doc", std::string("model1")}, {"path", (dir / "bevelled.obj").string()}}).ok);
        CHECK(run(s, "model.import", {{"doc", std::string("model3")}, {"path", (dir / "bevelled.obj").string()}}).ok);
        CHECK(f->object("Cube.001") && f->meshOf(*f->object("Cube.001"))->faces.size() == 26);

        // triangle formats
        for (const char* ext : {"glb", "fbx", "stl", "gltf"}) {
            const fs::path p = dir / (std::string("cube.") + ext);
            CHECK(run(s, "model.export", {{"doc", std::string("model1")}, {"path", p.string()}, {"modifiers", false}}).ok);
            const mesh::Mesh m = mesh::load(p);
            CHECK(m.triangleCount() == 12);
        }
        CHECK(run(s, "model.import", {{"doc", std::string("model3")}, {"path", (dir / "cube.stl").string()}}).ok);
        const Object* stl = f->object("cube");
        CHECK(stl && f->meshOf(*stl)->faces.size() == 6 && stats(*f->meshOf(*stl)).quads == 6 && closedMesh(*f->meshOf(*stl)));
        CHECK(!run(s, "model.export", {{"doc", std::string("model1")}, {"path", (dir / "x.xyz").string()}}).ok);
        const fs::path png = dir / "view.png";
        CHECK(run(s, "model.export", {{"doc", std::string("model1")}, {"path", png.string()}, {"size", std::int64_t{64}}}).ok && raster::load(png).width() == 64);
    }

    // ---- rendering and animation ----
    {
        Session s;
        registerCommands(s);
        run(s, "model.new");
        run(s, "model.material", {{"name", std::string("Material")}, {"color", std::string("#3366cc")}});
        Result r = run(s, "model.render", {{"width", std::int64_t{48}}, {"height", std::int64_t{32}}, {"mode", std::string("preview")}});
        CHECK(r.ok && r.attachments.size() == 1);
        const raster::Image img = raster::decode(r.attachments[0].bytes);
        CHECK(img.width() == 48 && img.height() == 32);
        // the cube is in the middle of the camera's picture and blue-ish
        const raster::Color c = img.at(24, 16);
        CHECK(c.b > c.r);
        r = run(s, "model.render", {{"width", std::int64_t{32}}, {"height", std::int64_t{24}}, {"samples", std::int64_t{2}},
                                    {"camera", std::string("view")}, {"ground", true}, {"path", (dir / "r.png").string()}});
        CHECK(r.ok && fs::exists(dir / "r.png"));
        CHECK(!run(s, "model.render", {{"camera", std::string("Cube")}}).ok);
        r = run(s, "model.to_render", {{"width", std::int64_t{64}}, {"height", std::int64_t{48}}});
        CHECK(r.ok && s.workspace().get("scene1"));

        CHECK(run(s, "model.keyframe", {{"object", std::string("Cube")}, {"frame", std::int64_t{1}}, {"location", NumberList{0, 0, 0}}}).ok);
        CHECK(run(s, "model.keyframe", {{"object", std::string("Cube")}, {"frame", std::int64_t{11}}, {"location", NumberList{0, 0, 4}},
                                        {"interp", std::string("linear")}}).ok);
        auto d = docIn(s);
        CHECK(d->object("Cube")->keys.at("location.z").size() == 2);
        CHECK(run(s, "model.frame", {{"frame", std::int64_t{6}}}).ok);
        CHECK(near(d->object("Cube")->location.z, 2, 1e-9));  // halfway, eased (bezier on the first key)
        CHECK(run(s, "model.frame", {{"frame", std::int64_t{20}}}).ok && near(d->object("Cube")->location.z, 4, 1e-9));
        CHECK(run(s, "model.frame", {{"start", std::int64_t{1}}, {"end", std::int64_t{11}}, {"fps", 12.0}}).ok);
        const fs::path gif = dir / "anim.gif";
        r = run(s, "model.render_animation", {{"path", gif.string()}, {"width", std::int64_t{24}}, {"height", std::int64_t{16}}, {"step", std::int64_t{5}}});
        CHECK(r.ok && fs::exists(gif) && fs::file_size(gif) > 100);
        r = run(s, "model.render_animation", {{"path", (dir / "seq.png").string()}, {"width", std::int64_t{16}}, {"height", std::int64_t{16}}, {"start", std::int64_t{1}}, {"end", std::int64_t{2}}});
        CHECK(r.ok && fs::exists(dir / "seq_0001.png") && fs::exists(dir / "seq_0002.png"));
        CHECK(run(s, "model.keyframe", {{"object", std::string("Cube")}, {"frame", std::int64_t{11}}, {"delete", true}}).ok);
        CHECK(d->object("Cube")->keys.at("location.z").size() == 1);
    }

    // ---- picking in the viewport ----
    {
        Session s;
        registerCommands(s);
        run(s, "model.new");
        run(s, "model.select_object", {{"action", std::string("none")}});
        Result r = run(s, "model.pick", {{"u", 0.5}, {"v", 0.5}, {"view", std::string("front")}});
        auto d = docIn(s);
        CHECK(r.ok && d->object("Cube")->selected && d->scene.active == "Cube");
        CHECK(run(s, "model.pick", {{"u", 0.01}, {"v", 0.01}, {"view", std::string("front")}}).ok && !d->object("Cube")->selected);
        CHECK(run(s, "model.mode", {{"object", std::string("Cube")}, {"mode", std::string("edit")}, {"select_mode", std::string("face")}}).ok);
        CHECK(run(s, "model.select", {{"type", std::string("none")}}).ok);
        CHECK(run(s, "model.pick", {{"u", 0.5}, {"v", 0.5}, {"view", std::string("front")}}).ok);
        auto mesh = [&]() -> const PolyMesh& { return meshNamed(s, "Cube"); };
        const PolyMesh& m = mesh();
        const auto faces = selectedFaces(m);
        CHECK(faces.size() == 1 && faces.size() == 1 && faceNormal(m, m.faces[faces[0]]).y < -0.9);  // the front face (faces -y)
        CHECK(run(s, "model.mode", {{"select_mode", std::string("vert")}}).ok);
        CHECK(run(s, "model.select", {{"type", std::string("rect")}, {"rect", NumberList{0, 0, 1, 1}}, {"view", std::string("front")}}).ok);
        CHECK(countSelection(mesh()).verts == 4);  // only the visible front vertices
        CHECK(run(s, "model.select", {{"type", std::string("rect")}, {"rect", NumberList{0, 0, 1, 1}}, {"view", std::string("front")}, {"xray", true}}).ok);
        CHECK(countSelection(mesh()).verts == 8);
        CHECK(run(s, "model.select", {{"type", std::string("none")}}).ok);
        // the view fits the floor grid too (6 m across), so the 2 m cube's top-left corner is at about 0.4, 0.4
        CHECK(run(s, "model.pick", {{"u", 0.4}, {"v", 0.4}, {"view", std::string("front")}}).ok && countSelection(mesh()).verts == 1);
        CHECK(run(s, "model.mode", {{"select_mode", std::string("edge")}}).ok);
        CHECK(run(s, "model.pick", {{"u", 0.5}, {"v", 0.4}, {"view", std::string("front")}}).ok && countSelection(mesh()).edges == 1);
        r = run(s, "model.elements", {{"what", std::string("edges")}, {"selected", true}});
        CHECK(r.ok && r.message.find("*") != std::string::npos);
    }

    // ---- dragging with the mouse (G / R / S in the window) ----
    {
        Session s;
        registerCommands(s);
        CHECK(run(s, "model.new", {}).ok);
        CHECK(run(s, "model.select_object", {{"objects", std::string("Cube")}}).ok);
        CHECK(run(s, "model.drag", {{"dx", 100.0}, {"axis", std::string("x")}, {"view", std::string("front")}}).ok);
        auto d = docIn(s);
        const Object* cube = d->object("Cube");
        CHECK(cube->location.x > 1 && std::fabs(cube->location.y) < 1e-9 && std::fabs(cube->location.z) < 1e-9);
        // the window puts its move arrows where model.pivot says the selection is
        Result pivot = run(s, "model.pivot", {});
        CHECK(pivot.ok && pivot.field("centre") && std::get<NumberList>(*pivot.field("centre"))[0] == cube->location.x);
        CHECK(run(s, "model.drag", {{"dx", 100.0}, {"mode", std::string("scale")}}).ok && std::fabs(d->object("Cube")->scale.x - 2) < 1e-9);
        CHECK(run(s, "model.select_object", {{"action", std::string("none")}}).ok);
        CHECK(!run(s, "model.drag", {{"dx", 10.0}}).ok);  // nothing selected
        CHECK(std::get<std::int64_t>(*run(s, "model.pivot", {}).field("selected")) == 0);
    }

    // ---- MCP ----
    {
        Session ms;
        registerCommands(ms);
        mcp::Server server(ms, {"wetwaresuite", "test", "model"});
        json list = json::parse(*server.handle(R"({"jsonrpc":"2.0","id":1,"method":"tools/list"})"))["result"]["tools"];
        std::size_t modelTools = 0;
        for (const auto& t : list) {
            const std::string name = t["name"];
            if (name.rfind("model_", 0) == 0) ++modelTools;
            if (name == "model_info" || name == "model_new" || name == "model_elements") CHECK(t["annotations"]["readOnlyHint"] == true);
            if (name == "model_export" || name == "model_extrude") CHECK(t["annotations"]["readOnlyHint"] == false);
        }
        CHECK(modelTools >= 55);
        json o = call(server, 2, "model_new", {{"preview", true}});
        CHECK(o["isError"] == false && o["structuredContent"]["doc"] == "model1" && o["content"].size() == 2);
        json x = call(server, 3, "model_select", {{"type", "all"}});
        CHECK(x["isError"] == false);
        x = call(server, 4, "model_extrude", {{"offset", 0.5}, {"individual", true}, {"preview", true}});
        CHECK(x["isError"] == false && x["content"].size() == 2 && x["structuredContent"]["faces"] == 30);
        CHECK(call(server, 5, "undo", json::object())["isError"] == false);
        CHECK(docIn(ms)->meshOf(*docIn(ms)->object("Cube"))->faces.size() == 6);
    }

    fs::remove_all(dir);
    return check::summary("model");
}
