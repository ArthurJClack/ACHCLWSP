#include <chrono>
#include <filesystem>
#include <fstream>
#include <thread>

#include <nlohmann/json.hpp>

#include "wetwaresuite/automate/automate.hpp"
#include "wetwaresuite/convert/convert.hpp"
#include "wetwaresuite/mcp/server.hpp"
#include "wetwaresuite/photo/photo.hpp"
#include "wetwaresuite/raster/codec.hpp"
#include "wetwaresuite/vector/vector.hpp"
#include "check.hpp"

using namespace wetwaresuite;
namespace fs = std::filesystem;
using nlohmann::json;

namespace {

std::int64_t intField(const Result& r, const char* key) {
    const Value* v = r.field(key);
    return v && std::holds_alternative<std::int64_t>(*v) ? std::get<std::int64_t>(*v) : -1;
}

void writeText(const fs::path& p, const std::string& text) {
    std::ofstream out(p, std::ios::binary);
    out << text;
}

void tools(Session& s) {
    photo::registerCommands(s);
    vector::registerCommands(s);
    convert::registerCommands(s);
    automate::registerCommands(s, tools);
}

int imageWidth(const fs::path& p) {
    try {
        return raster::load(p).width();
    } catch (const std::exception&) {
        return -1;
    }
}

json call(mcp::Server& server, int id, const std::string& tool, const json& args) {
    json req = {{"jsonrpc", "2.0"}, {"id", id}, {"method", "tools/call"}, {"params", {{"name", tool}, {"arguments", args}}}};
    return json::parse(*server.handle(req.dump()))["result"];
}

}  // namespace

int main() {
    const fs::path dir = fs::temp_directory_path() / "wetwaresuite_test_automate";
    fs::remove_all(dir);
    fs::create_directories(dir / "in" / "sub");
    fs::create_directories(dir / "logos");
    fs::create_directories(dir / "scripts");

    // ---- pieces ----
    auto vars = automate::parseVars("size=800, name=my photo,list=a\\,b");
    CHECK(vars.size() == 3 && vars["size"] == "800" && vars["name"] == "my photo" && vars["list"] == "a,b");
    CHECK(automate::parseVars(R"({"w": 10, "s": "x"})")["w"] == "10");
    bool threw = false;
    try {
        automate::parseVars("novalue");
    } catch (const ParamError&) {
        threw = true;
    }
    CHECK(threw);

    automate::Vars v = {{"stem", "logo"}, {"img", "img1"}, {"img.width", "640"}, {"out", "my dir"}};
    CHECK(automate::substitute("$stem.png", v) == "logo.png");
    CHECK(automate::substitute("${stem}_small.png", v) == "logo_small.png");
    CHECK(automate::substitute("$stem_small.png", v) == "$stem_small.png");  // unknown name left alone
    CHECK(automate::substitute("w=$img.width doc=$img", v) == "w=640 doc=img1");
    CHECK(automate::substitute("path=$out/x", v, true) == "path=my\\ dir/x");
    CHECK(automate::substitute("cost \\$5", v) == "cost $5" && automate::substitute("cost \\$5", v, true) == "cost \\$5");

    CHECK(automate::globMatch("*.PNG", "a.png"));
    CHECK(automate::globMatch("in/**/*.png", "in/sub/deep/a.png"));
    CHECK(automate::globMatch("in/**/*.png", "in/a.png"));
    CHECK(!automate::globMatch("in/*.png", "in/sub/a.png"));

    CHECK(automate::evalCondition("$x in png,jpg", {{"x", "JPG"}}));
    CHECK(automate::evalCondition("$x not in png,jpg", {{"x", "svg"}}));
    CHECK(automate::evalCondition("ext in .svg", {{"file.ext", "svg"}}));
    CHECK(automate::evalCondition("$w > 100", {{"w", "640"}}));
    CHECK(!automate::evalCondition("$w <= 100", {{"w", "640"}}));
    CHECK(automate::evalCondition("$n == logo", {{"n", "Logo"}}));
    CHECK(automate::evalCondition("$n matches lo*", {{"n", "logo"}}));
    CHECK(automate::evalCondition("not exists " + (dir / "nope").string(), {}));
    CHECK(automate::evalCondition("exists " + dir.string(), {}));

    json y = json::parse(automate::pipelineToJson(R"(# a pipeline
name: logos
vars:
  out: build   # trailing comment
  sizes: [16, 32, "64"]
steps:
  - mkdir: $out
  - for_each: logos/*.svg
    steps:
      - run: vector.open path=$file -> v
        capture: v
      - echo: "hi # not a comment"
  - "photo.new width=1 height=1"
  - name: block
    echo: |
      line one
      line two
)"));
    CHECK(y["name"] == "logos" && y["vars"]["out"] == "build" && y["vars"]["sizes"].size() == 3 && y["vars"]["sizes"][0] == 16);
    CHECK(y["steps"].size() == 4 && y["steps"][1]["for_each"] == "logos/*.svg" && y["steps"][1]["steps"].size() == 2);
    CHECK(y["steps"][1]["steps"][1]["echo"] == "hi # not a comment");
    CHECK(y["steps"][2] == "photo.new width=1 height=1");
    CHECK(y["steps"][3]["echo"] == "line one\nline two");
    threw = false;
    try {
        automate::pipelineToJson("steps:\n  - run: x\n   bad: indent\n");
    } catch (const ParamError& e) {
        threw = std::string(e.what()).find("line") != std::string::npos;
    }
    CHECK(threw);

    // ---- fixtures ----
    for (int i = 0; i < 3; ++i) raster::save(raster::Image(200 + i * 100, 100, {200, 50, 50, 255}), dir / "in" / ("photo" + std::to_string(i) + ".png"));
    raster::save(raster::Image(400, 300, {50, 50, 200, 255}), dir / "in" / "sub" / "deep.jpg");
    writeText(dir / "in" / "notes.txt", "not an image");
    for (const char* name : {"star", "circle"}) {
        writeText(dir / "logos" / (std::string(name) + ".svg"),
                  "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"100\" height=\"100\"><circle cx=\"50\" cy=\"50\" r=\"40\" fill=\"#cc3333\"/></svg>");
    }
    auto files = automate::expandGlob("in/**/*.*", dir);
    CHECK(files.size() == 5);
    CHECK(automate::expandGlob((dir / "in" / "*.png").generic_string()).size() == 3);
    CHECK(automate::expandGlob("in/photo*.png", dir).front().filename() == "photo0.png");

    Session s;
    tools(s);

    // ---- run_script ----
    writeText(dir / "scripts" / "thumb.txt",
              "# Make a small copy of one image.\n# Needs input and out.\nphoto.open path=$input -> img\n"
              "photo.resize doc=$img width=$width\nphoto.save doc=$img path=$out\n");
    Result missing = s.run("automate.run_script", {{"path", (dir / "scripts" / "thumb.txt").string()}, {"vars", std::string("input=x.png")}});
    CHECK(!missing.ok && missing.message.find("out") != std::string::npos && missing.message.find("width") != std::string::npos);
    Result ran = s.run("automate.run_script", {{"path", (dir / "scripts" / "thumb.txt").string()},
                                                {"vars", "input=" + (dir / "in" / "photo0.png").string() + ",width=50,out=" + (dir / "small.png").string()}});
    CHECK(ran.ok && intField(ran, "ran") == 3 && intField(ran, "failed") == 0);
    CHECK(imageWidth(dir / "small.png") == 50);
    CHECK(!s.workspace().ids().empty());  // documents stay open for the next step
    Result dry = s.run("automate.run_script", {{"text", std::string("photo.new width=$w height=$w -> a\nphoto.save doc=$a path=x.png")}, {"dry_run", true}});
    CHECK(dry.ok && dry.message.find("would run: photo.new width=$w") != std::string::npos);
    Result failing = s.run("automate.run_script", {{"text", std::string("photo.new width=10 height=10\nphoto.resize doc=nope width=5\necho after")}});
    CHECK(!failing.ok && failing.message.find("line 2") != std::string::npos && intField(failing, "ran") == 2);
    Result kept = s.run("automate.run_script", {{"text", std::string("photo.resize doc=nope width=5\necho after")}, {"keep_going", true}});
    CHECK(!kept.ok && intField(kept, "ran") == 2 && kept.message.find("after") != std::string::npos);
    // spaces in values survive as one argument
    fs::create_directories(dir / "with space");
    Result spaced = s.run("automate.run_script", {{"text", std::string("photo.new width=8 height=8 -> a\nphoto.save doc=$a path=$out")},
                                                   {"vars", "out=" + (dir / "with space" / "a b.png").string()}});
    CHECK(spaced.ok && fs::exists(dir / "with space" / "a b.png"));
    // a script can't call itself forever
    writeText(dir / "scripts" / "loop.txt", "automate.run_script path=$self vars=self=$self\n");
    Result loop = s.run("automate.run_script", {{"path", (dir / "scripts" / "loop.txt").string()}, {"vars", "self=" + (dir / "scripts" / "loop.txt").string()}});
    CHECK(!loop.ok && loop.message.find("deep") != std::string::npos);

    // ---- pipeline ----
    writeText(dir / "logos.yaml", R"(name: logo sizes
description: Every SVG logo as PNGs at three sizes.
vars:
  out: $pipeline_dir/out/logos
steps:
  - mkdir: $out
  - for_each: $pipeline_dir/logos/*.svg
    steps:
      - run: vector.open path=$file
        capture: v
      - for_each: [x]
        steps: []
      - for: [32, 48]
        as: px
        run: convert.file input=$file output=$out/${stem}_${px}.png width=$px
      - run: vector.save doc=$v path=$out/${stem}_100.png
      - run: vector.save doc=$v path=$out/${stem}_200.png scale=2
      - if: $stem == star
        run: vector.save doc=$v path=$out/${stem}_400.png scale=4
)");
    Result pipe = s.run("automate.pipeline", {{"path", (dir / "logos.yaml").string()}});
    CHECK(pipe.ok);
    if (!pipe.ok) std::printf("%s\n", pipe.message.c_str());
    CHECK(imageWidth(dir / "out" / "logos" / "star_200.png") == 200 && imageWidth(dir / "out" / "logos" / "circle_100.png") == 100);
    CHECK(fs::exists(dir / "out" / "logos" / "star_400.png") && !fs::exists(dir / "out" / "logos" / "circle_400.png"));
    CHECK(imageWidth(dir / "out" / "logos" / "star_48.png") == 48 && imageWidth(dir / "out" / "logos" / "circle_32.png") == 32);
    Result over = s.run("automate.pipeline", {{"path", (dir / "logos.yaml").string()}, {"vars", "out=" + (dir / "out" / "other").generic_string()}, {"dry_run", true}});
    CHECK(over.ok && over.message.find("out/other/star_100.png") != std::string::npos && !fs::exists(dir / "out" / "other"));

    const std::string jsonPipe = R"({"steps": [
        {"set": {"dest": ")" + (dir / "web").generic_string() + R"("}},
        {"mkdir": "$dest"},
        {"for_each": "$pipeline_dir/in/**/*", "ext": "png,jpg", "steps": [
            {"command": "photo.open", "args": {"path": "$file"}, "capture": "img"},
            {"if": "$img.width > 300", "run": "photo.resize doc=$img width=300"},
            {"run": "photo.save doc=$img path=$dest/$stem.jpg quality=80"},
            {"run": "photo.close doc=$img"}
        ]},
        {"run": "photo.open path=missing.png", "on_error": "continue"},
        {"echo": "done $dest"}
    ]})";
    writeText(dir / "web.json", jsonPipe);
    Result web = s.run("automate.pipeline", {{"path", (dir / "web.json").string()}});
    CHECK(!web.ok && intField(web, "failed") == 1);  // the deliberate missing file, which didn't stop it
    CHECK(web.message.find("done") != std::string::npos);
    CHECK(imageWidth(dir / "web" / "photo0.jpg") == 200 && imageWidth(dir / "web" / "photo2.jpg") == 300 && imageWidth(dir / "web" / "deep.jpg") == 300);
    CHECK(!fs::exists(dir / "web" / "notes.jpg"));
    CHECK(!s.run("automate.pipeline", {{"text", std::string("steps:\n  - bogus: 1\n")}}).ok);
    CHECK(!s.run("automate.pipeline", {{"text", std::string("{not json")}}).ok);
    // a per-file pipeline: needs file=, and derives $stem from it
    const std::string perFile = "steps:\n  - run: photo.open path=$file\n    capture: img\n  - if: $img.width > 100\n    run: photo.resize doc=$img width=100\n"
                                "  - run: photo.save doc=$img path=$dest/${stem}_small.png\n";
    Result noFile = s.run("automate.pipeline", {{"text", perFile}, {"vars", "dest=" + (dir / "pf").generic_string()}});
    CHECK(!noFile.ok && noFile.message.find("pass vars=file=") != std::string::npos);
    Result dryFile = s.run("automate.pipeline", {{"text", perFile}, {"dry_run", true}});
    CHECK(dryFile.ok && dryFile.message.find("would check: ") != std::string::npos && dryFile.message.find("needs vars: file, dest") != std::string::npos);
    fs::create_directories(dir / "pf");
    Result withFile = s.run("automate.pipeline", {{"text", perFile},
                                                  {"vars", "file=" + (dir / "in" / "photo2.png").generic_string() + ",dest=" + (dir / "pf").generic_string()}});
    CHECK(withFile.ok && imageWidth(dir / "pf" / "photo2_small.png") == 100);
    // brief output by default, everything with verbose=true
    Result brief = s.run("automate.run_script", {{"text", std::string("photo.new width=3 height=3 -> a\nphoto.info doc=$a")}});
    Result full = s.run("automate.run_script", {{"text", std::string("photo.new width=3 height=3 -> a\nphoto.info doc=$a")}, {"verbose", true}});
    CHECK(brief.ok && full.ok && full.message.size() > brief.message.size() && full.message.find("height: 3") != std::string::npos);

    // ---- watch (foreground, once) ----
    writeText(dir / "scripts" / "shrink.txt", "# Shrink a dropped image.\nphoto.open path=$file -> img\nphoto.resize doc=$img width=40\nphoto.save doc=$img path=$out/$stem.png\n");
    fs::create_directories(dir / "shrunk");
    Result once = s.run("automate.watch", {{"folder", (dir / "in").string()}, {"script", (dir / "scripts" / "shrink.txt").string()},
                                            {"pattern", std::string("*.png")}, {"vars", "out=" + (dir / "shrunk").string()}, {"once", true}, {"quiet", true}});
    CHECK(once.ok && intField(once, "runs") == 3);
    CHECK(imageWidth(dir / "shrunk" / "photo1.png") == 40);
    // state= remembers what was done: the second run only takes the changed file
    const Params stateRun = {{"folder", (dir / "in").string()}, {"script", (dir / "scripts" / "shrink.txt").string()},
                             {"pattern", std::string("*.png")}, {"vars", "out=" + (dir / "shrunk").string()},
                             {"state", (dir / "in" / "done.txt").string()}, {"once", true}, {"quiet", true}};
    Result first = s.run("automate.watch", stateRun);
    CHECK(first.ok && intField(first, "runs") == 3);
    Result again = s.run("automate.watch", stateRun);
    CHECK(again.ok && intField(again, "runs") == 0);
    CHECK(s.run("automate.run_script", {{"text", std::string("photo.new width=64 height=48 -> z\nphoto.save doc=$z path=$p")},
                                        {"vars", "p=" + (dir / "in" / "photo1.png").generic_string()}}).ok);
    Result changed = s.run("automate.watch", stateRun);
    CHECK(changed.ok && intField(changed, "runs") == 1);
    CHECK(!s.run("automate.watch", {{"folder", (dir / "nope").string()}, {"script", (dir / "scripts" / "shrink.txt").string()}}).ok);

    // ---- watch in the background ----
    fs::create_directories(dir / "drop");
    Result started = s.run("automate.watch_start", {{"folder", (dir / "drop").string()}, {"script", (dir / "scripts" / "shrink.txt").string()},
                                                     {"vars", "out=" + (dir / "shrunk").string()}, {"interval", 0.1}, {"settle", 0.0},
                                                     {"log", (dir / "watch.log").string()}});
    CHECK(started.ok);
    std::string id = started.field("id") ? toText(*started.field("id")) : "";
    CHECK(id == "w1");
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    raster::save(raster::Image(90, 90, {0, 255, 0, 255}), dir / "drop" / "dropped.png");
    for (int i = 0; i < 100 && !fs::exists(dir / "shrunk" / "dropped.png"); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(50));
    CHECK(imageWidth(dir / "shrunk" / "dropped.png") == 40);
    // the output file appears before the watcher counts the run, so wait for the count too
    Result status = s.run("automate.watch_status", {});
    for (int i = 0; i < 100 && intField(status, "runs") < 1; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        status = s.run("automate.watch_status", {});
    }
    CHECK(status.ok && status.message.find("w1") != std::string::npos && intField(status, "runs") >= 1);
    CHECK(s.run("automate.watch_stop", {{"id", id}}).ok);
    CHECK(!s.run("automate.watch_stop", {{"id", id}}).ok);
    CHECK(fs::exists(dir / "watch.log"));

    // ---- list_scripts ----
    Result listed = s.run("automate.list_scripts", {{"folder", (dir / "scripts").string()}});
    CHECK(listed.ok && intField(listed, "count") == 3);
    CHECK(listed.message.find("Make a small copy of one image. Needs input and out.") != std::string::npos);
    CHECK(listed.message.find("needs: input, out, width") != std::string::npos);
    CHECK(s.run("automate.list_scripts", {{"folder", dir.string()}}).message.find("Every SVG logo") != std::string::npos);

    // ---- pipeline_save, and what the pipeline builder writes ----
    {
        const std::string built = R"({"name": "b", "steps": [{"command": "photo.new", "args": {"width": "4", "height": "4"}}],
                                      "builder_off": [{"command": "photo.new", "args": {"width": "$nowhere"}, "at": 0}]})";
        Result saved = s.run("automate.pipeline_save", {{"path", (dir / "built" / "b.json").string()}, {"text", built}});
        CHECK(saved.ok && fs::exists(dir / "built" / "b.json"));
        CHECK(!s.run("automate.pipeline_save", {{"path", (dir / "built" / "b.json").string()}, {"text", built}}).ok);  // there already
        CHECK(s.run("automate.pipeline_save", {{"path", (dir / "built" / "b.json").string()}, {"text", built}, {"overwrite", true}}).ok);
        CHECK(!s.run("automate.pipeline_save", {{"path", (dir / "built" / "bad.json").string()}, {"text", std::string("{\"steps\": 5}")}}).ok);
        // a switched-off block's variables aren't asked for, and it doesn't run
        Result ran = s.run("automate.pipeline", {{"path", (dir / "built" / "b.json").string()}});
        CHECK(ran.ok && ran.message.find("1 command, 0 failed") != std::string::npos);
    }

    // ---- MCP ----
    {
        Session ms;
        tools(ms);
        mcp::Server server(ms, {"wetwaresuite", "test", "automate"});
        json list = json::parse(*server.handle(R"({"jsonrpc":"2.0","id":1,"method":"tools/list"})"))["result"]["tools"];
        int seen = 0;
        bool shell = false;
        for (const auto& t : list) {
            const std::string name = t["name"];
            if (name.rfind("automate_", 0) == 0) ++seen;
            shell = shell || name == "automate_shell";
            if (name == "automate_list_scripts") CHECK(t["annotations"]["readOnlyHint"] == true);
            if (name == "automate_pipeline") CHECK(t["annotations"]["readOnlyHint"] == false);
        }
        CHECK(seen == 8 && !shell);
        json r = call(server, 2, "automate_run_script", {{"text", "photo.new width=30 height=20 -> a\nphoto.preview doc=$a"}});
        CHECK(r["isError"] == false && r["structuredContent"]["ran"] == 2);
        CHECK(r["content"].size() == 2 && r["content"][1]["type"] == "image");
        json docs = json::parse(*server.handle(R"({"jsonrpc":"2.0","id":3,"method":"tools/call","params":{"name":"documents"}})"))["result"];
        CHECK(docs["structuredContent"]["documents"].size() == 1);
        json p = call(server, 4, "automate_pipeline", {{"text", "steps:\n  - photo.new width=5 height=5\n"}, {"dry_run", true}});
        CHECK(p["isError"] == false && p["structuredContent"]["ran"] == 1);
    }

    fs::remove_all(dir);
    return check::summary("automate");
}
