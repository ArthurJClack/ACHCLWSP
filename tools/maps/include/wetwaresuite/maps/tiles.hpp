// Offline tile files: PMTiles v3 archives (one file, many tiles, read in place)
// and Mapbox Vector Tiles (MVT), the format of the Protomaps map packs the hub
// installs. No network, no SQLite: MBTiles must be converted first (see docs).
#pragma once

#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "wetwaresuite/maps/maps.hpp"

namespace wetwaresuite::maps {

// ---- PMTiles v3 ----------------------------------------------------------------------

// Hilbert tile ids: z0 is 0, z1 tiles are 1..4, z2 tiles 5..20, and so on.
std::uint64_t zxyToTileId(int z, std::uint32_t x, std::uint32_t y);
bool tileIdToZxy(std::uint64_t id, int& z, std::uint32_t& x, std::uint32_t& y);  // false when the id is beyond z31

enum class TileType { Unknown = 0, Mvt = 1, Png = 2, Jpeg = 3, Webp = 4, Avif = 5 };
enum class TileCompression { Unknown = 0, None = 1, Gzip = 2, Brotli = 3, Zstd = 4 };
const char* toString(TileType t);
const char* toString(TileCompression c);

struct PmtilesHeader {
    std::uint64_t rootOffset = 0, rootLength = 0, metadataOffset = 0, metadataLength = 0;
    std::uint64_t leafOffset = 0, leafLength = 0, dataOffset = 0, dataLength = 0;
    std::uint64_t addressedTiles = 0, tileEntries = 0, tileContents = 0;
    bool clustered = false;
    TileCompression internalCompression = TileCompression::None, tileCompression = TileCompression::None;
    TileType tileType = TileType::Unknown;
    int minZoom = 0, maxZoom = 0, centerZoom = 0;
    double minLon = -180, minLat = -85, maxLon = 180, maxLat = 85, centerLon = 0, centerLat = 0;
};

struct PmtilesEntry {
    std::uint64_t tileId = 0, offset = 0;
    std::uint32_t length = 0, runLength = 0;  // runLength 0: a leaf directory, not a tile
};

PmtilesHeader parsePmtilesHeader(const std::string& bytes);  // the first 127 bytes; throws on a bad magic/version
std::vector<PmtilesEntry> parsePmtilesDirectory(const std::string& bytes);
std::string encodePmtilesDirectory(const std::vector<PmtilesEntry>& entries);

// Reads tiles out of a .pmtiles file without loading it (the planet is 100+ GB).
// Thread-safe; leaf directories are cached.
class PmtilesReader {
public:
    explicit PmtilesReader(const std::filesystem::path& path);  // throws std::runtime_error with a plain reason
    const PmtilesHeader& header() const { return header_; }
    const std::filesystem::path& path() const { return path_; }
    std::string metadata() const;  // the JSON metadata, decompressed ("{}" when empty)
    // The tile's bytes, decompressed; nullopt when the archive has no such tile.
    std::optional<std::string> tile(int z, std::uint32_t x, std::uint32_t y) const;

private:
    std::string read(std::uint64_t offset, std::uint64_t length) const;
    std::vector<PmtilesEntry> directory(std::uint64_t offset, std::uint64_t length) const;
    std::filesystem::path path_;
    PmtilesHeader header_;
    std::vector<PmtilesEntry> root_;
    mutable std::mutex mutex_;
    mutable std::map<std::uint64_t, std::vector<PmtilesEntry>> leaves_;
};

// What's in an archive: the header plus the metadata's name, attribution (plain text) and layers.
struct TilesInfo {
    PmtilesHeader header;
    std::string name, attribution, description;
    std::vector<std::string> layers;  // vector layer ids (from the metadata, else from one decoded tile)
};
TilesInfo describePmtiles(const PmtilesReader& reader);

// One shared reader per file (reopened when the file changes).
std::shared_ptr<const PmtilesReader> openPmtiles(const std::filesystem::path& path);

struct PmtilesTile {
    int z = 0;
    std::uint32_t x = 0, y = 0;
    std::string bytes;  // uncompressed
};
// Writes a PMTiles v3 archive (tiles gzip-compressed when `gzip`); used by tests and to pack tile folders.
// leafSize > 0 forces leaf directories of that many entries.
std::string writePmtiles(std::vector<PmtilesTile> tiles, TileType type, const std::string& metadataJson, bool gzip = true, std::size_t leafSize = 0);

std::string gunzip(const std::string& data);           // gzip or zlib; throws on corrupt data
std::string gzipCompress(const std::string& data);

// Map packs: <data folder>/packs/<id>/*.pmtiles, where `hub install` puts them
// ($WETWARESUITE_HOME, else ~/.local/share/wetwaresuite, ~/Library/Application Support/wetwaresuite, %APPDATA%\wetwaresuite).
std::filesystem::path packsDir();
std::vector<std::filesystem::path> findMapPacks();
std::optional<std::filesystem::path> bestMapPack();  // the installed pack with the deepest zoom

// Offline address search: the names of places and points of interest in a pack's vector tiles
// (layers places, pois, place_label, ...), read once (zoom 12 or less, at most 20,000 tiles) and
// kept in the cache folder. Point features with "name" and "kind".
std::vector<Feature> packPlaces(const std::filesystem::path& pack);
// Every installed pack's places, kept in memory until a pack changes.
std::shared_ptr<const std::vector<Feature>> allPackPlaces();

// ---- Mapbox Vector Tiles -------------------------------------------------------------

struct MvtLayer {
    std::string name;
    int extent = 4096;
    std::vector<Feature> features;  // geometry in tile units: 0..extent, y down (may stick out by the buffer)
};
std::vector<MvtLayer> decodeMvt(const std::string& bytes);  // throws std::runtime_error on a broken tile

}  // namespace wetwaresuite::maps
