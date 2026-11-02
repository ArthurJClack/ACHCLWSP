#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "wetwaresuite/cad/cad.hpp"
#include "wetwaresuite/core/cli.hpp"
#include "wetwaresuite/mcp/server.hpp"
#include "wetwaresuite/mesh/io.hpp"
#include "check.hpp"

using namespace wetwaresuite;
using namespace wetwaresuite::cad;

namespace {

constexpr double kPi = 3.14159265358979323846;

bool near(double a, double b, double tol = 1e-9) { return std::fabs(a - b) <= tol; }
bool within(double a, double b, double fraction) { return std::fabs(a - b) <= std::fabs(b) * fraction; }

// Area of a regular n-gon inscribed in radius r (what a polygon hole removes).
double ngon(double r, int n) { return 0.5 * n * r * r * std::sin(2 * kPi / n); }

double num(const Result& r, const std::string& key) {
    const Value* v = r.field(key);
    if (!v) return std::nan("");
    if (auto d = std::get_if<double>(v)) return *d;
    if (auto i = std::get_if<std::int64_t>(v)) return static_cast<double>(*i);
    return std::nan("");
}

Result run(Session& s, const std::string& line) {
    cli::Batch batch(s);
    Result r = batch.runLine(line);
    if (!r.ok) std::printf("  (%s -> %s)\n", line.c_str(), r.message.c_str());
    return r;
}

double volumeOf(Session& s, const std::string& doc = "cad1") { return num(run(s, "cad.info doc=" + doc), "volume"); }

void testExpressions() {
    auto lookup = [](const std::string& name) -> std::optional<double> {
        if (name == "width") return 40.0;
        if (name == "h") return 3.0;
        return std::nullopt;
    };
    CHECK(near(evaluate("1+2*3"), 7));
    CHECK(near(evaluate("(1+2)*3"), 9));
    CHECK(near(evaluate("-2^2"), -4));
    CHECK(near(evaluate("2^3^2"), 512));
    CHECK(near(evaluate("10 % 4"), 2));
    CHECK(near(evaluate("width/2+3", lookup), 23));
    CHECK(near(evaluate("-width", lookup), -40));
    CHECK(near(evaluate("sin(30)"), 0.5, 1e-12));
    CHECK(near(evaluate("cos(60)+tan(45)"), 1.5, 1e-12));
    CHECK(near(evaluate("sqrt(16)+abs(-2)"), 6));
    CHECK(near(evaluate("min(3, h, 7)+max(1,2)", lookup), 5));
    CHECK(near(evaluate("2*pi"), 2 * kPi));
    CHECK(near(evaluate("1.5e2 + .5"), 150.5));
    CHECK(near(evaluate("atan2(1,1)"), 45, 1e-12));
    CHECK(near(evaluate("round(2.6)+floor(1.9)+ceil(0.1)"), 5));
    bool threw = false;
    try { evaluate("1/0"); } catch (const ExprError&) { threw = true; }
    CHECK(threw);
    threw = false;
    try { evaluate("2 +"); } catch (const ExprError&) { threw = true; }
    CHECK(threw);
    threw = false;
    try { evaluate("nope*2", lookup); } catch (const ExprError& e) { threw = std::string(e.what()).find("nope") != std::string::npos; }
    CHECK(threw);
    threw = false;
    try { evaluate("system(1)"); } catch (const ExprError&) { threw = true; }
    CHECK(threw);
    threw = false;
    try { evaluate("(1+2"); } catch (const ExprError&) { threw = true; }
    CHECK(threw);
    auto ids = identifiers("width/2 + max(h, width) * pi");
    CHECK(ids.size() == 2 && ids[0] == "width" && ids[1] == "h");
    auto items = splitList(" 0,0, w, min(a,b) ;3 ");
    CHECK(items.size() == 5 && items[3] == "min(a,b)" && items[4] == "3");
    CHECK(validParamName("hole_d2") && !validParamName("2x") && !validParamName("sin") && !validParamName("pi"));
}

void testParamsAndVolumes() {
    Session s;
    registerCommands(s);
    CHECK(run(s, "cad.new units=mm").ok);
    CHECK(run(s, "cad.param_set doc=cad1 name=w value=60").ok);
    CHECK(run(s, "cad.param_set doc=cad1 name=d value=w/2").ok);
    CHECK(run(s, "cad.param_set doc=cad1 name=t value=5").ok);
    CHECK(run(s, "cad.param_set doc=cad1 name=hole value=6").ok);
    CHECK(!run(s, "cad.param_set doc=cad1 name=loop value=loop+1").ok);  // self reference
    CHECK(!run(s, "cad.param_set doc=cad1 name=sin value=2").ok);
    CHECK(run(s, "cad.box doc=cad1 name=plate width=w depth=d height=t").ok);
    CHECK(near(volumeOf(s), 60 * 30 * 5, 1e-6));

    // four through holes with a 2x2 pattern, plus one in the middle
    CHECK(run(s, "cad.hole doc=cad1 name=h1 target=plate diameter=hole x=8 y=8").ok);
    Result r = run(s, "cad.pattern_linear doc=cad1 target=h1 count=2 dx=w-16 count2=2 dy2=d-16");
    CHECK(r.ok && r.field("feature") && toText(*r.field("feature")) == "pattern_linear1");
    CHECK(run(s, "cad.hole doc=cad1 target=pattern_linear1 diameter=10 x=w/2 y=d/2").ok);
    double analytic = 60 * 30 * 5 - 4 * kPi * 9 * 5 - kPi * 25 * 5;
    double polygon = 60 * 30 * 5 - 4 * ngon(3, 64) * 5 - ngon(5, 64) * 5;
    double v = volumeOf(s);
    CHECK(within(v, analytic, 0.01));
    CHECK(within(v, polygon, 1e-6));
    Result info = run(s, "cad.info doc=cad1");
    CHECK(info.field("watertight") && std::get<bool>(*info.field("watertight")));
    CHECK(toText(*info.field("bodies")) == "hole1");

    // a param change regenerates everything and changes the volume
    CHECK(run(s, "cad.param_set doc=cad1 name=w value=80").ok);
    double v80 = volumeOf(s);
    double polygon80 = 80 * 40 * 5 - 4 * ngon(3, 64) * 5 - ngon(5, 64) * 5;
    CHECK(within(v80, polygon80, 1e-6));
    Result b = run(s, "cad.measure_bounds doc=cad1");
    CHECK(b.ok && toText(*b.field("size")) == "80,40,5");

    // section area at mid height
    Result sec = run(s, "cad.measure_section doc=cad1 z=t/2");
    CHECK(sec.ok && within(num(sec, "area"), polygon80 / 5, 1e-6) && num(sec, "loops") == 6);

    // mass of PLA, units mm
    Result m = run(s, "cad.measure_mass doc=cad1");
    CHECK(m.ok && within(num(m, "mass_g"), polygon80 / 1000 * 1.25, 1e-6));
    m = run(s, "cad.measure_mass doc=cad1 density=7.85");
    CHECK(m.ok && within(num(m, "mass_g"), polygon80 / 1000 * 7.85, 1e-6));

    // a param that's in use can't be deleted; an unused one can
    CHECK(!run(s, "cad.param_delete doc=cad1 name=hole").ok);
    CHECK(run(s, "cad.param_set doc=cad1 name=spare value=1").ok);
    CHECK(run(s, "cad.param_delete doc=cad1 name=spare").ok);

    // a value that breaks the model is refused and rolled back
    CHECK(!run(s, "cad.param_set doc=cad1 name=t value=-1").ok);
    CHECK(within(volumeOf(s), polygon80, 1e-6));
    CHECK(!run(s, "cad.feature_set doc=cad1 feature=plate param=width value=nothere").ok);
    CHECK(!run(s, "cad.feature_set doc=cad1 feature=plate param=colour value=red").ok);
    CHECK(within(volumeOf(s), polygon80, 1e-6));

    // feature_set: change the pattern count to 3 in X -> two more holes
    CHECK(run(s, "cad.feature_set doc=cad1 feature=pattern_linear1 values=\"count=3; dx=(w-16)/2\"").ok);
    double six = 80 * 40 * 5 - 6 * ngon(3, 64) * 5 - ngon(5, 64) * 5;
    CHECK(within(volumeOf(s), six, 1e-6));
    sec = run(s, "cad.measure_section doc=cad1 z=1");
    CHECK(num(sec, "loops") == 8);  // outline + 6 small holes + the middle one
}

void testPatternsAndBooleans() {
    Session s;
    registerCommands(s);
    run(s, "cad.new");
    // circular pattern of 6 pegs on a disc, unioned to it
    CHECK(run(s, "cad.cylinder doc=cad1 name=disc diameter=50 height=4").ok);
    CHECK(run(s, "cad.cylinder doc=cad1 name=peg diameter=4 height=6 x=20 z=4 segments=32").ok);
    CHECK(run(s, "cad.pattern_circular doc=cad1 name=pegs target=peg count=6").ok);
    Result sec = run(s, "cad.measure_section doc=cad1 z=7 feature=pegs");
    CHECK(sec.ok && num(sec, "loops") == 6);
    CHECK(within(num(sec, "area"), 6 * ngon(2, 32), 1e-6));
    CHECK(run(s, "cad.boolean doc=cad1 op=union a=disc b=pegs").ok);
    Result info = run(s, "cad.info doc=cad1");
    CHECK(toText(*info.field("bodies")) == "boolean1");
    CHECK(within(num(info, "volume"), ngon(25, 64) * 4 + 6 * ngon(2, 32) * 6, 1e-6));
    CHECK(std::get<bool>(*info.field("watertight")));

    // difference and intersection of two boxes
    Session t;
    registerCommands(t);
    run(t, "cad.new");
    run(t, "cad.box doc=cad1 name=a width=10 depth=10 height=10");
    run(t, "cad.box doc=cad1 name=b width=10 depth=10 height=10 x=5");
    CHECK(run(t, "cad.boolean doc=cad1 name=cut op=difference a=a b=b").ok);
    CHECK(near(volumeOf(t), 500, 1e-6));
    CHECK(run(t, "cad.feature_set doc=cad1 feature=cut param=op value=intersection").ok);
    CHECK(near(volumeOf(t), 500, 1e-6));
    CHECK(run(t, "cad.feature_set doc=cad1 feature=cut param=op value=union").ok);
    CHECK(near(volumeOf(t), 1500, 1e-6));
    CHECK(!run(t, "cad.feature_set doc=cad1 feature=cut param=op value=xor").ok);
    // suppressing the cutter keeps the first input
    CHECK(run(t, "cad.feature_set doc=cad1 feature=cut param=op value=difference").ok);
    CHECK(run(t, "cad.feature_suppress doc=cad1 feature=b").ok);
    CHECK(near(volumeOf(t), 1000, 1e-6));
    CHECK(run(t, "cad.feature_unsuppress doc=cad1 feature=b").ok);
    CHECK(near(volumeOf(t), 500, 1e-6));
    // suppressing the boolean leaves both boxes as separate bodies
    CHECK(run(t, "cad.feature_suppress doc=cad1 feature=cut").ok);
    CHECK(toText(*run(t, "cad.info doc=cad1").field("bodies")) == "a,b");
    // deleting a feature that's used is refused
    CHECK(!run(t, "cad.feature_delete doc=cad1 feature=a").ok);
    CHECK(run(t, "cad.feature_delete doc=cad1 feature=cut").ok);
    CHECK(run(t, "cad.feature_delete doc=cad1 feature=a").ok);
    // later features can't be referenced
    CHECK(!run(t, "cad.boolean doc=cad1 op=union a=b b=zzz").ok);

    // mirror and linear patterns of plain bodies
    Session u;
    registerCommands(u);
    run(u, "cad.new");
    run(u, "cad.box doc=cad1 name=blk width=2 depth=2 height=2 x=3");
    CHECK(run(u, "cad.mirror doc=cad1 target=blk plane=yz").ok);
    Result bb = run(u, "cad.measure_bounds doc=cad1");
    CHECK(toText(*bb.field("min")) == "-5,0,0" && near(volumeOf(u), 16, 1e-9));
    CHECK(run(u, "cad.pattern_linear doc=cad1 target=mirror1 count=4 dy=5").ok);
    CHECK(near(volumeOf(u), 64, 1e-9));
    bb = run(u, "cad.measure_bounds doc=cad1");
    CHECK(toText(*bb.field("max")) == "5,17,2");
    // overlapping copies are unioned, not double counted
    CHECK(run(u, "cad.feature_set doc=cad1 feature=pattern_linear1 param=dy value=1").ok);
    CHECK(near(volumeOf(u), 16 * 2.5, 1e-6));
    CHECK(std::get<bool>(*run(u, "cad.info doc=cad1").field("watertight")));
    // transform moves the result
    CHECK(run(u, "cad.transform doc=cad1 target=pattern_linear1 z=10 rz=90").ok);
    bb = run(u, "cad.measure_bounds doc=cad1");
    CHECK(toText(*bb.field("min")) == "-5,-5,10");
}

void testSketches() {
    Session s;
    registerCommands(s);
    run(s, "cad.new");
    run(s, "cad.param_set doc=cad1 name=a value=20");
    // square with a square hole
    CHECK(run(s, "cad.extrude doc=cad1 name=frame points=0,0,a,0,a,a,0,a hole_points=5,5,15,5,15,15,5,15 height=2").ok);
    CHECK(near(volumeOf(s), (400 - 100) * 2, 1e-6));
    CHECK(run(s, "cad.add_hole doc=cad1 feature=frame points=1,1,3,1,3,3,1,3").ok);
    CHECK(near(volumeOf(s), (400 - 104) * 2, 1e-6));
    // L-profile in the xz plane extruded along +Y
    CHECK(run(s, "cad.feature_delete doc=cad1 feature=frame").ok);
    CHECK(run(s, "cad.extrude doc=cad1 name=L plane=xz points=0,0,30,0,30,3,3,3,3,20,0,20 height=10").ok);
    Result bb = run(s, "cad.measure_bounds doc=cad1");
    CHECK(toText(*bb.field("size")) == "30,10,20");
    CHECK(near(volumeOf(s), (30 * 3 + 3 * 17) * 10, 1e-6));
    CHECK(run(s, "cad.feature_delete doc=cad1 feature=L").ok);
    // rounded rectangle, ring, slot
    CHECK(run(s, "cad.extrude_rect doc=cad1 width=20 depth=10 height=1 radius=2 segments=64").ok);
    CHECK(within(volumeOf(s), 200 - (4 - kPi) * 4, 1e-4));
    CHECK(run(s, "cad.feature_delete doc=cad1 feature=extrude_rect1").ok);
    CHECK(run(s, "cad.extrude_circle doc=cad1 diameter=20 inner_diameter=10 height=2 segments=128").ok);
    CHECK(within(volumeOf(s), (ngon(10, 128) - ngon(5, 128)) * 2, 1e-9));
    CHECK(run(s, "cad.feature_delete doc=cad1 feature=extrude_circle1").ok);
    CHECK(run(s, "cad.extrude_slot doc=cad1 length=30 width=10 height=1 segments=64").ok);
    CHECK(within(volumeOf(s), 20 * 10 + kPi * 25, 0.002));
    bb = run(s, "cad.measure_bounds doc=cad1");
    CHECK(toText(*bb.field("size")) == "30,10,1");
    CHECK(run(s, "cad.feature_delete doc=cad1 feature=extrude_slot1").ok);
    // revolve a rectangle into a tube
    CHECK(run(s, "cad.revolve doc=cad1 points=5,0,10,0,10,4,5,4 segments=128").ok);
    CHECK(within(volumeOf(s), (ngon(10, 128) - ngon(5, 128)) * 4, 1e-6));
    CHECK(run(s, "cad.feature_delete doc=cad1 feature=revolve1").ok);
    // primitives
    CHECK(run(s, "cad.sphere doc=cad1 radius=10").ok);
    CHECK(within(volumeOf(s), 4.0 / 3 * kPi * 1000, 0.02));
    CHECK(run(s, "cad.cone doc=cad1 d1=10 d2=0 height=9 x=40").ok);
    CHECK(run(s, "cad.torus doc=cad1 major=10 minor=2 x=-40").ok);
    CHECK(!run(s, "cad.cone doc=cad1 height=9").ok);
    CHECK(!run(s, "cad.cylinder doc=cad1 height=9").ok);
    CHECK(toText(*run(s, "cad.info doc=cad1").field("bodies")) == "sphere1,cone1,torus1");
}

void testHoles() {
    Session s;
    registerCommands(s);
    run(s, "cad.new");
    run(s, "cad.box doc=cad1 name=blk width=40 depth=40 height=10 center=true");
    // blind hole from the top, depth 4: entry z defaults to the top face
    CHECK(run(s, "cad.hole doc=cad1 name=h target=blk diameter=6 depth=4").ok);
    CHECK(within(volumeOf(s), 16000 - ngon(3, 64) * 4, 1e-6));
    Result sec = run(s, "cad.measure_section doc=cad1 z=4");
    CHECK(num(sec, "loops") == 2);
    sec = run(s, "cad.measure_section doc=cad1 z=0");
    CHECK(num(sec, "loops") == 1);
    // counterbore
    CHECK(run(s, "cad.feature_set doc=cad1 feature=h values=\"depth=0; counterbore_diameter=10; counterbore_depth=3\"").ok);
    CHECK(within(volumeOf(s), 16000 - ngon(3, 64) * 7 - ngon(5, 64) * 3, 1e-6));
    // countersink: 90 degrees from 12 down to 6 takes 3 mm
    CHECK(run(s, "cad.feature_set doc=cad1 feature=h values=\"counterbore_diameter=; counterbore_depth=; countersink_diameter=12\"").ok);
    double cone = 3.0 / 3 * (ngon(6, 64) + ngon(3, 64) + std::sqrt(ngon(6, 64) * ngon(3, 64)));
    CHECK(within(volumeOf(s), 16000 - ngon(3, 64) * 7 - cone, 1e-6));
    CHECK(!run(s, "cad.feature_set doc=cad1 feature=h param=counterbore_diameter value=10").ok);  // both
    // sideways hole along +x from the left face
    CHECK(run(s, "cad.hole doc=cad1 name=side target=h diameter=4 axis=x y=10").ok);
    sec = run(s, "cad.measure_section doc=cad1 z=0");
    CHECK(num(sec, "loops") == 3);  // the side hole splits the section in two, plus the vertical hole
    // mirror of a hole repeats the hole on the same body
    CHECK(run(s, "cad.mirror doc=cad1 target=side plane=xz").ok);
    Result info = run(s, "cad.info doc=cad1");
    CHECK(toText(*info.field("bodies")) == "mirror1" && std::get<bool>(*info.field("watertight")));
}

// The tutorial's bracket stays a clean closed solid as its parameters change.
void testBracket() {
    for (const char* width : {"40", "60"}) {
        for (const char* t : {"4", "6", "8"}) {
            Session s;
            registerCommands(s);
            cli::Batch b(s);
            const std::vector<std::string> lines = {
                "cad.new",
                "cad.param_set doc=cad1 name=width value=" + std::string(width),
                "cad.param_set doc=cad1 name=foot value=50",
                "cad.param_set doc=cad1 name=rise value=45",
                "cad.param_set doc=cad1 name=t value=" + std::string(t),
                "cad.param_set doc=cad1 name=bolt value=5.5",
                "cad.param_set doc=cad1 name=margin value=width/5",
                "cad.param_set doc=cad1 name=gusset value=min(foot,rise)/2",
                "cad.extrude doc=cad1 name=body plane=xz points=\"0,0, foot,0, foot,t, t,t, t,rise, 0,rise\" height=width",
                "cad.extrude doc=cad1 name=rib plane=xz points=\"t/2,t/2, t+gusset,t/2, t/2,t+gusset\" height=t y=(width-t)/2",
                "cad.boolean doc=cad1 name=bracket op=union a=body b=rib",
                "cad.hole doc=cad1 name=foot_hole target=bracket diameter=bolt x=(foot+t+gusset)/2 y=margin",
                "cad.pattern_linear doc=cad1 name=foot_holes target=foot_hole count=2 dy=width-2*margin",
                "cad.hole doc=cad1 name=wall_hole target=foot_holes diameter=bolt axis=x y=margin z=rise-margin "
                "countersink_diameter=bolt*2",
                "cad.pattern_linear doc=cad1 name=wall_holes target=wall_hole count=2 dy=width-2*margin"};
            for (const auto& line : lines) CHECK(b.runLine(line).ok);
            Result info = b.runLine("cad.info doc=cad1");
            CHECK(info.ok && std::get<bool>(*info.field("watertight")));
            CHECK(num(info, "triangles") < 3000);
            // the foot section has 2 holes at any width/thickness
            Result sec = b.runLine("cad.measure_section doc=cad1 z=t/2");
            CHECK(sec.ok && num(sec, "loops") == 3);
        }
    }
}

void testUndoRedo() {
    Session s;
    registerCommands(s);
    run(s, "cad.new");
    run(s, "cad.param_set doc=cad1 name=w value=10");
    run(s, "cad.box doc=cad1 name=b width=w depth=10 height=10");
    CHECK(near(volumeOf(s), 1000, 1e-9));
    run(s, "cad.param_set doc=cad1 name=w value=20");
    CHECK(near(volumeOf(s), 2000, 1e-9));
    CHECK(s.history().undo());
    CHECK(near(volumeOf(s), 1000, 1e-9));
    CHECK(s.history().redo());
    CHECK(near(volumeOf(s), 2000, 1e-9));
    run(s, "cad.feature_set doc=cad1 feature=b param=height value=w");
    CHECK(near(volumeOf(s), 4000, 1e-9));
    run(s, "cad.rename doc=cad1 feature=b to=block");
    run(s, "cad.hole doc=cad1 target=block diameter=2");
    CHECK(s.history().undo());
    CHECK(s.history().undo());
    auto doc = s.workspace().getAs<CadDoc>("cad1");
    CHECK(doc && doc->findFeature("b") && !doc->findFeature("block"));
    CHECK(s.history().undo());
    CHECK(near(volumeOf(s), 2000, 1e-9));
    CHECK(s.history().redo() && s.history().redo());
    CHECK(doc->findFeature("block"));
    run(s, "cad.feature_delete doc=cad1 feature=block");
    CHECK(doc->features.empty() && doc->mesh.empty());
    CHECK(s.history().undo());
    CHECK(doc->features.size() == 1 && near(volumeOf(s), 4000, 1e-9));
    run(s, "cad.close doc=cad1");
    CHECK(!s.workspace().get("cad1"));
    CHECK(s.history().undo());
    CHECK(s.workspace().getAs<CadDoc>("cad1"));
}

void testFiles() {
    const auto dir = std::filesystem::temp_directory_path() / "wetwaresuite_cad_test";
    std::filesystem::create_directories(dir);
    const std::string part = (dir / "part.wwcad").string(), stl = (dir / "part.stl").string(), png = (dir / "part.png").string();
    Session s;
    registerCommands(s);
    run(s, "cad.new units=in");
    run(s, "cad.param_set doc=cad1 name=w value=4");
    run(s, "cad.extrude_rect doc=cad1 name=plate width=w depth=w/2 height=0.25 radius=0.25");
    run(s, "cad.hole doc=cad1 target=plate diameter=0.2 x=0.5 y=0.5");
    run(s, "cad.pattern_circular doc=cad1 target=hole1 count=4 cx=w/2 cy=w/4 angle=360");
    run(s, "cad.feature_suppress doc=cad1 feature=pattern_circular1");
    run(s, "cad.feature_unsuppress doc=cad1 feature=pattern_circular1");
    CHECK(!run(s, "cad.import doc=cad1 path=nonexistent.stl").ok);
    const double v = volumeOf(s);
    CHECK(run(s, "cad.save doc=cad1 path=\"" + part + "\"").ok);
    CHECK(run(s, "cad.save doc=cad1 path=\"" + stl + "\"").ok);
    CHECK(run(s, "cad.save doc=cad1 path=\"" + png + "\" size=128 view=top").ok);
    CHECK(std::filesystem::file_size(png) > 100);
    CHECK(!run(s, "cad.save doc=cad1 path=part.xyz").ok);

    // .wwcad round trip keeps params, features and the model
    Result r = run(s, "cad.open path=\"" + part + "\"");
    CHECK(r.ok && toText(r.value) == "cad2");
    auto doc = s.workspace().getAs<CadDoc>("cad2");
    CHECK(doc && doc->units == "in" && doc->params.size() == 1 && doc->features.size() == 3);
    CHECK(within(volumeOf(s, "cad2"), v, 1e-9));
    CHECK(run(s, "cad.param_set doc=cad2 name=w value=6").ok);
    CHECK(volumeOf(s, "cad2") > v * 1.4);
    // automatic names continue after the loaded ones
    r = run(s, "cad.hole doc=cad2 target=pattern_circular1 diameter=0.1 x=1 y=1");
    CHECK(r.ok && toText(r.value) == "hole2");

    // exported STL loads back with the same volume
    mesh::Mesh m = mesh::load(stl);
    CHECK(within(mesh::volume(m), v, 1e-4) && mesh::isWatertight(m));
    // and opens in cad as an imported feature, which can be drilled
    r = run(s, "cad.open path=\"" + stl + "\"");
    CHECK(r.ok && toText(r.value) == "cad3");
    CHECK(within(volumeOf(s, "cad3"), v, 1e-4));
    CHECK(run(s, "cad.import doc=cad3 path=\"" + stl + "\" name=copy z=1").ok);
    CHECK(run(s, "cad.boolean doc=cad3 op=union a=imported1 b=copy").ok);
    CHECK(within(volumeOf(s, "cad3"), 2 * v, 1e-3));
    // imported meshes survive a save/open round trip
    const std::string part3 = (dir / "imported.wwcad").string();
    CHECK(run(s, "cad.save doc=cad3 path=\"" + part3 + "\"").ok);
    CHECK(run(s, "cad.open path=\"" + part3 + "\"").ok);
    CHECK(within(volumeOf(s, "cad4"), 2 * v, 1e-3));
    CHECK(!run(s, "cad.open path=\"" + (dir / "missing.wwcad").string() + "\"").ok);

    // saves carry the new format id; parts saved before the rename (.achcad, "achcad") still open
    std::string text;
    {
        std::ifstream in(part, std::ios::binary);
        text.assign(std::istreambuf_iterator<char>(in), {});
    }
    CHECK(text.find("\"format\": \"wwcad\"") != std::string::npos);
    std::string old;
    for (std::size_t i = 0; i < text.size(); ++i) {  // old name, CRLF line ends
        if (text.compare(i, 7, "\"wwcad\"") == 0) {
            old += "\"achcad\"";
            i += 6;
        } else {
            if (text[i] == '\n') old += '\r';
            old += text[i];
        }
    }
    const auto legacy = dir / "legacy.achcad";
    {
        std::ofstream out(legacy, std::ios::binary);
        out << old;
    }
    r = run(s, "cad.open path=\"" + legacy.string() + "\"");
    CHECK(r.ok);
    if (r.ok) CHECK(within(volumeOf(s, toText(r.value)), v, 1e-9));
    std::filesystem::remove_all(dir);
}

void testMcp() {
    Session session;
    registerCommands(session);
    mcp::Server server(session, "test", "0");
    auto call = [&](const std::string& tool, const std::string& args) {
        return server
            .handle(R"({"jsonrpc":"2.0","id":1,"method":"tools/call","params":{"name":")" + tool + R"(","arguments":)" + args + "}}")
            .value_or("");
    };
    std::string list = server.handle(R"({"jsonrpc":"2.0","id":1,"method":"tools/list"})").value_or("");
    for (const char* tool : {"cad_new", "cad_box", "cad_hole", "cad_pattern_circular", "cad_feature_set", "cad_measure_mass",
                             "cad_extrude_slot", "cad_param_set"}) {
        CHECK(list.find(std::string("\"") + tool + "\"") != std::string::npos);
    }
    std::string r = call("cad_new", R"({"units":"mm"})");
    CHECK(r.find("\"doc\":\"cad1\"") != std::string::npos);
    r = call("cad_param_set", R"({"doc":"cad1","name":"size","value":"20"})");
    CHECK(r.find("\"isError\":false") != std::string::npos);
    r = call("cad_box", R"({"doc":"cad1","width":"size","depth":"size","height":"size/4","center":true,"preview":true})");
    CHECK(r.find("\"type\":\"image\"") != std::string::npos);
    CHECK(r.find("\"feature\":\"box1\"") != std::string::npos);
    r = call("cad_preview", R"({"doc":"cad1","view":"top"})");
    CHECK(r.find("\"type\":\"image\"") != std::string::npos);
    r = call("undo", "{}");
    CHECK(r.find("undone") != std::string::npos);
    CHECK(session.workspace().getAs<CadDoc>("cad1")->features.empty());
    // plain numbers are accepted for expression fields too
    r = call("cad_box", R"({"doc":"cad1","name":"plate","width":12.25,"depth":10,"height":0.1})");
    CHECK(r.find("\"isError\":false") != std::string::npos);
    CHECK(near(volumeOf(session), 12.25 * 10 * 0.1, 1e-9));
}

}  // namespace

// Fillets, chamfers and shells: volumes against the exact shapes.
void testBlends() {
    Session s;
    registerCommands(s);
    auto watertight = [&](const std::string& doc) { return std::get<bool>(*run(s, "cad.info doc=" + doc).field("watertight")); };
    // a fully rounded box: its exact volume, within the polygon error
    run(s, "cad.new");
    run(s, "cad.box doc=cad1 name=b width=40 depth=30 height=10");
    Result edges = run(s, "cad.edges doc=cad1");
    CHECK(edges.ok && num(edges, "count") == 12);
    CHECK(run(s, "cad.fillet doc=cad1 name=f target=b radius=3").ok);
    const double r = 3, a = 40 - 2 * r, b = 30 - 2 * r, c = 10 - 2 * r;
    const double rounded = a * b * c + 2 * r * (a * b + b * c + a * c) + kPi * r * r * (a + b + c) + 4.0 / 3 * kPi * r * r * r;
    CHECK(within(volumeOf(s), rounded, 0.003) && watertight("cad1"));
    // vertical edges only
    CHECK(run(s, "cad.feature_set doc=cad1 feature=f values=\"edges=vertical; radius=5\"").ok);
    CHECK(within(volumeOf(s), 12000 - 4 * 25 * (1 - kPi / 4) * 10, 0.001));
    CHECK(!run(s, "cad.feature_set doc=cad1 feature=f param=edges value=sideways").ok);
    CHECK(!run(s, "cad.feature_set doc=cad1 feature=f param=radius value=40").ok);  // too big: refused and rolled back
    CHECK(within(volumeOf(s), 12000 - 4 * 25 * (1 - kPi / 4) * 10, 0.001));

    // one edge picked by a point near it: a 2 x 2 triangle along 20 mm comes off exactly
    run(s, "cad.new");
    run(s, "cad.box doc=cad2 name=cube width=20 depth=20 height=20");
    CHECK(run(s, "cad.chamfer doc=cad2 target=cube distance=2 near=20,10,20").ok);
    CHECK(near(volumeOf(s, "cad2"), 8000 - 2 * 20, 1e-6) && watertight("cad2"));

    // an inside corner gets filled: an L bracket
    run(s, "cad.new");
    run(s, "cad.extrude doc=cad3 name=l plane=xz points=\"0,0, 50,0, 50,6, 6,6, 6,40, 0,40\" height=30");
    CHECK(run(s, "cad.fillet doc=cad3 target=l radius=5 edges=inner").ok);
    CHECK(within(volumeOf(s, "cad3") - 15120, 30 * 25 * (1 - kPi / 4), 0.03) && watertight("cad3"));

    // round rims: a chamfered cylinder and a boss filleted into its base
    run(s, "cad.new");
    run(s, "cad.cylinder doc=cad4 name=cy diameter=30 height=20 segments=96");
    const double before = volumeOf(s, "cad4");
    CHECK(run(s, "cad.chamfer doc=cad4 target=cy distance=2").ok);
    CHECK(within(before - volumeOf(s, "cad4"), 2 * 2 * 2 * kPi * (15 - 2.0 / 3), 0.01) && watertight("cad4"));
    run(s, "cad.new");
    run(s, "cad.box doc=cad5 name=base width=60 depth=40 height=8");
    run(s, "cad.cylinder doc=cad5 name=boss diameter=20 height=15 x=30 y=20 z=8");
    run(s, "cad.boolean doc=cad5 name=u a=base b=boss");
    CHECK(run(s, "cad.fillet doc=cad5 name=inner target=u radius=3 edges=inner").ok);
    const double ring = 9 * (1 - kPi / 4) * 2 * kPi * (10 + 3 * (10 - 3 * kPi) / (3 * (4 - kPi)));  // Pappus
    CHECK(within(volumeOf(s, "cad5") - (19200 + kPi * 100 * 15), ring, 0.05) && watertight("cad5"));
    CHECK(run(s, "cad.fillet doc=cad5 target=inner radius=1 edges=top").ok && watertight("cad5"));

    // shells: a closed box and an open tray
    run(s, "cad.new");
    run(s, "cad.box doc=cad6 name=box width=50 depth=40 height=25");
    CHECK(run(s, "cad.shell doc=cad6 name=sh target=box thickness=2").ok);
    CHECK(near(volumeOf(s, "cad6"), 50000 - 46 * 36 * 21, 1e-4));
    CHECK(run(s, "cad.feature_set doc=cad6 feature=sh param=open value=top").ok);
    CHECK(near(volumeOf(s, "cad6"), 50000 - 46 * 36 * 23, 1e-4) && watertight("cad6"));
    CHECK(near(num(run(s, "cad.measure_section doc=cad6 z=10"), "loops"), 2));
    CHECK(!run(s, "cad.feature_set doc=cad6 feature=sh param=thickness value=30").ok);
    CHECK(!run(s, "cad.feature_set doc=cad6 feature=sh param=open value=upwards").ok);

    // STEP out and back in: the tray keeps its volume and stays closed
    const std::string step = (std::filesystem::temp_directory_path() / "wetwaresuite_cad_tray.step").string();
    CHECK(run(s, "cad.save doc=cad6 path=\"" + step + "\"").ok);
    Result opened = run(s, "cad.open path=\"" + step + "\"");
    CHECK(opened.ok);
    const std::string back = toText(*opened.field("doc"));
    CHECK(near(volumeOf(s, back), 50000 - 46 * 36 * 23, 1e-3) && watertight(back));

    // the tutorial's tray: its STEP comes back with long sliver faces, and a hole
    // drilled right on a sliver's diagonal still has to close
    const std::string t = toText(*run(s, "cad.new").field("doc"));
    run(s, "cad.extrude_rect doc=" + t + " name=block width=60 depth=40 height=25");
    run(s, "cad.fillet doc=" + t + " name=round target=block radius=4 edges=vertical");
    run(s, "cad.shell doc=" + t + " name=tray target=round thickness=2 open=top");
    run(s, "cad.chamfer doc=" + t + " name=bevel target=tray distance=1 edges=bottom");
    CHECK(run(s, "cad.fillet doc=" + t + " name=lip target=bevel radius=0.8 edges=top").ok && watertight(t));
    const double trayVolume = volumeOf(s, t);
    CHECK(run(s, "cad.save doc=" + t + " path=\"" + step + "\"").ok);
    opened = run(s, "cad.open path=\"" + step + "\"");
    const std::string drilled = toText(*opened.field("doc"));
    CHECK(within(volumeOf(s, drilled), trayVolume, 1e-6));
    CHECK(run(s, "cad.hole doc=" + drilled + " target=imported1 diameter=5 x=30 y=20").ok);
    CHECK(within(volumeOf(s, drilled), trayVolume - ngon(2.5, 64) * 2, 1e-6) && watertight(drilled));
    std::filesystem::remove(step);
}


// edge picking, offset face, configurations and sheet metal
void testGapClosers() {
    Session s;
    registerCommands(s);
    run(s, "cad.new");
    run(s, "cad.box doc=cad1 name=block width=40 depth=30 height=20");
    // a ray at the top-right edge (x=40, z=20, running along y)
    Result e = run(s, "cad.pick_edge doc=cad1 ray=60,15,40,-1,0,-1");
    CHECK(e.ok && e.field("hit") && toText(*e.field("hit")) == "true");
    CHECK(near(num(e, "length"), 30, 1e-6) && near(num(e, "edges"), 1));
    CHECK(e.field("outer") && toText(*e.field("outer")) == "true");
    const std::string mid = toText(*e.field("near"));
    CHECK(run(s, "cad.fillet doc=cad1 name=round target=block radius=3 near=" + mid).ok);
    CHECK(volumeOf(s) < 40 * 30 * 20 && volumeOf(s) > 40 * 30 * 20 - 30 * 9);
    // nothing there
    CHECK(run(s, "cad.pick_edge doc=cad1 ray=200,200,200,1,0,0").ok);

    // offset a curved face: a 10 mm boss grown to 12
    run(s, "cad.new");
    run(s, "cad.cylinder doc=cad2 name=boss diameter=10 height=20 segments=48");
    const double before = volumeOf(s, "cad2");
    CHECK(run(s, "cad.offset_face doc=cad2 name=grow target=boss face=5,0,10 distance=1").ok);
    CHECK(within(volumeOf(s, "cad2"), before * 1.44, 0.03));
    CHECK(!run(s, "cad.offset_face doc=cad2 target=boss face=500,0,0 distance=1").ok);

    // configurations: one tree, every size
    run(s, "cad.new");
    run(s, "cad.param_set doc=cad3 name=len value=30");
    run(s, "cad.box doc=cad3 name=bar width=len depth=10 height=10");
    CHECK(run(s, "cad.config_add doc=cad3 name=short values=\"len=10\"").ok);
    CHECK(run(s, "cad.config_add doc=cad3 name=long values=\"len=50; w=3\" use=true").ok);
    CHECK(near(volumeOf(s, "cad3"), 5000, 1e-6));
    CHECK(run(s, "cad.config_use doc=cad3 name=short").ok && near(volumeOf(s, "cad3"), 1000, 1e-6));
    CHECK(run(s, "undo").ok && near(volumeOf(s, "cad3"), 5000, 1e-6));
    Result list = run(s, "cad.config_list doc=cad3");
    CHECK(toText(*list.field("names")) == "short,long" && toText(*list.field("active")) == "long");
    const auto dir = std::filesystem::temp_directory_path() / "wetwaresuite_cad_config_test";
    std::filesystem::create_directories(dir);
    Result ex = run(s, "cad.config_export doc=cad3 path=\"" + (dir / "bar-{name}.stl").string() + "\"");
    CHECK(ex.ok && num(ex, "count") == 2);
    CHECK(std::filesystem::exists(dir / "bar-short.stl") && std::filesystem::exists(dir / "bar-long.stl"));
    CHECK(near(volumeOf(s, "cad3"), 5000, 1e-6));  // put back
    const auto saved = dir / "bar.wwcad";
    CHECK(run(s, "cad.save doc=cad3 path=\"" + saved.string() + "\"").ok);
    Result reopened = run(s, "cad.open path=\"" + saved.string() + "\"");
    CHECK(toText(*run(s, "cad.config_list doc=" + toText(*reopened.field("doc"))).field("names")) == "short,long");
    CHECK(!run(s, "cad.config_add doc=cad3 name=bad values=\"len\"").ok);
    CHECK(!run(s, "cad.config_use doc=cad3 name=nope").ok);

    // sheet metal: a 100 x 60 plate, 2 thick, a 20 mm flange bent up off the front
    const std::string sm = toText(*run(s, "cad.new").field("doc"));
    CHECK(run(s, "cad.sheet_base doc=" + sm + " name=plate width=100 depth=60 thickness=2 radius=2 k_factor=0.5").ok);
    CHECK(run(s, "cad.sheet_flange doc=" + sm + " name=lip target=plate edge=front length=20 angle=90").ok);
    const double bend = kPi / 4 * (16 - 4) * 100;
    CHECK(within(volumeOf(s, sm), 12000 + bend + 4000, 0.01));
    Result flat = run(s, "cad.sheet_flat doc=" + sm + " path=\"" + (dir / "flat.dxf").string() + "\"");
    CHECK(flat.ok && num(flat, "bends") == 1);
    CHECK(near(num(flat, "width"), 100, 1e-6) && near(num(flat, "height"), 60 + kPi / 2 * (2 + 0.5 * 2) + 20, 1e-6));
    CHECK(std::filesystem::exists(dir / "flat.dxf"));
    CHECK(!run(s, "cad.sheet_flat doc=cad1").ok);  // no sheet metal there
}

int main() {
    testExpressions();
    testParamsAndVolumes();
    testPatternsAndBooleans();
    testSketches();
    testHoles();
    testBlends();
    testBracket();
    testUndoRedo();
    testFiles();
    testMcp();
    testGapClosers();
    return check::summary("cad");
}
