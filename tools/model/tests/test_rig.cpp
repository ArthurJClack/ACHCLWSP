// The newer model features end to end: armatures, skinning, weight paint and skinned
// glTF export; the dope sheet commands; drag snapping, rings and scale boxes;
// viewport shading; the world (hdri) and normal/roughness maps; and the render
// tool reading OBJ files with several materials.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>

#include "check.hpp"
#include "wetwaresuite/model/model.hpp"
#include "wetwaresuite/raster/codec.hpp"
#include "wetwaresuite/render/render.hpp"

using namespace wetwaresuite;
using namespace wetwaresuite::model;
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

std::string str(const char* s) { return s; }

// World-space positions of an object's evaluated mesh (modifiers, armature pose).
std::vector<Vec3> evaluatedPoints(Session& s, const std::string& object) {
    auto d = docIn(s);
    d->refresh();
    return d->evaluated(object).tris.mesh.positions;
}

// The corner of the evaluated mesh nearest `p` in the rest pose, by index in `rest`.
std::size_t nearestIndex(const std::vector<Vec3>& pts, const Vec3& p) {
    std::size_t best = 0;
    for (std::size_t i = 1; i < pts.size(); ++i)
        if (mesh::length(pts[i] - p) < mesh::length(pts[best] - p)) best = i;
    return best;
}

std::string readFile(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

void writePng(const fs::path& path, int w, int h, std::uint8_t r, std::uint8_t g, std::uint8_t b) {
    raster::Image img(w, h);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            std::uint8_t* px = img.pixel(x, y);
            px[0] = r, px[1] = g, px[2] = b, px[3] = 255;
        }
    raster::save(img, path);
}

}  // namespace

int main() {
    const fs::path dir = fs::temp_directory_path() / "wetwaresuite_test_rig";
    fs::remove_all(dir);
    fs::create_directories(dir);

    // ---- armature, skin, pose ----
    {
        Session s;
        registerCommands(s);
        CHECK(run(s, "model.new").ok);
        CHECK(run(s, "model.armature_add", {{"location", NumberList{0, 0, -1}}, {"bone", str("Root")}, {"length", 1.0}}).ok);
        CHECK(run(s, "model.bone_add", {{"name", str("Top")}, {"head", NumberList{0, 0, 1}}, {"tail", NumberList{0, 0, 2.5}}, {"parent", str("Root")}}).ok);
        Result info = run(s, "model.rig_info");
        CHECK(info.ok && info.field("bones") && toText(*info.field("bones")).find("Top") != std::string::npos);

        CHECK(run(s, "model.skin", {{"object", str("Cube")}, {"armature", str("Armature")}}).ok);
        auto d = docIn(s);
        const Object* cube = d->object("Cube");
        CHECK(cube && cube->weights.count("Root") && cube->weights.count("Top"));
        CHECK(!cube->modifiers.empty() && cube->modifiers.front().type == "armature");

        const std::vector<Vec3> rest = evaluatedPoints(s, "Cube");
        CHECK(!rest.empty());
        const std::size_t top = nearestIndex(rest, {1, 1, 1}), bottom = nearestIndex(rest, {1, 1, -1});

        // turning the top bone about its own axis twists the top of the cube far more than the bottom
        CHECK(run(s, "model.pose", {{"armature", str("Armature")}, {"bone", str("Top")}, {"rotation", NumberList{0, 90, 0}}}).ok);
        const std::vector<Vec3> posed = evaluatedPoints(s, "Cube");
        CHECK(posed.size() == rest.size());
        if (posed.size() == rest.size()) {
            const double moveTop = mesh::length(posed[top] - rest[top]);
            const double moveBottom = mesh::length(posed[bottom] - rest[bottom]);
            CHECK(moveTop > 0.5);
            CHECK(moveTop > moveBottom * 1.5);
        }

        // painting the top bone's weight onto the bottom corner makes it follow the top
        const double before = cube->weights.at("Top").empty() ? 0 : *std::max_element(cube->weights.at("Top").begin(), cube->weights.at("Top").end());
        CHECK(before > 0);
        CHECK(run(s, "model.weight_paint", {{"object", str("Cube")}, {"bone", str("Top")}, {"at", NumberList{1, 1, -1}}, {"radius", 0.4}, {"strength", 1.0},
                                            {"weight", 1.0}, {"mode", str("replace")}})
                  .ok);
        const std::vector<Vec3> painted = evaluatedPoints(s, "Cube");
        if (painted.size() == rest.size()) CHECK(mesh::length(painted[bottom] - rest[bottom]) > 1.0);
        CHECK(!run(s, "model.weight_paint", {{"object", str("Cube")}, {"bone", str("Nope")}, {"at", NumberList{1, 1, -1}}}).ok);

        // pose keys show up as a dope sheet row, move and delete like any other keys
        CHECK(run(s, "model.pose", {{"bone", str("Top")}, {"rotation", NumberList{0, 0, 0}}, {"key", true}, {"frame", std::int64_t{1}}}).ok);
        CHECK(run(s, "model.pose", {{"bone", str("Top")}, {"rotation", NumberList{0, 45, 0}}, {"key", true}, {"frame", std::int64_t{10}}}).ok);
        Result keys = run(s, "model.keys");
        CHECK(keys.ok && toText(*keys.field("rows")).find("Armature\tpose.Top.rotation\t1,10") != std::string::npos);

        // skinned glTF keeps the bones, weights and the animation
        const fs::path glb = dir / "rig.glb";
        CHECK(run(s, "model.export", {{"path", glb.string()}}).ok);
        const std::string bytes = readFile(glb);
        CHECK(bytes.size() > 100 && bytes.compare(0, 4, "glTF") == 0);
        CHECK(bytes.find("\"skins\"") != std::string::npos && bytes.find("JOINTS_0") != std::string::npos && bytes.find("WEIGHTS_0") != std::string::npos);
        CHECK(bytes.find("\"animations\"") != std::string::npos);

        // bones and weights survive .wwmodel
        const fs::path ww = dir / "rig.wwmodel";
        CHECK(run(s, "model.save", {{"path", ww.string()}}).ok);
        Session s2;
        registerCommands(s2);
        CHECK(run(s2, "model.open", {{"path", ww.string()}}).ok);
        auto d2 = docIn(s2);
        CHECK(d2 && d2->object("Armature") && d2->object("Armature")->bones.size() == 2 && d2->object("Cube")->weights.count("Top"));
    }

    // ---- dope sheet: keys, move, copy, delete, scrub ----
    {
        Session s;
        registerCommands(s);
        CHECK(run(s, "model.new").ok);
        CHECK(run(s, "model.keyframe", {{"frame", std::int64_t{1}}}).ok);
        CHECK(run(s, "model.keyframe", {{"frame", std::int64_t{20}}, {"location", NumberList{4, 0, 0}}, {"channels", str("location")}}).ok);
        Result k = run(s, "model.keys");
        CHECK(k.ok && toText(*k.field("rows")).find("Cube\tall\t1,20") != std::string::npos);
        CHECK(toText(*k.field("rows")).find("Cube\tlocation\t1,20") != std::string::npos);

        CHECK(run(s, "model.key_move", {{"object", str("Cube")}, {"frame", std::int64_t{20}}, {"to", std::int64_t{30}}, {"row", str("location")}}).ok);
        auto d = docIn(s);
        const auto& lx = d->object("Cube")->keys.at("location.x");
        CHECK(lx.size() == 2 && lx.back().frame == 30 && near(lx.back().value, 4));
        CHECK(s.history().undo());
        CHECK(d->object("Cube")->keys.at("location.x").back().frame == 20);
        CHECK(s.history().redo());
        CHECK(!run(s, "model.key_move", {{"object", str("Cube")}, {"frame", std::int64_t{7}}, {"to", std::int64_t{8}}}).ok);

        CHECK(run(s, "model.key_move", {{"object", str("Cube")}, {"frame", std::int64_t{30}}, {"to", std::int64_t{40}}, {"copy", true}}).ok);
        CHECK(d->object("Cube")->keys.at("location.x").size() == 3);
        CHECK(run(s, "model.key_delete", {{"object", str("Cube")}, {"frame", std::int64_t{40}}}).ok);
        CHECK(d->object("Cube")->keys.at("location.x").size() == 2);
        CHECK(d->object("Cube")->keys.count("scale.x") == 1);  // other rows untouched

        // scrubbing moves the object but isn't an undo step; wrap loops the range
        const std::size_t steps = s.history().labels().size();
        Result sc = run(s, "model.scrub", {{"frame", std::int64_t{30}}});
        CHECK(sc.ok && d->scene.frame == 30 && near(d->object("Cube")->location.x, 4, 1e-9));
        CHECK(s.history().labels().size() == steps);
        CHECK(run(s, "model.frame", {{"start", std::int64_t{1}}, {"end", std::int64_t{10}}}).ok);
        CHECK(run(s, "model.scrub", {{"frame", std::int64_t{12}}, {"wrap", true}}).ok && d->scene.frame == 2);
    }

    // ---- drag: rings, scale boxes, snapping ----
    {
        Session s;
        registerCommands(s);
        CHECK(run(s, "model.new").ok);
        auto d = docIn(s);
        CHECK(run(s, "model.drag", {{"mode", str("rotate")}, {"axis", str("z")}, {"angle", 33.0}}).ok);
        CHECK(near(d->object("Cube")->rotation.z, 33, 1e-6));
        CHECK(run(s, "model.drag", {{"mode", str("rotate")}, {"axis", str("z")}, {"angle", 33.0}, {"snap", str("grid")}}).ok);
        CHECK(near(std::fmod(d->object("Cube")->rotation.z - 33 + 360, 5.0), 0, 1e-6) || near(std::fmod(d->object("Cube")->rotation.z - 33 + 360, 5.0), 5, 1e-6));
        CHECK(run(s, "model.drag", {{"mode", str("scale")}, {"factor", 1.23}, {"snap", str("grid")}}).ok);
        CHECK(near(d->object("Cube")->scale.x, 1.2, 1e-6) && near(d->object("Cube")->scale.z, 1.2, 1e-6));
        CHECK(run(s, "model.drag", {{"dx", 37.0}, {"axis", str("x")}, {"snap", str("grid")}, {"step", 0.5}}).ok);
        const double x = d->object("Cube")->location.x;
        CHECK(std::fabs(x) > 0.1 && near(std::fmod(std::fabs(x) + 1e-9, 0.5), 0, 1e-6));
    }

    // ---- viewport shading ----
    {
        Session s;
        registerCommands(s);
        CHECK(run(s, "model.new").ok);
        auto d = docIn(s);
        auto draw = [&](const std::string& shading) {
            OrbitView v;
            v.shading = shading;
            std::vector<std::uint8_t> rgba;
            CHECK(d->renderView(v, 96, 72, rgba) && rgba.size() == 96u * 72u * 4u);
            return rgba;
        };
        const auto material = draw(""), matcap = draw("matcap"), wire = draw("wireframe"), xray = draw("xray");
        CHECK(material != matcap && material != wire && wire != xray);
        Result pv = run(s, "model.info", {{"preview", true}, {"shading", str("xray")}});
        CHECK(pv.ok);
    }

    // ---- world (hdri) and texture maps in renders ----
    {
        Session s;
        registerCommands(s);
        CHECK(run(s, "model.new").ok);
        const fs::path sky = dir / "sky.png", bumps = dir / "bumps.png";
        writePng(sky, 64, 32, 255, 20, 20);
        writePng(bumps, 16, 16, 160, 128, 230);
        CHECK(!run(s, "model.world", {{"type", str("hdri")}}).ok);
        Result w = run(s, "model.world", {{"hdri", sky.string()}, {"strength", 1.0}});
        CHECK(w.ok && docIn(s)->scene.worldType == "hdri" && docIn(s)->scene.worldVisible);
        CHECK(run(s, "model.material", {{"name", str("Bumpy")}, {"normal_map", bumps.string()}, {"roughness_map", bumps.string()}, {"normal_strength", 0.5}}).ok);
        Result r = run(s, "model.render", {{"mode", str("preview")}, {"width", std::int64_t{64}}, {"height", std::int64_t{48}}});
        CHECK(r.ok && r.message.find("warning") == std::string::npos && r.attachments.size() == 1);
        if (r.ok && !r.attachments.empty()) {
            const raster::Image img = raster::decode(r.attachments[0].bytes);
            const std::uint8_t* corner = img.pixel(1, 1);
            CHECK(corner[0] > corner[1] + 60 && corner[0] > corner[2] + 60);  // the red panorama shows behind
        }
        // world and maps survive .wwmodel; OBJ export writes the maps into the .mtl
        const fs::path ww = dir / "world.wwmodel";
        CHECK(run(s, "model.save", {{"path", ww.string()}}).ok);
        Session s2;
        registerCommands(s2);
        CHECK(run(s2, "model.open", {{"path", ww.string()}}).ok);
        auto d2 = docIn(s2);
        CHECK(d2->scene.worldType == "hdri" && d2->scene.hdri == fs::absolute(sky).lexically_normal().string());
        const Material* m = d2->material("Bumpy");
        CHECK(m && m->normalMap == bumps.string() && near(m->normalStrength, 0.5) && m->roughnessMap == bumps.string());
        CHECK(run(s2, "model.export", {{"path", (dir / "bumpy.obj").string()}}).ok);
        const std::string mtl = readFile(dir / "bumpy.mtl");
        CHECK(mtl.find("norm ") != std::string::npos && mtl.find("map_Pr ") != std::string::npos);
    }

    // ---- the render tool: OBJ with several materials, parts, hdri sky ----
    {
        const fs::path obj = dir / "two.obj";
        {
            std::ofstream o(obj);
            o << "mtllib two.mtl\nv 0 0 0\nv 1 0 0\nv 1 1 0\nv 0 1 0\nv 0 0 1\nv 1 0 1\nv 1 1 1\nv 0 1 1\n"
                 "usemtl red\nf 1 2 6 5\nf 2 3 7 6\nf 5 6 7 8\nusemtl blue\nf 3 4 8 7\nf 4 1 5 8\nf 1 4 3 2\n";
            std::ofstream m(dir / "two.mtl");
            m << "newmtl red\nKd 0.9 0.1 0.1\nNs 900\nnewmtl blue\nKd 0.1 0.2 0.9\nPm 1\nPr 0.2\nnorm bumps.png\n";
        }
        std::vector<render::Material> parts;
        std::vector<int> partOf;
        const mesh::Mesh m = render::loadMeshFile(obj, parts, partOf);
        CHECK(m.triangleCount() == 12 && parts.size() == 2 && partOf.size() == 12);
        CHECK(parts.size() == 2 && parts[0].name == "red" && parts[1].name == "blue" && near(parts[1].metallic, 1) && near(parts[1].roughness, 0.2));
        CHECK(parts.size() == 2 && parts[0].color.x > parts[0].color.z && parts[1].normalMap == (dir / "bumps.png").lexically_normal().string());

        Session s;
        render::registerCommands(s);
        CHECK(run(s, "render.new").ok);
        Result add = run(s, "render.add", {{"doc", str("scene1")}, {"path", obj.string()}, {"name", str("box")}});
        CHECK(add.ok && add.message.find("2 materials") != std::string::npos);
        CHECK(run(s, "render.material", {{"doc", str("scene1")}, {"name", str("box")}, {"part", str("blue")}, {"roughness", 0.7}}).ok);
        CHECK(!run(s, "render.material", {{"doc", str("scene1")}, {"name", str("box")}, {"part", str("green")}}).ok);
        CHECK(run(s, "render.sky", {{"doc", str("scene1")}, {"hdri", (dir / "sky.png").string()}, {"rotation", 90.0}}).ok);
        Result img = run(s, "render.render", {{"doc", str("scene1")}, {"mode", str("preview")}, {"width", std::int64_t{48}}, {"height", std::int64_t{36}}});
        CHECK(img.ok && img.message.find("warning") == std::string::npos);
        const fs::path scene = dir / "two.scene.json";
        CHECK(run(s, "render.save", {{"doc", str("scene1")}, {"path", scene.string()}}).ok);
        const render::Scene back = render::loadScene(scene);
        const render::Object* box = back.findObject("box");
        CHECK(box && box->parts.size() == 2 && box->partOf && near(box->parts[1].roughness, 0.7));
        CHECK(back.sky.type == "hdri" && near(back.sky.rotation, 90));
    }

    fs::remove_all(dir);
    return check::summary("model rig");
}
