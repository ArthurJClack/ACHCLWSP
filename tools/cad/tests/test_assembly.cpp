// Assemblies (insert, mates, solve, interference, explode, BOM, files) and
// drawings (projected views with hidden lines, exported to SVG, DXF and PDF).
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

#include "wetwaresuite/cad/assembly.hpp"
#include "wetwaresuite/cad/cad.hpp"
#include "wetwaresuite/core/cli.hpp"
#include "check.hpp"

using namespace wetwaresuite;
using namespace wetwaresuite::cad;

namespace {

constexpr double kPi = 3.14159265358979323846;

bool near(double a, double b, double tol = 1e-6) { return std::fabs(a - b) <= tol; }

double num(const Result& r, const std::string& key) {
    const Value* v = r.field(key);
    if (!v) return std::nan("");
    if (auto d = std::get_if<double>(v)) return *d;
    if (auto i = std::get_if<std::int64_t>(v)) return static_cast<double>(*i);
    return std::nan("");
}

NumberList list(const Result& r, const std::string& key) {
    const Value* v = r.field(key);
    if (!v) return {};
    if (auto l = std::get_if<NumberList>(v)) return *l;
    return {};
}

std::string text(const Result& r, const std::string& key) {
    const Value* v = r.field(key);
    return v ? toText(*v) : std::string();
}

Result run(Session& s, const std::string& line) {
    cli::Batch batch(s);
    Result r = batch.runLine(line);
    if (!r.ok) std::printf("  (%s -> %s)\n", line.c_str(), r.message.c_str());
    return r;
}

std::filesystem::path tmp() {
    auto d = std::filesystem::temp_directory_path() / "wetwaresuite_cad_assembly_test";
    std::filesystem::create_directories(d);
    return d;
}

std::string slurp(const std::filesystem::path& p) {
    std::ifstream in(p, std::ios::binary);
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

mesh::Bounds componentBounds(Session& s, const std::string& asmId, const std::string& comp, bool exploded = false) {
    auto doc = s.workspace().getAs<AssemblyDoc>(asmId);
    if (!doc) return {};
    const Component* c = doc->findComponent(comp);
    return c ? mesh::bounds(doc->componentMesh(*c, exploded)) : mesh::Bounds{};
}

// cad1: a 40x40x10 plate with a 10 mm hole in the middle; cad2: a 10 mm peg, 30 long
void makeParts(Session& s) {
    run(s, "cad.new");
    run(s, "cad.box doc=cad1 name=plate width=40 depth=40 height=10");
    run(s, "cad.hole doc=cad1 name=h1 target=plate diameter=10 x=20 y=20");
    run(s, "cad.new");
    run(s, "cad.param_set doc=cad2 name=d value=10");
    run(s, "cad.cylinder doc=cad2 name=pin diameter=d height=30 segments=48");
}

void testMates() {
    Session s;
    registerCommands(s);
    makeParts(s);
    CHECK(run(s, "cad.assembly_new").ok);
    Result r = run(s, "cad.assembly_insert doc=asm1 part=cad1 name=base");
    CHECK(r.ok && num(r, "dof") == 0);  // the first component is fixed
    r = run(s, "cad.assembly_insert doc=asm1 part=cad2 name=peg x=100 y=-30 z=7 rx=25 ry=-40");
    CHECK(r.ok && num(r, "dof") == 6 && text(r, "free") == "peg");
    // the peg's axis on the hole's axis: 2 degrees of freedom are left (slide and spin)
    r = run(s, "cad.mate doc=asm1 type=concentric a=base/h1 b=peg/pin");
    CHECK(r.ok && num(r, "dof") == 2);
    // its bottom face on the plate's top face
    r = run(s, "cad.mate doc=asm1 type=coincident a=base/top b=peg/bottom");
    CHECK(r.ok && num(r, "dof") == 1);
    mesh::Bounds b = componentBounds(s, "asm1", "peg");
    CHECK(near(b.min.z, 10, 1e-6) && near(b.max.z, 40, 1e-6));
    CHECK(near((b.min.x + b.max.x) / 2, 20, 1e-6) && near((b.min.y + b.max.y) / 2, 20, 1e-6));
    // a distance mate instead lifts it off the plate; values can be assembly parameters
    run(s, "cad.mate_delete doc=asm1 mate=mate2");
    CHECK(run(s, "cad.assembly_param doc=asm1 name=gap value=5").ok);
    CHECK(run(s, "cad.mate doc=asm1 type=distance a=base/top b=peg/bottom value=gap name=lift").ok);
    CHECK(near(componentBounds(s, "asm1", "peg").min.z, 15, 1e-6));
    run(s, "cad.assembly_param doc=asm1 name=gap value=2");
    CHECK(near(componentBounds(s, "asm1", "peg").min.z, 12, 1e-6));
    CHECK(run(s, "cad.mate_set doc=asm1 mate=lift value=0").ok);
    CHECK(near(componentBounds(s, "asm1", "peg").min.z, 10, 1e-6));
    // a conflicting mate is refused and says which mates fight
    r = run(s, "cad.mate doc=asm1 type=distance a=base/top b=peg/bottom value=8");
    CHECK(!r.ok);
    // undo puts it back where it was
    CHECK(run(s, "cad.assembly_move doc=asm1 component=peg z=50 relative=true").ok);
    CHECK(near(componentBounds(s, "asm1", "peg").min.z, 10, 1e-6));  // mates still hold it
    CHECK(run(s, "cad.mate_delete doc=asm1 mate=lift").ok);
    CHECK(run(s, "cad.assembly_move doc=asm1 component=peg z=50 relative=true").ok);
    CHECK(near(componentBounds(s, "asm1", "peg").min.z, 60, 1e-6));
    CHECK(run(s, "undo").ok);
    CHECK(near(componentBounds(s, "asm1", "peg").min.z, 10, 1e-6));

    // fastened locks everything
    r = run(s, "cad.mate doc=asm1 type=fastened a=base/origin b=peg/origin");
    CHECK(r.ok && num(r, "dof") == 0);
    Result info = run(s, "cad.assembly_info doc=asm1");
    CHECK(info.ok && text(info, "components") == "base,peg" && num(info, "dof") == 0);

    // angle: an arm lying on the plate, pinned at a corner, turned 30 degrees
    Session t;
    registerCommands(t);
    run(t, "cad.new");
    run(t, "cad.box doc=cad1 name=plate width=40 depth=40 height=10");
    run(t, "cad.new");
    run(t, "cad.box doc=cad2 name=bar width=20 depth=2 height=2");
    run(t, "cad.assembly_new");
    run(t, "cad.assembly_insert doc=asm1 part=cad1 name=plate");
    run(t, "cad.assembly_insert doc=asm1 part=cad2 name=arm x=5 y=60 z=3 rz=70");
    CHECK(run(t, "cad.mate doc=asm1 type=coincident a=plate/top b=arm/bottom").ok);
    CHECK(run(t, "cad.mate doc=asm1 type=coincident a=plate/@0,0,10 b=arm/origin").ok);
    r = run(t, "cad.mate doc=asm1 type=angle a=plate/front b=arm/front value=30");
    CHECK(r.ok && num(r, "dof") == 0);
    b = componentBounds(t, "asm1", "arm");
    const double c30 = std::cos(kPi / 6), s30 = std::sin(kPi / 6);
    CHECK(near(b.max.x - b.min.x, 20 * c30 + 2 * s30, 1e-6) && near(b.max.y - b.min.y, 20 * s30 + 2 * c30, 1e-6));
    CHECK(near(b.min.z, 10, 1e-6));
    // parallel and perpendicular
    run(t, "cad.mate_delete doc=asm1 mate=mate3");
    CHECK(run(t, "cad.mate doc=asm1 type=perpendicular a=plate/front b=arm/front").ok);
    b = componentBounds(t, "asm1", "arm");
    CHECK(near(b.max.x - b.min.x, 2, 1e-6) && near(b.max.y - b.min.y, 20, 1e-6));
    // mates to unknown faces fail clearly
    CHECK(!run(t, "cad.mate doc=asm1 type=coincident a=plate/nowhere b=arm/top").ok);
    CHECK(!run(t, "cad.mate doc=asm1 type=coincident a=ghost/top b=arm/top").ok);
}

void testInterferenceBomExplode() {
    Session s;
    registerCommands(s);
    makeParts(s);
    run(s, "cad.new");
    run(s, "cad.box doc=cad3 name=blk width=10 depth=10 height=10");
    run(s, "cad.assembly_new");
    run(s, "cad.assembly_insert doc=asm1 part=cad1 name=base");
    run(s, "cad.assembly_insert doc=asm1 part=cad2 name=peg1 x=5 y=5 z=10 fixed=true");   // sits on top
    run(s, "cad.assembly_insert doc=asm1 part=cad2 name=peg2 x=35 y=5 z=10 fixed=true");
    Result r = run(s, "cad.assembly_interference doc=asm1");
    CHECK(r.ok && num(r, "count") == 0);  // touching is not interfering
    // a block sunk 5 mm into the plate's corner
    run(s, "cad.assembly_insert doc=asm1 part=cad3 name=blk x=35 y=35 z=5 fixed=true");
    r = run(s, "cad.assembly_interference doc=asm1");
    CHECK(r.ok && num(r, "count") == 1 && near(num(r, "volume"), 125, 1e-3) && text(r, "pairs") == "base,blk");

    // bill of materials: three parts, four components
    const auto csv = tmp() / "bom.csv";
    r = run(s, "cad.assembly_bom doc=asm1 density=1 path=\"" + csv.string() + "\"");
    CHECK(r.ok && num(r, "items") == 3 && num(r, "quantity") == 4);
    const std::string t = slurp(csv);
    CHECK(t.rfind("item,part,quantity", 0) == 0 && t.find(",cad2,2,") != std::string::npos);
    const double pinVolume = 0.5 * 48 * 25 * std::sin(2 * kPi / 48) * 30;
    CHECK(num(r, "mass_g") > 0);
    {
        auto doc = s.workspace().getAs<AssemblyDoc>("asm1");
        CHECK(doc && near(std::fabs(mesh::volume(*doc->findPart("cad2")->mesh)), pinVolume, 1e-6));
    }

    // the part changes; update pulls it in
    run(s, "cad.param_set doc=cad2 name=d value=6");
    CHECK(run(s, "cad.assembly_update doc=asm1").ok);
    {
        auto doc = s.workspace().getAs<AssemblyDoc>("asm1");
        CHECK(doc && near(std::fabs(mesh::volume(*doc->findPart("cad2")->mesh)), 0.5 * 48 * 9 * std::sin(2 * kPi / 48) * 30, 1e-6));
    }

    // exploded: components move apart, the assembled positions stay
    const mesh::Bounds before = componentBounds(s, "asm1", "blk");
    CHECK(run(s, "cad.assembly_explode doc=asm1 factor=1 axis=+z").ok);
    const mesh::Bounds after = componentBounds(s, "asm1", "blk", true);
    CHECK(after.min.z > before.min.z + 1 && near(componentBounds(s, "asm1", "blk").min.z, before.min.z));
    {
        auto doc = s.workspace().getAs<AssemblyDoc>("asm1");
        CHECK(doc && mesh::bounds(doc->mesh).max.z > 40 + 1);
    }
    const auto stl = tmp() / "exploded.stl";
    CHECK(run(s, "cad.assembly_save doc=asm1 path=\"" + stl.string() + "\"").ok && std::filesystem::file_size(stl) > 0);
    CHECK(run(s, "cad.assembly_explode doc=asm1 factor=0").ok);
    // removing a component drops its mates and its BOM line
    CHECK(run(s, "cad.assembly_remove doc=asm1 component=blk").ok);
    CHECK(num(run(s, "cad.assembly_bom doc=asm1"), "items") == 2);
    CHECK(run(s, "cad.assembly_preview doc=asm1 size=64").ok);
}

void testFiles() {
    const auto path = tmp() / "pegged.wwasm";
    {
        Session s;
        registerCommands(s);
        makeParts(s);
        run(s, "cad.assembly_new");
        run(s, "cad.assembly_insert doc=asm1 part=cad1 name=base");
        run(s, "cad.assembly_insert doc=asm1 part=cad2 name=peg x=80");
        run(s, "cad.mate doc=asm1 type=concentric a=base/h1 b=peg/pin");
        run(s, "cad.mate doc=asm1 type=distance a=base/top b=peg/bottom value=3");
        CHECK(run(s, "cad.assembly_save doc=asm1 path=\"" + path.string() + "\"").ok);
    }
    // a fresh session: the parts travel inside the file
    Session s;
    registerCommands(s);
    Result r = run(s, "cad.assembly_open path=\"" + path.string() + "\"");
    CHECK(r.ok);
    Result info = run(s, "cad.assembly_info doc=asm1");
    CHECK(text(info, "components") == "base,peg" && num(info, "mates") == 2 && num(info, "dof") == 1);
    CHECK(near(componentBounds(s, "asm1", "peg").min.z, 13, 1e-6));
    CHECK(run(s, "cad.mate_set doc=asm1 mate=mate2 value=0").ok);
    CHECK(near(componentBounds(s, "asm1", "peg").min.z, 10, 1e-6));
    CHECK(!run(s, "cad.assembly_open path=\"" + (tmp() / "missing.wwasm").string() + "\"").ok);
}

void testDrawings() {
    Session s;
    registerCommands(s);
    makeParts(s);
    // the plate's hole is hidden in the front and right views
    const auto svg = tmp() / "plate.svg", dxf = tmp() / "plate.dxf", pdf = tmp() / "plate.pdf";
    Result r = run(s, "cad.drawing doc=cad1 path=\"" + svg.string() + "\" title=Plate author=test");
    CHECK(r.ok && num(r, "visible") > 0 && num(r, "hidden") > 0 && num(r, "dimensions") >= 3);
    const std::string t = slurp(svg);
    CHECK(t.find("<svg") != std::string::npos && t.find("Plate") != std::string::npos);
    CHECK(run(s, "cad.drawing doc=cad1 path=\"" + dxf.string() + "\" scale=1:1 paper=A4").ok);
    const std::string d = slurp(dxf);
    CHECK(d.find("HIDDEN") != std::string::npos && d.find("VISIBLE") != std::string::npos && d.find("EOF") != std::string::npos);
    CHECK(run(s, "cad.drawing doc=cad1 path=\"" + pdf.string() + "\" projection=first").ok);
    CHECK(slurp(pdf).rfind("%PDF", 0) == 0);
    // no hidden lines when asked; a solid without hidden edges has none
    r = run(s, "cad.drawing doc=cad1 hidden=false");
    CHECK(r.ok && num(r, "hidden") == 0);
    r = run(s, "cad.drawing doc=cad2 views=front");
    CHECK(r.ok && num(r, "hidden") == 0 && num(r, "visible") > 0);
    // the drawing is a blueprint document that stays open
    CHECK(!text(r, "drawing").empty() && s.workspace().get(text(r, "drawing")) != nullptr);
    // an assembly drawing has a parts list
    run(s, "cad.assembly_new");
    run(s, "cad.assembly_insert doc=asm1 part=cad1 name=base");
    run(s, "cad.assembly_insert doc=asm1 part=cad2 name=peg z=10 x=20 y=20 fixed=true");
    const auto asvg = tmp() / "assembly.svg";
    r = run(s, "cad.drawing doc=asm1 path=\"" + asvg.string() + "\"");
    CHECK(r.ok && num(r, "visible") > 0);
    CHECK(slurp(asvg).find("cad2") != std::string::npos);
    CHECK(!run(s, "cad.drawing doc=cad1 views=sideways").ok);
    CHECK(!run(s, "cad.drawing doc=nothing").ok);
}


// limits stop a slider, gears turn together, and dragging moves the rest
void testLimitsGearsDrag() {
    Session s;
    registerCommands(s);
    makeParts(s);
    run(s, "cad.assembly_new");
    run(s, "cad.assembly_insert doc=asm1 part=cad1 name=base");
    run(s, "cad.assembly_insert doc=asm1 part=cad2 name=peg x=100");
    run(s, "cad.mate doc=asm1 type=concentric a=base/h1 b=peg/pin");
    // a slot: the peg's bottom can sit 0 to 5 above the plate
    Result r = run(s, "cad.mate doc=asm1 type=distance a=base/top b=peg/bottom min=0 max=5 name=travel");
    CHECK(r.ok && text(r, "limits_hit") == "travel");
    CHECK(near(componentBounds(s, "asm1", "peg").min.z, 10, 1e-6));
    r = run(s, "cad.assembly_move doc=asm1 component=peg z=3 relative=true");
    CHECK(r.ok && text(r, "limits_hit").empty() && near(componentBounds(s, "asm1", "peg").min.z, 13, 1e-6));
    r = run(s, "cad.assembly_move doc=asm1 component=peg z=50 relative=true");
    CHECK(r.ok && text(r, "limits_hit") == "travel" && near(componentBounds(s, "asm1", "peg").min.z, 15, 1e-6));
    CHECK(run(s, "cad.mate_set doc=asm1 mate=travel max=20").ok);
    CHECK(!run(s, "cad.mate doc=asm1 type=distance a=base/top b=peg/bottom min=5 max=1").ok);
    CHECK(!run(s, "cad.mate doc=asm1 type=distance a=base/top b=peg/bottom").ok);

    // two gears on a plate, the second turning twice as fast the other way
    Session g;
    registerCommands(g);
    run(g, "cad.new");
    run(g, "cad.box doc=cad1 name=plate width=80 depth=40 height=10");
    run(g, "cad.hole doc=cad1 name=h1 target=plate diameter=6 x=20 y=20");
    run(g, "cad.hole doc=cad1 name=h2 target=plate diameter=6 x=50 y=20");
    run(g, "cad.new");
    run(g, "cad.cylinder doc=cad2 name=wheel diameter=20 height=5 segments=24");
    run(g, "cad.box doc=cad2 name=tooth width=2 depth=2 height=5 x=9 y=-1");
    run(g, "cad.boolean doc=cad2 name=gear op=union a=wheel b=tooth");
    run(g, "cad.assembly_new");
    run(g, "cad.assembly_insert doc=asm1 part=cad1 name=plate");
    run(g, "cad.assembly_insert doc=asm1 part=cad2 name=g1 x=-40");
    run(g, "cad.assembly_insert doc=asm1 part=cad2 name=g2 x=90");
    for (const char* n : {"1", "2"}) {
        CHECK(run(g, std::string("cad.mate doc=asm1 type=concentric a=plate/h") + n + " b=g" + n + "/wheel").ok);
        CHECK(run(g, std::string("cad.mate doc=asm1 type=coincident a=plate/top b=g") + n + "/bottom").ok);
    }
    r = run(g, "cad.mate doc=asm1 type=gear a=g1/wheel b=g2/wheel value=2");
    CHECK(r.ok && num(r, "dof") == 1);
    auto turn = [&](const std::string& comp) {
        auto doc = g.workspace().getAs<AssemblyDoc>("asm1");
        const mesh::Vec3 x = doc->findComponent(comp)->pose.direction({1, 0, 0});
        return std::atan2(x.y, x.x) * 180 / kPi;
    };
    const double a1 = turn("g1"), a2 = turn("g2");
    r = run(g, "cad.assembly_move doc=asm1 component=g1 rz=10 relative=true drag=true");
    CHECK(r.ok);
    CHECK(near(turn("g1") - a1, 10, 1e-4) && near(turn("g2") - a2, -20, 1e-4));
    // the turning adds up drag by drag
    run(g, "cad.assembly_move doc=asm1 component=g1 rz=10 relative=true drag=true");
    CHECK(near(turn("g1") - a1, 20, 1e-4) && near(turn("g2") - a2, -40, 1e-4));
    // and still holds after a save and open
    const auto path = tmp() / "gears.wwasm";
    CHECK(run(g, "cad.assembly_save doc=asm1 path=\"" + path.string() + "\"").ok);
    CHECK(slurp(path).find("\"gear\"") != std::string::npos);
    CHECK(!run(g, "cad.mate doc=asm1 type=gear a=g1/top b=g2/wheel").ok);
}

void testSectionDetail() {
    Session s;
    registerCommands(s);
    makeParts(s);
    const auto dxf = tmp() / "section.dxf";
    Result r = run(s, "cad.drawing doc=cad1 section=x=20 detail=front:20,5,8 path=\"" + dxf.string() + "\"");
    CHECK(r.ok);
    const std::string d = slurp(dxf);
    CHECK(d.find("SECTION A-A") != std::string::npos && d.find("DETAIL B") != std::string::npos);
    CHECK(d.find("SECTION") != std::string::npos);
    CHECK(!run(s, "cad.drawing doc=cad1 section=w=3").ok);
}

}  // namespace

int main() {
    testMates();
    testInterferenceBomExplode();
    testFiles();
    testDrawings();
    testLimitsGearsDrag();
    testSectionDetail();
    return check::summary("cad_assembly");
}
