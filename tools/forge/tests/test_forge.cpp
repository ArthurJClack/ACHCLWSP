#include <cmath>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "wetwaresuite/forge/forge.hpp"
#include "wetwaresuite/forge/meshops.hpp"
#include "wetwaresuite/forge/sprite.hpp"
#include "wetwaresuite/forge/texture.hpp"
#include "wetwaresuite/mcp/server.hpp"
#include "wetwaresuite/mesh/document.hpp"
#include "wetwaresuite/mesh/primitives.hpp"
#include "wetwaresuite/raster/codec.hpp"
#include "wetwaresuite/raster/document.hpp"
#include "check.hpp"

using namespace wetwaresuite;
using json = nlohmann::ordered_json;
namespace fs = std::filesystem;

namespace {

std::string docOf(const Result& r) {
    const Value* v = r.field("doc");
    return v ? toText(*v) : "";
}

std::int64_t intField(const Result& r, const char* key) {
    const Value* v = r.field(key);
    return v && std::holds_alternative<std::int64_t>(*v) ? std::get<std::int64_t>(*v) : -1;
}

bool boolField(const Result& r, const char* key) {
    const Value* v = r.field(key);
    return v && std::holds_alternative<bool>(*v) && std::get<bool>(*v);
}

NumberList listField(const Result& r, const char* key) {
    const Value* v = r.field(key);
    return v && std::holds_alternative<NumberList>(*v) ? std::get<NumberList>(*v) : NumberList{};
}

bool near(double a, double b, double eps = 1e-6) { return std::fabs(a - b) <= eps; }

bool sameImage(const raster::Image& a, const raster::Image& b) {
    if (a.width() != b.width() || a.height() != b.height()) return false;
    for (int y = 0; y < a.height(); ++y) {
        for (int x = 0; x < a.width(); ++x) {
            if (!(a.at(x, y) == b.at(x, y))) return false;
        }
    }
    return true;
}

json call(mcp::Server& server, int id, const std::string& tool, const json& args) {
    json req = {{"jsonrpc", "2.0"}, {"id", id}, {"method", "tools/call"}, {"params", {{"name", tool}, {"arguments", args}}}};
    return json::parse(*server.handle(req.dump()))["result"];
}

std::string addImage(Session& s, const raster::Image& img) {
    auto doc = std::make_shared<raster::ImageDoc>();
    doc->image.width = img.width();
    doc->image.height = img.height();
    doc->image.layers.push_back({"Background", img});
    return s.workspace().add(doc, "img");
}

void testSprites(const fs::path& dir) {
    using forge::SpriteDoc;
    using raster::Color;
    fs::create_directories(dir);
    Session s;
    forge::registerCommands(s);
    auto str = [](const fs::path& p) { return p.string(); };

    // a new indexed sprite on the pico8 palette
    Result made = s.run("forge.sprite_new", {{"width", std::int64_t{16}}, {"height", std::int64_t{16}}, {"frames", std::int64_t{2}},
                                             {"duration", std::int64_t{120}}, {"color_mode", std::string("indexed")}, {"palette", std::string("pico8")}});
    CHECK(made.ok);
    const std::string id = docOf(made);
    CHECK(id == "spr1");
    auto doc = s.workspace().getAs<SpriteDoc>(id);
    CHECK(doc && doc->sprite.indexed && doc->sprite.palette.size() == 17 && doc->sprite.frames() == 2);
    CHECK(doc->sprite.palette[0].a == 0);

    // draw tools; colours snap to the palette
    Result fr = s.run("forge.draw", {{"doc", id}, {"tool", std::string("fill_rect")}, {"points", NumberList{2, 2, 13, 13}}, {"index", std::int64_t{8}}});
    CHECK(fr.ok && intField(fr, "changed") == 144);
    CHECK(doc->sprite.flatten(0).at(5, 5) == doc->sprite.palette[8]);
    Result pen = s.run("forge.draw", {{"doc", id}, {"tool", std::string("pencil")}, {"points", NumberList{0, 0, 15, 15}}, {"color", std::string("#ffffff")}});
    CHECK(pen.ok && intField(pen, "changed") == 4);  // the line over the rectangle is already that colour
    CHECK(doc->sprite.flatten(0).at(0, 0) == (Color{0xff, 0xf1, 0xe8, 255}));
    Result el = s.run("forge.draw", {{"doc", id}, {"frame", std::int64_t{1}}, {"tool", std::string("fill_ellipse")}, {"points", NumberList{2, 2, 13, 13}},
                                     {"color", std::string("#29adff")}});
    CHECK(el.ok && intField(el, "changed") > 80 && intField(el, "changed") < 144);
    Result bucket = s.run("forge.draw", {{"doc", id}, {"frame", std::int64_t{1}}, {"tool", std::string("bucket")}, {"points", NumberList{0, 15}},
                                         {"color", std::string("#00e436")}});
    CHECK(bucket.ok && intField(bucket, "changed") == 256 - intField(el, "changed"));
    CHECK(s.history().undo());
    CHECK(doc->sprite.flatten(1).at(0, 15).a == 0);
    Result line = s.run("forge.draw", {{"doc", id}, {"frame", std::int64_t{1}}, {"tool", std::string("line")}, {"points", NumberList{0, 0, 15, 0}},
                                       {"index", std::int64_t{12}}});
    CHECK(line.ok && intField(line, "changed") == 16);
    CHECK(!s.run("forge.draw", {{"doc", id}, {"tool", std::string("line")}, {"points", NumberList{0, 0}}}).ok);
    CHECK(!s.run("forge.draw", {{"doc", id}, {"frame", std::int64_t{9}}, {"points", NumberList{0, 0}}, {"index", std::int64_t{1}}}).ok);

    // frames, layers, tags
    Result added = s.run("forge.frame_add", {{"doc", id}, {"count", std::int64_t{2}}});
    CHECK(added.ok && intField(added, "frames") == 4);
    CHECK(sameImage(doc->sprite.flatten(3), doc->sprite.flatten(1)));
    CHECK(s.run("forge.sprite_layer_add", {{"doc", id}, {"name", std::string("top")}}).ok);
    CHECK(s.run("forge.draw", {{"doc", id}, {"layer", std::string("top")}, {"tool", std::string("rect")}, {"points", NumberList{4, 4, 11, 11}},
                               {"index", std::int64_t{12}}})
              .ok);
    CHECK(doc->sprite.layers.size() == 2 && doc->sprite.layers[1].name == "top");
    CHECK(s.run("forge.tag_add", {{"doc", id}, {"name", std::string("idle")}, {"from", std::int64_t{0}}, {"to", std::int64_t{1}}}).ok);
    CHECK(s.run("forge.tag_add", {{"doc", id}, {"name", std::string("run")}, {"from", std::int64_t{2}}, {"to", std::int64_t{3}},
                                  {"direction", std::string("pingpong")}})
              .ok);
    CHECK(!s.run("forge.tag_add", {{"doc", id}, {"name", std::string("bad")}, {"from", std::int64_t{3}}, {"to", std::int64_t{9}}}).ok);
    CHECK(doc->sprite.tags.size() == 2);
    CHECK((doc->sprite.tagFrames(doc->sprite.tags[1]) == std::vector<int>{2, 3}));
    CHECK(s.run("forge.frame_duration", {{"doc", id}, {"tag", std::string("run")}, {"ms", std::int64_t{60}}}).ok);
    CHECK((doc->sprite.durations == std::vector<int>{120, 120, 60, 60}));

    // .aseprite round trip
    const fs::path ase = dir / "hero.aseprite";
    CHECK(s.run("forge.sprite_save", {{"doc", id}, {"path", str(ase)}}).ok);
    Result reopened = s.run("forge.sprite_open", {{"path", str(ase)}});
    CHECK(reopened.ok);
    auto back = s.workspace().getAs<SpriteDoc>(docOf(reopened));
    CHECK(back && back->sprite.width == 16 && back->sprite.height == 16 && back->sprite.frames() == 4);
    CHECK(back->sprite.indexed && back->sprite.palette.size() == 17);
    CHECK(back->sprite.durations == doc->sprite.durations);
    CHECK(back->sprite.layers.size() == 2 && back->sprite.layers[1].name == "top");
    CHECK(back->sprite.tags.size() == 2 && back->sprite.tags[1].name == "run" && back->sprite.tags[1].direction == "pingpong");
    bool same = true;
    for (int f = 0; f < 4; ++f) same = same && sameImage(back->sprite.flatten(f), doc->sprite.flatten(f));
    CHECK(same);
    // RGBA round trip with layer opacity and blend
    {
        forge::Sprite rgba = forge::newSprite(5, 3, 2, 80);
        rgba.cel(0, 0).set(1, 1, Color{10, 20, 30, 200});
        rgba.cel(0, 1).set(4, 2, Color{250, 0, 0, 255});
        rgba.layers.push_back({"fx", false, 128, 1, false, 0});
        rgba.cels.push_back(std::vector<raster::Image>(2));
        rgba.cel(1, 1).set(0, 0, Color{1, 2, 3, 4});
        forge::Sprite r2 = forge::readAseprite(forge::writeAseprite(rgba));
        CHECK(r2.width == 5 && r2.height == 3 && r2.frames() == 2 && r2.durations[1] == 80);
        CHECK(r2.layers.size() == 2 && !r2.layers[1].visible && r2.layers[1].opacity == 128 && r2.layers[1].blend == 1);
        CHECK(r2.celIf(0, 0) && r2.celIf(0, 0)->at(1, 1) == (Color{10, 20, 30, 200}));
        CHECK(r2.celIf(0, 1) && r2.celIf(0, 1)->at(4, 2) == (Color{250, 0, 0, 255}));
        CHECK(r2.celIf(1, 1) && r2.celIf(1, 1)->at(0, 0) == (Color{1, 2, 3, 4}));
        CHECK(r2.celIf(1, 0) == nullptr || r2.celIf(1, 0)->width() == 0);
        bool threw = false;
        try {
            forge::readAseprite("not an aseprite file");
        } catch (const std::exception&) {
            threw = true;
        }
        CHECK(threw);
    }

    // sheet + JSON + Godot SpriteFrames + frames
    const fs::path sheet = dir / "hero.png";
    Result ex = s.run("forge.sprite_export", {{"doc", id}, {"output", str(sheet)}, {"layout", std::string("grid")}, {"padding", std::int64_t{1}},
                                              {"godot", str(dir / "hero.tres")}, {"frames_dir", str(dir / "frames")}});
    CHECK(ex.ok && intField(ex, "width") == 35 && intField(ex, "height") == 35);
    CHECK(fs::exists(dir / "hero.json") && fs::exists(dir / "hero.tres") && fs::exists(dir / "frames" / "hero_003.png"));
    {
        std::ifstream in(dir / "hero.json");
        json j = json::parse(in);
        CHECK(j["frames"].is_object() && j["frames"].size() == 4);
        CHECK(j["frames"]["hero 2.aseprite"]["duration"] == 60);
        CHECK(j["frames"]["hero 1.aseprite"]["frame"]["x"] == 18);
        CHECK(j["meta"]["frameTags"].size() == 2 && j["meta"]["frameTags"][1]["name"] == "run" && j["meta"]["frameTags"][1]["direction"] == "pingpong");
        CHECK(j["meta"]["size"]["w"] == 35 && j["meta"]["image"] == "hero.png");
        CHECK(j["meta"]["layers"].size() == 2);
        raster::Image img = raster::load(sheet);
        CHECK(img.width() == 35 && img.at(1 + 5, 1 + 5) == doc->sprite.flatten(0).at(5, 5));
        std::ifstream t(dir / "hero.tres");
        std::stringstream ts;
        ts << t.rdbuf();
        const std::string tres = ts.str();
        CHECK(tres.find("[gd_resource type=\"SpriteFrames\"") == 0);
        CHECK(tres.find("&\"idle\"") != std::string::npos && tres.find("&\"run\"") != std::string::npos);
        CHECK(tres.find("region = Rect2(18, 18, 16, 16)") != std::string::npos);
    }
    Result packed = s.run("forge.sprite_export", {{"doc", id}, {"output", str(dir / "packed.png")}, {"layout", std::string("packed")},
                                                  {"trim", true}, {"format", std::string("array")}});
    CHECK(packed.ok);
    {
        std::ifstream in(dir / "packed.json");
        json j = json::parse(in);
        CHECK(j["frames"].is_array() && j["frames"].size() == 4 && j["frames"][0]["trimmed"].is_boolean());
        CHECK(j["frames"][0]["sourceSize"]["w"] == 16);
    }

    // GIF
    const fs::path gif = dir / "hero.gif";
    CHECK(s.run("forge.sprite_save", {{"doc", id}, {"path", str(gif)}, {"scale", std::int64_t{2}}}).ok);
    {
        raster::Image first = raster::load(gif);
        CHECK(first.width() == 32 && first.height() == 32);
        CHECK(first.at(10, 10) == doc->sprite.flatten(0).at(5, 5));
        const std::string bytes = forge::encodeSpriteGif(doc->sprite, {2, 3}, 1);
        CHECK(bytes.rfind("GIF89a", 0) == 0 && bytes.back() == 0x3b && bytes.find("NETSCAPE2.0") != std::string::npos);
    }
    CHECK(s.run("forge.sprite_save", {{"doc", id}, {"path", str(dir / "run.gif")}, {"tag", std::string("run")}}).ok);
    CHECK(!s.run("forge.sprite_save", {{"doc", id}, {"path", str(dir / "x.gif")}, {"tag", std::string("nope")}}).ok);

    // preview with onion skin; info
    Result pv = s.run("forge.sprite_preview", {{"doc", id}, {"frame", std::int64_t{1}}, {"onion_before", std::int64_t{1}}, {"onion_after", std::int64_t{1}}});
    CHECK(pv.ok && !pv.attachments.empty());
    {
        raster::Image onion = forge::onionSkin(doc->sprite, 1, 1, 0);
        CHECK(onion.width() == 16 && onion.at(3, 3).a > 0);
    }
    Result inf = s.run("forge.sprite_info", {{"doc", id}});
    CHECK(inf.ok && intField(inf, "frames") == 4 && intField(inf, "layers") == 2);
    {
        const Value* names = inf.field("layer_names");
        const Value* ranges = inf.field("tag_ranges");
        const Value* shown = inf.field("layer_visible");
        CHECK(names && toText(*names) == "Layer 1,top");
        CHECK(ranges && toText(*ranges) == "0-1,2-3");
        CHECK(shown && toText(*shown) == "1,1");
    }

    // palette swap recolours an indexed sprite
    const Color before = doc->sprite.flatten(0).at(5, 5);
    Result swap = s.run("forge.sprite_palette", {{"doc", id}, {"action", std::string("preset")}, {"name", std::string("db16")}, {"remap", true}});
    CHECK(swap.ok && doc->sprite.palette.size() == 17);
    CHECK(!(doc->sprite.flatten(0).at(5, 5) == before));
    bool onPalette = true;
    for (int f = 0; f < doc->sprite.frames(); ++f) {
        raster::Image im = doc->sprite.flatten(f);
        for (int y = 0; y < im.height(); ++y)
            for (int x = 0; x < im.width(); ++x) {
                Color c = im.at(x, y);
                if (c.a == 0) continue;
                bool found = false;
                for (const auto& p : doc->sprite.palette) found = found || p == c;
                onPalette = onPalette && found;
            }
    }
    CHECK(onPalette);
    CHECK(s.history().undo());
    CHECK(doc->sprite.flatten(0).at(5, 5) == before);
    CHECK(s.run("forge.sprite_palette", {{"doc", id}, {"action", std::string("set_color")}, {"index", std::int64_t{8}}, {"color", std::string("#123456")}}).ok);
    CHECK(doc->sprite.flatten(0).at(5, 5) == (Color{0x12, 0x34, 0x56, 255}));
    CHECK(s.run("forge.sprite_palette", {{"doc", id}, {"action", std::string("save")}, {"path", str(dir / "pal.gpl")}}).ok);
    CHECK(forge::loadPalette(str(dir / "pal.gpl")).size() == 16);
    CHECK(forge::presetPalette("gameboy").size() == 4 && forge::presetPalette("nope").empty());

    // transform, resize, cel ops, frame remove/move all undo
    const raster::Image f0 = doc->sprite.flatten(0);
    CHECK(s.run("forge.sprite_transform", {{"doc", id}, {"flip_h", true}}).ok);
    CHECK(doc->sprite.flatten(0).at(15, 0) == f0.at(0, 0));
    CHECK(s.history().undo());
    CHECK(s.run("forge.sprite_transform", {{"doc", id}, {"rotate", std::int64_t{90}}}).ok);
    CHECK(doc->sprite.flatten(0).at(15, 0) == f0.at(0, 0));
    CHECK(s.history().undo());
    CHECK(s.run("forge.sprite_resize", {{"doc", id}, {"scale", std::int64_t{2}}}).ok);
    CHECK(doc->sprite.width == 32 && doc->sprite.flatten(0).at(11, 11) == f0.at(5, 5));
    CHECK(s.history().undo() && doc->sprite.width == 16);
    CHECK(s.run("forge.sprite_resize", {{"doc", id}, {"width", std::int64_t{20}}, {"anchor", std::string("top_left")}}).ok);
    CHECK(doc->sprite.width == 20 && doc->sprite.height == 16 && doc->sprite.flatten(0).at(5, 5) == f0.at(5, 5));
    CHECK(s.history().undo());
    CHECK(s.run("forge.cel_clear", {{"doc", id}, {"layer", std::string("top")}, {"frame", std::int64_t{0}}}).ok);
    CHECK(doc->sprite.flatten(0).at(4, 4) == doc->sprite.flatten(0).at(5, 5));  // the outline is gone
    CHECK(s.history().undo());
    CHECK(s.run("forge.frame_remove", {{"doc", id}, {"frame", std::int64_t{1}}}).ok);
    CHECK(doc->sprite.frames() == 3 && doc->sprite.tags[0].to == 0 && doc->sprite.tags[1].from == 1 && doc->sprite.tags[1].to == 2);
    CHECK(s.history().undo() && doc->sprite.frames() == 4 && doc->sprite.tags[0].to == 1);
    CHECK(s.run("forge.frame_move", {{"doc", id}, {"frame", std::int64_t{0}}, {"to", std::int64_t{3}}}).ok);
    CHECK(sameImage(doc->sprite.flatten(3), f0));
    CHECK(s.history().undo() && sameImage(doc->sprite.flatten(0), f0));
    CHECK(s.run("forge.sprite_layer_set", {{"doc", id}, {"layer", std::string("top")}, {"visible", false}}).ok);
    CHECK(!(doc->sprite.flatten(0).at(4, 4) == f0.at(4, 4)));
    CHECK(s.history().undo());

    // colour modes
    CHECK(s.run("forge.sprite_color_mode", {{"doc", id}, {"mode", std::string("rgba")}}).ok && !doc->sprite.indexed);
    CHECK(s.run("forge.draw", {{"doc", id}, {"points", NumberList{0, 0}}, {"color", std::string("#010203")}}).ok);
    CHECK(doc->sprite.flatten(0).at(0, 0) == (Color{1, 2, 3, 255}));
    CHECK(s.run("forge.sprite_color_mode", {{"doc", id}, {"mode", std::string("indexed")}, {"count", std::int64_t{8}}}).ok);
    CHECK(doc->sprite.indexed && doc->sprite.palette.size() <= 18);

    // opening a sheet cuts it into frames
    Result cut = s.run("forge.sprite_open", {{"path", str(sheet)}, {"columns", std::int64_t{2}}, {"rows", std::int64_t{2}}});
    CHECK(cut.ok);
    auto cutDoc = s.workspace().getAs<SpriteDoc>(docOf(cut));
    CHECK(cutDoc && cutDoc->sprite.frames() == 4);
    CHECK(s.run("forge.sprite_close", {{"doc", docOf(cut)}}).ok && !s.workspace().get(docOf(cut)));
    CHECK(!s.run("forge.sprite_open", {{"path", str(dir / "missing.aseprite")}}).ok);
}

}  // namespace

// a flat low-poly square and a high-poly one with a dome in the middle
void testBake(Session& s, const fs::path& dir) {
    mesh::Mesh low = mesh::plane(2, 2, 1, 1);
    forge::generateUvs(low, forge::Projection::Planar, 2, 1.0);
    mesh::Mesh high = mesh::plane(2, 2, 96, 96);
    for (auto& v : high.positions) {
        const double r = std::hypot(v.x, v.y);
        if (r < 0.5) v.z = std::sqrt(0.25 - r * r) * 0.4;
    }
    high.normals.clear();
    forge::recomputeNormals(high, 89);
    forge::BakeOptions o;
    o.size = 64;
    o.aoSamples = 32;
    o.distance = 0.3;
    o.aoDistance = 0.5;
    const forge::BakeResult b = forge::bakeMaps(low, high, o);
    CHECK(b.texels > 3500 && b.missed < b.texels / 50);
    auto px = [](const raster::Image& img, int x, int y) { return img.pixel(x, y); };
    // flat corner: straight up; the dome's right flank leans right (red up), its top flank up (green up)
    CHECK(std::abs(px(b.normalMap, 4, 4)[0] - 128) < 6 && px(b.normalMap, 4, 4)[2] > 245);
    CHECK(px(b.normalMap, 44, 32)[0] > 160);
    CHECK(px(b.normalMap, 32, 20)[1] > 160);
    // the floor next to the dome sees less sky than the open corner
    CHECK(px(b.aoMap, 4, 4)[0] > 230);
    CHECK(px(b.aoMap, 32 + 17, 32)[0] < px(b.aoMap, 4, 4)[0] - 20);
    CHECK(px(b.aoMap, 32, 32)[0] > 230);  // the dome's top is open
    // no UVs: a clear message
    mesh::Mesh bare = mesh::plane(2, 2, 1, 1);
    bare.uvs.clear();
    bool threw = false;
    try {
        forge::bakeMaps(bare, high, o);
    } catch (const std::exception& e) {
        threw = std::string(e.what()).find("UVs") != std::string::npos;
    }
    CHECK(threw);
    // and as a command, from files
    mesh::save(dir / "low.obj", low);
    mesh::save(dir / "high.obj", high);
    Result r = s.run("forge.mesh_bake", {{"low", (dir / "low.obj").string()}, {"high", (dir / "high.obj").string()},
                                         {"normal", (dir / "bake" / "n.png").string()}, {"ao", (dir / "bake" / "ao.png").string()},
                                         {"size", std::int64_t{64}}, {"samples", std::int64_t{8}}, {"distance", 0.3}});
    CHECK(r.ok && r.attachments.size() == 2 && fs::exists(dir / "bake" / "n.png") && fs::exists(dir / "bake" / "ao.png"));
    CHECK(!s.run("forge.mesh_bake", {{"low", (dir / "low.obj").string()}, {"high", (dir / "high.obj").string()}}).ok);
}

int main() {
    auto dir = fs::temp_directory_path() / "wetwaresuite_test_forge";
    fs::remove_all(dir);
    fs::create_directories(dir);

    Session s;
    forge::registerCommands(s);
    testBake(s, dir);
    for (const auto& info : s.registry().list()) CHECK(info.id.rfind("forge.", 0) == 0);

    // ---- meshes ----
    mesh::save(dir / "ball.obj", mesh::sphere(1.0, 64, 32));
    Result opened = s.run("forge.mesh_open", {{"path", (dir / "ball.obj").string()}});
    CHECK(opened.ok);
    const std::string ball = docOf(opened);
    CHECK(ball == "mesh1");
    const auto ballTris = intField(opened, "triangles");
    CHECK(ballTris > 3000);
    CHECK(boolField(opened, "watertight"));
    CHECK(!s.run("forge.mesh_open", {{"path", (dir / "nope.obj").string()}}).ok);

    Result info = s.run("forge.mesh_info", {{"doc", ball}});
    CHECK(info.ok && intField(info, "triangles") == ballTris && listField(info, "size").size() == 3);

    // decimate to about a target
    Result dec = s.run("forge.mesh_decimate", {{"doc", ball}, {"triangles", std::int64_t{800}}});
    CHECK(dec.ok);
    CHECK(intField(dec, "triangles") <= 800 && intField(dec, "triangles") >= 400);
    CHECK(boolField(s.run("forge.mesh_info", {{"doc", ball}}), "normals"));
    CHECK(s.history().undo());
    CHECK(intField(s.run("forge.mesh_info", {{"doc", ball}}), "triangles") == ballTris);
    Result half = s.run("forge.mesh_decimate", {{"doc", ball}, {"ratio", 0.5}});
    CHECK(half.ok && intField(half, "triangles") <= ballTris / 2 && intField(half, "triangles") >= ballTris / 5);
    CHECK(!s.run("forge.mesh_decimate", {{"doc", ball}}).ok);
    CHECK(!s.run("forge.mesh_decimate", {{"doc", ball}, {"ratio", 1.5}}).ok);

    // uvs
    CHECK(!boolField(s.run("forge.mesh_info", {{"doc", ball}}), "uvs"));
    for (const char* proj : {"box", "planar", "cylindrical", "spherical"}) {
        Result uv = s.run("forge.mesh_uv", {{"doc", ball}, {"projection", std::string(proj)}});
        CHECK(uv.ok);
        Result i = s.run("forge.mesh_info", {{"doc", ball}});
        CHECK(boolField(i, "uvs"));
        CHECK(boolField(i, "watertight"));  // splitting for UVs keeps positions shared
    }
    {
        auto doc = s.workspace().getAs<mesh::MeshDoc>(ball);
        bool inRange = true;
        for (const auto& uv : doc->mesh.uvs) inRange = inRange && uv.x >= -1e-9 && uv.x <= 1.5 + 1e-9 && uv.y >= -1e-9 && uv.y <= 1 + 1e-9;
        CHECK(inRange);
        mesh::Mesh cube = mesh::box({2, 2, 2});
        forge::generateUvs(cube, forge::Projection::Box);
        CHECK(cube.hasUvs() && cube.vertexCount() > 8);  // corners split where sides need different UVs
    }

    // normals: recompute keeps UVs, crease splits a cube
    CHECK(s.run("forge.mesh_normals", {{"doc", ball}, {"crease", 60.0}}).ok);
    {
        Result i = s.run("forge.mesh_info", {{"doc", ball}});
        CHECK(boolField(i, "normals") && boolField(i, "uvs"));
        mesh::Mesh cube = mesh::box({1, 1, 1});
        forge::recomputeNormals(cube, 30);
        CHECK(cube.vertexCount() == 24);
        forge::recomputeNormals(cube, 180);
        CHECK(cube.hasNormals());
    }
    CHECK(s.run("forge.mesh_normals", {{"doc", ball}, {"mode", std::string("flip")}}).ok);
    CHECK(mesh::volume(s.workspace().getAs<mesh::MeshDoc>(ball)->mesh) < 0);
    CHECK(s.history().undo());

    // Y-up conversion and transforms
    mesh::Mesh tower = mesh::box({1, 4, 1});  // tall along Y, like a Y-up export
    mesh::transform(tower, mesh::Mat4::translate({0, 2, 0}));
    mesh::save(dir / "tower.stl", tower);
    const std::string tw = docOf(s.run("forge.mesh_open", {{"path", (dir / "tower.stl").string()}}));
    CHECK(tw == "mesh2");
    Result up = s.run("forge.mesh_transform", {{"doc", tw}, {"axes", std::string("y_up_to_z_up")}});
    CHECK(up.ok);
    NumberList size = listField(up, "size");
    CHECK(size.size() == 3 && near(size[0], 1) && near(size[1], 1) && near(size[2], 4));
    NumberList mn = listField(up, "min");
    CHECK(mn.size() == 3 && near(mn[2], 0, 1e-9));
    Result back = s.run("forge.mesh_transform", {{"doc", tw}, {"axes", std::string("z_up_to_y_up")}});
    CHECK(near(listField(back, "size")[1], 4));
    CHECK(s.history().undo());
    Result moved = s.run("forge.mesh_transform", {{"doc", tw}, {"scale", NumberList{2}}, {"center", true}, {"floor", true}, {"move", NumberList{1, 0, 0}}});
    CHECK(moved.ok && near(listField(moved, "min")[2], 0, 1e-9) && near(listField(moved, "size")[2], 8) && near(listField(moved, "min")[0], 0));
    CHECK(!s.run("forge.mesh_transform", {{"doc", tw}, {"move", NumberList{1, 2}}}).ok);

    // weld triangle soup, merge
    {
        mesh::save(dir / "soup.obj", mesh::unweld(mesh::box({1, 1, 1})));
        const std::string soup = docOf(s.run("forge.mesh_open", {{"path", (dir / "soup.obj").string()}}));
        Result w = s.run("forge.mesh_weld", {{"doc", soup}});
        CHECK(w.ok && intField(w, "vertices") == 8 && intField(w, "removed") > 0);
        const auto before = intField(s.run("forge.mesh_info", {{"doc", tw}}), "triangles");
        Result m = s.run("forge.mesh_merge", {{"doc", tw}, {"other", soup}, {"move", NumberList{5, 0, 0}}});
        CHECK(m.ok && intField(m, "triangles") == before + 12);
        CHECK(s.run("forge.mesh_merge", {{"doc", tw}, {"other", (dir / "ball.obj").string()}}).ok);
        CHECK(!s.run("forge.mesh_merge", {{"doc", tw}, {"other", std::string("mesh99")}}).ok);
    }

    // save and reopen, render
    CHECK(s.run("forge.mesh_save", {{"doc", tw}, {"path", (dir / "out" / "tower.glb").string()}}).ok);
    CHECK(fs::exists(dir / "out" / "tower.glb"));
    CHECK(s.run("forge.mesh_open", {{"path", (dir / "out" / "tower.glb").string()}}).ok);
    CHECK(s.run("forge.mesh_save", {{"doc", tw}, {"path", (dir / "out" / "tower.png").string()}, {"width", std::int64_t{128}}}).ok);
    CHECK(raster::load(dir / "out" / "tower.png").width() == 128);
    CHECK(s.run("forge.mesh_save", {{"doc", tw}, {"path", (dir / "out" / "tower.fbx").string()}}).ok);
    CHECK(mesh::load(dir / "out" / "tower.fbx").triangleCount() > 0);
    CHECK(!s.run("forge.mesh_save", {{"doc", tw}, {"path", (dir / "out" / "tower.dae").string()}}).ok);

    // LOD chain
    Result lods = s.run("forge.mesh_lods", {{"input", (dir / "ball.obj").string()}, {"output", (dir / "lods" / "ball.glb").string()}});
    CHECK(lods.ok && intField(lods, "levels") == 4);
    NumberList counts = listField(lods, "triangles");
    CHECK(counts.size() == 4);
    for (int i = 0; i < 4; ++i) CHECK(fs::exists(dir / "lods" / ("ball_lod" + std::to_string(i) + ".glb")));
    CHECK(counts.size() == 4 && counts[0] == static_cast<double>(ballTris) && counts[1] < counts[0] && counts[2] < counts[1] && counts[3] < counts[2]);
    CHECK(counts.size() == 4 && counts[3] <= ballTris * 0.125 + 1);
    CHECK(mesh::load(dir / "lods" / "ball_lod2.glb").hasNormals());
    CHECK(!mesh::load(dir / "lods" / "ball_lod2.glb").hasUvs());
    CHECK(s.run("forge.mesh_lods", {{"input", (dir / "ball.obj").string()}, {"output", (dir / "lods" / "uv.glb").string()}, {"uv", std::string("spherical")}}).ok);
    CHECK(mesh::load(dir / "lods" / "uv_lod0.glb").hasUvs() && mesh::load(dir / "lods" / "uv_lod3.glb").hasUvs());
    CHECK(s.run("forge.mesh_lods", {{"doc", ball}, {"output", (dir / "lods" / "b.obj").string()}, {"ratios", NumberList{1, 0.3}}}).ok);
    CHECK(fs::exists(dir / "lods" / "b_lod1.obj") && !fs::exists(dir / "lods" / "b_lod2.obj"));
    CHECK(!s.run("forge.mesh_lods", {{"doc", ball}, {"output", (dir / "lods" / "b.obj").string()}, {"ratios", NumberList{0}}}).ok);

    // batch
    {
        fs::create_directories(dir / "models");
        mesh::save(dir / "models" / "a.obj", mesh::sphere(1, 32, 16));
        mesh::save(dir / "models" / "b.stl", mesh::cylinder(1, 2, 48));
        Result b = s.run("forge.mesh_batch", {{"input_dir", (dir / "models").string()},
                                              {"output_dir", (dir / "models" / "game").string()},
                                              {"axes", std::string("y_up_to_z_up")},
                                              {"ratio", 0.5},
                                              {"floor", true}});
        CHECK(b.ok && intField(b, "processed") == 2);
        CHECK(fs::exists(dir / "models" / "game" / "a.glb") && fs::exists(dir / "models" / "game" / "b.glb"));
        CHECK(intField(b, "triangles_out") < intField(b, "triangles_in"));
        Result again = s.run("forge.mesh_batch", {{"input_dir", (dir / "models").string()}, {"output_dir", (dir / "models" / "game").string()}});
        CHECK(again.ok && intField(again, "skipped") == 2);
    }

    // ---- textures ----
    // normal map of a flat image
    raster::save(raster::Image(16, 16, {90, 90, 90, 255}), dir / "flat.png");
    Result nm = s.run("forge.normal_map", {{"input", (dir / "flat.png").string()}, {"output", (dir / "flat_n.png").string()}});
    CHECK(nm.ok);
    {
        raster::Image n = raster::load(dir / "flat_n.png");
        bool flat = true;
        for (int y = 0; y < n.height(); ++y) {
            for (int x = 0; x < n.width(); ++x) flat = flat && n.at(x, y) == raster::Color{128, 128, 255, 255};
        }
        CHECK(flat);
        // a ramp that brightens downwards: OpenGL green rises, DirectX green falls
        raster::Image ramp(8, 8);
        for (int y = 0; y < 8; ++y) {
            for (int x = 0; x < 8; ++x) ramp.set(x, y, {static_cast<std::uint8_t>(y * 30), static_cast<std::uint8_t>(y * 30), static_cast<std::uint8_t>(y * 30), 255});
        }
        forge::NormalMapOptions o;
        o.wrap = false;
        CHECK(forge::normalMap(ramp, o).at(4, 4).g > 140);
        o.invertY = true;
        CHECK(forge::normalMap(ramp, o).at(4, 4).g < 116);
        CHECK(forge::normalMap(ramp, o).at(4, 4).r == 128);
    }
    CHECK(!s.run("forge.normal_map", {}).ok);
    CHECK(!s.run("forge.normal_map", {{"doc", std::string("img42")}}).ok);

    // seamless + tile preview
    {
        raster::Image grad(64, 64);
        for (int y = 0; y < 64; ++y) {
            for (int x = 0; x < 64; ++x) grad.set(x, y, {static_cast<std::uint8_t>(x * 4), static_cast<std::uint8_t>(y * 4), 100, 255});
        }
        const std::string g = addImage(s, grad);
        Result sm = s.run("forge.seamless", {{"doc", g}});
        CHECK(sm.ok);
        const Value* before = sm.field("seam_before");
        const Value* after = sm.field("seam_after");
        CHECK(before && after && std::get<double>(*after) < std::get<double>(*before) * 0.2);
        CHECK(s.history().undo());
        CHECK(sameImage(s.workspace().getAs<raster::ImageDoc>(g)->flatten(), grad));
        Result tp = s.run("forge.tile_preview", {{"doc", g}});
        CHECK(tp.ok && intField(tp, "width") == 192 && tp.attachments.size() == 1);
        CHECK(docOf(tp) != g);
    }

    // palette reduce
    {
        raster::Image pic(64, 64);
        for (int y = 0; y < 64; ++y) {
            for (int x = 0; x < 64; ++x) pic.set(x, y, {static_cast<std::uint8_t>(x * 4), static_cast<std::uint8_t>(y * 4), static_cast<std::uint8_t>((x + y) * 2), x < 4 ? std::uint8_t{0} : std::uint8_t{255}});
        }
        raster::save(pic, dir / "pic.png");
        CHECK(forge::countColors(pic) > 1000);
        for (const char* method : {"median_cut", "kmeans"}) {
            for (bool dither : {false, true}) {
                Result pr = s.run("forge.palette", {{"input", (dir / "pic.png").string()}, {"output", (dir / "pic16.png").string()},
                                                    {"colors", std::int64_t{16}}, {"method", std::string(method)}, {"dither", dither}});
                CHECK(pr.ok);
                raster::Image q = raster::load(dir / "pic16.png");
                CHECK(forge::countColors(q) <= 16);
                CHECK(q.at(1, 1).a == 0 && q.at(10, 10).a == 255);
            }
        }
        Result fixed = s.run("forge.palette", {{"input", (dir / "pic.png").string()}, {"output", (dir / "pic2.png").string()}, {"palette", std::string("#000000, #ffffff")}});
        CHECK(fixed.ok && forge::countColors(raster::load(dir / "pic2.png")) <= 2);
        CHECK(!s.run("forge.palette", {{"input", (dir / "pic.png").string()}}).ok);  // no output
        CHECK(!s.run("forge.palette", {{"input", (dir / "pic.png").string()}, {"output", (dir / "x.png").string()}, {"palette", std::string("#zz")}}).ok);
    }

    // upscale, outline, power of two
    {
        raster::Image spr(4, 4, {0, 0, 0, 0});
        spr.set(1, 1, {255, 0, 0, 255});
        spr.set(2, 1, {255, 0, 0, 255});
        const std::string id = addImage(s, spr);
        Result o = s.run("forge.outline", {{"doc", id}, {"color", std::string("#00ff00")}});
        CHECK(o.ok);
        raster::Image out = s.workspace().getAs<raster::ImageDoc>(id)->flatten();
        CHECK(out.at(0, 1) == raster::Color{0, 255, 0, 255} && out.at(1, 0) == raster::Color{0, 255, 0, 255});
        CHECK(out.at(0, 0).a == 0 && out.at(1, 1) == raster::Color{255, 0, 0, 255});
        CHECK(intField(s.run("forge.outline", {{"doc", id}, {"expand", true}, {"thickness", std::int64_t{2}}}), "width") == 8);
        Result u = s.run("forge.upscale", {{"doc", id}, {"factor", std::int64_t{3}}});
        CHECK(u.ok && intField(u, "width") == 24);
        Result pot = s.run("forge.resize_pot", {{"doc", id}, {"mode", std::string("up")}});
        CHECK(pot.ok && intField(pot, "width") == 32 && intField(pot, "height") == 32);
        CHECK(forge::nearestPowerOfTwo(700) == 512 && forge::nearestPowerOfTwo(800) == 1024 && forge::nextPowerOfTwo(513) == 1024);
    }

    // channel pack
    {
        raster::save(raster::Image(8, 8, {10, 10, 10, 255}), dir / "ao.png");
        raster::save(raster::Image(4, 4, {20, 20, 20, 255}), dir / "rough.png");
        const std::string metal = addImage(s, raster::Image(8, 8, {30, 30, 30, 255}));
        Result cp = s.run("forge.channel_pack", {{"red", (dir / "ao.png").string()}, {"green", (dir / "rough.png").string()},
                                                 {"blue", metal}, {"output", (dir / "orm.png").string()}});
        CHECK(cp.ok && intField(cp, "width") == 8);
        raster::Image orm = raster::load(dir / "orm.png");
        CHECK(orm.at(3, 3) == raster::Color{10, 20, 30, 255});
        CHECK(!s.run("forge.channel_pack", {{"output", (dir / "x.png").string()}}).ok);
        CHECK(!s.run("forge.channel_pack", {{"red", (dir / "missing.png").string()}}).ok);
    }

    // trim
    {
        raster::Image t(20, 20, {0, 0, 0, 0});
        for (int y = 7; y < 11; ++y) {
            for (int x = 5; x < 8; ++x) t.set(x, y, {1, 2, 3, 255});
        }
        raster::save(t, dir / "trim.png");
        Result tr = s.run("forge.trim", {{"input", (dir / "trim.png").string()}, {"output", (dir / "trimmed.png").string()}});
        CHECK(tr.ok && intField(tr, "x") == 5 && intField(tr, "y") == 7 && intField(tr, "width") == 3 && intField(tr, "height") == 4);
        CHECK(intField(tr, "original_width") == 20);
        Result padded = s.run("forge.trim", {{"input", (dir / "trim.png").string()}, {"output", (dir / "trimmed.png").string()}, {"padding", std::int64_t{1}}});
        CHECK(intField(padded, "x") == 4 && intField(padded, "width") == 5);
        raster::save(raster::Image(4, 4), dir / "empty.png");
        CHECK(!s.run("forge.trim", {{"input", (dir / "empty.png").string()}, {"output", (dir / "e.png").string()}}).ok);
    }

    // ---- sprite sheets ----
    {
        fs::create_directories(dir / "frames");
        std::vector<raster::Image> originals;
        const int sizes[][2] = {{10, 12}, {16, 16}, {7, 20}, {30, 5}, {12, 12}, {9, 9}};
        for (int i = 0; i < 6; ++i) {
            raster::Image f(sizes[i][0], sizes[i][1], {static_cast<std::uint8_t>(40 * i), 200, static_cast<std::uint8_t>(255 - 40 * i), 255});
            f.set(0, 0, {255, 255, 255, 255});
            originals.push_back(f);
            raster::save(f, dir / "frames" / ("run_" + std::to_string(i + 1) + ".png"));
        }
        // a frame with transparent borders, for trimming (sorts last: run_10)
        raster::Image padded(20, 20, {0, 0, 0, 0});
        for (int y = 3; y < 9; ++y) {
            for (int x = 4; x < 14; ++x) padded.set(x, y, {9, 9, 9, 255});
        }
        originals.push_back(padded);
        raster::save(padded, dir / "frames" / "run_10.png");

        for (const char* layout : {"packed", "grid"}) {
            for (bool trim : {false, true}) {
                const std::string name = std::string(layout) + (trim ? "_trim" : "");
                Result sh = s.run("forge.sprite_sheet", {{"input_dir", (dir / "frames").string()},
                                                         {"output", (dir / "sheets" / (name + ".png")).string()},
                                                         {"layout", std::string(layout)},
                                                         {"padding", std::int64_t{2}},
                                                         {"pot", trim},
                                                         {"trim", trim}});
                CHECK(sh.ok && intField(sh, "frames") == 7);
                std::ifstream in(dir / "sheets" / (name + ".json"));
                json atlas = json::parse(in);
                raster::Image sheet = raster::load(dir / "sheets" / (name + ".png"));
                CHECK(atlas["meta"]["size"]["w"] == sheet.width() && atlas["meta"]["size"]["h"] == sheet.height());
                CHECK(atlas["meta"]["image"] == name + ".png");
                CHECK(atlas["frames"].size() == 7);
                if (trim) CHECK(sheet.width() == static_cast<int>(forge::nextPowerOfTwo(sheet.width())));
                std::vector<forge::Rect> rects;
                int k = 0;
                std::vector<std::string> expectedOrder = {"run_1.png", "run_2.png", "run_3.png", "run_4.png", "run_5.png", "run_6.png", "run_10.png"};
                bool ordered = true, inside = true, matches = true;
                for (auto it = atlas["frames"].begin(); it != atlas["frames"].end(); ++it, ++k) {
                    ordered = ordered && it.key() == expectedOrder[static_cast<std::size_t>(k)];
                    const auto& f = it.value()["frame"];
                    forge::Rect r{f["x"].get<int>(), f["y"].get<int>(), f["w"].get<int>(), f["h"].get<int>()};
                    inside = inside && r.x >= 2 && r.y >= 2 && r.x + r.w <= sheet.width() && r.y + r.h <= sheet.height();
                    rects.push_back(r);
                    // the pixels in the frame rect are the original's (trimmed part)
                    const auto& sss = it.value()["spriteSourceSize"];
                    const raster::Image& orig = originals[static_cast<std::size_t>(k)];
                    CHECK(it.value()["sourceSize"]["w"] == orig.width());
                    for (int y = 0; y < r.h && matches; ++y) {
                        for (int x = 0; x < r.w && matches; ++x) {
                            matches = sheet.at(r.x + x, r.y + y) == orig.at(sss["x"].get<int>() + x, sss["y"].get<int>() + y);
                        }
                    }
                }
                CHECK(ordered && inside && matches);
                bool overlap = false;
                for (std::size_t i = 0; i < rects.size(); ++i) {
                    for (std::size_t j = i + 1; j < rects.size(); ++j) overlap = overlap || rects[i].overlaps(rects[j]);
                }
                CHECK(!overlap);
                if (trim && std::string(layout) == "packed") CHECK(atlas["frames"]["run_10.png"]["trimmed"] == true && atlas["frames"]["run_10.png"]["frame"]["w"] == 10);

                // split by the atlas gets every original back, names and all
                Result sp = s.run("forge.sprite_split", {{"input", (dir / "sheets" / (name + ".png")).string()},
                                                         {"atlas", (dir / "sheets" / (name + ".json")).string()},
                                                         {"output_dir", (dir / ("split_" + name)).string()}});
                CHECK(sp.ok && intField(sp, "frames") == 7);
                bool same = true;
                for (std::size_t i = 0; i < expectedOrder.size(); ++i) {
                    same = same && sameImage(raster::load(dir / ("split_" + name) / expectedOrder[i]), originals[i]);
                }
                CHECK(same);
            }
        }

        // grid round trip by rows/cols with padding
        fs::create_directories(dir / "walk");
        std::vector<raster::Image> walk;
        for (int i = 0; i < 6; ++i) {
            raster::Image f(8, 10, {0, 0, 0, 0});
            for (int y = 2; y < 8; ++y) f.set(i + 1, y, {255, static_cast<std::uint8_t>(i * 40), 0, 255});
            walk.push_back(f);
            raster::save(f, dir / "walk" / ("walk_" + std::to_string(i) + ".png"));
        }
        Result g = s.run("forge.sprite_sheet", {{"input_dir", (dir / "walk").string()}, {"output", (dir / "walk.png").string()},
                                                {"layout", std::string("grid")}, {"columns", std::int64_t{3}}, {"padding", std::int64_t{1}}});
        CHECK(g.ok && intField(g, "width") == 1 + 3 * 9 && intField(g, "height") == 1 + 2 * 11);
        Result sp = s.run("forge.sprite_split", {{"input", (dir / "walk.png").string()}, {"output_dir", (dir / "walk_split").string()},
                                                 {"cols", std::int64_t{3}}, {"rows", std::int64_t{2}}, {"padding", std::int64_t{1}}});
        CHECK(sp.ok && intField(sp, "frames") == 6);
        bool same = true;
        for (int i = 0; i < 6; ++i) same = same && sameImage(raster::load(dir / "walk_split" / ("walk_0" + std::to_string(i) + ".png")), walk[static_cast<std::size_t>(i)]);
        CHECK(same);
        Result byCell = s.run("forge.sprite_split", {{"doc", docOf(g)}, {"output_dir", (dir / "walk_cells").string()}, {"cell_width", std::int64_t{8}},
                                                     {"cell_height", std::int64_t{10}}, {"padding", std::int64_t{1}}, {"prefix", std::string("w")}});
        CHECK(byCell.ok && intField(byCell, "frames") == 6 && fs::exists(dir / "walk_cells" / "w_05.png"));
        CHECK(!s.run("forge.sprite_split", {{"input", (dir / "walk.png").string()}, {"output_dir", (dir / "x").string()}}).ok);
        Result list = s.run("forge.sprite_sheet", {{"files", (dir / "walk" / "walk_1.png").string() + "," + (dir / "walk" / "walk_0.png").string()},
                                                   {"output", (dir / "two.png").string()}});
        CHECK(list.ok && intField(list, "frames") == 2);
        CHECK(!s.run("forge.sprite_sheet", {{"output", (dir / "none.png").string()}}).ok);
        CHECK(forge::naturalLess("walk_2", "walk_10") && !forge::naturalLess("walk_10", "walk_2") && forge::naturalLess("a", "b"));
    }

    // ---- MCP ----
    {
        Session ms;
        forge::registerCommands(ms);
        mcp::Server server(ms, {"wetwaresuite", "test", "forge"});
        json list = json::parse(*server.handle(R"({"jsonrpc":"2.0","id":1,"method":"tools/list"})"))["result"]["tools"];
        bool sawPreview = false;
        for (const auto& t : list) {
            if (t["name"] == "forge_mesh_info") CHECK(t["annotations"]["readOnlyHint"] == true);
            if (t["name"] == "forge_mesh_lods") CHECK(t["annotations"]["readOnlyHint"] == false);
            sawPreview = sawPreview || t["name"] == "forge_mesh_preview";
        }
        CHECK(sawPreview && list.size() >= 24);
        json o = call(server, 2, "forge_mesh_open", {{"path", (dir / "ball.obj").string()}});
        CHECK(o["isError"] == false && o["structuredContent"]["doc"] == "mesh1");
        json pv = call(server, 3, "forge_mesh_preview", {{"doc", "mesh1"}, {"view", "front"}, {"size", 128}});
        CHECK(pv["isError"] == false);
        CHECK(pv["content"].size() == 2 && pv["content"][1]["type"] == "image" && pv["content"][1]["mimeType"] == "image/png");
        json d = call(server, 4, "forge_mesh_decimate", {{"doc", "mesh1"}, {"ratio", 0.25}, {"preview", true}});
        CHECK(d["isError"] == false && d["content"].size() == 2 && d["content"][1]["type"] == "image");
        json n = call(server, 5, "forge_normal_map", {{"input", (dir / "pic.png").string()}, {"preview", true}});
        CHECK(n["isError"] == false && n["structuredContent"]["doc"] == "img1" && n["content"].size() == 2);
        CHECK(call(server, 6, "undo", json::object())["isError"] == false);
    }

    testSprites(dir / "sprites");

    fs::remove_all(dir);
    return check::summary("forge");
}
