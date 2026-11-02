// levels: a headless game level and tilemap editor. Maps have a tile size, a
// width and height in cells, tile layers, object layers (entities with a type,
// position, size and custom properties) and tilesets cut from images. Cells
// hold Tiled-style global tile ids (gids): 0 is empty, tileset T's local tile
// n is T.firstGid + n. Files: Tiled TMX/TMJ (and .world files), a plain JSON
// format, LDtk-lite JSON, Godot 4 scenes (.tscn, export only), CSV per layer and
// flattened PNG renders.
#pragma once

#include <algorithm>
#include <array>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "wetwaresuite/core/command.hpp"
#include "wetwaresuite/raster/image.hpp"

namespace wetwaresuite::levels {

using raster::Color;
using raster::Image;

// Tiled keeps flip flags in the top bits of a gid.
constexpr std::uint32_t kFlipH = 0x80000000u;
constexpr std::uint32_t kFlipV = 0x40000000u;
constexpr std::uint32_t kFlipD = 0x20000000u;
constexpr std::uint32_t kGidMask = 0x0FFFFFFFu;

// A custom property: type is string, int, float, bool, color or file (as in Tiled).
struct Property {
    std::string type = "string";
    std::string value;
    bool operator==(const Property&) const = default;
};
using Properties = std::map<std::string, Property>;

// "true" -> bool, "12" -> int, "1.5" -> float, "#ff0000" -> color, anything else -> string.
Property inferProperty(const std::string& value);

struct ObjPoint {
    double x = 0, y = 0;
    bool operator==(const ObjPoint&) const = default;
};

struct MapObject {
    int id = 0;
    std::string name, type;
    double x = 0, y = 0, width = 0, height = 0;  // pixels, top-left origin (polygons: x,y is the first point's origin)
    double rotation = 0;                         // degrees clockwise around (x, y), as in Tiled
    std::string shape = "rect";                  // rect, ellipse, point, polygon or polyline
    std::vector<ObjPoint> points;                // polygon/polyline points relative to (x, y)
    std::uint32_t gid = 0;                       // a tile object: draws this tile in its rect
    Properties props;
};

// One frame of an animated tile (Tiled's <animation>): a local tile id shown for duration ms.
struct TileFrame {
    int tile = 0;
    int duration = 100;
    bool operator==(const TileFrame&) const = default;
};

// A Tiled Wang set (terrain set). type is corner, edge or mixed. Each tile's
// wangid lists the colour at top, top-right, right, bottom-right, bottom,
// bottom-left, left, top-left (0 = unset); colours are numbered from 1.
struct WangColor {
    std::string name;
    std::string color = "#ff0000";
    int tile = -1;
    double probability = 1;
};
struct WangSet {
    std::string name;
    std::string type = "corner";
    int tile = -1;
    std::vector<WangColor> colors;
    std::map<int, std::array<int, 8>> tiles;  // local tile id -> wangid
};

struct Tileset {
    std::string name;
    std::uint32_t firstGid = 1;
    int tileWidth = 16, tileHeight = 16;
    int margin = 0, spacing = 0;
    int columns = 0, tileCount = 0;
    std::string image;  // absolute path of the source image; empty for a colour-only tileset
    int imageWidth = 0, imageHeight = 0;
    std::shared_ptr<const Image> pixels;  // null when the image is missing (tiles render as colours)
    std::map<int, Properties> tileProps;  // per local tile id, e.g. solid=true
    std::map<int, std::vector<TileFrame>> animations;    // per local tile id
    std::map<int, std::vector<MapObject>> tileShapes;    // collision shapes per local tile id (px inside the tile)
    std::vector<WangSet> wangsets;

    bool contains(std::uint32_t gid) const {
        gid &= kGidMask;
        return gid >= firstGid && gid < firstGid + static_cast<std::uint32_t>(tileCount);
    }
    // Top-left pixel of local tile `id` in the image.
    int tileX(int id) const { return margin + (id % std::max(1, columns)) * (tileWidth + spacing); }
    int tileY(int id) const { return margin + (id / std::max(1, columns)) * (tileHeight + spacing); }
};

enum class LayerKind { Tiles, Objects, Image };

struct Layer {
    int id = 0;
    std::string name;
    LayerKind kind = LayerKind::Tiles;
    bool visible = true;
    double opacity = 1.0;
    bool collision = false;            // a collision grid: drawn as an overlay, cells are 0/1
    std::vector<std::uint32_t> cells;  // width * height gids, row by row (tile layers)
    std::vector<MapObject> objects;    // object layers
    Properties props;
    double offsetX = 0, offsetY = 0;      // px the layer is drawn shifted by
    double parallaxX = 1, parallaxY = 1;  // scroll speed relative to the camera (kept for engines)
    std::string tint;                     // "#rrggbb" multiplied in when drawn; empty = none
    // image layers
    std::string image;  // absolute path
    bool repeatX = false, repeatY = false;
    std::shared_ptr<const Image> pixels;  // null when the image is missing
};

struct Map {
    int width = 0, height = 0;          // cells
    int tileWidth = 16, tileHeight = 16;  // px
    std::vector<Tileset> tilesets;      // ordered by firstGid
    std::vector<Layer> layers;          // bottom first
    Properties props;
    std::string background;             // "#rrggbb", empty = transparent
    int nextLayerId = 1, nextObjectId = 1;
    // Tiled's map shapes: "orthogonal" (square grid), "isometric" (diamonds), "staggered"
    // (diamonds in shifted rows) or "hexagonal" (hexagons in shifted rows or columns).
    std::string orientation = "orthogonal";
    std::string staggerAxis = "y";      // staggered/hexagonal: rows ("y") or columns ("x") shift
    std::string staggerIndex = "odd";   // which of them shift: "odd" or "even"
    int hexSideLength = 0;              // hexagonal: length of the flat side, px

    bool inside(int x, int y) const { return x >= 0 && y >= 0 && x < width && y < height; }
    std::uint32_t at(const Layer& l, int x, int y) const { return l.cells[static_cast<std::size_t>(y) * width + x]; }
    void set(Layer& l, int x, int y, std::uint32_t gid) const { l.cells[static_cast<std::size_t>(y) * width + x] = gid; }

    // Layer by name, or by index ("0" = bottom); null when missing.
    Layer* layer(const std::string& nameOrIndex);
    const Layer* layer(const std::string& nameOrIndex) const;
    int layerIndex(const std::string& nameOrIndex) const;  // -1 when missing
    Layer& addLayer(const std::string& name, LayerKind kind, int index = -1);
    const Tileset* tilesetFor(std::uint32_t gid) const;
    Tileset* tileset(const std::string& nameOrIndex);
    const Tileset* tileset(const std::string& nameOrIndex) const;
    MapObject* object(int id, Layer** owner = nullptr);
    std::size_t objectCount() const;
    // The next free gid after the last tileset.
    std::uint32_t nextGid() const;
    // Properties of the tile a gid shows (null when it has none).
    const Properties* tileProperties(std::uint32_t gid) const;
};

Map newMap(int width, int height, int tileWidth, int tileHeight);

// Loads an image layer's picture (pixels stay null when the file is missing).
void attachLayerImage(Layer& layer);

// Slices an image into tiles. tileWidth/Height 0 = the map's tile size. Throws std::runtime_error.
Tileset loadTileset(const std::filesystem::path& image, int tileWidth, int tileHeight, int margin, int spacing,
                    const std::string& name = {});
// Appends a tileset, giving it the next free firstGid. Returns it.
Tileset& addTileset(Map& map, Tileset tileset);

// ---- painting (return the number of cells changed) ----

struct CellRect {
    int x = 0, y = 0, w = 0, h = 0;
};
CellRect clip(const Map& map, CellRect r);

int fillRect(const Map& map, Layer& layer, CellRect r, std::uint32_t gid, bool outline = false);
int floodFill(const Map& map, Layer& layer, int x, int y, std::uint32_t gid, bool diagonal = false);
int drawLine(const Map& map, Layer& layer, int x1, int y1, int x2, int y2, std::uint32_t gid);
// Copies `src` (a w x h block of gids, row by row) to (x, y); 0s are skipped when skipEmpty.
int stamp(const Map& map, Layer& layer, int x, int y, int w, int h, const std::vector<std::uint32_t>& src, bool skipEmpty);
// Cells of `from` in a rect, row by row (outside cells are 0).
std::vector<std::uint32_t> copyRegion(const Map& map, const Layer& from, CellRect r);

// 4-bit auto-tiling: every terrain cell of `src` inside `region` becomes
// base + mask in `dst`, mask = N 1 | E 2 | S 4 | W 8 for terrain neighbours.
// edgesSolid treats cells beyond the map as terrain.
int autotile(const Map& map, const Layer& src, Layer& dst, const std::function<bool(std::uint32_t)>& isTerrain,
             std::uint32_t base, CellRect region, bool edgesSolid);

// 8-bit "blob" auto-tiling with 47 tiles: corners count only when both sides
// next to them are terrain too. A terrain cell becomes base + blobIndex(mask),
// mask = N 1 | NE 2 | E 4 | SE 8 | S 16 | SW 32 | W 64 | NW 128.
int autotileBlob(const Map& map, const Layer& src, Layer& dst, const std::function<bool(std::uint32_t)>& isTerrain,
                 std::uint32_t base, CellRect region, bool edgesSolid);
// The 47 masks the blob rule produces, in tile order (blobMasks()[i] is tile base + i).
const std::vector<int>& blobMasks();
// Index of a (reduced) 8-bit mask in blobMasks(); -1 if it isn't one.
int blobIndex(int mask);

// Auto-tiling with a Tiled Wang set: every cell of `src` inside `region` that is
// terrain gets the tile of `set` whose wangid best matches its neighbours
// (`color` where the neighbour is terrain, `other` where it isn't; 0 = don't
// care). Ties go to the higher-probability colour, then the first tile. Returns
// cells changed; `unmatched` counts cells that had no exact match.
int autotileWang(const Map& map, const Layer& src, Layer& dst, const std::function<bool(std::uint32_t)>& isTerrain,
                 const Tileset& tileset, const WangSet& set, int color, int other, CellRect region, bool edgesSolid,
                 int* unmatched = nullptr);
// Fills a Wang set's tiles for a 16-tile corner template (tile i has corner
// bits NE 1, SE 2, SW 4, NW 8 set to color, else other) or a 47-tile blob
// template (blobMasks() order) starting at local tile `first`.
void wangTemplate(WangSet& set, const std::string& layout, int first, int color, int other);

// ---- randomness: deterministic on every platform for a given seed ----

class Rng {
public:
    explicit Rng(std::uint64_t seed);
    std::uint64_t next();
    double uniform();                // [0, 1)
    int range(int lo, int hiInclusive);

private:
    std::uint64_t s_;
};

// Random scatter of `gids` (chosen by `weights`, same length or empty) over
// cells in `region` where `allowed` says yes, each with probability density
// (or exactly `count` cells when count > 0).
int scatter(const Map& map, Layer& layer, const std::vector<std::uint32_t>& gids, const std::vector<double>& weights,
            CellRect region, double density, int count, std::uint64_t seed,
            const std::function<bool(int, int)>& allowed);

// ---- procedural generators ----

struct CaveOptions {
    double fill = 0.45;   // starting wall fraction
    int steps = 5;        // smoothing passes (wall if >= 5 of 8 neighbours are walls)
    bool connect = true;  // fill caves not connected to the largest one
    std::uint64_t seed = 1;
};
// Returns a width*height grid: true = wall. Map borders are always walls.
std::vector<bool> caves(int width, int height, const CaveOptions& options);

struct RoomOptions {
    int minRoom = 4, maxRoom = 10;  // room side lengths in cells
    int corridorWidth = 1;
    int padding = 1;                 // wall cells between a room and its BSP leaf edge
    std::uint64_t seed = 1;
};
struct Rooms {
    std::vector<bool> wall;  // width*height, true = wall
    std::vector<CellRect> rooms;
};
Rooms bspRooms(int width, int height, const RoomOptions& options);

struct NoiseOptions {
    double scale = 16;  // feature size in cells
    int octaves = 4;
    double persistence = 0.5;
    std::uint64_t seed = 1;
};
// Fractal value noise in 0..1, width*height values.
std::vector<double> noise(int width, int height, const NoiseOptions& options);

// ---- collision ----

// Solid cells as rectangles: runs along rows, merged down when they line up.
std::vector<CellRect> mergeRects(int width, int height, const std::vector<bool>& solid);

// ---- rendering ----

struct RenderOptions {
    double scale = 1.0;
    int time = 0;  // ms into tile animations (0 = first frames)
    bool grid = false;
    bool objects = true;
    bool collision = true;            // collision layers as a red overlay
    std::vector<std::string> layers;  // only these (names or indexes); empty = all visible
};
Image render(const Map& map, const RenderOptions& options = {});

// Map shapes: where a cell's tile goes and which cell a pixel is in.
bool orthogonal(const Map& map);
// Size of the whole map in px, for any orientation.
std::array<int, 2> mapPixelSize(const Map& map);
// Top-left px of cell (x, y)'s tileWidth x tileHeight footprint.
std::array<double, 2> cellOrigin(const Map& map, int x, int y);
// The cell's outline in px (4 points for squares and diamonds, 6 for hexagons).
std::vector<ObjPoint> cellOutline(const Map& map, int x, int y);
// The cell under map pixel (px, py); may be outside the map.
std::array<int, 2> cellAt(const Map& map, double px, double py);
// Neighbouring cells sharing an edge (4 for squares and diamonds, 6 for hexagons).
std::vector<std::array<int, 2>> cellNeighbours(const Map& map, int x, int y);
// An object's outline in map px with its rotation applied (rect corners,
// ellipse points, polygon/polyline points; empty for points).
std::vector<ObjPoint> objectWorldPoints(const MapObject& object);
// Colour a gid gets when it has no tileset image to draw from.
Color placeholderColor(std::uint32_t gid);

// Draws a placeholder tileset image: `style` "colors" (one hue per tile),
// "autotile" (16 tiles showing the N/E/S/W connections of the 4-bit rule),
// "blob" (47 tiles in blobMasks() order) or "corner" (16 Wang corner tiles:
// bits NE 1, SE 2, SW 4, NW 8 are terrain, colors[0], the rest ground, colors[1]).
Image placeholderTileset(int tiles, int columns, int tileWidth, int tileHeight, const std::string& style,
                         const std::vector<Color>& colors);

// ---- files ----

// Formats: tmx, tmj, json (plain), ldtk, tscn (Godot 4, save only), csv, png. From the extension; .json files are sniffed.
std::string formatFor(const std::filesystem::path& path);

Map loadMap(const std::filesystem::path& path);  // tmx, tmj, json, ldtk. Throws std::runtime_error.
// Encoding for TMX/TMJ tile data: csv, base64 or zlib.
void saveMap(const Map& map, const std::filesystem::path& path, const std::string& format, const std::string& encoding = "csv",
             double scale = 1.0);

std::string toTmx(const Map& map, const std::filesystem::path& dir, const std::string& encoding = "csv");
std::string toTmj(const Map& map, const std::filesystem::path& dir, const std::string& encoding = "csv");
std::string toPlainJson(const Map& map, const std::filesystem::path& dir);
std::string toLdtk(const Map& map, const std::filesystem::path& dir, std::vector<std::string>* warnings = nullptr);
Map fromTmx(const std::string& text, const std::filesystem::path& dir);
Map fromTmj(const std::string& text, const std::filesystem::path& dir);
Map fromPlainJson(const std::string& text, const std::filesystem::path& dir);
Map fromLdtk(const std::string& text, const std::filesystem::path& dir, const std::string& level = {});
// A Godot 4 scene: a Node2D with a TileMap (one TileMap layer per tile layer,
// a TileSet with one atlas source per tileset image and physics for tiles with
// collision shapes or solid=true), Sprite2D image layers, StaticBody2D
// collision from collision layers, and Marker2D/Area2D nodes for objects with
// their properties as metadata. Paths are relative to `dir`.
std::string toGodotScene(const Map& map, const std::filesystem::path& dir, const std::string& name = "Level",
                         std::vector<std::string>* warnings = nullptr);

// Tiled .world files: maps placed at pixel positions.
struct WorldMap {
    std::string file;  // absolute path
    int x = 0, y = 0;
    int width = 0, height = 0;  // px (0 = unknown)
};
std::string toWorldJson(const std::vector<WorldMap>& maps, const std::filesystem::path& dir);
std::vector<WorldMap> fromWorldJson(const std::string& text, const std::filesystem::path& dir);

// One tile layer as CSV rows. local=true writes tileset-local ids with -1 for
// empty (what most engines' CSV importers want); false writes raw gids, 0 empty.
std::string layerCsv(const Map& map, const Layer& layer, bool local = true);
// Reads CSV rows into a width*height gid grid (cells beyond the CSV stay 0).
std::vector<std::uint32_t> parseCsv(const Map& map, const std::string& text, bool local, int& rows, int& cols);

// ---- documents and commands ----

class LevelDoc : public Document {
public:
    Map map;
    std::string kind() const override { return "level"; }
    std::string summary() const override;
    std::unique_ptr<Attachment> preview() const override;
};

// Undoable edit: snapshots the map before `fn`, restores it on undo or failure.
class LevelEdit : public Action {
public:
    LevelEdit(std::shared_ptr<LevelDoc> doc, std::function<Result(LevelDoc&)> fn) : doc_(std::move(doc)), fn_(std::move(fn)) {}
    Result apply() override;
    void revert() override;

private:
    std::shared_ptr<LevelDoc> doc_;
    std::function<Result(LevelDoc&)> fn_;
    Map before_;
};

// A Tiled world: several levels placed side by side. Entries name open level
// documents (doc ids) or files.
class WorldDoc : public Document {
public:
    struct Entry {
        std::string doc;   // open level id, empty when only a file
        std::string file;  // where the level lives / will be saved
        int x = 0, y = 0;  // px
    };
    std::vector<Entry> entries;
    std::string kind() const override { return "world"; }
    std::string summary() const override;
};

// Registers every levels.* command.
void registerCommands(Session& session);

}  // namespace wetwaresuite::levels
