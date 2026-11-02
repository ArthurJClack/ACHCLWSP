// Tests for the full toolset: anchors and paths, regions, appearance,
// organisation, artboards, symbols, type, trace and the file round trips.
// Shapes are always looked up again after a command (never held across one).
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <system_error>

#include "wetwaresuite/core/cli.hpp"
#include "wetwaresuite/mcp/server.hpp"
#include "wetwaresuite/raster/codec.hpp"
#include "wetwaresuite/vector/vector.hpp"
#include "check.hpp"

using namespace wetwaresuite;
using namespace wetwaresuite::vector;

namespace {

// clean-up never throws (windows keeps a file locked while anything still has it open)
void removeFile(const std::filesystem::path& p) {
    std::error_code ec;
    std::filesystem::remove(p, ec);
}

bool near(double a, double b, double tol = 1e-6) { return std::abs(a - b) <= tol; }

std::filesystem::path tmp(const std::string& name) { return std::filesystem::temp_directory_path() / ("wetwaresuite_vtools_" + name); }

struct Fixture {
    Session session;
    cli::Batch batch{session};
    Fixture() { registerCommands(session); }
    Result run(const std::string& line) {
        Result r = batch.runLine(line);
        if (!r.ok) std::printf("  command failed: %s -> %s\n", line.c_str(), r.message.c_str());
        return r;
    }
    bool fails(const std::string& line) { return !batch.runLine(line).ok; }
    std::string field(const Result& r, const std::string& key) {
        const Value* v = r.field(key);
        return v ? toText(*v) : std::string();
    }
    VectorDoc& doc() { return *session.workspace().getAs<VectorDoc>("vec1"); }
    const Shape& shape(const std::string& id) { return doc().shapes[static_cast<std::size_t>(doc().find(id))]; }
    Bounds box(const std::string& id) { return shapeBounds(doc(), doc().find(id)); }
    int count(const std::string& id) {  // anchors
        return static_cast<int>(std::stoi(field(run("vector.nodes doc=vec1 shape=" + id), "count")));
    }
    Color px(double x, double y) {
        raster::Image img = render(doc(), 1.0);
        const std::uint8_t* p = img.pixel(static_cast<int>(x), static_cast<int>(y));
        return {p[0], p[1], p[2], p[3]};
    }
};

bool isColor(Color c, int r, int g, int b, int tol = 12) { return std::abs(c.r - r) <= tol && std::abs(c.g - g) <= tol && std::abs(c.b - b) <= tol; }

double outlineArea(const Outline& o) {
    double a = 0;
    for (const auto& c : o) a += signedArea(c.points);
    return std::abs(a);
}

void testAnchors() {
    Fixture f;
    f.run("vector.new width=100 height=100");
    Result r = f.run("vector.pen doc=vec1 nodes=\"10,50; 50,10,70,10; 90,50\"");
    const std::string pen = f.field(r, "shape");
    CHECK(f.count(pen) == 3);
    std::string nodes = f.field(f.run("vector.nodes doc=vec1 shape=" + pen), "nodes");
    CHECK(nodes.find(":symmetric") != std::string::npos);
    // moving a symmetric anchor's out handle mirrors the in handle
    CHECK(f.run("vector.move_node doc=vec1 shape=" + pen + " node=1 part=out x=80 y=20").ok);
    BezierPath bp = parsePath(f.shape(pen).d);
    CHECK(bp.size() == 1 && bp[0].nodes.size() == 3);
    if (bp.size() == 1 && bp[0].nodes.size() == 3) {
        const Node& n = bp[0].nodes[1];
        CHECK(near(n.out.x, 80, 1e-3) && near(n.out.y, 20, 1e-3) && near(n.in.x, 20, 1e-3) && near(n.in.y, 0, 1e-3));
    }
    // a new anchor on the curve keeps the shape
    const Bounds before = f.box(pen);
    r = f.run("vector.add_node doc=vec1 shape=" + pen + " node=1 t=0.5");
    CHECK(r.ok && f.count(pen) == 4 && f.field(r, "node") == "2");
    const Bounds after = f.box(pen);
    CHECK(near(before.x0, after.x0, 0.5) && near(before.y0, after.y0, 0.5) && near(before.x1, after.x1, 0.5));
    CHECK(f.run("vector.delete_node doc=vec1 shape=" + pen + " node=2").ok && f.count(pen) == 3);
    // a smooth anchor's other handle keeps its length but turns to stay in line
    CHECK(f.run("vector.move_node doc=vec1 shape=" + pen + " node=1 part=in x=20 y=10").ok);
    bp = parsePath(f.shape(pen).d);
    {
        const Node& n = bp[0].nodes[1];
        const double cross = (n.out.x - n.p.x) * (n.in.y - n.p.y) - (n.out.y - n.p.y) * (n.in.x - n.p.x);
        CHECK(near(cross, 0, 1e-3) && n.out.x > n.p.x && near(n.in.x, 20, 1e-3));
    }
    // move the anchor itself: handles come along
    CHECK(f.run("vector.move_node doc=vec1 shape=" + pen + " node=1 dx=5").ok);
    bp = parsePath(f.shape(pen).d);
    CHECK(near(bp[0].nodes[1].p.x, 55, 1e-3) && near(bp[0].nodes[1].in.x, 25, 1e-3));
    CHECK(f.run("vector.convert_node doc=vec1 shape=" + pen + " node=1 type=corner").ok);
    bp = parsePath(f.shape(pen).d);
    CHECK(nodeType(bp[0].nodes[1]) == NodeType::Corner);
    CHECK(f.run("vector.convert_node doc=vec1 shape=" + pen + " node=0,1,2 type=smooth").ok);
    bp = parsePath(f.shape(pen).d);
    CHECK(nodeType(bp[0].nodes[1]) != NodeType::Corner);
    // undo walks back through the edits
    CHECK(f.session.history().undo());
    bp = parsePath(f.shape(pen).d);
    CHECK(nodeType(bp[0].nodes[1]) == NodeType::Corner);
    CHECK(f.fails("vector.delete_node doc=vec1 shape=" + pen + " node=9"));
    // a rectangle's anchors are its corners, on the canvas (after its transform)
    r = f.run("vector.add_rect doc=vec1 x=10 y=10 width=20 height=10");
    const std::string rect = f.field(r, "shape");
    f.run("vector.transform doc=vec1 shape=" + rect + " dx=5");
    nodes = f.field(f.run("vector.nodes doc=vec1 shape=" + rect), "nodes");
    CHECK(nodes.rfind("0:15,10,", 0) == 0);
    // smooth pen
    r = f.run("vector.pen doc=vec1 nodes=\"10,90; 50,60; 90,90\" smooth=true");
    bp = parsePath(f.shape(f.field(r, "shape")).d);
    CHECK(bp.size() == 1 && nodeType(bp[0].nodes[1]) == NodeType::Symmetric);
    CHECK(f.fails("vector.pen doc=vec1 nodes=\"10,90\""));
    CHECK(f.fails("vector.pen doc=vec1 nodes=\"10,90; 5\""));
}

void testPathOps() {
    Fixture f;
    f.run("vector.new width=200 height=200");
    // join two open lines end to end, then close the result
    const std::string a = f.field(f.run("vector.add_polyline doc=vec1 points=0,0,10,0"), "shape");
    const std::string b = f.field(f.run("vector.add_polyline doc=vec1 points=30,10,20,10"), "shape");
    CHECK(f.run("vector.join doc=vec1 shape=" + a + " with=" + b).ok);
    CHECK(f.doc().shapes.size() == 1 && f.count(a) == 4);
    BezierPath bp = parsePath(f.shape(a).d);
    CHECK(near(bp[0].nodes[2].p.x, 20) && near(bp[0].nodes[3].p.x, 30));  // b was turned round to meet a's end
    CHECK(f.run("vector.join doc=vec1 shape=" + a).ok);
    CHECK(parsePath(f.shape(a).d)[0].closed);
    // scissors: a closed rect opens at a corner; an open path splits in two
    const std::string rect = f.field(f.run("vector.add_rect doc=vec1 x=50 y=50 width=40 height=40"), "shape");
    CHECK(f.run("vector.split doc=vec1 shape=" + rect + " node=2").ok);
    bp = parsePath(f.shape(rect).d);
    CHECK(bp.size() == 1 && !bp[0].closed && bp[0].nodes.size() == 5 && near(bp[0].nodes[0].p.x, 90) && near(bp[0].nodes[0].p.y, 90));
    Result r = f.run("vector.split doc=vec1 shape=" + rect + " x=50 y=70");
    CHECK(r.ok && !f.field(r, "other").empty());
    CHECK(f.doc().find(f.field(r, "other")) >= 0);
    // knife across a square: two halves
    const std::string sq = f.field(f.run("vector.add_rect doc=vec1 x=100 y=100 width=80 height=80 fill=red"), "shape");
    r = f.run("vector.cut doc=vec1 shapes=" + sq + " line=140,90,140,190");
    CHECK(r.ok && f.field(r, "pieces") == "2");
    const std::string pieces = f.field(r, "shapes");
    const std::string p1 = pieces.substr(0, pieces.find(',')), p2 = pieces.substr(pieces.find(',') + 1);
    CHECK(near(f.box(p1).width(), 40, 0.6) && near(f.box(p2).width(), 40, 0.6));
    CHECK(f.shape(p1).fill == Color{255, 0, 0, 255});
    CHECK(f.fails("vector.cut doc=vec1 shapes=" + p1 + " line=0,0,1,0"));  // the line (extended) runs past it
    // offset path: 5 px out round a 20x20 square
    const std::string small = f.field(f.run("vector.add_rect doc=vec1 x=10 y=150 width=20 height=20"), "shape");
    r = f.run("vector.offset doc=vec1 shape=" + small + " distance=5");
    CHECK(r.ok && near(f.box(f.field(r, "shape")).width(), 30, 0.05) && near(f.box(f.field(r, "shape")).x0, 5, 0.05));
    CHECK(f.doc().find(f.field(r, "shape")) < f.doc().find(small));  // behind the original
    r = f.run("vector.offset doc=vec1 shape=" + small + " distance=-5");
    CHECK(r.ok && near(f.box(f.field(r, "shape")).width(), 10, 0.05));
}

void testStrokeOps() {
    Fixture f;
    f.run("vector.new width=100 height=100");
    // outline stroke: a 10 px butt-capped line becomes an 80 x 10 filled shape
    const std::string line = f.field(f.run("vector.add_line doc=vec1 x1=10 y1=50 x2=90 y2=50 stroke=blue stroke_width=10"), "shape");
    CHECK(f.run("vector.stroke_style doc=vec1 shape=" + line + " cap=butt").ok);
    Result r = f.run("vector.outline_stroke doc=vec1 shape=" + line);
    CHECK(r.ok);
    const std::string out = f.field(r, "shape");
    CHECK(f.doc().find(line) < 0);  // nothing else to show: replaced
    const Bounds b = f.box(out);
    CHECK(near(b.width(), 80, 0.05) && near(b.height(), 10, 0.05));
    CHECK(f.shape(out).fill == Color{0, 0, 255, 255} && !f.shape(out).stroke);
    CHECK(near(outlineArea(outlineIn(f.doc(), f.doc().find(out))), 800, 1));
    // reverse: the anchors swap ends
    const std::string pl = f.field(f.run("vector.add_polyline doc=vec1 points=0,0,10,0,10,10"), "shape");
    CHECK(f.run("vector.reverse doc=vec1 shape=" + pl).ok);
    BezierPath bp = parsePath(f.shape(pl).d);
    CHECK(near(bp[0].nodes[0].p.y, 10) && near(bp[0].nodes[2].p.x, 0));
    CHECK(f.run("vector.close_path doc=vec1 shape=" + pl).ok && parsePath(f.shape(pl).d)[0].closed);
    // simplify a densely sampled circle
    std::string pts;
    for (int i = 0; i < 120; ++i) {
        const double a = i * 3.14159265358979 * 2 / 120;
        pts += (i ? "," : "") + std::to_string(50 + 30 * std::cos(a)) + "," + std::to_string(50 + 30 * std::sin(a));
    }
    const std::string circ = f.field(f.run("vector.add_polygon doc=vec1 points=" + pts), "shape");
    r = f.run("vector.simplify doc=vec1 shape=" + circ + " tolerance=0.5");
    CHECK(r.ok && std::stoi(f.field(r, "after")) < 20 && std::stoi(f.field(r, "before")) == 120);
    CHECK(near(f.box(circ).width(), 60, 1.0));
    // smooth: corners of a square become smooth curves
    const std::string sq = f.field(f.run("vector.add_polygon doc=vec1 points=10,10,40,10,40,40,10,40"), "shape");
    CHECK(f.run("vector.smooth doc=vec1 shape=" + sq).ok);
    bp = parsePath(f.shape(sq).d);
    CHECK(nodeType(bp[0].nodes[0]) != NodeType::Corner);
}

void testRegions() {
    Fixture f;
    f.run("vector.new width=200 height=100");
    // compound: a hole where the inner square sits
    std::string outer = f.field(f.run("vector.add_rect doc=vec1 x=10 y=10 width=80 height=80 fill=red"), "shape");
    std::string inner = f.field(f.run("vector.add_rect doc=vec1 x=30 y=30 width=40 height=40 fill=blue"), "shape");
    Result r = f.run("vector.compound doc=vec1 shapes=" + outer + "," + inner);
    CHECK(r.ok && f.doc().shapes.size() == 1);
    const std::string comp = f.field(r, "shape");
    CHECK(isColor(f.px(50, 50), 255, 255, 255) && isColor(f.px(20, 20), 255, 0, 0));
    r = f.run("vector.break_apart doc=vec1 shape=" + comp);
    CHECK(r.ok && f.doc().shapes.size() == 2);
    CHECK(isColor(f.px(50, 50), 255, 0, 0));  // both pieces filled now
    CHECK(f.run("undo").ok);
    r = f.run("vector.break_apart doc=vec1 shape=" + comp + " holes=true");
    CHECK(!r.ok);  // one piece with its hole
    // divide: two overlapping squares make three regions
    Fixture g;
    g.run("vector.new width=200 height=100");
    const std::string a = g.field(g.run("vector.add_rect doc=vec1 x=10 y=10 width=60 height=60 fill=red"), "shape");
    const std::string b = g.field(g.run("vector.add_rect doc=vec1 x=40 y=10 width=60 height=60 fill=blue"), "shape");
    r = g.run("vector.divide doc=vec1 shapes=" + a + "," + b);
    CHECK(r.ok && g.field(r, "count") == "3" && g.doc().shapes.size() == 3);
    double total = 0;
    int blue = 0;
    for (const auto& s : g.doc().shapes) {
        total += outlineArea(outline(s));
        if (s.fill == Color{0, 0, 255, 255}) ++blue;
    }
    CHECK(near(total, 3600 + 3600 - 1800, 2));  // two 60x60 squares overlapping by 30x60
    CHECK(blue == 2);  // the overlap takes the top (blue) shape's style
    CHECK(g.run("undo").ok && g.doc().shapes.size() == 2);
    // shape builder: merge the left part with the overlap, erase the right part
    r = g.run("vector.shape_builder doc=vec1 shapes=" + a + "," + b + " merge=20,40,55,40 erase=90,40");
    CHECK(r.ok && g.doc().shapes.size() == 1);
    const Bounds mb = g.box(g.field(r, "shape"));
    CHECK(near(mb.x0, 10, 0.1) && near(mb.x1, 70, 0.1));
    CHECK(g.shape(g.field(r, "shape")).fill == Color{255, 0, 0, 255});
    CHECK(g.fails("vector.shape_builder doc=vec1 shapes=" + g.field(r, "shape")));
}

void testPaintAndMasks() {
    Fixture f;
    f.run("vector.new width=100 height=100");
    // four separate lines that cross make a closed box; the bucket fills inside it
    f.run("vector.add_line doc=vec1 x1=10 y1=20 x2=90 y2=20 stroke=black stroke_width=2");
    f.run("vector.add_line doc=vec1 x1=10 y1=80 x2=90 y2=80 stroke=black stroke_width=2");
    f.run("vector.add_line doc=vec1 x1=20 y1=10 x2=20 y2=90 stroke=black stroke_width=2");
    f.run("vector.add_line doc=vec1 x1=80 y1=10 x2=80 y2=90 stroke=black stroke_width=2");
    Result r = f.run("vector.paint_region doc=vec1 x=50 y=50 fill=#00ff00");
    CHECK(r.ok);
    CHECK(isColor(f.px(50, 50), 0, 255, 0) && isColor(f.px(23, 23), 0, 255, 0) && isColor(f.px(15, 50), 255, 255, 255));
    CHECK(f.doc().find(f.field(r, "shape")) == 0);  // under the lines
    const Bounds b = f.box(f.field(r, "shape"));
    CHECK(b.x0 > 19 && b.x0 < 21 && b.x1 > 79 && b.x1 < 81);
    CHECK(f.fails("vector.paint_region doc=vec1 x=5 y=5 fill=red"));

    // clipping mask: a circle clips a square
    Fixture c;
    c.run("vector.new width=100 height=100");
    const std::string sq = c.field(c.run("vector.add_rect doc=vec1 x=10 y=10 width=80 height=80 fill=red"), "shape");
    const std::string circle = c.field(c.run("vector.add_ellipse doc=vec1 cx=50 cy=50 rx=30 fill=blue"), "shape");
    r = c.run("vector.clip doc=vec1 shapes=" + sq + "," + circle);
    CHECK(r.ok && c.field(r, "clip") == circle);
    const std::string group = c.field(r, "shape");
    CHECK(isColor(c.px(50, 50), 255, 0, 0) && isColor(c.px(15, 15), 255, 255, 255));
    CHECK(near(c.box(group).width(), 60, 0.5));  // the group's bounds are the clip's
    CHECK(c.run("vector.release_clip doc=vec1 shape=" + group).ok);
    CHECK(isColor(c.px(15, 15), 255, 0, 0));
    CHECK(c.run("vector.ungroup doc=vec1 shape=" + group).ok && c.shape(sq).parent.empty());

    // opacity mask: a white-to-black gradient fades a red square from left to right
    Fixture m;
    m.run("vector.new width=100 height=100");
    const std::string art = m.field(m.run("vector.add_rect doc=vec1 x=0 y=0 width=100 height=100 fill=red"), "shape");
    const std::string fade = m.field(m.run("vector.add_rect doc=vec1 x=0 y=0 width=100 height=100 fill=white"), "shape");
    m.run("vector.gradient doc=vec1 shape=" + fade + " colors=white,black");
    r = m.run("vector.mask doc=vec1 shapes=" + art + "," + fade);
    CHECK(r.ok);
    CHECK(isColor(m.px(2, 50), 255, 0, 0, 20) && isColor(m.px(97, 50), 255, 255, 255, 20));
    const Color mid = m.px(50, 50);
    CHECK(mid.r > 240 && mid.g > 90 && mid.g < 170);
}

void testAppearance() {
    Fixture f;
    f.run("vector.new width=120 height=100");
    // dashes with butt caps leave gaps
    const std::string line = f.field(f.run("vector.add_line doc=vec1 x1=0 y1=20 x2=100 y2=20 stroke=black stroke_width=6"), "shape");
    CHECK(f.run("vector.stroke_style doc=vec1 shape=" + line + " cap=butt dash=10,10").ok);
    CHECK(isColor(f.px(5, 20), 0, 0, 0) && isColor(f.px(15, 20), 255, 255, 255));
    // arrowhead past the end, inside alignment, profiles
    CHECK(f.run("vector.stroke_style doc=vec1 shape=" + line + " dash=none end_arrow=triangle").ok);
    CHECK(f.shape(line).line.endArrow == "triangle" && f.shape(line).line.dash.empty());
    CHECK(isColor(f.px(100, 20), 0, 0, 0) && isColor(f.px(81, 27), 0, 0, 0));  // the arrowhead is wider than the line
    CHECK(f.fails("vector.stroke_style doc=vec1 shape=" + line + " cap=pointy"));
    // appearance: a wide blue outline under a red fill shows as a halo
    const std::string box = f.field(f.run("vector.add_rect doc=vec1 x=30 y=40 width=40 height=40 fill=red"), "shape");
    CHECK(f.run("vector.appearance doc=vec1 shape=" + box + " add=\"stroke color=blue width=10 under\"").ok);
    CHECK(f.shape(box).appearance.size() == 1);
    CHECK(isColor(f.px(27, 60), 0, 0, 255) && isColor(f.px(50, 60), 255, 0, 0));
    CHECK(f.run("vector.appearance doc=vec1 shape=" + box + " clear=true").ok && f.shape(box).appearance.empty());
    // effects: a hard drop shadow
    CHECK(f.run("vector.effect doc=vec1 shape=" + box + " type=drop_shadow dx=10 dy=10 radius=0.5 opacity=1 color=black").ok);
    CHECK(isColor(f.px(75, 85), 0, 0, 0, 40) && isColor(f.px(50, 60), 255, 0, 0));
    CHECK(f.run("vector.effect doc=vec1 shape=" + box + " remove=drop_shadow").ok && f.shape(box).effects.empty());
    CHECK(f.fails("vector.effect doc=vec1 shape=" + box + " type=sparkle"));
    // pattern: stripes of two colours
    CHECK(f.run("vector.pattern doc=vec1 shape=" + box + " type=checker color=black background=white size=10").ok);
    raster::Image img = render(f.doc(), 1.0);
    int dark = 0, light = 0;
    for (int y = 42; y < 78; ++y) {
        for (int x = 32; x < 68; ++x) {
            const auto* p = img.pixel(x, y);
            if (p[0] < 40) ++dark;
            if (p[0] > 215 && p[1] > 215) ++light;
        }
    }
    CHECK(dark > 300 && light > 300);
    // swatches: change the swatch, every use follows
    CHECK(f.run("vector.swatch doc=vec1 name=brand color=#ff8800").ok);
    CHECK(f.run("vector.pattern doc=vec1 shape=" + box + " remove=true").ok);
    CHECK(f.run("vector.swatch doc=vec1 name=brand shapes=" + box).ok && f.shape(box).fillSwatch == "brand");
    CHECK(f.run("vector.swatch doc=vec1 name=brand color=#0080ff").ok);
    CHECK(f.shape(box).fill == Color{0, 0x80, 0xff, 255});
    CHECK(f.run("vector.style doc=vec1 shape=" + box + " fill=red").ok && f.shape(box).fillSwatch.empty());
    // blend modes: yellow multiplied over cyan gives green
    Fixture b;
    b.run("vector.new width=50 height=50");
    b.run("vector.add_rect doc=vec1 x=0 y=0 width=50 height=50 fill=cyan");
    const std::string top = b.field(b.run("vector.add_rect doc=vec1 x=0 y=0 width=50 height=50 fill=yellow"), "shape");
    CHECK(b.run("vector.style doc=vec1 shape=" + top + " blend=multiply").ok);
    CHECK(isColor(b.px(25, 25), 0, 255, 0));
    CHECK(b.fails("vector.style doc=vec1 shape=" + top + " blend=sideways"));
    // gradients on outlines and freeform fills
    const std::string g = b.field(b.run("vector.add_rect doc=vec1 x=0 y=0 width=50 height=50 fill=white stroke=black stroke_width=4"), "shape");
    CHECK(b.run("vector.gradient doc=vec1 shape=" + g + " target=stroke colors=red,blue").ok && b.shape(g).strokeGradient);
    CHECK(b.run("vector.gradient doc=vec1 shape=" + g + " type=freeform points=\"0,0,red 1,1,blue\"").ok);
    CHECK(b.shape(g).gradient && b.shape(g).gradient->freeform);
    CHECK(isColor(b.px(8, 8), 255, 0, 0, 60) && isColor(b.px(42, 42), 0, 0, 255, 60));
}

void testOrganize() {
    Fixture f;
    f.run("vector.new width=200 height=200");
    Result r = f.run("vector.add_layer doc=vec1 name=Background");
    const std::string bg = f.field(r, "shape");
    const std::string a = f.field(f.run("vector.add_rect doc=vec1 x=10 y=10 width=20 height=20 fill=red"), "shape");
    CHECK(f.shape(a).parent == bg);
    const std::string fg = f.field(f.run("vector.add_layer doc=vec1 name=Art"), "shape");
    const std::string b = f.field(f.run("vector.add_rect doc=vec1 x=50 y=30 width=30 height=10 fill=blue"), "shape");
    const std::string c = f.field(f.run("vector.add_ellipse doc=vec1 cx=150 cy=100 rx=10 fill=green"), "shape");
    CHECK(f.shape(b).parent == fg);
    r = f.run("vector.layers doc=vec1");
    CHECK(f.field(r, "layers") == bg + "," + fg && f.field(r, "current") == fg);
    // hiding a layer hides what's in it; locking keeps clicks off
    CHECK(f.run("vector.style doc=vec1 shape=" + bg + " visible=false").ok && isColor(f.px(20, 20), 255, 255, 255));
    CHECK(f.run("vector.style doc=vec1 shape=" + bg + " visible=true").ok && isColor(f.px(20, 20), 255, 0, 0));
    CHECK(f.field(f.run("vector.hit doc=vec1 x=20 y=20"), "shape") == a);
    CHECK(f.run("vector.lock doc=vec1 shape=" + bg).ok);
    CHECK(f.field(f.run("vector.hit doc=vec1 x=20 y=20"), "shape").empty());
    CHECK(f.fails("vector.transform doc=vec1 shape=" + a + " dx=5"));
    CHECK(f.run("vector.lock doc=vec1 shape=" + bg + " locked=false").ok);
    // move between layers; the shape lands on top of the target layer
    CHECK(f.run("vector.move_to doc=vec1 shape=" + a + " into=" + fg).ok && f.shape(a).parent == fg);
    // group: clicks select the group; transforms move everything inside
    r = f.run("vector.group doc=vec1 shapes=" + b + "," + c);
    const std::string grp = f.field(r, "shape");
    CHECK(r.ok && f.shape(b).parent == grp && f.shape(grp).parent == fg);
    r = f.run("vector.hit doc=vec1 x=60 y=35");
    CHECK(f.field(r, "shape") == b && f.field(r, "select") == grp);
    CHECK(f.run("vector.transform doc=vec1 shape=" + grp + " dx=10 dy=5").ok);
    CHECK(near(f.box(b).x0, 60, 1e-6) && near(f.box(c).y0, 95, 1e-6));
    // arrange among siblings: the group goes under shape a within the layer
    const auto sibs = [&] { return f.doc().children(fg); };
    CHECK(f.run("vector.reorder doc=vec1 shape=" + grp + " to=back").ok);
    CHECK(f.doc().shapes[static_cast<std::size_t>(sibs().front())].id == grp);
    CHECK(f.run("vector.reorder doc=vec1 shape=" + grp + " to=front").ok);
    CHECK(f.doc().shapes[static_cast<std::size_t>(sibs().back())].id == grp);
    CHECK(f.run("vector.ungroup doc=vec1 shape=" + grp).ok && f.shape(b).parent == fg && f.doc().find(grp) < 0);
    // select by area
    r = f.run("vector.select_area doc=vec1 x=0 y=0 width=100 height=100");
    CHECK(f.field(r, "count") == "2");
    // deleting a layer removes what's in it
    CHECK(f.run("vector.delete doc=vec1 shape=" + fg).ok && f.doc().shapes.size() == 1);
    CHECK(f.run("undo").ok && f.doc().shapes.size() == 5);
}

void testAlignTransform() {
    Fixture f;
    f.run("vector.new width=300 height=200");
    const std::string a = f.field(f.run("vector.add_rect doc=vec1 x=10 y=10 width=20 height=20"), "shape");
    const std::string b = f.field(f.run("vector.add_rect doc=vec1 x=60 y=50 width=40 height=20"), "shape");
    const std::string c = f.field(f.run("vector.add_rect doc=vec1 x=200 y=90 width=10 height=30"), "shape");
    {  // every shape's box, for the window's smart guides
        Result boxes = f.run("vector.boxes doc=vec1 except=" + a);
        const std::string text = f.field(boxes, "boxes");
        CHECK(boxes.ok && text.find(b + " 60 50 40 20") != std::string::npos && text.find(a + " ") == std::string::npos);
    }
    CHECK(f.run("vector.align doc=vec1 shapes=" + a + "," + b + "," + c + " horizontal=left").ok);
    CHECK(near(f.box(b).x0, 10) && near(f.box(c).x0, 10));
    CHECK(f.run("vector.align doc=vec1 shapes=" + a + "," + b + " vertical=bottom to=key key=" + c).ok);
    CHECK(near(f.box(a).y1, 120) && near(f.box(b).y1, 120) && near(f.box(c).y1, 120));
    CHECK(f.run("vector.align doc=vec1 shape=" + a + " horizontal=right margin=5").ok && near(f.box(a).x1, 295));
    f.run("undo");
    f.run("undo");
    f.run("undo");
    // distribute: equal gaps across
    CHECK(f.run("vector.distribute doc=vec1 shapes=" + a + "," + b + "," + c).ok);
    const double g1 = f.box(b).x0 - f.box(a).x1, g2 = f.box(c).x0 - f.box(b).x1;
    CHECK(near(g1, g2, 1e-6) && near(f.box(a).x0, 10) && near(f.box(c).x1, 210));
    CHECK(f.run("vector.distribute doc=vec1 shapes=" + a + "," + b + "," + c + " gap=5").ok);
    CHECK(near(f.box(b).x0 - f.box(a).x1, 5) && near(f.box(c).x0 - f.box(b).x1, 5));
    CHECK(f.fails("vector.distribute doc=vec1 shapes=" + a + "," + b));
    // rotating two shapes a quarter turn around their shared centre
    Fixture t;
    t.run("vector.new width=200 height=200");
    const std::string p = t.field(t.run("vector.add_rect doc=vec1 x=0 y=90 width=20 height=20"), "shape");
    const std::string q = t.field(t.run("vector.add_rect doc=vec1 x=180 y=90 width=20 height=20"), "shape");
    CHECK(t.run("vector.transform doc=vec1 shapes=" + p + "," + q + " rotate=90").ok);
    CHECK(near(t.box(p).x0, 90, 1e-6) && near(t.box(p).y0, 0, 1e-6) && near(t.box(q).y1, 200, 1e-6));
    // flip and each
    CHECK(t.run("vector.transform doc=vec1 shapes=" + p + "," + q + " scale=2 each=true").ok);
    CHECK(near(t.box(p).width(), 40, 1e-6) && near(t.box(p).x0, 80, 1e-6));
    const std::string tri = t.field(t.run("vector.add_polygon doc=vec1 points=10,10,40,10,10,40"), "shape");
    CHECK(t.run("vector.transform doc=vec1 shape=" + tri + " flip=horizontal").ok);
    {
        const Outline o = outlineIn(t.doc(), t.doc().find(tri));
        CHECK(near(bounds(o).x0, 10, 1e-6) && near(bounds(o).x1, 40, 1e-6));
        // the right angle is now on the right
        bool cornerRight = false;
        for (const auto& pt : o[0].points) cornerRight |= near(pt.x, 40, 1e-6) && near(pt.y, 40, 1e-6);
        CHECK(cornerRight);
    }
    // skew bakes into the geometry
    CHECK(t.run("vector.transform doc=vec1 shape=" + tri + " skew_x=45").ok);
    CHECK(t.shape(tri).kind == ShapeKind::Path && t.shape(tri).transform.identity());
    CHECK(near(t.box(tri).width(), 60, 1e-3));
}

void testRepeatSymbols() {
    Fixture f;
    f.run("vector.new width=400 height=400");
    const std::string a = f.field(f.run("vector.add_rect doc=vec1 x=10 y=10 width=20 height=10 fill=red"), "shape");
    Result r = f.run("vector.repeat doc=vec1 shape=" + a + " mode=grid rows=2 columns=3 gap_x=5 gap_y=5");
    CHECK(r.ok && f.field(r, "copies") == "5");
    const std::string grid = f.field(r, "shape");
    CHECK(f.doc().children(grid).size() == 6);
    CHECK(near(f.box(grid).x1, 10 + 3 * 20 + 2 * 5) && near(f.box(grid).y1, 10 + 2 * 10 + 5));
    f.run("undo");
    r = f.run("vector.repeat doc=vec1 shape=" + a + " mode=radial count=4 cx=20 cy=50");
    CHECK(r.ok && f.doc().children(f.field(r, "shape")).size() == 4);
    CHECK(near(f.box(f.field(r, "shape")).y1, 90, 1e-6));  // the copy opposite sits 40 px below the centre's mirror
    f.run("undo");
    r = f.run("vector.repeat doc=vec1 shape=" + a + " mode=mirror x=50");
    CHECK(r.ok && near(f.box(f.field(r, "shape")).x1, 90, 1e-6));
    f.run("undo");
    // symbols: the art becomes an instance; more instances follow the symbol
    const std::string b = f.field(f.run("vector.add_ellipse doc=vec1 cx=50 cy=15 rx=5 fill=blue"), "shape");
    r = f.run("vector.symbol doc=vec1 name=badge shapes=" + a + "," + b);
    CHECK(r.ok && f.doc().shapes.size() == 1);
    const std::string inst = f.field(r, "shape");
    CHECK(f.shape(inst).kind == ShapeKind::Instance && near(f.box(inst).x0, 10) && near(f.box(inst).x1, 55));
    r = f.run("vector.place_symbol doc=vec1 name=badge x=100 y=200");
    const std::string inst2 = f.field(r, "shape");
    CHECK(near(f.box(inst2).x0, 100) && near(f.box(inst2).y0, 200));
    CHECK(isColor(f.px(105, 205), 255, 0, 0));
    CHECK(f.run("vector.transform doc=vec1 shape=" + inst2 + " scale=2").ok && near(f.box(inst2).width(), 90, 1e-6));
    // redefine: every instance changes
    const std::string green = f.field(f.run("vector.add_rect doc=vec1 x=0 y=0 width=10 height=10 fill=green"), "shape");
    CHECK(f.run("vector.symbol doc=vec1 name=badge shapes=" + green + " keep=true").ok);
    CHECK(near(f.box(inst).width(), 10));
    // expand: editable shapes again
    r = f.run("vector.expand_symbol doc=vec1 shape=" + inst2);
    CHECK(r.ok && f.shape(f.field(r, "shape")).kind == ShapeKind::Group && f.doc().find(inst2) < 0);
    CHECK(near(f.box(f.field(r, "shape")).width(), 20, 1e-6));
    CHECK(f.fails("vector.place_symbol doc=vec1 name=nothing"));
}

void testArtboardsAndSnap() {
    Fixture f;
    f.run("vector.new width=100 height=100");
    CHECK(f.run("vector.add_artboard doc=vec1 name=front x=0 y=0 width=100 height=100").ok);
    CHECK(f.run("vector.add_artboard doc=vec1 name=back x=120 y=0 width=50 height=80").ok);
    CHECK(f.doc().width == 170 && f.doc().height == 100);
    CHECK(f.fails("vector.add_artboard doc=vec1 name=front x=0 y=0 width=10 height=10"));
    f.run("vector.add_rect doc=vec1 x=130 y=10 width=20 height=20 fill=red");
    const auto png = tmp("art.png");
    Result r = f.run("vector.save doc=vec1 path=" + png.string() + " artboard=all");
    CHECK(r.ok);
    const auto backPng = tmp("art-back.png");
    CHECK(std::filesystem::exists(tmp("art-front.png")) && std::filesystem::exists(backPng));
    if (std::filesystem::exists(backPng)) {
        raster::Image img = raster::load(backPng);
        CHECK(img.width() == 50 && img.height() == 80 && img.pixel(15, 15)[0] == 255 && img.pixel(15, 15)[1] == 0);
    }
    const auto pdf = tmp("art.pdf");
    CHECK(f.run("vector.save doc=vec1 path=" + pdf.string() + " artboard=all").ok);
    std::string bytes;
    {
        std::ifstream in(pdf, std::ios::binary);  // closed before the file is removed below (windows)
        bytes.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    }
    std::size_t pages = 0;
    for (std::size_t at = bytes.find("/Type /Page"); at != std::string::npos; at = bytes.find("/Type /Page", at + 1)) {
        if (bytes.compare(at, 12, "/Type /Pages") != 0) ++pages;
    }
    CHECK(pages == 2);
    CHECK(f.run("vector.save doc=vec1 path=" + tmp("one.svg").string() + " artboard=back").ok);
    auto one = openSvg(tmp("one.svg"));
    CHECK(one->width == 50 && one->height == 80);
    CHECK(f.fails("vector.save doc=vec1 path=x.png artboard=nope"));
    CHECK(f.field(f.run("vector.artboards doc=vec1"), "count") == "2");
    CHECK(f.run("vector.remove_artboard doc=vec1 artboard=2").ok && f.doc().artboards.size() == 1);
    for (const auto& p : {png, backPng, tmp("art-front.png"), pdf, tmp("one.svg")}) removeFile(p);
    // grid, guides and snapping
    CHECK(f.run("vector.grid doc=vec1 size=10 snap_grid=true").ok);
    r = f.run("vector.snap doc=vec1 x=13 y=17");
    CHECK(f.field(r, "x") == "10" && f.field(r, "y") == "20" && f.field(r, "snapped") == "grid");
    CHECK(f.run("vector.guides doc=vec1 x=33").ok);
    CHECK(f.run("vector.grid doc=vec1 snap_grid=false snap_guides=true").ok);
    r = f.run("vector.snap doc=vec1 x=31 y=17");
    CHECK(f.field(r, "x") == "33" && f.field(r, "y") == "17");
    CHECK(f.run("vector.grid doc=vec1 snap_points=true").ok);
    r = f.run("vector.snap doc=vec1 x=128 y=12");
    CHECK(f.field(r, "x") == "130" && f.field(r, "y") == "10" && f.field(r, "snapped") == "point");
}

void testType() {
    Fixture f;
    f.run("vector.new width=400 height=300");
    Result r = f.run("vector.add_text doc=vec1 text=\"the quick brown fox jumps over the lazy dog again and again\" x=10 y=10 size=20 box_width=150 box_height=200");
    const std::string area = f.field(r, "shape");
    CHECK(r.ok && wrapText(f.shape(area)).size() >= 3);
    const Bounds ab = f.box(area);
    CHECK(ab.x1 <= 161 && ab.y0 >= 10);
    r = f.run("vector.set_text doc=vec1 shape=" + area + " box_height=30");
    CHECK(r.ok && f.field(r, "overflow") == "true");
    CHECK(f.run("vector.set_text doc=vec1 shape=" + area + " box_height=280 box_width=300 columns=2 column_gap=20 align=justify").ok);
    CHECK(f.shape(area).columns == 2 && f.shape(area).align == "justify");
    CHECK(f.fails("vector.set_text doc=vec1 shape=" + area + " align=sideways"));
    // text on a path follows the path, and moves with it
    const std::string curve = f.field(f.run("vector.add_path doc=vec1 d=\"M20 250 C 120 150, 220 350, 380 250\" fill=none stroke=gray"), "shape");
    r = f.run("vector.add_text doc=vec1 text=\"along the curve\" x=0 y=0 size=18 on_path=" + curve);
    const std::string onp = f.field(r, "shape");
    CHECK(r.ok && f.shape(onp).onPath == curve);
    Bounds tb = f.box(onp);
    CHECK(tb.x0 > 15 && tb.y0 > 180 && tb.y1 < 320);
    CHECK(f.run("vector.transform doc=vec1 shape=" + curve + " dy=-100").ok);
    CHECK(near(f.box(onp).y0, tb.y0 - 100, 1.0));
    CHECK(f.fails("vector.add_text doc=vec1 text=x x=0 y=0 on_path=" + area));
    // case and styles
    CHECK(f.run("vector.set_text doc=vec1 shape=" + onp + " case=upper").ok && f.shape(onp).textCase == "upper");
    CHECK(f.run("vector.text_style doc=vec1 name=heading font=serif size=32 fill=navy case=title shapes=" + area).ok);
    CHECK(near(f.shape(area).fontSize, 32) && f.shape(area).textStyle == "heading" && f.shape(area).fill == Color{0, 0, 128, 255});
    CHECK(f.run("vector.text_style doc=vec1 name=heading size=24").ok && near(f.shape(area).fontSize, 24) && f.shape(area).font == "serif");
    r = f.run("vector.add_text doc=vec1 text=Title x=10 y=290 style=heading");
    CHECK(r.ok && near(f.shape(f.field(r, "shape")).fontSize, 24));
    CHECK(f.fails("vector.set_text doc=vec1 shape=" + area + " style=missing"));
    // outline text: letters become a path
    CHECK(f.run("vector.to_path doc=vec1 shape=" + onp).ok && f.shape(onp).kind == ShapeKind::Path);
}

void testTrace() {
    raster::Image img(40, 30);
    for (int y = 0; y < 30; ++y) {
        for (int x = 0; x < 40; ++x) {
            std::uint8_t* p = img.pixel(x, y);
            const bool ink = x >= 10 && x < 30 && y >= 5 && y < 25;
            p[0] = p[1] = p[2] = ink ? 0 : 255;
            p[3] = 255;
        }
    }
    const auto file = tmp("trace.png");
    raster::save(img, file);
    Fixture f;
    f.run("vector.new width=200 height=200");
    Result r = f.run("vector.trace doc=vec1 path=" + file.string() + " smooth=0");
    CHECK(r.ok && f.field(r, "paths") == "1");
    const std::string g = f.field(r, "shape");
    const Bounds b = f.box(g);
    CHECK(near(b.x0, 10, 0.01) && near(b.x1, 30, 0.01) && near(b.y0, 5, 0.01) && near(b.y1, 25, 0.01));
    // placed and scaled, smoothed
    r = f.run("vector.trace doc=vec1 path=" + file.string() + " x=100 y=100 width=80");
    const Bounds b2 = f.box(f.field(r, "shape"));
    CHECK(near(b2.x0, 120, 2) && near(b2.x1, 160, 2));
    // colours: a red and a blue block
    for (int y = 0; y < 30; ++y) {
        for (int x = 0; x < 40; ++x) {
            std::uint8_t* p = img.pixel(x, y);
            p[0] = x < 20 ? 255 : 0;
            p[1] = 0;
            p[2] = x < 20 ? 0 : 255;
        }
    }
    raster::save(img, file);
    r = f.run("vector.trace doc=vec1 path=" + file.string() + " colors=3");
    CHECK(r.ok && f.field(r, "paths") == "2");
    removeFile(file);
    CHECK(f.fails("vector.trace doc=vec1 path=/nonexistent.png"));
}

void testRoundTrips() {
    Fixture f;
    f.run("vector.new width=300 height=200");
    f.run("vector.add_layer doc=vec1 name=Main");
    const std::string a = f.field(f.run("vector.add_rect doc=vec1 x=10 y=10 width=50 height=40 fill=red stroke=black stroke_width=3"), "shape");
    f.run("vector.stroke_style doc=vec1 shape=" + a + " cap=square join=bevel dash=4,2 align=inside");
    f.run("vector.appearance doc=vec1 shape=" + a + " add=\"stroke color=gold width=8 under\"");
    f.run("vector.effect doc=vec1 shape=" + a + " type=drop_shadow dx=3 dy=3");
    const std::string b = f.field(f.run("vector.add_ellipse doc=vec1 cx=120 cy=60 rx=30 fill=blue"), "shape");
    f.run("vector.pattern doc=vec1 shape=" + b + " type=dots color=white size=8");
    f.run("vector.style doc=vec1 shape=" + b + " blend=multiply");
    const std::string c = f.field(f.run("vector.add_rect doc=vec1 x=100 y=40 width=60 height=40 fill=green"), "shape");
    const std::string cut = f.field(f.run("vector.add_ellipse doc=vec1 cx=130 cy=60 rx=20 fill=black"), "shape");
    const std::string clipGroup = f.field(f.run("vector.clip doc=vec1 shapes=" + c + "," + cut), "shape");
    f.run("vector.swatch doc=vec1 name=brand color=#123456");
    f.run("vector.text_style doc=vec1 name=body size=14");
    f.run("vector.add_artboard doc=vec1 name=page x=0 y=0 width=300 height=200");
    f.run("vector.guides doc=vec1 x=50 y=60");
    const std::string sym = f.field(f.run("vector.add_ellipse doc=vec1 cx=250 cy=150 rx=10 fill=purple"), "shape");
    const std::string inst = f.field(f.run("vector.symbol doc=vec1 name=dot shapes=" + sym), "shape");
    f.run("vector.place_symbol doc=vec1 name=dot x=200 y=20");
    const raster::Image before = render(f.doc(), 1.0);

    const std::string svg = toSvg(f.doc());
    auto back = fromSvg(svg);
    CHECK(back->shapes.size() == f.doc().shapes.size());
    const int ia = back->find(a);
    CHECK(ia >= 0);
    if (ia >= 0) {
        const Shape& s = back->shapes[static_cast<std::size_t>(ia)];
        CHECK(s.line.cap == "square" && s.line.join == "bevel" && s.line.dash.size() == 2 && s.line.align == "inside");
        CHECK(s.appearance.size() == 1 && s.appearance[0].under && near(s.appearance[0].width, 8));
        CHECK(s.effects.size() == 1 && s.effects[0].type == "drop_shadow");
        CHECK(!s.parent.empty() && back->shapes[static_cast<std::size_t>(back->find(s.parent))].layer);
    }
    const int ib = back->find(b);
    CHECK(ib >= 0 && back->shapes[static_cast<std::size_t>(ib)].pattern && back->shapes[static_cast<std::size_t>(ib)].blend == "multiply");
    const int ig = back->find(clipGroup);
    CHECK(ig >= 0 && back->shapes[static_cast<std::size_t>(ig)].clip == cut);
    CHECK(back->swatch("brand") && back->textStyles.size() == 1 && back->artboards.size() == 1 && back->guidesX.size() == 1);
    CHECK(back->findSymbol("dot") && back->find(inst) >= 0 && back->shapes[static_cast<std::size_t>(back->find(inst))].kind == ShapeKind::Instance);
    // the reopened drawing looks the same
    const raster::Image after = render(*back, 1.0);
    long diff = 0;
    for (int y = 0; y < before.height(); ++y) {
        for (int x = 0; x < before.width(); ++x) {
            for (int k = 0; k < 4; ++k) diff += std::abs(before.pixel(x, y)[k] - after.pixel(x, y)[k]) > 8 ? 1 : 0;
        }
    }
    CHECK(diff < before.width() * before.height() / 200);
    CHECK(toSvg(*back) == svg);  // and writes the same file again

    // PDF and EPS keep the shapes as vectors that open again
    Fixture g;
    g.run("vector.new width=200 height=100 background=none");
    g.run("vector.add_rect doc=vec1 x=10 y=10 width=80 height=40 fill=red");
    g.run("vector.add_ellipse doc=vec1 cx=150 cy=50 rx=30 ry=20 fill=none stroke=blue stroke_width=4");
    for (const char* ext : {".pdf", ".eps", ".ai"}) {
        const auto file = tmp(std::string("rt") + ext);
        CHECK(g.run("vector.save doc=vec1 path=" + file.string()).ok);
        Result r = g.run("vector.open path=" + file.string());
        CHECK(r.ok);
        auto opened = g.session.workspace().getAs<VectorDoc>(g.field(r, "doc"));
        CHECK(opened && opened->width == 200 && opened->height == 100);
        if (!opened) continue;
        int rects = 0, rings = 0;
        for (int i = 0; i < static_cast<int>(opened->shapes.size()); ++i) {
            const Shape& s = opened->shapes[static_cast<std::size_t>(i)];
            if (s.kind == ShapeKind::Group) continue;
            const Bounds bb = shapeBounds(*opened, i);
            if (s.fill && s.fill->r == 255 && s.fill->g == 0 && near(bb.x0, 10, 0.5) && near(bb.width(), 80, 0.5)) ++rects;
            if (s.stroke && s.stroke->b == 255 && near(bb.width(), 60, 0.5) && near(s.strokeWidth, 4, 0.1)) ++rings;
        }
        CHECK(rects == 1 && rings == 1);
        removeFile(file);
    }
    const auto pdf = tmp("conv.pdf"), out = tmp("conv.svg");
    g.run("vector.save doc=vec1 path=" + pdf.string());
    CHECK(g.run("vector.convert input=" + pdf.string() + " output=" + out.string()).ok && std::filesystem::exists(out));
    removeFile(pdf);
    removeFile(out);
}

void testToolsOverMcp() {
    Session session;
    registerCommands(session);
    mcp::Server server(session, "test", "0");
    const std::string list = server.handle(R"({"jsonrpc":"2.0","id":1,"method":"tools/list"})").value_or("");
    for (const char* tool : {"vector_pen", "vector_nodes", "vector_move_node", "vector_shape_builder", "vector_paint_region", "vector_clip",
                             "vector_mask", "vector_stroke_style", "vector_appearance", "vector_effect", "vector_add_layer", "vector_group",
                             "vector_distribute", "vector_repeat", "vector_symbol", "vector_add_artboard", "vector_trace", "vector_text_style",
                             "vector_hit", "vector_snap", "vector_cut", "vector_offset", "vector_outline_stroke", "vector_divide"}) {
        CHECK(list.find(std::string("\"") + tool + "\"") != std::string::npos);
    }
}

}  // namespace

void runToolTests() {
    testAnchors();
    testPathOps();
    testStrokeOps();
    testRegions();
    testPaintAndMasks();
    testAppearance();
    testOrganize();
    testAlignTransform();
    testRepeatSymbols();
    testArtboardsAndSnap();
    testType();
    testTrace();
    testRoundTrips();
    testToolsOverMcp();
}
