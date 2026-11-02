#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <random>
#include <sstream>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "wetwaresuite/board/board.hpp"
#include "wetwaresuite/core/cli.hpp"
#include "wetwaresuite/mcp/server.hpp"
#include "wetwaresuite/raster/codec.hpp"
#include "check.hpp"

// Defined by stb_image_write inside libs/raster.
extern "C" unsigned char* stbi_zlib_compress(unsigned char* data, int data_len, int* out_len, int quality);

using namespace wetwaresuite;
using namespace wetwaresuite::board;
namespace fs = std::filesystem;

namespace {

fs::path tempDir() {
    std::random_device rd;
    const auto stamp = static_cast<unsigned long long>(std::chrono::high_resolution_clock::now().time_since_epoch().count()) ^ rd();
    fs::path d = fs::temp_directory_path() / ("wetwaresuite_board_test_" + std::to_string(stamp));
    fs::remove_all(d);
    fs::create_directories(d);
    return d;
}

std::string q(const fs::path& p) { return "\"" + p.generic_string() + "\""; }

Result run(Session& s, const std::string& line) {
    cli::Batch batch(s);
    Result r = batch.runLine(line);
    if (!r.ok) std::printf("  (%s -> %s)\n", line.c_str(), r.message.c_str());
    return r;
}

std::string str(const Result& r, const std::string& key) {
    const Value* v = r.field(key);
    return v ? toText(*v) : std::string();
}

Element shape(Kind k, double x, double y, double w, double h, const std::string& name = {}) {
    Element e;
    e.kind = k;
    e.x = x;
    e.y = y;
    e.w = w;
    e.h = h;
    e.name = name;
    return e;
}

std::string connect(Board& b, const std::string& from, const std::string& to, const std::string& route = "straight") {
    Element c;
    c.kind = Kind::Connector;
    c.from = from;
    c.to = to;
    c.route = route;
    return b.add(c);
}

bool segmentHitsBox(Pt a, Pt c, const Box& bx) {
    // the segments here are axis-aligned
    const double x0 = std::min(a.x, c.x), x1 = std::max(a.x, c.x), y0 = std::min(a.y, c.y), y1 = std::max(a.y, c.y);
    return x1 > bx.x0 + 0.5 && x0 < bx.x1 - 0.5 && y1 > bx.y0 + 0.5 && y0 < bx.y1 - 0.5;
}

int nonBackground(const raster::Image& img, Color bg) {
    int n = 0;
    for (int y = 0; y < img.height(); ++y) {
        for (int x = 0; x < img.width(); ++x) {
            const Color c = img.at(x, y);
            if (std::abs(c.r - bg.r) + std::abs(c.g - bg.g) + std::abs(c.b - bg.b) > 30) ++n;
        }
    }
    return n;
}

void testModel() {
    CHECK(parseKind("circle") == Kind::Ellipse);
    CHECK(parseKind("Database") == Kind::Cylinder);
    CHECK(parseKind("sticky") == Kind::Note);
    CHECK(!parseKind("blob"));
    Board b;
    const std::string a = b.add(shape(Kind::Rect, 0, 0, 100, 50, "A"));
    const std::string c = b.add(shape(Kind::Ellipse, 300, 0, 100, 50, "C"));
    CHECK(a == "e1" && c == "e2");
    CHECK(b.find("A") == 0 && b.find("e2") == 1 && b.find("zz") == -1);
    const std::string link = connect(b, a, c);
    CHECK(b.remove({"C"}) == 2);  // the connector goes with it
    CHECK(b.find(link) == -1 && b.elements.size() == 1);
    // groups move their children
    Board g;
    const std::string x = g.add(shape(Kind::Rect, 0, 0, 50, 50));
    const std::string y = g.add(shape(Kind::Rect, 100, 0, 50, 50));
    Element grp;
    grp.kind = Kind::Group;
    const std::string gid = g.add(grp);
    g.get(x)->parent = gid;
    g.get(y)->parent = gid;
    refreshGroups(g);
    CHECK(g.get(gid)->w == 150);
    moveBy(g, *g.get(gid), 10, 20);
    CHECK(g.get(y)->x == 110 && g.get(y)->y == 20);
    // style resolution
    Board t;
    t.theme = "sketchy";
    Element n = shape(Kind::Note, 0, 0, 100, 100);
    Resolved s = resolve(t, n);
    CHECK(s.sketch && s.fill && !s.stroke);
    n.style.fill = "#ff0000";
    CHECK(resolve(t, n).fill->r == 255);
    CHECK(base64Decode(base64Encode("hello world!")) == "hello world!");
    CHECK(base64Encode("ab") == "YWI=");
}

void testConnectors() {
    Board b;
    const std::string a = b.add(shape(Kind::Rect, 0, 0, 100, 50));
    const std::string c = b.add(shape(Kind::Rect, 300, 0, 100, 50));
    const std::string s = connect(b, a, c);
    auto pts = connectorPath(b, *b.get(s));
    CHECK(pts.size() == 2);
    CHECK(std::abs(pts.front().x - 100) < 0.01 && std::abs(pts.front().y - 25) < 0.01);
    CHECK(std::abs(pts.back().x - 300) < 0.01);
    // attached: moving a shape moves the connector's end
    moveBy(b, *b.get(c), 0, 200);
    pts = connectorPath(b, *b.get(s));
    CHECK(pts.back().y > 150);
    // a diamond's boundary is inside its box corners
    Board d;
    const std::string da = d.add(shape(Kind::Diamond, 0, 0, 100, 100));
    const std::string db = d.add(shape(Kind::Rect, 300, 300, 50, 50));
    pts = connectorPath(d, *d.get(connect(d, da, db)));
    CHECK(std::abs(pts.front().x + pts.front().y - 150) < 0.5);  // on the edge x + y = 150
    // elbow routes go around a shape in the way
    Board e;
    const std::string l = e.add(shape(Kind::Rect, 0, 0, 80, 60));
    const std::string r = e.add(shape(Kind::Rect, 400, 0, 80, 60));
    const std::string mid = e.add(shape(Kind::Rect, 200, -60, 60, 180));
    pts = connectorPath(e, *e.get(connect(e, l, r, "elbow")));
    CHECK(pts.size() >= 4);
    const Element* m = e.get(mid);
    const Box mb{m->x, m->y, m->x + m->w, m->y + m->h, false};
    bool hit = false, orthogonal = true;
    for (std::size_t i = 1; i < pts.size(); ++i) {
        hit = hit || segmentHitsBox(pts[i - 1], pts[i], mb);
        orthogonal = orthogonal && (std::abs(pts[i].x - pts[i - 1].x) < 1e-6 || std::abs(pts[i].y - pts[i - 1].y) < 1e-6);
    }
    CHECK(!hit);
    CHECK(orthogonal);
    CHECK(std::abs(pts.front().x - 80) < 0.01 && std::abs(pts.back().x - 400) < 0.01);
    // curved and self loops
    Board f;
    const std::string fa = f.add(shape(Kind::Rect, 0, 0, 100, 50));
    const std::string fb = f.add(shape(Kind::Rect, 0, 200, 100, 50));
    CHECK(connectorPath(f, *f.get(connect(f, fa, fb, "curved"))).size() > 10);
    CHECK(connectorPath(f, *f.get(connect(f, fa, fa))).size() == 5);
    // ink smoothing keeps the ends
    std::vector<InkPoint> raw{{0, 0, 1}, {10, 3, 1}, {20, -2, 1}, {30, 4, 1}, {40, 0, 1}};
    auto sm = smoothInk(raw, 0.5);
    CHECK(sm.size() > raw.size());
    CHECK(sm.front().x == 0 && sm.back().x == 40);
}

void testLayout() {
    Board b;
    std::vector<std::string> ids;
    for (int i = 0; i < 6; ++i) ids.push_back(b.add(shape(Kind::Rect, 0, 0, 100, 50)));
    connect(b, ids[0], ids[1]);
    connect(b, ids[0], ids[2]);
    connect(b, ids[1], ids[3]);
    connect(b, ids[2], ids[3]);
    connect(b, ids[3], ids[4]);
    connect(b, ids[4], ids[0]);  // a cycle
    LayoutOptions o;
    std::string error;
    CHECK(layout(b, o, error) == 5);  // ids[5] isn't connected
    CHECK(error.empty());
    CHECK(b.get(ids[1])->y > b.get(ids[0])->y);
    CHECK(b.get(ids[3])->y > b.get(ids[1])->y);
    CHECK(std::abs(b.get(ids[1])->y - b.get(ids[2])->y) < 0.01);  // same rank
    CHECK(std::abs(b.get(ids[1])->x - b.get(ids[2])->x) >= 100);  // no overlap
    o.direction = "LR";
    layout(b, o, error);
    CHECK(b.get(ids[3])->x > b.get(ids[1])->x);
    for (const char* alg : {"tree", "mindmap", "radial", "force", "grid"}) {
        o.algorithm = alg;
        CHECK(layout(b, o, error) == 5);
        // nothing overlaps
        bool overlap = false;
        for (int i = 0; i < 5; ++i) {
            for (int j = i + 1; j < 5; ++j) {
                const Box bi = elementBox(b, *b.get(ids[static_cast<std::size_t>(i)])), bj = elementBox(b, *b.get(ids[static_cast<std::size_t>(j)]));
                overlap = overlap || bi.intersects(bj);
            }
        }
        if (overlap) std::printf("  overlap after %s\n", alg);
        CHECK(!overlap);
    }
    o.algorithm = "spiral";
    CHECK(layout(b, o, error) == -1 && !error.empty());
}

void testMermaid() {
    Board b;
    ImportOptions o;
    auto st = fromMermaid(b, R"(flowchart LR
  A([Start]) --> B{Ready?}
  B -- yes --> C[Ship it]
  B -->|no| D[(Backlog)]
  D -.-> B & E((Done))
  C ==> E
  F[/Input/] --- G{{Prep}}
  subgraph Ops [Operations team]
    C
    D
  end
  classDef warn fill:#fff3bf,stroke:#f08c00
  class D warn
  style E fill:#d3f9d8
  %% comment
)",
                          o);
    CHECK(st.kind == "flowchart");
    CHECK(st.nodes == 7);
    CHECK(st.edges == 7);
    CHECK(st.frames == 1);
    CHECK(b.get("A")->kind == Kind::Stadium && b.get("A")->text == "Start");
    CHECK(b.get("B")->kind == Kind::Diamond);
    CHECK(b.get("D")->kind == Kind::Cylinder && b.get("D")->style.fill == std::string("#fff3bf"));
    CHECK(b.get("E")->kind == Kind::Ellipse && std::abs(b.get("E")->w - b.get("E")->h) < 0.01);
    CHECK(b.get("E")->style.fill == std::string("#d3f9d8"));
    CHECK(b.get("F")->kind == Kind::Parallelogram && b.get("G")->kind == Kind::Hexagon);
    CHECK(b.get("C")->parent == b.get("Ops")->id && b.get("Ops")->text == "Operations team");
    int labelled = 0, dashed = 0, thick = 0, plain = 0;
    for (const auto& e : b.elements) {
        if (e.kind != Kind::Connector) continue;
        labelled += e.text == "yes" || e.text == "no";
        dashed += e.style.dash == std::string("dashed");
        thick += e.style.strokeWidth.value_or(0) > 3;
        plain += e.endArrow == "none";
    }
    CHECK(labelled == 2 && dashed == 2 && thick == 1 && plain == 1);
    // LR: the flow goes right
    CHECK(b.get("B")->x > b.get("A")->x);
    // the frame holds its nodes
    const Box f = elementBox(b, *b.get("Ops"));
    CHECK(f.contains(elementBox(b, *b.get("C"))) && f.contains(elementBox(b, *b.get("D"))));

    Board s;
    auto seq = fromMermaid(s, R"(sequenceDiagram
  participant A as Alice
  actor B as Bob
  A->>B: Hello
  B-->>A: Hi!
  loop every minute
    A-)B: ping
  end
  Note right of B: thinks
  B->>B: self
)",
                           o);
    CHECK(seq.kind == "sequence");
    CHECK(seq.edges == 4);
    CHECK(s.get("A")->text == "Alice" && s.get("B")->kind == Kind::Stadium);
    CHECK(s.get("A_end") && s.get("A_lifeline"));
    CHECK(seq.frames == 1);

    Board m;
    auto mm = fromMermaid(m, "mindmap\n  root((Trip))\n    Packing\n      Passport\n    Route\n", o);
    CHECK(mm.kind == "mindmap" && mm.nodes == 4 && mm.edges == 3);
    CHECK(m.elements.front().kind == Kind::Ellipse && m.elements.front().text == "Trip");

    Board crlf;  // Windows line endings
    auto cr = fromMermaid(crlf, "graph LR\r\n  A[One] --> B[Two]\r\n  B --> C\r\n", o);
    CHECK(cr.nodes == 3 && crlf.get("B")->text == "Two");
    Board crOutline;
    CHECK(fromOutline(crOutline, "- a\r\n  - b\r\n  - c\r\n", o).nodes == 3);
    CHECK(crOutline.elements.front().text == "a");
    Board ol;
    auto out = fromOutline(ol, "- Fruit\n  - Apple\n  - Pear\n- Veg\n    - Leek\n", o, "Food");
    CHECK(out.nodes == 6 && out.edges == 5);
    CHECK(ol.elements.front().text == "Food");
    Board h;
    auto hd = fromOutline(h, "# Plan\n## A\n- a1\n- a2\n## B\n- b1\n", o);
    CHECK(hd.nodes == 6 && hd.edges == 5);
    bool threw = false;
    try {
        Board e;
        fromMermaid(e, "   \n", o);
    } catch (const std::exception&) {
        threw = true;
    }
    CHECK(threw);
}

void testRender(const fs::path& dir) {
    Board b;
    const std::string a = b.add(shape(Kind::Rounded, 0, 0, 160, 70));
    b.get(a)->text = "Hello board";
    const std::string c = b.add(shape(Kind::Cylinder, 300, 0, 120, 90));
    b.add(shape(Kind::Cloud, 0, 150, 160, 100));
    b.add(shape(Kind::Note, 300, 150, 140, 140));
    Element lbl;
    lbl.kind = Kind::Connector;
    lbl.from = a;
    lbl.to = c;
    lbl.text = "reads";
    lbl.endArrow = "triangle";
    b.add(lbl);
    Element ink;
    ink.kind = Kind::Ink;
    for (int i = 0; i < 20; ++i) ink.ink.push_back({static_cast<double>(i * 10), 320 + std::sin(i * 0.5) * 10, i / 20.0});
    b.add(ink);
    RenderOptions o;
    raster::Image img = renderImage(b, o);
    CHECK(img.width() > 400 && img.height() > 300);
    const int clean = nonBackground(img, {255, 255, 255, 255});
    CHECK(clean > 2000);
    b.theme = "sketchy";
    raster::Image sk = renderImage(b, o);
    CHECK(nonBackground(sk, {255, 255, 255, 255}) > 2000);
    // the same scene twice gives the same pixels (seeded wobble)
    CHECK(raster::encodePng(sk) == raster::encodePng(renderImage(b, o)));
    b.theme = "dark";
    raster::Image dk = renderImage(b, o);
    CHECK(dk.at(2, 2).r < 60);
    o.transparent = true;
    CHECK(renderImage(b, o).at(1, 1).a == 0);
    o.transparent = false;
    // vector outputs
    const std::string svg = toSvg(b, o);
    CHECK(svg.find("<svg") != std::string::npos && svg.find(">Hello board</text>") != std::string::npos);
    CHECK(svg.find("<g id=\"e1\">") != std::string::npos);
    const std::string pdf = toPdf(b, o);
    CHECK(pdf.rfind("%PDF-", 0) == 0 && pdf.find("/Helvetica") != std::string::npos && pdf.find("%%EOF") != std::string::npos);
    // region and only
    RenderOptions only;
    only.only = {a};
    const Box r = renderRegion(b, only);
    CHECK(std::abs(r.w() - (160 + 64)) < 0.01);
    // images: file and data URL
    raster::Image pic(40, 20, Color{200, 30, 30, 255});
    raster::save(pic, dir / "pic.png");
    Element im = shape(Kind::Image, 0, 400, 80, 40);
    im.src = (dir / "pic.png").string();
    b.theme = "default";
    const std::string imId = b.add(im);
    RenderOptions io;
    io.only = {imId};
    io.padding = 0;
    raster::Image ir = renderImage(b, io);
    CHECK(ir.at(40, 20).r > 150 && ir.at(40, 20).g < 80);
    CHECK(loadImageSrc("data:image/png;base64," + base64Encode(raster::encodePng(pic))) != nullptr);
}

void testFiles(const fs::path& dir) {
    Board b;
    b.title = "Files";
    b.theme = "sketchy";
    const std::string a = b.add(shape(Kind::Rect, 0, 0, 120, 60, "A"));
    b.get(a)->text = "Start";
    b.get(a)->style.fill = "#ffc9c9";
    const std::string d = b.add(shape(Kind::Diamond, 250, 0, 120, 80, "B"));
    b.get(d)->text = "Choose";
    const std::string cy = b.add(shape(Kind::Cylinder, 250, 200, 100, 80));
    const std::string c1 = connect(b, a, d, "elbow");
    b.get(c1)->text = "go";
    connect(b, d, cy, "curved");
    Element ink;
    ink.kind = Kind::Ink;
    ink.ink = {{0, 300, 0.2}, {50, 320, 0.8}, {100, 300, 1}};
    b.add(ink);
    Element fr = shape(Kind::Frame, -40, -60, 460, 400);
    fr.text = "Area";
    const std::string fid = b.add(fr);
    b.get(a)->parent = fid;
    b.ensureLayer("notes").visible = false;
    Element t = shape(Kind::Text, 0, 420, 100, 20);
    t.text = "hidden note";
    t.layer = "notes";
    b.add(t);

    // native round trip keeps everything
    const Board back = fromJson(toJson(b));
    CHECK(back.elements.size() == b.elements.size());
    CHECK(back.get("A")->style.fill == std::string("#ffc9c9") && back.get("A")->parent == fid);
    CHECK(back.layers.size() == 2 && !back.layers[1].visible);
    CHECK(back.get(c1)->route == "elbow" && back.get(c1)->text == "go");
    CHECK(back.theme == "sketchy" && back.nextId == b.nextId);

    // Excalidraw
    const std::string ex = toExcalidraw(b);
    const auto j = nlohmann::json::parse(ex);
    CHECK(j["type"] == "excalidraw" && j["elements"].size() >= 8);
    int bound = 0;
    for (const auto& e : j["elements"]) {
        if (e["type"] == "arrow" && e["startBinding"].is_object()) ++bound;
    }
    CHECK(bound == 2);
    const Board exb = fromExcalidraw(ex);
    int shapes = 0, conns = 0, inks = 0;
    for (const auto& e : exb.elements) {
        shapes += isNode(e.kind) && e.kind != Kind::Group;
        conns += e.kind == Kind::Connector;
        inks += e.kind == Kind::Ink;
    }
    CHECK(conns == 2 && inks == 1);
    CHECK(shapes == 5);
    bool cylinder = false, label = false, attached = false;
    for (const auto& e : exb.elements) {
        cylinder = cylinder || e.kind == Kind::Cylinder;
        label = label || (e.kind == Kind::Diamond && e.text == "Choose");
        attached = attached || (e.kind == Kind::Connector && !e.from.empty() && !e.to.empty() && e.text == "go");
    }
    CHECK(cylinder && label && attached);
    CHECK(exb.theme == "sketchy");

    // draw.io
    const std::string dio = toDrawio(b);
    CHECK(dio.find("<mxfile") != std::string::npos && dio.find("rhombus") != std::string::npos && dio.find("edge=\"1\"") != std::string::npos);
    const Board db = fromDrawio(dio);
    int dShapes = 0, dConns = 0;
    for (const auto& e : db.elements) {
        dShapes += isNode(e.kind) && e.kind != Kind::Group && e.kind != Kind::Frame;
        dConns += e.kind == Kind::Connector && !e.from.empty();
    }
    CHECK(dShapes == 4 && dConns == 2);
    const Element* dd = nullptr;
    for (const auto& e : db.elements) {
        if (e.kind == Kind::Diamond) dd = &e;
    }
    CHECK(dd && dd->text == "Choose" && std::abs(dd->x - 250) < 0.01);
    // the child of the frame comes back at its absolute position
    for (const auto& e : db.elements) {
        if (e.text == "Start") CHECK(std::abs(e.x) < 0.01 && !e.parent.empty());
    }
    CHECK(db.layers.size() == 2 && !db.layers[1].visible);
    // a compressed page (deflate without the zlib header, base64)
    const std::string inner = "<mxGraphModel><root><mxCell id=\"0\"/><mxCell id=\"1\" parent=\"0\"/><mxCell id=\"2\" value=\"Hi &amp; bye\" "
                              "style=\"ellipse;fillColor=#dae8fc;\" vertex=\"1\" parent=\"1\"><mxGeometry x=\"10\" y=\"20\" width=\"80\" height=\"40\" "
                              "as=\"geometry\"/></mxCell></root></mxGraphModel>";
    int len = 0;
    unsigned char* z = stbi_zlib_compress(reinterpret_cast<unsigned char*>(const_cast<char*>(inner.data())), static_cast<int>(inner.size()), &len, 8);
    const std::string raw(reinterpret_cast<char*>(z) + 2, static_cast<std::size_t>(len - 6));
    std::free(z);
    CHECK(fromDrawio("<?xml version=\"1.0\"?>\r\n<mxfile>\r\n<diagram name=\"P\">\r\n" + base64Encode(raw) + "\r\n</diagram>\r\n</mxfile>\r\n").elements.size() == 1);
    const Board cz = fromDrawio("<mxfile><diagram name=\"P\">" + base64Encode(raw) + "</diagram></mxfile>");
    CHECK(cz.elements.size() == 1 && cz.elements[0].kind == Kind::Ellipse && cz.elements[0].text == "Hi & bye");
    CHECK(cz.elements[0].style.fill == std::string("#dae8fc"));

    // files on disk by extension
    for (const char* name : {"x.board.json", "x.excalidraw", "x.drawio", "x.svg", "x.pdf", "x.png", "x.jpg"}) {
        saveBoard(b, dir / name);
        CHECK(fs::exists(dir / name) && fs::file_size(dir / name) > 100);
    }
    CHECK(loadBoard(dir / "x.board.json").elements.size() == b.elements.size());
    CHECK(loadBoard(dir / "x.excalidraw").elements.size() >= 7);
    CHECK(loadBoard(dir / "x.drawio").elements.size() >= 7);
    {
        auto renamed = [](std::string t, const std::string& from, const std::string& to) {  // undo the rename in a saved file
            for (auto at = t.find(from); at != std::string::npos; at = t.find(from, at + to.size())) t.replace(at, from.size(), to);
            return t;
        };
        // boards and Excalidraw extras saved before the rename still load the same
        const std::string fresh = toJson(b), ex = toExcalidraw(b);
        CHECK(fresh.find("\"wetwaresuite-board\"") != std::string::npos && ex.find("\"wetwaresuite\"") != std::string::npos);
        const std::string oldBoard = renamed(fresh, "wetwaresuite-board", "achclwss-board");
        {
            std::ofstream out(dir / "old.board.json", std::ios::binary);
            out << oldBoard;
        }
        CHECK(toJson(loadBoard(dir / "old.board.json")) == toJson(fromJson(fresh)));
        CHECK(toJson(fromExcalidraw(renamed(ex, "\"wetwaresuite\"", "\"achclwss\""))) == toJson(fromExcalidraw(ex)));
    }
    bool bad = false;
    try {
        fromJson("{\"nope\": 1}");
    } catch (const std::exception&) {
        bad = true;
    }
    CHECK(bad);
}

void testCommands(const fs::path& dir) {
    Session s;
    registerCommands(s);
    Result r = run(s, "board.new title=Plan theme=default");
    CHECK(r.ok && str(r, "doc") == "board1");
    r = run(s, "board.add doc=board1 type=rounded text=\"Idea\" x=0 y=0 name=idea");
    CHECK(r.ok && str(r, "id") == "e1");
    r = run(s, "board.add doc=board1 type=diamond text=\"Worth it?\" near=idea side=right gap=80");
    CHECK(r.ok && std::stod(str(r, "x")) > 150);
    r = run(s, "board.add doc=board1 type=note text=\"Ask the team\" near=e2 side=below fill=#ffd8a8");
    CHECK(r.ok);
    r = run(s, "board.connect doc=board1 from=idea to=e2 label=think end_arrow=triangle route=elbow");
    CHECK(r.ok && str(r, "id") == "e4");
    r = run(s, "board.connect doc=board1 from=e2 to=e1,e3 route=curved");
    CHECK(r.ok && str(r, "count") == "2");
    CHECK(!run(s, "board.connect doc=board1 from=e2").ok);
    CHECK(!run(s, "board.add doc=board1 fill=notacolour").ok);
    CHECK(!run(s, "board.add doc=nope").ok);
    r = run(s, "board.set doc=board1 ids=e1 dx=10 dy=-20 fill=#d0ebff bold=true");
    CHECK(r.ok);
    auto doc = s.workspace().getAs<BoardDoc>("board1");
    CHECK(doc->board.get("e1")->x == 10 && doc->board.get("e1")->y == -20);
    CHECK(run(s, "undo").ok);
    CHECK(doc->board.get("e1")->x == 0 && !doc->board.get("e1")->style.fill);
    CHECK(run(s, "redo").ok);
    r = run(s, "board.set doc=board1 ids=e1 text=\"A much longer idea that needs room\"");
    CHECK(r.ok && doc->board.get("e1")->w > 120);
    r = run(s, "board.ink doc=board1 points=0,300,40,320,80,300,120,330 pressures=0.2,0.6,1,0.4 width=6 color=#e03131");
    CHECK(r.ok && doc->board.get(str(r, "id"))->ink.size() > 4);
    r = run(s, "board.group doc=board1 ids=e1,e2 kind=frame label=Thinking");
    CHECK(r.ok);
    const std::string frame = str(r, "id");
    CHECK(doc->board.elements.front().id == frame);  // frames go under
    r = run(s, "board.set doc=board1 ids=" + frame + " dx=100");
    CHECK(r.ok && doc->board.get("e1")->x == 110);
    r = run(s, "board.add doc=board1 type=rect text=a x=0 y=500");
    r = run(s, "board.add doc=board1 type=rect text=b x=300 y=530 w=80");
    r = run(s, "board.add doc=board1 type=rect text=c x=900 y=510");
    r = run(s, "board.find doc=board1 text=\"\" type=rect");
    const std::string rects = str(r, "ids");
    CHECK(str(r, "count") == "3");
    CHECK(run(s, "board.align doc=board1 ids=" + rects + " edge=top").ok);
    CHECK(run(s, "board.distribute doc=board1 ids=" + rects + " axis=horizontal").ok);
    {
        std::vector<double> ys, gaps;
        std::vector<const Element*> es;
        for (const auto& e : doc->board.elements) {
            if (e.kind == Kind::Rect) es.push_back(&e);
        }
        std::sort(es.begin(), es.end(), [](const Element* x, const Element* y) { return x->x < y->x; });
        CHECK(es.size() == 3 && es[0]->y == 500 && es[1]->y == 500 && es[2]->y == 500);
        CHECK(std::abs((es[1]->x - (es[0]->x + es[0]->w)) - (es[2]->x - (es[1]->x + es[1]->w))) < 0.01);
    }
    r = run(s, "board.find doc=board1 connected=e2");
    CHECK(str(r, "count") == "2");
    r = run(s, "board.info doc=board1");
    CHECK(r.ok && r.message.find("e1 -> e2 elbow") != std::string::npos);
    r = run(s, "board.info doc=board1 id=e4");
    CHECK(r.ok && str(r, "from") == "e1");
    r = run(s, "board.preview doc=board1 max_size=400");
    CHECK(r.ok && r.attachments.size() == 1 && r.attachments[0].mime == "image/png");
    // what the window uses: the picture's area and what's under the mouse
    r = run(s, "board.view doc=board1");
    CHECK(r.ok && std::stod(str(r, "scale")) > 0 && std::stod(str(r, "width")) > 0);
    {
        const Element* a = doc->board.get(rects.substr(0, rects.find(',')));
        r = run(s, "board.hit doc=board1 x=" + std::to_string(a->x + a->w / 2) + " y=" + std::to_string(a->y + a->h / 2));
        CHECK(r.ok && str(r, "id") == a->id && str(r, "type") == "rect");
        r = run(s, "board.hit doc=board1 x=-5000 y=-5000");
        CHECK(r.ok && str(r, "id").empty());
    }
    r = run(s, "board.layer doc=board1 op=add name=sketch");
    CHECK(r.ok);
    r = run(s, "board.ink doc=board1 points=0,0,100,100 layer=sketch");
    CHECK(r.ok);
    CHECK(run(s, "board.layer doc=board1 op=set name=sketch locked=true").ok);
    CHECK(!run(s, "board.set doc=board1 ids=" + str(r, "id") + " dx=5").ok);
    CHECK(run(s, "board.layer doc=board1 op=remove name=sketch delete_elements=true").ok);
    CHECK(run(s, "board.theme doc=board1 theme=sketchy preview=true").attachments.size() == 1);
    r = run(s, "board.layout doc=board1 algorithm=layered direction=LR");
    CHECK(r.ok);
    r = run(s, "board.duplicate doc=board1 ids=e3 dx=0 dy=200");
    CHECK(r.ok && !str(r, "ids").empty());
    CHECK(run(s, "board.order doc=board1 ids=e3 to=back").ok);
    CHECK(run(s, "board.remove doc=board1 ids=e2").ok);
    for (const auto& e : doc->board.elements) CHECK(e.from != "e2" && e.to != "e2");
    // exports
    for (const char* name : {"plan.board.json", "plan.png", "plan.svg", "plan.pdf", "plan.excalidraw", "plan.drawio"}) {
        r = run(s, "board.export doc=board1 path=" + q(dir / name) + " scale=1.5");
        CHECK(r.ok && fs::exists(dir / name));
    }
    r = run(s, "board.save doc=board1 path=" + q(dir / "saved.board.json"));
    CHECK(r.ok);
    r = run(s, "board.open path=" + q(dir / "saved.board.json"));
    CHECK(r.ok && str(r, "doc") == "board2");
    r = run(s, "board.import doc=board2 path=" + q(dir / "plan.excalidraw"));
    CHECK(r.ok && std::stoi(str(r, "added")) > 5);
    // text to diagram on a new board, undoable
    r = run(s, "board.from_mermaid text=\"graph TD\\nA[One] --> B[Two]\\nB --> C[Three]\" preview=true");
    CHECK(r.ok && str(r, "doc") == "board3" && str(r, "nodes") == "3" && r.attachments.size() == 1);
    CHECK(r.message.find("on board3") != std::string::npos);
    CHECK(run(s, "board.set doc=board3 ids=B fill=#b2f2bb").ok);
    CHECK(run(s, "undo").ok && run(s, "undo").ok);
    CHECK(!s.workspace().get("board3"));
    CHECK(run(s, "redo").ok && s.workspace().get("board3"));
    std::ofstream(dir / "outline.md") << "- Root\n  - A\n  - B\n";
    r = run(s, "board.from_outline doc=board3 path=" + q(dir / "outline.md") + " layout=tree");
    CHECK(r.ok && str(r, "nodes") == "3");
    CHECK(!run(s, "board.from_mermaid").ok);
    CHECK(!run(s, "board.open path=" + q(dir / "missing.board.json")).ok);
}

void testMcp() {
    Session session;
    registerCommands(session);
    mcp::Server server(session, "test", "0");
    auto call = [&](const std::string& tool, const std::string& args) {
        return server.handle(R"({"jsonrpc":"2.0","id":1,"method":"tools/call","params":{"name":")" + tool + R"(","arguments":)" + args + "}}")
            .value_or("");
    };
    const std::string list = server.handle(R"({"jsonrpc":"2.0","id":1,"method":"tools/list"})").value_or("");
    for (const char* tool : {"board_new", "board_open", "board_save", "board_export", "board_add", "board_set", "board_remove", "board_connect",
                             "board_ink", "board_group", "board_align", "board_distribute", "board_layout", "board_from_mermaid",
                             "board_from_outline", "board_find", "board_info", "board_preview", "board_layer", "board_theme"}) {
        CHECK(list.find(std::string("\"") + tool + "\"") != std::string::npos);
    }
    std::string r = call("board_from_mermaid", R"({"text":"flowchart LR\nA-->B\nB-->C","theme":"sketchy","preview":true})");
    CHECK(r.find("\"doc\":\"board1\"") != std::string::npos);
    CHECK(r.find("\"type\":\"image\"") != std::string::npos);
    r = call("board_add", R"({"doc":"board1","type":"cloud","text":"Ideas","near":"C","side":"below"})");
    CHECK(r.find("\"isError\":false") != std::string::npos);
    r = call("board_preview", R"({"doc":"board1"})");
    CHECK(r.find("\"type\":\"image\"") != std::string::npos);
    r = call("undo", "{}");
    CHECK(r.find("undone") != std::string::npos);
    r = call("board_add", R"({"doc":"nope"})");
    CHECK(r.find("\"isError\":true") != std::string::npos);
}

}  // namespace

int main() {
    const fs::path dir = tempDir();
    testModel();
    testConnectors();
    testLayout();
    testMermaid();
    testRender(dir);
    testFiles(dir);
    testCommands(dir / "cmd");
    testMcp();
    std::error_code ec;
    fs::remove_all(dir, ec);
    return check::summary("board");
}
