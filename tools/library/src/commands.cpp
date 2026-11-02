// library.* commands. Each loads the catalog (cached while the file on disk
// is unchanged), does its work and writes the catalog back when it changed.
// Tag, rate, label, note and forget are undoable: they remember what they
// replaced and put it back.
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <fstream>
#include <functional>
#include <map>
#include <mutex>
#include <numeric>
#include <set>
#include <sstream>
#include <thread>

#include <nlohmann/json.hpp>

#include "wetwaresuite/library/library.hpp"
#include "wetwaresuite/raster/codec.hpp"

namespace wetwaresuite::library {

using nlohmann::json;

namespace {

// ---- shared state -------------------------------------------------------------

struct State {
    std::string current;  // the catalog file the last command used
    struct Cached {
        Catalog catalog;
        fs::file_time_type time{};
        std::uintmax_t size = 0;
    };
    std::map<std::string, Cached> cache;

    Catalog& open(const fs::path& file) {
        const std::string key = normPath(file);
        std::error_code ec;
        auto t = fs::last_write_time(file, ec);
        if (ec) t = {};
        std::uintmax_t sz = fs::file_size(file, ec);
        if (ec) sz = 0;
        auto it = cache.find(key);
        if (it == cache.end() || it->second.time != t || it->second.size != sz) {
            Cached c{Catalog::load(file), t, sz};
            c.catalog.file = file;
            it = cache.insert_or_assign(key, std::move(c)).first;
        }
        current = key;
        return it->second.catalog;
    }

    void commit(Catalog& catalog) {
        catalog.save();
        const std::string key = normPath(catalog.file);
        auto it = cache.find(key);
        if (it == cache.end()) return;
        std::error_code ec;
        it->second.time = fs::last_write_time(catalog.file, ec);
        it->second.size = fs::file_size(catalog.file, ec);
    }
};

using StatePtr = std::shared_ptr<State>;

std::string isoTime(std::int64_t t) {
    std::int64_t days = t >= 0 ? t / 86400 : (t - 86399) / 86400;
    std::int64_t secs = t - days * 86400;
    // civil from days (Howard Hinnant)
    days += 719468;
    const std::int64_t era = (days >= 0 ? days : days - 146096) / 146097;
    const auto doe = static_cast<unsigned>(days - era * 146097);
    const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    const std::int64_t y = static_cast<std::int64_t>(yoe) + era * 400;
    const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const unsigned mp = (5 * doy + 2) / 153;
    const unsigned d = doy - (153 * mp + 2) / 5 + 1;
    const unsigned m = mp < 10 ? mp + 3 : mp - 9;
    char buf[64];
    std::snprintf(buf, sizeof buf, "%04d-%02u-%02u %02d:%02d", static_cast<int>(y + (m <= 2)), m, d,
                  static_cast<int>(secs / 3600), static_cast<int>(secs / 60 % 60));
    return buf;
}

std::vector<std::string> splitOn(const std::string& s, const std::string& seps) {
    std::vector<std::string> out;
    std::string cur;
    for (char c : s) {
        if (seps.find(c) != std::string::npos) {
            auto a = cur.find_first_not_of(" \t\r"), b = cur.find_last_not_of(" \t\r");
            if (a != std::string::npos) out.push_back(cur.substr(a, b - a + 1));
            cur.clear();
        } else {
            cur += c;
        }
    }
    auto a = cur.find_first_not_of(" \t\r"), b = cur.find_last_not_of(" \t\r");
    if (a != std::string::npos) out.push_back(cur.substr(a, b - a + 1));
    return out;
}

bool globMatch(const std::string& pattern, const std::string& text) {
    std::size_t p = 0, t = 0, star = std::string::npos, resume = 0;
    auto eq = [](char a, char b) { return std::tolower(static_cast<unsigned char>(a)) == std::tolower(static_cast<unsigned char>(b)); };
    while (t < text.size()) {
        if (p < pattern.size() && (pattern[p] == '?' || eq(pattern[p], text[t]))) {
            ++p;
            ++t;
        } else if (p < pattern.size() && pattern[p] == '*') {
            star = p++;
            resume = t;
        } else if (star != std::string::npos) {
            p = star + 1;
            t = ++resume;
        } else {
            return false;
        }
    }
    while (p < pattern.size() && pattern[p] == '*') ++p;
    return p == pattern.size();
}

// Runs fn(i) for i in [0, n) on several threads.
void parallelFor(std::size_t n, std::int64_t threads, const std::function<void(std::size_t)>& fn) {
    std::size_t hw = std::max(1u, std::thread::hardware_concurrency());
    std::size_t count = threads > 0 ? static_cast<std::size_t>(threads) : std::min<std::size_t>(hw, 8);
    count = std::min(count, n);
    if (count <= 1) {
        for (std::size_t i = 0; i < n; ++i) fn(i);
        return;
    }
    std::atomic<std::size_t> next{0};
    std::vector<std::thread> pool;
    for (std::size_t t = 0; t < count; ++t) {
        pool.emplace_back([&] {
            for (std::size_t i = next++; i < n; i = next++) fn(i);
        });
    }
    for (auto& th : pool) th.join();
}

// ---- params -----------------------------------------------------------------

ParamSpec libraryParam() {
    return {"library", ParamType::String,
            "catalog file or the folder holding it (default: the one used last, else " + std::string(kCatalogName) +
                " in this folder or a parent)",
            false};
}
ParamSpec queryParam(const std::string& what) {
    return {"query", ParamType::String,
            what + ": a search such as 'type:image tag:beach rating>=3' (see library.search)", false};
}
ParamSpec pathsParam() {
    return {"paths", ParamType::String, "files to act on: one per line, or separated by commas", false};
}
ParamSpec jsonParam(const std::string& what) {
    return {"json", ParamType::Bool, "also return " + what + " as JSON text, for scripts (the Python client turns it on)", false, {}, false};
}
ParamSpec collectionParam() { return {"collection", ParamType::String, "act on the files in this collection", false}; }
ParamSpec previewParam(const std::string& what) {
    return {"preview", ParamType::Bool, what, false, {}, false};
}

// Finds the catalog file a command means.
std::optional<fs::path> catalogFile(const State& st, const Params& p, std::string& error) {
    std::string lib = stringParam(p, "library");
    std::error_code ec;
    if (!lib.empty()) {
        fs::path f(lib);
        if (fs::is_directory(f, ec) || lib.back() == '/' || lib.back() == '\\') return catalogIn(f);
        return f;
    }
    if (!st.current.empty()) return fs::path(st.current);
    for (fs::path dir = fs::current_path(ec); !dir.empty(); dir = dir.parent_path()) {
        const fs::path found = catalogIn(dir);
        if (fs::exists(found, ec)) return found;
        if (dir == dir.parent_path()) break;
    }
    error = "no library found: run library.index root=<folder> first, or pass library=<catalog file or folder>";
    return std::nullopt;
}

// Opens the catalog a command means; a missing file is an error unless allowNew.
Catalog* openCatalog(State& st, const Params& p, std::string& error, bool allowNew = false) {
    auto file = catalogFile(st, p, error);
    if (!file) return nullptr;
    std::error_code ec;
    if (!allowNew && !fs::exists(*file, ec)) {
        error = "no library at " + file->string() + ": run library.index root=<folder> first";
        return nullptr;
    }
    try {
        return &st.open(*file);
    } catch (const std::exception& e) {
        error = e.what();
        return nullptr;
    }
}

std::vector<std::string> pathList(const std::string& text) {
    std::vector<std::string> out;
    for (const auto& line : splitOn(text, "\n")) {
        std::error_code ec;
        if (line.find(',') != std::string::npos && !fs::exists(line, ec)) {
            for (const auto& part : splitOn(line, ",")) out.push_back(part);
        } else {
            out.push_back(line);
        }
    }
    return out;
}

// The files a command acts on: paths=, else query= and/or collection=. With none given, every file
// when all is true, else an error. Paths not in the catalog yet are indexed when addMissing.
std::vector<Item*> selectItems(Catalog& cat, const Params& p, std::string& error, bool all, bool addMissing = false) {
    std::vector<Item*> out;
    const bool hasPaths = p.count("paths") && !stringParam(p, "paths").empty();
    const bool hasQuery = p.count("query") > 0;
    const bool hasCollection = p.count("collection") && !stringParam(p, "collection").empty();
    if (hasPaths && (hasQuery || hasCollection)) {
        error = "give paths= or query=/collection=, not both";
        return {};
    }
    if (hasPaths) {
        bool added = false;
        for (const auto& raw : pathList(stringParam(p, "paths"))) {
            std::string path = normPath(raw);
            Item* item = cat.find(path);
            std::error_code ec;
            if (!item && addMissing && fs::is_regular_file(path, ec)) {
                try {
                    cat.items.push_back(readItem(path));
                    cat.rebuildIndex();
                    added = true;
                } catch (const std::exception& e) {
                    error = e.what();
                    return {};
                }
            }
            if (!item && !cat.find(path)) {
                error = raw + " is not in the library" + (fs::exists(path, ec) ? " (run library.index on its folder)" : " (no such file)");
                return {};
            }
            out.push_back(cat.find(path));
        }
        if (added) {  // pointers may have moved when items grew
            out.clear();
            for (const auto& raw : pathList(stringParam(p, "paths"))) out.push_back(cat.find(normPath(raw)));
        }
        return out;
    }
    if (!hasQuery && !hasCollection && !all) {
        error = "say which files: query=<search>, paths=<files> or collection=<name> (query=\"\" means every file)";
        return {};
    }
    std::string q = stringParam(p, "query");
    if (hasCollection) q += " \"collection:" + stringParam(p, "collection") + "\"";
    try {
        auto r = search(cat, q, {"", -1, 0});
        for (const Item* i : r.items) out.push_back(const_cast<Item*>(i));
    } catch (const ParamError& e) {
        error = e.what();
    }
    return out;
}

// ---- output -----------------------------------------------------------------

std::string dims(const Item& i) {
    std::ostringstream o;
    if (i.width && i.height) o << i.width << "x" << i.height;
    if (i.duration > 0) {
        if (o.tellp() > 0) o << " ";
        int s = static_cast<int>(std::lround(i.duration));
        char buf[24];
        std::snprintf(buf, sizeof buf, "%d:%02d", s / 60, s % 60);
        o << buf;
    }
    if (i.pages) o << (o.tellp() > 0 ? " " : "") << i.pages << (i.pages == 1 ? " page" : " pages");
    if (i.triangles) o << (o.tellp() > 0 ? " " : "") << i.triangles << " tris";
    return o.str();
}

std::string line(const Item& i) {
    std::string out = i.path + "  [" + (i.type.empty() ? "?" : i.type) + "] " + formatSize(i.size);
    std::string d = dims(i);
    if (!d.empty()) out += " " + d;
    if (i.rating) out += " " + std::string(static_cast<std::size_t>(i.rating), '*');
    if (!i.label.empty()) out += " (" + i.label + ")";
    if (!i.tags.empty()) {
        out += " #";
        for (std::size_t t = 0; t < i.tags.size(); ++t) out += (t ? " #" : "") + i.tags[t];
    }
    return out;
}

json itemJson(const Item& i) {
    json j = {{"path", i.path}, {"name", i.name()}, {"type", i.type}, {"format", i.format}, {"ext", i.ext},
              {"size", i.size}, {"modified", isoTime(i.mtime)}, {"hash", i.hash}};
    if (i.width) j["width"] = i.width;
    if (i.height) j["height"] = i.height;
    if (i.duration > 0) j["duration"] = i.duration;
    if (i.pages) j["pages"] = i.pages;
    if (i.triangles) j["triangles"] = i.triangles;
    if (!i.bounds.empty()) j["bounds"] = i.bounds;
    j["tags"] = i.tags;
    j["rating"] = i.rating;
    if (!i.label.empty()) j["label"] = i.label;
    if (!i.notes.empty()) j["notes"] = i.notes;
    return j;
}

template <typename Items>
std::string itemsJson(const Items& items) {
    json arr = json::array();
    for (const auto* i : items) arr.push_back(itemJson(*i));
    return arr.dump(-1, ' ', false, json::error_handler_t::replace);
}

template <typename Items>
std::string joinPaths(const Items& items) {
    std::string out;
    for (const auto* i : items) out += (out.empty() ? "" : "\n") + i->path;
    return out;
}

// The cached thumbnail, made (and cached when `write`) if needed. Empty when there is none.
raster::Image thumbnail(const Catalog& cat, const Item& item, int size, bool write, bool force = false, std::string* why = nullptr,
                        bool* made = nullptr) {
    const fs::path cached = thumbPath(cat, item, size);
    std::error_code ec;
    if (!force && fs::exists(cached, ec)) {
        try {
            return raster::load(cached);
        } catch (const std::exception&) {
        }
    }
    raster::Image img = renderThumbnail(item.path, item.type, item.ext, size, why);
    if (!img.empty() && write) {
        fs::create_directories(cached.parent_path(), ec);
        try {
            raster::save(img, cached);
        } catch (const std::exception&) {
        }
    }
    if (made) *made = !img.empty();
    return img;
}

Result& attachSheet(Result& r, const Catalog& cat, const std::vector<const Item*>& items, int size, int columns, bool write,
                    std::size_t max = 60) {
    std::vector<raster::Image> thumbs;
    for (std::size_t i = 0; i < items.size() && i < max; ++i) thumbs.push_back(thumbnail(cat, *items[i], size, write));
    if (thumbs.empty()) return r;
    auto sheet = contactSheet(thumbs, std::min(size, 256), columns);
    r.attach({"image/png", raster::encodePng(sheet), "contact-sheet.png"});
    return r;
}

// ---- index ------------------------------------------------------------------

struct IndexOptions {
    bool recursive = true;
    bool incremental = true;
    bool hidden = false;
    bool probe = true;
    bool prune = true;
    bool fullHash = false;
    std::int64_t threads = 0;
    std::vector<std::string> include, exclude;
};

IndexOptions indexOptions(const Params& p) {
    IndexOptions o;
    o.recursive = boolParam(p, "recursive", true);
    o.incremental = boolParam(p, "incremental", true);
    o.hidden = boolParam(p, "hidden", false);
    o.probe = boolParam(p, "probe", true);
    o.prune = boolParam(p, "prune", true);
    o.fullHash = boolParam(p, "full_hash", false);
    o.threads = intParam(p, "threads", 0);
    o.include = splitOn(stringParam(p, "include"), ",;");
    o.exclude = splitOn(stringParam(p, "exclude"), ",;");
    return o;
}

std::vector<ParamSpec> indexParams() {
    return {{"recursive", ParamType::Bool, "include subfolders", false, {}, true},
            {"incremental", ParamType::Bool, "skip files whose size and modification time haven't changed", false, {}, true},
            {"include", ParamType::String, "only files matching these wildcards, e.g. *.png,*.jpg", false},
            {"exclude", ParamType::String, "skip files and folders matching these wildcards, e.g. cache,*.tmp", false},
            {"hidden", ParamType::Bool, "include hidden files and folders (names starting with a dot)", false, {}, false},
            {"probe", ParamType::Bool, "measure durations (ffprobe), page counts and mesh bounds; false is faster", false, {}, true},
            {"prune", ParamType::Bool, "drop entries for files that no longer exist (tags follow files that moved)", false, {}, true},
            {"full_hash", ParamType::Bool, "hash every byte of large files instead of their first and last MiB", false, {}, false},
            {"threads", ParamType::Int, "worker threads (0 = automatic)", false, {}, std::int64_t{0}}};
}

struct IndexStats {
    std::size_t files = 0, added = 0, updated = 0, unchanged = 0, removed = 0, moved = 0;
    std::vector<std::string> errors;
};

// Merges tags found inside a file into its catalog entry. `replace` takes the file's values instead.
bool mergeFileTags(Item& item, const FileTags& t, bool replace) {
    bool changed = false;
    if (replace) {
        changed = item.tags != t.tags || item.rating != t.rating || item.label != t.label || item.notes != t.notes;
        item.tags = t.tags;
        item.rating = t.rating;
        item.label = t.label;
        item.notes = t.notes;
        return changed;
    }
    for (const auto& tag : t.tags) {
        if (!item.hasTag(tag)) {
            item.tags.push_back(tag);
            changed = true;
        }
    }
    if (!item.rating && t.rating) item.rating = t.rating, changed = true;
    if (item.label.empty() && !t.label.empty()) item.label = t.label, changed = true;
    if (item.notes.empty() && !t.notes.empty()) item.notes = t.notes, changed = true;
    return changed;
}

bool skipName(const std::string& name, const IndexOptions& o) {
    if (name == kCatalogName || name == kThumbDir || name.rfind(std::string(kCatalogName), 0) == 0) return true;
    if (name == kOldThumbDir || name.rfind(std::string(kOldCatalogName), 0) == 0) return true;
    if (!o.hidden && !name.empty() && name[0] == '.') return true;
    if (name.size() > 4 && lower(name.substr(name.size() - 4)) == ".xmp") return true;  // sidecars: their tags are read with the file
    if (name.size() > 6 && name.compare(name.size() - 6, 6, ".wwtmp") == 0) return true;
    for (const auto& x : o.exclude) {
        if (globMatch(x, name)) return true;
    }
    return false;
}

void indexRoot(Catalog& cat, const fs::path& rootPath, const IndexOptions& o, IndexStats& stats) {
    const std::string root = normPath(rootPath);
    std::vector<fs::path> files;
    std::error_code ec;
    auto consider = [&](const fs::directory_entry& e) {
        const std::string name = e.path().filename().string();
        std::error_code fec;
        if (!e.is_regular_file(fec) || skipName(name, o)) return;
        if (!o.include.empty()) {
            bool ok = false;
            for (const auto& inc : o.include) ok = ok || globMatch(inc, name);
            if (!ok) return;
        }
        files.push_back(e.path());
    };
    if (o.recursive) {
        fs::recursive_directory_iterator it(rootPath, fs::directory_options::skip_permission_denied, ec), end;
        while (!ec && it != end) {
            const auto& e = *it;
            std::error_code dec;
            if (e.is_directory(dec) && skipName(e.path().filename().string(), o)) {
                it.disable_recursion_pending();
            } else {
                consider(e);
            }
            it.increment(ec);
            if (ec) {  // unreadable entry: skip it and carry on
                stats.errors.push_back(ec.message());
                ec.clear();
            }
        }
    } else {
        for (const auto& e : fs::directory_iterator(rootPath, fs::directory_options::skip_permission_denied, ec)) consider(e);
    }

    std::set<std::string> seen;
    std::vector<fs::path> work;
    for (const auto& f : files) {
        const std::string key = normPath(f);
        seen.insert(key);
        const Item* existing = cat.find(key);
        if (o.incremental && existing) {
            std::error_code fec;
            auto size = fs::file_size(f, fec);
            auto tick = static_cast<std::int64_t>(fs::last_write_time(f, fec).time_since_epoch().count());
            if (!fec && size == existing->size && tick == existing->mtick) {
                ++stats.unchanged;
                continue;
            }
        }
        work.push_back(f);
    }
    stats.files += files.size();

    std::vector<std::optional<Item>> read(work.size());
    std::vector<std::optional<FileTags>> inside(work.size());
    std::vector<char> isNew(work.size());
    for (std::size_t i = 0; i < work.size(); ++i) isNew[i] = cat.find(normPath(work[i])) == nullptr;
    std::vector<std::string> errors(work.size());
    parallelFor(work.size(), o.threads, [&](std::size_t i) {
        try {
            read[i] = readItem(work[i], o.probe, o.fullHash);
            if (isNew[i]) inside[i] = readFileTags(work[i]);  // tags that came with a new file
        } catch (const std::exception& e) {
            errors[i] = e.what();
        }
    });

    std::set<std::string> addedPaths;
    for (std::size_t i = 0; i < work.size(); ++i) {
        if (!read[i]) {
            stats.errors.push_back(errors[i]);
            continue;
        }
        Item& fresh = *read[i];
        if (Item* old = cat.find(fresh.path)) {
            fresh.tags = old->tags;
            fresh.rating = old->rating;
            fresh.label = old->label;
            fresh.notes = old->notes;
            if (old->hash == fresh.hash) {
                fresh.dhash = old->dhash;
                fresh.phash = old->phash;
            }
            *old = std::move(fresh);
            ++stats.updated;
        } else {
            if (inside[i]) mergeFileTags(fresh, *inside[i], false);
            addedPaths.insert(fresh.path);
            cat.items.push_back(std::move(fresh));
            ++stats.added;
        }
    }

    if (o.prune) {
        auto under = [&](const std::string& path) {
            if (path.size() <= root.size() || path.compare(0, root.size(), root) != 0 || path[root.size()] != '/') return false;
            return o.recursive || path.find('/', root.size() + 1) == std::string::npos;
        };
        std::vector<Item> gone;
        std::vector<Item> keep;
        keep.reserve(cat.items.size());
        for (auto& it : cat.items) {
            std::error_code fec;
            if (under(it.path) && !seen.count(it.path) && !fs::exists(it.path, fec)) gone.push_back(std::move(it));
            else keep.push_back(std::move(it));
        }
        if (!gone.empty()) {
            // a vanished file whose content turned up as a new file has moved: keep its tags
            std::map<std::string, std::string> movedTo;
            for (auto& g : gone) {
                for (auto& k : keep) {
                    if (k.hash == g.hash && k.size == g.size && addedPaths.erase(k.path)) {
                        k.tags = g.tags;
                        k.rating = g.rating;
                        k.label = g.label;
                        k.notes = g.notes;
                        movedTo[g.path] = k.path;
                        ++stats.moved;
                        break;
                    }
                }
            }
            for (auto& [name, col] : cat.collections) {
                for (auto& path : col.paths) {
                    if (movedTo.count(path)) path = movedTo[path];
                }
            }
            stats.removed += gone.size() - movedTo.size();
            stats.added -= std::min(stats.added, movedTo.size());
        }
        cat.items = std::move(keep);
    }
    cat.rebuildIndex();
    if (std::find(cat.roots.begin(), cat.roots.end(), root) == cat.roots.end()) cat.roots.push_back(root);
}

Result indexResult(const std::string& what, const IndexStats& s, double seconds, const Catalog& cat) {
    std::ostringstream msg;
    msg << "indexed " << what << ": " << s.files << " files (" << s.added << " new, " << s.updated << " updated, " << s.unchanged
        << " unchanged, " << s.removed << " removed";
    if (s.moved) msg << ", " << s.moved << " moved";
    msg << ") in " << std::fixed;
    msg.precision(2);
    msg << seconds << " s; library has " << cat.items.size() << " files";
    for (std::size_t i = 0; i < s.errors.size() && i < 5; ++i) msg << "\n  skipped: " << s.errors[i];
    Result r = Result::success(msg.str());
    r.with("library", normPath(cat.file))
        .with("files", static_cast<std::int64_t>(s.files))
        .with("added", static_cast<std::int64_t>(s.added))
        .with("updated", static_cast<std::int64_t>(s.updated))
        .with("unchanged", static_cast<std::int64_t>(s.unchanged))
        .with("removed", static_cast<std::int64_t>(s.removed))
        .with("moved", static_cast<std::int64_t>(s.moved))
        .with("errors", static_cast<std::int64_t>(s.errors.size()))
        .with("total", static_cast<std::int64_t>(cat.items.size()));
    return r;
}

// ---- undoable metadata edits ------------------------------------------------------

struct Meta {
    std::vector<std::string> tags;
    int rating = 0;
    std::string label, notes;
};

class MetaEdit : public Action {
public:
    MetaEdit(StatePtr st, Params p, std::string verb, std::function<std::string(const Params&)> check,
             std::function<bool(Item&, const Params&)> change)
        : st_(std::move(st)), p_(std::move(p)), verb_(std::move(verb)), check_(std::move(check)), change_(std::move(change)) {}

    Result apply() override {
        if (auto err = check_(p_); !err.empty()) return Result::failure(err);
        std::string error;
        Catalog* cat = openCatalog(*st_, p_, error);
        if (!cat) return Result::failure(error);
        file_ = cat->file;
        if (paths_.empty()) {
            auto items = selectItems(*cat, p_, error, false, true);
            if (!error.empty()) return Result::failure(error);
            for (auto* i : items) paths_.push_back(i->path);
        }
        before_.clear();
        std::size_t changed = 0;
        for (const auto& path : paths_) {
            Item* item = cat->find(path);
            if (!item) continue;
            before_.push_back({path, {item->tags, item->rating, item->label, item->notes}});
            if (change_(*item, p_)) ++changed;
        }
        try {
            st_->commit(*cat);
        } catch (const std::exception& e) {
            return Result::failure(e.what());
        }
        std::string msg = verb_ + " " + std::to_string(changed) + " of " + std::to_string(paths_.size()) + " files";
        for (std::size_t i = 0; i < paths_.size() && i < 20; ++i) msg += "\n  " + line(*cat->find(paths_[i]));
        if (paths_.size() > 20) msg += "\n  ... and " + std::to_string(paths_.size() - 20) + " more";
        Result r = Result::success(msg);
        return r.with("changed", static_cast<std::int64_t>(changed)).with("files", static_cast<std::int64_t>(paths_.size()));
    }

    void revert() override {
        try {
            Catalog& cat = st_->open(file_);
            for (const auto& [path, m] : before_) {
                if (Item* item = cat.find(path)) {
                    item->tags = m.tags;
                    item->rating = m.rating;
                    item->label = m.label;
                    item->notes = m.notes;
                }
            }
            st_->commit(cat);
        } catch (const std::exception&) {
        }
    }

private:
    StatePtr st_;
    Params p_;
    std::string verb_;
    std::function<std::string(const Params&)> check_;
    std::function<bool(Item&, const Params&)> change_;
    fs::path file_;
    std::vector<std::string> paths_;
    std::vector<std::pair<std::string, Meta>> before_;
};

class TagWrite : public Action {
public:
    TagWrite(StatePtr st, Params p) : st_(std::move(st)), p_(std::move(p)) {}

    Result apply() override {
        std::string error;
        Catalog* cat = openCatalog(*st_, p_, error);
        if (!cat) return Result::failure(error);
        file_ = cat->file;
        const std::string where = stringParam(p_, "where", "auto");
        if (paths_.empty()) {
            auto items = selectItems(*cat, p_, error, false, true);
            if (!error.empty()) return Result::failure(error);
            for (auto* i : items) paths_.push_back(i->path);
        }
        backup_.clear();
        facts_.clear();
        std::string rows, problems;
        std::size_t done = 0;
        for (const auto& path : paths_) {
            Item* item = cat->find(path);
            if (!item) continue;
            FileTags t;
            t.tags = item->tags;
            t.rating = item->rating;
            t.label = item->label;
            t.notes = item->notes;
            try {
                const std::string went = writeFileTags(path, t, where, &backup_);
                if (went == "the file") {
                    // the photo itself changed: keep its facts current so a rescan doesn't see a new file
                    facts_.push_back({path, *item});
                    Item fresh = readItem(path, false);
                    item->size = fresh.size;
                    item->mtime = fresh.mtime;
                    item->mtick = fresh.mtick;
                    item->hash = fresh.hash;
                }
                rows += path + "\t" + went + "\n";
                ++done;
            } catch (const std::exception& e) {
                problems += "\n  " + path + ": " + e.what();
            }
        }
        if (!facts_.empty()) {
            try {
                st_->commit(*cat);
            } catch (const std::exception& e) {
                return Result::failure(e.what());
            }
        }
        if (!done && !problems.empty()) return Result::failure("couldn't save tags into the files:" + problems);
        std::string msg = "saved tags into " + std::to_string(done) + " of " + std::to_string(paths_.size()) + " files";
        if (!problems.empty()) msg += "; skipped:" + problems;
        Result r = Result::success(msg);
        return r.with("files", static_cast<std::int64_t>(done)).with("rows", rows);
    }

    void revert() override {
        for (auto it = backup_.rbegin(); it != backup_.rend(); ++it) {
            std::error_code ec;
            if (!it->bytes) {
                fs::remove(it->path, ec);
                continue;
            }
            std::ofstream out(it->path, std::ios::binary | std::ios::trunc);
            out.write(it->bytes->data(), static_cast<std::streamsize>(it->bytes->size()));
        }
        if (facts_.empty()) return;
        try {
            Catalog& cat = st_->open(file_);
            for (const auto& [path, old] : facts_) {
                if (Item* item = cat.find(path)) {
                    Item fresh = readItem(path, false);  // the restored bytes have a new file time
                    item->size = fresh.size;
                    item->mtime = fresh.mtime;
                    item->mtick = fresh.mtick;
                    item->hash = fresh.hash;
                }
            }
            st_->commit(cat);
        } catch (const std::exception&) {
        }
    }

private:
    StatePtr st_;
    Params p_;
    fs::path file_;
    std::vector<std::string> paths_;
    std::vector<FileBackup> backup_;
    std::vector<std::pair<std::string, Item>> facts_;
};

class ForgetEdit : public Action {
public:
    ForgetEdit(StatePtr st, Params p) : st_(std::move(st)), p_(std::move(p)) {}
    Result apply() override {
        std::string error;
        Catalog* cat = openCatalog(*st_, p_, error);
        if (!cat) return Result::failure(error);
        file_ = cat->file;
        auto items = selectItems(*cat, p_, error, false);
        if (!error.empty()) return Result::failure(error);
        std::set<std::string> drop;
        for (auto* i : items) drop.insert(i->path);
        removed_.clear();
        std::vector<Item> keep;
        for (auto& it : cat->items) {
            if (drop.count(it.path)) removed_.push_back(it);
            else keep.push_back(std::move(it));
        }
        cat->items = std::move(keep);
        cat->rebuildIndex();
        try {
            st_->commit(*cat);
        } catch (const std::exception& e) {
            return Result::failure(e.what());
        }
        Result r = Result::success("removed " + std::to_string(removed_.size()) + " files from the library (the files are untouched)");
        return r.with("removed", static_cast<std::int64_t>(removed_.size())).with("total", static_cast<std::int64_t>(cat->items.size()));
    }
    void revert() override {
        try {
            Catalog& cat = st_->open(file_);
            for (const auto& it : removed_) {
                if (!cat.find(it.path)) cat.items.push_back(it);
            }
            cat.rebuildIndex();
            st_->commit(cat);
        } catch (const std::exception&) {
        }
    }

private:
    StatePtr st_;
    Params p_;
    fs::path file_;
    std::vector<Item> removed_;
};

// ---- file operations ---------------------------------------------------------

bool moveFile(const fs::path& from, const fs::path& to, std::string& error) {
    std::error_code ec;
    fs::rename(from, to, ec);
    if (!ec) return true;
    // across drives: copy, then remove
    fs::copy_file(from, to, fs::copy_options::overwrite_existing, ec);
    if (ec) {
        error = ec.message();
        return false;
    }
    fs::last_write_time(to, fs::last_write_time(from, ec), ec);
    fs::remove(from, ec);
    return true;
}

std::string renderName(const std::string& tmpl, const Item& item, std::size_t n) {
    std::string out;
    const fs::path p(item.path);
    for (std::size_t i = 0; i < tmpl.size(); ++i) {
        if (tmpl[i] != '{') {
            out += tmpl[i];
            continue;
        }
        auto close = tmpl.find('}', i);
        if (close == std::string::npos) {
            out += tmpl.substr(i);
            break;
        }
        std::string key = tmpl.substr(i + 1, close - i - 1);
        i = close;
        int width = 0;
        if (auto colon = key.find(':'); colon != std::string::npos) {
            width = std::atoi(key.c_str() + colon + 1);
            key.resize(colon);
        }
        std::string v;
        if (key == "name" || key == "stem") v = p.stem().string();
        else if (key == "ext") v = p.extension().string();
        else if (key == "n") v = std::to_string(n);
        else if (key == "date") v = isoTime(item.mtime).substr(0, 10);
        else if (key == "width") v = std::to_string(item.width);
        else if (key == "height") v = std::to_string(item.height);
        else if (key == "rating") v = std::to_string(item.rating);
        else if (key == "type") v = item.type;
        else if (key == "hash") v = item.hash.substr(0, 8);
        else if (key == "label") v = item.label;
        else v = "{" + key + "}";
        while (static_cast<int>(v.size()) < width) v = "0" + v;
        out += v;
    }
    return out;
}

void renameInCollections(Catalog& cat, const std::string& from, const std::string& to) {
    for (auto& [name, col] : cat.collections) {
        for (auto& path : col.paths) {
            if (path == from) path = to;
        }
    }
}

const std::vector<std::string>& labelChoices() {
    static const std::vector<std::string> c = {"red", "orange", "yellow", "green", "blue", "purple", "gray", "none"};
    return c;
}

}  // namespace

void registerCommands(Session& session) {
    auto& reg = session.registry();
    auto st = std::make_shared<State>();

    // ---- index / rescan ----
    {
        std::vector<ParamSpec> params = {{"root", ParamType::String, "folder to index", true},
                                         {"library", ParamType::String,
                                          "catalog file to write (default: " + std::string(kCatalogName) + " inside root)", false}};
        for (auto& ip : indexParams()) params.push_back(ip);
        reg.add({"library.index", "Index folder",
                 "Scan a folder into a library catalog: path, size, modification time, content hash, detected type, pixel "
                 "size, duration, page count, mesh bounds. Incremental by default: files whose size and time haven't "
                 "changed are skipped, so re-running it is the way to pick up changes. Tags, ratings, labels and notes "
                 "survive re-indexing and follow files that were moved or renamed.",
                 params, false, false},
                [st](const Params& p) {
                    return std::make_unique<FunctionAction>([st, p] {
                        const fs::path root = stringParam(p, "root");
                        std::error_code ec;
                        if (!fs::is_directory(root, ec)) return Result::failure("no folder at " + root.string());
                        std::string lib = stringParam(p, "library");
                        fs::path file = lib.empty() ? catalogIn(root) : fs::path(lib);
                        if (!lib.empty() && (fs::is_directory(file, ec) || lib.back() == '/')) file = catalogIn(file);
                        auto t0 = std::chrono::steady_clock::now();
                        try {
                            Catalog& cat = st->open(file);
                            IndexStats stats;
                            indexRoot(cat, root, indexOptions(p), stats);
                            st->commit(cat);
                            double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
                            return indexResult(normPath(root), stats, secs, cat);
                        } catch (const std::exception& e) {
                            return Result::failure(e.what());
                        }
                    });
                });

        std::vector<ParamSpec> rparams = {libraryParam()};
        for (auto& ip : indexParams()) rparams.push_back(ip);
        reg.add({"library.rescan", "Rescan library",
                 "Re-index every folder the library already covers (incrementally), picking up new, changed, moved and "
                 "deleted files. There is no live file watching: run this (or library.index) whenever you want the "
                 "catalog up to date, e.g. from cron or automate.watch.",
                 rparams, false, false},
                [st](const Params& p) {
                    return std::make_unique<FunctionAction>([st, p] {
                        std::string error;
                        Catalog* cat = openCatalog(*st, p, error);
                        if (!cat) return Result::failure(error);
                        auto t0 = std::chrono::steady_clock::now();
                        IndexStats stats;
                        std::string missing;
                        try {
                            for (const auto& root : std::vector<std::string>(cat->roots)) {
                                std::error_code ec;
                                if (!fs::is_directory(root, ec)) {
                                    missing += "\n  root not found (drive unplugged?): " + root;
                                    continue;
                                }
                                indexRoot(*cat, root, indexOptions(p), stats);
                            }
                            st->commit(*cat);
                        } catch (const std::exception& e) {
                            return Result::failure(e.what());
                        }
                        double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
                        Result r = indexResult(std::to_string(cat->roots.size()) + (cat->roots.size() == 1 ? " folder" : " folders"), stats, secs, *cat);
                        r.message += missing;
                        return r;
                    });
                });
    }

    // ---- search ----
    reg.add({"library.search", "Search library",
             "Find files in the library. Words are ANDed: free text matches file names, tags and notes; *.png style "
             "wildcards match names; fields: type:image (image, raw, vector, cad, 3d, texture, audio, video, document, "
             "spreadsheet, slides, archive, font, ...; groups: picture, mesh), ext:png, tag:x, label:red, format:jpeg, "
             "name:, path:, in:<folder>, has:tags|notes|rating|label, orientation:landscape, collection:<name>; "
             "comparisons: rating>=3, size>10mb, width>=1920, height, duration>60 (s, m, h), pages>10, triangles, "
             "megapixels, modified<7d (within 7 days; s h d w m y) or modified>2024-01-01; prefix - to negate "
             "(-tag:used); sort:-size and limit:20 inside the query or as params. Comma lists mean any of: type:image,video.",
             {queryParam("the search (empty = everything)"),
              collectionParam(),
              libraryParam(),
              {"sort", ParamType::String, "sort field: path, name, size, modified, rating, width, height, pixels, duration, pages, type, ext; -field for descending", false},
              {"limit", ParamType::Int, "most results to return (0 = all)", false, {}, std::int64_t{100}},
              {"offset", ParamType::Int, "skip this many results first (paging)", false, {}, std::int64_t{0}},
              previewParam("attach a contact sheet of the results (first 60) as a PNG"),
              {"columns", ParamType::Int, "contact sheet columns", false, {}, std::int64_t{6}},
              jsonParam("every field of every result in `items`")},
             false, true},
            [st](const Params& p) {
                return std::make_unique<FunctionAction>([st, p] {
                    std::string error;
                    Catalog* cat = openCatalog(*st, p, error);
                    if (!cat) return Result::failure(error);
                    SearchOptions o;
                    o.sort = stringParam(p, "sort");
                    o.offset = intParam(p, "offset", 0);
                    std::string q = stringParam(p, "query");
                    if (!stringParam(p, "collection").empty()) q += " \"collection:" + stringParam(p, "collection") + "\"";
                    const bool queryLimit = lower(q).find("limit:") != std::string::npos;
                    std::int64_t limit = intParam(p, "limit", 100);
                    o.limit = p.count("limit") ? (limit <= 0 ? -1 : limit) : (queryLimit ? -1 : 100);
                    SearchResult r;
                    try {
                        r = search(*cat, q, o);
                    } catch (const ParamError& e) {
                        return Result::failure(e.what());
                    }
                    std::string msg = std::to_string(r.total) + (r.total == 1 ? " match" : " matches");
                    if (r.items.size() < r.total) msg += ", showing " + std::to_string(r.items.size());
                    for (const Item* i : r.items) msg += "\n" + line(*i);
                    Result res = Result::success(msg);
                    res.with("count", static_cast<std::int64_t>(r.items.size())).with("total", static_cast<std::int64_t>(r.total));
                    if (boolParam(p, "json")) res.with("items", itemsJson(r.items));
                    if (boolParam(p, "preview")) attachSheet(res, *cat, r.items, 160, static_cast<int>(intParam(p, "columns", 6)), false);
                    return res;
                });
            });

    // ---- info ----
    reg.add({"library.info", "File info",
             "Everything the library knows about one file: type, size, dates, dimensions, duration, pages, mesh bounds, "
             "hash, tags, rating, label, notes. A file not in the library is read on the spot (and not added).",
             {{"path", ParamType::String, "the file", true}, libraryParam(), previewParam("attach its thumbnail")},
             false, true},
            [st](const Params& p) {
                return std::make_unique<FunctionAction>([st, p] {
                    std::string error;
                    const std::string path = normPath(stringParam(p, "path"));
                    Catalog* cat = openCatalog(*st, p, error);
                    std::optional<Item> loose;
                    const Item* item = cat ? cat->find(path) : nullptr;
                    if (!item) {
                        std::error_code ec;
                        if (!fs::is_regular_file(path, ec)) return Result::failure("no file at " + path);
                        try {
                            loose = readItem(path);
                        } catch (const std::exception& e) {
                            return Result::failure(e.what());
                        }
                        item = &*loose;
                    }
                    const Item& i = *item;
                    std::ostringstream o;
                    o << i.path << (!loose ? "" : cat ? " (not in the library)" : " (no library found: pass library=)") << "\n"
                      << "type: " << i.type << " (" << i.format << ")\n"
                      << "size: " << formatSize(i.size) << " (" << i.size << " bytes)\n"
                      << "modified: " << isoTime(i.mtime);
                    if (!dims(i).empty()) o << "\ndimensions: " << dims(i);
                    if (!i.bounds.empty()) o << "\nbounds: " << i.bounds[0] << " x " << i.bounds[1] << " x " << i.bounds[2];
                    o << "\nhash: " << i.hash;
                    if (!i.tags.empty()) {
                        o << "\ntags:";
                        for (const auto& t : i.tags) o << " " << t;
                    }
                    if (i.rating) o << "\nrating: " << i.rating;
                    if (!i.label.empty()) o << "\nlabel: " << i.label;
                    if (!i.notes.empty()) o << "\nnotes: " << i.notes;
                    Result r = Result::success(o.str());
                    std::string tags;
                    for (const auto& t : i.tags) tags += (tags.empty() ? "" : ",") + t;
                    r.with("path", i.path).with("type", i.type).with("format", i.format).with("size", static_cast<std::int64_t>(i.size))
                        .with("modified", isoTime(i.mtime)).with("hash", i.hash);
                    if (i.width) r.with("width", static_cast<std::int64_t>(i.width)).with("height", static_cast<std::int64_t>(i.height));
                    if (i.duration > 0) r.with("duration", i.duration);
                    if (i.pages) r.with("pages", static_cast<std::int64_t>(i.pages));
                    if (i.triangles) r.with("triangles", i.triangles);
                    if (!i.bounds.empty()) r.with("bounds", NumberList(i.bounds));
                    r.with("tags", tags).with("rating", static_cast<std::int64_t>(i.rating)).with("label", i.label).with("notes", i.notes)
                        .with("in_library", !loose.has_value());
                    if (boolParam(p, "preview")) {
                        std::string why;
                        Catalog scratch;
                        scratch.file = cat ? cat->file : fs::path(path).parent_path() / kCatalogName;
                        raster::Image t = thumbnail(cat ? *cat : scratch, i, 256, false, false, &why);
                        if (!t.empty()) r.attach({"image/png", raster::encodePng(t), i.name() + ".png"});
                        else r.message += "\n(no preview: " + why + ")";
                    }
                    return r;
                });
            });

    // ---- metadata ----
    auto selection = [](std::vector<ParamSpec> extra) {
        std::vector<ParamSpec> out = std::move(extra);
        out.push_back(pathsParam());
        out.push_back(queryParam("or every file matching this search"));
        out.push_back(collectionParam());
        out.push_back(libraryParam());
        return out;
    };
    auto noCheck = [](const Params&) { return std::string(); };

    reg.add({"library.tag", "Tag files",
             "Add tags to files: a list of paths, every result of a search, or a collection. Files given by path that "
             "aren't indexed yet are added. Undoable.",
             selection({{"tags", ParamType::String, "tags to add, comma separated", true}}), true, false},
            [st, noCheck](const Params& p) {
                return std::make_unique<MetaEdit>(st, p, "tagged", noCheck, [](Item& item, const Params& q) {
                    bool changed = false;
                    for (const auto& t : splitOn(stringParam(q, "tags"), ",")) {
                        if (!item.hasTag(t)) {
                            item.tags.push_back(t);
                            changed = true;
                        }
                    }
                    return changed;
                });
            });

    reg.add({"library.untag", "Untag files",
             "Remove tags from files (paths, a search or a collection). tags=* removes every tag. Undoable.",
             selection({{"tags", ParamType::String, "tags to remove, comma separated, or * for all", true}}), true, false},
            [st, noCheck](const Params& p) {
                return std::make_unique<MetaEdit>(st, p, "untagged", noCheck, [](Item& item, const Params& q) {
                    const auto drop = splitOn(stringParam(q, "tags"), ",");
                    const bool all = drop.size() == 1 && drop[0] == "*";
                    std::size_t before = item.tags.size();
                    item.tags.erase(std::remove_if(item.tags.begin(), item.tags.end(),
                                                   [&](const std::string& t) {
                                                       if (all) return true;
                                                       for (const auto& d : drop) {
                                                           if (lower(d) == lower(t)) return true;
                                                       }
                                                       return false;
                                                   }),
                                    item.tags.end());
                    return item.tags.size() != before;
                });
            });

    reg.add({"library.rate", "Rate files", "Give files a rating from 0 (none) to 5 stars. Undoable.",
             selection({{"rating", ParamType::Int, "0 to 5", true}}), true, false},
            [st](const Params& p) {
                return std::make_unique<MetaEdit>(
                    st, p, "rated",
                    [](const Params& q) {
                        auto r = intParam(q, "rating");
                        return r < 0 || r > 5 ? std::string("rating must be 0 to 5") : std::string();
                    },
                    [](Item& item, const Params& q) {
                        int r = static_cast<int>(intParam(q, "rating"));
                        bool changed = item.rating != r;
                        item.rating = r;
                        return changed;
                    });
            });

    reg.add({"library.label", "Colour label", "Give files a colour label (none clears it). Undoable.",
             selection({{"label", ParamType::String, "the colour", true, labelChoices()}}), true, false},
            [st, noCheck](const Params& p) {
                return std::make_unique<MetaEdit>(st, p, "labelled", noCheck, [](Item& item, const Params& q) {
                    std::string l = stringParam(q, "label");
                    if (l == "none") l.clear();
                    bool changed = item.label != l;
                    item.label = l;
                    return changed;
                });
            });

    reg.add({"library.note", "Note on files", "Set (or append to) the notes on files. Notes are searched as free text. Undoable.",
             selection({{"text", ParamType::String, "the note (empty clears it)", true},
                        {"append", ParamType::Bool, "add to the existing note on a new line instead of replacing it", false, {}, false}}),
             true, false},
            [st, noCheck](const Params& p) {
                return std::make_unique<MetaEdit>(st, p, "noted", noCheck, [](Item& item, const Params& q) {
                    std::string text = stringParam(q, "text");
                    std::string n = boolParam(q, "append") && !item.notes.empty() && !text.empty() ? item.notes + "\n" + text : text;
                    bool changed = item.notes != n;
                    item.notes = n;
                    return changed;
                });
            });

    reg.add({"library.write_tags", "Save tags into files",
             "Write the library's tags, rating, colour label and notes into the files themselves as XMP, so they "
             "travel with the photo and show up in Lightroom, Bridge, darktable, digiKam and file managers. JPEG and PNG "
             "get them embedded (the picture itself isn't re-encoded); everything else gets a .xmp sidecar next to it. "
             "The rest of any XMP already there is kept. Undoable.",
             selection({{"where", ParamType::String, "auto (embed in JPEG/PNG, sidecar for the rest), embed or sidecar", false,
                         {"auto", "embed", "sidecar"}, std::string("auto")}}),
             true, false},
            [st](const Params& p) { return std::make_unique<TagWrite>(st, p); });

    reg.add({"library.read_tags", "Load tags from files",
             "Read tags, rating, colour label and notes stored inside files (embedded XMP, a .xmp sidecar, or the EXIF "
             "rating/keywords Windows writes) into the library. New files get these automatically when indexed. By "
             "default tags are added and the rest only fills empty fields; replace=true takes the file's values. Undoable.",
             selection({{"replace", ParamType::Bool, "use the file's values instead of merging", false, {}, false}}), true, false},
            [st, noCheck](const Params& p) {
                return std::make_unique<MetaEdit>(st, p, "read tags for", noCheck, [](Item& item, const Params& q) {
                    auto t = readFileTags(item.path);
                    if (!t) return boolParam(q, "replace") ? mergeFileTags(item, FileTags{}, true) : false;
                    return mergeFileTags(item, *t, boolParam(q, "replace"));
                });
            });

    reg.add({"library.forget", "Forget files",
             "Remove entries from the library without touching the files themselves (paths, a search or a "
             "collection). Undoable. Re-indexing the folder brings them back.",
             selection({}), true, false},
            [st](const Params& p) { return std::make_unique<ForgetEdit>(st, p); });

    // ---- duplicates ----
    reg.add({"library.duplicates", "Find duplicates",
             "Group files with identical content (same size and hash). verify reads large candidates in full so "
             "only true copies are reported. `extras` lists every copy except the one `keep` picks per group, ready to "
             "pass to library.tag, library.move or library.forget as paths=.",
             {queryParam("only look among files matching this search"), libraryParam(),
              {"verify", ParamType::Bool, "confirm large files byte for byte (full hash)", false, {}, true},
              {"keep", ParamType::String, "which copy each group keeps (the rest go into extras)", false,
               {"first", "oldest", "newest", "shortest", "rated"}, std::string("first")},
              {"min_size", ParamType::String, "ignore files smaller than this, e.g. 1kb", false, {}, std::string("1")},
              jsonParam("the groups (lists of paths, keeper first) in `items`")},
             false, true},
            [st](const Params& p) {
                return std::make_unique<FunctionAction>([st, p] {
                    std::string error;
                    Catalog* cat = openCatalog(*st, p, error);
                    if (!cat) return Result::failure(error);
                    std::uint64_t minSize = 1;
                    std::vector<Item*> scope;
                    try {
                        minSize = parseSize(stringParam(p, "min_size", "1"));
                        scope = selectItems(*cat, p, error, true);
                    } catch (const ParamError& e) {
                        return Result::failure(e.what());
                    }
                    if (!error.empty()) return Result::failure(error);
                    std::map<std::pair<std::uint64_t, std::string>, std::vector<const Item*>> groups;
                    for (const Item* i : scope) {
                        if (i->size >= minSize && !i->hash.empty()) groups[{i->size, i->hash}].push_back(i);
                    }
                    std::vector<std::vector<const Item*>> dupes;
                    const bool verify = boolParam(p, "verify", true);
                    for (auto& [key, members] : groups) {
                        if (members.size() < 2) continue;
                        if (verify && key.first > 2 * 1024 * 1024) {
                            std::map<std::string, std::vector<const Item*>> split;
                            for (const Item* i : members) {
                                try {
                                    split[contentHash(i->path, true)].push_back(i);
                                } catch (const std::exception&) {
                                }
                            }
                            for (auto& [h, g] : split) {
                                if (g.size() > 1) dupes.push_back(g);
                            }
                        } else {
                            dupes.push_back(members);
                        }
                    }
                    const std::string keep = stringParam(p, "keep", "first");
                    std::uint64_t wasted = 0;
                    std::size_t files = 0;
                    std::vector<const Item*> extras;
                    json groupsJson = json::array();
                    std::sort(dupes.begin(), dupes.end(), [](const auto& a, const auto& b) {
                        return a.front()->size * a.size() > b.front()->size * b.size();
                    });
                    std::string msg;
                    for (auto& g : dupes) {
                        std::size_t k = 0;
                        for (std::size_t i = 1; i < g.size(); ++i) {
                            bool better = keep == "oldest"   ? g[i]->mtime < g[k]->mtime
                                        : keep == "newest"   ? g[i]->mtime > g[k]->mtime
                                        : keep == "shortest" ? g[i]->path.size() < g[k]->path.size()
                                        : keep == "rated"    ? g[i]->rating > g[k]->rating
                                                             : false;
                            if (better) k = i;
                        }
                        std::swap(g[0], g[k]);
                        wasted += g.front()->size * (g.size() - 1);
                        files += g.size();
                        msg += "\n" + std::to_string(g.size()) + " copies of " + formatSize(g.front()->size) + " (hash " + g.front()->hash + "):";
                        json gj = json::array();
                        for (std::size_t i = 0; i < g.size(); ++i) {
                            msg += "\n  " + std::string(i == 0 ? "keep " : "     ") + g[i]->path;
                            gj.push_back(g[i]->path);
                            if (i > 0) extras.push_back(g[i]);
                        }
                        groupsJson.push_back(gj);
                    }
                    Result r = Result::success(std::to_string(dupes.size()) + " groups of duplicates, " + std::to_string(files) +
                                               " files, " + formatSize(wasted) + " reclaimable" + msg);
                    // extras first, so a batch "-> extras" capture holds the paths
                    r.with("extras", joinPaths(extras))
                        .with("groups", static_cast<std::int64_t>(dupes.size()))
                        .with("files", static_cast<std::int64_t>(files))
                        .with("wasted_bytes", static_cast<std::int64_t>(wasted));
                    if (boolParam(p, "json")) r.with("items", groupsJson.dump(-1, ' ', false, json::error_handler_t::replace));
                    return r;
                });
            });

    // ---- similar ----
    reg.add({"library.similar", "Find similar images",
             "Find images that look alike (resized, recompressed, lightly edited or near-identical shots) by "
             "perceptual hash and Hamming distance (0 = same picture, up to ~10 = similar, 64 bits in all). With path=, "
             "lists images like that one; without, groups every similar set. Hashes are computed once from the "
             "thumbnails and stored in the catalog.",
             {{"path", ParamType::String, "find images like this one (in the library or not)", false},
              queryParam("only look among images matching this search"), libraryParam(),
              {"threshold", ParamType::Int, "largest Hamming distance that counts as similar (0..64)", false, {}, std::int64_t{10}},
              {"method", ParamType::String, "phash compares DCT frequencies (robust to resizing, recompression and edits); dhash compares neighbouring brightness (fine for detailed photos, but smooth gradients and flat images all look alike to it)",
               false, {"dhash", "phash"}, std::string("phash")},
              {"threads", ParamType::Int, "worker threads for hashing (0 = automatic)", false, {}, std::int64_t{0}},
              previewParam("attach a contact sheet of the matches (or the first groups)"),
              jsonParam("the matches (with path=) or groups in `items`")},
             false, false},
            [st](const Params& p) {
                return std::make_unique<FunctionAction>([st, p] {
                    std::string error;
                    Catalog* cat = openCatalog(*st, p, error);
                    if (!cat) return Result::failure(error);
                    Params scopeParams = p;
                    scopeParams.erase("paths");
                    std::vector<Item*> scope = selectItems(*cat, scopeParams, error, true);
                    if (!error.empty()) return Result::failure(error);
                    scope.erase(std::remove_if(scope.begin(), scope.end(),
                                               [](const Item* i) {
                                                   return !(i->type == "image" || i->type == "raw" || i->type == "texture" || i->ext == "svg");
                                               }),
                                scope.end());
                    const bool phash = stringParam(p, "method", "phash") == "phash";
                    const int threshold = static_cast<int>(std::clamp<std::int64_t>(intParam(p, "threshold", 10), 0, 64));
                    std::vector<Item*> todo;
                    for (auto* i : scope) {
                        if (i->dhash.empty() || i->phash.empty()) todo.push_back(i);
                    }
                    std::atomic<std::size_t> failed{0};
                    parallelFor(todo.size(), intParam(p, "threads", 0), [&](std::size_t k) {
                        Item& i = *todo[k];
                        raster::Image t = thumbnail(*cat, i, 256, true);
                        if (t.empty()) {
                            ++failed;
                            return;
                        }
                        i.dhash = hex64(dHash(t));
                        i.phash = hex64(pHash(t));
                    });
                    if (!todo.empty()) {
                        try {
                            st->commit(*cat);
                        } catch (const std::exception& e) {
                            return Result::failure(e.what());
                        }
                    }
                    auto hashOf = [phash](const Item& i) { return parseHex64(phash ? i.phash : i.dhash); };
                    std::vector<const Item*> hashed;
                    for (auto* i : scope) {
                        if (!(phash ? i->phash : i->dhash).empty()) hashed.push_back(i);
                    }
                    Result r;
                    const bool preview = boolParam(p, "preview");
                    if (p.count("path")) {
                        const std::string path = normPath(stringParam(p, "path"));
                        std::uint64_t ref = 0;
                        const Item* refItem = cat->find(path);
                        if (refItem && !(phash ? refItem->phash : refItem->dhash).empty()) {
                            ref = hashOf(*refItem);
                        } else {
                            std::error_code ec;
                            if (!fs::is_regular_file(path, ec)) return Result::failure("no file at " + path);
                            std::string why;
                            raster::Image img = renderThumbnail(path, "image", extOf(path), 256, &why);
                            if (img.empty()) return Result::failure("can't read " + path + ": " + why);
                            ref = phash ? pHash(img) : dHash(img);
                        }
                        std::vector<std::pair<int, const Item*>> hits;
                        for (const Item* i : hashed) {
                            if (i->path == path) continue;
                            int d = hammingDistance(ref, hashOf(*i));
                            if (d <= threshold) hits.push_back({d, i});
                        }
                        std::sort(hits.begin(), hits.end(), [](const auto& a, const auto& b) {
                            return a.first != b.first ? a.first < b.first : a.second->path < b.second->path;
                        });
                        std::string msg = std::to_string(hits.size()) + (hits.size() == 1 ? " image" : " images") + " like " + path;
                        std::vector<const Item*> items;
                        NumberList distances;
                        for (const auto& [d, i] : hits) {
                            msg += "\n  " + std::to_string(d) + "  " + line(*i);
                            items.push_back(i);
                            distances.push_back(d);
                        }
                        r = Result::success(msg);
                        r.with("paths", joinPaths(items)).with("count", static_cast<std::int64_t>(hits.size())).with("distances", distances);
                        if (boolParam(p, "json")) r.with("items", itemsJson(items));
                        if (preview) {
                            std::vector<const Item*> sheet = items;
                            if (refItem) sheet.insert(sheet.begin(), refItem);
                            attachSheet(r, *cat, sheet, 160, 6, true);
                        }
                    } else {
                        // union-find over every pair within the threshold
                        std::vector<std::size_t> parent(hashed.size());
                        std::iota(parent.begin(), parent.end(), 0);
                        std::function<std::size_t(std::size_t)> root = [&](std::size_t x) {
                            while (parent[x] != x) x = parent[x] = parent[parent[x]];
                            return x;
                        };
                        std::vector<std::uint64_t> hs(hashed.size());
                        for (std::size_t i = 0; i < hashed.size(); ++i) hs[i] = hashOf(*hashed[i]);
                        for (std::size_t i = 0; i < hashed.size(); ++i) {
                            for (std::size_t j = i + 1; j < hashed.size(); ++j) {
                                if (hammingDistance(hs[i], hs[j]) <= threshold) parent[root(i)] = root(j);
                            }
                        }
                        std::map<std::size_t, std::vector<std::size_t>> sets;
                        for (std::size_t i = 0; i < hashed.size(); ++i) sets[root(i)].push_back(i);
                        std::vector<std::vector<std::size_t>> groups;
                        for (auto& [k, g] : sets) {
                            if (g.size() > 1) groups.push_back(g);
                        }
                        std::sort(groups.begin(), groups.end(), [](const auto& a, const auto& b) { return a.size() > b.size(); });
                        std::string msg = std::to_string(groups.size()) + " groups of similar images among " + std::to_string(hashed.size());
                        json gj = json::array();
                        std::vector<const Item*> sheet;
                        std::size_t files = 0;
                        for (const auto& g : groups) {
                            msg += "\n" + std::to_string(g.size()) + " alike:";
                            json one = json::array();
                            for (std::size_t idx : g) {
                                int d = hammingDistance(hs[g.front()], hs[idx]);
                                msg += "\n  " + std::to_string(d) + "  " + line(*hashed[idx]);
                                one.push_back(hashed[idx]->path);
                                if (sheet.size() < 60) sheet.push_back(hashed[idx]);
                            }
                            files += g.size();
                            gj.push_back(one);
                        }
                        r = Result::success(msg);
                        r.with("groups", static_cast<std::int64_t>(groups.size())).with("files", static_cast<std::int64_t>(files));
                        if (boolParam(p, "json")) r.with("items", gj.dump(-1, ' ', false, json::error_handler_t::replace));
                        if (preview) attachSheet(r, *cat, sheet, 160, 6, true);
                    }
                    if (failed) r.message += "\n(" + std::to_string(failed.load()) + " images couldn't be read)";
                    r.with("hashed", static_cast<std::int64_t>(todo.size() - failed.load()));
                    return r;
                });
            });

    // ---- thumbnails ----
    reg.add({"library.thumbnails", "Make thumbnails",
             "Generate cached PNG thumbnails (in " + std::string(kThumbDir) + " next to the catalog) for images, SVG, "
             "meshes, video (ffmpeg), audio waveforms (ffmpeg) and PDFs (pdftoppm). sheet= writes a numbered "
             "contact sheet; preview=true attaches it.",
             {pathsParam(), queryParam("which files (default: every file)"), collectionParam(), libraryParam(),
              {"size", ParamType::Int, "thumbnail size in pixels (longest side)", false, {}, std::int64_t{256}},
              {"force", ParamType::Bool, "remake thumbnails that are already cached", false, {}, false},
              {"sheet", ParamType::String, "also write a contact sheet PNG of these files here", false},
              {"columns", ParamType::Int, "contact sheet columns", false, {}, std::int64_t{6}},
              {"sheet_limit", ParamType::Int, "most files on the contact sheet", false, {}, std::int64_t{100}},
              {"threads", ParamType::Int, "worker threads (0 = automatic)", false, {}, std::int64_t{0}},
              previewParam("attach the contact sheet (first 60 files)")},
             false, false},
            [st](const Params& p) {
                return std::make_unique<FunctionAction>([st, p] {
                    std::string error;
                    Catalog* cat = openCatalog(*st, p, error);
                    if (!cat) return Result::failure(error);
                    auto items = selectItems(*cat, p, error, true);
                    if (!error.empty()) return Result::failure(error);
                    const int size = static_cast<int>(std::clamp<std::int64_t>(intParam(p, "size", 256), 16, 2048));
                    const bool force = boolParam(p, "force");
                    std::vector<raster::Image> thumbs(items.size());
                    std::vector<std::string> why(items.size());
                    std::atomic<std::size_t> made{0}, cached{0};
                    parallelFor(items.size(), intParam(p, "threads", 0), [&](std::size_t k) {
                        std::error_code ec;
                        bool wasCached = !force && fs::exists(thumbPath(*cat, *items[k], size), ec);
                        bool did = false;
                        thumbs[k] = thumbnail(*cat, *items[k], size, true, force, &why[k], &did);
                        if (wasCached && !thumbs[k].empty()) ++cached;
                        else if (did) ++made;
                    });
                    std::size_t none = 0;
                    std::map<std::string, std::size_t> reasons;
                    for (std::size_t k = 0; k < items.size(); ++k) {
                        if (thumbs[k].empty()) {
                            ++none;
                            ++reasons[why[k].empty() ? "unreadable" : why[k]];
                        }
                    }
                    std::string msg = std::to_string(made.load()) + " thumbnails made, " + std::to_string(cached.load()) + " already cached, " +
                                      std::to_string(none) + " without a thumbnail (of " + std::to_string(items.size()) + " files)";
                    for (const auto& [reason, n] : reasons) msg += "\n  " + std::to_string(n) + ": " + reason;
                    Result r = Result::success(msg);
                    r.with("made", static_cast<std::int64_t>(made.load())).with("cached", static_cast<std::int64_t>(cached.load()))
                        .with("missing", static_cast<std::int64_t>(none))
                        .with("cache", normPath(cat->file.parent_path() / kThumbDir));
                    const std::string sheetPath = stringParam(p, "sheet");
                    const bool preview = boolParam(p, "preview");
                    if (!sheetPath.empty() || preview) {
                        const auto limit = static_cast<std::size_t>(std::max<std::int64_t>(1, intParam(p, "sheet_limit", 100)));
                        std::vector<raster::Image> onSheet;
                        std::string legend;
                        for (std::size_t k = 0; k < items.size() && onSheet.size() < limit; ++k) {
                            onSheet.push_back(thumbs[k]);
                            legend += "\n  " + std::to_string(onSheet.size()) + ": " + items[k]->path;
                        }
                        const int columns = static_cast<int>(intParam(p, "columns", 6));
                        raster::Image sheet = contactSheet(onSheet, std::min(size, 256), columns);
                        if (!sheetPath.empty() && !sheet.empty()) {
                            try {
                                std::error_code ec;
                                if (auto parent = fs::path(sheetPath).parent_path(); !parent.empty()) fs::create_directories(parent, ec);
                                raster::save(sheet, sheetPath);
                                r.message += "\nwrote contact sheet " + sheetPath + " (" + std::to_string(sheet.width()) + "x" +
                                             std::to_string(sheet.height()) + "):" + legend;
                                r.with("sheet", sheetPath);
                            } catch (const std::exception& e) {
                                return Result::failure(e.what());
                            }
                        }
                        if (preview && !sheet.empty()) {
                            if (onSheet.size() > 60) {
                                onSheet.resize(60);
                                sheet = contactSheet(onSheet, std::min(size, 256), columns);
                            }
                            r.attach({"image/png", raster::encodePng(sheet), "contact-sheet.png"});
                            if (sheetPath.empty()) r.message += "\ncontact sheet:" + legend;
                        }
                    }
                    return r;
                });
            });

    // ---- collections ----
    reg.add({"library.collection_save", "Save collection",
             "Save a named collection: a smart one (query=, re-evaluated whenever it's used, so new files appear in "
             "it) or a static list (paths=, or query= with static=true to freeze today's results). Use it with "
             "collection= on other commands or collection:<name> in searches. Saving an existing name replaces it.",
             {{"name", ParamType::String, "collection name", true},
              {"query", ParamType::String, "the search that defines a smart collection", false},
              {"paths", ParamType::String, "files for a static collection: one per line or comma separated", false},
              {"static", ParamType::Bool, "store the query's current results as a fixed list", false, {}, false},
              libraryParam()},
             false, false},
            [st](const Params& p) {
                return std::make_unique<FunctionAction>([st, p] {
                    std::string error;
                    Catalog* cat = openCatalog(*st, p, error);
                    if (!cat) return Result::failure(error);
                    Collection col;
                    col.name = stringParam(p, "name");
                    if (col.name.empty() || col.name.find_first_of(" ,\"") != std::string::npos) {
                        return Result::failure("collection names can't be empty or contain spaces, commas or quotes");
                    }
                    const bool hasPaths = !stringParam(p, "paths").empty();
                    if (!hasPaths && !p.count("query")) return Result::failure("give query= (smart) or paths= (static)");
                    std::size_t count = 0;
                    if (hasPaths || boolParam(p, "static")) {
                        Params sel = p;
                        sel.erase("collection");
                        auto items = selectItems(*cat, sel, error, false);
                        if (!error.empty()) return Result::failure(error);
                        for (auto* i : items) col.paths.push_back(i->path);
                        count = col.paths.size();
                        if (col.paths.empty()) return Result::failure("no files matched, so the static collection would be empty");
                    } else {
                        col.query = stringParam(p, "query");
                        try {
                            Catalog probe = *cat;
                            probe.collections[col.name] = col;
                            count = search(probe, col.query).total;
                        } catch (const ParamError& e) {
                            return Result::failure(e.what());
                        }
                    }
                    const bool replaced = cat->collections.count(col.name) > 0;
                    const bool smart = col.smart();
                    cat->collections[col.name] = col;
                    try {
                        st->commit(*cat);
                    } catch (const std::exception& e) {
                        return Result::failure(e.what());
                    }
                    Result r = Result::success(std::string(replaced ? "replaced " : "saved ") + (smart ? "smart" : "static") +
                                               " collection " + col.name + " (" + std::to_string(count) + " files now)");
                    return r.with("name", col.name).with("smart", smart).with("files", static_cast<std::int64_t>(count));
                });
            });

    reg.add({"library.collection_delete", "Delete collection", "Delete a saved collection (the files stay).",
             {{"name", ParamType::String, "collection name", true}, libraryParam()}, false, false},
            [st](const Params& p) {
                return std::make_unique<FunctionAction>([st, p] {
                    std::string error;
                    Catalog* cat = openCatalog(*st, p, error);
                    if (!cat) return Result::failure(error);
                    if (!cat->collections.erase(stringParam(p, "name"))) return Result::failure("no collection called " + stringParam(p, "name"));
                    try {
                        st->commit(*cat);
                    } catch (const std::exception& e) {
                        return Result::failure(e.what());
                    }
                    return Result::success("deleted collection " + stringParam(p, "name"));
                });
            });

    reg.add({"library.collections", "List collections", "List saved collections with their searches and current sizes.",
             {libraryParam(), jsonParam("the collections in `items`")}, false, true},
            [st](const Params& p) {
                return std::make_unique<FunctionAction>([st, p] {
                    std::string error;
                    Catalog* cat = openCatalog(*st, p, error);
                    if (!cat) return Result::failure(error);
                    std::string msg = std::to_string(cat->collections.size()) + " collections";
                    json arr = json::array();
                    std::string names;
                    for (const auto& [name, col] : cat->collections) {
                        std::size_t n = col.paths.size();
                        if (col.smart()) {
                            try {
                                n = search(*cat, col.query).total;
                            } catch (const ParamError&) {
                            }
                        }
                        msg += "\n  " + name + " (" + std::to_string(n) + " files): " + (col.smart() ? col.query : "static list");
                        arr.push_back({{"name", name}, {"smart", col.smart()}, {"query", col.query}, {"files", n}});
                        names += (names.empty() ? "" : ",") + name;
                    }
                    Result r = Result::success(msg);
                    r.with("count", static_cast<std::int64_t>(cat->collections.size())).with("names", names);
                    if (boolParam(p, "json")) r.with("items", arr.dump());
                    return r;
                });
            });

    // ---- move / rename / copy ----
    auto fileOpParams = [selection](std::vector<ParamSpec> extra) {
        extra.push_back({"overwrite", ParamType::Bool, "replace files that already exist at the destination", false, {}, false});
        extra.push_back({"dry_run", ParamType::Bool, "only say what would happen", false, {}, false});
        return selection(std::move(extra));
    };

    reg.add({"library.move", "Move files",
             "Move files (paths, a search or a collection) into a folder and update the library, keeping their tags, "
             "ratings and notes. Existing files are skipped unless overwrite=true. Try dry_run=true first.",
             fileOpParams({{"to", ParamType::String, "destination folder (created if needed)", true}}), false, false},
            [st](const Params& p) {
                return std::make_unique<FunctionAction>([st, p] {
                    std::string error;
                    Catalog* cat = openCatalog(*st, p, error);
                    if (!cat) return Result::failure(error);
                    auto items = selectItems(*cat, p, error, false);
                    if (!error.empty()) return Result::failure(error);
                    const fs::path to = stringParam(p, "to");
                    const bool dry = boolParam(p, "dry_run"), overwrite = boolParam(p, "overwrite");
                    std::error_code ec;
                    if (!dry) fs::create_directories(to, ec);
                    std::size_t moved = 0, skipped = 0;
                    std::string msg;
                    for (Item* item : items) {
                        const fs::path dest = to / fs::path(item->path).filename();
                        const std::string destKey = normPath(dest);
                        if (destKey == item->path) continue;
                        if (fs::exists(dest, ec) && !overwrite) {
                            ++skipped;
                            msg += "\n  skipped (exists): " + destKey;
                            continue;
                        }
                        msg += "\n  " + item->path + " -> " + destKey;
                        if (dry) {
                            ++moved;
                            continue;
                        }
                        std::string why;
                        if (!moveFile(item->path, dest, why)) {
                            ++skipped;
                            msg += " (failed: " + why + ")";
                            continue;
                        }
                        if (Item* old = cat->find(destKey); old && old != item) old->path.clear();  // overwritten entry
                        renameInCollections(*cat, item->path, destKey);
                        item->path = destKey;
                        ++moved;
                    }
                    if (!dry) {
                        cat->items.erase(std::remove_if(cat->items.begin(), cat->items.end(), [](const Item& i) { return i.path.empty(); }),
                                         cat->items.end());
                        cat->rebuildIndex();
                        try {
                            st->commit(*cat);
                        } catch (const std::exception& e) {
                            return Result::failure(e.what());
                        }
                    }
                    Result r = Result::success(std::string(dry ? "would move " : "moved ") + std::to_string(moved) + " files, skipped " +
                                               std::to_string(skipped) + msg);
                    return r.with("moved", static_cast<std::int64_t>(moved)).with("skipped", static_cast<std::int64_t>(skipped));
                });
            });

    reg.add({"library.rename", "Rename files",
             "Rename files in place and update the library. name is a template: {name} (old name without extension), "
             "{ext} (with its dot), {n} (1, 2, 3 in order; {n:3} pads to 001), {date} (modified, YYYY-MM-DD), {width}, "
             "{height}, {rating}, {type}, {label}, {hash}. E.g. name=holiday_{n:3}{ext}. Try dry_run=true first.",
             fileOpParams({{"name", ParamType::String, "new name or name template", true},
                           {"sort", ParamType::String, "order for {n} (same fields as library.search)", false, {}, std::string("path")},
                           {"start", ParamType::Int, "first {n}", false, {}, std::int64_t{1}}}),
             false, false},
            [st](const Params& p) {
                return std::make_unique<FunctionAction>([st, p] {
                    std::string error;
                    Catalog* cat = openCatalog(*st, p, error);
                    if (!cat) return Result::failure(error);
                    auto items = selectItems(*cat, p, error, false);
                    if (!error.empty()) return Result::failure(error);
                    std::vector<const Item*> ordered(items.begin(), items.end());
                    try {
                        // reuse the search sorter through a tiny catalog of just these items
                        Catalog tmp;
                        for (const Item* i : ordered) tmp.items.push_back(*i);
                        tmp.rebuildIndex();
                        auto sorted = search(tmp, "", {stringParam(p, "sort", "path"), -1, 0});
                        ordered.clear();
                        for (const Item* i : sorted.items) ordered.push_back(cat->find(i->path));
                    } catch (const ParamError& e) {
                        return Result::failure(e.what());
                    }
                    const std::string tmpl = stringParam(p, "name");
                    const bool dry = boolParam(p, "dry_run"), overwrite = boolParam(p, "overwrite");
                    std::size_t n = static_cast<std::size_t>(std::max<std::int64_t>(0, intParam(p, "start", 1)));
                    std::size_t renamed = 0, skipped = 0;
                    std::set<std::string> taken;
                    std::string msg;
                    for (const Item* ci : ordered) {
                        Item* item = cat->find(ci->path);
                        std::string newName = renderName(tmpl, *item, n++);
                        if (newName.empty() || newName.find_first_of("/\\") != std::string::npos || newName == "." || newName == "..") {
                            return Result::failure("'" + newName + "' is not a file name (rename keeps files in their folder; use library.move to move them)");
                        }
                        const fs::path dest = fs::path(item->path).parent_path() / newName;
                        const std::string destKey = normPath(dest);
                        if (destKey == item->path) continue;
                        std::error_code ec;
                        if ((fs::exists(dest, ec) && !overwrite) || taken.count(destKey)) {
                            ++skipped;
                            msg += "\n  skipped (exists): " + destKey;
                            continue;
                        }
                        taken.insert(destKey);
                        msg += "\n  " + item->name() + " -> " + newName;
                        if (dry) {
                            ++renamed;
                            continue;
                        }
                        std::string why;
                        if (!moveFile(item->path, dest, why)) {
                            ++skipped;
                            msg += " (failed: " + why + ")";
                            continue;
                        }
                        renameInCollections(*cat, item->path, destKey);
                        item->path = destKey;
                        cat->rebuildIndex();
                        ++renamed;
                    }
                    if (!dry) {
                        try {
                            st->commit(*cat);
                        } catch (const std::exception& e) {
                            return Result::failure(e.what());
                        }
                    }
                    Result r = Result::success(std::string(dry ? "would rename " : "renamed ") + std::to_string(renamed) + " files, skipped " +
                                               std::to_string(skipped) + msg);
                    return r.with("renamed", static_cast<std::int64_t>(renamed)).with("skipped", static_cast<std::int64_t>(skipped));
                });
            });

    reg.add({"library.copy", "Copy files",
             "Copy files (paths, a search or a collection) into a folder, adding the copies to the library with the same "
             "tags, rating, label and notes. Handy for gathering a selection for a client or a project.",
             fileOpParams({{"to", ParamType::String, "destination folder (created if needed)", true}}), false, false},
            [st](const Params& p) {
                return std::make_unique<FunctionAction>([st, p] {
                    std::string error;
                    Catalog* cat = openCatalog(*st, p, error);
                    if (!cat) return Result::failure(error);
                    auto selected = selectItems(*cat, p, error, false);
                    if (!error.empty()) return Result::failure(error);
                    std::vector<Item> items;
                    for (auto* i : selected) items.push_back(*i);
                    const fs::path to = stringParam(p, "to");
                    const bool dry = boolParam(p, "dry_run"), overwrite = boolParam(p, "overwrite");
                    std::error_code ec;
                    if (!dry) fs::create_directories(to, ec);
                    std::size_t copied = 0, skipped = 0;
                    std::string msg;
                    for (const Item& item : items) {
                        const fs::path dest = to / fs::path(item.path).filename();
                        const std::string destKey = normPath(dest);
                        if (destKey == item.path || (fs::exists(dest, ec) && !overwrite)) {
                            ++skipped;
                            msg += "\n  skipped (exists): " + destKey;
                            continue;
                        }
                        msg += "\n  " + item.path + " -> " + destKey;
                        if (dry) {
                            ++copied;
                            continue;
                        }
                        fs::copy_file(item.path, dest, fs::copy_options::overwrite_existing, ec);
                        if (ec) {
                            ++skipped;
                            msg += " (failed: " + ec.message() + ")";
                            continue;
                        }
                        Item copy = item;
                        copy.path = destKey;
                        auto t = fs::last_write_time(dest, ec);
                        copy.mtick = static_cast<std::int64_t>(t.time_since_epoch().count());
                        copy.mtime = unixSeconds(t);
                        if (Item* old = cat->find(destKey)) *old = copy;
                        else cat->items.push_back(copy);
                        cat->rebuildIndex();
                        ++copied;
                    }
                    if (!dry) {
                        try {
                            st->commit(*cat);
                        } catch (const std::exception& e) {
                            return Result::failure(e.what());
                        }
                    }
                    Result r = Result::success(std::string(dry ? "would copy " : "copied ") + std::to_string(copied) + " files, skipped " +
                                               std::to_string(skipped) + msg);
                    return r.with("copied", static_cast<std::int64_t>(copied)).with("skipped", static_cast<std::int64_t>(skipped));
                });
            });

    // ---- stats ----
    reg.add({"library.stats", "Library stats",
             "Counts and sizes by type and extension, how many files are tagged, rated and labelled, duplicate waste, "
             "and the folders the library covers. query= narrows it.",
             {queryParam("only count files matching this search"), collectionParam(), libraryParam(),
              jsonParam("the per-type and per-extension counts in `types` and `extensions`")},
             false, true},
            [st](const Params& p) {
                return std::make_unique<FunctionAction>([st, p] {
                    std::string error;
                    Catalog* cat = openCatalog(*st, p, error);
                    if (!cat) return Result::failure(error);
                    auto items = selectItems(*cat, p, error, true);
                    if (!error.empty()) return Result::failure(error);
                    std::map<std::string, std::pair<std::size_t, std::uint64_t>> byType, byExt;
                    std::map<std::pair<std::uint64_t, std::string>, std::size_t> hashes;
                    std::uint64_t total = 0;
                    std::size_t tagged = 0, rated = 0, labelled = 0, noted = 0;
                    std::int64_t lastIndexed = 0;
                    for (const Item* i : items) {
                        auto& t = byType[i->type.empty() ? "unknown" : i->type];
                        ++t.first;
                        t.second += i->size;
                        auto& e = byExt[i->ext.empty() ? "(none)" : i->ext];
                        ++e.first;
                        e.second += i->size;
                        total += i->size;
                        tagged += !i->tags.empty();
                        rated += i->rating > 0;
                        labelled += !i->label.empty();
                        noted += !i->notes.empty();
                        lastIndexed = std::max(lastIndexed, i->indexed);
                        if (i->size > 0) ++hashes[{i->size, i->hash}];
                    }
                    std::uint64_t wasted = 0;
                    std::size_t dupGroups = 0;
                    for (const auto& [k, n] : hashes) {
                        if (n > 1) {
                            ++dupGroups;
                            wasted += k.first * (n - 1);
                        }
                    }
                    std::ostringstream o;
                    o << items.size() << " files, " << formatSize(total) << "\nby type:";
                    json types = json::object(), exts = json::object();
                    std::vector<std::pair<std::string, std::pair<std::size_t, std::uint64_t>>> tv(byType.begin(), byType.end());
                    std::sort(tv.begin(), tv.end(), [](const auto& a, const auto& b) { return a.second.second > b.second.second; });
                    for (const auto& [k, v] : tv) {
                        o << "\n  " << k << ": " << v.first << " files, " << formatSize(v.second);
                        types[k] = {{"files", v.first}, {"bytes", v.second}};
                    }
                    std::vector<std::pair<std::string, std::pair<std::size_t, std::uint64_t>>> ev(byExt.begin(), byExt.end());
                    std::sort(ev.begin(), ev.end(), [](const auto& a, const auto& b) { return a.second.first > b.second.first; });
                    o << "\ntop extensions:";
                    for (std::size_t k = 0; k < ev.size(); ++k) {
                        if (k < 12) o << "\n  " << ev[k].first << ": " << ev[k].second.first << " files, " << formatSize(ev[k].second.second);
                        exts[ev[k].first] = {{"files", ev[k].second.first}, {"bytes", ev[k].second.second}};
                    }
                    o << "\ntagged " << tagged << ", rated " << rated << ", labelled " << labelled << ", with notes " << noted;
                    o << "\nduplicates: " << dupGroups << " groups, " << formatSize(wasted) << " reclaimable";
                    o << "\ncollections: " << cat->collections.size();
                    o << "\nroots:";
                    for (const auto& r : cat->roots) o << "\n  " << r;
                    if (lastIndexed) o << "\nlast indexed: " << isoTime(lastIndexed) << " UTC";
                    o << "\ncatalog: " << normPath(cat->file);
                    Result r = Result::success(o.str());
                    r.with("files", static_cast<std::int64_t>(items.size())).with("bytes", static_cast<std::int64_t>(total))
                        .with("tagged", static_cast<std::int64_t>(tagged)).with("rated", static_cast<std::int64_t>(rated))
                        .with("labelled", static_cast<std::int64_t>(labelled))
                        .with("duplicate_groups", static_cast<std::int64_t>(dupGroups)).with("wasted_bytes", static_cast<std::int64_t>(wasted));
                    if (boolParam(p, "json")) r.with("types", types.dump()).with("extensions", exts.dump());
                    return r;
                });
            });

    // ---- export ----
    reg.add({"library.export", "Export list",
             "Write files from the library (a search, a collection or everything) to CSV (spreadsheets), JSON or JSON "
             "lines, with the columns you pick.",
             {{"output", ParamType::String, "file to write: .csv, .json or .jsonl", true},
              queryParam("which files (default: every file)"), collectionParam(), pathsParam(), libraryParam(),
              {"fields", ParamType::String, "columns, comma separated", false, {},
               std::string("path,name,type,format,ext,size,modified,width,height,duration,pages,rating,label,tags,notes,hash")},
              {"sort", ParamType::String, "sort field (as library.search)", false, {}, std::string("path")}},
             false, false},
            [st](const Params& p) {
                return std::make_unique<FunctionAction>([st, p] {
                    std::string error;
                    Catalog* cat = openCatalog(*st, p, error);
                    if (!cat) return Result::failure(error);
                    auto selected = selectItems(*cat, p, error, true);
                    if (!error.empty()) return Result::failure(error);
                    std::vector<const Item*> items(selected.begin(), selected.end());
                    {
                        Catalog tmp;
                        for (const Item* i : items) tmp.items.push_back(*i);
                        tmp.rebuildIndex();
                        try {
                            auto sorted = search(tmp, "", {stringParam(p, "sort", "path"), -1, 0});
                            items.clear();
                            for (const Item* i : sorted.items) items.push_back(cat->find(i->path));
                        } catch (const ParamError& e) {
                            return Result::failure(e.what());
                        }
                    }
                    const auto fields = splitOn(stringParam(p, "fields",
                                                            "path,name,type,format,ext,size,modified,width,height,duration,pages,rating,label,tags,notes,hash"),
                                                ",");
                    static const std::set<std::string> known = {"path", "name", "type", "format", "ext", "size", "modified", "mtime",
                                                                "width", "height", "duration", "pages", "triangles", "bounds", "rating",
                                                                "label", "tags", "notes", "hash", "dhash", "phash", "indexed"};
                    for (const auto& f : fields) {
                        if (!known.count(f)) return Result::failure("unknown field '" + f + "'");
                    }
                    auto value = [](const Item& i, const std::string& f) -> json {
                        if (f == "path") return i.path;
                        if (f == "name") return i.name();
                        if (f == "type") return i.type;
                        if (f == "format") return i.format;
                        if (f == "ext") return i.ext;
                        if (f == "size") return i.size;
                        if (f == "modified") return isoTime(i.mtime);
                        if (f == "mtime") return i.mtime;
                        if (f == "width") return i.width;
                        if (f == "height") return i.height;
                        if (f == "duration") return i.duration;
                        if (f == "pages") return i.pages;
                        if (f == "triangles") return i.triangles;
                        if (f == "bounds") return i.bounds;
                        if (f == "rating") return i.rating;
                        if (f == "label") return i.label;
                        if (f == "tags") return i.tags;
                        if (f == "notes") return i.notes;
                        if (f == "hash") return i.hash;
                        if (f == "dhash") return i.dhash;
                        if (f == "phash") return i.phash;
                        return i.indexed;
                    };
                    const fs::path out = stringParam(p, "output");
                    const std::string ext = extOf(out);
                    if (ext != "csv" && ext != "json" && ext != "jsonl") return Result::failure("output must end in .csv, .json or .jsonl");
                    std::error_code ec;
                    if (!out.parent_path().empty()) fs::create_directories(out.parent_path(), ec);
                    std::ofstream f(out, std::ios::binary | std::ios::trunc);
                    if (!f) return Result::failure("cannot write " + out.string());
                    const auto dumpOpts = json::error_handler_t::replace;
                    if (ext == "csv") {
                        auto cell = [](std::string s) {
                            if (s.find_first_of(",\"\n\r") == std::string::npos) return s;
                            std::string q = "\"";
                            for (char c : s) q += c == '"' ? std::string("\"\"") : std::string(1, c);
                            return q + "\"";
                        };
                        for (std::size_t k = 0; k < fields.size(); ++k) f << (k ? "," : "") << fields[k];
                        f << "\r\n";
                        for (const Item* i : items) {
                            for (std::size_t k = 0; k < fields.size(); ++k) {
                                json v = value(*i, fields[k]);
                                std::string s;
                                if (v.is_string()) s = v.get<std::string>();
                                else if (v.is_array()) {
                                    for (const auto& e : v) s += (s.empty() ? "" : ";") + (e.is_string() ? e.get<std::string>() : e.dump());
                                } else s = v.dump();
                                f << (k ? "," : "") << cell(s);
                            }
                            f << "\r\n";
                        }
                    } else {
                        nlohmann::ordered_json arr = nlohmann::ordered_json::array();
                        for (const Item* i : items) {
                            nlohmann::ordered_json row = nlohmann::ordered_json::object();
                            for (const auto& fl : fields) row[fl] = value(*i, fl);
                            if (ext == "jsonl") f << row.dump(-1, ' ', false, dumpOpts) << "\n";
                            else arr.push_back(row);
                        }
                        if (ext == "json") f << arr.dump(2, ' ', false, dumpOpts) << "\n";
                    }
                    if (!f) return Result::failure("cannot write " + out.string());
                    Result r = Result::success("wrote " + std::to_string(items.size()) + " files to " + out.string());
                    return r.with("output", out.string()).with("files", static_cast<std::int64_t>(items.size()));
                });
            });
}

}  // namespace wetwaresuite::library
