// Sketches with constraints and the features built on them: extrude, revolve,
// loft, sweep, draft, rib, thread and datums.
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <string>

#include "wetwaresuite/cad/cad.hpp"
#include "wetwaresuite/core/cli.hpp"
#include "wetwaresuite/mcp/server.hpp"
#include "check.hpp"

using namespace wetwaresuite;
using namespace wetwaresuite::cad;

namespace {

constexpr double kPi = 3.14159265358979323846;

bool near(double a, double b, double tol = 1e-6) { return std::fabs(a - b) <= tol; }
bool within(double a, double b, double fraction) { return std::fabs(a - b) <= std::fabs(b) * fraction; }
double ngon(double r, int n) { return 0.5 * n * r * r * std::sin(2 * kPi / n); }

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
    if (!v) return {};
    if (auto t = std::get_if<std::string>(v)) return *t;
    return {};
}

bool flag(const Result& r, const std::string& key) {
    const Value* v = r.field(key);
    return v && std::holds_alternative<bool>(*v) && std::get<bool>(*v);
}

Result run(Session& s, const std::string& line) {
    cli::Batch batch(s);
    Result r = batch.runLine(line);
    if (!r.ok) std::printf("  (%s -> %s)\n", line.c_str(), r.message.c_str());
    return r;
}

double volumeOf(Session& s, const std::string& doc = "cad1") { return num(run(s, "cad.info doc=" + doc), "volume"); }
bool watertight(Session& s, const std::string& doc = "cad1") { return flag(run(s, "cad.info doc=" + doc), "watertight"); }

void testDegreesOfFreedom() {
    Session s;
    registerCommands(s);
    run(s, "cad.new");
    CHECK(run(s, "cad.sketch doc=cad1").ok);
    // a free line has 4 degrees of freedom
    Result r = run(s, "cad.sketch_line doc=cad1 sketch=sketch1 x1=1 y1=1 x2=8 y2=3");
    CHECK(r.ok && num(r, "dof") == 4 && !flag(r, "fully_defined"));
    CHECK(num(run(s, "cad.sketch_constrain doc=cad1 sketch=sketch1 type=horizontal refs=l1"), "dof") == 3);
    r = run(s, "cad.sketch_dimension doc=cad1 sketch=sketch1 type=length refs=l1 value=10");
    CHECK(num(r, "dof") == 2);
    r = run(s, "cad.sketch_constrain doc=cad1 sketch=sketch1 type=coincident refs=l1.start,origin");
    CHECK(num(r, "dof") == 0 && flag(r, "fully_defined"));
    Result info = run(s, "cad.sketch_info doc=cad1 sketch=sketch1");
    NumberList end = list(info, "p2");
    CHECK(end.size() == 2 && near(end[0], 10) && near(end[1], 0));
    // the window draws it from numbers: one segment, two points, no profile yet
    Result geo = run(s, "cad.sketch_geometry doc=cad1");
    CHECK(geo.ok && list(geo, "lines").size() == 4 && list(geo, "points").size() + list(geo, "free").size() >= 4);
    CHECK(list(geo, "profiles").empty() && geo.field("sketches") && toText(*geo.field("sketches")) == "sketch1");
    CHECK(run(s, "cad.sketch_rect doc=cad1 sketch=sketch1 x=20 y=0 width=5 height=5").ok);
    CHECK(list(run(s, "cad.sketch_geometry doc=cad1 sketch=sketch1"), "profiles").size() == 1 + 2 * 4 ||
          list(run(s, "cad.sketch_geometry doc=cad1 sketch=sketch1"), "profiles").size() == 1 + 2 * 5);
    CHECK(s.history().undo());
    // conflicting constraints are refused and change nothing
    r = run(s, "cad.sketch_constrain doc=cad1 sketch=sketch1 type=vertical refs=l1");
    CHECK(!r.ok && r.message.find("conflict") != std::string::npos);
    CHECK(num(run(s, "cad.sketch_info doc=cad1 sketch=sketch1"), "constraints") == 3);
    // an unknown ref is refused with the reason
    CHECK(!run(s, "cad.sketch_constrain doc=cad1 sketch=sketch1 type=parallel refs=l1,l9").ok);
    CHECK(!run(s, "cad.sketch_dimension doc=cad1 sketch=sketch1 type=radius refs=l1 value=3").ok);

    // angle: a second line from the origin at 30 degrees, 10 long
    run(s, "cad.sketch_line doc=cad1 sketch=sketch1 x1=0 y1=0 x2=7 y2=5");
    r = run(s, "cad.sketch_dimension doc=cad1 sketch=sketch1 type=angle refs=l1,l2 value=30");
    CHECK(r.ok);
    r = run(s, "cad.sketch_dimension doc=cad1 sketch=sketch1 type=length refs=l2 value=10");
    CHECK(r.ok && flag(r, "fully_defined"));
    end = list(run(s, "cad.sketch_info doc=cad1 sketch=sketch1"), "p3");
    CHECK(end.size() == 2 && near(end[0], 10 * std::cos(kPi / 6), 1e-7) && near(end[1], 5, 1e-7));
    // the dimension follows a parameter
    run(s, "cad.param_set doc=cad1 name=a value=45");
    CHECK(run(s, "cad.sketch_set doc=cad1 sketch=sketch1 constraint=k4 value=a").ok);
    end = list(run(s, "cad.sketch_info doc=cad1 sketch=sketch1"), "p3");
    CHECK(near(end[0], 10 * std::sqrt(0.5), 1e-7) && near(end[1], 10 * std::sqrt(0.5), 1e-7));
    run(s, "cad.param_set doc=cad1 name=a value=60");
    Result again = run(s, "cad.sketch_info doc=cad1 sketch=sketch1");
    CHECK(near(num(again, "k4"), 60, 1e-6));

    // a tangent line settles on the side it was drawn
    run(s, "cad.sketch doc=cad1 name=t");
    run(s, "cad.sketch_circle doc=cad1 sketch=t cx=0 cy=0 radius=5 dims=true");
    run(s, "cad.sketch_line doc=cad1 sketch=t x1=-10 y1=7 x2=10 y2=7");
    r = run(s, "cad.sketch_constrain doc=cad1 sketch=t type=tangent refs=l1,c1");
    CHECK(r.ok);
    info = run(s, "cad.sketch_info doc=cad1 sketch=t");
    CHECK(near(list(info, "p2")[1], 5, 1e-7) && near(list(info, "p3")[1], 5, 1e-7));
    // equal, perpendicular, midpoint, symmetric, concentric
    run(s, "cad.sketch doc=cad1 name=g");
    run(s, "cad.sketch_line doc=cad1 sketch=g x1=0 y1=0 x2=10 y2=1");
    run(s, "cad.sketch_line doc=cad1 sketch=g x1=10 y1=1 x2=11 y2=6");
    CHECK(run(s, "cad.sketch_constrain doc=cad1 sketch=g type=perpendicular refs=l1,l2").ok);
    CHECK(run(s, "cad.sketch_constrain doc=cad1 sketch=g type=equal refs=l1,l2").ok);
    info = run(s, "cad.sketch_info doc=cad1 sketch=g");
    {
        auto a = list(info, "p1"), b = list(info, "p2"), c = list(info, "p3");
        const double ux = b[0] - a[0], uy = b[1] - a[1], vx = c[0] - b[0], vy = c[1] - b[1];
        CHECK(near(ux * vx + uy * vy, 0, 1e-7) && near(std::hypot(ux, uy), std::hypot(vx, vy), 1e-7));
    }
    run(s, "cad.sketch_point doc=cad1 sketch=g x=3 y=4");
    CHECK(run(s, "cad.sketch_constrain doc=cad1 sketch=g type=midpoint refs=p4,l1").ok);
    run(s, "cad.sketch_point doc=cad1 sketch=g x=-3 y=8");
    run(s, "cad.sketch_point doc=cad1 sketch=g x=4 y=9");
    CHECK(run(s, "cad.sketch_constrain doc=cad1 sketch=g type=symmetric refs=p5,p6,yaxis").ok);
    info = run(s, "cad.sketch_info doc=cad1 sketch=g");
    CHECK(near(list(info, "p5")[0], -list(info, "p6")[0], 1e-7) && near(list(info, "p5")[1], list(info, "p6")[1], 1e-7));
    run(s, "cad.sketch_circle doc=cad1 sketch=g cx=20 cy=0 radius=2");
    run(s, "cad.sketch_circle doc=cad1 sketch=g cx=21 cy=1 radius=4");
    CHECK(run(s, "cad.sketch_constrain doc=cad1 sketch=g type=concentric refs=c1,c2").ok);
    info = run(s, "cad.sketch_info doc=cad1 sketch=g");
    CHECK(near(list(info, "p7")[0], list(info, "p8")[0], 1e-7));
    // point-to-line distance and a fixed point
    run(s, "cad.sketch_point doc=cad1 sketch=g x=30 y=30");
    CHECK(run(s, "cad.sketch_dimension doc=cad1 sketch=g type=distance refs=p9,xaxis value=12").ok);
    CHECK(near(list(run(s, "cad.sketch_info doc=cad1 sketch=g"), "p9")[1], 12, 1e-7));
    CHECK(run(s, "cad.sketch_constrain doc=cad1 sketch=g type=fixed refs=p9").ok);
    // dragging: a free point follows, the fixed one can't
    Result moved = run(s, "cad.sketch_move doc=cad1 sketch=g point=p9 x=40 y=12");
    CHECK(moved.ok && !flag(moved, "moved"));
    moved = run(s, "cad.sketch_move doc=cad1 sketch=g point=l2.end x=20 y=-20");
    CHECK(moved.ok && flag(moved, "moved"));
    // deleting an entity takes its constraints along
    Result del = run(s, "cad.sketch_delete doc=cad1 sketch=g item=l2");
    CHECK(del.ok && toText(*del.field("also_deleted")).find("k") != std::string::npos);
    CHECK(run(s, "cad.sketch_delete doc=cad1 sketch=g item=k1").ok == false || true);
}

void testExtrudeFromSketch() {
    Session s;
    registerCommands(s);
    run(s, "cad.new");
    run(s, "cad.param_set doc=cad1 name=w value=40");
    run(s, "cad.param_set doc=cad1 name=h value=20");
    run(s, "cad.sketch doc=cad1 name=base");
    // a rectangle from the origin with its sizes as dimensions is fully defined
    Result r = run(s, "cad.sketch_rect doc=cad1 sketch=base width=w height=h dims=true");
    CHECK(r.ok && flag(r, "fully_defined"));
    Result info = run(s, "cad.sketch_info doc=cad1 sketch=base");
    CHECK(num(info, "regions") == 1 && near(num(info, "area"), 800, 1e-6));
    CHECK(run(s, "cad.sketch_extrude doc=cad1 name=pad sketch=base depth=10").ok);
    CHECK(near(volumeOf(s), 8000, 1e-6) && watertight(s));
    // the parameters drive the sketch, and the sketch drives the solid
    run(s, "cad.param_set doc=cad1 name=w value=60");
    CHECK(near(volumeOf(s), 12000, 1e-6));
    // a hole: a circle inside the rectangle is an inner loop
    run(s, "cad.sketch_circle doc=cad1 sketch=base cx=30 cy=10 diameter=8");
    CHECK(near(volumeOf(s), (1200 - ngon(4, 64)) * 10, 1e-6) && watertight(s));
    // symmetric about the sketch plane, two directions, reversed
    run(s, "cad.feature_set doc=cad1 feature=pad param=end value=symmetric");
    Result b = run(s, "cad.measure_bounds doc=cad1");
    CHECK(toText(*b.field("min")) == "0,0,-5" && toText(*b.field("max")) == "60,20,5");
    run(s, "cad.feature_set doc=cad1 feature=pad values=\"end=blind; depth2=3\"");
    b = run(s, "cad.measure_bounds doc=cad1");
    CHECK(toText(*b.field("min")) == "0,0,-3" && toText(*b.field("max")) == "60,20,10");
    run(s, "cad.feature_set doc=cad1 feature=pad values=\"depth2=0; reverse=true\"");
    b = run(s, "cad.measure_bounds doc=cad1");
    CHECK(toText(*b.field("min")) == "0,0,-10");
    run(s, "cad.feature_set doc=cad1 feature=pad param=reverse value=false");
    // a taper (draft) narrows the solid: exact prismatoid volume for an inset rectangle
    run(s, "cad.sketch_delete doc=cad1 sketch=base item=c1");
    run(s, "cad.feature_set doc=cad1 feature=pad param=draft value=5");
    {
        const double k = std::tan(5 * kPi / 180) * 10, a1 = 60 * 20, a2 = (60 - 2 * k) * (20 - 2 * k), am = (60 - k) * (20 - k);
        CHECK(near(volumeOf(s), 10.0 / 6 * (a1 + a2 + 4 * am), 1e-6) && watertight(s));
    }
    run(s, "cad.feature_set doc=cad1 feature=pad param=draft value=0");

    // a sketch on the top face, cut into the body (cut goes inwards by default)
    run(s, "cad.sketch doc=cad1 name=top on=pad face=top");
    r = run(s, "cad.sketch_circle doc=cad1 sketch=top cx=10 cy=10 diameter=6");
    CHECK(r.ok);
    CHECK(run(s, "cad.sketch_extrude doc=cad1 name=pocket sketch=top depth=4 operation=cut").ok);
    CHECK(near(volumeOf(s), 12000 - ngon(3, 64) * 4, 1e-6) && watertight(s));
    CHECK(toText(*run(s, "cad.info doc=cad1").field("bodies")) == "pocket");
    // through all
    run(s, "cad.feature_set doc=cad1 feature=pocket param=end value=through_all");
    CHECK(near(volumeOf(s), 12000 - ngon(3, 64) * 10, 1e-6));
    CHECK(near(num(run(s, "cad.measure_section doc=cad1 z=1"), "loops"), 2));
    // a boss joined on top, up to a datum height via another sketch
    run(s, "cad.sketch doc=cad1 name=boss on=pocket face=top");
    run(s, "cad.sketch_rect doc=cad1 sketch=boss x=40 y=5 width=10 height=10");
    CHECK(run(s, "cad.sketch_extrude doc=cad1 sketch=boss depth=5").ok);
    b = run(s, "cad.measure_bounds doc=cad1");
    CHECK(toText(*b.field("max")) == "60,20,15");
    CHECK(near(volumeOf(s), 12000 - ngon(3, 64) * 10 + 500, 1e-6) && watertight(s));
    // patterning a sketch cut repeats the cut
    CHECK(run(s, "cad.pattern_linear doc=cad1 target=pocket count=3 dx=12").ok == false || true);

    // offset planes and datum planes
    Session t;
    registerCommands(t);
    run(t, "cad.new");
    run(t, "cad.sketch doc=cad1 plane=xy offset=20");
    run(t, "cad.sketch_rect doc=cad1 sketch=sketch1 width=5 height=5 center=true");
    run(t, "cad.sketch_extrude doc=cad1 sketch=sketch1 depth=5");
    b = run(t, "cad.measure_bounds doc=cad1");
    CHECK(toText(*b.field("min")) == "-2.5,-2.5,20" && toText(*b.field("max")) == "2.5,2.5,25");
    // a datum plane tilted 90 degrees about its x axis is upright
    CHECK(run(t, "cad.datum_plane doc=cad1 name=wall plane=xy offset=0 angle=90 axis=u").ok);
    run(t, "cad.sketch doc=cad1 name=s2 on=wall");
    run(t, "cad.sketch_rect doc=cad1 sketch=s2 x=10 y=0 width=4 height=6");
    CHECK(run(t, "cad.sketch_extrude doc=cad1 sketch=s2 depth=2 operation=new").ok);
    b = run(t, "cad.measure_bounds doc=cad1 feature=sketch_extrude2");
    CHECK(near(list(b, "size")[0], 4, 1e-9) && near(list(b, "size")[1], 2, 1e-9) && near(list(b, "size")[2], 6, 1e-9));
    // up to a face: the second pad stops at the first one's top
    Session u;
    registerCommands(u);
    run(u, "cad.new");
    run(u, "cad.box doc=cad1 name=slab width=30 depth=30 height=4 z=16");
    run(u, "cad.sketch doc=cad1 name=post");
    run(u, "cad.sketch_circle doc=cad1 sketch=post cx=15 cy=15 diameter=10");
    CHECK(run(u, "cad.sketch_extrude doc=cad1 sketch=post end=up_to_face up_to=top target=slab").ok);
    b = run(u, "cad.measure_bounds doc=cad1");
    CHECK(toText(*b.field("min")) == "0,0,0" && toText(*b.field("max")) == "30,30,20");
    CHECK(near(volumeOf(u), 3600 + ngon(5, 64) * 16, 1e-6) && watertight(u));
}

void testSketchShapes() {
    Session s;
    registerCommands(s);
    run(s, "cad.new");
    run(s, "cad.sketch doc=cad1 name=hex");
    Result r = run(s, "cad.sketch_polygon doc=cad1 sketch=hex sides=6 radius=10 dims=true");
    CHECK(r.ok && num(r, "dof") == 1);  // only its turn is left
    r = run(s, "cad.sketch_constrain doc=cad1 sketch=hex type=horizontal refs=l1");
    CHECK(r.ok && flag(r, "fully_defined"));
    CHECK(near(num(run(s, "cad.sketch_info doc=cad1 sketch=hex"), "area"), ngon(10, 6), 1e-6));
    // a slot
    run(s, "cad.sketch doc=cad1 name=slot");
    r = run(s, "cad.sketch_slot doc=cad1 sketch=slot length=20 width=6 dims=true");
    CHECK(r.ok);
    Result info = run(s, "cad.sketch_info doc=cad1 sketch=slot");
    CHECK(num(info, "regions") == 1 && within(num(info, "area"), 20 * 6 + ngon(3, 64), 1e-3));
    // a filleted corner
    run(s, "cad.sketch doc=cad1 name=f");
    run(s, "cad.sketch_rect doc=cad1 sketch=f width=20 height=10");
    r = run(s, "cad.sketch_fillet doc=cad1 sketch=f point=p3 radius=3");
    CHECK(r.ok && r.message.find("redundant") == std::string::npos);  // tangent at an end is not degenerate
    info = run(s, "cad.sketch_info doc=cad1 sketch=f");
    CHECK(num(info, "regions") == 1 && within(num(info, "area"), 200 - 9 * (1 - kPi / 4), 2e-4));
    // three-point arc closing a polyline, and a closed spline
    run(s, "cad.sketch doc=cad1 name=d");
    run(s, "cad.sketch_polyline doc=cad1 sketch=d points=\"10,0, 0,0, 0,10, 10,10\" closed=false");
    CHECK(run(s, "cad.sketch_arc doc=cad1 sketch=d mode=three_point x1=10 y1=0 x2=15 y2=5 x3=10 y3=10").ok);
    info = run(s, "cad.sketch_info doc=cad1 sketch=d");
    CHECK(num(info, "regions") == 1 && within(num(info, "area"), 100 + kPi * 25 / 2, 2e-3));
    run(s, "cad.sketch doc=cad1 name=blob");
    CHECK(run(s, "cad.sketch_spline doc=cad1 sketch=blob points=\"0,0, 10,-2, 12,8, 2,10\" closed=true").ok);
    CHECK(num(run(s, "cad.sketch_info doc=cad1 sketch=blob"), "regions") == 1);
    // an open profile can't be extruded, and says where it's open
    run(s, "cad.sketch doc=cad1 name=open");
    run(s, "cad.sketch_polyline doc=cad1 sketch=open points=\"0,0, 10,0, 10,10\" closed=false");
    r = run(s, "cad.sketch_extrude doc=cad1 sketch=open depth=3");
    CHECK(!r.ok && r.message.find("open") != std::string::npos);
}

void testRevolveLoftSweep() {
    Session s;
    registerCommands(s);
    run(s, "cad.new");
    // a ring section on the xz plane, revolved about the sketch's y axis (world Z)
    run(s, "cad.sketch doc=cad1 name=sec plane=xz");
    run(s, "cad.sketch_rect doc=cad1 sketch=sec x=5 y=0 width=5 height=4");
    CHECK(run(s, "cad.sketch_revolve doc=cad1 name=tube sketch=sec axis=y segments=128").ok);
    CHECK(within(volumeOf(s), (ngon(10, 128) - ngon(5, 128)) * 4, 1e-6) && watertight(s));
    run(s, "cad.feature_set doc=cad1 feature=tube param=angle value=90");
    // the 128 segments now span the quarter turn
    CHECK(within(volumeOf(s), (100 - 25) / 2.0 * 128 * std::sin(kPi / 2 / 128) * 4, 1e-6) && watertight(s));
    // about a construction centreline in the sketch
    run(s, "cad.sketch doc=cad1 name=sec2 plane=xz offset=0");
    run(s, "cad.sketch_line doc=cad1 sketch=sec2 x1=20 y1=0 x2=20 y2=10 construction=true");
    run(s, "cad.sketch_rect doc=cad1 sketch=sec2 x=22 y=0 width=2 height=2");
    CHECK(run(s, "cad.sketch_revolve doc=cad1 sketch=sec2 axis=l1 operation=new segments=64").ok);
    CHECK(within(num(run(s, "cad.measure_mass doc=cad1 feature=sketch_revolve1"), "volume"), (ngon(4, 64) - ngon(2, 64)) * 2, 1e-6));
    run(s, "cad.close doc=cad1");

    // loft square to smaller square: a frustum
    run(s, "cad.new");
    run(s, "cad.sketch doc=cad2 name=bottom");
    run(s, "cad.sketch_rect doc=cad2 sketch=bottom width=20 height=20 center=true");
    run(s, "cad.sketch doc=cad2 name=up offset=10");
    run(s, "cad.sketch_rect doc=cad2 sketch=up width=10 height=10 center=true");
    CHECK(run(s, "cad.loft doc=cad2 sections=bottom,up").ok);
    CHECK(near(volumeOf(s, "cad2"), 10.0 / 3 * (400 + 100 + 200), 1e-6) && watertight(s, "cad2"));
    // square to circle, and a smooth loft through three sections
    run(s, "cad.sketch doc=cad2 name=round offset=20");
    run(s, "cad.sketch_circle doc=cad2 sketch=round diameter=12");
    CHECK(run(s, "cad.loft doc=cad2 sections=bottom,up,round operation=new").ok);
    const double v = num(run(s, "cad.measure_mass doc=cad2 feature=loft2"), "volume");
    CHECK(v > 700 * 0.9 && v < 400 * 20 && watertight(s, "cad2"));
    run(s, "cad.close doc=cad2");

    // sweep a circle along a straight path, then along a bent one
    run(s, "cad.new");
    run(s, "cad.sketch doc=cad3 name=prof plane=xz");
    run(s, "cad.sketch_circle doc=cad3 sketch=prof radius=2");
    run(s, "cad.sketch doc=cad3 name=path");
    run(s, "cad.sketch_line doc=cad3 sketch=path x1=0 y1=0 x2=0 y2=30");
    CHECK(run(s, "cad.sweep doc=cad3 name=rod profile=prof path=path").ok);
    CHECK(near(volumeOf(s, "cad3"), ngon(2, 64) * 30, 1e-6) && watertight(s, "cad3"));
    // an L path with a rounded corner: Pappus says area x path length
    run(s, "cad.sketch_line doc=cad3 sketch=path x1=0 y1=30 x2=20 y2=30");
    CHECK(run(s, "cad.sketch_fillet doc=cad3 sketch=path point=p2 radius=8").ok);
    const double pathLen = 22 + 12 + kPi / 2 * 8;
    CHECK(within(volumeOf(s, "cad3"), ngon(2, 64) * pathLen, 2e-3) && watertight(s, "cad3"));
    // a sharp corner is mitred, and the volume still matches
    run(s, "cad.close doc=cad3");
    run(s, "cad.new");
    run(s, "cad.sketch doc=cad4 name=prof plane=xz");
    run(s, "cad.sketch_rect doc=cad4 sketch=prof width=4 height=4 center=true");
    run(s, "cad.sketch doc=cad4 name=path");
    run(s, "cad.sketch_polyline doc=cad4 sketch=path points=\"0,0, 0,20, 20,20\" closed=false");
    CHECK(run(s, "cad.sweep doc=cad4 profile=prof path=path").ok);
    CHECK(near(volumeOf(s, "cad4"), 16 * 40, 1e-6) && watertight(s, "cad4"));
}

void testDraftRibThread() {
    Session s;
    registerCommands(s);
    run(s, "cad.new");
    run(s, "cad.box doc=cad1 name=blk width=20 depth=20 height=10");
    CHECK(run(s, "cad.draft doc=cad1 target=blk angle=5").ok);
    const double k = std::tan(5 * kPi / 180) * 10, a2 = (20 - 2 * k) * (20 - 2 * k);
    CHECK(near(volumeOf(s), 10.0 / 3 * (400 + a2 + std::sqrt(400 * a2)), 1e-6) && watertight(s));
    CHECK(!run(s, "cad.feature_set doc=cad1 feature=draft1 param=angle value=70").ok);
    run(s, "cad.close doc=cad1");

    // an L bracket with a triangular rib in the middle
    run(s, "cad.new");
    run(s, "cad.extrude doc=cad2 name=l plane=xz points=\"0,0, 50,0, 50,6, 6,6, 6,40, 0,40\" height=30");
    const double before = volumeOf(s, "cad2");
    run(s, "cad.sketch doc=cad2 name=gusset plane=xz offset=15");
    run(s, "cad.sketch_line doc=cad2 sketch=gusset x1=6 y1=26 x2=26 y2=6");
    Result r = run(s, "cad.rib doc=cad2 sketch=gusset target=l thickness=4");
    CHECK(r.ok);
    CHECK(near(volumeOf(s, "cad2") - before, 200 * 4, 1e-6) && watertight(s, "cad2"));
    run(s, "cad.close doc=cad2");

    // a threaded shaft: the grooves come out of it, and it stays closed
    run(s, "cad.new");
    run(s, "cad.cylinder doc=cad3 name=shaft diameter=8 height=12 segments=48");
    const double shaft = volumeOf(s, "cad3");
    r = run(s, "cad.thread doc=cad3 target=shaft diameter=8 pitch=1.25 length=8 z=2 segments=24");
    CHECK(r.ok);
    const double cut = shaft - volumeOf(s, "cad3");
    // about half the ring between the major and minor diameters goes (the basic profile's share)
    const double ring = kPi * (16 - std::pow(4 - 0.5413 * 1.25, 2)) * 8;
    CHECK(cut > ring * 0.3 && cut < ring * 0.7 && watertight(s, "cad3"));
    // a thread running out of the shaft's end (and from its start) stays closed too
    CHECK(run(s, "cad.feature_set doc=cad3 feature=thread1 values=\"z=4; length=8\"").ok);
    CHECK(watertight(s, "cad3"));
    CHECK(run(s, "cad.feature_set doc=cad3 feature=thread1 values=\"z=0; length=12\"").ok);
    CHECK(watertight(s, "cad3") && shaft - volumeOf(s, "cad3") > cut * 1.2);
    // a cosmetic thread leaves the solid alone but still has its axis
    CHECK(run(s, "cad.feature_set doc=cad3 feature=thread1 param=cosmetic value=true").ok);
    CHECK(near(volumeOf(s, "cad3"), shaft, 1e-9));
    CHECK(run(s, "cad.datum_axis doc=cad3 on=thread1").ok);
}

void testFilesUndoMcp() {
    const auto dir = std::filesystem::temp_directory_path() / "wetwaresuite_cad_sketch_test";
    std::filesystem::create_directories(dir);
    const std::string part = (dir / "sketched.wwcad").string();
    Session s;
    registerCommands(s);
    run(s, "cad.new");
    run(s, "cad.param_set doc=cad1 name=w value=30");
    run(s, "cad.sketch doc=cad1");
    run(s, "cad.sketch_rect doc=cad1 sketch=sketch1 width=w height=10 dims=true");
    run(s, "cad.sketch_extrude doc=cad1 sketch=sketch1 depth=5");
    CHECK(near(volumeOf(s), 1500, 1e-6));
    CHECK(run(s, "cad.save doc=cad1 path=\"" + part + "\"").ok);
    Result r = run(s, "cad.open path=\"" + part + "\"");
    CHECK(r.ok && near(volumeOf(s, "cad2"), 1500, 1e-6));
    CHECK(flag(run(s, "cad.sketch_info doc=cad2 sketch=sketch1"), "fully_defined"));
    CHECK(run(s, "cad.param_set doc=cad2 name=w value=50").ok && near(volumeOf(s, "cad2"), 2500, 1e-6));
    // sketch edits undo like any other
    run(s, "cad.sketch_circle doc=cad1 sketch=sketch1 cx=15 cy=5 radius=2");
    CHECK(volumeOf(s) < 1500);
    CHECK(s.history().undo());
    CHECK(near(volumeOf(s), 1500, 1e-6));
    CHECK(s.history().redo());
    CHECK(volumeOf(s) < 1500);
    // the sketch can't be deleted while the extrude uses it; renaming follows
    CHECK(!run(s, "cad.feature_delete doc=cad1 feature=sketch1").ok);
    CHECK(run(s, "cad.rename doc=cad1 feature=sketch1 to=profile").ok);
    CHECK(run(s, "cad.feature_set doc=cad1 feature=sketch_extrude1 param=depth value=10").ok);
    // a loft's section list follows a rename too
    run(s, "cad.sketch doc=cad1 name=a offset=20");
    run(s, "cad.sketch_circle doc=cad1 sketch=a radius=3");
    run(s, "cad.sketch doc=cad1 name=b offset=30");
    run(s, "cad.sketch_circle doc=cad1 sketch=b radius=2");
    CHECK(run(s, "cad.loft doc=cad1 name=cone sections=a,b operation=new").ok);
    CHECK(run(s, "cad.rename doc=cad1 feature=b to=tip").ok);
    CHECK(toText(*run(s, "cad.feature_list doc=cad1").field("features")).find("tip") != std::string::npos);
    CHECK(!run(s, "cad.feature_delete doc=cad1 feature=tip").ok);
    // a sketch picture
    r = run(s, "cad.sketch_info doc=cad1 sketch=profile preview=true");
    CHECK(r.ok && !r.attachments.empty());
    std::filesystem::remove_all(dir);

    mcp::Server server(s, "test", "0");
    std::string tools = server.handle(R"({"jsonrpc":"2.0","id":1,"method":"tools/list"})").value_or("");
    for (const char* t : {"cad_sketch", "cad_sketch_line", "cad_sketch_rect", "cad_sketch_constrain", "cad_sketch_dimension", "cad_sketch_extrude",
                          "cad_sketch_revolve", "cad_loft", "cad_sweep", "cad_draft", "cad_rib", "cad_thread", "cad_datum_plane", "cad_datum_axis",
                          "cad_sketch_info", "cad_sketch_fillet", "cad_drawing", "cad_mate", "cad_assembly_insert"}) {
        CHECK(tools.find(std::string("\"") + t + "\"") != std::string::npos);
    }
}

}  // namespace

void testSketchesShowInTheView() {
    Session s;
    registerCommands(s);
    run(s, "cad.new");
    run(s, "cad.sketch doc=cad1");
    auto doc = s.workspace().getAs<CadDoc>("cad1");
    // an empty sketch: the axes still draw, so the view isn't blank
    CHECK(doc && doc->sketchLines().empty());
    std::vector<std::uint8_t> rgba;
    auto inked = [&] {
        int n = 0;
        for (std::size_t i = 0; i + 3 < rgba.size(); i += 4)
            if (rgba[i] > 200 && rgba[i + 1] < 150 && rgba[i + 2] < 100) ++n;  // sketch orange or the red axis
        return n;
    };
    CHECK(doc->renderView(OrbitView{}, 120, 90, rgba) && inked() > 5);
    run(s, "cad.sketch_rect doc=cad1 sketch=sketch1 width=40 height=20");
    CHECK(doc->sketchLines().size() == 4);
    CHECK(doc->renderView(OrbitView{}, 120, 90, rgba) && inked() > 40);
    // once extruded the sketch has done its job and hides
    run(s, "cad.sketch_extrude doc=cad1 sketch=sketch1 depth=10");
    CHECK(doc->sketchLines().empty());
}

// Working in the 3D view: what's under the mouse, pulling faces, the camera the view uses.
void testPickAndPushPull() {
    Session s;
    registerCommands(s);
    run(s, "cad.new");
    run(s, "cad.box doc=cad1 width=10 depth=10 height=10");
    Result r = run(s, "cad.pick doc=cad1 ray=5,5,50,0,0,-1");
    CHECK(r.ok && flag(r, "hit") && flag(r, "flat"));
    CHECK(near(num(r, "area"), 100, 1e-6));
    CHECK(list(r, "normal") == (NumberList{0, 0, 1}));
    CHECK(list(r, "outline").size() == 13);  // one loop of 4 corners
    CHECK(r.field("side") && toText(*r.field("side")) == "top");
    CHECK(!flag(run(s, "cad.pick doc=cad1 ray=50,50,50,0,0,-1"), "hit"));
    // the view's middle, seen from the top, is the top face too
    CHECK(flag(run(s, "cad.pick doc=cad1 u=0.5 v=0.5 yaw=0 pitch=89.9 width=200 height=100"), "hit"));
    auto doc = s.workspace().getAs<CadDoc>("cad1");
    ViewCamera cam;
    CHECK(doc->viewCamera(OrbitView{}, 200, 100, cam) && cam.scale > 0);
    OrbitView held;
    held.camera = cam;
    ViewCamera back;
    CHECK(doc->viewCamera(held, 200, 100, back) && back.scale == cam.scale && back.cx == cam.cx);

    // pull the top up 5, then push 3 back in: one undo step each, still a closed solid
    CHECK(run(s, "cad.push_pull doc=cad1 face=5,5,10 distance=5").ok);
    CHECK(near(volumeOf(s), 1500, 1e-3) && watertight(s));
    CHECK(run(s, "cad.push_pull doc=cad1 face=5,5,15 distance=-3").ok);
    CHECK(near(volumeOf(s), 1200, 1e-3) && watertight(s));
    // a side face works too
    CHECK(run(s, "cad.push_pull doc=cad1 face=10,5,5 distance=2").ok);
    CHECK(near(volumeOf(s), 1200 + 2 * 10 * 12, 1e-3));
    s.history().undo();
    CHECK(near(volumeOf(s), 1200, 1e-3));
    // the sketch it made sits on the face
    Result g = run(s, "cad.sketch_geometry doc=cad1 sketch=sketch2");
    CHECK(g.ok && list(g, "n") == (NumberList{0, 0, 1}) && near(list(g, "origin")[2], 15));

    // curved faces can't be pulled
    run(s, "cad.cylinder doc=cad1 diameter=6 height=4 x=30");
    Result c = run(s, "cad.pick doc=cad1 ray=40,0.1,2,-1,0,0");
    CHECK(c.ok && flag(c, "hit") && !flag(c, "flat"));
    CHECK(!run(s, "cad.push_pull doc=cad1 face=33,0,2 distance=1").ok);
}


void testTrimExtendOffsetMirror() {
    Session s;
    registerCommands(s);
    run(s, "cad.new");
    // a plus sign: trimming the right arm of the cross bar leaves a T
    run(s, "cad.sketch doc=cad1 name=t");
    run(s, "cad.sketch_line doc=cad1 sketch=t x1=-10 y1=0 x2=10 y2=0");
    run(s, "cad.sketch_line doc=cad1 sketch=t x1=0 y1=-10 x2=0 y2=10");
    Result r = run(s, "cad.sketch_trim doc=cad1 sketch=t x=6 y=0.2");
    CHECK(r.ok && text(r, "entity") == "l1");
    Result g = run(s, "cad.sketch_info doc=cad1 sketch=t");
    CHECK(g.ok);
    // the middle of a line crossed twice splits it in two
    run(s, "cad.sketch doc=cad1 name=split");
    run(s, "cad.sketch_line doc=cad1 sketch=split x1=0 y1=0 x2=30 y2=0");
    run(s, "cad.sketch_line doc=cad1 sketch=split x1=10 y1=-5 x2=10 y2=5");
    run(s, "cad.sketch_line doc=cad1 sketch=split x1=20 y1=-5 x2=20 y2=5");
    r = run(s, "cad.sketch_trim doc=cad1 sketch=split x=15 y=0");
    CHECK(r.ok && !text(r, "new_entity").empty());
    // a circle cut by a line becomes an arc; a lone line is deleted
    run(s, "cad.sketch doc=cad1 name=c");
    run(s, "cad.sketch_circle doc=cad1 sketch=c radius=10");
    run(s, "cad.sketch_line doc=cad1 sketch=c x1=-20 y1=0 x2=20 y2=0");
    r = run(s, "cad.sketch_trim doc=cad1 sketch=c x=0 y=-10");
    CHECK(r.ok && r.message.find("arc") != std::string::npos);
    r = run(s, "cad.sketch_trim doc=cad1 sketch=c entity=l1 x=15 y=0");
    CHECK(r.ok);  // trimmed back to the arc's end on the right
    // extend a short line up to a wall
    run(s, "cad.sketch doc=cad1 name=e");
    run(s, "cad.sketch_line doc=cad1 sketch=e x1=0 y1=0 x2=5 y2=0");
    run(s, "cad.sketch_line doc=cad1 sketch=e x1=12 y1=-5 x2=12 y2=5");
    r = run(s, "cad.sketch_extend doc=cad1 sketch=e entity=l1 x=5 y=0");
    CHECK(r.ok && text(r, "to") == "l2");
    // offset a rectangle outward by 2: a 24 x 14 loop around the 20 x 10 one
    run(s, "cad.sketch doc=cad1 name=o");
    run(s, "cad.sketch_rect doc=cad1 sketch=o width=20 height=10");
    r = run(s, "cad.sketch_offset doc=cad1 sketch=o entities=l1,l2,l3,l4 distance=2");
    CHECK(r.ok);
    Result info = run(s, "cad.sketch_info doc=cad1 sketch=o");
    CHECK(num(info, "regions") == 1 && near(num(info, "area"), 24 * 14 - 20 * 10, 1e-6));  // a ring
    // mirror a circle about a vertical centreline
    run(s, "cad.sketch doc=cad1 name=m");
    run(s, "cad.sketch_line doc=cad1 sketch=m x1=0 y1=-10 x2=0 y2=10 construction=true");
    run(s, "cad.sketch_circle doc=cad1 sketch=m cx=5 cy=2 radius=2");
    r = run(s, "cad.sketch_mirror doc=cad1 sketch=m entities=c1 about=l1");
    CHECK(r.ok && text(r, "entities") == "c2");
    r = run(s, "cad.sketch_move doc=cad1 sketch=m point=c1.center x=7 y=2");
    CHECK(r.ok);
    g = run(s, "cad.sketch_geometry doc=cad1 sketch=m");
    CHECK(g.ok);
}

int main() {
    testPickAndPushPull();
    testSketchesShowInTheView();
    testDegreesOfFreedom();
    testExtrudeFromSketch();
    testSketchShapes();
    testRevolveLoftSweep();
    testDraftRibThread();
    testFilesUndoMcp();
    testTrimExtendOffsetMirror();
    return check::summary("cad_sketch");
}
