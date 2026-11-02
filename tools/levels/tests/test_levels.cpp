#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include "wetwaresuite/core/cli.hpp"
#include "wetwaresuite/levels/levels.hpp"
#include "wetwaresuite/mcp/server.hpp"
#include "wetwaresuite/raster/codec.hpp"
#include "check.hpp"

// Defined by stb_image_write inside libs/raster.
extern "C" unsigned char* stbi_zlib_compress(unsigned char* data, int data_len, int* out_len, int quality);

using namespace wetwaresuite;
using namespace wetwaresuite::levels;
namespace fs = std::filesystem;

namespace {

fs::path tempDir() {
    // unique per run, so parallel test runs (two build dirs) don't share files
    const auto stamp = static_cast<unsigned long long>(std::chrono::high_resolution_clock::now().time_since_epoch().count());
    fs::path d = fs::temp_directory_path() / ("wetwaresuite_levels_test_" + std::to_string(stamp));
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

std::int64_t intField(const Result& r, const std::string& key) {
    const Value* v = r.field(key);
    if (!v) return -999;
    if (auto i = std::get_if<std::int64_t>(v)) return *i;
    return -998;
}

std::string readAll(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    std::stringstream s;
    s << in.rdbuf();
    return s.str();
}

Map& mapOf(Session& s, const std::string& id) { return s.workspace().getAs<LevelDoc>(id)->map; }

void testProperties() {
    CHECK(inferProperty("true").type == "bool");
    CHECK(inferProperty("12").type == "int");
    CHECK(inferProperty("-3").type == "int");
    CHECK(inferProperty("1.5").type == "float");
    CHECK(inferProperty("#ff0000").type == "color");
    CHECK(inferProperty("gold").type == "string");
    CHECK(inferProperty("1.").type == "string");
    CHECK(inferProperty("").type == "string");
}

void testPainting() {
    Map m = newMap(10, 8, 16, 16);
    m.layers.reserve(8);  // the references below stay valid as layers are added
    Layer& l = m.addLayer("g", LayerKind::Tiles);
    CHECK(l.cells.size() == 80);
    CHECK(fillRect(m, l, {1, 1, 4, 3}, 5) == 12);
    CHECK(m.at(l, 1, 1) == 5 && m.at(l, 4, 3) == 5 && m.at(l, 5, 3) == 0);
    CHECK(fillRect(m, l, {1, 1, 4, 3}, 5) == 0);  // nothing changes the second time
    CHECK(fillRect(m, l, {-5, -5, 7, 7}, 2) == 4);  // clipped to the map
    Layer& o = m.addLayer("o", LayerKind::Tiles);
    CHECK(fillRect(m, o, {0, 0, 5, 4}, 3, true) == 14);  // 5x4 border
    CHECK(m.at(o, 2, 2) == 0 && m.at(o, 0, 2) == 3);
    // flood the empty area inside the border
    CHECK(floodFill(m, o, 2, 2, 7) == 6);
    CHECK(floodFill(m, o, 2, 2, 7) == 0);
    // outside the border is one big empty region: 80 - 20
    CHECK(floodFill(m, o, 9, 7, 1) == 60);
    Layer& d = m.addLayer("d", LayerKind::Tiles);
    CHECK(drawLine(m, d, 0, 0, 9, 7, 4) == 10);  // Bresenham: max(dx, dy) + 1 cells
    CHECK(m.at(d, 0, 0) == 4 && m.at(d, 9, 7) == 4);
    CHECK(drawLine(m, d, 3, 5, 3, 5, 6) == 1);
    // diagonal flood crosses corners, plain flood doesn't
    Map m2 = newMap(3, 3, 8, 8);
    Layer& c = m2.addLayer("c", LayerKind::Tiles);
    m2.set(c, 0, 0, 1);
    m2.set(c, 1, 1, 1);
    m2.set(c, 2, 2, 1);
    Layer copy = c;
    CHECK(floodFill(m2, c, 0, 0, 2, false) == 1);
    CHECK(floodFill(m2, copy, 0, 0, 2, true) == 3);
    // stamp and copy
    auto block = copyRegion(m, l, {0, 0, 3, 2});
    CHECK(block.size() == 6 && block[0] == 2 && block[2] == 0 && block[5] == 5);
    Layer& s = m.addLayer("s", LayerKind::Tiles);
    CHECK(stamp(m, s, 8, 7, 3, 2, block, false) == 2);  // only 2x1 of it fits
    CHECK(m.at(s, 8, 7) == 2);
    std::vector<std::uint32_t> holes = {1, 0, 0, 1};
    m.set(s, 1, 0, 9);
    stamp(m, s, 0, 0, 2, 2, holes, true);
    CHECK(m.at(s, 1, 0) == 9 && m.at(s, 0, 0) == 1);
    stamp(m, s, 0, 0, 2, 2, holes, false);
    CHECK(m.at(s, 1, 0) == 0);
}

void testAutotile() {
    Map m = newMap(5, 5, 8, 8);
    Layer& l = m.addLayer("t", LayerKind::Tiles);
    fillRect(m, l, {1, 1, 3, 3}, 1);
    auto any = [](std::uint32_t g) { return g != 0; };
    CHECK(autotile(m, l, l, any, 100, {0, 0, 5, 5}, false) == 9);
    CHECK(m.at(l, 2, 2) == 100 + 15);         // all four neighbours
    CHECK(m.at(l, 1, 1) == 100 + 2 + 4);      // E + S
    CHECK(m.at(l, 3, 3) == 100 + 1 + 8);      // N + W
    CHECK(m.at(l, 2, 1) == 100 + 2 + 4 + 8);  // E S W
    CHECK(m.at(l, 0, 0) == 0);
    // a lone cell at the edge with edges solid counts the outside as terrain
    Map e = newMap(3, 1, 8, 8);
    Layer& el = e.addLayer("t", LayerKind::Tiles);
    e.set(el, 0, 0, 1);
    autotile(e, el, el, any, 0, {0, 0, 3, 1}, true);
    CHECK(e.at(el, 0, 0) == 1 + 4 + 8);  // N, S and W are off the map
}

bool allReachable(int w, int h, const std::vector<bool>& wall) {
    std::vector<bool> seen(wall.size());
    std::size_t floors = 0, start = wall.size();
    for (std::size_t i = 0; i < wall.size(); ++i) {
        if (!wall[i]) {
            ++floors;
            if (start == wall.size()) start = i;
        }
    }
    if (!floors) return false;
    std::deque<std::size_t> qq{start};
    seen[start] = true;
    std::size_t n = 0;
    while (!qq.empty()) {
        std::size_t i = qq.front();
        qq.pop_front();
        ++n;
        const int x = static_cast<int>(i % w), y = static_cast<int>(i / w);
        const int d[4][2] = {{1, 0}, {-1, 0}, {0, 1}, {0, -1}};
        for (auto& dd : d) {
            const int nx = x + dd[0], ny = y + dd[1];
            if (nx < 0 || ny < 0 || nx >= w || ny >= h) continue;
            const std::size_t j = static_cast<std::size_t>(ny) * w + nx;
            if (wall[j] || seen[j]) continue;
            seen[j] = true;
            qq.push_back(j);
        }
    }
    return n == floors;
}

void testGenerators() {
    Rng a(5), b(5), c(6);
    CHECK(a.next() == b.next());
    CHECK(a.next() != c.next());
    for (int i = 0; i < 1000; ++i) {
        double u = a.uniform();
        int r = a.range(3, 7);
        CHECK(u >= 0 && u < 1 && r >= 3 && r <= 7);
    }
    CaveOptions co;
    co.seed = 42;
    auto cv = caves(60, 40, co);
    CHECK(cv == caves(60, 40, co));
    co.seed = 43;
    CHECK(cv != caves(60, 40, co));
    bool borders = true;
    for (int x = 0; x < 60; ++x) borders = borders && cv[x] && cv[39 * 60 + x];
    for (int y = 0; y < 40; ++y) borders = borders && cv[y * 60] && cv[y * 60 + 59];
    CHECK(borders);
    CHECK(allReachable(60, 40, cv));

    RoomOptions ro;
    ro.seed = 9;
    Rooms rooms = bspRooms(64, 48, ro);
    CHECK(rooms.rooms.size() >= 4);
    CHECK(allReachable(64, 48, rooms.wall));
    for (std::size_t i = 0; i < rooms.rooms.size(); ++i) {
        const CellRect& r = rooms.rooms[i];
        CHECK(r.w >= ro.minRoom && r.w <= ro.maxRoom && r.h >= ro.minRoom && r.h <= ro.maxRoom);
        CHECK(r.x >= 1 && r.y >= 1 && r.x + r.w <= 63 && r.y + r.h <= 47);
        for (std::size_t j = i + 1; j < rooms.rooms.size(); ++j) {
            const CellRect& o = rooms.rooms[j];
            CHECK(!(r.x < o.x + o.w && o.x < r.x + r.w && r.y < o.y + o.h && o.y < r.y + r.h));
        }
    }
    CHECK(bspRooms(64, 48, ro).rooms.size() == rooms.rooms.size());

    NoiseOptions no;
    auto n1 = noise(50, 30, no);
    double lo = 1, hi = 0;
    for (double v : n1) {
        lo = std::min(lo, v);
        hi = std::max(hi, v);
    }
    CHECK(lo == 0.0 && hi == 1.0);
    CHECK(n1 == noise(50, 30, no));

    std::vector<bool> solid(6 * 4, false);
    for (int x = 0; x < 6; ++x) solid[x] = solid[3 * 6 + x] = true;  // top and bottom rows
    for (int y = 0; y < 4; ++y) solid[y * 6] = true;                  // left column
    auto rects = mergeRects(6, 4, solid);
    std::vector<bool> covered(solid.size(), false);
    for (const auto& r : rects) {
        for (int y = r.y; y < r.y + r.h; ++y) {
            for (int x = r.x; x < r.x + r.w; ++x) {
                CHECK(!covered[static_cast<std::size_t>(y) * 6 + x]);
                covered[static_cast<std::size_t>(y) * 6 + x] = true;
            }
        }
    }
    CHECK(covered == solid);
    CHECK(rects.size() == 3);
    std::vector<bool> block(4 * 4, false);
    for (int y = 1; y < 3; ++y) {
        for (int x = 1; x < 4; ++x) block[static_cast<std::size_t>(y) * 4 + x] = true;
    }
    CHECK(mergeRects(4, 4, block).size() == 1);
}

void testCommands(const fs::path& dir) {
    Session s;
    registerCommands(s);
    CHECK(run(s, "levels.new width=20 height=12 tile_width=8").ok);
    CHECK(run(s, "levels.tileset_placeholder doc=lvl1 path=" + q(dir / "t.png") + " tiles=20 columns=5 name=terrain").ok);
    Map& m = mapOf(s, "lvl1");
    CHECK(m.tilesets.size() == 1 && m.tilesets[0].tileCount == 20 && m.tilesets[0].columns == 5 && m.tilesets[0].firstGid == 1);
    CHECK(run(s, "levels.paint doc=lvl1 x=3 y=4 tile=7").ok);
    CHECK(m.at(m.layers[0], 3, 4) == 8);
    CHECK(!run(s, "levels.paint doc=lvl1 x=3 y=4 tile=20").ok);   // past the tileset
    CHECK(!run(s, "levels.paint doc=lvl1 x=30 y=4 tile=1").ok);   // outside the map
    CHECK(run(s, "levels.paint doc=lvl1 cells=0,0,1,0,2,0 tile=2 flip_h=true").ok);
    CHECK(m.at(m.layers[0], 1, 0) == (3u | kFlipH));
    Result r = run(s, "levels.fill_rect doc=lvl1 x=10 y=2 width=5 height=4 tile=1");
    CHECK(intField(r, "changed") == 20);
    CHECK(intField(run(s, "levels.find doc=lvl1 tile=1"), "count") == 20);
    CHECK(run(s, "undo").ok);
    CHECK(intField(run(s, "levels.find doc=lvl1 tile=1"), "count") == 0);
    CHECK(run(s, "redo").ok);
    CHECK(intField(run(s, "levels.erase doc=lvl1 x=10 y=2 width=2 height=2"), "changed") == 4);
    CHECK(intField(run(s, "levels.line doc=lvl1 x1=0 y1=11 x2=19 y2=11 tile=4"), "changed") == 20);
    CHECK(intField(run(s, "levels.flood_fill doc=lvl1 x=0 y=11 tile=5"), "changed") == 20);
    CHECK(intField(run(s, "levels.count doc=lvl1"), "filled") == 16 + 1 + 3 + 20);
    // layers
    CHECK(run(s, "levels.layer_add doc=lvl1 name=deco").ok);
    CHECK(!run(s, "levels.layer_add doc=lvl1 name=deco").ok);
    CHECK(run(s, "levels.layer_add doc=lvl1 name=back index=0").ok);
    CHECK(m.layers[0].name == "back" && m.layers[2].name == "deco");
    CHECK(run(s, "levels.layer_move doc=lvl1 layer=back direction=top").ok);
    CHECK(m.layers.back().name == "back");
    CHECK(run(s, "levels.layer_move doc=lvl1 layer=back index=0").ok);
    CHECK(run(s, "levels.layer_rename doc=lvl1 layer=back name=sky").ok);
    CHECK(m.layer("sky") && !m.layer("back"));
    CHECK(run(s, "levels.layer_set doc=lvl1 layer=sky visible=false opacity=0.5").ok);
    CHECK(!m.layer("sky")->visible && m.layer("sky")->opacity == 0.5);
    CHECK(run(s, "levels.layer_remove doc=lvl1 layer=sky").ok);
    CHECK(m.layers.size() == 2);
    CHECK(run(s, "undo").ok);
    CHECK(m.layers.size() == 3);
    CHECK(run(s, "levels.layer_remove doc=lvl1 layer=0").ok);
    // stamp a tileset block onto deco: tiles (1,1)-(2,2) of a 5-column sheet = ids 6,7,11,12
    CHECK(run(s, "levels.stamp doc=lvl1 layer=deco x=0 y=5 tileset_rect=1,1,2,2").ok);
    const Layer* deco = m.layer("deco");
    CHECK(m.at(*deco, 0, 5) == 7 && m.at(*deco, 1, 5) == 8 && m.at(*deco, 0, 6) == 12 && m.at(*deco, 1, 6) == 13);
    CHECK(run(s, "levels.stamp doc=lvl1 layer=deco x=5 y=5 from_x=0 from_y=5 width=2 height=2").ok);
    CHECK(m.at(*m.layer("deco"), 6, 6) == 13);
    // scatter an exact count, only on empty cells, same result for the same seed
    CHECK(intField(run(s, "levels.scatter doc=lvl1 layer=deco tiles=14,15 weights=1,3 count=25 seed=4"), "changed") == 25);
    std::vector<std::uint32_t> first = m.layer("deco")->cells;
    CHECK(run(s, "undo").ok);
    CHECK(run(s, "levels.scatter doc=lvl1 layer=deco tiles=14,15 weights=1,3 count=25 seed=4").ok);
    CHECK(m.layer("deco")->cells == first);
    CHECK(m.at(*m.layer("deco"), 0, 5) == 7);  // the stamp survived: only empty cells
    // scatter only where ground has tile 4 (the bottom row, which flood_fill made 5)
    CHECK(intField(run(s, "levels.scatter doc=lvl1 layer=deco tiles=16 on_layer=ground on_tiles=5 density=1 only_empty=false"), "changed") == 20);
    // tile properties and collision
    CHECK(run(s, "levels.tile_property doc=lvl1 tile=5 key=solid value=true").ok);
    CHECK(m.tilesets[0].tileProps[5]["solid"].type == "bool");
    r = run(s, "levels.collision doc=lvl1 from=ground solid=property");
    CHECK(intField(r, "solid_cells") == 20 && intField(r, "rectangles") == 1);
    const Layer* col = m.layer("collision");
    CHECK(col && col->kind == LayerKind::Objects && col->objects.size() == 1 && col->objects[0].width == 160 && col->objects[0].y == 88);
    r = run(s, "levels.collision doc=lvl1 from=ground output=grid");
    CHECK(intField(r, "solid_cells") == 40);
    col = m.layer("collision");
    CHECK(col && col->kind == LayerKind::Tiles && col->collision && m.at(*col, 3, 4) == 1);
    CHECK(intField(run(s, "levels.collision doc=lvl1 from=ground solid=list tiles=7"), "solid_cells") == 1);
    // objects
    r = run(s, "levels.object_add doc=lvl1 layer=entities name=hero type=player cell=2,3 properties=\"hp=10; speed=1.5; boss=false\"");
    CHECK(r.ok && intField(r, "id") > 0);
    const int hero = static_cast<int>(intField(r, "id"));
    MapObject* ob = m.object(hero);
    CHECK(ob && ob->x == 16 && ob->y == 24 && ob->width == 8 && ob->props["hp"].type == "int" && ob->props["speed"].type == "float");
    CHECK(run(s, "levels.object_move doc=lvl1 id=" + std::to_string(hero) + " dx=4 dy=-2").ok);
    ob = m.object(hero);
    CHECK(ob->x == 20 && ob->y == 22);
    CHECK(run(s, "levels.object_set doc=lvl1 id=" + std::to_string(hero) + " key=hp value=12 name=Hero").ok);
    ob = m.object(hero);
    CHECK(ob->props["hp"].value == "12" && ob->name == "Hero");
    CHECK(run(s, "levels.object_set doc=lvl1 id=" + std::to_string(hero) + " key=boss value=").ok);
    CHECK(m.object(hero)->props.count("boss") == 0);
    r = run(s, "levels.object_add doc=lvl1 layer=entities type=coin shape=point x=50 y=50");
    CHECK(intField(run(s, "levels.object_list doc=lvl1 type=coin"), "count") == 1);
    CHECK(run(s, "levels.object_remove doc=lvl1 id=" + std::to_string(intField(r, "id"))).ok);
    CHECK(intField(run(s, "levels.object_list doc=lvl1 type=coin"), "count") == 0);
    CHECK(!run(s, "levels.object_move doc=lvl1 id=9999 x=1").ok);
    CHECK(run(s, "levels.cell doc=lvl1 x=2 y=3").message.find("Hero") != std::string::npos);
    // resize around the centre: content and objects shift by (2, 1)
    CHECK(run(s, "levels.resize doc=lvl1 width=24 height=14 anchor=center").ok);
    CHECK(m.width == 24 && m.height == 14 && m.at(*m.layer("ground"), 5, 5) == 8 && m.object(hero)->x == 36);
    CHECK(run(s, "undo").ok);
    CHECK(m.width == 20 && m.at(*m.layer("ground"), 3, 4) == 8);
    // generators through commands
    CHECK(run(s, "levels.new width=48 height=32 tile_width=16").ok);
    CHECK(run(s, "levels.tileset_placeholder doc=lvl2 path=" + q(dir / "walls.png") + " style=autotile").ok);
    r = run(s, "levels.gen_rooms doc=lvl2 wall=15 floor=-1 objects_layer=rooms seed=3");
    CHECK(r.ok && intField(r, "rooms") >= 2);
    CHECK(intField(run(s, "levels.object_list doc=lvl2 type=spawn"), "count") == 1);
    CHECK(intField(run(s, "levels.object_list doc=lvl2 type=room"), "count") == intField(r, "rooms"));
    CHECK(run(s, "levels.autotile doc=lvl2 base=0 target=walls").ok);
    CHECK(mapOf(s, "lvl2").layer("walls") != nullptr);
    CHECK(run(s, "levels.gen_caves doc=lvl2 wall=15 seed=5").ok);
    CHECK(run(s, "levels.gen_noise doc=lvl2 tiles=1,2,-1 thresholds=0.3,0.6").ok);
    CHECK(!run(s, "levels.gen_noise doc=lvl2 tiles=1,2 thresholds=0.3,0.6").ok);
    Result pv = run(s, "levels.preview doc=lvl2 grid=true");
    CHECK(pv.ok && pv.attachments.size() == 1 && pv.attachments[0].mime == "image/png");
    CHECK(run(s, "levels.info doc=lvl2 preview=true").attachments.size() == 1);
    // no-tileset maps still work: tile n is drawn as a colour
    CHECK(run(s, "levels.new width=4 height=4").ok);
    CHECK(run(s, "levels.fill_rect doc=lvl3 x=0 y=0 width=4 height=4 tile=0 preview=true").attachments.size() == 1);
    CHECK(mapOf(s, "lvl3").at(mapOf(s, "lvl3").layers[0], 0, 0) == 1);
    CHECK(run(s, "levels.close doc=lvl3").ok);
    CHECK(!s.workspace().get("lvl3"));
    CHECK(run(s, "undo").ok);
    CHECK(s.workspace().get("lvl3") != nullptr);
}

void testTilesetSlicing(const fs::path& dir) {
    // 2 columns x 3 rows of 16x16 with margin 1 and spacing 2: 1 + 16 + 2 + 16 + 1 = 36 wide
    Image sheet(36, 54, Color{0, 0, 0, 0});
    for (int r = 0; r < 3; ++r) {
        for (int c = 0; c < 2; ++c) {
            for (int y = 0; y < 16; ++y) {
                for (int x = 0; x < 16; ++x) {
                    sheet.set(1 + c * 18 + x, 1 + r * 18 + y, Color{static_cast<std::uint8_t>(40 * (r * 2 + c + 1)), 0, 0, 255});
                }
            }
        }
    }
    raster::save(sheet, dir / "sheet.png");
    Tileset t = loadTileset(dir / "sheet.png", 16, 16, 1, 2);
    CHECK(t.columns == 2 && t.tileCount == 6 && t.tileX(3) == 19 && t.tileY(3) == 19);
    Map m = newMap(2, 1, 16, 16);
    addTileset(m, t);
    Layer& l = m.addLayer("g", LayerKind::Tiles);
    m.set(l, 0, 0, 1 + 5);
    m.set(l, 1, 0, 1 + 2);
    Image img = render(m);
    CHECK(img.width() == 32 && img.height() == 16);
    CHECK(img.at(5, 5).r == 240 && img.at(20, 5).r == 120);
    bool threw = false;
    try {
        loadTileset(dir / "sheet.png", 64, 64, 0, 0);
    } catch (const std::exception&) {
        threw = true;
    }
    CHECK(threw);
}

Map sample(const fs::path& dir) {
    Map m = newMap(6, 4, 16, 16);
    raster::save(placeholderTileset(8, 4, 16, 16, "colors", {}), dir / "art" / "tiles.png");
    Tileset t = loadTileset(dir / "art" / "tiles.png", 16, 16, 0, 0, "tiles");
    t.tileProps[3]["solid"] = {"bool", "true"};
    addTileset(m, t);
    m.props["music"] = {"string", "cave & <theme>"};
    m.props["gravity"] = {"float", "9.8"};
    m.background = "#102030";
    Layer& g = m.addLayer("ground", LayerKind::Tiles);
    for (int i = 0; i < 24; ++i) g.cells[static_cast<std::size_t>(i)] = static_cast<std::uint32_t>(i % 9);  // 0 = empty
    g.cells[5] |= kFlipH;
    g.props["depth"] = {"int", "2"};
    Layer& c = m.addLayer("solid", LayerKind::Tiles);
    c.collision = true;
    c.cells[0] = 1;
    Layer& o = m.addLayer("things", LayerKind::Objects);
    MapObject a;
    a.id = m.nextObjectId++;
    a.name = "door";
    a.type = "trigger";
    a.x = 16;
    a.y = 32;
    a.width = 32;
    a.height = 16;
    a.props["target"] = {"string", "level2"};
    a.props["locked"] = {"bool", "true"};
    o.objects.push_back(a);
    MapObject b;
    b.id = m.nextObjectId++;
    b.name = "spawn";
    b.type = "spawn";
    b.shape = "point";
    b.x = 40;
    b.y = 24;
    o.objects.push_back(b);
    return m;
}

void sameMap(const Map& a, const Map& b, bool exactProps = true, bool checkFlips = true) {
    CHECK(a.width == b.width && a.height == b.height && a.tileWidth == b.tileWidth);
    CHECK(a.tilesets.size() == b.tilesets.size());
    if (!b.tilesets.empty()) {
        CHECK(b.tilesets[0].pixels != nullptr);
        CHECK(b.tilesets[0].tileCount == a.tilesets[0].tileCount);
        if (exactProps) CHECK(b.tilesets[0].tileProps == a.tilesets[0].tileProps);
    }
    CHECK(a.layers.size() == b.layers.size());
    for (std::size_t i = 0; i < a.layers.size() && i < b.layers.size(); ++i) {
        const Layer& x = a.layers[i];
        const Layer& y = b.layers[i];
        CHECK(x.name == y.name && x.kind == y.kind && x.collision == y.collision);
        if (checkFlips) CHECK(x.cells == y.cells);
        else {
            bool same = x.cells.size() == y.cells.size();
            for (std::size_t k = 0; same && k < x.cells.size(); ++k) same = (x.cells[k] & kGidMask) == (y.cells[k] & kGidMask);
            CHECK(same);
        }
        if (exactProps) CHECK(x.props == y.props);
        CHECK(x.objects.size() == y.objects.size());
        for (std::size_t k = 0; k < x.objects.size() && k < y.objects.size(); ++k) {
            const MapObject& p = x.objects[k];
            const MapObject& r = y.objects[k];
            CHECK(p.type == r.type && p.x == r.x && p.y == r.y && p.shape == r.shape && p.props == r.props);
            if (exactProps) CHECK(p.name == r.name && p.id == r.id && p.width == r.width);
        }
    }
    if (exactProps) CHECK(a.props == b.props);
}

std::string gzipOf(const std::string& data) {
    int len = 0;
    unsigned char* z = stbi_zlib_compress(reinterpret_cast<unsigned char*>(const_cast<char*>(data.data())), static_cast<int>(data.size()), &len, 8);
    std::string zs(reinterpret_cast<char*>(z), static_cast<std::size_t>(len));
    std::free(z);
    std::string out("\x1f\x8b\x08\x00\x00\x00\x00\x00\x00\x03", 10);
    out += zs.substr(2, zs.size() - 6);  // raw deflate without the zlib header and adler32
    out += std::string(8, '\0');         // crc32 + size (not checked)
    return out;
}

std::string b64(const std::string& in) {
    static const char* k = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    std::size_t i = 0;
    for (; i + 2 < in.size(); i += 3) {
        unsigned v = static_cast<unsigned char>(in[i]) << 16 | static_cast<unsigned char>(in[i + 1]) << 8 | static_cast<unsigned char>(in[i + 2]);
        for (int s = 18; s >= 0; s -= 6) out += k[v >> s & 63];
    }
    if (i < in.size()) {
        unsigned v = static_cast<unsigned char>(in[i]) << 16;
        if (i + 1 < in.size()) v |= static_cast<unsigned char>(in[i + 1]) << 8;
        out += k[v >> 18 & 63];
        out += k[v >> 12 & 63];
        out += i + 1 < in.size() ? k[v >> 6 & 63] : '=';
        out += '=';
    }
    return out;
}

void testFiles(const fs::path& dir) {
    fs::create_directories(dir / "art");
    const Map m = sample(dir);
    for (const char* enc : {"csv", "base64", "zlib"}) {
        saveMap(m, dir / "maps" / (std::string("a_") + enc + ".tmx"), "tmx", enc);
        sameMap(m, loadMap(dir / "maps" / (std::string("a_") + enc + ".tmx")));
        saveMap(m, dir / "maps" / (std::string("a_") + enc + ".tmj"), "tmj", enc);
        sameMap(m, loadMap(dir / "maps" / (std::string("a_") + enc + ".tmj")));
    }
    const std::string tmx = readAll(dir / "maps" / "a_csv.tmx");
    CHECK(tmx.find("<image source=\"../art/tiles.png\"") != std::string::npos);
    CHECK(tmx.find("cave &amp; &lt;theme&gt;") != std::string::npos);
    CHECK(tmx.find("<data encoding=\"csv\">") != std::string::npos);
    CHECK(tmx.find("<point/>") != std::string::npos);
    saveMap(m, dir / "plain.json", "json");
    sameMap(m, loadMap(dir / "plain.json"));
    CHECK(readAll(dir / "plain.json").find("\"format\": \"wetwaresuite-level\"") != std::string::npos);
    CHECK(formatFor(dir / "plain.json") == "json");
    CHECK(formatFor(dir / "maps" / "a_csv.tmj") == "tmj");
    saveMap(m, dir / "lite.ldtk", "ldtk");
    {
        // LDtk keeps tiles, flips, collision as IntGrid and entities with fields (names come back via a field)
        Map back = loadMap(dir / "lite.ldtk");
        sameMap(m, back, false, true);
        CHECK(back.layer("things")->objects[0].name == "door");
        CHECK(formatFor(dir / "lite.ldtk") == "ldtk");
        fs::copy_file(dir / "lite.ldtk", dir / "lite_as.json");
        CHECK(formatFor(dir / "lite_as.json") == "ldtk");
    }
    saveMap(m, dir / "flat.png", "png", "csv", 2.0);
    Image flat = raster::load(dir / "flat.png");
    CHECK(flat.width() == 192 && flat.height() == 128);
    // CSV
    const std::string csv = layerCsv(m, m.layers[0], true);
    CHECK(csv.substr(0, 19) == "-1,0,1,2,3,4\n5,6,7,");
    int rows = 0, cols = 0;
    CHECK(parseCsv(m, csv, true, rows, cols) == std::vector<std::uint32_t>([&] {
              auto c = m.layers[0].cells;
              for (auto& g : c) g &= kGidMask;
              return c;
          }()));
    CHECK(rows == 4 && cols == 6);
    CHECK(layerCsv(m, m.layers[0], false).substr(0, 4) == "0,1,");

    // a hand-written Tiled map: external TSX, gzip data, group layer, tile object, class attribute
    raster::save(placeholderTileset(4, 2, 8, 8, "colors", {}), dir / "tiny.png");
    {
        std::ofstream(dir / "tiny.tsx") << "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<tileset version=\"1.10\" name=\"tiny\" tilewidth=\"8\" "
                                           "tileheight=\"8\" tilecount=\"4\" columns=\"2\">\n <image source=\"tiny.png\" width=\"16\" height=\"16\"/>\n"
                                           " <tile id=\"2\" type=\"lava\"><properties><property name=\"damage\" type=\"int\" value=\"3\"/></properties></tile>\n"
                                           "</tileset>\n";
        std::string raw;
        const std::uint32_t gids[6] = {1, 2, 3, 4, 0, 1 | kFlipV};
        for (auto g : gids) {
            for (int b = 0; b < 4; ++b) raw += static_cast<char>(g >> (8 * b) & 0xFF);
        }
        std::ofstream(dir / "hand.tmx") << "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<!-- made by hand -->\n"
                                           "<map version=\"1.10\" orientation=\"orthogonal\" renderorder=\"right-down\" width=\"3\" height=\"2\" "
                                           "tilewidth=\"8\" tileheight=\"8\" infinite=\"0\" nextlayerid=\"5\" nextobjectid=\"3\">\n"
                                           " <tileset firstgid=\"1\" source=\"tiny.tsx\"/>\n"
                                           " <group id=\"3\" name=\"stuff\">\n"
                                           "  <layer id=\"1\" name=\"floor\" width=\"3\" height=\"2\">\n"
                                           "   <data encoding=\"base64\" compression=\"gzip\">\n   " +
                                               b64(gzipOf(raw)) +
                                               "\n   </data>\n  </layer>\n </group>\n"
                                               " <layer id=\"2\" name=\"xml\" width=\"3\" height=\"2\"><data><tile gid=\"2\"/><tile/><tile gid=\"3\"/></data></layer>\n"
                                               " <objectgroup id=\"4\" name=\"ents\">\n  <object id=\"1\" class=\"chest\" gid=\"2\" x=\"8\" y=\"16\" width=\"8\" height=\"8\"/>\n"
                                               "  <object id=\"2\" name=\"n&#233;\" x=\"1.5\" y=\"2\"><properties><property name=\"note\">multi\nline</property></properties></object>\n"
                                               " </objectgroup>\n</map>\n";
    }
    Map h = loadMap(dir / "hand.tmx");
    CHECK(h.width == 3 && h.height == 2 && h.tilesets.size() == 1 && h.tilesets[0].pixels && h.tilesets[0].name == "tiny");
    CHECK(h.layers.size() == 3 && h.layers[0].name == "floor" && h.layers[0].cells[3] == 4 && h.layers[0].cells[5] == (1 | kFlipV));
    CHECK(h.layers[1].cells[0] == 2 && h.layers[1].cells[1] == 0 && h.layers[1].cells[2] == 3 && h.layers[1].cells[3] == 0);
    CHECK(h.layers[2].objects.size() == 2 && h.layers[2].objects[0].type == "chest" && h.layers[2].objects[0].y == 8);
    CHECK(h.layers[2].objects[1].name == "n\xC3\xA9" && h.layers[2].objects[1].props["note"].value == "multi\nline");
    CHECK(h.tileProperties(3) && h.tileProperties(3)->at("damage").value == "3" && h.tileProperties(3)->at("type").value == "lava");
    CHECK(h.nextObjectId == 3);
    {
        // the same file with Windows line endings reads the same
        std::ifstream in(dir / "hand.tmx", std::ios::binary);
        std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>()), crlf;
        for (char c : text) {
            if (c == '\n' && (crlf.empty() || crlf.back() != '\r')) crlf += '\r';
            crlf += c;
        }
        std::ofstream(dir / "hand_crlf.tmx", std::ios::binary) << crlf;
    }
    Map hc = loadMap(dir / "hand_crlf.tmx");
    CHECK(hc.layers.size() == 3 && hc.layers[2].objects.size() == 2 && hc.layers[2].objects[1].props["note"].value == "multi\nline");
    bool threw = false;
    try {
        fromTmx("<map orientation=\"isometric\" width=\"2\" height=\"2\"/>", dir);
    } catch (const std::exception&) {
        threw = true;
    }
    CHECK(threw);
    threw = false;
    try {
        fromTmx("<map width=\"2\" height=\"2\" tilewidth=\"8\" tileheight=\"8\"><layer name=\"a\"><data encoding=\"csv\">1,2</data></map>", dir);
    } catch (const std::exception&) {
        threw = true;
    }
    CHECK(threw);

    // commands: save/open/export/import
    Session s;
    registerCommands(s);
    CHECK(run(s, "levels.open path=" + q(dir / "maps" / "a_csv.tmx")).ok);
    CHECK(run(s, "levels.save doc=lvl1 path=" + q(dir / "out" / "copy.tmj")).ok);
    CHECK(run(s, "levels.save doc=lvl1 path=" + q(dir / "out" / "copy.json")).ok);
    CHECK(formatFor(dir / "out" / "copy.json") == "json");
    CHECK(run(s, "levels.save doc=lvl1 path=" + q(dir / "out" / "copy.ldtk")).ok);
    CHECK(run(s, "levels.save doc=lvl1 path=" + q(dir / "out" / "copy.png") + " scale=0.5").ok);
    CHECK(raster::load(dir / "out" / "copy.png").width() == 48);
    CHECK(!run(s, "levels.save doc=lvl1 path=" + q(dir / "out" / "copy.csv")).ok);
    Result r = run(s, "levels.export_csv doc=lvl1 output_dir=" + q(dir / "csv"));
    CHECK(intField(r, "count") == 2 && fs::exists(dir / "csv" / "ground.csv"));
    CHECK(run(s, "levels.import_csv doc=lvl1 path=" + q(dir / "csv" / "ground.csv") + " layer=copy").ok);
    CHECK(mapOf(s, "lvl1").layer("copy")->cells[1] == m.layers[0].cells[1]);
    CHECK(run(s, "levels.open path=" + q(dir / "lite.ldtk") + " level=Level_0").ok);
    CHECK(!run(s, "levels.open path=" + q(dir / "lite.ldtk") + " level=Nope").ok);
    CHECK(!run(s, "levels.open path=" + q(dir / "missing.tmx")).ok);
    {
        std::ofstream(dir / "bad.tmx") << "<map width=\"2\" height=\"2\"><layer></map>";
    }
    CHECK(!run(s, "levels.open path=" + q(dir / "bad.tmx")).ok);
    // a map whose tileset image is missing still opens; tiles draw as colours
    fs::copy_file(dir / "maps" / "a_csv.tmx", dir / "elsewhere.tmx");
    r = run(s, "levels.open path=" + q(dir / "elsewhere.tmx") + " preview=true");
    CHECK(r.ok && r.message.find("not found") != std::string::npos && r.attachments.size() == 1);
}

void testMcp() {
    Session session;
    registerCommands(session);
    mcp::Server server(session, "test", "0");
    auto call = [&](const std::string& tool, const std::string& args) {
        return server.handle(R"({"jsonrpc":"2.0","id":1,"method":"tools/call","params":{"name":")" + tool + R"(","arguments":)" + args + "}}")
            .value_or("");
    };
    std::string list = server.handle(R"({"jsonrpc":"2.0","id":1,"method":"tools/list"})").value_or("");
    for (const char* tool : {"levels_new", "levels_paint", "levels_fill_rect", "levels_flood_fill", "levels_autotile", "levels_gen_caves",
                             "levels_gen_rooms", "levels_gen_noise", "levels_object_add", "levels_collision", "levels_save", "levels_preview",
                             "levels_stamp", "levels_scatter", "levels_resize", "levels_find"}) {
        CHECK(list.find(std::string("\"") + tool + "\"") != std::string::npos);
    }
    std::string r = call("levels_new", R"({"width":24,"height":16})");
    CHECK(r.find("\"doc\":\"lvl1\"") != std::string::npos);
    r = call("levels_gen_rooms", R"({"doc":"lvl1","wall":0,"floor":-1,"seed":2,"objects_layer":"rooms","preview":true})");
    CHECK(r.find("\"isError\":false") != std::string::npos);
    CHECK(r.find("\"type\":\"image\"") != std::string::npos);
    r = call("levels_paint", R"({"doc":"lvl1","cells":[1,1,2,2],"tile":3})");
    CHECK(r.find("\"isError\":false") != std::string::npos);
    r = call("levels_preview", R"({"doc":"lvl1","grid":true})");
    CHECK(r.find("\"type\":\"image\"") != std::string::npos);
    r = call("undo", "{}");
    CHECK(r.find("undone") != std::string::npos);
    CHECK(mapOf(session, "lvl1").at(mapOf(session, "lvl1").layers[0], 1, 1) != 4);
    r = call("levels_paint", R"({"doc":"nope","x":1,"y":1,"tile":3})");
    CHECK(r.find("\"isError\":true") != std::string::npos);
}

void testBlobAndWang() {
    // the 47 blob masks: corners only with both sides
    CHECK(blobMasks().size() == 47);
    CHECK(blobMasks().front() == 0 && blobMasks().back() == 255);
    CHECK(blobIndex(255) == 46 && blobIndex(2) == -1 && blobIndex(1 | 4 | 2) >= 0);

    Map m = newMap(6, 5, 8, 8);
    Layer& l = m.addLayer("t", LayerKind::Tiles);
    fillRect(m, l, {1, 1, 3, 3}, 1);
    const int n = autotileBlob(m, l, l, [](std::uint32_t g) { return (g & kGidMask) != 0; }, 100, {0, 0, 6, 5}, false);
    CHECK(n == 9);
    CHECK(m.at(l, 2, 2) == 100u + static_cast<std::uint32_t>(blobIndex(255)));  // the middle sees all eight
    CHECK(m.at(l, 1, 1) == 100u + static_cast<std::uint32_t>(blobIndex(4 | 8 | 16)));  // top-left: E, SE, S
    CHECK(m.at(l, 0, 0) == 0);

    // a corner Wang set from the template: interior cells get the all-terrain tile, rims mixed ones
    Tileset ts;
    ts.name = "w";
    ts.firstGid = 1;
    ts.tileCount = 16;
    ts.columns = 4;
    WangSet ws;
    ws.name = "terrain";
    ws.colors = {{"grass", "#00ff00", -1, 1}, {"dirt", "#884400", -1, 1}};
    wangTemplate(ws, "corner16", 0, 1, 2);
    CHECK(ws.type == "corner" && ws.tiles.size() == 16);
    CHECK((ws.tiles[15] == std::array<int, 8>{0, 1, 0, 1, 0, 1, 0, 1}));
    Map w = newMap(6, 6, 8, 8);
    Layer& wl = w.addLayer("t", LayerKind::Tiles);
    fillRect(w, wl, {1, 1, 4, 4}, 99);
    int unmatched = -1;
    autotileWang(w, wl, wl, [](std::uint32_t g) { return (g & kGidMask) != 0; }, ts, ws, 1, 2, {0, 0, 6, 6}, false, &unmatched);
    CHECK(unmatched == 0);
    CHECK(w.at(wl, 2, 2) == 16);         // all four corners terrain: tile 15
    CHECK(w.at(wl, 1, 1) == 1u + 2u);    // top-left of the block: only SE (bit 2)
    CHECK(w.at(wl, 4, 1) == 1u + 4u);    // top-right: only SW
    CHECK(w.at(wl, 2, 1) == 1u + 2u + 4u);  // top edge: SE and SW

    WangSet blob;
    wangTemplate(blob, "blob47", 0, 1, 0);
    CHECK(blob.type == "mixed" && blob.tiles.size() == 47);
    WangSet edge;
    wangTemplate(edge, "edge16", 0, 1, 2);
    CHECK(edge.type == "edge" && (edge.tiles[5] == std::array<int, 8>{1, 0, 2, 0, 1, 0, 2, 0}));

    // placeholders
    CHECK(placeholderTileset(47, 8, 8, 8, "blob", {}).width() == 64);
    CHECK(placeholderTileset(16, 4, 8, 8, "corner", {}).height() == 32);
}

void testTiledExtras(const fs::path& dir) {
    fs::create_directories(dir);
    raster::save(placeholderTileset(16, 4, 8, 8, "colors", {}), dir / "t.png");
    raster::save(placeholderTileset(4, 4, 8, 8, "colors", {}), dir / "sky.png");
    Map m = newMap(6, 4, 8, 8);
    Tileset t = loadTileset(dir / "t.png", 8, 8, 0, 0);
    t.animations[2] = {{2, 100}, {3, 150}};
    MapObject slope;
    slope.shape = "polygon";
    slope.points = {{0, 8}, {8, 0}, {8, 8}};
    t.tileShapes[5].push_back(slope);
    WangSet ws;
    ws.name = "grass";
    ws.colors = {{"grass", "#00ff00", 3, 0.5}};
    wangTemplate(ws, "corner16", 0, 1, 0);
    t.wangsets.push_back(ws);
    addTileset(m, t);
    Layer& sky = m.addLayer("sky", LayerKind::Image);
    sky.image = fs::absolute(dir / "sky.png").lexically_normal().string();
    sky.repeatX = true;
    sky.offsetY = 4;
    sky.parallaxX = 0.5;
    sky.tint = "#ff8080";
    attachLayerImage(sky);
    CHECK(sky.pixels && sky.pixels->width() == 32);
    Layer& g = m.addLayer("ground", LayerKind::Tiles);
    fillRect(m, g, {0, 2, 6, 2}, 3 | kFlipH);
    g.offsetX = 2;
    Layer& o = m.addLayer("ents", LayerKind::Objects);
    MapObject poly;
    poly.id = m.nextObjectId++;
    poly.shape = "polygon";
    poly.x = 10;
    poly.y = 5;
    poly.points = {{0, 0}, {10, 0}, {5, 7.5}};
    poly.rotation = 30;
    o.objects.push_back(poly);
    MapObject line = poly;
    line.id = m.nextObjectId++;
    line.shape = "polyline";
    line.rotation = 0;
    o.objects.push_back(line);
    MapObject chest;
    chest.id = m.nextObjectId++;
    chest.gid = 4;
    chest.x = 16;
    chest.y = 8;
    chest.width = chest.height = 8;
    chest.type = "chest";
    o.objects.push_back(chest);

    for (const char* ext : {".tmx", ".tmj", ".json"}) {
        const fs::path file = dir / (std::string("extras") + ext);
        saveMap(m, file, formatFor(file) == "json" || std::string(ext) == ".json" ? "json" : formatFor(file));
        Map b = loadMap(file);
        CHECK(b.layers.size() == 3);
        if (b.layers.size() != 3) continue;
        const Layer& bs = b.layers[0];
        CHECK(bs.kind == LayerKind::Image && bs.pixels && bs.repeatX && !bs.repeatY);
        CHECK(bs.offsetY == 4 && bs.parallaxX == 0.5 && bs.tint == "#ff8080");
        CHECK(b.layers[1].offsetX == 2 && b.layers[1].cells[12] == (3u | kFlipH));
        const auto& objs = b.layers[2].objects;
        CHECK(objs.size() == 3);
        if (objs.size() == 3) {
            CHECK(objs[0].shape == "polygon" && objs[0].points.size() == 3 && objs[0].points[2].y == 7.5 && objs[0].rotation == 30);
            CHECK(objs[1].shape == "polyline" && objs[1].points.size() == 3);
            CHECK(objs[2].gid == 4 && objs[2].y == 8 && objs[2].type == "chest");
        }
        const Tileset& bt = b.tilesets[0];
        CHECK(bt.animations.count(2) && bt.animations.at(2).size() == 2 && bt.animations.at(2)[1].duration == 150);
        CHECK(bt.tileShapes.count(5) && bt.tileShapes.at(5)[0].shape == "polygon" && bt.tileShapes.at(5)[0].points.size() == 3);
        CHECK(bt.wangsets.size() == 1 && bt.wangsets[0].tiles.size() == 16 && bt.wangsets[0].colors[0].probability == 0.5 &&
              bt.wangsets[0].colors[0].tile == 3 && bt.wangsets[0].tiles.at(9) == ws.tiles.at(9));
    }
    // animation frames show in renders at their time
    RenderOptions ro;
    ro.objects = false;
    Map anim = newMap(1, 1, 8, 8);
    addTileset(anim, t);
    Layer& al = anim.addLayer("a", LayerKind::Tiles);
    al.cells[0] = 3;  // local tile 2, animated 2 -> 3
    ro.time = 0;
    const Color c0 = render(anim, ro).at(4, 4);
    ro.time = 120;
    const Color c1 = render(anim, ro).at(4, 4);
    CHECK(!(c0 == c1));
    CHECK(objectWorldPoints(poly).size() == 3);

    // an infinite Tiled map becomes a finite one around its chunks; objects move with it
    std::ofstream(dir / "inf.tmx") << "<?xml version=\"1.0\"?>\n<map orientation=\"orthogonal\" width=\"30\" height=\"20\" tilewidth=\"8\" tileheight=\"8\" infinite=\"1\">\n"
                                      " <layer id=\"1\" name=\"a\" width=\"30\" height=\"20\"><data encoding=\"csv\">\n"
                                      "  <chunk x=\"-16\" y=\"0\" width=\"16\" height=\"16\">" +
                                          [] {
                                              std::string s;
                                              for (int i = 0; i < 256; ++i) s += (i ? "," : "") + std::string(i == 17 ? "5" : "0");
                                              return s;
                                          }() +
                                          "</chunk>\n  <chunk x=\"0\" y=\"16\" width=\"16\" height=\"16\">" + [] {
                                              std::string s;
                                              for (int i = 0; i < 256; ++i) s += (i ? "," : "") + std::string(i == 0 ? "7" : "0");
                                              return s;
                                          }() + "</chunk>\n </data></layer>\n"
                                                " <objectgroup id=\"2\" name=\"o\"><object id=\"1\" x=\"0\" y=\"0\"><point/></object></objectgroup>\n</map>\n";
    Map inf = loadMap(dir / "inf.tmx");
    CHECK(inf.width == 32 && inf.height == 32);
    CHECK(inf.at(inf.layers[0], 1, 1) == 5 && inf.at(inf.layers[0], 16, 16) == 7);
    CHECK(inf.layers[1].objects.size() == 1 && inf.layers[1].objects[0].x == 128);
    // TMJ infinite with base64 chunks
    {
        std::string raw;
        for (int i = 0; i < 4; ++i) {
            const std::uint32_t gid = i == 3 ? 9 : 0;
            for (int b = 0; b < 4; ++b) raw += static_cast<char>(gid >> (8 * b) & 0xFF);
        }
        std::ofstream(dir / "inf.tmj") << "{\"orientation\":\"orthogonal\",\"infinite\":true,\"tiledversion\":\"1.10\",\"tilewidth\":8,\"tileheight\":8,"
                                          "\"width\":4,\"height\":4,\"layers\":[{\"type\":\"tilelayer\",\"name\":\"a\",\"encoding\":\"base64\",\"chunks\":["
                                          "{\"x\":4,\"y\":-2,\"width\":2,\"height\":2,\"data\":\"" +
                                              b64(raw) + "\"}]}]}";
        Map ij = loadMap(dir / "inf.tmj");
        CHECK(ij.width == 2 && ij.height == 2 && ij.at(ij.layers[0], 1, 1) == 9);
    }
}

void testGodotAndWorld(const fs::path& dir) {
    fs::create_directories(dir);
    Session s;
    registerCommands(s);
    CHECK(run(s, "levels.new width=8 height=5 tile_width=16 tile_height=16").ok);
    CHECK(run(s, "levels.tileset_placeholder doc=lvl1 path=" + q(dir / "art" / "corner.png") + " style=corner").ok);
    CHECK(mapOf(s, "lvl1").tilesets[0].wangsets.size() == 1);
    CHECK(run(s, "levels.fill_rect doc=lvl1 x=1 y=1 width=5 height=3 tile=0").ok);
    Result r = run(s, "levels.autotile doc=lvl1 mode=wang wangset=terrain other=2");
    CHECK(r.ok && intField(r, "unmatched") == 0);
    CHECK(mapOf(s, "lvl1").at(mapOf(s, "lvl1").layers[0], 3, 2) == 16);
    CHECK(run(s, "undo").ok);
    CHECK(mapOf(s, "lvl1").at(mapOf(s, "lvl1").layers[0], 3, 2) == 1);
    CHECK(run(s, "redo").ok);
    CHECK(!run(s, "levels.autotile doc=lvl1 mode=wang wangset=nope").ok);
    // a second tileset for the blob rule
    CHECK(run(s, "levels.tileset_placeholder doc=lvl1 path=" + q(dir / "art" / "blob.png") + " style=blob").ok);
    CHECK(run(s, "levels.layer_add doc=lvl1 name=deco").ok);
    CHECK(run(s, "levels.fill_rect doc=lvl1 layer=deco x=0 y=0 width=3 height=2 tile=0 tileset=blob").ok);
    CHECK(run(s, "levels.autotile doc=lvl1 layer=deco mode=blob tileset=blob edges=empty").ok);
    // the terrain brush: paint and fix edges in one undoable step
    {
        Session t;
        registerCommands(t);
        CHECK(run(t, "levels.new width=8 height=6").ok);
        CHECK(run(t, "levels.tileset_placeholder doc=lvl1 path=" + q(dir / "art" / "auto.png") + " style=autotile").ok);
        CHECK(run(t, "levels.terrain doc=lvl1 cells=2,2,3,2,4,2").ok);
        const Map& tm = mapOf(t, "lvl1");
        const std::uint32_t first = tm.tilesets[0].firstGid;
        CHECK(tm.at(tm.layers[0], 3, 2) == first + 2 + 8);  // east and west neighbours
        CHECK(tm.at(tm.layers[0], 2, 2) == first + 2);       // east only
        CHECK(run(t, "levels.terrain doc=lvl1 cells=4,2 erase=true").ok);
        CHECK(mapOf(t, "lvl1").at(mapOf(t, "lvl1").layers[0], 4, 2) == 0);
        CHECK(mapOf(t, "lvl1").at(mapOf(t, "lvl1").layers[0], 3, 2) == first + 8);  // its neighbour's edge followed
        CHECK(run(t, "undo").ok);
        CHECK(mapOf(t, "lvl1").at(mapOf(t, "lvl1").layers[0], 4, 2) != 0);
        // objects report where they are, for clicking
        CHECK(run(t, "levels.layer_add doc=lvl1 name=things kind=objects").ok);
        CHECK(run(t, "levels.object_add doc=lvl1 layer=things name=spawn x=16 y=32 width=16 height=8").ok);
        Result ol = run(t, "levels.object_list doc=lvl1");
        CHECK(ol.field("boxes") && std::get<std::string>(*ol.field("boxes")) == "16,32,32,40");
        CHECK(std::get<std::string>(*ol.field("names")) == "spawn");
    }
    const std::string lvl1 = "lvl1";
    const Map& m1 = mapOf(s, lvl1);
    CHECK(m1.at(m1.layers[1], 0, 0) == m1.tilesets[1].firstGid + static_cast<std::uint32_t>(blobIndex(4 | 8 | 16)));
    // what the window's tile picker reads
    {
        Result tp = run(s, "levels.tileset_preview doc=lvl1 tileset=blob");
        CHECK(tp.ok && intField(tp, "tiles") == 47 && intField(tp, "columns") == 8 && intField(tp, "first_gid") == 17);
        CHECK(tp.attachments.size() == 1);
        const raster::Image sheet = raster::decode(tp.attachments[0].bytes);
        CHECK(sheet.width() == 8 * 16 && sheet.height() == 6 * 16);
        CHECK(sheet.at(1, 1) == m1.tilesets[1].pixels->at(1, 1));
        CHECK(!run(s, "levels.tileset_preview doc=lvl1 tileset=nope").ok);
        Result info = run(s, "levels.info doc=lvl1");
        const Value* kinds = info.field("layer_kinds");
        CHECK(kinds && toText(*kinds) == "tiles,tiles");
    }
    // random brush
    r = run(s, "levels.fill_rect doc=lvl1 layer=deco x=4 y=2 width=4 height=3 random=1,2 seed=3");
    CHECK(r.ok && intField(r, "changed") == 12);
    std::set<std::uint32_t> seen;
    for (int y = 2; y < 5; ++y)
        for (int x = 4; x < 8; ++x) seen.insert(mapOf(s, "lvl1").at(mapOf(s, "lvl1").layers[1], x, y));
    CHECK(seen == std::set<std::uint32_t>({2, 3}));
    CHECK(!run(s, "levels.fill_rect doc=lvl1 x=0 y=0 width=1 height=1").ok);  // no tile and no random
    CHECK(run(s, "levels.flood_fill doc=lvl1 layer=deco x=7 y=0 random=4,5").ok);
    CHECK(run(s, "levels.paint doc=lvl1 layer=deco cells=0,4,1,4 random=6").ok);
    CHECK(mapOf(s, "lvl1").at(mapOf(s, "lvl1").layers[1], 1, 4) == 7);
    // animations, tile shapes, wang sets by command
    CHECK(run(s, "levels.tile_animation doc=lvl1 tile=3 frames=3,4,5 durations=100,100,300").ok);
    CHECK(mapOf(s, "lvl1").tilesets[0].animations.at(3).size() == 3);
    CHECK(!run(s, "levels.tile_animation doc=lvl1 tile=3 frames=3,99").ok);
    CHECK(run(s, "levels.tile_collision doc=lvl1 tile=15 shape=polygon points=0,16,16,0,16,16").ok);
    CHECK(run(s, "levels.tile_collision doc=lvl1 tile=14 shape=rect y=8 height=8").ok);
    CHECK(run(s, "levels.tile_property doc=lvl1 tile=6 key=solid value=true").ok);
    CHECK(run(s, "levels.wangset doc=lvl1 tileset=blob name=walls colors=wall:#444444 layout=blob47 color=1 other=0").ok);
    CHECK(mapOf(s, "lvl1").tilesets[1].wangsets[0].tiles.size() == 47);
    CHECK(run(s, "levels.wang_tile doc=lvl1 tileset=blob wangset=walls tile=0 wangid=0,0,0,0,0,0,0,0").ok);
    CHECK(mapOf(s, "lvl1").tilesets[1].wangsets[0].tiles.size() == 46);
    CHECK(!run(s, "levels.wang_tile doc=lvl1 tileset=blob wangset=walls tile=0 wangid=1,2").ok);
    // shapes in collision output
    r = run(s, "levels.collision doc=lvl1 from=ground");
    CHECK(r.ok && intField(r, "shapes") == 3);  // three all-terrain cells carry the slope
    const Layer* col = mapOf(s, "lvl1").layer("collision");
    CHECK(col && col->objects.size() > 3);
    // objects: polygon, polyline, tile object
    CHECK(run(s, "levels.object_add doc=lvl1 layer=ents shape=polygon x=10 y=10 points=0,0,20,0,10,15 rotation=45 properties=\"hp=2\"").ok);
    CHECK(!run(s, "levels.object_add doc=lvl1 layer=ents shape=polygon x=10 y=10 points=0,0,20,0").ok);
    CHECK(run(s, "levels.object_add doc=lvl1 layer=ents shape=point cell=1,1 type=spawn").ok);
    r = run(s, "levels.object_add doc=lvl1 layer=ents cell=2,1 tile=3 name=chest");
    CHECK(r.ok);
    CHECK(mapOf(s, "lvl1").object(static_cast<int>(intField(r, "id")))->gid == 4);
    CHECK(run(s, "levels.object_set doc=lvl1 id=" + std::to_string(intField(r, "id")) + " rotation=90").ok);
    // image layer
    CHECK(run(s, "levels.layer_add doc=lvl1 name=bg kind=image image=" + q(dir / "art" / "corner.png") + " index=0 repeat_x=true").ok);
    CHECK(!run(s, "levels.layer_add doc=lvl1 name=bg2 kind=image").ok);
    CHECK(run(s, "levels.layer_set doc=lvl1 layer=bg parallax_x=0.25 tint=#a0a0ff offset_y=3").ok);
    CHECK(!run(s, "levels.paint doc=lvl1 layer=bg x=0 y=0 tile=1").ok);
    CHECK(run(s, "levels.preview doc=lvl1 time=150").ok);

    // Godot scene
    r = run(s, "levels.save doc=lvl1 path=" + q(dir / "game" / "level.tscn"));
    CHECK(r.ok);
    const std::string tscn = readAll(dir / "game" / "level.tscn");
    CHECK(tscn.rfind("[gd_scene load_steps=", 0) == 0);
    CHECK(tscn.find("path=\"../art/corner.png\"") != std::string::npos);
    CHECK(tscn.find("[sub_resource type=\"TileSetAtlasSource\"") != std::string::npos);
    CHECK(tscn.find("3:3/0/physics_layer_0/polygon_0/points = PackedVector2Array(-8, 8, 8, -8, 8, 8)") != std::string::npos);
    CHECK(tscn.find("2:1/0/physics_layer_0/polygon_0/points") != std::string::npos);  // solid=true tile 6
    CHECK(tscn.find("type=\"TileMap\"") != std::string::npos && tscn.find("layer_0/name = \"ground\"") != std::string::npos);
    // cell (3,2) holds local tile 15 = atlas 3:3 of source 0: 2<<16|3, 0|3<<16, 3
    CHECK(tscn.find("131075, 196608, 3") != std::string::npos);
    CHECK(tscn.find("type=\"Sprite2D\"") != std::string::npos && tscn.find("type=\"Marker2D\"") != std::string::npos);
    CHECK(tscn.find("metadata/hp = 2") != std::string::npos && tscn.find("CollisionPolygon2D") != std::string::npos);
    CHECK(tscn.find("metadata/type = \"spawn\"") != std::string::npos);
    CHECK(mapOf(s, "lvl1").layers.size() == 5);
    CHECK(!run(s, "levels.open path=" + q(dir / "game" / "level.tscn")).ok);

    // worlds
    CHECK(run(s, "levels.save doc=lvl1 path=" + q(dir / "world" / "one.tmx")).ok);
    CHECK(run(s, "levels.new width=4 height=5 tile_width=16 tile_height=16").ok);  // lvl2
    CHECK(run(s, "levels.world_new").ok);
    CHECK(run(s, "levels.world_add world=world1 level=lvl1").ok);
    CHECK(!run(s, "levels.world_add world=world1 level=lvl2").ok);  // never saved: needs file=
    r = run(s, "levels.world_add world=world1 level=lvl2 file=" + q(dir / "world" / "two.tmj") + " side=below beside=lvl1");
    CHECK(r.ok && intField(r, "x") == 0 && intField(r, "y") == 80);
    CHECK(!run(s, "levels.world_add world=world1 level=lvl1").ok);
    CHECK(run(s, "undo").ok);
    CHECK(s.workspace().getAs<WorldDoc>("world1")->entries.size() == 1);
    CHECK(run(s, "redo").ok);
    r = run(s, "levels.world_save world=world1 path=" + q(dir / "world" / "game.world"));
    CHECK(r.ok && intField(r, "levels") == 2);
    CHECK(fs::exists(dir / "world" / "two.tmj"));
    const std::string world = readAll(dir / "world" / "game.world");
    CHECK(world.find("\"fileName\": \"one.tmx\"") != std::string::npos && world.find("\"y\": 80") != std::string::npos);
    r = run(s, "levels.world_preview world=world1");
    CHECK(r.ok && intField(r, "width") == 128 && intField(r, "height") == 160 && !r.attachments.empty());
    r = run(s, "levels.world_open path=" + q(dir / "world" / "game.world"));
    CHECK(r.ok && intField(r, "count") == 2);
    CHECK(run(s, "levels.world_remove world=world1 level=two.tmj").ok);
    CHECK(s.workspace().getAs<WorldDoc>("world1")->entries.size() == 1);
}

}  // namespace

int main() {
    testProperties();
    testPainting();
    testAutotile();
    testGenerators();
    const fs::path dir = tempDir();
    testCommands(dir / "cmd");
    testTilesetSlicing(dir);
    testFiles(dir / "files");
    testMcp();
    testBlobAndWang();
    testTiledExtras(dir / "extras");
    testGodotAndWorld(dir / "godot");
    std::error_code ec;
    fs::remove_all(dir, ec);
    return check::summary("levels");
}
