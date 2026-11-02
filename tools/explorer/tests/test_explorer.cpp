// explorer: checksums, archives, the trash, and every command with undo.
#include <chrono>
#include <cstdlib>
#include <filesystem>

#include <nlohmann/json.hpp>

#include "check.hpp"
#include "wetwaresuite/core/process.hpp"
#include "wetwaresuite/explorer/explorer.hpp"
#include "wetwaresuite/mcp/server.hpp"
#include "wetwaresuite/raster/codec.hpp"
#include "wetwaresuite/raster/ora.hpp"
#include "wetwaresuite/sysutil/sysutil.hpp"

using namespace wetwaresuite;
using nlohmann::json;
namespace fs = std::filesystem;

namespace {

std::string field(const Result& r, const char* key) {
    const Value* v = r.field(key);
    return v ? toText(*v) : std::string();
}

bool there(const fs::path& p) {
    std::error_code ec;
    return fs::exists(p, ec);
}

void setEnv(const char* k, const std::string& v) {
#ifdef _WIN32
    _putenv_s(k, v.c_str());
#else
    setenv(k, v.c_str(), 1);
#endif
}

}  // namespace

int main() {
    const fs::path dir = fs::temp_directory_path() / ("ww-explorer-test-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    fs::create_directories(dir / "home");
    setEnv("WETWARESUITE_HOME", sysutil::fromPath(dir / "data"));  // the trash goes here
#if defined(__linux__)
    setEnv("XDG_DATA_HOME", sysutil::fromPath(dir / "xdg"));  // the desktop trash, kept in the test folder
    const bool desktopTrash = true;
#else
    setEnv("WETWARESUITE_TRASH", "suite");  // never fill the real Recycle Bin / Trash from a test
    const bool desktopTrash = false;
#endif

    // ---- checksums ----
    {
        CHECK(explorer::md5Hex("") == "d41d8cd98f00b204e9800998ecf8427e");
        CHECK(explorer::md5Hex("abc") == "900150983cd24fb0d6963f7d28e17f72");
        CHECK(explorer::sha1Hex("abc") == "a9993e364706816aba3e25717850c26c9cd0d89d");
        CHECK(explorer::sha256Hex("abc") == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
        CHECK(explorer::sha256Hex(std::string(1000000, 'a')) == "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0");
        CHECK(explorer::sha1Hex("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq") == "84983e441c3bd26ebaae4aa1f95129e5e54670f1");
        CHECK(explorer::crc32("123456789", 9) == 0xCBF43926u);
        sysutil::writeFile(dir / "abc.txt", "abc");
        CHECK(explorer::checksumFile(dir / "abc.txt", "SHA256") == explorer::sha256Hex("abc"));
        CHECK(explorer::checksumFile(dir / "abc.txt", "crc32") == "352441c2");
    }

    // ---- archives ----
    const fs::path src = dir / "src";
    fs::create_directories(src / "sub" / "deeper");
    sysutil::writeFile(src / "a.txt", std::string(5000, 'x') + "end");
    sysutil::writeFile(src / "sub" / "b.bin", std::string("\0\1\2\3", 4));
    sysutil::writeFile(src / "sub" / "deeper" / (std::string(110, 'n') + ".txt"), "long name");
    for (const char* kind : {"zip", "tar", "tar.gz"}) {
        const fs::path out = dir / (std::string("pack.") + kind);
        std::size_t n = explorer::archiveCreate(out, {src}, kind);
        CHECK(n == 3);
        CHECK(explorer::archiveKind(out) == kind);
        auto items = explorer::archiveList(out);
        bool sawLong = false, sawDir = false;
        for (const auto& i : items) {
            if (i.name == "src/sub/deeper/" + std::string(110, 'n') + ".txt") sawLong = true;
            if (i.name == "src/sub/" && i.dir) sawDir = true;
        }
        CHECK(sawLong && sawDir);
        const fs::path dest = dir / (std::string("out-") + kind);
        auto written = explorer::archiveExtract(out, dest);
        CHECK(sysutil::readFile(dest / "src" / "a.txt") == std::string(5000, 'x') + "end");
        CHECK(sysutil::readFile(dest / "src" / "sub" / "b.bin") == std::string("\0\1\2\3", 4));
        CHECK(sysutil::readFile(dest / "src" / "sub" / "deeper" / (std::string(110, 'n') + ".txt")) == "long name");
        bool threw = false;
        try {
            explorer::archiveExtract(out, dest);  // already there
        } catch (const std::exception&) {
            threw = true;
        }
        CHECK(threw);
        CHECK(!explorer::archiveExtract(out, dest, {"src/sub"}, true).empty());
        // other programs read what we write
        if (findProgram("python3")) {
            std::string script = std::string(kind) == "zip" ? "import zipfile,sys; z=zipfile.ZipFile(sys.argv[1]); assert z.testzip() is None; print(len(z.namelist()))"
                                                            : "import tarfile,sys; t=tarfile.open(sys.argv[1]); print(len(t.getnames()))";
            ProcessResult pr = runProgram({"python3", "-c", script, sysutil::fromPath(out)});
            CHECK(pr.exitCode == 0 && sysutil::trim(pr.output) == "6");
        }
    }
    {
        // a zip someone else made, with an unsafe name, is refused
        std::string evil = raster::writeZip({{"../evil.txt", "bad"}, {"ok.txt", "fine"}});
        sysutil::writeFile(dir / "evil.zip", evil);
        bool threw = false;
        try {
            explorer::archiveExtract(dir / "evil.zip", dir / "evil-out");
        } catch (const std::exception&) {
            threw = true;
        }
        CHECK(threw && !there(dir / "evil.txt"));
        CHECK(explorer::archiveList(dir / "evil.zip").size() == 2);
        CHECK(explorer::archiveKind(dir / "abc.txt").empty());
    }

    // ---- folders, trash ----
    {
        auto entries = explorer::listFolder(src);
        CHECK(entries.size() == 2 && entries[0].dir && entries[0].name == "sub" && entries[1].name == "a.txt" && entries[1].size == 5003);
        CHECK(explorer::freeName(src / "a.txt") == src / "a (2).txt");
        std::uint64_t files = 0;
        CHECK(explorer::sizeOf(src, &files) == 5003 + 4 + 9 && files == 3);
        sysutil::writeFile(dir / "gone.txt", "bye");
        std::string id = explorer::moveToTrash(dir / "gone.txt");
        CHECK(!there(dir / "gone.txt") && explorer::trashItems().size() == 1);
        CHECK(explorer::restoreFromTrash(id) == fs::absolute(dir / "gone.txt").lexically_normal());
        CHECK(sysutil::readFile(dir / "gone.txt") == "bye" && explorer::trashItems().empty());
        CHECK(!explorer::places().empty());
    }

    Session s;
    explorer::registerCommands(s);
    const fs::path work = dir / "work";
    fs::create_directories(work / "in");
    sysutil::writeFile(work / "in" / "photo1.jpg", "one");
    sysutil::writeFile(work / "in" / "photo2.jpg", "two");
    sysutil::writeFile(work / "in" / "notes.txt", "hello there\nsecond line\n");
    const std::string in = sysutil::fromPath(work / "in");

    // ---- browsing ----
    {
        Result l = s.run("explorer.list", {{"folder", in}, {"sort", std::string("name")}});
        CHECK(l.ok && field(l, "count") == "3" && l.message.find("notes.txt") != std::string::npos);
        Result info = s.run("explorer.info", {{"path", sysutil::fromPath(work / "in" / "notes.txt")}});
        CHECK(info.ok && field(info, "bytes") == "24");
        Result pv = s.run("explorer.preview", {{"path", sysutil::fromPath(work / "in" / "notes.txt")}, {"lines", std::int64_t{1}}});
        CHECK(pv.ok && field(pv, "kind") == "text" && field(pv, "text") == "hello there\n");
        raster::Image img(40, 20);
        raster::save(img, work / "pic.png");
        Result pi = s.run("explorer.preview", {{"path", sysutil::fromPath(work / "pic.png")}});
        CHECK(pi.ok && field(pi, "kind") == "image" && field(pi, "width") == "40" && pi.attachments.size() == 1);
        Result pz = s.run("explorer.preview", {{"path", sysutil::fromPath(dir / "pack.zip")}});
        CHECK(pz.ok && field(pz, "kind") == "archive");
        Result pb = s.run("explorer.preview", {{"path", sysutil::fromPath(src / "sub" / "b.bin")}});
        CHECK(pb.ok && field(pb, "kind") == "binary");
        Result sr = s.run("explorer.search", {{"folder", sysutil::fromPath(work)}, {"name", std::string("*.jpg")}});
        CHECK(sr.ok && field(sr, "count") == "2");
        CHECK(field(s.run("explorer.search", {{"folder", sysutil::fromPath(work)}, {"contains", std::string("SECOND")}}), "count") == "1");
        CHECK(field(s.run("explorer.search", {{"folder", sysutil::fromPath(work)}, {"name", std::string("photo")}, {"kind", std::string("file")}}), "count") == "2");
        CHECK(field(s.run("explorer.search", {{"folder", sysutil::fromPath(work)}, {"min_size", 10.0}}), "count") != "0");
        Result sz = s.run("explorer.size", {{"folder", sysutil::fromPath(work)}});
        CHECK(sz.ok && sz.message.find("in/") != std::string::npos);
        CHECK(s.run("explorer.places", {}).ok);
    }

    // ---- changes and undo ----
    {
        const std::string out = sysutil::fromPath(work / "out");
        Result c = s.run("explorer.copy", {{"paths", sysutil::fromPath(work / "in" / "photo1.jpg") + "|" + sysutil::fromPath(work / "in" / "notes.txt")}, {"to", out}});
        CHECK(c.ok && there(work / "out" / "photo1.jpg") && there(work / "out" / "notes.txt"));
        Result c2 = s.run("explorer.copy", {{"paths", sysutil::fromPath(work / "in" / "photo1.jpg")}, {"to", out}});
        CHECK(c2.ok && there(work / "out" / "photo1 (2).jpg"));
        CHECK(s.history().undo());
        CHECK(!there(work / "out" / "photo1 (2).jpg"));
        CHECK(s.history().undo());
        CHECK(!there(work / "out" / "photo1.jpg"));
        CHECK(s.history().redo());
        CHECK(there(work / "out" / "photo1.jpg"));

        Result mv = s.run("explorer.move", {{"paths", sysutil::fromPath(work / "out" / "notes.txt")}, {"to", sysutil::fromPath(work / "moved")}});
        CHECK(mv.ok && there(work / "moved" / "notes.txt") && !there(work / "out" / "notes.txt"));
        CHECK(s.history().undo());
        CHECK(there(work / "out" / "notes.txt"));

        CHECK(s.run("explorer.rename", {{"path", sysutil::fromPath(work / "out" / "notes.txt")}, {"name", std::string("renamed.txt")}}).ok);
        CHECK(there(work / "out" / "renamed.txt"));
        CHECK(!s.run("explorer.rename", {{"path", sysutil::fromPath(work / "out" / "renamed.txt")}, {"name", std::string("a/b")}}).ok);
        CHECK(s.history().undo());
        CHECK(there(work / "out" / "notes.txt"));

        Result del = s.run("explorer.delete", {{"paths", sysutil::fromPath(work / "out")}});
        CHECK(del.ok && !there(work / "out"));
        if (desktopTrash) {
            // in the freedesktop trash, with its .trashinfo, and listed as such
            CHECK(field(del, "system") == "1" && fs::is_directory(dir / "xdg" / "Trash" / "files" / "out"));
            const std::string info = sysutil::readFile(dir / "xdg" / "Trash" / "info" / "out.trashinfo");
            CHECK(info.rfind("[Trash Info]\nPath=/", 0) == 0 && info.find("DeletionDate=") != std::string::npos && info.find("/work/out\n") != std::string::npos);
        }
        Result tr = s.run("explorer.trash", {});
        CHECK(tr.ok && field(tr, "rows").find(sysutil::fromPath(fs::absolute(work / "out").lexically_normal()) + "\t") != std::string::npos);  // undone copies are in there too
        CHECK(s.history().undo());
        CHECK(there(work / "out" / "photo1.jpg"));
        if (desktopTrash) CHECK(!there(dir / "xdg" / "Trash" / "files" / "out") && !there(dir / "xdg" / "Trash" / "info" / "out.trashinfo"));

        // delete, then restore by id
        CHECK(s.run("explorer.delete", {{"paths", sysutil::fromPath(work / "out" / "photo1.jpg")}}).ok);
        std::string rows = field(s.run("explorer.trash", {}), "rows");
        std::string id = rows.substr(0, rows.find('\t'));
        CHECK(s.run("explorer.restore", {{"id", id}}).ok && there(work / "out" / "photo1.jpg"));
        if (desktopTrash) {
            // emptied from the desktop trash by someone else: the suite forgets it
            sysutil::writeFile(work / "out" / "extra.txt", "x");
            CHECK(s.run("explorer.delete", {{"paths", sysutil::fromPath(work / "out" / "extra.txt")}}).ok);
            const std::size_t before = explorer::trashItems().size();
            fs::remove(dir / "xdg" / "Trash" / "files" / "extra.txt");
            CHECK(explorer::trashItems().size() == before - 1);
            // system=false keeps it in the suite's own trash
            Result own = s.run("explorer.delete", {{"paths", sysutil::fromPath(work / "out" / "photo1.jpg")}, {"system", false}});
            CHECK(own.ok && field(own, "system") == "0" && !there(dir / "xdg" / "Trash" / "files" / "photo1.jpg"));
            CHECK(s.history().undo() && there(work / "out" / "photo1.jpg"));
            // a second item with the same name gets name.2
            sysutil::writeFile(work / "dup.txt", "1");
            CHECK(s.run("explorer.delete", {{"paths", sysutil::fromPath(work / "dup.txt")}}).ok);
            sysutil::writeFile(work / "dup.txt", "2");
            CHECK(s.run("explorer.delete", {{"paths", sysutil::fromPath(work / "dup.txt")}}).ok);
            CHECK(sysutil::readFile(dir / "xdg" / "Trash" / "files" / "dup.txt.2") == "2");
        }

        CHECK(s.run("explorer.new_folder", {{"path", sysutil::fromPath(work / "new" / "deep")}}).ok && fs::is_directory(work / "new" / "deep"));
        CHECK(s.history().undo());
        CHECK(!there(work / "new"));
        CHECK(s.run("explorer.new_file", {{"path", sysutil::fromPath(work / "hello.md")}, {"text", std::string("# hi")}}).ok);
        CHECK(sysutil::readFile(work / "hello.md") == "# hi");
        CHECK(!s.run("explorer.new_file", {{"path", sysutil::fromPath(work / "hello.md")}}).ok);

        // rename many
        Result dry = s.run("explorer.batch_rename", {{"folder", in}, {"files", std::string("*.jpg")}, {"number", std::string("holiday {n:3}")}, {"dry_run", true}});
        CHECK(dry.ok && field(dry, "count") == "2" && dry.message.find("photo1.jpg -> holiday 001.jpg") != std::string::npos);
        CHECK(there(work / "in" / "photo1.jpg"));
        Result withExt = s.run("explorer.batch_rename", {{"folder", in}, {"files", std::string("*.jpg")}, {"number", std::string("trip-{n}.JPG")}, {"dry_run", true}});
        CHECK(withExt.ok && withExt.message.find("photo1.jpg -> trip-1.JPG\n") != std::string::npos);
        CHECK(s.run("explorer.batch_rename", {{"folder", in}, {"find", std::string("photo")}, {"replace", std::string("pic")}}).ok);
        CHECK(there(work / "in" / "pic1.jpg") && there(work / "in" / "pic2.jpg") && !there(work / "in" / "photo1.jpg"));
        CHECK(s.history().undo());
        CHECK(there(work / "in" / "photo1.jpg") && !there(work / "in" / "pic1.jpg"));
        // swapping names works through temporary names
        CHECK(s.run("explorer.batch_rename", {{"folder", in}, {"files", std::string("photo*")}, {"find", std::string("photo(\\d)")}, {"replace", std::string("photo$1x")}, {"regex", true}}).ok);
        CHECK(there(work / "in" / "photo1x.jpg"));

        // archives through commands
        Result ar = s.run("explorer.archive", {{"paths", in}, {"output", sysutil::fromPath(work / "in.zip")}});
        CHECK(ar.ok && field(ar, "files") == "3");
        Result al = s.run("explorer.archive_list", {{"path", sysutil::fromPath(work / "in.zip")}});
        CHECK(al.ok && field(al, "count") == "4" && field(al, "format") == "zip");
        Result ex = s.run("explorer.extract", {{"path", sysutil::fromPath(work / "in.zip")}});
        CHECK(ex.ok && there(work / "in (2)" / "in" / "notes.txt"));  // "in" is taken, so a free name
        CHECK(s.history().undo());
        CHECK(!there(work / "in (2)"));
        CHECK(s.history().undo());
        CHECK(!there(work / "in.zip"));

        // checks
        Result ck = s.run("explorer.checksum", {{"paths", sysutil::fromPath(dir / "abc.txt")}, {"expected", explorer::sha256Hex("abc")}});
        CHECK(ck.ok && field(ck, "matches") == "true");
        CHECK(!s.run("explorer.checksum", {{"paths", sysutil::fromPath(dir / "abc.txt")}, {"expected", std::string("00")}}).ok);
        CHECK(field(s.run("explorer.checksum", {{"paths", sysutil::fromPath(dir / "abc.txt")}, {"algorithm", std::string("md5")}}), "checksum") == "900150983cd24fb0d6963f7d28e17f72");
        Result cmp = s.run("explorer.compare", {{"a", sysutil::fromPath(src)}, {"b", sysutil::fromPath(dir / "out-zip" / "src")}});
        CHECK(cmp.ok && field(cmp, "same") == "true");
        sysutil::writeFile(dir / "out-zip" / "src" / "a.txt", "changed");
        sysutil::writeFile(dir / "out-zip" / "src" / "extra.txt", "x");
        Result cmp2 = s.run("explorer.compare", {{"a", sysutil::fromPath(src)}, {"b", sysutil::fromPath(dir / "out-zip" / "src")}});
        CHECK(field(cmp2, "different") == "1" && field(cmp2, "only_b") == "1");
        CHECK(field(s.run("explorer.compare", {{"a", sysutil::fromPath(dir / "abc.txt")}, {"b", sysutil::fromPath(dir / "abc.txt")}}), "same") == "true");
        Result dup = s.run("explorer.duplicates", {{"folder", sysutil::fromPath(dir / "out-tar")}});
        CHECK(dup.ok && field(dup, "groups") == "0");
        Result dup2 = s.run("explorer.duplicates", {{"folder", sysutil::fromPath(dir)}, {"min_size", 100.0}});
        CHECK(dup2.ok && field(dup2, "groups") != "0");  // a.txt unpacked from each archive

        Result empty = s.run("explorer.empty_trash", {});
        CHECK(empty.ok && field(s.run("explorer.trash", {}), "count") == "0");
        if (desktopTrash) CHECK(!there(dir / "xdg" / "Trash" / "files" / "dup.txt") && !there(dir / "xdg" / "Trash" / "info" / "dup.txt.2.trashinfo"));
    }

    // ---- MCP ----
    {
        mcp::Server server(s, {"wetwaresuite", "test", "explorer"});
        json list = json::parse(*server.handle(R"({"jsonrpc":"2.0","id":1,"method":"tools/list"})"))["result"]["tools"];
        int seen = 0;
        for (const auto& t : list) {
            if (std::string(t["name"]).rfind("explorer_", 0) == 0) ++seen;
        }
        CHECK(seen == 23);
        json call = {{"jsonrpc", "2.0"}, {"id", 2}, {"method", "tools/call"},
                     {"params", {{"name", "explorer_list"}, {"arguments", {{"folder", in}}}}}};
        json res = json::parse(*server.handle(call.dump()))["result"];
        CHECK(res["isError"] == false && res["structuredContent"]["count"] == 3);
    }

    std::error_code ec;
    fs::remove_all(dir, ec);
    return check::summary("explorer");
}
