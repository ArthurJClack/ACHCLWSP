// Every cad.* command. Edits run inside a CadEdit, so each is one undo step and
// a change that breaks the tree is rolled back with the reason; reading and
// measuring commands stay out of the history.
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <fstream>

#include "wetwaresuite/cad/cad.hpp"
#include "wetwaresuite/mesh/io.hpp"
#include "wetwaresuite/mesh/ops.hpp"
#include "wetwaresuite/mesh/render.hpp"
#include "wetwaresuite/raster/codec.hpp"
#include "blend.hpp"

namespace wetwaresuite::cad {

namespace {

using mesh::Mesh;
using mesh::Vec3;
using Edit = std::function<Result(CadDoc&, const Params&)>;

std::string num(double v) {
    if (std::abs(v) < 5e-7) return "0";
    char buf[64];
    std::snprintf(buf, sizeof buf, "%.6g", v);
    return buf;
}

std::string lower(std::string s) {
    for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

bool has(const Params& p, const std::string& key) {
    auto it = p.find(key);
    return it != p.end() && !std::holds_alternative<std::monostate>(it->second);
}

bool validFeatureName(const std::string& name) {
    if (name.empty() || name.size() > 64) return false;
    return std::all_of(name.begin(), name.end(),
                       [](char c) { return std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '-'; });
}

ParamSpec docParam() { return {"doc", ParamType::String, "id of an open cad document, e.g. cad1", true}; }
ParamSpec featureParam(const std::string& what = "feature name, e.g. plate or hole1 (cad.feature_list shows them)") {
    return {"feature", ParamType::String, what, true};
}

std::vector<ParamSpec> previewParams() { return {mesh::previewParam(), mesh::viewParam()}; }

ParamSpec specFor(const FieldDef& f) {
    ParamSpec s{f.name, ParamType::String, f.help, f.required, {}, {}};
    switch (f.kind) {
        case FieldKind::Number: s.description += " (number or expression)"; break;
        case FieldKind::Integer: s.description += " (whole number or expression)"; break;
        case FieldKind::Points: s.description += " (comma-separated numbers or expressions)"; break;
        case FieldKind::Ref: s.description += " (name of an earlier feature)"; break;
        case FieldKind::Refs: s.description += " (names of earlier features, comma-separated)"; break;
        case FieldKind::Choice: s.choices = f.choices; break;
        case FieldKind::Bool: s.type = ParamType::Bool; break;
        case FieldKind::Text: break;
    }
    if (!f.fallback.empty()) {
        if (f.kind == FieldKind::Bool) s.defaultValue = f.fallback == "true";
        else s.defaultValue = f.fallback;
    }
    return s;
}

// The text stored for a field from a command param.
std::string fieldText(const Value& v) {
    if (auto b = std::get_if<bool>(&v)) return *b ? "true" : "false";
    return toText(v);
}

ActionFactory cadCommand(Session& session, Edit edit, bool undoable = true) {
    return [&session, edit, undoable](const Params& p) -> std::unique_ptr<Action> {
        const std::string id = stringParam(p, "doc");
        auto doc = session.workspace().getAs<CadDoc>(id);
        if (!doc) {
            return std::make_unique<FunctionAction>(
                [id] { return Result::failure("no open cad document called '" + id + "' (use cad.new or cad.open first)"); });
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
        return std::make_unique<CadEdit>(doc, [edit, p](CadDoc& d) { return edit(d, p); });
    };
}

double totalVolume(const CadDoc& doc) {
    double v = 0;
    for (const auto& b : doc.bodies) v += mesh::volume(b.mesh);
    return v;
}

// The start of every edit's reply; thenReport() adds the model's numbers.
Result edited(const std::string& message, const Params& p, const std::string& feature = {}) {
    Result r = Result::success(message, feature.empty() ? Value{} : Value{feature});
    r.with("doc", stringParam(p, "doc"));
    if (!feature.empty()) r.with("feature", feature);
    return r;
}

// Wraps an edit so the reply carries the regenerated model's numbers and preview.
Edit thenReport(Edit edit) {
    return [edit](CadDoc& doc, const Params& p) {
        Result r = edit(doc, p);
        if (!r.ok) return r;
        doc.regenerate();
        const auto b = mesh::bounds(doc.mesh);
        r.with("bodies", static_cast<std::int64_t>(doc.bodies.size()))
            .with("volume", totalVolume(doc))
            .with("size", NumberList{b.size().x, b.size().y, b.size().z})
            .with("triangles", static_cast<std::int64_t>(doc.mesh.triangleCount()));
        r.message += " (" + std::to_string(doc.bodies.size()) + (doc.bodies.size() == 1 ? " body" : " bodies") +
                     ", volume " + num(totalVolume(doc)) + " " + doc.units + "^3)";
        return mesh::maybePreview(r, doc, p);
    };
}

// "w/2 (=20)" for an expression, the text itself for anything else.
std::string valueText(const CadDoc& doc, const std::string& v, bool numeric = true) {
    if (!numeric) return v;
    try {
        double x = doc.eval(v);
        std::string n = num(x);
        return n == v ? v : v + " (=" + n + ")";
    } catch (const std::exception&) {
        return v + " (error)";
    }
}

std::string describeFeature(const CadDoc& doc, const Feature& f) {
    const FeatureType* t = featureType(f.type);
    std::string s = f.name + " " + f.type + " [" + doc.featureStatus(f.name) + "]";
    for (const auto& [key, value] : f.values) {
        if (value.empty()) continue;
        const FieldDef* d = t ? t->field(key) : nullptr;
        std::string v = valueText(doc, value, d && (d->kind == FieldKind::Number || d->kind == FieldKind::Integer));
        if (v.size() > 70) v = v.substr(0, 67) + "...";
        s += " " + key + "=" + v;
    }
    if (!f.source.empty()) s += " source=" + f.source;
    if (const Mesh* m = doc.featureMesh(f.name)) {
        const auto b = mesh::bounds(*m);
        s += " bounds=(" + num(b.min.x) + "," + num(b.min.y) + "," + num(b.min.z) + ")-(" + num(b.max.x) + "," +
             num(b.max.y) + "," + num(b.max.z) + ")";
    }
    return s;
}

// Features (other than `self`) whose fields use the parameter `name`.
std::vector<std::string> paramUsers(const CadDoc& doc, const std::string& name) {
    std::vector<std::string> users;
    auto uses = [&](const std::string& expr) {
        auto ids = identifiers(expr);
        return std::find(ids.begin(), ids.end(), name) != ids.end();
    };
    for (const auto& p : doc.params) {
        if (p.name != name && uses(p.expr)) users.push_back("parameter " + p.name);
    }
    for (const auto& f : doc.features) {
        const FeatureType* t = featureType(f.type);
        for (const auto& [key, value] : f.values) {
            const FieldDef* d = t ? t->field(key) : nullptr;
            if (!d) continue;
            bool found = false;
            if (d->kind == FieldKind::Number || d->kind == FieldKind::Integer) found = uses(value);
            if (d->kind == FieldKind::Points) {
                for (const auto& item : splitList(value)) found = found || uses(item);
            }
            if (found) {
                users.push_back("feature " + f.name);
                break;
            }
        }
    }
    return users;
}

std::vector<std::string> featureUsers(const CadDoc& doc, const std::string& name) {
    std::vector<std::string> users;
    for (const auto& f : doc.features) {
        const FeatureType* t = featureType(f.type);
        if (!t) continue;
        for (const auto& field : t->fields) {
            auto it = f.values.find(field.name);
            if (it == f.values.end()) continue;
            bool uses = field.kind == FieldKind::Ref && it->second == name;
            if (field.kind == FieldKind::Refs) {
                for (const auto& item : splitList(it->second)) uses = uses || item == name;
            }
            if (uses) {
                users.push_back(f.name);
                break;
            }
        }
    }
    return users;
}

// Sets one field of a feature, validating the key and simple kinds.
bool setField(Feature& f, const std::string& key, const std::string& value, std::string& error) {
    const FeatureType* t = featureType(f.type);
    const FieldDef* d = t ? t->field(key) : nullptr;
    if (!d) {
        std::string keys;
        if (t) {
            for (const auto& fd : t->fields) keys += (keys.empty() ? "" : ", ") + fd.name;
        }
        error = "a " + f.type + " has no parameter '" + key + "' (it has: " + keys + ")";
        return false;
    }
    if (value.empty()) {
        if (d->required) {
            error = key + " is required and can't be cleared";
            return false;
        }
        f.values.erase(key);
        return true;
    }
    if (d->kind == FieldKind::Choice && std::find(d->choices.begin(), d->choices.end(), value) == d->choices.end()) {
        std::string list;
        for (const auto& c : d->choices) list += (list.empty() ? "" : ", ") + c;
        error = key + " must be one of: " + list;
        return false;
    }
    std::string v = value;
    if (d->kind == FieldKind::Bool) {
        const std::string l = lower(value);
        if (l == "true" || l == "1" || l == "yes" || l == "on") v = "true";
        else if (l == "false" || l == "0" || l == "no" || l == "off") v = "false";
        else {
            error = key + " must be true or false";
            return false;
        }
    }
    f.values[key] = v;
    return true;
}

// Resolves "x,y,z" or a feature name (its bounding-box centre).
bool pointArg(const CadDoc& doc, const std::string& text, Vec3& out, std::string& error) {
    if (const Mesh* m = doc.featureMesh(text)) {
        out = mesh::bounds(*m).center();
        return true;
    }
    if (doc.findFeature(text)) {
        error = "feature " + text + " has no geometry (suppressed or empty)";
        return false;
    }
    auto items = splitList(text);
    if (items.size() != 3) {
        error = "'" + text + "' is neither a feature name nor an x,y,z point";
        return false;
    }
    try {
        out = {doc.eval(items[0]), doc.eval(items[1]), doc.eval(items[2])};
    } catch (const std::exception& e) {
        error = e.what();
        return false;
    }
    return true;
}

// The mesh a measuring command looks at: one feature, or the whole model.
const Mesh* target(const CadDoc& doc, const Params& p, std::string& error) {
    if (!has(p, "feature")) {
        if (doc.mesh.empty()) error = "the model is empty; add a feature first";
        return doc.mesh.empty() ? nullptr : &doc.mesh;
    }
    const std::string name = stringParam(p, "feature");
    const Mesh* m = doc.featureMesh(name);
    if (!m) error = doc.findFeature(name) ? "feature " + name + " has no geometry" : "no feature called '" + name + "'";
    return m;
}

ParamSpec optionalFeature() {
    return {"feature", ParamType::String, "measure just this feature's own result (default: the whole model)", false};
}

Result docCreated(Session& session, std::shared_ptr<CadDoc> doc, const std::string& verb) {
    std::string id = session.workspace().add(doc, "cad");
    Result r = Result::success(verb + " " + id + " (" + doc->summary() + ")", id);
    r.with("doc", id)
        .with("units", doc->units)
        .with("features", static_cast<std::int64_t>(doc->features.size()))
        .with("bodies", static_cast<std::int64_t>(doc->bodies.size()));
    return r;
}

class CloseAction : public Action {
public:
    CloseAction(Session& session, std::string id) : session_(session), id_(std::move(id)) {}
    Result apply() override {
        doc_ = session_.workspace().take(id_);
        if (!doc_) return Result::failure("no open document called '" + id_ + "'");
        return Result::success("closed " + id_);
    }
    void revert() override {
        if (doc_) session_.workspace().put(id_, doc_);
    }

private:
    Session& session_;
    std::string id_;
    std::shared_ptr<Document> doc_;
};

void registerFeatureCommand(Session& session, const FeatureType& t) {
    std::vector<ParamSpec> params{docParam()};
    for (const auto& f : t.fields) params.push_back(specFor(f));
    params.push_back({"name", ParamType::String, "name for the feature (default: type + number, e.g. " + t.type + "1)", false});
    for (auto& s : previewParams()) params.push_back(s);
    const std::string type = t.type;
    session.registry().add(
        {"cad." + type, t.title, t.description + " Returns the feature name.", params, true},
        cadCommand(session, thenReport([type](CadDoc& doc, const Params& p) {
            Feature f;
            f.type = type;
            f.name = has(p, "name") ? stringParam(p, "name") : doc.newFeatureName(type);
            if (!validFeatureName(f.name)) return Result::failure("feature names use letters, digits, _ and -");
            if (doc.findFeature(f.name)) return Result::failure("there is already a feature called '" + f.name + "'");
            const FeatureType* ft = featureType(type);
            for (const auto& field : ft->fields) {
                auto it = p.find(field.name);
                if (it == p.end() || std::holds_alternative<std::monostate>(it->second)) {
                    if (field.required) return Result::failure(field.name + " is required");
                    continue;
                }
                std::string error;
                if (!setField(f, field.name, fieldText(it->second), error)) return Result::failure(error);
            }
            const std::string name = f.name;
            // join / cut / intersect without a target works on the last body, like SolidWorks' merge result
            if (ft->field("operation") && ft->field("target") && !f.values.count("target")) {
                const std::string op = f.values.count("operation") ? f.values["operation"] : ft->field("operation")->fallback;
                if (op != "new" && !doc.bodies.empty()) f.values["target"] = doc.bodies.back().feature;
                else if (op == "join") f.values["operation"] = "new";
                else if (op != "new") return Result::failure(op + " needs a body to work on: add one first or give target=");
            }
            doc.features.push_back(std::move(f));
            return edited("added " + type + " " + name, p, name);
        })));
}

}  // namespace

// cad.edges: the sharp edges fillet and chamfer can pick, with points to pass as near=.
void registerEdgeCommand(Session& session) {
    session.registry().add(
        {"cad.edges", "List Edges",
         "List the sharp edges of the model or of one feature (where flat faces meet at angle degrees or more): "
         "midpoint, length, direction and outer/inner. Pass a midpoint to cad.fillet or cad.chamfer as near=x,y,z.",
         {docParam(), optionalFeature(), {"angle", ParamType::Number, "smallest angle between the faces, degrees", false, {}, 30.0}},
         false, true},
        cadCommand(session, [](CadDoc& doc, const Params& p) {
            std::string error;
            const Mesh* m = target(doc, p, error);
            if (!m) return Result::failure(error);
            const auto edges = detail::sharpEdges(*m, numberParam(p, "angle", 30));
            std::string text;
            NumberList mids;
            int outer = 0;
            for (std::size_t i = 0; i < edges.size(); ++i) {
                const auto& e = edges[i];
                const Vec3 mid = (e.a + e.b) * 0.5, d = mesh::normalize(e.b - e.a);
                const char* along = std::abs(d.z) > 1 - 1e-9 ? "z" : std::abs(d.x) > 1 - 1e-9 ? "x" : std::abs(d.y) > 1 - 1e-9 ? "y" : "slanted";
                text += "\n  " + std::to_string(i + 1) + ": " + num(mid.x) + "," + num(mid.y) + "," + num(mid.z) + "  length " +
                        num(mesh::length(e.b - e.a)) + ", along " + along + ", " + (e.convex ? "outer" : "inner") + ", " + num(e.angle) + " deg";
                mids.insert(mids.end(), {mid.x, mid.y, mid.z});
                outer += e.convex ? 1 : 0;
            }
            Result r = Result::success(std::to_string(edges.size()) + " sharp edges (" + std::to_string(outer) + " outer, " +
                                       std::to_string(edges.size() - static_cast<std::size_t>(outer)) + " inner)" + text);
            r.with("count", static_cast<std::int64_t>(edges.size())).with("midpoints", mids);
            return r;
        }, false));
}

void registerSketchCommands(Session& session);
void registerSketchConstraintCommands(Session& session);
void registerAssemblyCommands(Session& session);
void registerDrawingCommands(Session& session);
void registerPickCommands(Session& session);
void registerConfigCommands(Session& session);
void registerSheetMetalCommands(Session& session);

void registerCommands(Session& session) {
    auto& reg = session.registry();
    registerSketchCommands(session);
    registerPickCommands(session);
    registerSketchConstraintCommands(session);
    registerAssemblyCommands(session);
    registerDrawingCommands(session);
    registerConfigCommands(session);
    registerSheetMetalCommands(session);

    // ---- documents ----------------------------------------------------------------

    reg.add({"cad.new", "New Part",
             "Create an empty parametric part and return its id (e.g. cad1). Then define parameters with "
             "cad.param_set and add features (cad.box, cad.extrude_rect, cad.hole, ...). Z is up.",
             {{"units", ParamType::String, "length unit, used for mass", false, unitNames(), std::string("mm")}}, false, true},
            [&session](const Params& p) -> std::unique_ptr<Action> {
                return std::make_unique<FunctionAction>(
                    [&session, p] { return docCreated(session, newCad(stringParam(p, "units", "mm")), "created"); });
            });

    reg.add({"cad.open", "Open Part",
             "Open a .wwcad part (parameters and feature tree), or import a STEP or mesh file (step, stl, obj, ply, off, "
             "glb, gltf, 3mf, fbx) as a new part with one fixed 'imported' feature. STEP faces (planes, cylinders, cones, "
             "spheres, tori, B-splines) are turned into fine triangles. Returns the document id.",
             {{"path", ParamType::String, "path to a .wwcad, .step or mesh file", true}}, false, true},
            [&session](const Params& p) -> std::unique_ptr<Action> {
                return std::make_unique<FunctionAction>([&session, p] {
                    try {
                        return docCreated(session, openCad(stringParam(p, "path")), "opened");
                    } catch (const std::exception& e) {
                        return Result::failure(e.what());
                    }
                });
            });

    reg.add({"cad.import", "Import Mesh",
             "Add a mesh or STEP file (stl, obj, ply, off, glb, gltf, 3mf, fbx, step) to a part as a fixed 'imported' feature, which "
             "booleans, holes and patterns can use like any other.",
             [] {
                 std::vector<ParamSpec> s{docParam(), {"path", ParamType::String, "mesh file", true}};
                 for (const auto& f : featureType("imported")->fields) s.push_back(specFor(f));
                 s.push_back({"name", ParamType::String, "feature name (default imported1, ...)", false});
                 for (auto& x : previewParams()) s.push_back(x);
                 return s;
             }(),
             true},
            cadCommand(session, thenReport([](CadDoc& doc, const Params& p) {
                Feature f;
                f.type = "imported";
                f.name = has(p, "name") ? stringParam(p, "name") : doc.newFeatureName("imported");
                if (!validFeatureName(f.name)) return Result::failure("feature names use letters, digits, _ and -");
                if (doc.findFeature(f.name)) return Result::failure("there is already a feature called '" + f.name + "'");
                f.source = stringParam(p, "path");
                f.imported = std::make_shared<const Mesh>(mesh::load(f.source));
                for (const auto& field : featureType("imported")->fields) {
                    std::string error;
                    if (has(p, field.name) && !setField(f, field.name, fieldText(p.at(field.name)), error)) return Result::failure(error);
                }
                const std::string name = f.name;
                doc.features.push_back(std::move(f));
                return edited("imported " + name, p, name);
            })));

    reg.add({"cad.save", "Save Part",
             "Save a part. .wwcad keeps the parameters and feature tree (reopen with cad.open); step exports a solid "
             "B-rep for other CAD programs (flat faces; curved ones as facets); stl, obj, ply, off, glb, gltf, 3mf or fbx export the mesh; png, jpg, bmp, tga or ppm render a picture (view, size).",
             {docParam(),
              {"path", ParamType::String, "output file; the extension picks the format", true},
              {"ascii", ParamType::Bool, "STL/PLY: write text instead of binary", false, {}, false},
              {"size", ParamType::Int, "image exports: width and height in px", false, {}, std::int64_t{1024}},
              mesh::viewParam()},
             false},
            cadCommand(session, [](CadDoc& doc, const Params& p) {
                const std::string path = stringParam(p, "path");
                std::string ext = lower(std::filesystem::path(path).extension().string());
                if (!ext.empty()) ext = ext.substr(1);
                Result r = Result::success("saved " + path, path);
                r.with("path", path);
                if (ext == "wwcad" || ext == "achcad") {  // achcad: the old name, still accepted
                    std::ofstream out(path, std::ios::binary);
                    if (!out) return Result::failure("can't write " + path);
                    out << toJson(doc);
                    if (!out) return Result::failure("can't write " + path);
                    doc.path = path;
                    return r;
                }
                if (doc.mesh.empty()) return Result::failure("the model is empty, nothing to export");
                const std::vector<std::string> images = {"png", "jpg", "jpeg", "bmp", "tga", "ppm"};
                if (std::find(images.begin(), images.end(), ext) != images.end()) {
                    const int size = static_cast<int>(std::clamp<std::int64_t>(intParam(p, "size", 1024), 16, 8192));
                    mesh::RenderOptions o = doc.previewOptions;
                    if (auto v = mesh::parseView(stringParam(p, "view", "iso"))) o.view = *v;
                    raster::save(mesh::render(doc.mesh, size, size, o), path);
                    return r;
                }
                if (!mesh::canWrite(path)) {
                    return Result::failure("unknown format '" + ext + "': use .wwcad, step, stl, obj, ply, off, glb, gltf, 3mf, fbx, png or jpg");
                }
                mesh::SaveOptions so;
                so.ascii = boolParam(p, "ascii", false);
                mesh::save(path, doc.mesh, so);
                r.with("triangles", static_cast<std::int64_t>(doc.mesh.triangleCount()))
                    .with("watertight", mesh::isWatertight(doc.mesh));
                return r;
            }, false));

    reg.add({"cad.close", "Close Part", "Close a part (undo reopens it).", {docParam()}, true},
            [&session](const Params& p) -> std::unique_ptr<Action> {
                const std::string id = stringParam(p, "doc");
                if (!session.workspace().getAs<CadDoc>(id)) {
                    return std::make_unique<FunctionAction>([id] { return Result::failure("no open cad document called '" + id + "'"); });
                }
                return std::make_unique<CloseAction>(session, id);
            });

    reg.add({"cad.info", "Part Info",
             "Describe a part: units, every parameter with its value, every feature in order with its status "
             "(body, used by another feature, suppressed), the resulting bodies and the whole model's bounding box, "
             "volume, surface area and whether it is watertight.",
             {docParam()}, false, true},
            cadCommand(session, [](CadDoc& doc, const Params& p) {
                const auto b = mesh::bounds(doc.mesh);
                const double vol = totalVolume(doc), area = mesh::surfaceArea(doc.mesh);
                const bool tight = !doc.mesh.empty() && mesh::isWatertight(doc.mesh);
                std::string text = stringParam(p, "doc") + ": " + doc.units + ", " + std::to_string(doc.params.size()) +
                                   " params, " + std::to_string(doc.features.size()) + " features, " +
                                   std::to_string(doc.bodies.size()) + " bodies";
                text += "\nparameters:";
                if (doc.params.empty()) text += " none";
                for (const auto& par : doc.params) text += "\n  " + par.name + " = " + valueText(doc, par.expr);
                text += "\nfeatures:";
                if (doc.features.empty()) text += " none";
                for (const auto& f : doc.features) text += "\n  " + describeFeature(doc, f);
                text += "\nbodies:";
                if (doc.bodies.empty()) text += " none";
                std::string names;
                for (const auto& body : doc.bodies) {
                    text += "\n  " + body.feature + ": volume " + num(mesh::volume(body.mesh)) + ", " + mesh::describe(body.mesh);
                    names += (names.empty() ? "" : ",") + body.feature;
                }
                if (!doc.mesh.empty()) {
                    text += "\nmodel: size " + num(b.size().x) + " x " + num(b.size().y) + " x " + num(b.size().z) + " " +
                            doc.units + ", min (" + num(b.min.x) + ", " + num(b.min.y) + ", " + num(b.min.z) + "), max (" +
                            num(b.max.x) + ", " + num(b.max.y) + ", " + num(b.max.z) + "), volume " + num(vol) + " " + doc.units +
                            "^3, area " + num(area) + " " + doc.units + "^2, " + (tight ? "watertight" : "not watertight");
                }
                std::string featureNames;
                for (const auto& f : doc.features) featureNames += (featureNames.empty() ? "" : ",") + f.name;
                Result r = Result::success(text);
                r.with("doc", stringParam(p, "doc"))
                    .with("units", doc.units)
                    .with("params", static_cast<std::int64_t>(doc.params.size()))
                    .with("features", featureNames)
                    .with("bodies", names)
                    .with("min", NumberList{b.empty() ? 0 : b.min.x, b.empty() ? 0 : b.min.y, b.empty() ? 0 : b.min.z})
                    .with("max", NumberList{b.empty() ? 0 : b.max.x, b.empty() ? 0 : b.max.y, b.empty() ? 0 : b.max.z})
                    .with("size", NumberList{b.size().x, b.size().y, b.size().z})
                    .with("volume", vol)
                    .with("area", area)
                    .with("watertight", tight)
                    .with("triangles", static_cast<std::int64_t>(doc.mesh.triangleCount()));
                return r;
            }, false));

    reg.add({"cad.preview", "Preview Part",
             "Render the part (or one feature's own result) to a PNG so you can see it: view iso, front, back, "
             "left, right (or side), top, bottom.",
             {docParam(), mesh::viewParam(),
              {"size", ParamType::Int, "image width and height in px", false, {}, std::int64_t{512}},
              {"feature", ParamType::String, "render only this feature's result", false}},
             false, true},
            cadCommand(session, [](CadDoc& doc, const Params& p) {
                std::string error;
                const Mesh* m = target(doc, p, error);
                if (!m) return Result::failure(error);
                const int size = static_cast<int>(std::clamp<std::int64_t>(intParam(p, "size", 512), 16, 4096));
                mesh::RenderOptions o = doc.previewOptions;
                if (auto v = mesh::parseView(stringParam(p, "view", "iso"))) o.view = *v;
                Result r = Result::success("preview " + std::to_string(size) + "x" + std::to_string(size) + " (" +
                                           stringParam(p, "view", "iso") + ")");
                r.with("doc", stringParam(p, "doc"));
                r.attach({"image/png", raster::encodePng(mesh::render(*m, size, size, o)), "preview.png"});
                return r;
            }, false));

    // ---- parameters --------------------------------------------------------------------

    reg.add({"cad.param_set", "Set Parameter",
             "Create or change a named parameter. value is a number or an expression over other parameters "
             "(e.g. width/2+3, sqrt(a^2+b^2), min(w,h)). Every feature using it regenerates; if the new value breaks "
             "a feature the change is refused with the reason.",
             {docParam(), {"name", ParamType::String, "parameter name: letters, digits, _ (e.g. width)", true},
              {"value", ParamType::String, "number or expression", true}, mesh::previewParam(), mesh::viewParam()},
             true},
            cadCommand(session, thenReport([](CadDoc& doc, const Params& p) {
                const std::string name = stringParam(p, "name"), value = stringParam(p, "value");
                if (!validParamName(name)) {
                    return Result::failure("'" + name + "' isn't a valid parameter name (letters, digits and _, starting with a letter; not a function name or pi)");
                }
                bool found = false;
                for (auto& par : doc.params) {
                    if (par.name == name) {
                        par.expr = value;
                        found = true;
                    }
                }
                if (!found) doc.params.push_back({name, value});
                const double v = doc.paramValue(name);
                Result r = edited(name + " = " + value + (num(v) == value ? "" : " = " + num(v)), p);
                r.value = v;
                r.with("name", name).with("value", v);
                return r;
            })));

    reg.add({"cad.param_list", "List Parameters", "List every parameter with its expression and current value.", {docParam()},
             false, true},
            cadCommand(session, [](CadDoc& doc, const Params& p) {
                std::string text, names;
                for (const auto& par : doc.params) {
                    std::string v;
                    try {
                        v = num(doc.paramValue(par.name));
                    } catch (const std::exception& e) {
                        v = std::string("error: ") + e.what();
                    }
                    text += (text.empty() ? "" : "\n") + par.name + " = " + par.expr + (v == par.expr ? "" : " = " + v);
                    names += (names.empty() ? "" : ",") + par.name;
                }
                Result r = Result::success(text.empty() ? "no parameters" : text);
                r.with("doc", stringParam(p, "doc")).with("params", names);
                return r;
            }, false));

    reg.add({"cad.param_delete", "Delete Parameter", "Delete a parameter. Refused while any feature or parameter uses it.",
             {docParam(), {"name", ParamType::String, "parameter name", true}}, true},
            cadCommand(session, thenReport([](CadDoc& doc, const Params& p) {
                const std::string name = stringParam(p, "name");
                if (!doc.findParam(name)) return Result::failure("no parameter called '" + name + "'");
                auto users = paramUsers(doc, name);
                if (!users.empty()) {
                    std::string list;
                    for (const auto& u : users) list += (list.empty() ? "" : ", ") + u;
                    return Result::failure("can't delete " + name + ": used by " + list);
                }
                doc.params.erase(std::remove_if(doc.params.begin(), doc.params.end(), [&](const Parameter& x) { return x.name == name; }),
                                 doc.params.end());
                return edited("deleted parameter " + name, p);
            })));

    // ---- features ----------------------------------------------------------------------

    registerEdgeCommand(session);

    for (const auto& t : featureTypes()) {
        if (t.type != "imported") registerFeatureCommand(session, t);
    }

    reg.add({"cad.add_hole", "Add Sketch Hole",
             "Add another hole outline to an extrude feature's sketch (the first can be given as hole_points).",
             {docParam(), featureParam("an extrude feature"),
              {"points", ParamType::String, "hole outline x,y,x,y,... (numbers or expressions)", true}, mesh::previewParam(),
              mesh::viewParam()},
             true},
            cadCommand(session, thenReport([](CadDoc& doc, const Params& p) {
                const std::string name = stringParam(p, "feature");
                Feature* f = doc.findFeature(name);
                if (!f) return Result::failure("no feature called '" + name + "'");
                if (f->type != "extrude") return Result::failure(name + " is a " + f->type + "; add_hole works on extrude features");
                std::string key = "hole_points";
                for (int i = 2; f->values.count(key) && !f->values[key].empty(); ++i) key = "hole_points_" + std::to_string(i);
                f->values[key] = stringParam(p, "points");
                return edited("added " + key + " to " + name, p, name).with("key", key);
            })));

    // ---- editing the tree --------------------------------------------------------------

    reg.add({"cad.feature_set", "Edit Feature",
             "Change parameters of an existing feature, then regenerate everything after it: param=width value=60, "
             "or several at once with values=\"width=60; height=5\". An empty value resets a field to its default. "
             "Refused (and rolled back) if the tree no longer builds.",
             {docParam(), featureParam(), {"param", ParamType::String, "field to change, e.g. width, diameter, count", false},
              {"value", ParamType::String, "new value: number, expression, feature name or choice", false},
              {"values", ParamType::String, "several changes: key=value pairs separated by ';'", false},
              mesh::previewParam(), mesh::viewParam()},
             true},
            cadCommand(session, thenReport([](CadDoc& doc, const Params& p) {
                const std::string name = stringParam(p, "feature");
                Feature* f = doc.findFeature(name);
                if (!f) return Result::failure("no feature called '" + name + "'");
                std::vector<std::pair<std::string, std::string>> changes;
                if (has(p, "param")) changes.emplace_back(stringParam(p, "param"), stringParam(p, "value"));
                if (has(p, "values")) {
                    std::string all = stringParam(p, "values"), item;
                    auto flush = [&]() -> bool {
                        auto a = item.find_first_not_of(" \t"), b = item.find_last_not_of(" \t");
                        std::string s = a == std::string::npos ? "" : item.substr(a, b - a + 1);
                        item.clear();
                        if (s.empty()) return true;
                        auto eq = s.find('=');
                        if (eq == std::string::npos || eq == 0) return false;
                        std::string k = s.substr(0, eq), v = s.substr(eq + 1);
                        k.erase(k.find_last_not_of(" \t") + 1);
                        v.erase(0, v.find_first_not_of(" \t") == std::string::npos ? v.size() : v.find_first_not_of(" \t"));
                        changes.emplace_back(k, v);
                        return true;
                    };
                    for (char c : all) {
                        if (c == ';') {
                            if (!flush()) return Result::failure("values must look like \"width=60; height=5\"");
                        } else {
                            item += c;
                        }
                    }
                    if (!flush()) return Result::failure("values must look like \"width=60; height=5\"");
                }
                if (changes.empty()) return Result::failure("give param and value, or values=\"key=value; ...\"");
                std::string list;
                for (const auto& [k, v] : changes) {
                    std::string error;
                    if (!setField(*f, k, v, error)) return Result::failure(name + ": " + error);
                    list += (list.empty() ? "" : ", ") + k + "=" + (v.empty() ? "(default)" : v);
                }
                return edited("set " + name + " " + list, p, name);
            })));

    reg.add({"cad.feature_delete", "Delete Feature", "Remove a feature. Refused while a later feature uses it (delete that first).",
             {docParam(), featureParam(), mesh::previewParam(), mesh::viewParam()}, true},
            cadCommand(session, thenReport([](CadDoc& doc, const Params& p) {
                const std::string name = stringParam(p, "feature");
                int i = doc.featureIndex(name);
                if (i < 0) return Result::failure("no feature called '" + name + "'");
                auto users = featureUsers(doc, name);
                if (!users.empty()) {
                    std::string list;
                    for (const auto& u : users) list += (list.empty() ? "" : ", ") + u;
                    return Result::failure("can't delete " + name + ": used by " + list);
                }
                doc.features.erase(doc.features.begin() + i);
                return edited("deleted " + name, p, name);
            })));

    for (bool suppress : {true, false}) {
        reg.add({suppress ? "cad.feature_suppress" : "cad.feature_unsuppress", suppress ? "Suppress Feature" : "Unsuppress Feature",
                 suppress ? "Turn a feature off without deleting it: it produces nothing, operations on it are skipped, "
                            "and a boolean whose cutter is suppressed keeps its first input."
                          : "Turn a suppressed feature back on.",
                 {docParam(), featureParam(), mesh::previewParam(), mesh::viewParam()}, true},
                cadCommand(session, thenReport([suppress](CadDoc& doc, const Params& p) {
                    const std::string name = stringParam(p, "feature");
                    Feature* f = doc.findFeature(name);
                    if (!f) return Result::failure("no feature called '" + name + "'");
                    f->suppressed = suppress;
                    return edited(std::string(suppress ? "suppressed " : "unsuppressed ") + name, p, name);
                })));
    }

    reg.add({"cad.rename", "Rename Feature", "Rename a feature; features that refer to it follow.",
             {docParam(), featureParam(), {"to", ParamType::String, "new name (letters, digits, _ and -)", true}}, true},
            cadCommand(session, thenReport([](CadDoc& doc, const Params& p) {
                const std::string from = stringParam(p, "feature"), to = stringParam(p, "to");
                Feature* f = doc.findFeature(from);
                if (!f) return Result::failure("no feature called '" + from + "'");
                if (!validFeatureName(to)) return Result::failure("feature names use letters, digits, _ and -");
                if (doc.findFeature(to)) return Result::failure("there is already a feature called '" + to + "'");
                f->name = to;
                for (auto& other : doc.features) {
                    const FeatureType* t = featureType(other.type);
                    for (const auto& field : t->fields) {
                        auto it = other.values.find(field.name);
                        if (it == other.values.end()) continue;
                        if (field.kind == FieldKind::Ref && it->second == from) it->second = to;
                        if (field.kind == FieldKind::Refs) {
                            std::string list;
                            for (const auto& item : splitList(it->second)) list += (list.empty() ? "" : ",") + (item == from ? to : item);
                            it->second = list;
                        }
                    }
                }
                if (auto it = doc.results.find(from); it != doc.results.end()) {
                    doc.results[to] = it->second;
                    doc.results.erase(from);
                }
                return edited("renamed " + from + " to " + to, p, to);
            })));

    reg.add({"cad.feature_list", "List Features",
             "List the feature tree in order: name, type, status, every set field (with evaluated values) and bounds.",
             {docParam()}, false, true},
            cadCommand(session, [](CadDoc& doc, const Params& p) {
                std::string text, names;
                for (const auto& f : doc.features) {
                    text += (text.empty() ? "" : "\n") + describeFeature(doc, f);
                    names += (names.empty() ? "" : ",") + f.name;
                }
                Result r = Result::success(text.empty() ? "no features" : text);
                r.with("doc", stringParam(p, "doc")).with("features", names).with("count", static_cast<std::int64_t>(doc.features.size()));
                return r;
            }, false));

    // ---- measuring ---------------------------------------------------------------------

    reg.add({"cad.measure_distance", "Measure Distance",
             "Distance between two points. Each end is an x,y,z point (numbers or expressions) or a feature name, "
             "which means the centre of its bounding box.",
             {docParam(), {"from", ParamType::String, "x,y,z or a feature name", true},
              {"to", ParamType::String, "x,y,z or a feature name", true}},
             false, true},
            cadCommand(session, [](CadDoc& doc, const Params& p) {
                Vec3 a, b;
                std::string error;
                if (!pointArg(doc, stringParam(p, "from"), a, error) || !pointArg(doc, stringParam(p, "to"), b, error)) {
                    return Result::failure(error);
                }
                const Vec3 d = b - a;
                const double dist = length(d);
                Result r = Result::success("distance " + num(dist) + " " + doc.units + " (dx " + num(d.x) + ", dy " + num(d.y) +
                                               ", dz " + num(d.z) + ")",
                                           dist);
                r.with("distance", dist).with("delta", NumberList{d.x, d.y, d.z}).with("from", NumberList{a.x, a.y, a.z}).with(
                    "to", NumberList{b.x, b.y, b.z});
                return r;
            }, false));

    reg.add({"cad.measure_bounds", "Measure Bounds", "Bounding box of the model or of one feature: min, max, size and centre.",
             {docParam(), optionalFeature()}, false, true},
            cadCommand(session, [](CadDoc& doc, const Params& p) {
                std::string error;
                const Mesh* m = target(doc, p, error);
                if (!m) return Result::failure(error);
                const auto b = mesh::bounds(*m);
                const Vec3 s = b.size(), c = b.center();
                Result r = Result::success("size " + num(s.x) + " x " + num(s.y) + " x " + num(s.z) + " " + doc.units + ", min (" +
                                           num(b.min.x) + ", " + num(b.min.y) + ", " + num(b.min.z) + "), max (" + num(b.max.x) +
                                           ", " + num(b.max.y) + ", " + num(b.max.z) + "), centre (" + num(c.x) + ", " + num(c.y) +
                                           ", " + num(c.z) + ")");
                r.with("min", NumberList{b.min.x, b.min.y, b.min.z})
                    .with("max", NumberList{b.max.x, b.max.y, b.max.z})
                    .with("size", NumberList{s.x, s.y, s.z})
                    .with("center", NumberList{c.x, c.y, c.z});
                return r;
            }, false));

    reg.add({"cad.measure_mass", "Measure Mass",
             "Volume, surface area and mass of the model or one feature. density is in g/cm^3 (default 1.25, PLA; "
             "PETG 1.27, ABS 1.04, aluminium 2.70, steel 7.85) and may be an expression.",
             {docParam(), optionalFeature(),
              {"density", ParamType::String, "g/cm^3, number or expression", false, {}, std::string("1.25")}},
             false, true},
            cadCommand(session, [](CadDoc& doc, const Params& p) {
                std::string error;
                const Mesh* m = target(doc, p, error);
                if (!m) return Result::failure(error);
                double density = doc.eval(stringParam(p, "density", "1.25"));
                if (density < 0) return Result::failure("density can't be negative");
                double vol = 0;
                if (has(p, "feature")) {
                    vol = mesh::volume(*m);
                } else {
                    vol = totalVolume(doc);
                }
                const double cm = unitsToCm(doc.units);
                const double cm3 = vol * cm * cm * cm, mass = cm3 * density, area = mesh::surfaceArea(*m);
                Result r = Result::success("volume " + num(vol) + " " + doc.units + "^3 (" + num(cm3) + " cm^3), area " + num(area) +
                                               " " + doc.units + "^2, mass " + num(mass) + " g at " + num(density) + " g/cm^3" +
                                               (mesh::isWatertight(*m) ? "" : " (not watertight: volume is approximate)"),
                                           mass);
                r.with("volume", vol).with("volume_cm3", cm3).with("area", area).with("mass_g", mass).with("density", density).with(
                    "watertight", mesh::isWatertight(*m));
                return r;
            }, false));

    reg.add({"cad.measure_section", "Measure Section",
             "Cut the model (or one feature) with the horizontal plane Z = z and report the cross-section: area "
             "(holes subtracted), number of loops and perimeter.",
             {docParam(), {"z", ParamType::String, "height of the cut, number or expression", true}, optionalFeature()},
             false, true},
            cadCommand(session, [](CadDoc& doc, const Params& p) {
                std::string error;
                const Mesh* m = target(doc, p, error);
                if (!m) return Result::failure(error);
                const double z = doc.eval(stringParam(p, "z"));
                auto loops = mesh::slice(*m, z);
                double area = 0, perimeter = 0;
                for (const auto& loop : loops) {
                    area += mesh::loopArea(loop);
                    for (std::size_t i = 0; i < loop.size(); ++i) perimeter += length(loop[(i + 1) % loop.size()] - loop[i]);
                }
                Result r = Result::success("section at z=" + num(z) + ": area " + num(area) + " " + doc.units + "^2, " +
                                               std::to_string(loops.size()) + (loops.size() == 1 ? " loop" : " loops") +
                                               ", perimeter " + num(perimeter) + " " + doc.units,
                                           area);
                r.with("z", z).with("area", area).with("loops", static_cast<std::int64_t>(loops.size())).with("perimeter", perimeter);
                return r;
            }, false));
}

}  // namespace wetwaresuite::cad
