#include "wetwaresuite/mesh/document.hpp"

#include <algorithm>
#include <cstdio>

#include "wetwaresuite/raster/codec.hpp"

namespace wetwaresuite::mesh {

namespace {

std::string fmt(double v) {
    char buf[32];
    std::snprintf(buf, sizeof buf, "%.4g", v);
    return buf;
}

}  // namespace

std::string describe(const Mesh& m) {
    std::string s = std::to_string(m.triangleCount()) + " triangles, " + std::to_string(m.vertexCount()) + " vertices";
    if (m.empty()) return s;
    const Bounds b = bounds(m);
    const Vec3 sz = b.size();
    s += ", size " + fmt(sz.x) + " x " + fmt(sz.y) + " x " + fmt(sz.z) + " at (" + fmt(b.min.x) + ", " + fmt(b.min.y) + ", " +
         fmt(b.min.z) + ")";
    s += isWatertight(m) ? ", watertight" : ", open";
    return s;
}

std::string MeshDoc::summary() const {
    std::string s = describe(mesh);
    if (!path.empty()) s += ", " + path;
    return s;
}

std::unique_ptr<Attachment> MeshDoc::preview() const {
    return std::make_unique<Attachment>(Attachment{"image/png", raster::encodePng(render(mesh, 512, 512, previewOptions)), "preview.png"});
}

ViewCamera toViewCamera(const Projection& p) {
    ViewCamera c;
    const Vec3* v[3] = {&p.right, &p.up, &p.forward};
    double* out[3] = {c.right, c.up, c.forward};
    for (int i = 0; i < 3; ++i) out[i][0] = v[i]->x, out[i][1] = v[i]->y, out[i][2] = v[i]->z;
    c.cx = p.cx;
    c.cy = p.cy;
    c.scale = p.scale;
    return c;
}

Projection toProjection(const ViewCamera& c) {
    Projection p;
    p.right = {c.right[0], c.right[1], c.right[2]};
    p.up = {c.up[0], c.up[1], c.up[2]};
    p.forward = {c.forward[0], c.forward[1], c.forward[2]};
    p.cx = c.cx;
    p.cy = c.cy;
    p.scale = c.scale;
    return p;
}

ViewCamera orbitCamera(const Mesh& mesh, const OrbitView& view, int width, int height, RenderOptions o) {
    if (view.camera) return *view.camera;
    o.orbit = Orbit{view.yaw, view.pitch, view.zoom, view.panX, view.panY};
    return toViewCamera(fitProjection(mesh, width, height, o));
}

bool renderOrbit(const Mesh& mesh, const OrbitView& view, int width, int height, std::vector<std::uint8_t>& rgba, RenderOptions o) {
    if (width == 0 && height == 0) return true;  // just asking
    if (width <= 0 || height <= 0) return false;
    o.orbit = Orbit{view.yaw, view.pitch, view.zoom, view.panX, view.panY};
    if (view.camera) o.projection = toProjection(*view.camera);
    o.antialias = view.antialias;
    if (auto sh = parseShading(view.shading)) o.shading = *sh;
    o.outline = o.outline && view.antialias;  // outlines and smooth normals cost extra passes; skip them mid-drag
    o.smooth = o.smooth && view.antialias;
    const raster::Image img = render(mesh, width, height, o);
    const std::uint8_t* px = img.data();
    rgba.assign(px, px + static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 4);
    return true;
}

bool MeshDoc::renderView(const OrbitView& view, int width, int height, std::vector<std::uint8_t>& rgba) const {
    return renderOrbit(mesh, view, width, height, rgba, previewOptions);
}

bool MeshDoc::viewCamera(const OrbitView& view, int width, int height, ViewCamera& camera) const {
    camera = orbitCamera(mesh, view, width, height, previewOptions);
    return true;
}

std::shared_ptr<MeshDoc> newMesh(Mesh mesh) {
    auto doc = std::make_shared<MeshDoc>();
    doc->mesh = std::move(mesh);
    return doc;
}

std::shared_ptr<MeshDoc> openMesh(const std::filesystem::path& path) {
    auto doc = newMesh(load(path));
    doc->path = path.string();
    return doc;
}

void saveMesh(const MeshDoc& doc, const std::filesystem::path& path, const SaveOptions& options) { save(path, doc.mesh, options); }

Result MeshEdit::apply() {
    before_ = doc_->mesh;
    Result r;
    try {
        r = fn_(*doc_);
    } catch (const std::exception& e) {
        r = Result::failure(e.what());
    }
    if (!r.ok) revert();
    return r;
}

void MeshEdit::revert() { doc_->mesh = before_; }

ParamSpec docParam() { return {"doc", ParamType::String, "id of an open mesh, e.g. mesh1", true}; }

ParamSpec previewParam() { return {"preview", ParamType::Bool, "attach a PNG render of the result", false, {}, false}; }

ParamSpec viewParam() {
    return {"view", ParamType::String, "camera for renders and previews", false, viewNames(), std::string("iso")};
}

ActionFactory meshCommand(Session& session, std::function<Result(MeshDoc&, const Params&)> edit, bool undoable) {
    return [&session, edit, undoable](const Params& p) -> std::unique_ptr<Action> {
        const std::string id = stringParam(p, "doc");
        auto doc = session.workspace().getAs<MeshDoc>(id);
        if (!doc) {
            return std::make_unique<FunctionAction>([id] { return Result::failure("no open mesh called '" + id + "'"); });
        }
        if (!undoable) {
            return std::make_unique<FunctionAction>([doc, edit, p] {
                try {
                    return edit(*doc, p);
                } catch (const std::exception& e) {
                    return Result::failure(e.what());
                }
            });
        }
        return std::make_unique<MeshEdit>(doc, [edit, p](MeshDoc& d) { return edit(d, p); });
    };
}

Result& maybePreview(Result& r, const MeshDoc& doc, const Params& p) {
    if (!r.ok || !boolParam(p, "preview", false)) return r;
    RenderOptions o = doc.previewOptions;
    if (auto v = parseView(stringParam(p, "view", "iso"))) o.view = *v;
    r.attach({"image/png", raster::encodePng(render(doc.mesh, 512, 512, o)), "preview.png"});
    return r;
}

Result meshResult(const std::string& message, const MeshDoc& doc, const Params& p) {
    Result r = Result::success(message);
    const Vec3 sz = bounds(doc.mesh).size();
    r.with("doc", stringParam(p, "doc"))
        .with("triangles", static_cast<std::int64_t>(doc.mesh.triangleCount()))
        .with("vertices", static_cast<std::int64_t>(doc.mesh.vertexCount()))
        .with("size", NumberList{sz.x, sz.y, sz.z});
    return maybePreview(r, doc, p);
}

}  // namespace wetwaresuite::mesh
