// forge.mesh_*: game-ready meshes on libs/mesh documents.
#include <algorithm>
#include <cmath>
#include <cstdio>

#include "wetwaresuite/forge/forge.hpp"
#include "wetwaresuite/forge/meshops.hpp"
#include "wetwaresuite/mesh/document.hpp"
#include "wetwaresuite/mesh/ops.hpp"
#include "wetwaresuite/raster/codec.hpp"
#include "common.hpp"

namespace wetwaresuite::forge::detail {

using mesh::Mesh;
using mesh::MeshDoc;
using mesh::Vec3;

namespace {

const std::vector<std::string> kRenderExts = {"png", "jpg", "jpeg", "bmp", "tga", "ppm"};

bool isRender(const fs::path& p) {
    const auto e = lowerExt(p);
    return std::find(kRenderExts.begin(), kRenderExts.end(), e) != kRenderExts.end();
}

std::string num(double v) {
    char buf[32];
    std::snprintf(buf, sizeof buf, "%.4g", v);
    return buf;
}

NumberList vec(const Vec3& v) { return {v.x, v.y, v.z}; }

Result failed(const std::string& message) { return Result::failure(message); }

ParamSpec axesParam() {
    return {"axes", ParamType::String,
            "up-axis conversion: y_up_to_z_up for Y-up files (most OBJ/FBX exports) before saving glb, z_up_to_y_up "
            "before saving OBJ/STL for a Y-up engine",
            false, {"none", "y_up_to_z_up", "z_up_to_y_up"}, std::string("none")};
}

ParamSpec normalsParam(bool fallback) {
    return {"normals", ParamType::Bool, "recompute normals afterwards (decimation drops them)", false, {}, fallback};
}

ParamSpec creaseParam() {
    return {"crease", ParamType::Number,
            "edges sharper than this many degrees stay hard; 180 = smooth everywhere, 0 = flat facets", false, {}, 60.0};
}

// Writes a mesh, or a render of it when the extension is an image format.
Result writeMesh(const Mesh& m, const fs::path& path, const Params& p, const mesh::RenderOptions& base) {
    makeParent(path);
    if (isRender(path)) {
        mesh::RenderOptions ro = base;
        if (auto v = mesh::parseView(stringParam(p, "view", "iso"))) ro.view = *v;
        ro.outline = boolParam(p, "outline", true);
        int w = static_cast<int>(intParam(p, "width", 1024)), h = static_cast<int>(intParam(p, "height", 0));
        if (h <= 0) h = w;
        if (w < 16 || h < 16 || w > 8192 || h > 8192) return failed("renders are 16..8192 pixels a side");
        raster::save(mesh::render(m, w, h, ro), path);
    } else {
        if (!mesh::canWrite(path)) {
            return failed("can't save ." + lowerExt(path) + "; use one of: " + joinList(mesh::writableExtensions()) + " (or png/jpg for a render)");
        }
        mesh::SaveOptions so;
        so.ascii = boolParam(p, "ascii", false);
        mesh::save(path, m, so);
    }
    std::error_code ec;
    return Result::success("saved " + path.string())
        .with("path", path.string())
        .with("bytes", static_cast<std::int64_t>(fs::file_size(path, ec)));
}

std::string info(const Mesh& m) {
    std::string s = mesh::describe(m);
    s += std::string("\nnormals: ") + (m.hasNormals() ? "yes" : "no") + ", uvs: " + (m.hasUvs() ? "yes" : "no");
    if (!m.empty() && !mesh::isWatertight(m)) {
        const auto open = mesh::boundaryEdgeCount(m);
        s += open ? ", " + std::to_string(open) + " open edges (holes)" : std::string(", no holes but some edges are shared by more than two triangles or wound inconsistently");
    }
    return s;
}

Result& infoFields(Result& r, const Mesh& m) {
    const mesh::Bounds b = mesh::bounds(m);
    const bool closed = !m.empty() && mesh::isWatertight(m);
    r.with("triangles", static_cast<std::int64_t>(m.triangleCount()))
        .with("vertices", static_cast<std::int64_t>(m.vertexCount()))
        .with("size", vec(b.size()))
        .with("min", b.empty() ? NumberList{0, 0, 0} : vec(b.min))
        .with("max", b.empty() ? NumberList{0, 0, 0} : vec(b.max))
        .with("watertight", closed)
        .with("open_edges", static_cast<std::int64_t>(m.empty() ? 0 : mesh::boundaryEdgeCount(m)))
        .with("normals", m.hasNormals())
        .with("uvs", m.hasUvs())
        .with("area", mesh::surfaceArea(m));
    if (closed) r.with("volume", mesh::volume(m));
    return r;
}

Projection parseProjection(const std::string& name) {
    return name == "planar" ? Projection::Planar
           : name == "cylindrical" ? Projection::Cylindrical
           : name == "spherical"   ? Projection::Spherical
                                   : Projection::Box;
}

int parseAxis(const std::string& name) { return name == "x" ? 0 : name == "y" ? 1 : 2; }

// "uv" / "uv_axis" on commands that decimate: decimation drops UVs, and the
// same projection gives every LOD the same mapping.
std::vector<ParamSpec> uvParams() {
    return {{"uv", ParamType::String, "project new UVs on each result (decimation drops UVs)", false,
             {"none", "box", "planar", "cylindrical", "spherical"}, std::string("none")},
            {"uv_axis", ParamType::String, "axis for the uv projection", false, {"x", "y", "z"}, std::string("z")}};
}

void applyUvParam(Mesh& m, const Params& p) {
    const std::string uv = stringParam(p, "uv", "none");
    if (uv != "none" && !m.empty()) generateUvs(m, parseProjection(uv), parseAxis(stringParam(p, "uv_axis", "z")));
}

std::vector<ParamSpec> concat(std::vector<ParamSpec> a, const std::vector<ParamSpec>& b) {
    a.insert(a.end(), b.begin(), b.end());
    return a;
}

std::vector<fs::path> meshFiles(const fs::path& dir, bool recursive, const fs::path& skipDir) {
    std::vector<fs::path> files;
    std::error_code ec;
    const fs::path skip = fs::weakly_canonical(skipDir, ec);
    const fs::path inAbs = fs::weakly_canonical(dir, ec);
    auto consider = [&](const fs::directory_entry& e) {
        if (!e.is_regular_file() || !mesh::canRead(e.path())) return;
        if (skip != inAbs) {
            auto rel = fs::weakly_canonical(e.path(), ec).lexically_relative(skip);
            if (!rel.empty() && *rel.begin() != "..") return;  // results inside the input folder are not sources
        }
        files.push_back(e.path());
    };
    if (recursive) {
        for (const auto& e : fs::recursive_directory_iterator(dir, ec)) consider(e);
    } else {
        for (const auto& e : fs::directory_iterator(dir, ec)) consider(e);
    }
    std::sort(files.begin(), files.end());
    return files;
}

// Shared by mesh_transform and mesh_batch. Order: axes, scale, rotate, center, floor, move.
std::string applyTransform(Mesh& m, const Params& p) {
    auto axes = parseAxes(stringParam(p, "axes", "none"));
    if (!axes) return "unknown axes";
    convertAxes(m, *axes);
    NumberList scale = listParam(p, "scale");
    if (p.count("scale") && std::holds_alternative<double>(p.at("scale"))) scale = {numberParam(p, "scale")};
    if (!scale.empty()) {
        if (scale.size() != 1 && scale.size() != 3) return "scale takes 1 number (uniform) or 3 (x,y,z)";
        Vec3 s = scale.size() == 1 ? Vec3{scale[0], scale[0], scale[0]} : Vec3{scale[0], scale[1], scale[2]};
        if (s.x == 0 || s.y == 0 || s.z == 0) return "scale can't be 0";
        mesh::transform(m, mesh::Mat4::scale(s));
    }
    NumberList rot = listParam(p, "rotate");
    if (!rot.empty()) {
        if (rot.size() != 3) return "rotate takes 3 angles in degrees (x,y,z)";
        mesh::transform(m, mesh::Mat4::rotate({rot[0], rot[1], rot[2]}));
    }
    if (boolParam(p, "center", false)) {
        const mesh::Bounds b = mesh::bounds(m);
        if (!b.empty()) mesh::transform(m, mesh::Mat4::translate(-b.center()));
    }
    if (boolParam(p, "floor", false)) {
        const mesh::Bounds b = mesh::bounds(m);
        if (!b.empty()) mesh::transform(m, mesh::Mat4::translate({0, 0, -b.min.z}));
    }
    NumberList move = listParam(p, "move");
    if (!move.empty()) {
        if (move.size() != 3) return "move takes 3 numbers (x,y,z)";
        mesh::transform(m, mesh::Mat4::translate({move[0], move[1], move[2]}));
    }
    return {};
}

}  // namespace

void registerMeshCommands(Session& session) {
    auto& reg = session.registry();
    Session* s = &session;

    reg.add({"forge.mesh_open",
             "Open mesh",
             "Open a 3D model as a mesh document: " + joinList(mesh::readableExtensions()) +
                 ". Returns its id (mesh1) and what's in it. glTF is converted from Y-up to this suite's Z-up; other "
                 "formats are read as they are (use forge.mesh_transform axes=y_up_to_z_up for Y-up OBJ/STL files).",
             {{"path", ParamType::String, "model file to open", true}},
             false, true},
            [s](const Params& p) {
                return std::make_unique<FunctionAction>([s, path = stringParam(p, "path")] {
                    if (!mesh::canRead(path)) {
                        return failed("can't open " + path + ": use one of " + joinList(mesh::readableExtensions()));
                    }
                    try {
                        auto doc = mesh::openMesh(path);
                        const std::string id = s->workspace().add(doc, "mesh");
                        Result r = Result::success("opened " + path + " as " + id + ": " + mesh::describe(doc->mesh));
                        r.with("doc", id);
                        return infoFields(r, doc->mesh);
                    } catch (const std::exception& e) {
                        return failed(e.what());
                    }
                });
            });

    reg.add({"forge.mesh_save",
             "Save mesh",
             "Save the mesh. The extension picks the format: " + joinList(mesh::writableExtensions()) +
                 " (glb is the usual choice for game engines), or png/jpg/bmp/tga to write a rendered picture.",
             {mesh::docParam(),
              {"path", ParamType::String, "file to write, e.g. rock.glb or rock.png", true},
              {"ascii", ParamType::Bool, "STL/PLY: write text instead of binary", false, {}, false},
              {"width", ParamType::Int, "render width in px (image formats)", false, {}, std::int64_t{1024}},
              {"height", ParamType::Int, "render height in px (default: same as width)", false},
              mesh::viewParam(),
              {"outline", ParamType::Bool, "render dark lines on edges and silhouettes", false, {}, true}},
             false},
            mesh::meshCommand(
                session,
                [](MeshDoc& doc, const Params& p) {
                    if (doc.mesh.empty()) return failed("the mesh has no triangles to save");
                    const fs::path path = stringParam(p, "path");
                    Result r = writeMesh(doc.mesh, path, p, doc.previewOptions);
                    if (r.ok && !isRender(path)) doc.path = path.string();
                    return r;
                },
                false));

    reg.add({"forge.mesh_close", "Close mesh", "Close an open mesh document (unsaved changes are lost).", {mesh::docParam()}, false},
            [s](const Params& p) {
                return std::make_unique<FunctionAction>([s, id = stringParam(p, "doc")] {
                    if (!s->workspace().getAs<MeshDoc>(id)) return failed("no open mesh called '" + id + "'");
                    s->workspace().take(id);
                    return Result::success("closed " + id);
                });
            });

    reg.add({"forge.mesh_info",
             "Mesh info",
             "Describe an open mesh: triangles, vertices, bounds (size, min, max), whether it's watertight, open "
             "edges, whether it has normals and UVs, surface area and volume.",
             {mesh::docParam()},
             false, true},
            mesh::meshCommand(
                session,
                [](MeshDoc& doc, const Params& p) {
                    Result r = Result::success(info(doc.mesh));
                    r.with("doc", stringParam(p, "doc"));
                    return infoFields(r, doc.mesh);
                },
                false));

    reg.add({"forge.mesh_preview",
             "Preview mesh",
             "Render the mesh as a PNG to look at it, with outlines on edges and silhouettes.",
             {mesh::docParam(), mesh::viewParam(),
              {"size", ParamType::Int, "image size in px", false, {}, std::int64_t{512}},
              {"outline", ParamType::Bool, "draw edge and silhouette lines", false, {}, true},
              {"smooth", ParamType::Bool, "smooth shading (false shows flat facets)", false, {}, true}},
             false, true},
            mesh::meshCommand(
                session,
                [](MeshDoc& doc, const Params& p) {
                    mesh::RenderOptions ro = doc.previewOptions;
                    if (auto v = mesh::parseView(stringParam(p, "view", "iso"))) ro.view = *v;
                    ro.outline = boolParam(p, "outline", true);
                    ro.smooth = boolParam(p, "smooth", true);
                    const int size = static_cast<int>(std::clamp<std::int64_t>(intParam(p, "size", 512), 16, 4096));
                    const std::string id = stringParam(p, "doc");
                    Result r = Result::success("preview of " + id + " (" + stringParam(p, "view", "iso") + ")");
                    r.attach({"image/png", raster::encodePng(mesh::render(doc.mesh, size, size, ro)), id + ".png"});
                    return r;
                },
                false));

    reg.add({"forge.mesh_transform",
             "Transform mesh",
             "Move, rotate, scale, center, drop to the floor, or convert between Y-up and Z-up. Steps run in this "
             "order: axes, scale, rotate, center, floor, move.",
             {mesh::docParam(),
              axesParam(),
              {"scale", ParamType::NumberList, "1 number (uniform) or 3 (x,y,z), e.g. 0.01 for cm to m", false},
              {"rotate", ParamType::NumberList, "degrees about x,y,z (applied in that order)", false},
              {"center", ParamType::Bool, "move the bounding-box centre to the origin", false, {}, false},
              {"floor", ParamType::Bool, "drop it so the lowest point sits at z = 0", false, {}, false},
              {"move", ParamType::NumberList, "offset x,y,z", false},
              mesh::previewParam(), mesh::viewParam()},
             true},
            mesh::meshCommand(session, [](MeshDoc& doc, const Params& p) {
                const std::string err = applyTransform(doc.mesh, p);
                if (!err.empty()) return failed(err);
                Result r = mesh::meshResult("transformed: " + mesh::describe(doc.mesh), doc, p);
                const mesh::Bounds b = mesh::bounds(doc.mesh);
                return r.with("min", vec(b.min)).with("max", vec(b.max));
            }));

    reg.add({"forge.mesh_decimate",
             "Decimate mesh",
             "Reduce the triangle count to about `triangles`, or to `ratio` of what it has now, by vertex clustering "
             "(fast; good for LODs and background props, rougher than an artist's retopology). Normals are "
             "recomputed; UVs are dropped (run forge.mesh_uv again if you need them).",
             {mesh::docParam(),
              {"triangles", ParamType::Int, "target triangle count", false},
              {"ratio", ParamType::Number, "target as a fraction of the current count, e.g. 0.25", false},
              normalsParam(true), creaseParam(), mesh::previewParam(), mesh::viewParam()},
             true},
            mesh::meshCommand(session, [](MeshDoc& doc, const Params& p) {
                const std::size_t before = doc.mesh.triangleCount();
                const bool hadUvs = doc.mesh.hasUvs();
                const bool normals = boolParam(p, "normals", true);
                const double crease = numberParam(p, "crease", 60);
                if (p.count("triangles")) {
                    const auto t = intParam(p, "triangles");
                    if (t < 1) return failed("triangles must be at least 1");
                    doc.mesh = decimateTo(doc.mesh, static_cast<std::size_t>(t), normals, crease);
                } else if (p.count("ratio")) {
                    const double ratio = numberParam(p, "ratio");
                    if (ratio <= 0 || ratio > 1) return failed("ratio must be in (0, 1]");
                    doc.mesh = decimateRatio(doc.mesh, ratio, normals, crease);
                } else {
                    return failed("give triangles= (a target count) or ratio= (e.g. 0.5)");
                }
                if (doc.mesh.empty()) return failed("that would leave no triangles; aim higher");
                std::string msg = "decimated " + std::to_string(before) + " -> " + std::to_string(doc.mesh.triangleCount()) + " triangles";
                if (hadUvs && !doc.mesh.hasUvs()) msg += " (UVs dropped; forge.mesh_uv makes new ones)";
                return mesh::meshResult(msg, doc, p).with("before", static_cast<std::int64_t>(before));
            }));

    reg.add({"forge.mesh_normals",
             "Mesh normals",
             "Recompute vertex normals (smooth, with edges sharper than `crease` degrees kept hard), flip the mesh "
             "inside out (winding and normals), or clear normals.",
             {mesh::docParam(),
              {"mode", ParamType::String, "what to do", false, {"recompute", "flip", "clear"}, std::string("recompute")},
              creaseParam(), mesh::previewParam(), mesh::viewParam()},
             true},
            mesh::meshCommand(session, [](MeshDoc& doc, const Params& p) {
                const std::string mode = stringParam(p, "mode", "recompute");
                if (mode == "flip") {
                    mesh::flip(doc.mesh);
                    return mesh::meshResult("flipped winding and normals", doc, p);
                }
                if (mode == "clear") {
                    doc.mesh.normals.clear();
                    return mesh::meshResult("normals cleared", doc, p);
                }
                const double crease = numberParam(p, "crease", 60);
                recomputeNormals(doc.mesh, crease);
                return mesh::meshResult("normals recomputed (crease " + num(crease) + " degrees)", doc, p);
            }));

    reg.add({"forge.mesh_weld",
             "Weld vertices",
             "Merge vertices closer than `tolerance` (0 = identical positions), drop triangles that collapse and "
             "unused vertices. Fixes 'triangle soup' exports (every triangle with its own corners) and near-duplicate "
             "vertices; note it merges UV and normal seams too (merged vertices keep the first one's normal and UV).",
             {mesh::docParam(),
              {"tolerance", ParamType::Number, "merge distance in model units", false, {}, 0.0},
              mesh::previewParam(), mesh::viewParam()},
             true},
            mesh::meshCommand(session, [](MeshDoc& doc, const Params& p) {
                const double tol = numberParam(p, "tolerance", 0);
                if (tol < 0) return failed("tolerance can't be negative");
                const auto before = static_cast<std::int64_t>(doc.mesh.vertexCount());
                mesh::weld(doc.mesh, tol);
                const auto after = static_cast<std::int64_t>(doc.mesh.vertexCount());
                return mesh::meshResult("welded " + std::to_string(before) + " -> " + std::to_string(after) + " vertices", doc, p)
                    .with("removed", before - after);
            }));

    reg.add({"forge.mesh_merge",
             "Merge meshes",
             "Append another mesh (an open mesh id or a model file) into this one, optionally offset. Normals and "
             "UVs are kept only if both have them.",
             {mesh::docParam(),
              {"other", ParamType::String, "id of an open mesh (mesh2) or a model file", true},
              {"move", ParamType::NumberList, "offset x,y,z for the appended mesh", false},
              mesh::previewParam(), mesh::viewParam()},
             true},
            mesh::meshCommand(session, [s](MeshDoc& doc, const Params& p) {
                const std::string other = stringParam(p, "other");
                Mesh m;
                if (auto od = s->workspace().getAs<MeshDoc>(other)) {
                    m = od->mesh;
                } else if (mesh::canRead(other) && fs::exists(other)) {
                    m = mesh::load(other);
                } else {
                    return failed("'" + other + "' is neither an open mesh nor a model file");
                }
                NumberList move = listParam(p, "move");
                if (!move.empty()) {
                    if (move.size() != 3) return failed("move takes 3 numbers (x,y,z)");
                    mesh::transform(m, mesh::Mat4::translate({move[0], move[1], move[2]}));
                }
                const auto added = static_cast<std::int64_t>(m.triangleCount());
                mesh::append(doc.mesh, m);
                return mesh::meshResult("merged " + std::to_string(added) + " triangles: " + mesh::describe(doc.mesh), doc, p)
                    .with("added", added);
            }));

    reg.add({"forge.mesh_uv",
             "Generate UVs",
             "Give the mesh simple texture coordinates by projection: planar (flat along an axis, for floors, decals, "
             "terrain), box (each face from its closest side, for crates, walls and rocks), cylindrical (barrels, "
             "trees, columns) or spherical (planets, balls). Replaces existing UVs.",
             {mesh::docParam(),
              {"projection", ParamType::String, "how to project", false, {"box", "planar", "cylindrical", "spherical"}, std::string("box")},
              {"axis", ParamType::String, "planar: direction to project along; cylindrical/spherical: the pole axis", false,
               {"x", "y", "z"}, std::string("z")},
              {"scale", ParamType::Number, "multiply UVs (2 = the texture repeats twice across)", false, {}, 1.0},
              mesh::previewParam(), mesh::viewParam()},
             true},
            mesh::meshCommand(session, [](MeshDoc& doc, const Params& p) {
                if (doc.mesh.empty()) return failed("the mesh has no triangles");
                const std::string proj = stringParam(p, "projection", "box");
                const double scale = numberParam(p, "scale", 1.0);
                if (scale == 0) return failed("scale can't be 0");
                generateUvs(doc.mesh, parseProjection(proj), parseAxis(stringParam(p, "axis", "z")), scale);
                return mesh::meshResult(proj + " UVs generated", doc, p);
            }));

    reg.add({"forge.mesh_lods",
             "Write LOD chain",
             "Write levels of detail for a game engine: one file per ratio, named <name>_lod0.<ext> .. _lodN, each "
             "decimated from the original (ratio 1 = the original). Decimation drops UVs; uv= projects fresh ones on each level (the same mapping on all of them). Works on an open mesh (doc) or a file (input).",
             concat({{"doc", ParamType::String, "id of an open mesh", false},
                     {"input", ParamType::String, "model file to read instead of doc", false},
                     {"output", ParamType::String, "base file name, e.g. lods/rock.glb gives lods/rock_lod0.glb ...", true},
                     {"ratios", ParamType::NumberList, "triangle fraction per level, largest first", false, {}, NumberList{1, 0.5, 0.25, 0.125}},
                     normalsParam(true), creaseParam(),
                     {"ascii", ParamType::Bool, "STL/PLY: write text instead of binary", false, {}, false}},
                    uvParams()),
             false},
            [s](const Params& p) -> std::unique_ptr<Action> {
                return std::make_unique<FunctionAction>([s, p]() -> Result {
                    try {
                        Mesh src;
                        if (p.count("doc")) {
                            auto doc = s->workspace().getAs<MeshDoc>(stringParam(p, "doc"));
                            if (!doc) return failed("no open mesh called '" + stringParam(p, "doc") + "'");
                            src = doc->mesh;
                        } else if (p.count("input")) {
                            src = mesh::load(stringParam(p, "input"));
                        } else {
                            return failed("give doc= (an open mesh) or input= (a model file)");
                        }
                        if (src.empty()) return failed("the mesh has no triangles");
                        const fs::path out = stringParam(p, "output");
                        if (!mesh::canWrite(out)) return failed("output needs a mesh extension: " + joinList(mesh::writableExtensions()));
                        NumberList ratios = p.count("ratios") ? listParam(p, "ratios") : NumberList{1, 0.5, 0.25, 0.125};
                        if (ratios.empty()) return failed("ratios needs at least one number");
                        for (double r : ratios) {
                            if (r <= 0 || r > 1) return failed("each ratio must be in (0, 1]");
                        }
                        const bool normals = boolParam(p, "normals", true);
                        const double crease = numberParam(p, "crease", 60);
                        mesh::SaveOptions so;
                        so.ascii = boolParam(p, "ascii", false);
                        makeParent(out);
                        NumberList counts;
                        std::vector<std::string> files;
                        for (std::size_t i = 0; i < ratios.size(); ++i) {
                            Mesh lod = decimateRatio(src, ratios[i], normals, crease);
                            if (ratios[i] >= 1 && normals && !lod.hasNormals()) recomputeNormals(lod, crease);
                            if (lod.empty()) return failed("ratio " + num(ratios[i]) + " leaves no triangles");
                            if (ratios[i] < 1 || !lod.hasUvs()) applyUvParam(lod, p);
                            fs::path file = out.parent_path() / (out.stem().string() + "_lod" + std::to_string(i) + out.extension().string());
                            mesh::save(file, lod, so);
                            files.push_back(file.string());
                            counts.push_back(static_cast<double>(lod.triangleCount()));
                        }
                        std::string msg = "wrote " + std::to_string(files.size()) + " LODs:";
                        for (std::size_t i = 0; i < files.size(); ++i) msg += "\n  " + files[i] + " (" + toText(counts[i]) + " triangles)";
                        return Result::success(msg)
                            .with("files", joinList(files))
                            .with("triangles", counts)
                            .with("levels", static_cast<std::int64_t>(files.size()));
                    } catch (const std::exception& e) {
                        return failed(e.what());
                    }
                });
            });

    reg.add({"forge.mesh_bake",
             "Bake textures",
             "Bake a high-poly model's detail into texture maps for a low-poly game mesh, like Blender, Substance or "
             "xNormal: a tangent-space normal map (OpenGL style, green up, for Godot and Blender; directx=true for "
             "Unreal) and an ambient occlusion map, laid out on the low mesh's UVs. Unwrap the low mesh first "
             "(model.uv_unwrap, then export it; or forge.mesh_uv). Both meshes must sit in the same place.",
             {{"doc", ParamType::String, "the low-poly mesh (open, with UVs)", false},
              {"low", ParamType::String, "a low-poly model file instead of doc", false},
              {"high", ParamType::String, "the high-poly model file, or an open mesh id", true},
              {"normal", ParamType::String, "where to write the normal map (.png)", false},
              {"ao", ParamType::String, "where to write the ambient occlusion map (.png)", false},
              {"size", ParamType::Int, "texture size, px (square)", false, {}, std::int64_t{512}},
              {"samples", ParamType::Int, "AO rays per texel: more is smoother and slower", false, {}, std::int64_t{24}},
              {"distance", ParamType::Number, "how far apart the two surfaces may be (default: 4% of the model)", false},
              {"ao_distance", ParamType::Number, "how far away something still darkens AO (default: 15% of the model)", false},
              {"padding", ParamType::Int, "px grown past UV island edges", false, {}, std::int64_t{4}},
              {"directx", ParamType::Bool, "flip green for DirectX/Unreal", false, {}, false}},
             false},
            [s](const Params& p) -> std::unique_ptr<Action> {
                return std::make_unique<FunctionAction>([s, p]() -> Result {
                    try {
                        auto openOrLoad = [&](const std::string& v) {
                            if (auto d = s->workspace().getAs<MeshDoc>(v)) return d->mesh;
                            return mesh::load(v);
                        };
                        Mesh low;
                        if (p.count("doc")) {
                            auto d = s->workspace().getAs<MeshDoc>(stringParam(p, "doc"));
                            if (!d) return failed("no open mesh called '" + stringParam(p, "doc") + "'");
                            low = d->mesh;
                        } else if (p.count("low")) {
                            low = mesh::load(stringParam(p, "low"));
                        } else {
                            return failed("give doc= (an open low-poly mesh) or low= (a file)");
                        }
                        const Mesh high = openOrLoad(stringParam(p, "high"));
                        if (!p.count("normal") && !p.count("ao")) return failed("give normal=file.png and/or ao=file.png");
                        BakeOptions o;
                        o.size = static_cast<int>(intParam(p, "size", 512));
                        o.normal = p.count("normal") > 0;
                        o.ao = p.count("ao") > 0;
                        o.aoSamples = static_cast<int>(std::clamp<std::int64_t>(intParam(p, "samples", 24), 1, 1024));
                        o.distance = numberParam(p, "distance", 0);
                        o.aoDistance = numberParam(p, "ao_distance", 0);
                        o.padding = static_cast<int>(std::clamp<std::int64_t>(intParam(p, "padding", 4), 0, 64));
                        o.directXGreen = boolParam(p, "directx", false);
                        const BakeResult br = bakeMaps(low, high, o);
                        std::string msg = "baked " + std::to_string(o.size) + "x" + std::to_string(o.size) + " from " +
                                          std::to_string(high.triangleCount()) + " onto " + std::to_string(low.triangleCount()) + " triangles";
                        Result r = Result::success("");
                        auto write = [&](const char* key, const raster::Image& img, const char* what) {
                            const fs::path f = stringParam(p, key);
                            makeParent(f);
                            raster::save(img, f);
                            msg += std::string("\n  ") + what + ": " + f.string();
                            r.with(key, f.string());
                            r.attach({"image/png", raster::encodePng(img), f.filename().string()});
                        };
                        if (o.normal) write("normal", br.normalMap, "normal map");
                        if (o.ao) write("ao", br.aoMap, "ambient occlusion");
                        const double miss = br.texels ? 100.0 * static_cast<double>(br.missed) / static_cast<double>(br.texels) : 0;
                        if (miss > 5) msg += "\n" + num(miss) + "% of the texels found no high surface: raise distance= or check the two meshes line up";
                        r.message = msg;
                        return r.with("texels", static_cast<std::int64_t>(br.texels)).with("missed", static_cast<std::int64_t>(br.missed));
                    } catch (const std::exception& e) {
                        return failed(e.what());
                    }
                });
            });

    reg.add({"forge.mesh_batch",
             "Batch process meshes",
             "Prepare a whole folder of models in one go: optional weld, decimation, axis conversion, scale, "
             "center/floor, normal recomputation and UV projection (in that order), then save every model in one format to an output folder. Originals are never "
             "replaced; existing results are skipped unless overwrite=true.",
             {{"input_dir", ParamType::String, "folder with the source models", true},
              {"output_dir", ParamType::String, "folder for results (created if missing)", true},
              {"format", ParamType::String, "output format", false, mesh::writableExtensions(), std::string("glb")},
              axesParam(),
              {"scale", ParamType::Number, "uniform scale, e.g. 0.01 for cm to m", false, {}, 1.0},
              {"center", ParamType::Bool, "center each model on the origin", false, {}, false},
              {"floor", ParamType::Bool, "drop each model onto z = 0", false, {}, false},
              {"ratio", ParamType::Number, "decimate to this fraction of the triangles", false},
              {"triangles", ParamType::Int, "decimate to at most this many triangles", false},
              normalsParam(false), creaseParam(),
              uvParams()[0], uvParams()[1],
              {"weld", ParamType::Bool, "weld identical vertices first", false, {}, false},
              {"recursive", ParamType::Bool, "include subfolders", false, {}, false},
              {"overwrite", ParamType::Bool, "replace results that already exist", false, {}, false}},
             false},
            [](const Params& p) -> std::unique_ptr<Action> {
                return std::make_unique<FunctionAction>([p]() -> Result {
                    const fs::path in = stringParam(p, "input_dir"), out = stringParam(p, "output_dir");
                    std::error_code ec;
                    if (!fs::is_directory(in, ec)) return failed("not a folder: " + in.string());
                    if (p.count("ratio") && (numberParam(p, "ratio") <= 0 || numberParam(p, "ratio") > 1)) return failed("ratio must be in (0, 1]");
                    if (p.count("triangles") && intParam(p, "triangles") < 1) return failed("triangles must be at least 1");
                    if (numberParam(p, "scale", 1) == 0) return failed("scale can't be 0");
                    fs::create_directories(out, ec);
                    if (ec) return failed("can't create " + out.string() + ": " + ec.message());
                    const std::string format = stringParam(p, "format", "glb");
                    const bool overwrite = boolParam(p, "overwrite", false);
                    const auto files = meshFiles(in, boolParam(p, "recursive", false), out);
                    if (files.empty()) return failed("no models found in " + in.string());
                    const double crease = numberParam(p, "crease", 60);
                    Params tp;  // transform params for applyTransform
                    tp["axes"] = stringParam(p, "axes", "none");
                    tp["scale"] = NumberList{numberParam(p, "scale", 1.0)};
                    tp["center"] = boolParam(p, "center", false);
                    tp["floor"] = boolParam(p, "floor", false);
                    int done = 0, skipped = 0;
                    std::int64_t trisIn = 0, trisOut = 0;
                    std::string report, failures;
                    for (const auto& f : files) {
                        fs::path target = out / fs::relative(f, in, ec);
                        target.replace_extension(format);
                        if (fs::exists(target, ec) && fs::equivalent(target, f, ec)) {
                            failures += "\n  " + f.string() + ": would replace the original; pick another output_dir or format";
                            continue;
                        }
                        if (!overwrite && fs::exists(target, ec)) {
                            ++skipped;
                            continue;
                        }
                        try {
                            Mesh m = mesh::load(f);
                            if (m.empty()) throw std::runtime_error("no triangles");
                            const auto before = static_cast<std::int64_t>(m.triangleCount());
                            if (boolParam(p, "weld", false)) mesh::weld(m, 0);
                            // decimate before centering so the result sits exactly on the floor
                            if (p.count("triangles")) m = decimateTo(m, static_cast<std::size_t>(intParam(p, "triangles")), true, crease);
                            if (p.count("ratio")) m = decimateRatio(m, numberParam(p, "ratio"), true, crease);
                            const std::string err = applyTransform(m, tp);
                            if (!err.empty()) return failed(err);
                            if (boolParam(p, "normals", false)) recomputeNormals(m, crease);
                            applyUvParam(m, p);
                            if (m.empty()) throw std::runtime_error("decimation left no triangles");
                            fs::create_directories(target.parent_path(), ec);
                            mesh::save(target, m);
                            ++done;
                            trisIn += before;
                            trisOut += static_cast<std::int64_t>(m.triangleCount());
                            report += "\n  " + target.string() + " (" + std::to_string(before) + " -> " + std::to_string(m.triangleCount()) + " triangles)";
                        } catch (const std::exception& e) {
                            failures += "\n  " + f.string() + ": " + e.what();
                        }
                    }
                    std::string msg = "processed " + std::to_string(done) + " of " + std::to_string(files.size()) + " models into " +
                                      out.string() + (skipped ? " (" + std::to_string(skipped) + " already there; overwrite=true to redo)" : "") +
                                      report + failures;
                    Result r = done > 0 || (skipped > 0 && failures.empty()) ? Result::success(msg) : Result::failure(msg);
                    return r.with("processed", static_cast<std::int64_t>(done))
                        .with("skipped", static_cast<std::int64_t>(skipped))
                        .with("failed", static_cast<std::int64_t>(files.size()) - done - skipped)
                        .with("triangles_in", trisIn)
                        .with("triangles_out", trisOut);
                });
            });
}

}  // namespace wetwaresuite::forge::detail
