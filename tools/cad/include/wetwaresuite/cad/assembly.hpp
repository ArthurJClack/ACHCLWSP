// Assemblies: parts (cad documents or mesh files) placed as components and
// held together by mates (coincident, concentric, distance, angle, parallel,
// perpendicular, fastened, gear, rack). Distance and angle mates can be limits
// (min/max) instead of fixed values. Mates are solved numerically for the components'
// positions; the first component is fixed by default. The assembled model is
// a mesh document, so previews, exports and drawings work on it as on a part.
#pragma once

#include <filesystem>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "wetwaresuite/cad/cad.hpp"

namespace wetwaresuite::cad {

struct AssemblyPart {
    std::string name;       // e.g. bracket
    std::string source;     // file it came from, may be empty
    std::string sourceDoc;  // workspace id it was copied from, may be empty
    std::string json;       // its .wwcad tree (cad parts); empty for plain meshes
    std::shared_ptr<const CadDoc> doc;      // built part (cad parts), for its datums, holes and faces
    std::shared_ptr<const mesh::Mesh> mesh;  // its solid in its own coordinates
};

struct Component {
    std::string name;  // bracket1
    std::string part;  // AssemblyPart::name
    mesh::Mat4 pose;   // part -> assembly
    bool fixed = false;
};

// a and b are "component/reference": a face (top, bottom, front, back, left,
// right, +x ..., or a point x,y,z near it), a part plane (xy, xz, yz), axis
// (x, y, z), origin, a point (@x,y,z), or a feature of the part (a datum
// plane or axis, a sketch's plane, a hole's / cylinder's axis; hole1#2 for the
// second hole of a pattern).
struct Mate {
    std::string name;
    std::string type;  // coincident, concentric, distance, angle, parallel, perpendicular, fastened, gear, rack
    std::string a, b;
    std::string value;  // distance / angle: an expression over the assembly's parameters; gear: turns of b per
                        // turn of a; rack: distance b slides per radian a turns (the pinion's pitch radius)
    std::string min, max;  // distance / angle: limits instead of (or as well as) a value; empty = none
    bool flip = false;  // planes: face the same way instead of opposite; gear / rack: turn the same way
    std::vector<double> lock;  // fastened: the relative pose when it was made; gear / rack: both poses at the last solve
};

const std::vector<std::string>& mateTypes();

class AssemblyDoc : public mesh::MeshDoc {
public:
    std::string units = "mm";
    std::vector<Parameter> params;
    std::vector<AssemblyPart> parts;
    std::vector<Component> components;
    std::vector<Mate> mates;
    std::map<std::string, int> counters;
    // exploded view: 0 = assembled; components move apart along `explodeAxis`
    // ("radial" = away from the centre) by factor x their size
    double explode = 0;
    std::string explodeAxis = "radial";

    // after solve()
    int dof = 0;
    int redundant = 0;
    std::vector<std::string> freeComponents;
    std::vector<std::string> limitsHit;  // limit mates holding at a bound after solve()
    // the next solve() holds this component where it is and moves the rest (dragging); cleared by solve()
    std::string hold;

    std::string kind() const override { return "assembly"; }
    std::string summary() const override;

    const AssemblyPart* findPart(const std::string& name) const;
    Component* findComponent(const std::string& name);
    const Component* findComponent(const std::string& name) const;
    Mate* findMate(const std::string& name);
    std::string newName(const std::string& base);
    double eval(const std::string& expr) const;

    // Solves the mates (moving free components), then rebuilds the mesh.
    // Throws std::runtime_error naming the mates that can't all hold.
    void solve();
    void rebuildMesh();
    mesh::Mesh componentMesh(const Component& c, bool exploded = false) const;
};

// Builds a part from a cad document (its tree is copied) or loads a file.
AssemblyPart partFromDoc(const CadDoc& doc, const std::string& name, const std::string& sourceDoc);
AssemblyPart partFromFile(const std::filesystem::path& path, const std::string& name);  // throws

std::string assemblyToJson(const AssemblyDoc& doc);
std::shared_ptr<AssemblyDoc> assemblyFromJson(const std::string& text);  // throws std::runtime_error

class AssemblyEdit : public Action {
public:
    AssemblyEdit(std::shared_ptr<AssemblyDoc> doc, std::function<Result(AssemblyDoc&)> fn) : doc_(std::move(doc)), fn_(std::move(fn)) {}
    Result apply() override;
    void revert() override;

private:
    struct Snapshot {
        std::vector<Parameter> params;
        std::vector<AssemblyPart> parts;
        std::vector<Component> components;
        std::vector<Mate> mates;
        std::map<std::string, int> counters;
        double explode = 0;
        std::string explodeAxis;
    };
    std::shared_ptr<AssemblyDoc> doc_;
    std::function<Result(AssemblyDoc&)> fn_;
    Snapshot before_;
};

}  // namespace wetwaresuite::cad
