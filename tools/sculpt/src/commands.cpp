// sculpt.* commands.
#include <algorithm>
#include <optional>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>

#include "wetwaresuite/mesh/document.hpp"
#include "wetwaresuite/mesh/io.hpp"
#include "wetwaresuite/raster/codec.hpp"
#include "wetwaresuite/raster/document.hpp"
#include "wetwaresuite/sculpt/sculpt.hpp"

namespace wetwaresuite::sculpt {

namespace fs = std::filesystem;
using mesh::MeshDoc;

std::string SculptDoc::summary() const {
    std::string s = MeshDoc::summary();
    std::size_t masked = 0;
    for (float m : mask) masked += m >= 0.5f ? 1 : 0;
    if (masked) s += ", " + std::to_string(masked) + " vertices masked";
    if (quads) s += ", quads";
    return s;
}

namespace {

std::string num(double v) {
    char buf[32];
    std::snprintf(buf, sizeof buf, "%.4g", v);
    return buf;
}

std::string lowerExt(const fs::path& p) {
    std::string e = p.extension().string();
    if (!e.empty() && e[0] == '.') e.erase(0, 1);
    std::transform(e.begin(), e.end(), e.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return e;
}

std::string joinList(const std::vector<std::string>& items) {
    std::string out;
    for (const auto& i : items) out += (out.empty() ? "" : ", ") + i;
    return out;
}

Result failed(const std::string& message) { return Result::failure(message); }

bool isImageExt(const fs::path& p) {
    const std::string e = lowerExt(p);
    return e == "png" || e == "jpg" || e == "jpeg" || e == "bmp" || e == "tga" || e == "ppm";
}

// ---- undo ----

// Snapshots the mesh, and for sculpt documents the mask and quad flag too.
class SculptEdit : public Action {
public:
    SculptEdit(std::shared_ptr<MeshDoc> doc, std::function<Result(MeshDoc&)> fn) : doc_(std::move(doc)), fn_(std::move(fn)) {}
    Result apply() override {
        before_ = doc_->mesh;
        if (auto* s = dynamic_cast<SculptDoc*>(doc_.get())) {
            mask_ = s->mask;
            quads_ = s->quads;
        }
        Result r;
        try {
            r = fn_(*doc_);
        } catch (const std::exception& e) {
            r = Result::failure(e.what());
        }
        if (!r.ok) revert();
        return r;
    }
    void revert() override {
        doc_->mesh = before_;
        if (auto* s = dynamic_cast<SculptDoc*>(doc_.get())) {
            s->mask = mask_;
            s->quads = quads_;
        }
    }

private:
    std::shared_ptr<MeshDoc> doc_;
    std::function<Result(MeshDoc&)> fn_;
    Mesh before_;
    Mask mask_;
    bool quads_ = false;
};

// What sculpting needs: one vertex per position (files often split them for
// UVs or normals) and no stale normals.
void prepare(MeshDoc& doc) {
    auto* s = dynamic_cast<SculptDoc*>(&doc);
    if (!s) {
        Mesh welded = doc.mesh;
        welded.normals.clear();
        mesh::weld(welded, 0);
        if (welded.vertexCount() != doc.mesh.vertexCount() || welded.triangleCount() != doc.mesh.triangleCount()) {
            welded.uvs.clear();
            doc.mesh = std::move(welded);
            return;
        }
    }
    doc.mesh.normals.clear();
    if (s && s->mask.size() != doc.mesh.vertexCount()) s->mask.clear();
}

Mask& maskOf(MeshDoc& doc) {
    static thread_local Mask none;
    none.clear();
    auto* s = dynamic_cast<SculptDoc*>(&doc);
    return s ? s->mask : none;
}

void topologyChanged(MeshDoc& doc, bool quads) {
    if (auto* s = dynamic_cast<SculptDoc*>(&doc)) s->quads = quads;
}

using EditFn = std::function<Result(MeshDoc&, const Params&)>;

std::shared_ptr<SculptDoc> loadSculpt(const std::string& path);
Result saveTo(MeshDoc& doc, const fs::path& path, const Params& p);

// Factory for a command on one open mesh ("doc"): sculpt documents and any other mesh document.
// Edits also run on files: input= (and output= to write the result) instead of doc=.
ActionFactory command(Session& session, EditFn fn, bool undoable = true) {
    return [&session, fn, undoable](const Params& p) -> std::unique_ptr<Action> {
        if (!p.count("doc")) {
            if (!undoable || !p.count("input")) {
                return std::make_unique<FunctionAction>([] { return Result::failure("give doc=<id> of an open mesh, or input=<model file> (and output=<file>)"); });
            }
            return std::make_unique<FunctionAction>([fn, p] {
                try {
                    const std::string input = stringParam(p, "input");
                    auto doc = loadSculpt(input);
                    Params q = p;
                    q["doc"] = input;
                    Result r = fn(*doc, q);
                    if (!r.ok || !p.count("output")) return r;
                    const std::string output = stringParam(p, "output");
                    Result w = saveTo(*doc, output, q);
                    if (!w.ok) return w;
                    r.message += "\nsaved " + output;
                    return r.with("output", output);
                } catch (const std::exception& e) {
                    return Result::failure(e.what());
                }
            });
        }
        const std::string id = stringParam(p, "doc");
        auto doc = session.workspace().getAs<MeshDoc>(id);
        if (!doc) {
            return std::make_unique<FunctionAction>([id] {
                return Result::failure("no open mesh called '" + id + "' (sculpt.new, sculpt.open or forge.mesh_open make one)");
            });
        }
        if (!undoable) {
            return std::make_unique<FunctionAction>([doc, fn, p] {
                try {
                    return fn(*doc, p);
                } catch (const std::exception& e) {
                    return Result::failure(e.what());
                }
            });
        }
        return std::make_unique<SculptEdit>(doc, [fn, p](MeshDoc& d) {
            prepare(d);
            return fn(d, p);
        });
    };
}

// ---- params ----

ParamSpec docParam() { return {"doc", ParamType::String, "id of an open mesh, e.g. sculpt1 (any mesh document works)", true}; }
ParamSpec previewParam() { return {"preview", ParamType::Bool, "attach a PNG render of the result", false, {}, false}; }
ParamSpec viewParam() { return {"view", ParamType::String, "camera: iso, front, back, left, right, top, bottom", false, mesh::viewNames(), std::string("iso")}; }
ParamSpec yawParam() { return {"yaw", ParamType::Number, "turn the model this many degrees about Z (counter-clockwise from above) before viewing", false, {}, 0.0}; }
ParamSpec pitchParam() { return {"pitch", ParamType::Number, "tilt the model this many degrees toward the camera (about the view's right axis)", false, {}, 0.0}; }

std::vector<ParamSpec> withPreview(std::vector<ParamSpec> ps) {
    ps.push_back(previewParam());
    ps.push_back(viewParam());
    ps.push_back(yawParam());
    ps.push_back(pitchParam());
    return ps;
}

std::vector<ParamSpec> docPreview(std::vector<ParamSpec> ps) {
    ps.insert(ps.begin(), {"output", ParamType::String, "with input=: file to write the result to (format from the extension)", false});
    ps.insert(ps.begin(), {"input", ParamType::String, "instead of doc: a model file to load, edit and (with output=) save, without opening it", false});
    ps.insert(ps.begin(), {"doc", ParamType::String, "id of an open mesh, e.g. sculpt1 (any mesh document works); or use input= and output=", false});
    return withPreview(std::move(ps));
}

ViewRay viewOf(const Params& p) {
    ViewRay v;
    if (auto parsed = mesh::parseView(stringParam(p, "view", "iso"))) v.view = *parsed;
    v.yaw = numberParam(p, "yaw", 0);
    v.pitch = numberParam(p, "pitch", 0);
    return v;
}

raster::Image renderView(const MeshDoc& doc, const ViewRay& v, int size) {
    mesh::RenderOptions o = doc.previewOptions;
    o.view = v.view;
    return mesh::render(turnedForView(doc.mesh, v), size, size, o);
}

Result& addFields(Result& r, const MeshDoc& doc, const Params& p) {
    const Vec3 sz = mesh::bounds(doc.mesh).size();
    r.with("doc", stringParam(p, "doc"))
        .with("triangles", static_cast<std::int64_t>(doc.mesh.triangleCount()))
        .with("vertices", static_cast<std::int64_t>(doc.mesh.vertexCount()))
        .with("size", NumberList{sz.x, sz.y, sz.z});
    if (r.ok && boolParam(p, "preview", false)) r.attach({"image/png", raster::encodePng(renderView(doc, viewOf(p), 512)), "preview.png"});
    return r;
}

Result done(const std::string& message, const MeshDoc& doc, const Params& p) {
    Result r = Result::success(message);
    return addFields(r, doc, p);
}

Vec3 vec3(const NumberList& l, const Vec3& fallback = {}) {
    if (l.size() != 3) return fallback;
    return {l[0], l[1], l[2]};
}

std::string axisName(int a) { return a == 0 ? "x" : a == 1 ? "y" : "z"; }
int parseAxis(const std::string& s) { return s == "x" ? 0 : s == "y" ? 1 : s == "z" ? 2 : -1; }

std::string addDoc(Session& s, std::shared_ptr<SculptDoc> doc) { return s.workspace().add(std::move(doc), "sculpt"); }

// ---- OBJ with quads ----

std::string writeQuadObj(const Mesh& m) {
    std::string out = "# wetwaresuite sculpt\n";
    char buf[128];
    for (const auto& p : m.positions) {
        std::snprintf(buf, sizeof buf, "v %.6g %.6g %.6g\n", p.x, p.y, p.z);
        out += buf;
    }
    auto tri = [&](std::size_t t) {
        std::snprintf(buf, sizeof buf, "f %u %u %u\n", m.indices[t * 3] + 1, m.indices[t * 3 + 1] + 1, m.indices[t * 3 + 2] + 1);
        out += buf;
    };
    std::size_t t = 0;
    for (; t + 1 < m.triangleCount(); t += 2) {
        const std::uint32_t* a = &m.indices[t * 3];
        const std::uint32_t* b = &m.indices[(t + 1) * 3];
        bool wrote = false;
        for (int k = 0; k < 3 && !wrote; ++k) {
            const std::uint32_t x = a[k], y = a[(k + 1) % 3], z = a[(k + 2) % 3];
            for (int j = 0; j < 3; ++j) {
                if (b[j] == y && b[(j + 1) % 3] == x) {
                    const std::uint32_t d = b[(j + 2) % 3];
                    std::snprintf(buf, sizeof buf, "f %u %u %u %u\n", x + 1, d + 1, y + 1, z + 1);
                    out += buf;
                    wrote = true;
                    break;
                }
            }
        }
        if (!wrote) {
            tri(t);
            tri(t + 1);
        }
    }
    for (; t < m.triangleCount(); ++t) tri(t);
    return out;
}

std::size_t countQuads(const Mesh& m) {
    std::size_t q = 0;
    for (std::size_t t = 0; t + 1 < m.triangleCount(); t += 2) {
        const std::uint32_t* a = &m.indices[t * 3];
        const std::uint32_t* b = &m.indices[(t + 1) * 3];
        int shared = 0;
        for (int i = 0; i < 3; ++i)
            for (int j = 0; j < 3; ++j) shared += a[i] == b[j] ? 1 : 0;
        q += shared == 2 ? 1 : 0;
    }
    return q;
}

std::shared_ptr<SculptDoc> loadSculpt(const std::string& path) {
    if (!mesh::canRead(path)) throw std::runtime_error("can't open " + path + ": use one of " + joinList(mesh::readableExtensions()));
    auto doc = std::make_shared<SculptDoc>();
    doc->mesh = mesh::load(path);
    doc->path = path;
    doc->mesh.normals.clear();
    Mesh welded = doc->mesh;
    mesh::weld(welded, 0);
    if (welded.vertexCount() != doc->mesh.vertexCount() || welded.triangleCount() != doc->mesh.triangleCount()) {
        welded.uvs.clear();
        doc->mesh = std::move(welded);
    }
    if (doc->mesh.empty()) throw std::runtime_error(path + " has no triangles");
    return doc;
}

Result saveTo(MeshDoc& doc, const fs::path& path, const Params& p) {
    if (doc.mesh.empty()) return failed("the mesh has no triangles to save");
    if (auto parent = path.parent_path(); !parent.empty()) {
        std::error_code ec;
        fs::create_directories(parent, ec);
    }
    const std::string ext = lowerExt(path);
    auto* sd = dynamic_cast<SculptDoc*>(&doc);
    std::string note;
    if (isImageExt(path)) {
        const int size = static_cast<int>(std::clamp<std::int64_t>(intParam(p, "size", 1024), 16, 8192));
        raster::save(renderView(doc, viewOf(p), size), path);
    } else if (!mesh::canWrite(path)) {
        return failed("can't save ." + ext + "; use one of: " + joinList(mesh::writableExtensions()) + " (or png/jpg for a picture)");
    } else if (ext == "obj" && sd && sd->quads) {
        std::ofstream f(path, std::ios::binary);
        f << writeQuadObj(doc.mesh);
        if (!f) return failed("couldn't write " + path.string());
        note = " with quads";
    } else {
        Mesh out = doc.mesh;
        if (boolParam(p, "normals", true) && ext != "stl" && ext != "off" && ext != "3mf") mesh::computeNormals(out, 180);
        mesh::SaveOptions so;
        so.ascii = boolParam(p, "ascii", false);
        mesh::save(path, out, so);
    }
    if (!isImageExt(path)) doc.path = path.string();
    std::error_code ec;
    Result r = Result::success("saved " + path.string() + note);
    return r.with("path", path.string()).with("bytes", static_cast<std::int64_t>(fs::file_size(path, ec)));
}

// ---- stroke points ----

// World points from the "points" param: x,y,z triples, or u,v pairs on a preview when space=screen.
// First hit of a ray on the mesh (Moller-Trumbore over every triangle).
std::optional<Vec3> rayHit(const Mesh& m, const Vec3& o, const Vec3& dir) {
    double best = 1e300;
    for (std::size_t t = 0; t < m.triangleCount(); ++t) {
        const Vec3 a = m.corner(t, 0), b = m.corner(t, 1), c = m.corner(t, 2);
        const Vec3 e1 = b - a, e2 = c - a, pv = mesh::cross(dir, e2);
        const double det = mesh::dot(e1, pv);
        if (std::fabs(det) < 1e-18) continue;
        const Vec3 tv = o - a;
        const double u = mesh::dot(tv, pv) / det;
        if (u < 0 || u > 1) continue;
        const Vec3 qv = mesh::cross(tv, e1);
        const double v = mesh::dot(dir, qv) / det, d = mesh::dot(e2, qv) / det;
        if (v < 0 || u + v > 1 || d < 0 || d >= best) continue;
        best = d;
    }
    if (best == 1e300) return std::nullopt;
    return o + dir * best;
}

// space=ray: x,y,z,dx,dy,dz per point, as a window casts them from the mouse. Misses are dropped
// (with their pressure); grab moves along the plane through where it picked up, facing the view.
std::vector<Vec3> rayStroke(const Mesh& m, const NumberList& r, Brush brush, std::vector<double>& pressure, std::string& error) {
    std::vector<Vec3> out;
    std::vector<double> keptPressure;
    if (r.empty() || r.size() % 6 != 0) {
        error = "with space=ray, points takes x,y,z,dx,dy,dz per point";
        return {};
    }
    std::optional<Vec3> anchor;
    for (std::size_t i = 0; i + 5 < r.size(); i += 6) {
        const Vec3 o{r[i], r[i + 1], r[i + 2]};
        Vec3 d{r[i + 3], r[i + 4], r[i + 5]};
        if (mesh::length(d) < 1e-18) continue;
        d = mesh::normalize(d);
        std::optional<Vec3> q;
        if (brush == Brush::Grab && anchor) {
            const double den = mesh::dot(d, d);
            q = o + d * (mesh::dot(*anchor - o, d) / den);  // the point on the ray at the anchor's depth
        } else {
            q = rayHit(m, o, d);
            if (brush == Brush::Grab && q) anchor = q;
        }
        if (!q) continue;
        out.push_back(*q);
        if (i / 6 < pressure.size()) keptPressure.push_back(pressure[i / 6]);
    }
    if (out.empty()) error = "the stroke doesn't touch the model";
    if (!pressure.empty()) pressure = keptPressure;
    return out;
}

std::vector<Vec3> strokePoints(const Mesh& m, const Params& p, Brush brush, std::string& error) {
    const NumberList pts = listParam(p, "points");
    const bool screen = stringParam(p, "space", "world") == "screen";
    std::vector<Vec3> out;
    if (!screen) {
        if (pts.empty() || pts.size() % 3 != 0) {
            error = "points takes x,y,z triples, e.g. points=0,0,1,0.5,0,1 (or space=screen with u,v pairs)";
            return {};
        }
        for (std::size_t i = 0; i < pts.size(); i += 3) out.push_back({pts[i], pts[i + 1], pts[i + 2]});
        return out;
    }
    if (pts.empty() || pts.size() % 2 != 0) {
        error = "with space=screen, points takes u,v pairs from 0 to 1 (left to right, top to bottom of the preview)";
        return {};
    }
    const ViewRay v = viewOf(p);
    if (brush == Brush::Grab) {
        auto anchor = screenToSurface(m, v, pts[0], pts[1]);
        if (!anchor) {
            error = "the first point (" + num(pts[0]) + ", " + num(pts[1]) + ") doesn't land on the model in that view";
            return {};
        }
        out.push_back(*anchor);
        for (std::size_t i = 2; i < pts.size(); i += 2) out.push_back(screenToPlane(m, v, pts[i], pts[i + 1], *anchor));
        return out;
    }
    // Densify in screen space so the stroke follows the surface, not a chord under it.
    const double radius = numberParam(p, "radius", 0);
    const double longest = std::max(mesh::bounds(m).diagonal(), 1e-12);
    const double step = std::max(0.002, radius > 0 ? radius * numberParam(p, "spacing", 0.25) / longest * 0.5 : 0.01);
    for (std::size_t i = 0; i < pts.size(); i += 2) {
        const double u1 = pts[i], v1 = pts[i + 1];
        if (i == 0) {
            if (auto hit = screenToSurface(m, v, u1, v1)) out.push_back(*hit);
            continue;
        }
        const double u0 = pts[i - 2], v0 = pts[i - 1];
        const int n = std::max(1, static_cast<int>(std::ceil(std::hypot(u1 - u0, v1 - v0) / step)));
        for (int k = 1; k <= n && out.size() < kMaxDabs; ++k) {
            const double t = static_cast<double>(k) / n;
            if (auto hit = screenToSurface(m, v, u0 + (u1 - u0) * t, v0 + (v1 - v0) * t)) out.push_back(*hit);
        }
    }
    if (out.empty()) error = "none of the points land on the model in that view (u,v run 0..1 from the top left of the preview)";
    return out;
}

}  // namespace

namespace {

void registerDocumentCommands(Session& session) {
    auto& reg = session.registry();
    Session* s = &session;
    (void)s;

    // ---- documents ----

    reg.add({"sculpt.new",
             "New sculpt",
             "Start a sculpt from a primitive: an icosphere (even triangles everywhere, the usual start), a cube with a "
             "grid on each face, or a flat plane (for reliefs and displacement). `level` subdivides: sphere 20 x 4^level "
             "triangles, cube 12 x 4^level, plane 2 x 4^level. Up to " + std::to_string(kMaxTriangles) + " triangles.",
             withPreview({{"shape", ParamType::String, "starting shape", false, {"sphere", "cube", "plane"}, std::string("sphere")},
                          {"level", ParamType::Int, "subdivision level (sphere 4 = 5120 triangles)", false, {}, std::int64_t{4}},
                          {"size", ParamType::Number, "diameter (sphere) or edge length (cube, plane) in model units", false, {}, 2.0}}),
             false, true},
            [s](const Params& p) {
                return std::make_unique<FunctionAction>([s, p] {
                    const auto shape = parseShape(stringParam(p, "shape", "sphere"));
                    if (!shape) return failed("shape is sphere, cube or plane");
                    try {
                        auto doc = std::make_shared<SculptDoc>();
                        doc->mesh = primitive(*shape, numberParam(p, "size", 2), static_cast<int>(intParam(p, "level", 4)));
                        const std::string id = addDoc(*s, doc);
                        Params q = p;
                        q["doc"] = id;
                        return done("new " + stringParam(p, "shape", "sphere") + " " + id + ": " + mesh::describe(doc->mesh), *doc, q);
                    } catch (const std::exception& e) {
                        return failed(e.what());
                    }
                });
            });

    reg.add({"sculpt.open",
             "Open for sculpting",
             "Open a model file as a sculpt document: " + joinList(mesh::readableExtensions()) +
                 ". Vertices are welded (one per position) so strokes don't tear the surface; normals are dropped and "
                 "UVs too when welding had to merge seams.",
             withPreview({{"path", ParamType::String, "model file", true}}),
             false, true},
            [s](const Params& p) {
                return std::make_unique<FunctionAction>([s, p] {
                    const std::string path = stringParam(p, "path");
                    try {
                        auto doc = loadSculpt(path);
                        const std::string id = addDoc(*s, doc);
                        Params q = p;
                        q["doc"] = id;
                        return done("opened " + path + " as " + id + ": " + mesh::describe(doc->mesh), *doc, q);
                    } catch (const std::exception& e) {
                        return failed(e.what());
                    }
                });
            });

    reg.add({"sculpt.copy",
             "Copy into a sculpt",
             "Make a new sculpt document from any open mesh: a forge model (forge.mesh_open), a cad part, or another "
             "sculpt. The source is left alone, so a cad part can still be rebuilt.",
             withPreview({{"source", ParamType::String, "id of the open mesh to copy, e.g. mesh1 or cad1", true}}),
             false, true},
            [s](const Params& p) {
                return std::make_unique<FunctionAction>([s, p] {
                    const std::string src = stringParam(p, "source");
                    auto from = s->workspace().getAs<MeshDoc>(src);
                    if (!from) return failed("no open mesh called '" + src + "'");
                    if (from->mesh.empty()) return failed(src + " has no triangles");
                    auto doc = std::make_shared<SculptDoc>();
                    doc->mesh = from->mesh;
                    if (auto* fromSculpt = dynamic_cast<SculptDoc*>(from.get())) {
                        doc->mask = fromSculpt->mask;
                        doc->quads = fromSculpt->quads;
                    }
                    Mesh welded = doc->mesh;
                    welded.normals.clear();
                    mesh::weld(welded, 0);
                    if (welded.vertexCount() != doc->mesh.vertexCount()) {
                        welded.uvs.clear();
                        doc->mesh = std::move(welded);
                        doc->mask.clear();
                        doc->quads = false;
                    }
                    doc->mesh.normals.clear();
                    const std::string id = addDoc(*s, doc);
                    Params q = p;
                    q["doc"] = id;
                    return done("copied " + src + " into " + id + ": " + mesh::describe(doc->mesh), *doc, q);
                });
            });

    reg.add({"sculpt.close", "Close sculpt", "Close an open sculpt or mesh document (unsaved changes are lost).", {docParam()}, false},
            [s](const Params& p) {
                return std::make_unique<FunctionAction>([s, id = stringParam(p, "doc")] {
                    if (!s->workspace().getAs<MeshDoc>(id)) return failed("no open mesh called '" + id + "'");
                    s->workspace().take(id);
                    return Result::success("closed " + id);
                });
            });

}

void registerBrushCommands(Session& session) {
    auto& reg = session.registry();
    Session* s = &session;
    (void)s;

    // ---- brushes ----

    reg.add({"sculpt.stroke",
             "Brush stroke",
             "Run a brush along a path. Brushes: draw (raise along the area's normal), inflate (push out along each "
             "vertex's normal), smooth, flatten, pinch (pull toward the stroke line), grab (drag: the first point picks "
             "up the surface, later points are where it goes), crease (a sharp groove; invert=true for a ridge). "
             "points are x,y,z triples in model space, snapped onto the surface, or with space=screen u,v pairs on "
             "a preview from view/yaw/pitch (0..1 from the top left), cast onto the surface. Masked vertices are "
             "protected. symmetry mirrors the stroke across the origin planes.",
             docPreview({{"brush", ParamType::String, "brush", false, brushNames(), std::string("draw")},
                          {"points", ParamType::NumberList, "x,y,z,x,y,z,... (or u,v,u,v,... with space=screen)", true},
                          {"space", ParamType::String, "how points are given: world x,y,z; screen u,v; ray x,y,z,dx,dy,dz (cast onto the surface, as a window does from the mouse)", false, {"world", "screen", "ray"}, std::string("world")},
                          {"radius", ParamType::Number, "brush radius in model units (default: a tenth of the model's size)", false},
                          {"strength", ParamType::Number, "0..1; draw/inflate/crease move up to radius x strength x 0.15 per dab (default 0.5; grab 1)", false},
                          {"falloff", ParamType::String, "how the effect fades toward the edge", false, falloffNames(), std::string("smooth")},
                          {"spacing", ParamType::Number, "distance between dabs, as a fraction of the radius", false, {}, 0.25},
                          {"invert", ParamType::Bool, "dig in instead of raising (draw, inflate, crease; pinch pushes apart)", false, {}, false},
                          {"symmetry", ParamType::String, "mirror the stroke: none, x, y, z, xy, xz, yz, xyz", false, {}, std::string("none")},
                          {"pressures", ParamType::NumberList, "a pen's pressure per point, 0..1: scales the strength along the stroke", false}}),
             true},
            command(session, [](MeshDoc& doc, const Params& p) {
                const auto brush = parseBrush(stringParam(p, "brush", "draw"));
                if (!brush) return failed("unknown brush; use one of " + joinList(brushNames()));
                const auto falloff = parseFalloff(stringParam(p, "falloff", "smooth"));
                if (!falloff) return failed("unknown falloff; use one of " + joinList(falloffNames()));
                const int sym = parseSymmetry(stringParam(p, "symmetry", "none"));
                if (sym < 0) return failed("symmetry is none or a mix of x, y and z, e.g. x or xz");
                if (doc.mesh.empty()) return failed("the mesh has no triangles");
                BrushSettings bs;
                bs.brush = *brush;
                bs.falloff = *falloff;
                bs.symmetry = sym;
                const double diag = mesh::bounds(doc.mesh).diagonal();
                bs.radius = p.count("radius") ? numberParam(p, "radius") : diag / 10;
                bs.strength = p.count("strength") ? numberParam(p, "strength") : (*brush == Brush::Grab ? 1.0 : 0.5);
                bs.spacing = numberParam(p, "spacing", 0.25);
                bs.invert = boolParam(p, "invert", false);
                if (p.count("pressures")) bs.pressure = std::get<NumberList>(p.at("pressures"));
                if (!(bs.radius > 0)) return failed("radius must be positive");
                if (bs.strength < 0 || bs.strength > 1) return failed("strength runs from 0 to 1 (use invert=true to dig)");
                if (!(bs.spacing >= 0.02)) return failed("spacing must be at least 0.02");
                Params q = p;
                q["radius"] = bs.radius;
                std::string error;
                const std::vector<Vec3> path = stringParam(p, "space", "world") == "ray" ? rayStroke(doc.mesh, listParam(p, "points"), *brush, bs.pressure, error)
                                                                                         : strokePoints(doc.mesh, q, *brush, error);
                if (!error.empty()) return failed(error);
                const StrokeStats st = stroke(doc.mesh, maskOf(doc), path, bs);
                if (st.moved == 0) return failed("the stroke didn't move anything: it's off the model, fully masked, or too weak");
                Result r = done(stringParam(p, "brush", "draw") + " stroke: " + std::to_string(st.dabs) + " dabs moved " + std::to_string(st.moved) +
                                    " vertices (up to " + num(st.maxOffset) + ")",
                                doc, p);
                const Vec3 a = path.front(), b = path.back();
                return r.with("dabs", static_cast<std::int64_t>(st.dabs))
                    .with("moved", static_cast<std::int64_t>(st.moved))
                    .with("max_offset", st.maxOffset)
                    .with("radius", bs.radius)
                    .with("start", NumberList{a.x, a.y, a.z})
                    .with("end", NumberList{b.x, b.y, b.z});
            }));

    reg.add({"sculpt.surface_point",
             "Surface point",
             "Find the model-space point under (u, v) of a preview rendered with the same view/yaw/pitch: u,v run "
             "0..1 from the top-left corner. Use it to turn a spot you see in a preview into x,y,z for strokes and masks.",
             {docParam(), {"u", ParamType::Number, "0 = left edge, 1 = right edge", true}, {"v", ParamType::Number, "0 = top edge, 1 = bottom edge", true},
              viewParam(), yawParam(), pitchParam()},
             false, true},
            command(
                session,
                [](MeshDoc& doc, const Params& p) {
                    auto hit = screenToSurface(doc.mesh, viewOf(p), numberParam(p, "u"), numberParam(p, "v"));
                    if (!hit) return failed("that spot shows background, not the model");
                    Result r = Result::success("surface at " + num(hit->x) + ", " + num(hit->y) + ", " + num(hit->z));
                    return r.with("point", NumberList{hit->x, hit->y, hit->z});
                },
                false));

}

void registerMeshEditCommands(Session& session) {
    auto& reg = session.registry();
    Session* s = &session;
    (void)s;

    // ---- whole-mesh edits ----

    reg.add({"sculpt.smooth",
             "Smooth mesh",
             "Smooth the whole mesh (masked vertices are protected, open edges stay put). taubin keeps the volume; "
             "laplacian also shrinks it.",
             docPreview({{"iterations", ParamType::Int, "passes", false, {}, std::int64_t{5}},
                          {"strength", ParamType::Number, "how far each pass moves toward the neighbours' average, (0, 1]", false, {}, 0.5},
                          {"method", ParamType::String, "smoothing method", false, {"taubin", "laplacian"}, std::string("taubin")}}),
             true},
            command(session, [](MeshDoc& doc, const Params& p) {
                const auto it = intParam(p, "iterations", 5);
                if (it < 1 || it > 500) return failed("iterations runs from 1 to 500");
                const double most = smooth(doc.mesh, maskOf(doc), static_cast<int>(it), numberParam(p, "strength", 0.5), stringParam(p, "method", "taubin") == "taubin");
                return done("smoothed (" + std::to_string(it) + " passes, moved up to " + num(most) + ")", doc, p).with("max_offset", most);
            }));

    reg.add({"sculpt.symmetrize",
             "Symmetrize",
             "Make the model symmetric: copy one half over the other across the plane through the origin. "
             "direction=positive copies the +side (e.g. x > 0) onto the -side. The mask is cleared.",
             docPreview({{"axis", ParamType::String, "mirror across the plane perpendicular to this axis", false, {"x", "y", "z"}, std::string("x")},
                          {"direction", ParamType::String, "which half wins", false, {"positive", "negative"}, std::string("positive")}}),
             true},
            command(session, [](MeshDoc& doc, const Params& p) {
                const int axis = parseAxis(stringParam(p, "axis", "x"));
                const bool pos = stringParam(p, "direction", "positive") == "positive";
                doc.mesh = symmetrize(doc.mesh, axis, pos);
                maskOf(doc).clear();
                topologyChanged(doc, false);
                return done(std::string("mirrored the ") + (pos ? "+" : "-") + axisName(axis) + " half: " + mesh::describe(doc.mesh), doc, p);
            }));

    reg.add({"sculpt.subdivide",
             "Subdivide",
             "Split every triangle into four, `levels` times: loop rounds the shape off (a subdivision surface), "
             "midpoint keeps it exactly and just adds detail to sculpt into. Each level is 4x the triangles; the "
             "limit is " + std::to_string(kMaxTriangles) + ". The mask is carried over.",
             docPreview({{"method", ParamType::String, "subdivision rule", false, {"loop", "midpoint"}, std::string("loop")},
                          {"levels", ParamType::Int, "how many times (1..4)", false, {}, std::int64_t{1}}}),
             true},
            command(session, [](MeshDoc& doc, const Params& p) {
                const auto levels = intParam(p, "levels", 1);
                if (levels < 1 || levels > 4) return failed("levels runs from 1 to 4");
                const bool loop = stringParam(p, "method", "loop") == "loop";
                const std::size_t before = doc.mesh.triangleCount();
                std::size_t projected = before;
                for (int i = 0; i < levels; ++i) projected *= 4;
                if (projected > kMaxTriangles) {
                    return failed(std::to_string(levels) + " level(s) would make " + std::to_string(projected) + " triangles, over the limit of " +
                                  std::to_string(kMaxTriangles) + "; use fewer levels or decimate first");
                }
                Mask& mask = maskOf(doc);
                const bool keepUv = doc.mesh.hasUvs();
                for (int i = 0; i < levels; ++i) doc.mesh = subdivide(doc.mesh, loop, &mask);
                if (!keepUv) doc.mesh.uvs.clear();
                topologyChanged(doc, false);
                return done("subdivided " + std::to_string(before) + " -> " + std::to_string(doc.mesh.triangleCount()) + " triangles", doc, p)
                    .with("before", static_cast<std::int64_t>(before));
            }));

    reg.add({"sculpt.remesh",
             "Voxel remesh",
             "Rebuild the surface with even, quad-shaped faces from a voxel grid `resolution` cells along the longest "
             "side (or cells of `voxel_size`). Merges overlapping parts, closes small cracks and evens out stretched "
             "triangles after heavy sculpting. Open meshes come back as a thin closed shell. Fine detail smaller than "
             "a cell is lost. Limit " + std::to_string(kMaxResolution) + " cells. The mask is cleared.",
             docPreview({{"resolution", ParamType::Int, "cells along the longest side (4.." + std::to_string(kMaxResolution) + ")", false, {}, std::int64_t{64}},
                          {"voxel_size", ParamType::Number, "cell size in model units, instead of resolution", false}}),
             true},
            command(session, [](MeshDoc& doc, const Params& p) {
                int res = static_cast<int>(intParam(p, "resolution", 64));
                if (p.count("voxel_size")) {
                    const double vs = numberParam(p, "voxel_size");
                    if (!(vs > 0)) return failed("voxel_size must be positive");
                    const Vec3 sz = mesh::bounds(doc.mesh).size();
                    const double cells = std::ceil(std::max({sz.x, sz.y, sz.z}) / vs);
                    if (cells > kMaxResolution) {
                        return failed("voxel_size " + num(vs) + " needs " + num(cells) + " cells along the longest side; the limit is " +
                                      std::to_string(kMaxResolution) + ", so use a voxel_size of at least " + num(std::max({sz.x, sz.y, sz.z}) / kMaxResolution));
                    }
                    res = static_cast<int>(cells);
                }
                const std::size_t before = doc.mesh.triangleCount();
                bool shell = false;
                doc.mesh = voxelRemesh(doc.mesh, res, &shell);
                maskOf(doc).clear();
                topologyChanged(doc, true);
                std::string msg = "remeshed at " + std::to_string(res) + " cells: " + std::to_string(before) + " -> " +
                                  std::to_string(doc.mesh.triangleCount()) + " triangles (" + std::to_string(countQuads(doc.mesh)) + " quads)";
                if (shell) msg += "; the mesh was open, so it became a thin shell around the surface";
                return done(msg, doc, p).with("resolution", static_cast<std::int64_t>(res)).with("quads", static_cast<std::int64_t>(countQuads(doc.mesh)));
            }));

    reg.add({"sculpt.decimate",
             "Decimate",
             "Reduce the triangle count to `triangles` (or `ratio` of the current count) with quadric edge collapse: "
             "flat areas lose triangles first and the silhouette and open edges are kept. Masked vertices (mask >= "
             "0.5) are locked, so you can protect a face or hands.",
             docPreview({{"triangles", ParamType::Int, "target triangle count", false},
                          {"ratio", ParamType::Number, "target as a fraction of the current count, e.g. 0.25", false}}),
             true},
            command(session, [](MeshDoc& doc, const Params& p) {
                const std::size_t before = doc.mesh.triangleCount();
                std::size_t target = 0;
                if (p.count("triangles")) {
                    const auto t = intParam(p, "triangles");
                    if (t < 4) return failed("triangles must be at least 4");
                    target = static_cast<std::size_t>(t);
                } else if (p.count("ratio")) {
                    const double r = numberParam(p, "ratio");
                    if (!(r > 0) || r > 1) return failed("ratio must be in (0, 1]");
                    target = std::max<std::size_t>(4, static_cast<std::size_t>(std::llround(static_cast<double>(before) * r)));
                } else {
                    return failed("give triangles= (a target count) or ratio= (e.g. 0.5)");
                }
                Mask& mask = maskOf(doc);
                doc.mesh = decimateQuadric(doc.mesh, target, &mask);
                topologyChanged(doc, false);
                std::string msg = "decimated " + std::to_string(before) + " -> " + std::to_string(doc.mesh.triangleCount()) + " triangles";
                if (doc.mesh.triangleCount() > target * 11 / 10 + 2) msg += " (stopped early: the rest would fold faces or touch locked vertices)";
                return done(msg, doc, p).with("before", static_cast<std::int64_t>(before));
            }));

    reg.add({"sculpt.retopo",
             "Retopologize",
             "Quick automatic retopology: rebuilds the model as about `quads` evenly sized quads (voxel remesh sized "
             "to match, then relaxed and projected back onto the original surface). Export as OBJ to get real quad "
             "faces; other formats get each quad as two triangles. Edge loops don't follow features the way hand "
             "retopology would.",
             docPreview({{"quads", ParamType::Int, "target number of quads", false, {}, std::int64_t{2000}},
                          {"relax", ParamType::Int, "relax-and-project rounds (0 = none)", false, {}, std::int64_t{8}}}),
             true},
            command(session, [](MeshDoc& doc, const Params& p) {
                const auto quads = intParam(p, "quads", 2000);
                const auto relax = intParam(p, "relax", 8);
                if (quads < 6) return failed("quads must be at least 6");
                if (relax < 0 || relax > 100) return failed("relax runs from 0 to 100");
                const std::size_t before = doc.mesh.triangleCount();
                doc.mesh = retopologize(doc.mesh, static_cast<std::size_t>(quads), static_cast<int>(relax));
                maskOf(doc).clear();
                topologyChanged(doc, true);
                const std::size_t q = countQuads(doc.mesh);
                return done("retopologized " + std::to_string(before) + " triangles into " + std::to_string(q) + " quads", doc, p)
                    .with("quads", static_cast<std::int64_t>(q));
            }));

    reg.add({"sculpt.displace",
             "Displace from heightmap",
             "Push the surface in and out along its normals by an image's brightness: offset = amount x (brightness - "
             "midlevel). `image` is an image file or an open image (photo.open). mapping=auto uses the mesh's UVs if "
             "it has them, else a planar projection along `axis` (right for a plane from sculpt.new). Subdivide first: "
             "detail finer than the triangles can't show.",
             docPreview({{"image", ParamType::String, "heightmap file (png, jpg, ...) or an open image id like img1", true},
                          {"amount", ParamType::Number, "offset for white minus offset for black, in model units", false, {}, 0.1},
                          {"midlevel", ParamType::Number, "brightness (0..1) that stays put", false, {}, 0.5},
                          {"mapping", ParamType::String, "how the image wraps the mesh", false, {"auto", "uv", "planar", "cylindrical", "spherical"}, std::string("auto")},
                          {"axis", ParamType::String, "planar projection direction, or the pole for cylindrical/spherical", false, {"x", "y", "z"}, std::string("z")},
                          {"tile", ParamType::Number, "repeat the image this many times across the mapping", false, {}, 1.0}}),
             true},
            command(session, [s](MeshDoc& doc, const Params& p) {
                const std::string src = stringParam(p, "image");
                raster::Image img;
                if (auto idoc = s->workspace().getAs<raster::ImageDoc>(src)) {
                    img = idoc->flatten();
                } else if (fs::exists(src)) {
                    img = raster::load(src);
                } else {
                    return failed("no image file or open image called '" + src + "'");
                }
                const auto mapping = parseMapping(stringParam(p, "mapping", "auto"));
                if (!mapping) return failed("unknown mapping");
                DisplaceSettings ds;
                ds.mapping = *mapping;
                ds.axis = parseAxis(stringParam(p, "axis", "z"));
                ds.amount = numberParam(p, "amount", 0.1);
                ds.midlevel = numberParam(p, "midlevel", 0.5);
                ds.tile = numberParam(p, "tile", 1);
                if (!(ds.tile > 0)) return failed("tile must be positive");
                const double most = displace(doc.mesh, maskOf(doc), img, ds);
                return done("displaced by " + src + " (up to " + num(most) + ")", doc, p).with("max_offset", most);
            }));

    reg.add({"sculpt.mask",
             "Mask",
             "Protect parts of the model from strokes, smoothing and displacement (and lock them during decimate). "
             "shape=sphere masks within `radius` of `center`; shape=plane masks the side `normal` points to from "
             "`point`; softness fades the edge. mode combines with the existing mask. Other shapes: all, clear, "
             "invert, grow and shrink (by `steps` edge rings), blur.",
             docPreview({{"shape", ParamType::String, "what to do", false, {"sphere", "plane", "all", "clear", "invert", "grow", "shrink", "blur"}, std::string("sphere")},
                          {"center", ParamType::NumberList, "sphere centre x,y,z", false},
                          {"radius", ParamType::Number, "sphere radius", false},
                          {"point", ParamType::NumberList, "a point on the plane x,y,z", false, {}, NumberList{0, 0, 0}},
                          {"normal", ParamType::NumberList, "plane normal x,y,z: the side it points to is masked", false, {}, NumberList{0, 0, 1}},
                          {"softness", ParamType::Number, "width of the fade at the edge, in model units", false, {}, 0.0},
                          {"mode", ParamType::String, "combine with the current mask", false, {"set", "add", "subtract", "intersect"}, std::string("set")},
                          {"steps", ParamType::Int, "edge rings for grow/shrink, passes for blur", false, {}, std::int64_t{1}}}),
             true},
            command(session, [](MeshDoc& doc, const Params& p) {
                auto* sd = dynamic_cast<SculptDoc*>(&doc);
                if (!sd) return failed("masks live on sculpt documents; run sculpt.copy source=" + stringParam(p, "doc") + " first");
                const std::string shape = stringParam(p, "shape", "sphere");
                const auto mode = parseMaskMode(stringParam(p, "mode", "set"));
                if (!mode) return failed("mode is set, add, subtract or intersect");
                const std::size_t nv = doc.mesh.vertexCount();
                Mask& mask = sd->mask;
                const int steps = static_cast<int>(intParam(p, "steps", 1));
                if (shape == "sphere") {
                    if (!p.count("center") || !p.count("radius")) return failed("shape=sphere needs center=x,y,z and radius=");
                    const NumberList c = listParam(p, "center");
                    if (c.size() != 3) return failed("center takes 3 numbers");
                    if (!(numberParam(p, "radius") > 0)) return failed("radius must be positive");
                    maskSphere(doc.mesh, mask, vec3(c), numberParam(p, "radius"), numberParam(p, "softness", 0), *mode);
                } else if (shape == "plane") {
                    const NumberList pt = listParam(p, "point"), n = listParam(p, "normal");
                    if ((p.count("point") && pt.size() != 3) || (p.count("normal") && n.size() != 3)) return failed("point and normal take 3 numbers each");
                    maskPlane(doc.mesh, mask, vec3(pt, {0, 0, 0}), vec3(n, {0, 0, 1}), numberParam(p, "softness", 0), *mode);
                } else if (shape == "all") {
                    mask.assign(nv, 1.f);
                } else if (shape == "clear") {
                    mask.clear();
                } else if (shape == "invert") {
                    mask.resize(nv, 0.f);
                    for (auto& m : mask) m = 1.f - m;
                } else if (shape == "grow" || shape == "shrink") {
                    if (steps < 1 || steps > 100) return failed("steps runs from 1 to 100");
                    growMask(doc.mesh, mask, shape == "grow" ? steps : -steps);
                } else if (shape == "blur") {
                    if (steps < 1 || steps > 100) return failed("steps runs from 1 to 100");
                    blurMask(doc.mesh, mask, steps);
                } else {
                    return failed("unknown mask shape");
                }
                std::size_t masked = 0;
                for (float m : mask) masked += m >= 0.5f ? 1 : 0;
                return done("mask " + shape + ": " + std::to_string(masked) + " of " + std::to_string(nv) + " vertices masked", doc, p)
                    .with("masked", static_cast<std::int64_t>(masked));
            }));

}

void registerOutputCommands(Session& session) {
    auto& reg = session.registry();
    Session* s = &session;
    (void)s;

    // ---- looking and saving ----

    reg.add({"sculpt.stats",
             "Sculpt stats",
             "Numbers about the mesh: triangles, vertices, edges, open and non-manifold edges, edge lengths (min, "
             "mean, max), area, volume (when watertight), Euler characteristic, masked vertices, quads.",
             {docParam()},
             false, true},
            command(
                session,
                [](MeshDoc& doc, const Params& p) {
                    const Stats st = stats(doc.mesh);
                    std::size_t masked = 0;
                    bool quads = false;
                    if (auto* sd = dynamic_cast<SculptDoc*>(&doc)) {
                        for (float m : sd->mask) masked += m >= 0.5f ? 1 : 0;
                        quads = sd->quads;
                    }
                    const Vec3 sz = mesh::bounds(doc.mesh).size();
                    std::string msg = mesh::describe(doc.mesh) + "\n" + std::to_string(st.edges) + " edges (" + std::to_string(st.openEdges) + " open, " +
                                      std::to_string(st.nonManifoldEdges) + " non-manifold), edge length " + num(st.minEdge) + " / " + num(st.meanEdge) +
                                      " / " + num(st.maxEdge) + " (min / mean / max)";
                    if (masked) msg += "\n" + std::to_string(masked) + " vertices masked";
                    Result r = Result::success(msg);
                    r.with("doc", stringParam(p, "doc"))
                        .with("triangles", static_cast<std::int64_t>(st.triangles))
                        .with("vertices", static_cast<std::int64_t>(st.vertices))
                        .with("edges", static_cast<std::int64_t>(st.edges))
                        .with("open_edges", static_cast<std::int64_t>(st.openEdges))
                        .with("non_manifold_edges", static_cast<std::int64_t>(st.nonManifoldEdges))
                        .with("edge_min", st.minEdge)
                        .with("edge_mean", st.meanEdge)
                        .with("edge_max", st.maxEdge)
                        .with("size", NumberList{sz.x, sz.y, sz.z})
                        .with("area", st.area)
                        .with("watertight", st.watertight)
                        .with("euler", static_cast<std::int64_t>(st.euler))
                        .with("masked", static_cast<std::int64_t>(masked))
                        .with("quads", static_cast<std::int64_t>(quads ? countQuads(doc.mesh) : 0));
                    if (st.watertight) r.with("volume", st.volume);
                    return r;
                },
                false));

    reg.add({"sculpt.preview",
             "Preview sculpt",
             "Render the model as a PNG from a view, optionally turned by yaw/pitch degrees for any angle. Strokes "
             "with space=screen and sculpt.surface_point read u,v positions on this same picture.",
             {docParam(), viewParam(), yawParam(), pitchParam(),
              {"size", ParamType::Int, "image size in px", false, {}, std::int64_t{512}},
              {"outline", ParamType::Bool, "dark lines on silhouettes and sharp edges", false, {}, false},
              {"flat", ParamType::Bool, "flat shading, to see the triangles", false, {}, false}},
             false, true},
            command(
                session,
                [](MeshDoc& doc, const Params& p) {
                    const int size = static_cast<int>(std::clamp<std::int64_t>(intParam(p, "size", 512), 16, 4096));
                    mesh::RenderOptions o = doc.previewOptions;
                    const ViewRay v = viewOf(p);
                    o.view = v.view;
                    o.outline = boolParam(p, "outline", false);
                    o.smooth = !boolParam(p, "flat", false);
                    const std::string id = stringParam(p, "doc");
                    Result r = Result::success("preview of " + id + " (" + stringParam(p, "view", "iso") +
                                               (v.yaw != 0 || v.pitch != 0 ? ", yaw " + num(v.yaw) + ", pitch " + num(v.pitch) : std::string()) + ")");
                    r.attach({"image/png", raster::encodePng(mesh::render(turnedForView(doc.mesh, v), size, size, o)), id + ".png"});
                    return r.with("doc", id);
                },
                false));

    reg.add({"sculpt.export",
             "Export sculpt",
             "Save the model. The extension picks the format: stl, obj, ply, glb, gltf, fbx (also off, 3mf, step), or png/jpg "
             "for a rendered picture from view/yaw/pitch. OBJ keeps quads after sculpt.remesh or sculpt.retopo.",
             {docParam(),
              {"path", ParamType::String, "file to write, e.g. head.stl, head.obj, head.glb, head.png", true},
              {"normals", ParamType::Bool, "write smooth vertex normals (obj, ply, glb, gltf)", false, {}, true},
              {"ascii", ParamType::Bool, "STL/PLY: write text instead of binary", false, {}, false},
              {"size", ParamType::Int, "picture size in px (png/jpg)", false, {}, std::int64_t{1024}},
              viewParam(), yawParam(), pitchParam()},
             false},
            command(
                session,
                [](MeshDoc& doc, const Params& p) {
                    return saveTo(doc, stringParam(p, "path"), p);
                },
                false));
}

}  // namespace

void registerCommands(Session& session) {
    registerDocumentCommands(session);
    registerBrushCommands(session);
    registerMeshEditCommands(session);
    registerOutputCommands(session);
}

}  // namespace wetwaresuite::sculpt
