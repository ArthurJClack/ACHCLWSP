#include <filesystem>
#include <fstream>
#include <sstream>

#include <nlohmann/json.hpp>

#include "wetwaresuite/core/process.hpp"
#include "wetwaresuite/library/library.hpp"
#include "wetwaresuite/mcp/server.hpp"
#include "wetwaresuite/mesh/io.hpp"
#include "wetwaresuite/mesh/primitives.hpp"
#include "wetwaresuite/raster/codec.hpp"
#include "wetwaresuite/raster/ops.hpp"
#include "check.hpp"

using namespace wetwaresuite;
namespace fs = std::filesystem;
using nlohmann::json;

namespace {

std::int64_t intField(const Result& r, const char* key) {
    const Value* v = r.field(key);
    return v && std::holds_alternative<std::int64_t>(*v) ? std::get<std::int64_t>(*v) : -1;
}

std::string strField(const Result& r, const char* key) {
    const Value* v = r.field(key);
    return v ? toText(*v) : "";
}

void writeText(const fs::path& p, const std::string& text) {
    std::ofstream out(p, std::ios::binary);
    out << text;
}

// A gradient with a dark square: something perceptual hashes can tell apart.
raster::Image picture(int w, int h, int squareX) {
    raster::Image img(w, h);
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            auto v = static_cast<std::uint8_t>(255 * x / w);
            img.set(x, y, {v, v, 128, 255});
        }
    }
    for (int y = h / 4; y < h / 2; ++y) {
        for (int x = squareX; x < squareX + w / 4 && x < w; ++x) img.set(x, y, {10, 10, 10, 255});
    }
    return img;
}

raster::Image stripes(int w, int h) {
    raster::Image img(w, h);
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            std::uint8_t v = (y / 8) % 2 ? 250 : 5;
            img.set(x, y, {v, v, v, 255});
        }
    }
    return img;
}

json call(mcp::Server& server, int id, const std::string& tool, const json& args) {
    json req = {{"jsonrpc", "2.0"}, {"id", id}, {"method", "tools/call"}, {"params", {{"name", tool}, {"arguments", args}}}};
    return json::parse(*server.handle(req.dump()))["result"];
}

}  // namespace

int main() {
    const fs::path dir = fs::temp_directory_path() / "wetwaresuite_test_library";
    fs::remove_all(dir);
    const fs::path assets = dir / "assets";
    fs::create_directories(assets / "photos");
    fs::create_directories(assets / "models");
    fs::create_directories(assets / ".hidden");

    // ---- unit pieces ----
    CHECK(library::parseSize("10mb") == 10ull * 1024 * 1024);
    CHECK(library::parseSize("1.5k") == 1536);
    CHECK(library::parseSize("200") == 200);
    CHECK(library::formatSize(1536) == "1.5 KB");
    CHECK(library::fnv1a64Hex("") == "cbf29ce484222325");
    CHECK(library::fnv1a64Hex("a") == "af63dc4c8601ec8c");
    CHECK(library::hammingDistance(0, 0xFF) == 8);
    CHECK(library::parseHex64(library::hex64(0x0123456789abcdefull)) == 0x0123456789abcdefull);

    raster::save(picture(320, 200, 40), assets / "photos" / "sunset.png");
    raster::save(picture(320, 200, 40), assets / "photos" / "sunset copy.png");                    // exact duplicate
    raster::save(raster::resize(picture(320, 200, 40), 160, 100), assets / "photos" / "sunset_small.jpg", 92);  // similar
    raster::save(stripes(3840, 64), assets / "photos" / "wide.bmp");
    raster::save(picture(64, 128, 10), assets / "photos" / "tall.tga");
    mesh::save(assets / "models" / "box.stl", mesh::box({10, 20, 30}));
    writeText(assets / "logo.svg", "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"120\" height=\"80\"><rect x=\"10\" y=\"10\" "
                                   "width=\"100\" height=\"60\" fill=\"#3366cc\"/></svg>");
    writeText(assets / "readme.txt", "notes about the assets");
    writeText(assets / ".hidden" / "secret.txt", "hidden");

    auto size = library::imageHeaderSize(assets / "photos" / "sunset_small.jpg");
    CHECK(size && size->first == 160 && size->second == 100);
    CHECK(library::imageHeaderSize(assets / "photos" / "wide.bmp")->first == 3840);
    CHECK(library::imageHeaderSize(assets / "photos" / "tall.tga")->second == 128);

    const bool haveFfmpeg = findProgram("ffmpeg").has_value();
    if (haveFfmpeg) {
        runProgram({findProgram("ffmpeg")->string(), "-v", "error", "-y", "-f", "lavfi", "-i", "sine=frequency=440:duration=2",
                    (assets / "tone.wav").string()});
        runProgram({findProgram("ffmpeg")->string(), "-v", "error", "-y", "-f", "lavfi", "-i", "testsrc=size=320x240:rate=10:duration=2",
                    "-pix_fmt", "yuv420p", (assets / "clip.mp4").string()});
    }
    // a one-page PDF, written by hand
    {
        std::string body = "%PDF-1.4\n";
        std::vector<std::size_t> offsets;
        auto obj = [&](const std::string& o) {
            offsets.push_back(body.size());
            body += std::to_string(offsets.size()) + " 0 obj\n" + o + "\nendobj\n";
        };
        obj("<< /Type /Catalog /Pages 2 0 R >>");
        obj("<< /Type /Pages /Kids [3 0 R] /Count 1 >>");
        obj("<< /Type /Page /Parent 2 0 R /MediaBox [0 0 200 100] /Contents 4 0 R >>");
        std::string stream = "0 0 1 rg 20 20 160 60 re f";
        obj("<< /Length " + std::to_string(stream.size()) + " >>\nstream\n" + stream + "\nendstream");
        std::size_t xref = body.size();
        body += "xref\n0 5\n0000000000 65535 f \n";
        for (auto o : offsets) {
            char buf[32];
            std::snprintf(buf, sizeof buf, "%010zu 00000 n \n", o);
            body += buf;
        }
        body += "trailer\n<< /Size 5 /Root 1 0 R >>\nstartxref\n" + std::to_string(xref) + "\n%%EOF\n";
        writeText(assets / "sheet.pdf", body);
    }

    Session s;
    library::registerCommands(s);

    // ---- index ----
    CHECK(!s.run("library.search", {{"query", std::string("")}}).ok);  // nothing indexed yet (no library in cwd)
    Result idx = s.run("library.index", {{"root", assets.string()}});
    CHECK(idx.ok);
    const std::int64_t expected = 9 + (haveFfmpeg ? 2 : 0);
    CHECK(intField(idx, "added") == expected);
    CHECK(fs::exists(assets / library::kCatalogName));
    Result again = s.run("library.index", {{"root", assets.string()}});
    CHECK(intField(again, "unchanged") == expected && intField(again, "added") == 0);

    auto cat = library::Catalog::load(assets / library::kCatalogName);
    CHECK(static_cast<std::int64_t>(cat.items.size()) == expected);
    const auto* png = cat.find(library::normPath(assets / "photos" / "sunset.png"));
    CHECK(png && png->type == "image" && png->width == 320 && png->height == 200 && png->hash.size() == 16);
    const auto* stl = cat.find(library::normPath(assets / "models" / "box.stl"));
    CHECK(stl && stl->triangles == 12 && stl->bounds.size() == 3 && std::abs(stl->bounds[2] - 30) < 1e-6);
    const auto* svg = cat.find(library::normPath(assets / "logo.svg"));
    CHECK(svg && svg->type == "vector" && svg->width == 120 && svg->height == 80);
    const auto* pdf = cat.find(library::normPath(assets / "sheet.pdf"));
    CHECK(pdf && pdf->pages == 1);
    CHECK(!cat.find(library::normPath(assets / ".hidden" / "secret.txt")));
    if (haveFfmpeg && findProgram("ffprobe")) {
        const auto* wav = cat.find(library::normPath(assets / "tone.wav"));
        CHECK(wav && wav->type == "audio" && wav->duration > 1.9 && wav->duration < 2.1);
        const auto* mp4 = cat.find(library::normPath(assets / "clip.mp4"));
        CHECK(mp4 && mp4->type == "video" && mp4->width == 320 && mp4->height == 240);
    }
    // catalog paths are stored relative to its folder
    {
        std::ifstream in(assets / library::kCatalogName);
        std::string all((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        CHECK(all.find("\"photos/sunset.png\"") != std::string::npos);
    }
    // a catalog from before the rename (old file name and header key) is still found and read
    {
        std::ifstream in(assets / library::kCatalogName, std::ios::binary);
        std::string all((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        const std::string key = "wetwaresuite_library";
        CHECK(all.find(key) != std::string::npos);
        if (all.find(key) != std::string::npos) all.replace(all.find(key), key.size(), "achclwss_library");
        const fs::path oldDir = assets.parent_path() / (assets.filename().string() + "_old");
        fs::create_directories(oldDir);
        writeText(oldDir / library::kOldCatalogName, all);
        CHECK(library::catalogIn(oldDir) == oldDir / library::kOldCatalogName);
        CHECK(library::Catalog::load(library::catalogIn(oldDir)).items.size() == cat.items.size());
        Session old;
        library::registerCommands(old);
        CHECK(old.run("library.search", {{"query", std::string("")}, {"library", oldDir.string()}}).ok);
        fs::remove_all(oldDir);
    }

    // ---- search ----
    auto count = [&](const std::string& q) { return intField(s.run("library.search", {{"query", q}}), "total"); };
    CHECK(count("") == expected);
    CHECK(count("type:image") == 4);  // tga counts as a texture
    CHECK(count("type:picture") == 5);
    CHECK(count("ext:png") == 2);
    CHECK(count("ext:png,jpg") == 3);
    CHECK(count("sunset") == 3);
    CHECK(count("sunset -ext:jpg") == 2);
    CHECK(count("*.png") == 2);
    CHECK(count("width>=1920") == 1);
    CHECK(count("type:picture width>=100 height<150") == 2);  // small jpg and wide bmp
    CHECK(count("orientation:portrait") == 1);
    CHECK(count("type:mesh") == 1);
    CHECK(count("triangles>10") == 1);
    CHECK(count("size>1mb") == 0);
    CHECK(count("modified<1d") == expected);
    CHECK(count("modified>1d") == 0);
    CHECK(count("modified>2000-01-01") == expected);
    CHECK(count("in:" + (assets / "photos").string()) == 5);
    CHECK(count("pages>=1") == 1);
    CHECK(!s.run("library.search", {{"query", std::string("bogus:1")}}).ok);
    CHECK(!s.run("library.search", {{"query", std::string("width>abc")}}).ok);
    Result sorted = s.run("library.search", {{"query", std::string("type:image sort:-width limit:2")}, {"json", true}});
    CHECK(intField(sorted, "count") == 2 && intField(sorted, "total") == 4);
    CHECK(json::parse(strField(sorted, "items"))[0]["name"] == "wide.bmp");
    Result paged = s.run("library.search", {{"query", std::string("type:image")}, {"sort", std::string("name")}, {"limit", std::int64_t{1}}, {"offset", std::int64_t{1}}, {"json", true}});
    CHECK(json::parse(strField(paged, "items"))[0]["name"] == "sunset.png");

    // ---- tags, ratings, labels, notes (undoable) ----
    Result tagged = s.run("library.tag", {{"query", std::string("sunset")}, {"tags", std::string("beach,summer")}});
    CHECK(tagged.ok && intField(tagged, "changed") == 3);
    CHECK(count("tag:beach") == 3);
    CHECK(count("beach") == 3);  // free text covers tags
    CHECK(s.history().undo());
    CHECK(count("tag:beach") == 0);
    CHECK(s.history().redo());
    CHECK(count("tag:beach") == 3);
    CHECK(s.run("library.untag", {{"paths", (assets / "photos" / "sunset_small.jpg").string()}, {"tags", std::string("beach")}}).ok);
    CHECK(count("tag:beach") == 2 && count("tag:summer") == 3);
    CHECK(s.run("library.rate", {{"query", std::string("ext:png")}, {"rating", std::int64_t{4}}}).ok);
    CHECK(!s.run("library.rate", {{"query", std::string("ext:png")}, {"rating", std::int64_t{9}}}).ok);
    CHECK(count("rating>=3") == 2);
    CHECK(s.run("library.label", {{"paths", (assets / "logo.svg").string()}, {"label", std::string("red")}}).ok);
    CHECK(count("label:red") == 1);
    CHECK(s.run("library.note", {{"paths", (assets / "logo.svg").string()}, {"text", std::string("client approved")}}).ok);
    CHECK(count("approved") == 1);
    CHECK(!s.run("library.tag", {{"tags", std::string("x")}}).ok);  // no selection given
    CHECK(!s.run("library.tag", {{"paths", (dir / "nope.png").string()}, {"tags", std::string("x")}}).ok);

    // ---- info ----
    Result info = s.run("library.info", {{"path", (assets / "photos" / "sunset.png").string()}, {"preview", true}});
    CHECK(info.ok && intField(info, "width") == 320 && intField(info, "rating") == 4 && strField(info, "tags") == "beach,summer");
    CHECK(info.attachments.size() == 1 && info.attachments[0].mime == "image/png");
    CHECK(!s.run("library.info", {{"path", (dir / "missing.png").string()}}).ok);

    // ---- duplicates and similar ----
    Result dup = s.run("library.duplicates", {});
    CHECK(dup.ok && intField(dup, "groups") == 1 && intField(dup, "files") == 2);
    CHECK(strField(dup, "extras").find("sunset") != std::string::npos);
    Result sim = s.run("library.similar", {{"path", (assets / "photos" / "sunset.png").string()}, {"threshold", std::int64_t{12}}});
    CHECK(sim.ok);
    CHECK(strField(sim, "paths").find("sunset_small.jpg") != std::string::npos);
    CHECK(strField(sim, "paths").find("wide.bmp") == std::string::npos);
    Result groups = s.run("library.similar", {{"method", std::string("phash")}, {"threshold", std::int64_t{12}}});
    CHECK(groups.ok && intField(groups, "groups") >= 1);
    CHECK(!library::Catalog::load(assets / library::kCatalogName).find(library::normPath(assets / "photos" / "sunset.png"))->dhash.empty());

    // ---- thumbnails and contact sheets ----
    Result th = s.run("library.thumbnails", {{"query", std::string("")}, {"sheet", (dir / "sheet.png").string()}, {"preview", true}});
    CHECK(th.ok && intField(th, "made") + intField(th, "cached") >= 7);
    CHECK(fs::exists(dir / "sheet.png") && th.attachments.size() == 1);
    CHECK(fs::exists(assets / library::kThumbDir));
    if (haveFfmpeg) CHECK(intField(th, "missing") == (findProgram("pdftoppm") ? 1 : 2));  // readme.txt, and sheet.pdf without poppler
    if (findProgram("pdftoppm")) {
        auto c = library::Catalog::load(assets / library::kCatalogName);
        CHECK(fs::exists(library::thumbPath(c, *c.find(library::normPath(assets / "sheet.pdf")), 256)));
    }
    Result prev = s.run("library.search", {{"query", std::string("type:image")}, {"preview", true}});
    CHECK(prev.attachments.size() == 1);
    raster::Image sheet = library::contactSheet({picture(50, 50, 5), raster::Image(), stripes(80, 20)}, 64, 2);
    CHECK(sheet.width() == 2 * (64 + 4) + 4 && sheet.height() == 2 * (64 + 4) + 4);

    // ---- collections ----
    CHECK(s.run("library.collection_save", {{"name", std::string("hero")}, {"query", std::string("rating>=4")}}).ok);
    CHECK(s.run("library.collection_save", {{"name", std::string("picked")}, {"paths", (assets / "logo.svg").string() + "," + (assets / "readme.txt").string()}}).ok);
    CHECK(!s.run("library.collection_save", {{"name", std::string("bad name")}, {"query", std::string("")}}).ok);
    CHECK(count("collection:hero") == 2);
    CHECK(count("collection:picked") == 2);
    CHECK(count("collection:hero,picked") == 4);
    CHECK(intField(s.run("library.collections", {}), "count") == 2);
    CHECK(s.run("library.rate", {{"paths", (assets / "photos" / "tall.tga").string()}, {"rating", std::int64_t{5}}}).ok);
    CHECK(count("collection:hero") == 3);  // smart collections update themselves
    CHECK(intField(s.run("library.search", {{"query", std::string("")}, {"collection", std::string("hero")}}), "total") == 3);

    // ---- stats and export ----
    Result stats = s.run("library.stats", {{"json", true}});
    CHECK(stats.ok && intField(stats, "files") == expected && intField(stats, "duplicate_groups") == 1);
    CHECK(json::parse(strField(stats, "types"))["image"]["files"] == 4);
    CHECK(s.run("library.export", {{"output", (dir / "list.csv").string()}, {"query", std::string("type:image")}}).ok);
    {
        std::ifstream in(dir / "list.csv");
        std::string header;
        std::getline(in, header);
        CHECK(header.rfind("path,name,type", 0) == 0);
        int lines = 0;
        for (std::string l; std::getline(in, l);) ++lines;
        CHECK(lines == 4);
    }
    CHECK(s.run("library.export", {{"output", (dir / "list.json").string()}, {"fields", std::string("name,rating,tags")}}).ok);
    {
        std::ifstream in(dir / "list.json");
        json j = json::parse(in);
        CHECK(static_cast<std::int64_t>(j.size()) == expected && j[0].contains("rating") && !j[0].contains("path"));
    }
    CHECK(!s.run("library.export", {{"output", (dir / "list.xml").string()}}).ok);

    // ---- move, rename, copy ----
    Result dry = s.run("library.move", {{"query", std::string("ext:png")}, {"to", (assets / "best").string()}, {"dry_run", true}});
    CHECK(dry.ok && intField(dry, "moved") == 2 && !fs::exists(assets / "best"));
    Result moved = s.run("library.move", {{"query", std::string("ext:png")}, {"to", (assets / "best").string()}});
    CHECK(moved.ok && intField(moved, "moved") == 2 && fs::exists(assets / "best" / "sunset.png"));
    CHECK(count("in:" + (assets / "best").string() + " tag:beach rating=4") == 2);
    Result ren = s.run("library.rename", {{"query", std::string("in:" + (assets / "best").string())}, {"name", std::string("beach_{n:2}{ext}")}});
    CHECK(ren.ok && intField(ren, "renamed") == 2 && fs::exists(assets / "best" / "beach_01.png") && fs::exists(assets / "best" / "beach_02.png"));
    CHECK(count("name:beach_*") == 2 && count("tag:beach") == 2);
    CHECK(!s.run("library.rename", {{"paths", (assets / "readme.txt").string()}, {"name", std::string("../x.txt")}}).ok);
    Result cp = s.run("library.copy", {{"collection", std::string("picked")}, {"to", (dir / "handoff").string()}});
    CHECK(cp.ok && intField(cp, "copied") == 2 && fs::exists(dir / "handoff" / "logo.svg"));
    CHECK(count("label:red") == 2);  // the copy kept its label

    // ---- incremental rescan: changes, deletions and moves made outside ----
    raster::save(picture(500, 400, 100), assets / "photos" / "tall.tga");  // changed
    fs::remove(assets / "readme.txt");                                        // deleted
    fs::create_directories(assets / "vector");
    fs::rename(assets / "logo.svg", assets / "vector" / "logo.svg");          // moved: keeps its label and note
    Result rescan = s.run("library.rescan", {});
    CHECK(rescan.ok);
    if (intField(rescan, "removed") != 1) std::printf("%s\n", rescan.message.c_str());
    CHECK(intField(rescan, "updated") == 1 && intField(rescan, "moved") == 1 && intField(rescan, "removed") == 1);
    cat = library::Catalog::load(assets / library::kCatalogName);
    const auto* logo = cat.find(library::normPath(assets / "vector" / "logo.svg"));
    CHECK(logo && logo->label == "red" && logo->notes == "client approved");
    CHECK(cat.find(library::normPath(assets / "photos" / "tall.tga"))->width == 500);
    CHECK(cat.find(library::normPath(assets / "photos" / "tall.tga"))->rating == 5);
    CHECK(cat.collections.at("picked").paths[0] == library::normPath(assets / "vector" / "logo.svg"));

    // ---- forget (undoable) ----
    const std::int64_t before = count("");
    CHECK(s.run("library.forget", {{"query", std::string("type:video")}}).ok);
    if (haveFfmpeg) CHECK(count("") == before - 1);
    CHECK(s.history().undo());
    CHECK(count("") == before);

    // ---- a second catalog at a chosen path ----
    Result other = s.run("library.index", {{"root", (assets / "models").string()}, {"library", (dir / "models.jsonl").string()}});
    CHECK(other.ok && intField(other, "total") == 1 && fs::exists(dir / "models.jsonl"));
    CHECK(intField(s.run("library.search", {{"query", std::string("")}}), "total") == 1);  // the last used catalog
    CHECK(intField(s.run("library.search", {{"query", std::string("")}, {"library", assets.string()}}), "total") == before);

    // ---- tags inside the files (XMP) ----
    {
        const fs::path tg = dir / "tagged";
        fs::create_directories(tg);
        raster::save(picture(64, 48, 8), tg / "a.jpg", 90);
        raster::save(picture(64, 48, 8), tg / "b.png");
        writeText(tg / "c.cr2", "not really a raw");
        const std::string lib = (dir / "tagged-lib.json").string();
        Session ts;
        library::registerCommands(ts);
        CHECK(ts.run("library.index", {{"root", tg.string()}, {"library", lib}}).ok);
        CHECK(ts.run("library.tag", {{"query", std::string("")}, {"tags", std::string("beach,café & sun")}}).ok);
        CHECK(ts.run("library.rate", {{"query", std::string("")}, {"rating", std::int64_t{4}}}).ok);
        CHECK(ts.run("library.label", {{"query", std::string("")}, {"label", std::string("red")}}).ok);
        CHECK(ts.run("library.note", {{"query", std::string("")}, {"text", std::string("first <light>")}}).ok);
        Result w = ts.run("library.write_tags", {{"query", std::string("")}});
        CHECK(w.ok && intField(w, "files") == 3);
        CHECK(strField(w, "rows").find("c.cr2\tsidecar c.xmp") != std::string::npos);
        CHECK(fs::exists(tg / "c.xmp"));
        // the pictures still decode, with the tags inside
        CHECK(raster::load(tg / "a.jpg").width() == 64 && raster::load(tg / "b.png").height() == 48);
        for (const char* f : {"a.jpg", "b.png", "c.cr2"}) {
            auto t = library::readFileTags(tg / f);
            CHECK(t && t->rating == 4 && t->label == "red" && t->notes == "first <light>");
            CHECK(t && t->tags.size() == 2 && t->tags[1] == "café & sun");
        }
        // the catalog still knows the photo (no "changed" file on the next rescan)
        Result again = ts.run("library.index", {{"root", tg.string()}, {"library", lib}});
        CHECK(again.ok && intField(again, "total") == 3);
        // a fresh library picks the tags up from the files; sidecars aren't listed as files
        const std::string lib2 = (dir / "tagged-lib2.json").string();
        Result fresh = ts.run("library.index", {{"root", tg.string()}, {"library", lib2}});
        CHECK(fresh.ok && intField(fresh, "total") == 3);
        CHECK(intField(ts.run("library.search", {{"query", std::string("tag:beach rating:4 label:red")}, {"library", lib2}}), "total") == 3);
        // rewriting keeps other XMP and updates ours
        std::string side = [&] { std::ifstream in(tg / "c.xmp"); std::ostringstream o; o << in.rdbuf(); return o.str(); }();
        side.replace(side.find("<rdf:Description rdf:about=\"\""), 29, "<rdf:Description rdf:about=\"\" xmlns:crs=\"http://ns.adobe.com/camera-raw-settings/1.0/\" crs:Exposure2012=\"+0.50\"");
        writeText(tg / "c.xmp", side);
        CHECK(ts.run("library.untag", {{"paths", (tg / "c.cr2").string()}, {"tags", std::string("beach")}, {"library", lib}}).ok);
        CHECK(ts.run("library.write_tags", {{"paths", (tg / "c.cr2").string()}, {"library", lib}}).ok);
        side = [&] { std::ifstream in(tg / "c.xmp"); std::ostringstream o; o << in.rdbuf(); return o.str(); }();
        CHECK(side.find("crs:Exposure2012=\"+0.50\"") != std::string::npos);
        auto ct = library::readFileTags(tg / "c.cr2");
        CHECK(ct && ct->tags.size() == 1 && ct->tags[0] == "café & sun");
        // undo puts the sidecar back as it was
        CHECK(ts.history().undo());
        ct = library::readFileTags(tg / "c.cr2");
        CHECK(ct && ct->tags.size() == 2);
        // read_tags merges what's in the files into a library that lost them
        CHECK(ts.run("library.untag", {{"query", std::string("")}, {"tags", std::string("*")}, {"library", lib2}}).ok);
        Result rd = ts.run("library.read_tags", {{"query", std::string("")}, {"library", lib2}});
        CHECK(rd.ok && intField(rd, "changed") == 3);
        CHECK(intField(ts.run("library.search", {{"query", std::string("tag:beach")}, {"library", lib2}}), "total") == 3);
        // where=embed refuses formats it can't embed in
        CHECK(!ts.run("library.write_tags", {{"paths", (tg / "c.cr2").string()}, {"where", std::string("embed")}, {"library", lib2}}).ok);
    }

    // ---- MCP ----
    {
        Session ms;
        library::registerCommands(ms);
        mcp::Server server(ms, {"wetwaresuite", "test", "library"});
        json list = json::parse(*server.handle(R"({"jsonrpc":"2.0","id":1,"method":"tools/list"})"))["result"]["tools"];
        int seen = 0;
        for (const auto& t : list) {
            const std::string name = t["name"];
            if (name.rfind("library_", 0) == 0) ++seen;
            if (name == "library_search") CHECK(t["annotations"]["readOnlyHint"] == true);
            if (name == "library_move") CHECK(t["annotations"]["readOnlyHint"] == false && t["annotations"]["destructiveHint"] == true);
            if (name == "library_tag") CHECK(t["annotations"]["destructiveHint"] == false);
        }
        CHECK(seen == 23);
        json r = call(server, 2, "library_search", {{"library", assets.string()}, {"query", "type:image"}, {"preview", true}});
        CHECK(r["isError"] == false && r["structuredContent"]["total"] == 4);
        CHECK(r["content"].size() == 2 && r["content"][1]["type"] == "image");
        json t = call(server, 3, "library_tag", {{"query", "ext:bmp"}, {"tags", "texture,4k"}});
        CHECK(t["isError"] == false && t["structuredContent"]["changed"] == 1);
        CHECK(call(server, 4, "undo", json::object())["isError"] == false);
        json q = call(server, 5, "library_search", {{"query", "tag:4k"}});
        CHECK(q["structuredContent"]["total"] == 0);
    }

    fs::remove_all(dir);
    return check::summary("library");
}
