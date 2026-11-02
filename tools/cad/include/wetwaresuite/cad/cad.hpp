// cad: precision parametric CAD, headless first. A CadDoc is a list of named
// parameters (width = 40, hole_d = width / 8) and an ordered feature tree
// (box, extrude, hole, pattern, boolean, ...). Every numeric field of a feature
// is an expression over those parameters, and the solid is regenerated from
// the tree whenever anything changes, so changing one parameter resizes the
// whole part. Results are cached per feature (keyed by its evaluated inputs),
// so an edit only rebuilds the features it affects.
//
// Every action is a cad.* command; edits snapshot the tree for undo. The
// regenerated solid lives in MeshDoc::mesh, so mesh tools and previews see it.
#pragma once

#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

#include "wetwaresuite/cad/sketch.hpp"
#include "wetwaresuite/core/command.hpp"
#include "wetwaresuite/mesh/document.hpp"
#include "wetwaresuite/mesh/mesh.hpp"

namespace wetwaresuite::cad {

// ---- expressions ------------------------------------------------------------

class ExprError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

// Looks up a name; nullopt means "unknown".
using Resolver = std::function<std::optional<double>(const std::string&)>;

// Evaluates + - * / % ^ (right-associative), parentheses, unary minus, numbers
// (1, 2.5, 1e3), pi, names (through `lookup`) and the functions sin cos tan
// asin acos atan (degrees), atan2(y, x), sqrt abs floor ceil round, min max
// (any number of arguments) and pow(a, b). Throws ExprError with the reason;
// the result is always finite.
double evaluate(const std::string& expr, const Resolver& lookup = {});
// Names an expression refers to (not functions or pi), in order, without repeats.
std::vector<std::string> identifiers(const std::string& expr);
// Splits "0,0, w,0, w,min(a,b)" at top-level commas or semicolons into trimmed,
// non-empty items.
std::vector<std::string> splitList(const std::string& text);
// A valid parameter name: letter or _ then letters, digits, _; not a function or pi.
bool validParamName(const std::string& name);

// ---- the document -------------------------------------------------------------

struct Parameter {
    std::string name;
    std::string expr;
};

// A named set of parameter values (a design table row, SolidWorks' configurations): using it
// sets those parameters, so one part makes an M3, M4 and M5 version of itself.
struct Configuration {
    std::string name;
    std::vector<Parameter> values;
};

struct Feature {
    std::string name;
    std::string type;  // box, cylinder, ..., boolean, hole, pattern_linear, ...
    std::map<std::string, std::string> values;  // field -> text; missing fields use the type's default
    bool suppressed = false;
    std::shared_ptr<const mesh::Mesh> imported;  // type "imported" only
    std::string source;                          // type "imported": where it came from
    Sketch sketch;                               // type "sketch": its points, entities and constraints
};

// Units are a label for lengths, used for mass (density is g/cm^3).
const std::vector<std::string>& unitNames();  // mm cm m in
double unitsToCm(const std::string& units);

struct Body {
    std::string feature;
    mesh::Mesh mesh;
};

class CadDoc : public mesh::MeshDoc {
public:
    std::string units = "mm";
    std::vector<Parameter> params;
    std::vector<Feature> features;
    std::map<std::string, int> counters;  // per type, for automatic names
    std::vector<Configuration> configs;
    std::string activeConfig;  // the configuration last used ("" = none)

    std::string kind() const override { return "cad"; }
    std::string summary() const override;
    // the solid plus its open sketches as lines (and the axes while the part is empty)
    bool renderView(const OrbitView& view, int width, int height, std::vector<std::uint8_t>& rgba) const override;
    bool viewCamera(const OrbitView& view, int width, int height, ViewCamera& camera) const override;
    std::vector<mesh::Line3> sketchLines() const;
    mesh::RenderOptions viewOptions() const;  // how renderView draws it: the solid, its sketches, axes

    const Parameter* findParam(const std::string& name) const;
    int featureIndex(const std::string& name) const;  // -1 if missing
    Feature* findFeature(const std::string& name);
    const Feature* findFeature(const std::string& name) const;
    std::string newFeatureName(const std::string& type);

    double paramValue(const std::string& name) const;   // throws ExprError
    double eval(const std::string& expr) const;          // expression over the params

    // Rebuilds `mesh`, `bodies` and per-feature results from the tree. Throws
    // std::runtime_error (with the feature's name) when something can't be built.
    void regenerate();

    // After regenerate():
    std::vector<Body> bodies;  // features not consumed by a later feature
    const mesh::Mesh* featureMesh(const std::string& name) const;  // a feature's own result, null if none
    std::string featureStatus(const std::string& name) const;     // body, consumed, suppressed, empty

    struct Cached {
        std::string signature;
        std::shared_ptr<const mesh::Mesh> mesh;                  // null = produced nothing
        std::vector<std::shared_ptr<const mesh::Mesh>> tools;    // hole cutters, so patterns/mirrors can repeat them
        std::shared_ptr<const mesh::Mesh> display;               // mesh with flat faces re-triangulated, for bodies
        std::shared_ptr<const FeatureExtra> extra;               // solved sketch, datum plane, axes
        std::string key;                                         // inputs, when they differ from signature (sketches)
    };
    const FeatureExtra* featureExtra(const std::string& name) const;  // null if none
    std::map<std::string, Cached> results;  // feature name -> last result
    std::map<std::string, std::string> consumedBy;
};

std::shared_ptr<CadDoc> newCad(const std::string& units = "mm");
// .wwcad (JSON; the old .achcad still opens) or any mesh format (becomes one "imported" feature). Throws.
std::shared_ptr<CadDoc> openCad(const std::filesystem::path& path);
std::string toJson(const CadDoc& doc);
std::shared_ptr<CadDoc> fromJson(const std::string& text);  // throws std::runtime_error

// Feature types and their fields, which drive the commands and feature_set.
// Ref: one earlier feature (required unless the field says otherwise); Refs: a comma-separated list of them.
enum class FieldKind { Number, Integer, Points, Ref, Choice, Bool, Text, Refs };

struct FieldDef {
    std::string name;
    FieldKind kind = FieldKind::Number;
    std::string help;
    std::string fallback;  // default text; empty = none
    bool required = false;
    std::vector<std::string> choices;
};

struct FeatureType {
    std::string type;
    std::string title;
    std::string description;
    std::vector<FieldDef> fields;
    const FieldDef* field(const std::string& name) const;
};

const std::vector<FeatureType>& featureTypes();
const FeatureType* featureType(const std::string& type);

// Undoable edit of a CadDoc: snapshots the tree, runs fn, regenerates; a
// failure (or a tree that no longer builds) restores the snapshot.
class CadEdit : public Action {
public:
    CadEdit(std::shared_ptr<CadDoc> doc, std::function<Result(CadDoc&)> fn) : doc_(std::move(doc)), fn_(std::move(fn)) {}
    Result apply() override;
    void revert() override;

private:
    struct Snapshot {
        std::string units;
        std::vector<Parameter> params;
        std::vector<Feature> features;
        std::map<std::string, int> counters;
        std::vector<Configuration> configs;
        std::string activeConfig;
    };
    std::shared_ptr<CadDoc> doc_;
    std::function<Result(CadDoc&)> fn_;
    Snapshot before_;
};

void registerCommands(Session& session);

}  // namespace wetwaresuite::cad
