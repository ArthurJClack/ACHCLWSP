// Offline address search from map packs: the names of places, neighbourhoods and points
// of interest in a pack's vector tiles, read once and kept in the cache folder as a small
// tab-separated file, so maps.search finds "Montmartre" or "Gare du Nord" with no network.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <mutex>
#include <set>
#include <sstream>

#include "wetwaresuite/maps/maps.hpp"
#include "wetwaresuite/maps/tiles.hpp"

namespace wetwaresuite::maps {

namespace fs = std::filesystem;

namespace {

constexpr std::size_t kMaxTiles = 20000;

bool placeLayer(const std::string& name) {
    static const std::set<std::string> names = {"places", "place", "pois", "poi", "place_label", "poi_label", "natural_label",
                                                "mountain_peak", "water_name", "aerodrome_label", "transit_stop_label"};
    return names.count(name) > 0;
}

std::string text(const Props& p, const char* key) {
    auto it = p.find(key);
    return it != p.end() && it->second.kind == Prop::String ? it->second.str : std::string();
}

std::string clean(std::string s) {
    for (auto& c : s)
        if (c == '\t' || c == '\n' || c == '\r') c = ' ';
    return s;
}

fs::path indexFile(const fs::path& pack) {
    std::error_code ec;
    const auto size = fs::file_size(pack, ec);
    const auto when = fs::last_write_time(pack, ec).time_since_epoch().count();
    std::uint64_t h = 1469598103934665603ull;
    for (unsigned char c : fs::absolute(pack, ec).string() + "|" + std::to_string(static_cast<unsigned long long>(size)) + "|" + std::to_string(static_cast<long long>(when))) h = (h ^ c) * 1099511628211ull;
    char hex[17];
    std::snprintf(hex, sizeof hex, "%016llx", static_cast<unsigned long long>(h));
    return defaultTileCache().parent_path() / "places" / (pack.stem().string() + "-" + hex + ".tsv");
}

std::vector<Feature> readIndex(const fs::path& file) {
    std::vector<Feature> out;
    std::ifstream in(file, std::ios::binary);
    std::string line;
    while (std::getline(in, line)) {
        std::istringstream row(line);
        std::string name, kind, lon, lat;
        if (!std::getline(row, name, '\t') || !std::getline(row, kind, '\t') || !std::getline(row, lon, '\t') || !std::getline(row, lat)) continue;
        Feature f;
        f.geom.type = GeomType::Point;
        f.geom.points.push_back({std::strtod(lon.c_str(), nullptr), std::strtod(lat.c_str(), nullptr)});
        f.props["name"] = Prop::string(name);
        if (!kind.empty()) f.props["kind"] = Prop::string(kind);
        out.push_back(std::move(f));
    }
    return out;
}

}  // namespace

std::vector<Feature> packPlaces(const fs::path& pack) {
    const fs::path cached = indexFile(pack);
    std::error_code ec;
    if (fs::is_regular_file(cached, ec)) return readIndex(cached);
    const auto reader = openPmtiles(pack);
    const PmtilesHeader& h = reader->header();
    std::vector<Feature> out;
    if (h.tileType != TileType::Mvt) return out;
    // the pack's area (a pack with no bounds in its header covers the world)
    double minLon = h.minLon, minLat = h.minLat, maxLon = h.maxLon, maxLat = h.maxLat;
    if (!(minLon < maxLon && minLat < maxLat)) {
        minLon = -180;
        maxLon = 180;
        minLat = -85.05;
        maxLat = 85.05;
    }
    auto tileRange = [&](int z, double& x0, double& y0, double& x1, double& y1) {
        lonLatToTile(minLon, maxLat, z, x0, y0);
        lonLatToTile(maxLon, minLat, z, x1, y1);
        const double n = std::ldexp(1.0, z) - 1;
        x0 = std::clamp(std::floor(x0), 0.0, n);
        y0 = std::clamp(std::floor(y0), 0.0, n);
        x1 = std::clamp(std::floor(x1), 0.0, n);
        y1 = std::clamp(std::floor(y1), 0.0, n);
        return (x1 - x0 + 1) * (y1 - y0 + 1);
    };
    // every zoom from the pack's first, as deep as the tile budget allows: small places and shops
    // only show up in deeper tiles, big ones are kept from the shallow ones
    int deepest = h.maxZoom;
    while (deepest > h.minZoom) {
        double total = 0, x0, y0, x1, y1;
        for (int z = h.minZoom; z <= deepest; ++z) total += tileRange(z, x0, y0, x1, y1);
        if (total <= static_cast<double>(kMaxTiles)) break;
        --deepest;
    }
    std::set<std::string> seen;  // name + rough spot: one place repeated across zooms and tile buffers
    for (int zoom = h.minZoom; zoom <= deepest; ++zoom) {
        double x0, y0, x1, y1;
        tileRange(zoom, x0, y0, x1, y1);
        for (auto ty = static_cast<std::uint32_t>(y0); ty <= static_cast<std::uint32_t>(y1); ++ty) {
            for (auto tx = static_cast<std::uint32_t>(x0); tx <= static_cast<std::uint32_t>(x1); ++tx) {
                std::optional<std::string> bytes;
                std::vector<MvtLayer> layers;
                try {
                    bytes = reader->tile(zoom, tx, ty);
                    if (!bytes) continue;
                    layers = decodeMvt(*bytes);
                } catch (const std::exception&) {
                    continue;  // one broken tile doesn't stop the rest
                }
                for (const auto& l : layers) {
                    if (!placeLayer(l.name)) continue;
                    for (const auto& f : l.features) {
                        std::string name = text(f.props, "name");
                        if (name.empty()) name = text(f.props, "name:en");
                        if (name.empty() || f.geom.empty()) continue;
                        const Coord at = f.geom.type == GeomType::Point && !f.geom.points.empty() ? f.geom.points[0] : labelPoint(f.geom);
                        if (at.x < 0 || at.y < 0 || at.x > l.extent || at.y > l.extent) continue;  // in the buffer: its own tile has it
                        double lon = 0, lat = 0;
                        tileToLonLat(tx + at.x / l.extent, ty + at.y / l.extent, zoom, lon, lat);
                        std::string kind = text(f.props, "kind");
                        if (kind.empty()) kind = text(f.props, "pmap:kind");
                        if (kind.empty()) kind = text(f.props, "class");
                        char key[64];
                        std::snprintf(key, sizeof key, "|%.1f|%.1f", lon, lat);
                        if (!seen.insert(name + key).second) continue;
                        Feature p;
                        p.geom.type = GeomType::Point;
                        p.geom.points.push_back({lon, lat});
                        p.props["name"] = Prop::string(clean(name));
                        if (!kind.empty()) p.props["kind"] = Prop::string(clean(kind));
                        out.push_back(std::move(p));
                    }
                }
            }
        }
    }
    // kept for next time (best effort: a read-only cache just means reading the pack again)
    fs::create_directories(cached.parent_path(), ec);
    std::ofstream file(cached, std::ios::binary);
    for (const auto& f : out) {
        char coords[64];
        std::snprintf(coords, sizeof coords, "%.6f\t%.6f", f.geom.points[0].x, f.geom.points[0].y);
        file << text(f.props, "name") << '\t' << text(f.props, "kind") << '\t' << coords << '\n';
    }
    return out;
}

std::shared_ptr<const std::vector<Feature>> allPackPlaces() {
    static std::mutex m;
    static std::string key;
    static std::shared_ptr<const std::vector<Feature>> cached = std::make_shared<std::vector<Feature>>();
    std::string now;
    const auto packs = findMapPacks();
    std::error_code ec;
    for (const auto& p : packs) now += p.string() + "|" + std::to_string(fs::file_size(p, ec)) + ";";
    std::lock_guard<std::mutex> g(m);
    if (now == key) return cached;
    auto all = std::make_shared<std::vector<Feature>>();
    for (const auto& p : packs) {
        try {
            auto part = packPlaces(p);
            all->insert(all->end(), std::make_move_iterator(part.begin()), std::make_move_iterator(part.end()));
        } catch (const std::exception&) {
            // a half-copied pack: skip it
        }
    }
    key = now;
    cached = all;
    return cached;
}

}  // namespace wetwaresuite::maps
