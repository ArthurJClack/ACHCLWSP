// Sculpt tests. Meshes stay small (a few thousand triangles at most) so the
// suite runs fast and in little memory.
#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <sstream>

#include <nlohmann/json.hpp>

#include "wetwaresuite/mcp/server.hpp"
#include "wetwaresuite/mesh/document.hpp"
#include "wetwaresuite/mesh/io.hpp"
#include "wetwaresuite/mesh/primitives.hpp"
#include "wetwaresuite/mesh/topology.hpp"
#include "wetwaresuite/raster/codec.hpp"
#include "wetwaresuite/sculpt/sculpt.hpp"
#include "check.hpp"

using namespace wetwaresuite;
using json = nlohmann::ordered_json;
namespace fs = std::filesystem;
using mesh::Vec3;

namespace {

std::string docOf(const Result& r) {
    const Value* v = r.field("doc");
    return v ? toText(*v) : "";
}

std::int64_t intField(const Result& r, const char* key) {
    const Value* v = r.field(key);
    return v && std::holds_alternative<std::int64_t>(*v) ? std::get<std::int64_t>(*v) : -1;
}

double numField(const Result& r, const char* key) {
    const Value* v = r.field(key);
    return v && std::holds_alternative<double>(*v) ? std::get<double>(*v) : -1e300;
}

NumberList listField(const Result& r, const char* key) {
    const Value* v = r.field(key);
    return v && std::holds_alternative<NumberList>(*v) ? std::get<NumberList>(*v) : NumberList{};
}

bool near(double a, double b, double eps) { return std::fabs(a - b) <= eps; }

bool finite(const mesh::Mesh& m) {
    for (const auto& p : m.positions) {
        if (!std::isfinite(p.x) || !std::isfinite(p.y) || !std::isfinite(p.z)) return false;
    }
    return true;
}

double maxZ(const mesh::Mesh& m) {
    double z = -1e300;
    for (const auto& p : m.positions) z = std::max(z, p.z);
    return z;
}

json call(mcp::Server& server, int id, const std::string& tool, const json& args) {
    json req = {{"jsonrpc", "2.0"}, {"id", id}, {"method", "tools/call"}, {"params", {{"name", tool}, {"arguments", args}}}};
    return json::parse(*server.handle(req.dump()))["result"];
}

std::shared_ptr<sculpt::SculptDoc> docIn(Session& s, const std::string& id) { return s.workspace().getAs<sculpt::SculptDoc>(id); }

}  // namespace

int main() {
    auto dir = fs::temp_directory_path() / "wetwaresuite_test_sculpt";
    fs::remove_all(dir);
    fs::create_directories(dir);

    // ---- libs/mesh topology helpers ----
    {
        mesh::Mesh cube = mesh::box({2, 2, 2});
        cube.normals.clear();
        mesh::weld(cube, 0);
        CHECK(cube.vertexCount() == 8);
        const auto nbr = mesh::vertexNeighbors(cube);
        CHECK(nbr.offsets.size() == cube.vertexCount() + 1);
        for (std::size_t v = 0; v < cube.vertexCount(); ++v) CHECK(nbr.count(v) >= 3);
        const auto vt = mesh::vertexTriangles(cube);
        CHECK(vt.items.size() == cube.indices.size());
        const auto n = mesh::vertexNormals(cube);
        for (std::size_t v = 0; v < cube.vertexCount(); ++v) CHECK(mesh::dot(n[v], mesh::normalize(cube.positions[v])) > 0.9);
        CHECK(mesh::length(mesh::closestPointOnTriangle({0.2, 0.2, 5}, {0, 0, 0}, {1, 0, 0}, {0, 1, 0}) - Vec3{0.2, 0.2, 0}) < 1e-12);
        CHECK(mesh::closestPointOnTriangle({-1, -1, 0}, {0, 0, 0}, {1, 0, 0}, {0, 1, 0}) == Vec3{0, 0, 0});
        CHECK(near(mesh::rayTriangle({0.2, 0.2, 5}, {0, 0, -1}, {0, 0, 0}, {1, 0, 0}, {0, 1, 0}), 5, 1e-12));
        CHECK(mesh::rayTriangle({2, 2, 5}, {0, 0, -1}, {0, 0, 0}, {1, 0, 0}, {0, 1, 0}) < 0);
        mesh::Mesh plane = mesh::plane(1, 1, 2, 2);
        const auto b = mesh::boundaryVertices(plane);
        std::size_t inner = 0;
        for (bool x : b) inner += x ? 0 : 1;
        CHECK(inner == 1);
    }

    // ---- primitives ----
    {
        using sculpt::Shape;
        mesh::Mesh sphere = sculpt::primitive(Shape::Sphere, 2, 2);
        CHECK(sphere.triangleCount() == 320 && sculpt::primitiveTriangles(Shape::Sphere, 2) == 320);
        CHECK(mesh::isWatertight(sphere) && mesh::volume(sphere) > 3.5 && mesh::volume(sphere) < 4.19);
        mesh::Mesh cube = sculpt::primitive(Shape::Cube, 2, 2);
        CHECK(cube.triangleCount() == 192 && mesh::isWatertight(cube) && near(mesh::volume(cube), 8, 1e-9));
        CHECK(cube.vertexCount() == 98);  // 6*25 - shared edges and corners
        mesh::Mesh plane = sculpt::primitive(Shape::Plane, 4, 2);
        CHECK(plane.triangleCount() == 32 && !mesh::isWatertight(plane) && plane.hasUvs());
        bool threw = false;
        try {
            sculpt::primitive(Shape::Sphere, 2, 9);  // 5.2 million triangles: refused before allocating
        } catch (const std::invalid_argument& e) {
            threw = std::string(e.what()).find("limit") != std::string::npos;
        }
        CHECK(threw);
        CHECK(sculpt::parseSymmetry("xz") == 5 && sculpt::parseSymmetry("none") == 0 && sculpt::parseSymmetry("q") < 0);
        CHECK(sculpt::falloffWeight(sculpt::Falloff::Smooth, 0) == 1 && sculpt::falloffWeight(sculpt::Falloff::Smooth, 1) == 0);
    }

    Session s;
    sculpt::registerCommands(s);
    for (const auto& info : s.registry().list()) CHECK(info.id.rfind("sculpt.", 0) == 0);

    // ---- new, stats, errors ----
    Result n = s.run("sculpt.new", {{"shape", std::string("sphere")}, {"level", std::int64_t{3}}, {"size", 2.0}});
    CHECK(n.ok && docOf(n) == "sculpt1" && intField(n, "triangles") == 1280);
    CHECK(!s.run("sculpt.new", {{"level", std::int64_t{10}}}).ok);
    CHECK(!s.run("sculpt.new", {{"shape", std::string("donut")}}).ok);
    {
        Result st = s.run("sculpt.stats", {{"doc", std::string("sculpt1")}});
        CHECK(st.ok && intField(st, "edges") == 1920 && intField(st, "open_edges") == 0 && intField(st, "euler") == 2);
        CHECK(st.field("volume") != nullptr && numField(st, "edge_mean") > 0);
    }
    CHECK(s.history().labels().empty());  // new and stats are not undo steps

    // ---- strokes cast from the view (space=ray) and pen pressure ----
    {
        Session ps;
        sculpt::registerCommands(ps);
        auto rise = [&](const NumberList& pressures) {
            Result nn = ps.run("sculpt.new", {{"shape", std::string("sphere")}, {"level", std::int64_t{3}}, {"size", 2.0}});
            auto d = docIn(ps, docOf(nn));
            const double top = maxZ(d->mesh);
            Params p{{"doc", docOf(nn)}, {"brush", std::string("draw")}, {"space", std::string("ray")},
                     {"points", NumberList{-0.2, 0, 5, 0, 0, -1, 0.2, 0, 5, 0, 0, -1}}, {"radius", 0.5}, {"strength", 1.0}};
            if (!pressures.empty()) p["pressures"] = pressures;
            Result r = ps.run("sculpt.stroke", p);
            CHECK(r.ok && finite(d->mesh));
            return maxZ(d->mesh) - top;
        };
        const double full = rise({});
        const double light = rise({0.2, 0.2});
        CHECK(full > 0.03);
        CHECK(light > 0 && light < full * 0.6);
        // a ray that misses the model is an error, not a silent no-op
        Result miss = ps.run("sculpt.stroke", {{"doc", std::string("sculpt1")}, {"space", std::string("ray")}, {"points", NumberList{5, 5, 5, 0, 0, 1}}});
        CHECK(!miss.ok || intField(miss, "moved") <= 0);
    }

    // ---- strokes ----
    {
        auto doc = docIn(s, "sculpt1");
        const double top = maxZ(doc->mesh);
        Result r = s.run("sculpt.stroke", {{"doc", std::string("sculpt1")}, {"brush", std::string("draw")}, {"points", NumberList{-0.3, 0, 1, 0.3, 0, 1}},
                                          {"radius", 0.5}, {"strength", 1.0}});
        CHECK(r.ok && intField(r, "moved") > 10 && intField(r, "dabs") >= 3);
        CHECK(maxZ(doc->mesh) > top + 0.05 && finite(doc->mesh) && mesh::isWatertight(doc->mesh));
        // Undo puts every vertex back.
        CHECK(s.history().undo());
        CHECK(near(maxZ(doc->mesh), top, 1e-12));
        CHECK(s.history().redo());
        CHECK(maxZ(doc->mesh) > top + 0.05);

        // Invert digs in.
        const double before = maxZ(doc->mesh);
        CHECK(s.run("sculpt.stroke", {{"doc", std::string("sculpt1")}, {"points", NumberList{0, 0, 1.2}}, {"radius", 0.6}, {"strength", 1.0}, {"invert", true}}).ok);
        CHECK(maxZ(doc->mesh) < before);

        // Every brush moves something and keeps the mesh finite.
        for (const char* b : {"inflate", "smooth", "flatten", "pinch", "crease", "grab"}) {
            Result br = s.run("sculpt.stroke", {{"doc", std::string("sculpt1")}, {"brush", std::string(b)}, {"points", NumberList{0.9, 0, 0.3, 0.9, 0.3, 0.3}},
                                               {"radius", 0.4}, {"falloff", std::string("linear")}});
            CHECK(br.ok && intField(br, "moved") > 0);
            CHECK(finite(doc->mesh));
        }
        // Grab drags the picked-up vertices by the path's offset.
        mesh::Mesh pre = doc->mesh;
        CHECK(s.run("sculpt.stroke", {{"doc", std::string("sculpt1")}, {"brush", std::string("grab")}, {"points", NumberList{0, -1, 0, 0, -1.5, 0}},
                                     {"radius", 0.3}, {"falloff", std::string("constant")}})
                  .ok);
        double most = 0;
        for (std::size_t v = 0; v < pre.vertexCount(); ++v) most = std::max(most, mesh::length(doc->mesh.positions[v] - pre.positions[v]));
        CHECK(near(most, 0.5, 1e-9));

        // Symmetry: a stroke on +x also shapes -x.
        Result sym = s.run("sculpt.new", {{"level", std::int64_t{3}}});
        const std::string id = docOf(sym);
        auto sd = docIn(s, id);
        CHECK(s.run("sculpt.stroke", {{"doc", id}, {"points", NumberList{1, 0, 0}}, {"radius", 0.5}, {"strength", 1.0}, {"symmetry", std::string("x")}}).ok);
        double minX = 0, maxX = 0;
        for (const auto& p : sd->mesh.positions) {
            minX = std::min(minX, p.x);
            maxX = std::max(maxX, p.x);
        }
        CHECK(maxX > 1.03 && near(minX, -maxX, 1e-9));

        // Screen space: the middle of a front view is the sphere's front (-Y) side.
        Result sp = s.run("sculpt.surface_point", {{"doc", id}, {"u", 0.5}, {"v", 0.5}, {"view", std::string("front")}});
        CHECK(sp.ok && listField(sp, "point").size() == 3 && near(listField(sp, "point")[1], -1, 0.06));
        CHECK(!s.run("sculpt.surface_point", {{"doc", id}, {"u", 0.01}, {"v", 0.01}, {"view", std::string("front")}}).ok);
        Result yawed = s.run("sculpt.surface_point", {{"doc", id}, {"u", 0.5}, {"v", 0.5}, {"view", std::string("front")}, {"yaw", 90.0}});
        CHECK(yawed.ok && near(std::fabs(listField(yawed, "point")[0]), 1.03, 0.1));  // turned 90: the bumped +x/-x side faces the camera
        Result ss = s.run("sculpt.stroke", {{"doc", id}, {"space", std::string("screen")}, {"view", std::string("top")},
                                           {"points", NumberList{0.3, 0.5, 0.7, 0.5}}, {"radius", 0.3}, {"preview", true}});
        CHECK(ss.ok && ss.attachments.size() == 1 && ss.attachments[0].mime == "image/png");
        CHECK(listField(ss, "start").size() == 3 && listField(ss, "start")[2] > 0.5);  // seen from the top, so on the upper half
        CHECK(!s.run("sculpt.stroke", {{"doc", id}, {"space", std::string("screen")}, {"points", NumberList{0.5}}}).ok);
        CHECK(!s.run("sculpt.stroke", {{"doc", id}, {"points", NumberList{0, 0}}}).ok);
        CHECK(!s.run("sculpt.stroke", {{"doc", id}, {"points", NumberList{0, 0, 5, 0, 0, 1}}, {"spacing", 0.0}}).ok);

        // Masks protect.
        CHECK(s.run("sculpt.mask", {{"doc", id}, {"shape", std::string("all")}}).ok);
        CHECK(!s.run("sculpt.stroke", {{"doc", id}, {"points", NumberList{0, 0, 1}}}).ok);
        Result mi = s.run("sculpt.mask", {{"doc", id}, {"shape", std::string("invert")}});
        CHECK(mi.ok && intField(mi, "masked") == 0);
        Result ms = s.run("sculpt.mask", {{"doc", id}, {"shape", std::string("sphere")}, {"center", NumberList{0, 0, 1}}, {"radius", 0.5}});
        CHECK(ms.ok && intField(ms, "masked") > 5);
        const auto masked = intField(ms, "masked");
        Result grown = s.run("sculpt.mask", {{"doc", id}, {"shape", std::string("grow")}, {"steps", std::int64_t{2}}});
        CHECK(grown.ok && intField(grown, "masked") > masked);
        CHECK(s.history().undo());
        CHECK(intField(s.run("sculpt.stats", {{"doc", id}}), "masked") == masked);
        const mesh::Mesh beforeMasked = sd->mesh;
        CHECK(s.run("sculpt.stroke", {{"doc", id}, {"points", NumberList{0, 0, 1}}, {"radius", 0.3}, {"falloff", std::string("constant")}}).ok == false);
        CHECK(s.run("sculpt.smooth", {{"doc", id}, {"iterations", std::int64_t{3}}}).ok);
        for (std::size_t v = 0; v < sd->mesh.vertexCount(); ++v) {
            if (sd->mask[v] >= 1) CHECK(sd->mesh.positions[v] == beforeMasked.positions[v]);
        }
        Result mp = s.run("sculpt.mask", {{"doc", id}, {"shape", std::string("plane")}, {"point", NumberList{0, 0, 0}}, {"normal", NumberList{0, 0, 1}}});
        CHECK(mp.ok && intField(mp, "masked") > 200 && intField(mp, "masked") < 500);
        CHECK(s.run("sculpt.mask", {{"doc", id}, {"shape", std::string("clear")}}).ok);
        CHECK(!s.run("sculpt.mask", {{"doc", id}, {"shape", std::string("sphere")}}).ok);
    }

    // ---- smoothing ----
    {
        Result r = s.run("sculpt.new", {{"shape", std::string("cube")}, {"level", std::int64_t{2}}});
        const std::string id = docOf(r);
        auto doc = docIn(s, id);
        const double v0 = mesh::volume(doc->mesh);
        CHECK(s.run("sculpt.smooth", {{"doc", id}, {"iterations", std::int64_t{10}}, {"method", std::string("laplacian")}}).ok);
        const double vl = mesh::volume(doc->mesh);
        CHECK(vl < v0);
        CHECK(s.history().undo());
        CHECK(s.run("sculpt.smooth", {{"doc", id}, {"iterations", std::int64_t{10}}}).ok);
        CHECK(mesh::volume(doc->mesh) > vl);  // taubin shrinks less
        CHECK(!s.run("sculpt.smooth", {{"doc", id}, {"strength", 2.0}}).ok);
    }

    // ---- subdivide ----
    {
        Result r = s.run("sculpt.new", {{"level", std::int64_t{1}}});
        const std::string id = docOf(r);
        auto doc = docIn(s, id);
        Result sub = s.run("sculpt.subdivide", {{"doc", id}, {"levels", std::int64_t{2}}});
        CHECK(sub.ok && intField(sub, "triangles") == 80 * 16 && mesh::isWatertight(doc->mesh));
        Result mid = s.run("sculpt.subdivide", {{"doc", id}, {"method", std::string("midpoint")}});
        CHECK(mid.ok && intField(mid, "triangles") == 80 * 64);
        Result big = s.run("sculpt.subdivide", {{"doc", id}, {"levels", std::int64_t{4}}});  // 1.3 million: refused up front
        CHECK(!big.ok && big.message.find("limit") != std::string::npos);
        CHECK(doc->mesh.triangleCount() == 80 * 64);
        // Mask is carried over.
        mesh::Mesh small = sculpt::primitive(sculpt::Shape::Sphere, 2, 0);
        sculpt::Mask mask(small.vertexCount(), 1.f);
        mesh::Mesh sm = sculpt::subdivide(small, true, &mask);
        CHECK(mask.size() == sm.vertexCount() && mask.back() == 1.f);
        // Loop keeps an open plane's border on its edges.
        mesh::Mesh pl = sculpt::primitive(sculpt::Shape::Plane, 2, 1);
        mesh::Mesh pls = sculpt::subdivide(pl, true);
        CHECK(near(mesh::bounds(pls).size().x, 2, 1e-12) && mesh::boundaryEdgeCount(pls) == 2 * mesh::boundaryEdgeCount(pl));
    }

    // ---- remesh ----
    {
        mesh::Mesh cube = mesh::box({2, 2, 2});
        bool shell = true;
        mesh::Mesh rm = sculpt::voxelRemesh(cube, 16, &shell);
        CHECK(!shell && rm.triangleCount() % 2 == 0 && rm.triangleCount() > 500);
        CHECK(mesh::isWatertight(rm) && near(mesh::volume(rm), 8, 0.8));
        // Overlapping solids merge into one closed surface.
        mesh::Mesh two = mesh::box({2, 2, 2});
        mesh::Mesh other = mesh::box({2, 2, 2});
        mesh::transform(other, mesh::Mat4::translate({1, 0, 0}));
        mesh::append(two, other);
        mesh::Mesh merged = sculpt::voxelRemesh(two, 24);
        CHECK(mesh::isWatertight(merged) && near(mesh::volume(merged), 12, 1.5));
        // Open meshes become a shell.
        mesh::Mesh sheet = sculpt::voxelRemesh(sculpt::primitive(sculpt::Shape::Plane, 2, 1), 16, &shell);
        CHECK(shell && !sheet.empty());
        bool threw = false;
        try {
            sculpt::voxelRemesh(cube, 1000);
        } catch (const std::length_error& e) {
            threw = std::string(e.what()).find("limit") != std::string::npos;
        }
        CHECK(threw);

        Result r = s.run("sculpt.new", {{"level", std::int64_t{2}}});
        const std::string id = docOf(r);
        Result re = s.run("sculpt.remesh", {{"doc", id}, {"resolution", std::int64_t{20}}});
        CHECK(re.ok && intField(re, "quads") > 100 && intField(re, "quads") * 2 == intField(re, "triangles"));
        CHECK(docIn(s, id)->quads);
        CHECK(!s.run("sculpt.remesh", {{"doc", id}, {"resolution", std::int64_t{500}}}).ok);
        CHECK(!s.run("sculpt.remesh", {{"doc", id}, {"voxel_size", 0.001}}).ok);
        CHECK(s.run("sculpt.remesh", {{"doc", id}, {"voxel_size", 0.25}}).ok);
        CHECK(s.history().undo() && s.history().undo());
        CHECK(docIn(s, id)->mesh.triangleCount() == 320 && !docIn(s, id)->quads);
    }

    // ---- decimate ----
    {
        mesh::Mesh sphere = sculpt::primitive(sculpt::Shape::Sphere, 2, 3);
        mesh::Mesh d = sculpt::decimateQuadric(sphere, 300);
        CHECK(d.triangleCount() <= 302 && d.triangleCount() >= 250);
        CHECK(mesh::isWatertight(d) && near(mesh::volume(d), mesh::volume(sphere), 0.25));
        // An open plane keeps its outline.
        mesh::Mesh pl = sculpt::primitive(sculpt::Shape::Plane, 2, 4);
        mesh::Mesh pd = sculpt::decimateQuadric(pl, 40);
        CHECK(pd.triangleCount() < 200 && near(mesh::bounds(pd).size().x, 2, 1e-9) && near(mesh::bounds(pd).size().y, 2, 1e-9));
        CHECK(near(mesh::surfaceArea(pd), 4, 1e-6));
        // Locked vertices survive.
        sculpt::Mask lock(sphere.vertexCount(), 0.f);
        std::size_t locked = 0;
        for (std::size_t v = 0; v < sphere.vertexCount(); ++v) {
            if (sphere.positions[v].z > 0.8) { lock[v] = 1.f; ++locked; }
        }
        mesh::Mesh ld = sculpt::decimateQuadric(sphere, 300, &lock);
        std::size_t kept = 0;
        for (float m : lock) kept += m >= 0.5f ? 1 : 0;
        CHECK(lock.size() == ld.vertexCount() && kept == locked);

        Result r = s.run("sculpt.new", {{"level", std::int64_t{3}}});
        const std::string id = docOf(r);
        Result dr = s.run("sculpt.decimate", {{"doc", id}, {"ratio", 0.25}});
        CHECK(dr.ok && intField(dr, "triangles") <= 322 && intField(dr, "before") == 1280);
        CHECK(!s.run("sculpt.decimate", {{"doc", id}}).ok);
        CHECK(!s.run("sculpt.decimate", {{"doc", id}, {"ratio", 2.0}}).ok);
    }

    // ---- retopo ----
    {
        Result r = s.run("sculpt.new", {{"level", std::int64_t{3}}});
        const std::string id = docOf(r);
        Result rt = s.run("sculpt.retopo", {{"doc", id}, {"quads", std::int64_t{400}}});
        CHECK(rt.ok && intField(rt, "quads") > 250 && intField(rt, "quads") < 700);
        auto doc = docIn(s, id);
        CHECK(doc->quads && mesh::isWatertight(doc->mesh));
        double worst = 0;
        for (const auto& p : doc->mesh.positions) worst = std::max(worst, std::fabs(mesh::length(p) - 1));
        CHECK(worst < 0.03);  // projected back onto the sphere
        // OBJ export keeps quads.
        const fs::path obj = dir / "retopo.obj";
        CHECK(s.run("sculpt.export", {{"doc", id}, {"path", obj.string()}}).ok);
        std::ifstream f(obj);
        std::string line;
        std::size_t quads = 0;
        while (std::getline(f, line)) {
            if (line.rfind("f ", 0) == 0) {
                std::istringstream ls(line.substr(2));
                int k = 0;
                std::string tok;
                while (ls >> tok) ++k;
                quads += k == 4 ? 1 : 0;
            }
        }
        CHECK(static_cast<std::int64_t>(quads) == intField(rt, "quads"));
        mesh::Mesh back = mesh::load(obj);
        CHECK(back.triangleCount() == doc->mesh.triangleCount() && mesh::isWatertight(back));
    }

    // ---- symmetrize ----
    {
        Result r = s.run("sculpt.new", {{"level", std::int64_t{3}}});
        const std::string id = docOf(r);
        CHECK(s.run("sculpt.stroke", {{"doc", id}, {"points", NumberList{0.7, 0.3, 0.6}}, {"radius", 0.5}, {"strength", 1.0}}).ok);
        Result sy = s.run("sculpt.symmetrize", {{"doc", id}, {"axis", std::string("x")}});
        CHECK(sy.ok);
        auto doc = docIn(s, id);
        CHECK(mesh::isWatertight(doc->mesh));
        const mesh::Bounds b = mesh::bounds(doc->mesh);
        CHECK(near(b.min.x, -b.max.x, 1e-9));
        std::size_t paired = 0;
        for (const auto& p : doc->mesh.positions) {
            for (const auto& q : doc->mesh.positions) {
                if (mesh::length(q - Vec3{-p.x, p.y, p.z}) < 1e-9) { ++paired; break; }
            }
        }
        CHECK(paired == doc->mesh.vertexCount());
        // A box offset to one side mirrors to the other side.
        mesh::Mesh box = mesh::box({2, 2, 2});
        mesh::transform(box, mesh::Mat4::translate({0.5, 0, 0}));
        mesh::Mesh sb = sculpt::symmetrize(box, 0, true);
        CHECK(mesh::isWatertight(sb) && near(mesh::volume(sb), 12, 1e-9));
        bool threw = false;
        try {
            mesh::Mesh right = mesh::box({1, 1, 1}, false);
            mesh::transform(right, mesh::Mat4::translate({1, 0, 0}));
            sculpt::symmetrize(right, 0, false);
        } catch (const std::invalid_argument&) {
            threw = true;
        }
        CHECK(threw);
    }

    // ---- displace ----
    {
        raster::Image grad(32, 32);
        for (int y = 0; y < 32; ++y)
            for (int x = 0; x < 32; ++x) {
                const auto g = static_cast<std::uint8_t>(x * 255 / 31);
                grad.set(x, y, {g, g, g, 255});
            }
        const fs::path hm = dir / "height.png";
        raster::save(grad, hm);
        Result r = s.run("sculpt.new", {{"shape", std::string("plane")}, {"level", std::int64_t{4}}, {"size", 2.0}});
        const std::string id = docOf(r);
        Result d = s.run("sculpt.displace", {{"doc", id}, {"image", hm.string()}, {"amount", 0.4}, {"midlevel", 0.0}});
        CHECK(d.ok && near(numField(d, "max_offset"), 0.4, 1e-6));
        auto doc = docIn(s, id);
        double zl = 0, zr = 0;
        for (const auto& p : doc->mesh.positions) {
            if (p.x < -0.99) zl = p.z;
            if (p.x > 0.99) zr = p.z;
        }
        CHECK(near(zl, 0, 1e-6) && near(zr, 0.4, 1e-6));
        CHECK(!s.run("sculpt.displace", {{"doc", id}, {"image", (dir / "missing.png").string()}}).ok);
        Result sph = s.run("sculpt.new", {{"level", std::int64_t{2}}});
        CHECK(s.run("sculpt.displace", {{"doc", docOf(sph)}, {"image", hm.string()}, {"mapping", std::string("spherical")}}).ok);
        CHECK(!s.run("sculpt.displace", {{"doc", docOf(sph)}, {"image", hm.string()}, {"mapping", std::string("uv")}}).ok);
    }

    // ---- export, open, copy, interop ----
    {
        Result r = s.run("sculpt.new", {{"level", std::int64_t{2}}});
        const std::string id = docOf(r);
        for (const char* ext : {"stl", "obj", "ply", "glb", "gltf"}) {
            const fs::path p = dir / (std::string("ball.") + ext);
            Result e = s.run("sculpt.export", {{"doc", id}, {"path", p.string()}});
            CHECK(e.ok && fs::file_size(p) > 100);
            Result o = s.run("sculpt.open", {{"path", p.string()}});
            CHECK(o.ok && intField(o, "triangles") == 320);
            auto od = docIn(s, docOf(o));
            CHECK(od && od->mesh.vertexCount() == 162 && !od->mesh.hasNormals());  // welded again on open
        }
        // File mode: input= and output= without opening anything.
        const fs::path rmObj = dir / "ball_remeshed.obj";
        Result fm = s.run("sculpt.remesh", {{"input", (dir / "ball.stl").string()}, {"output", rmObj.string()}, {"resolution", std::int64_t{12}}});
        CHECK(fm.ok && fs::exists(rmObj) && intField(fm, "quads") > 50);
        {
            std::ifstream f(rmObj);
            std::string all((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
            CHECK(all.find("\nf ") != std::string::npos);
        }
        CHECK(!s.run("sculpt.smooth", {}).ok);
        CHECK(!s.run("sculpt.smooth", {{"input", (dir / "missing.stl").string()}}).ok);
        const fs::path png = dir / "ball.png";
        Result pr = s.run("sculpt.export", {{"doc", id}, {"path", png.string()}, {"size", std::int64_t{64}}, {"yaw", 30.0}});
        CHECK(pr.ok && raster::load(png).width() == 64);
        CHECK(!s.run("sculpt.export", {{"doc", id}, {"path", (dir / "ball.xyz").string()}}).ok);
        CHECK(!s.run("sculpt.open", {{"path", (dir / "nothing.stl").string()}}).ok);

        // A plain mesh document (what forge.mesh_open and cad make) works with sculpt commands, and copies into a sculpt.
        auto plain = mesh::newMesh(mesh::unweld(mesh::box({2, 2, 2})));
        const std::string mid = s.workspace().add(plain, "mesh");
        Result st = s.run("sculpt.stroke", {{"doc", mid}, {"points", NumberList{0, 0, 1}}, {"radius", 2.0}});
        CHECK(st.ok && plain->mesh.vertexCount() == 8);  // welded first, so the stroke doesn't tear it
        CHECK(!s.run("sculpt.mask", {{"doc", mid}, {"shape", std::string("all")}}).ok);
        Result cp = s.run("sculpt.copy", {{"source", mid}});
        CHECK(cp.ok && docIn(s, docOf(cp)) != nullptr);
        CHECK(s.run("sculpt.mask", {{"doc", docOf(cp)}, {"shape", std::string("all")}}).ok);
        CHECK(!s.run("sculpt.copy", {{"source", std::string("nope")}}).ok);
        Result pv = s.run("sculpt.preview", {{"doc", id}, {"view", std::string("front")}, {"pitch", 20.0}, {"size", std::int64_t{96}}});
        CHECK(pv.ok && pv.attachments.size() == 1 && raster::decode(pv.attachments[0].bytes).width() == 96);
        CHECK(s.run("sculpt.close", {{"doc", id}}).ok);
        CHECK(!s.run("sculpt.stats", {{"doc", id}}).ok);
    }

    // ---- MCP ----
    {
        Session ms;
        sculpt::registerCommands(ms);
        mcp::Server server(ms, {"wetwaresuite", "test", "sculpt"});
        json list = json::parse(*server.handle(R"({"jsonrpc":"2.0","id":1,"method":"tools/list"})"))["result"]["tools"];
        bool sawStroke = false;
        for (const auto& t : list) {
            if (t["name"] == "sculpt_preview" || t["name"] == "sculpt_stats" || t["name"] == "sculpt_new") CHECK(t["annotations"]["readOnlyHint"] == true);
            if (t["name"] == "sculpt_export" || t["name"] == "sculpt_stroke") CHECK(t["annotations"]["readOnlyHint"] == false);
            if (t["name"] == "sculpt_stroke") {
                sawStroke = true;
                CHECK(t["inputSchema"]["properties"]["brush"]["enum"].size() == 7);
            }
        }
        CHECK(sawStroke && list.size() >= 16);
        json o = call(server, 2, "sculpt_new", {{"level", 2}, {"preview", true}});
        CHECK(o["isError"] == false && o["structuredContent"]["doc"] == "sculpt1" && o["content"].size() == 2 && o["content"][1]["type"] == "image");
        json st = call(server, 3, "sculpt_stroke", {{"doc", "sculpt1"}, {"brush", "inflate"}, {"space", "screen"}, {"view", "front"}, {"points", {0.4, 0.4, 0.6, 0.4}}, {"preview", true}});
        CHECK(st["isError"] == false && st["content"].size() == 2);
        json bad = call(server, 4, "sculpt_remesh", {{"doc", "sculpt1"}, {"resolution", 4000}});
        CHECK(bad["isError"] == true);
        CHECK(call(server, 5, "undo", json::object())["isError"] == false);
    }

    fs::remove_all(dir);
    return check::summary("sculpt");
}
