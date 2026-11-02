// library: an asset browser for every format, headless first. A catalog file
// (JSON lines, default .wetwaresuite-library.json in the indexed folder) records
// each file's size, modification time, content hash, detected type and cheap
// facts (pixel size, duration, page count, mesh bounds) plus what people add:
// tags, a rating, a colour label and notes. Searches use a small query
// language ("type:image width>=3840 -tag:used sort:-size limit:20"); thumbnails
// are cached PNGs next to the catalog. Everything is a library.* command.
#pragma once

#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "wetwaresuite/core/command.hpp"
#include "wetwaresuite/raster/image.hpp"

namespace wetwaresuite::library {

namespace fs = std::filesystem;

// The default catalog file name, created in the indexed folder.
inline constexpr const char* kCatalogName = ".wetwaresuite-library.json";
// Thumbnails cache folder, next to the catalog file.
inline constexpr const char* kThumbDir = ".wetwaresuite-thumbs";
// The same two from before the rename: an old catalog is still found and read.
inline constexpr const char* kOldCatalogName = ".achclwss-library.json";
inline constexpr const char* kOldThumbDir = ".achclwss-thumbs";
// The catalog in a folder: the default name, or an old-named one if only that exists.
fs::path catalogIn(const fs::path& dir);

struct Item {
    std::string path;          // absolute, forward slashes
    std::uint64_t size = 0;    // bytes
    std::int64_t mtime = 0;    // seconds since 1970 (UTC)
    std::int64_t mtick = 0;    // raw file-clock ticks, for exact change detection
    std::string hash;          // 16 hex digits: FNV-1a 64 of the content (or of its first and last MiB plus size)
    std::string type;          // toolkit category: image, raw, vector, cad, 3d, texture, audio, video, document, ...
    std::string format;        // e.g. "PNG image"
    std::string ext;           // lowercase, no dot
    int width = 0, height = 0; // pixels (images, video, SVG canvas)
    double duration = 0;       // seconds (audio, video)
    int pages = 0;             // documents (PDF)
    std::int64_t triangles = 0;  // meshes
    std::vector<double> bounds;  // meshes: size along x, y, z
    std::vector<std::string> tags;
    int rating = 0;            // 0..5
    std::string label;         // red, orange, yellow, green, blue, purple, gray or empty
    std::string notes;
    std::string dhash, phash;  // perceptual hashes, 16 hex digits, empty until computed
    std::int64_t indexed = 0;  // when this entry was last (re)read, seconds since 1970

    std::string name() const;  // file name
    bool hasTag(const std::string& tag) const;  // case-insensitive
};

struct Collection {
    std::string name;
    std::string query;               // smart collection: a saved search
    std::vector<std::string> paths;  // static collection: a fixed list of files
    bool smart() const { return paths.empty(); }
};

class Catalog {
public:
    fs::path file;                  // where it is stored
    std::vector<std::string> roots;  // folders indexed into it (absolute, forward slashes)
    std::vector<Item> items;
    std::map<std::string, Collection> collections;

    // Reads a catalog; a missing file gives an empty catalog. Throws std::runtime_error on a corrupt one.
    static Catalog load(const fs::path& file);
    // Writes atomically (temporary file, then rename). Throws std::runtime_error.
    void save() const;

    Item* find(const std::string& path);
    const Item* find(const std::string& path) const;
    void rebuildIndex();  // call after adding, removing or renaming items

private:
    std::unordered_map<std::string, std::size_t> byPath_;
};

// ---- files ------------------------------------------------------------------

// Absolute, normalised, forward slashes: the form item paths use.
std::string normPath(const fs::path& p);
std::string lower(std::string s);
std::string extOf(const fs::path& p);  // lowercase, no dot

// FNV-1a 64 of the whole file when it is at most 2 MiB (or `full`), else of
// its first and last MiB plus its size. 16 hex digits. Throws when unreadable.
std::string contentHash(const fs::path& path, bool full = false);
std::string fnv1a64Hex(const std::string& bytes);

// Seconds since 1970 for a file time.
std::int64_t unixSeconds(fs::file_time_type t);

// Reads a file's facts (size, time, hash, type, dimensions...) into a fresh item.
// `probe` also runs ffprobe/pdfinfo and loads meshes and SVGs for their sizes.
Item readItem(const fs::path& path, bool probe = true, bool fullHash = false);

// Pixel size from a raster file's header (PNG, JPEG, GIF, BMP, WebP, PSD, TGA, PNM, TIFF, DDS, QOI).
std::optional<std::pair<int, int>> imageHeaderSize(const fs::path& path);

// ---- tags inside the files (XMP / EXIF) -----------------------------------------

struct FileTags {
    std::vector<std::string> tags;
    int rating = 0;
    std::string label, notes;
    std::string from;  // "the file", "exif" or "sidecar photo.xmp"
    bool any() const;
};

// One file's bytes before a write (nullopt: it didn't exist), so undo can put it back.
struct FileBackup {
    fs::path path;
    std::optional<std::string> bytes;
};

// The .xmp sidecar for a file: photo.xmp (Bridge, Lightroom), or photo.jpg.xmp when only that exists (darktable).
fs::path sidecarPath(const fs::path& file);
// Tags, rating, colour label and description from the XMP embedded in a JPEG or PNG, the
// EXIF rating/keywords of a JPEG, or the file's .xmp sidecar. nullopt when there are none.
std::optional<FileTags> readFileTags(const fs::path& path);
// Writes the four fields into the file's XMP (where: auto = embed in JPEG/PNG and a sidecar for
// everything else, embed, sidecar), keeping the rest of an existing packet. Returns where it
// went. Throws std::runtime_error.
std::string writeFileTags(const fs::path& path, const FileTags& tags, const std::string& where,
                          std::vector<FileBackup>* backup = nullptr);

// ---- queries ----------------------------------------------------------------

struct Query;  // parsed form, see query.cpp

struct SearchOptions {
    std::string sort;    // field, "-field" for descending; overrides sort: in the query
    std::int64_t limit = -1;  // -1 = the query's limit: or everything
    std::int64_t offset = 0;
};

struct SearchResult {
    std::vector<const Item*> items;  // the page asked for
    std::size_t total = 0;           // matches before limit/offset
};

// Throws ParamError for a malformed query (with a readable reason).
SearchResult search(const Catalog& catalog, const std::string& query, const SearchOptions& options = {},
                    std::int64_t now = 0);
// True when one item matches (sort: and limit: are ignored).
bool matches(const Catalog& catalog, const Item& item, const std::string& query, std::int64_t now = 0);

// "10mb" -> 10485760; "1.5k" -> 1536. Throws ParamError.
std::uint64_t parseSize(const std::string& text);
std::string formatSize(std::uint64_t bytes);  // "12.3 MB"

// ---- perceptual hashes and thumbnails -----------------------------------------

std::uint64_t dHash(const raster::Image& img);
std::uint64_t pHash(const raster::Image& img);
int hammingDistance(std::uint64_t a, std::uint64_t b);
std::string hex64(std::uint64_t v);
std::uint64_t parseHex64(const std::string& s);

// Renders a thumbnail of any supported file, fitting in size x size. Empty image when
// the format has no thumbnailer (or its helper program is missing). `why` says why.
raster::Image renderThumbnail(const fs::path& path, const std::string& type, const std::string& ext, int size,
                              std::string* why = nullptr);
// The cached thumbnail file for an item.
fs::path thumbPath(const Catalog& catalog, const Item& item, int size);
// Lays thumbnails out on a grid with a number in each cell's corner.
raster::Image contactSheet(const std::vector<raster::Image>& thumbs, int cell, int columns);

// ---- commands -----------------------------------------------------------------

// Registers every library.* command.
void registerCommands(Session& session);

}  // namespace wetwaresuite::library
