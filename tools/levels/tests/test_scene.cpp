// Godot-style scenes: building a scene with commands, the .tscn it writes, reading it back,
// moving nodes the way the viewport does, clicking nodes, 3D transforms and levels in scenes.
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "wetwaresuite/core/cli.hpp"
#include "wetwaresuite/levels/levels.hpp"
#include "wetwaresuite/levels/scene.hpp"
#include "wetwaresuite/raster/codec.hpp"
#include "check.hpp"

using namespace wetwaresuite;
namespace fs = std::filesystem;

namespace {

fs::path tempDir() {
    const auto stamp = static_cast<unsigned long long>(std::chrono::high_resolution_clock::now().time_since_epoch().count());
    fs::path d = fs::temp_directory_path() / ("wetwaresuite_scene_test_" + std::to_string(stamp));
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

std::string text(const Result& r, const std::string& key) {
    const Value* v = r.field(key);
    if (!v) return {};
    if (auto s = std::get_if<std::string>(v)) return *s;
    return {};
}

double number(const Result& r, const std::string& key) {
    const Value* v = r.field(key);
    if (!v) return -9999;
    if (auto d = std::get_if<double>(v)) return *d;
    if (auto i = std::get_if<std::int64_t>(v)) return static_cast<double>(*i);
    return -9998;
}

std::string readAll(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    std::stringstream s;
    s << in.rdbuf();
    return s.str();
}

// the value of one property in scene.inspect's lists
std::string valueOf(Session& s, const std::string& doc, const std::string& node, const std::string& prop) {
    Result r = run(s, "scene.inspect doc=" + doc + " node=" + node);
    std::vector<std::string> names, values;
    std::stringstream a(text(r, "names")), b(text(r, "values"));
    std::string x;
    while (std::getline(a, x, '|')) names.push_back(x);
    while (std::getline(b, x, '|')) values.push_back(x);
    for (std::size_t i = 0; i < names.size() && i < values.size(); ++i)
        if (names[i] == prop) return values[i];
    return "?";
}

std::vector<double> nums(const std::string& s) {
    std::vector<double> out;
    const char* p = s.c_str();
    while (*p) {
        char* end = nullptr;
        const double v = std::strtod(p, &end);
        if (end == p) {
            ++p;
            continue;
        }
        out.push_back(v);
        p = end;
    }
    return out;
}

bool near(double a, double b, double tol = 1e-3) { return std::abs(a - b) <= tol; }

void writePng(const fs::path& p, int w, int h, raster::Color c) {
    fs::create_directories(p.parent_path());
    std::ofstream(p, std::ios::binary) << raster::encodePng(raster::Image(w, h, c));
}

void test2D(const fs::path& dir) {
    Session s;
    levels::registerCommands(s);
    scene::registerCommands(s);
    writePng(dir / "art" / "hero.png", 16, 24, {60, 120, 210, 255});
    CHECK(run(s, "scene.project dir=" + q(dir) + " name=Test main_scene=main.tscn width=640 height=360").ok);
    CHECK(fs::exists(dir / "project.godot"));
    CHECK(readAll(dir / "project.godot").find("config/name=\"Test\"") != std::string::npos);

    Result r = run(s, "scene.new root=Node2D name=Main path=" + q(dir / "main.tscn"));
    CHECK(r.ok);
    const std::string doc = text(r, "doc").empty() ? std::string("scn1") : text(r, "doc");
    CHECK(run(s, "scene.add doc=" + doc + " type=CharacterBody2D name=Player props=\"position=64,140\"").ok);
    CHECK(run(s, "scene.add doc=" + doc + " type=Sprite2D parent=Player props=\"texture=" + (dir / "art" / "hero.png").generic_string() + "\"").ok);
    CHECK(run(s, "scene.add doc=" + doc + " type=CollisionShape2D parent=Player props=\"shape=capsule:7x24\"").ok);
    CHECK(!run(s, "scene.add doc=" + doc + " type=NoSuchNode").ok);
    // undo takes back one add
    CHECK(run(s, "scene.add doc=" + doc + " type=Marker2D name=Spare").ok);
    CHECK(run(s, "undo").ok);
    r = run(s, "scene.tree doc=" + doc);
    CHECK(number(r, "count") == 4);
    CHECK(text(r, "nodes").find("Spare") == std::string::npos);
    CHECK(number(r, "dims") == 2);

    // inspector values come back as typed
    CHECK(valueOf(s, doc, "Player", "position") == "64, 140");
    CHECK(valueOf(s, doc, "Player/CollisionShape2D", "shape") == "capsule:7x24");
    CHECK(valueOf(s, doc, "Player/Sprite2D", "texture") == "res://art/hero.png");
    r = run(s, "scene.inspect doc=" + doc + " node=Player/Sprite2D");
    CHECK(text(r, "sections").find("Sprite2D") != std::string::npos);
    CHECK(text(r, "kinds").find("texture") != std::string::npos);

    // the viewport's Move: a world-space drag under a turned parent lands in the parent's space
    CHECK(run(s, "scene.set doc=" + doc + " node=Player props=\"rotation=90\"").ok);
    CHECK(run(s, "scene.transform doc=" + doc + " node=Player/Sprite2D dx=10 dy=0").ok);
    auto p = nums(valueOf(s, doc, "Player/Sprite2D", "position"));
    CHECK(p.size() == 2 && near(p[0], 0) && near(p[1], -10));
    CHECK(run(s, "scene.transform doc=" + doc + " node=Player rotate=-90").ok);
    CHECK(nums(valueOf(s, doc, "Player", "rotation")).at(0) == 0);
    CHECK(run(s, "scene.transform doc=" + doc + " node=Player/Sprite2D scale=2").ok);
    p = nums(valueOf(s, doc, "Player/Sprite2D", "scale"));
    CHECK(p.size() == 2 && near(p[0], 2) && near(p[1], 2));
    CHECK(run(s, "scene.set doc=" + doc + " node=Player/Sprite2D props=\"position=0,0; scale=1,1\"").ok);

    // a Control scales its size, not its scale
    CHECK(run(s, "scene.add doc=" + doc + " type=Label name=Score props=\"text=coins: 0; position=8,8; size=120,24\"").ok);
    CHECK(run(s, "scene.transform doc=" + doc + " node=Score scale=2").ok);
    p = nums(valueOf(s, doc, "Score", "size"));
    CHECK(p.size() == 2 && near(p[0], 240) && near(p[1], 48));

    // clicking: the topmost node at a world point
    r = run(s, "scene.hit doc=" + doc + " x=64 y=140");
    CHECK(text(r, "node").rfind("Player", 0) == 0);
    r = run(s, "scene.hit doc=" + doc + " x=5000 y=5000");
    CHECK(text(r, "node").empty());
    // the picture reports where the world landed and the selected node's box
    r = run(s, "scene.preview doc=" + doc + " selected=Player/Sprite2D");
    CHECK(r.ok && !r.attachments.empty() && number(r, "scale") > 0);
    CHECK(near(number(r, "sel_x0"), 56) && near(number(r, "sel_y1"), 152));

    // signals, scripts and groups
    CHECK(run(s, "scene.add doc=" + doc + " type=Area2D name=Coin props=\"position=200,100\"").ok);
    CHECK(run(s, "scene.connect doc=" + doc + " signal=body_entered from=Coin to=.").ok);
    CHECK(run(s, "scene.script doc=" + doc + " node=Player path=" + q(dir / "player.gd") + " template=character2d").ok);
    CHECK(readAll(dir / "player.gd").find("extends CharacterBody2D") != std::string::npos);
    CHECK(run(s, "scene.set doc=" + doc + " node=Coin props=\"groups=coins\"").ok);
    // connecting to a node with a script gives the script the method, as Godot does
    CHECK(run(s, "scene.script doc=" + doc + " node=. path=" + q(dir / "main.gd")).ok);
    CHECK(run(s, "scene.add doc=" + doc + " type=Area2D name=Coin2").ok);
    CHECK(run(s, "scene.connect doc=" + doc + " signal=body_entered from=Coin2 to=.").ok);
    CHECK(readAll(dir / "main.gd").find("func _on_coin2_body_entered(body: Node) -> void:") != std::string::npos);

    CHECK(run(s, "scene.save doc=" + doc).ok);
    const std::string tscn = readAll(dir / "main.tscn");
    CHECK(tscn.rfind("[gd_scene", 0) == 0);
    CHECK(tscn.find("format=3") != std::string::npos);
    CHECK(tscn.find("path=\"res://art/hero.png\"") != std::string::npos);
    CHECK(tscn.find("path=\"res://player.gd\"") != std::string::npos);
    CHECK(tscn.find("[node name=\"Player\" type=\"CharacterBody2D\" parent=\".\"]") != std::string::npos);
    CHECK(tscn.find("position = Vector2(64, 140)") != std::string::npos);
    CHECK(tscn.find("type=\"CapsuleShape2D\"") != std::string::npos);
    CHECK(tscn.find("[connection signal=\"body_entered\" from=\"Coin\" to=\".\"") != std::string::npos);
    CHECK(tscn.find("groups=[\"coins\"]") != std::string::npos);

    // and it reads back the same
    r = run(s, "scene.open path=" + q(dir / "main.tscn"));
    CHECK(r.ok);
    const std::string doc2 = text(r, "doc");
    CHECK(!doc2.empty() && doc2 != doc);
    CHECK(valueOf(s, doc2, "Player", "position") == "64, 140");
    CHECK(valueOf(s, doc2, "Player/Sprite2D", "texture") == "res://art/hero.png");
    CHECK(text(run(s, "scene.inspect doc=" + doc2 + " node=Coin"), "groups") == "coins");
}

void testGodotFile(const fs::path& dir) {
    // a scene as Godot writes it, with things this editor doesn't know about: they survive a save
    const std::string godot =
        "[gd_scene load_steps=2 format=3 uid=\"uid://b1x2y3\"]\n\n"
        "[sub_resource type=\"RectangleShape2D\" id=\"RectangleShape2D_a1\"]\n"
        "size = Vector2(32, 16)\n\n"
        "[node name=\"Root\" type=\"Node2D\"]\n"
        "custom_thing = 5\n\n"
        "[node name=\"Wall\" type=\"StaticBody2D\" parent=\".\"]\n"
        "position = Vector2(10, 20)\n"
        "collision_layer = 3\n\n"
        "[node name=\"Shape\" type=\"CollisionShape2D\" parent=\"Wall\"]\n"
        "shape = SubResource(\"RectangleShape2D_a1\")\n";
    std::ofstream(dir / "godot.tscn", std::ios::binary) << godot;
    Session s;
    levels::registerCommands(s);
    scene::registerCommands(s);
    Result r = run(s, "scene.open path=" + q(dir / "godot.tscn"));
    CHECK(r.ok);
    const std::string doc = text(r, "doc");
    CHECK(valueOf(s, doc, "Wall/Shape", "shape") == "rect:32x16");
    CHECK(run(s, "scene.set doc=" + doc + " node=Wall props=\"position=11,20\"").ok);
    CHECK(run(s, "scene.save doc=" + doc + " path=" + q(dir / "godot2.tscn")).ok);
    const std::string out = readAll(dir / "godot2.tscn");
    CHECK(out.find("custom_thing = 5") != std::string::npos);
    CHECK(out.find("collision_layer = 3") != std::string::npos);
    CHECK(out.find("uid=\"uid://b1x2y3\"") != std::string::npos);
    CHECK(out.find("position = Vector2(11, 20)") != std::string::npos);
    CHECK(out.find("size = Vector2(32, 16)") != std::string::npos);
}

void test3D(const fs::path& dir) {
    Session s;
    levels::registerCommands(s);
    scene::registerCommands(s);
    Result r = run(s, "scene.new root=Node3D name=World path=" + q(dir / "world.tscn"));
    const std::string doc = text(r, "doc");
    CHECK(run(s, "scene.add doc=" + doc + " type=MeshInstance3D name=Crate props=\"mesh=box:1x1x1; position=2,0.5,0; rotation=0,30,0\"").ok);
    CHECK(run(s, "scene.add doc=" + doc + " type=Camera3D props=\"position=0,3,8\"").ok);
    CHECK(number(run(s, "scene.tree doc=" + doc), "dims") == 3);
    CHECK(run(s, "scene.transform doc=" + doc + " node=Crate dx=1 rotate=15 axis=y").ok);
    auto pos = nums(valueOf(s, doc, "Crate", "position"));
    CHECK(pos.size() == 3 && near(pos[0], 3) && near(pos[1], 0.5));
    CHECK(run(s, "scene.save doc=" + doc).ok);
    const std::string tscn = readAll(dir / "world.tscn");
    CHECK(tscn.find("transform = Transform3D(") != std::string::npos);
    CHECK(tscn.find("type=\"BoxMesh\"") != std::string::npos);
    r = run(s, "scene.open path=" + q(dir / "world.tscn"));
    const std::string doc2 = text(r, "doc");
    auto rot = nums(valueOf(s, doc2, "Crate", "rotation"));
    CHECK(rot.size() == 3 && near(rot[0], 0, 0.01) && near(rot[1], 45, 0.01) && near(rot[2], 0, 0.01));
    pos = nums(valueOf(s, doc2, "Crate", "position"));
    CHECK(pos.size() == 3 && near(pos[0], 3) && near(pos[1], 0.5) && near(pos[2], 0));
    r = run(s, "scene.preview doc=" + doc2 + " camera=true");
    CHECK(r.ok && text(r, "view") == "3d" && !r.attachments.empty());
}

void testLevelInScene(const fs::path& dir) {
    Session s;
    levels::registerCommands(s);
    scene::registerCommands(s);
    CHECK(run(s, "scene.project dir=" + q(dir) + " name=Lv").ok);
    CHECK(run(s, "levels.new width=10 height=4 tile_width=16").ok);
    CHECK(run(s, "levels.tileset_placeholder doc=lvl1 path=" + q(dir / "art" / "tiles.png") + " style=autotile").ok);
    CHECK(run(s, "levels.fill_rect doc=lvl1 x=0 y=3 width=10 height=1 tile=15").ok);
    Result r = run(s, "scene.new root=Node2D name=Main path=" + q(dir / "main.tscn"));
    const std::string doc = text(r, "doc");
    CHECK(run(s, "scene.add_level doc=" + doc + " level=lvl1 path=" + q(dir / "levels" / "one.tscn")).ok);
    CHECK(fs::exists(dir / "levels" / "one.tscn"));
    r = run(s, "scene.tree doc=" + doc);
    CHECK(text(r, "types").find("instance") != std::string::npos);
    // a click on the level picks the instance, not a node inside it
    r = run(s, "scene.hit doc=" + doc + " x=40 y=56");
    CHECK(text(r, "node") == "one");
    CHECK(run(s, "scene.save doc=" + doc).ok);
    CHECK(readAll(dir / "main.tscn").find("path=\"res://levels/one.tscn\"") != std::string::npos);
}


void testPlay(const fs::path& dir) {
    Session s;
    levels::registerCommands(s);
    scene::registerCommands(s);
    CHECK(run(s, "scene.project dir=" + q(dir) + " name=Play width=640 height=360").ok);
    // the ground: a level with collision, 40 cells wide, the floor at y = 160
    CHECK(run(s, "levels.new width=40 height=12 tile_width=16").ok);
    CHECK(run(s, "levels.tileset_placeholder doc=lvl1 path=" + q(dir / "art" / "tiles.png") + " style=autotile").ok);
    CHECK(run(s, "levels.fill_rect doc=lvl1 x=0 y=10 width=40 height=2 tile=15").ok);
    CHECK(run(s, "levels.collision doc=lvl1").ok);
    Result r = run(s, "scene.new root=Node2D name=Main path=" + q(dir / "main.tscn"));
    const std::string doc = text(r, "doc");
    CHECK(run(s, "scene.add_level doc=" + doc + " level=lvl1 path=" + q(dir / "levels" / "one.tscn")).ok);
    CHECK(run(s, "scene.add doc=" + doc + " type=CharacterBody2D name=Player props=\"position=64,100\"").ok);
    CHECK(run(s, "scene.add doc=" + doc + " type=CollisionShape2D parent=Player props=\"shape=rect:16x24\"").ok);
    CHECK(run(s, "scene.add doc=" + doc + " type=Camera2D parent=Player").ok);
    CHECK(run(s, "scene.script doc=" + doc + " node=Player path=" + q(dir / "player.gd") + " template=character2d").ok);
    CHECK(run(s, "scene.add doc=" + doc + " type=Area2D name=Coin props=\"position=200,148\"").ok);
    CHECK(run(s, "scene.add doc=" + doc + " type=CollisionShape2D parent=Coin props=\"shape=circle:5\"").ok);
    CHECK(run(s, "scene.add doc=" + doc + " type=RigidBody2D name=Crate props=\"position=400,40\"").ok);
    CHECK(run(s, "scene.add doc=" + doc + " type=CollisionShape2D parent=Crate props=\"shape=rect:16x16\"").ok);
    CHECK(run(s, "scene.script doc=" + doc + " node=. path=" + q(dir / "main.gd")).ok);
    CHECK(run(s, "scene.connect doc=" + doc + " signal=body_entered from=Coin to=.").ok);
    {
        std::string code = readAll(dir / "main.gd");
        const auto at = code.find("func _on_coin_body_entered");
        CHECK(at != std::string::npos);
        const auto pass = code.find("\tpass", at);
        if (pass != std::string::npos) code.replace(pass, 5, "\t$Coin.queue_free()");
        std::ofstream(dir / "main.gd", std::ios::binary) << code;
    }
    // standing still: the player lands on the floor and the crate falls onto it
    r = run(s, "scene.play doc=" + doc + " seconds=2");
    CHECK(r.ok);
    CHECK(std::abs(number(r, "Player.y") - 148) < 1.5);  // floor at 160, half the box is 12
    CHECK(std::abs(number(r, "Crate.y") - 152) < 1.5);
    CHECK(number(r, "freed") == 0);
    // running right picks the coin up; a GIF of the run is written
    r = run(s, "scene.play doc=" + doc + " seconds=1.5 input=\"right 0.5-1.5; jump 1.2\" fps=10 path=" + q(dir / "run.gif"));
    CHECK(r.ok);
    CHECK(number(r, "Player.x") > 300);
    CHECK(number(r, "freed") == 1);
    CHECK(r.message.find("Coin freed") != std::string::npos);
    CHECK(readAll(dir / "run.gif").rfind("GIF89a", 0) == 0);
    CHECK(raster::decode(readAll(dir / "run.gif")).width() == 320);
    // live stepping, like the Play button
    r = run(s, "scene.play_step doc=" + doc + " restart=true seconds=0.5 raw=true scale=0.25");
    CHECK(r.ok && number(r, "width") == 160 && !r.attachments.empty() && r.attachments[0].bytes.size() == 160u * 90 * 4);
    r = run(s, "scene.play_step doc=" + doc + " keys=right seconds=0.5");
    CHECK(r.ok && number(r, "Player.x") > 100);
    CHECK(run(s, "scene.play_step doc=" + doc + " stop=true").ok);
    // without the collision layer the play test says so
    CHECK(run(s, "scene.new root=Node2D name=Bare path=" + q(dir / "bare.tscn")).ok);
    r = run(s, "scene.play doc=scn2 seconds=0.2");
    CHECK(r.ok);
}

}  // namespace

int main() {
    const fs::path dir = tempDir();
    fs::create_directories(dir / "a");
    fs::create_directories(dir / "b");
    fs::create_directories(dir / "c");
    fs::create_directories(dir / "d");
    fs::create_directories(dir / "e");
    test2D(dir / "a");
    testGodotFile(dir / "b");
    test3D(dir / "c");
    testLevelInScene(dir / "d");
    testPlay(dir / "e");
    std::error_code ec;
    fs::remove_all(dir, ec);
    return check::summary("scene");
}
