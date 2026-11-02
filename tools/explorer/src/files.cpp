// Folder listings, places, the trash, copying and moving, and opening files in
// the system's own apps.
#include <algorithm>
#include <atomic>
#include <chrono>
#include <ctime>
#include <fstream>
#include <map>
#include <stdexcept>

#include "wetwaresuite/explorer/explorer.hpp"
#include "wetwaresuite/sysutil/sysutil.hpp"
#include "wetwaresuite/toolkit/detect.hpp"

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shellapi.h>
#else
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace wetwaresuite::explorer {

namespace {

bool isHidden(const fs::path& p, const std::string& name) {
    if (!name.empty() && name[0] == '.') return true;
#ifdef _WIN32
    DWORD attrs = GetFileAttributesW(p.c_str());
    return attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_HIDDEN);
#else
    (void)p;
    return false;
#endif
}

std::string typeName(const fs::path& p, bool dir) {
    if (dir) return "folder";
    std::string ext = sysutil::extensionOf(p);
    // cheap names by extension; toolkit::detectFile (reads the file) is for info and preview
    toolkit::FileType t = toolkit::detect({}, p);
    if (!t.format.empty() && t.format != "Plain text" && t.format != "Unknown binary") return t.format;
    return ext.empty() ? "file" : ext.substr(1) + " file";
}

std::string readSmall(const fs::path& p) {
    try {
        return sysutil::readFile(p);
    } catch (const std::exception&) {
        return {};
    }
}

}  // namespace

std::int64_t modifiedTime(const fs::path& p) {
    std::error_code ec;
    auto ft = fs::last_write_time(p, ec);
    if (ec) return 0;
    auto sys = std::chrono::time_point_cast<std::chrono::system_clock::duration>(ft - fs::file_time_type::clock::now() + std::chrono::system_clock::now());
    return static_cast<std::int64_t>(std::chrono::duration_cast<std::chrono::seconds>(sys.time_since_epoch()).count());
}

std::string formatTime(std::int64_t t) {
    if (t <= 0) return "";
    std::time_t tt = static_cast<std::time_t>(t);
    std::tm tm{};
#ifdef _WIN32
    localtime_s(&tm, &tt);
#else
    localtime_r(&tt, &tm);
#endif
    char buf[32];
    std::strftime(buf, sizeof buf, "%Y-%m-%d %H:%M", &tm);
    return buf;
}

std::vector<Entry> listFolder(const fs::path& dir, bool hidden, const std::string& sort, bool descending) {
    std::error_code ec;
    if (!fs::is_directory(dir, ec)) throw std::runtime_error("not a folder: " + sysutil::fromPath(dir));
    std::vector<Entry> out;
    fs::directory_iterator it(dir, fs::directory_options::skip_permission_denied, ec), end;
    if (ec) throw std::runtime_error("can't open " + sysutil::fromPath(dir) + ": " + ec.message());
    for (; it != end; it.increment(ec)) {
        if (ec) break;
        Entry e;
        e.name = sysutil::nameOf(it->path());
        e.path = sysutil::fromPath(it->path());
        std::error_code e1;
        e.dir = it->is_directory(e1);
        e.link = it->is_symlink(e1);
        e.hidden = isHidden(it->path(), e.name);
        if (e.hidden && !hidden) continue;
        e.size = e.dir ? 0 : static_cast<std::uint64_t>(it->file_size(e1));
        if (e1) e.size = 0;
        e.modified = modifiedTime(it->path());
        e.type = typeName(it->path(), e.dir);
        out.push_back(std::move(e));
    }
    auto key = [&](const Entry& a, const Entry& b) {
        if (sort == "size" && a.size != b.size) return a.size < b.size;
        if (sort == "date" && a.modified != b.modified) return a.modified < b.modified;
        if (sort == "type" && a.type != b.type) return a.type < b.type;
        return sysutil::lowerAscii(a.name) < sysutil::lowerAscii(b.name);
    };
    std::sort(out.begin(), out.end(), [&](const Entry& a, const Entry& b) {
        if (a.dir != b.dir) return a.dir;
        return descending ? key(b, a) : key(a, b);
    });
    return out;
}

std::vector<std::pair<std::string, fs::path>> places() {
    std::vector<std::pair<std::string, fs::path>> out;
    const fs::path home = sysutil::homeDir();
    auto add = [&](const std::string& name, const fs::path& p) {
        std::error_code ec;
        if (!p.empty() && fs::is_directory(p, ec)) {
            for (const auto& [n, q] : out) {
                if (q == p) return;
            }
            out.emplace_back(name, p);
        }
    };
    add("Home", home);
    // Linux: the folders named in ~/.config/user-dirs.dirs, which may be translated
    std::map<std::string, fs::path> xdg;
#if !defined(_WIN32) && !defined(__APPLE__)
    for (const auto& line : sysutil::splitLines(readSmall(home / ".config" / "user-dirs.dirs"))) {
        auto eq = line.find('=');
        if (line.rfind("XDG_", 0) != 0 || eq == std::string::npos) continue;
        std::string key = line.substr(4, eq - 4), val = line.substr(eq + 1);
        if (val.size() >= 2 && val.front() == '"') val = val.substr(1, val.size() - 2);
        if (val.rfind("$HOME", 0) == 0) val = sysutil::fromPath(home) + val.substr(5);
        xdg[key] = sysutil::toPath(val);
    }
#endif
    auto user = [&](const std::string& name, const char* key) {
        auto it = xdg.find(key);
        add(name, it != xdg.end() ? it->second : home / name);
    };
    user("Desktop", "DESKTOP_DIR");
    user("Documents", "DOCUMENTS_DIR");
    user("Downloads", "DOWNLOAD_DIR");
    user("Pictures", "PICTURES_DIR");
    user("Music", "MUSIC_DIR");
#ifdef __APPLE__
    add("Movies", home / "Movies");
#else
    user("Videos", "VIDEOS_DIR");
#endif
    add("Suite data", sysutil::dataDir());
    std::error_code ec;
    add("Temporary files", fs::temp_directory_path(ec));
#ifdef _WIN32
    DWORD drives = GetLogicalDrives();
    for (int i = 0; i < 26; ++i) {
        if (drives & (1u << i)) {
            std::string d = std::string(1, static_cast<char>('A' + i)) + ":/";
            add("Drive " + d.substr(0, 2), sysutil::toPath(d));
        }
    }
#else
    add("Computer", "/");
    std::vector<fs::path> mountRoots = {"/Volumes", "/mnt", "/media"};
    std::string me = sysutil::env("USER");
    if (!me.empty()) {
        mountRoots.push_back(fs::path("/media") / me);
        mountRoots.push_back(fs::path("/run/media") / me);
    }
    for (const auto& root : mountRoots) {
        for (const auto& e : fs::directory_iterator(root, fs::directory_options::skip_permission_denied, ec)) {
            std::error_code e2;
            if (e.is_directory(e2) && sysutil::nameOf(e.path()) != me) add(sysutil::nameOf(e.path()), e.path());
        }
    }
#endif
    return out;
}

bool launch(const fs::path& p, std::string& error) {
    std::error_code ec;
    if (!fs::exists(p, ec)) {
        error = "not found: " + sysutil::fromPath(p);
        return false;
    }
#ifdef _WIN32
    auto r = reinterpret_cast<INT_PTR>(ShellExecuteW(nullptr, L"open", p.c_str(), nullptr, nullptr, SW_SHOWNORMAL));
    if (r <= 32) {
        error = "Windows has no app set for this kind of file";
        return false;
    }
    return true;
#else
#ifdef __APPLE__
    const char* opener = "open";
#else
    const char* opener = "xdg-open";
#endif
    const std::string path = sysutil::fromPath(p);
    pid_t pid = fork();
    if (pid < 0) {
        error = "couldn't start " + std::string(opener);
        return false;
    }
    if (pid == 0) {
        setsid();
        if (fork() == 0) {
            execlp(opener, opener, path.c_str(), static_cast<char*>(nullptr));
            _exit(127);
        }
        _exit(0);
    }
    int status = 0;
    waitpid(pid, &status, 0);
    return true;
#endif
}

// ---- copying and moving ----

fs::path freeName(const fs::path& wanted) {
    std::error_code ec;
    if (!fs::exists(wanted, ec)) return wanted;
    const std::string stem = sysutil::fromPath(wanted.stem()), ext = sysutil::fromPath(wanted.extension());
    for (int i = 2; i < 10000; ++i) {
        fs::path c = wanted.parent_path() / sysutil::toPath(stem + " (" + std::to_string(i) + ")" + ext);
        if (!fs::exists(c, ec)) return c;
    }
    throw std::runtime_error("no free name for " + sysutil::fromPath(wanted));
}

void copyAny(const fs::path& from, const fs::path& to, bool overwrite) {
    std::error_code ec;
    if (!fs::exists(from, ec)) throw std::runtime_error("not found: " + sysutil::fromPath(from));
    if (fs::exists(to, ec) && !overwrite) throw std::runtime_error(sysutil::fromPath(to) + " already exists");
    const fs::path a = fs::weakly_canonical(from, ec), b = fs::weakly_canonical(to, ec);
    auto rel = b.lexically_relative(a);
    if (!rel.empty() && *rel.begin() != "..") throw std::runtime_error("can't copy a folder into itself");
    auto opts = fs::copy_options::recursive | fs::copy_options::copy_symlinks;
    if (overwrite) opts |= fs::copy_options::overwrite_existing;
    fs::copy(from, to, opts, ec);
    if (ec) throw std::runtime_error("couldn't copy " + sysutil::fromPath(from) + ": " + ec.message());
}

void moveAny(const fs::path& from, const fs::path& to) {
    std::error_code ec;
    if (!fs::exists(from, ec) && !fs::is_symlink(from, ec)) throw std::runtime_error("not found: " + sysutil::fromPath(from));
    if (fs::exists(to, ec)) throw std::runtime_error(sysutil::fromPath(to) + " already exists");
    fs::rename(from, to, ec);
    if (!ec) return;
    // another drive: copy, then remove the original
    copyAny(from, to, false);
    fs::remove_all(from, ec);
    if (ec) throw std::runtime_error("copied, but couldn't remove " + sysutil::fromPath(from) + ": " + ec.message());
}

std::uint64_t sizeOf(const fs::path& p, std::uint64_t* files) {
    std::error_code ec;
    if (!fs::is_directory(p, ec)) {
        if (files) *files = 1;
        auto s = fs::file_size(p, ec);
        return ec ? 0 : s;
    }
    std::uint64_t total = 0, n = 0;
    for (fs::recursive_directory_iterator it(p, fs::directory_options::skip_permission_denied, ec), end; it != end; it.increment(ec)) {
        if (ec) break;
        std::error_code e2;
        if (it->is_regular_file(e2)) {
            total += it->file_size(e2);
            ++n;
        }
    }
    if (files) *files = n;
    return total;
}

// ---- trash ----

fs::path trashFolder() { return sysutil::dataDir() / "trash"; }

namespace {

fs::path newTrashSlot(std::string& id) {
    std::error_code ec;
    static std::atomic<int> counter{0};
    const std::int64_t now = static_cast<std::int64_t>(std::time(nullptr));
    fs::path slot;
    do {
        char num[16];
        std::snprintf(num, sizeof num, "%05d", ++counter % 100000);
        id = std::to_string(now) + "-" + num;
        slot = trashFolder() / id;
    } while (fs::exists(slot, ec));
    fs::create_directories(slot, ec);
    if (ec) throw std::runtime_error("can't make the trash folder " + sysutil::fromPath(slot));
    return slot;
}

std::map<std::string, std::string> trashInfo(const fs::path& slot) {
    std::map<std::string, std::string> out;
    for (const auto& line : sysutil::splitLines(readSmall(slot / "info.txt"))) {
        auto eq = line.find('=');
        if (eq != std::string::npos) out[line.substr(0, eq)] = line.substr(eq + 1);
    }
    return out;
}

}  // namespace

std::string moveToTrash(const fs::path& p) {
    std::error_code ec;
    if (!fs::exists(p, ec) && !fs::is_symlink(p, ec)) throw std::runtime_error("not found: " + sysutil::fromPath(p));
    const fs::path abs = fs::absolute(p, ec).lexically_normal();
    std::string id;
    const fs::path slot = newTrashSlot(id);
    const std::int64_t now = static_cast<std::int64_t>(std::time(nullptr));
    std::uint64_t files = 0;
    const std::uint64_t size = sizeOf(abs, &files);
    const bool dir = fs::is_directory(abs, ec);
    std::string name = sysutil::nameOf(abs);
    if (name.empty()) throw std::runtime_error("can't delete a whole drive");
    try {
        moveAny(abs, slot / sysutil::toPath(name));
    } catch (...) {
        fs::remove_all(slot, ec);
        throw;
    }
    sysutil::writeFile(slot / "info.txt", "original=" + sysutil::fromPath(abs) + "\ndeleted=" + std::to_string(now) + "\nsize=" + std::to_string(size) +
                                              "\ndir=" + (dir ? "1" : "0") + "\nname=" + name + "\n");
    return id;
}

std::string moveToSystemTrash(const fs::path& p, bool* usedSystem) {
    if (usedSystem) *usedSystem = false;
    std::error_code ec;
    if (!fs::exists(p, ec) && !fs::is_symlink(p, ec)) throw std::runtime_error("not found: " + sysutil::fromPath(p));
    const fs::path abs = fs::absolute(p, ec).lexically_normal();
    if (!systemTrashWanted()) return moveToTrash(abs);
    const std::string name = sysutil::nameOf(abs);
    if (name.empty()) throw std::runtime_error("can't delete a whole drive");
    std::uint64_t files = 0;
    const std::uint64_t size = sizeOf(abs, &files);
    const bool dir = fs::is_directory(abs, ec);
    SystemTrashed where;
    try {
        where = moveToSystemTrashRaw(abs);
    } catch (const std::exception&) {
        return moveToTrash(abs);  // another drive, no desktop trash: the suite's own trash still has undo
    }
    std::string id;
    const fs::path slot = newTrashSlot(id);
    const std::int64_t now = static_cast<std::int64_t>(std::time(nullptr));
    sysutil::writeFile(slot / "info.txt", "original=" + sysutil::fromPath(abs) + "\ndeleted=" + std::to_string(now) + "\nsize=" + std::to_string(size) +
                                              "\ndir=" + (dir ? "1" : "0") + "\nname=" + name + "\nsystem=" +
                                              (where.item.empty() ? std::string("?") : sysutil::fromPath(where.item)) + "\nrecord=" + sysutil::fromPath(where.record) + "\n");
    if (usedSystem) *usedSystem = true;
    return id;
}

std::vector<TrashItem> trashItems() {
    std::vector<TrashItem> out;
    std::error_code ec;
    for (const auto& e : fs::directory_iterator(trashFolder(), ec)) {
        TrashItem t;
        t.id = sysutil::nameOf(e.path());
        const auto info = trashInfo(e.path());
        auto get = [&](const char* k) {
            auto it = info.find(k);
            return it == info.end() ? std::string() : it->second;
        };
        t.original = get("original");
        t.deleted = std::atoll(get("deleted").c_str());
        t.size = std::strtoull(get("size").c_str(), nullptr, 10);
        t.dir = get("dir") == "1";
        t.system = get("system");
        if (t.original.empty()) continue;
        std::error_code e2;
        if (!t.system.empty() && t.system != "?" && !fs::exists(sysutil::toPath(t.system), e2) && !fs::is_symlink(sysutil::toPath(t.system), e2)) {
            fs::remove_all(e.path(), e2);  // emptied from the computer's trash (or put back) by someone else
            continue;
        }
        out.push_back(t);
    }
    std::sort(out.begin(), out.end(), [](const TrashItem& a, const TrashItem& b) { return a.deleted > b.deleted || (a.deleted == b.deleted && a.id > b.id); });
    return out;
}

fs::path restoreFromTrash(const std::string& id, const fs::path& to) {
    if (id.empty() || id.find('/') != std::string::npos || id.find('\\') != std::string::npos || id.find("..") != std::string::npos)
        throw std::runtime_error("bad trash id '" + id + "'");
    const fs::path slot = trashFolder() / id;
    const auto info = trashInfo(slot);
    auto get = [&](const char* k) {
        auto it = info.find(k);
        return it == info.end() ? std::string() : it->second;
    };
    const std::string original = get("original"), name = get("name"), system = get("system");
    if (original.empty()) throw std::runtime_error("nothing in the trash called '" + id + "' (explorer.trash lists it)");
    if (system == "?") throw std::runtime_error(original + " is in the Recycle Bin: put it back from there (right-click, Restore)");
    fs::path target = to.empty() ? sysutil::toPath(original) : to;
    std::error_code ec;
    if (fs::exists(target, ec)) throw std::runtime_error(sysutil::fromPath(target) + " exists again; restore to another place with to=");
    fs::create_directories(target.parent_path(), ec);
    if (!system.empty()) {
        const fs::path item = sysutil::toPath(system);
        if (!fs::exists(item, ec) && !fs::is_symlink(item, ec)) {
            fs::remove_all(slot, ec);
            throw std::runtime_error(original + " is gone from the computer's trash (it was emptied, or put back already)");
        }
        moveAny(item, target);
        if (!get("record").empty()) fs::remove(sysutil::toPath(get("record")), ec);
    } else {
        moveAny(slot / sysutil::toPath(name), target);
    }
    fs::remove_all(slot, ec);
    return target;
}

std::size_t emptyTrash(double olderThanDays) {
    std::size_t n = 0;
    const double now = static_cast<double>(std::time(nullptr));
    std::error_code ec;
    for (const auto& t : trashItems()) {
        if (olderThanDays > 0 && now - static_cast<double>(t.deleted) < olderThanDays * 86400) continue;
        if (!t.system.empty() && t.system != "?") {
            // deleted in the suite, so emptying the suite's trash takes it out of the computer's too
            const auto info = trashInfo(trashFolder() / t.id);
            fs::remove_all(sysutil::toPath(t.system), ec);
            if (auto it = info.find("record"); it != info.end() && !it->second.empty()) fs::remove(sysutil::toPath(it->second), ec);
        }
        fs::remove_all(trashFolder() / t.id, ec);
        if (!ec) ++n;
    }
    return n;
}

}  // namespace wetwaresuite::explorer
