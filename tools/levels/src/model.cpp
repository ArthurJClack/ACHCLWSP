// The map model, painting, generators and rendering.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <deque>
#include <stdexcept>

#include "wetwaresuite/levels/levels.hpp"
#include "wetwaresuite/raster/codec.hpp"
#include "wetwaresuite/raster/ops.hpp"
#include "wetwaresuite/raster/paint.hpp"

namespace wetwaresuite::levels {

namespace {

bool parseIndex(const std::string& s, int& out) {
    if (s.empty() || s.size() > 6) return false;
    for (char c : s) {
        if (c < '0' || c > '9') return false;
    }
    out = std::stoi(s);
    return true;
}

}  // namespace

Property inferProperty(const std::string& value) {
    if (value == "true" || value == "false") return {"bool", value};
    if (!value.empty()) {
        std::size_t i = (value[0] == '-' || value[0] == '+') ? 1 : 0;
        bool digits = i < value.size(), dot = false;
        for (std::size_t k = i; k < value.size(); ++k) {
            if (value[k] == '.' && !dot) dot = true;
            else if (value[k] < '0' || value[k] > '9') digits = false;
        }
        if (digits && value.back() != '.') return {dot ? "float" : "int", value};
        if (value[0] == '#' && (value.size() == 7 || value.size() == 9) && Color::parse(value)) return {"color", value};
    }
    return {"string", value};
}

// ---- Map ----------------------------------------------------------------------

int Map::layerIndex(const std::string& key) const {
    for (std::size_t i = 0; i < layers.size(); ++i) {
        if (layers[i].name == key) return static_cast<int>(i);
    }
    int idx = 0;
    if (parseIndex(key, idx) && idx < static_cast<int>(layers.size())) return idx;
    return -1;
}

Layer* Map::layer(const std::string& key) {
    int i = layerIndex(key);
    return i < 0 ? nullptr : &layers[static_cast<std::size_t>(i)];
}

const Layer* Map::layer(const std::string& key) const {
    int i = layerIndex(key);
    return i < 0 ? nullptr : &layers[static_cast<std::size_t>(i)];
}

Layer& Map::addLayer(const std::string& name, LayerKind kind, int index) {
    Layer l;
    l.id = nextLayerId++;
    l.name = name;
    l.kind = kind;
    if (kind == LayerKind::Tiles) l.cells.assign(static_cast<std::size_t>(width) * height, 0);
    if (index < 0 || index > static_cast<int>(layers.size())) index = static_cast<int>(layers.size());
    return *layers.insert(layers.begin() + index, std::move(l));
}

const Tileset* Map::tilesetFor(std::uint32_t gid) const {
    gid &= kGidMask;
    if (gid == 0) return nullptr;
    const Tileset* best = nullptr;
    for (const auto& t : tilesets) {
        if (t.firstGid <= gid && (!best || t.firstGid > best->firstGid)) best = &t;
    }
    return best && best->contains(gid) ? best : nullptr;
}

Tileset* Map::tileset(const std::string& key) {
    for (auto& t : tilesets) {
        if (t.name == key) return &t;
    }
    int idx = 0;
    if (parseIndex(key, idx) && idx < static_cast<int>(tilesets.size())) return &tilesets[static_cast<std::size_t>(idx)];
    return nullptr;
}

const Tileset* Map::tileset(const std::string& key) const { return const_cast<Map*>(this)->tileset(key); }

MapObject* Map::object(int id, Layer** owner) {
    for (auto& l : layers) {
        for (auto& o : l.objects) {
            if (o.id == id) {
                if (owner) *owner = &l;
                return &o;
            }
        }
    }
    return nullptr;
}

std::size_t Map::objectCount() const {
    std::size_t n = 0;
    for (const auto& l : layers) n += l.objects.size();
    return n;
}

std::uint32_t Map::nextGid() const {
    std::uint32_t g = 1;
    for (const auto& t : tilesets) g = std::max(g, t.firstGid + static_cast<std::uint32_t>(t.tileCount));
    return g;
}

const Properties* Map::tileProperties(std::uint32_t gid) const {
    const Tileset* t = tilesetFor(gid);
    if (!t) return nullptr;
    auto it = t->tileProps.find(static_cast<int>((gid & kGidMask) - t->firstGid));
    return it == t->tileProps.end() ? nullptr : &it->second;
}

Map newMap(int width, int height, int tileWidth, int tileHeight) {
    Map m;
    m.width = width;
    m.height = height;
    m.tileWidth = tileWidth;
    m.tileHeight = tileHeight;
    return m;
}

Tileset loadTileset(const std::filesystem::path& image, int tileWidth, int tileHeight, int margin, int spacing, const std::string& name) {
    if (tileWidth <= 0 || tileHeight <= 0) throw std::runtime_error("tile size must be positive");
    Tileset t;
    t.name = name.empty() ? image.stem().string() : name;
    t.tileWidth = tileWidth;
    t.tileHeight = tileHeight;
    t.margin = std::max(0, margin);
    t.spacing = std::max(0, spacing);
    std::error_code ec;
    auto abs = std::filesystem::absolute(image, ec);
    t.image = (ec ? image : abs).lexically_normal().string();
    auto img = std::make_shared<Image>(raster::load(image));
    t.imageWidth = img->width();
    t.imageHeight = img->height();
    t.columns = (t.imageWidth - 2 * t.margin + t.spacing) / (tileWidth + t.spacing);
    const int rows = (t.imageHeight - 2 * t.margin + t.spacing) / (tileHeight + t.spacing);
    if (t.columns < 1 || rows < 1) {
        throw std::runtime_error(image.string() + " (" + std::to_string(t.imageWidth) + "x" + std::to_string(t.imageHeight) +
                                 ") is smaller than one " + std::to_string(tileWidth) + "x" + std::to_string(tileHeight) + " tile");
    }
    t.tileCount = t.columns * rows;
    t.pixels = std::move(img);
    return t;
}

void attachLayerImage(Layer& layer) {
    layer.pixels.reset();
    if (layer.image.empty()) return;
    std::error_code ec;
    if (!std::filesystem::exists(layer.image, ec)) return;
    try {
        layer.pixels = std::make_shared<Image>(raster::load(layer.image));
    } catch (const std::exception&) {
        layer.pixels.reset();
    }
}

Tileset& addTileset(Map& map, Tileset tileset) {
    tileset.firstGid = map.nextGid();
    map.tilesets.push_back(std::move(tileset));
    return map.tilesets.back();
}

// ---- painting -----------------------------------------------------------------

CellRect clip(const Map& map, CellRect r) {
    int x0 = std::max(0, r.x), y0 = std::max(0, r.y);
    int x1 = std::min(map.width, r.x + r.w), y1 = std::min(map.height, r.y + r.h);
    return {x0, y0, std::max(0, x1 - x0), std::max(0, y1 - y0)};
}

int fillRect(const Map& map, Layer& layer, CellRect r, std::uint32_t gid, bool outline) {
    const CellRect c = clip(map, r);
    int n = 0;
    for (int y = c.y; y < c.y + c.h; ++y) {
        for (int x = c.x; x < c.x + c.w; ++x) {
            if (outline && x != r.x && y != r.y && x != r.x + r.w - 1 && y != r.y + r.h - 1) continue;
            if (map.at(layer, x, y) != gid) {
                map.set(layer, x, y, gid);
                ++n;
            }
        }
    }
    return n;
}

int floodFill(const Map& map, Layer& layer, int x, int y, std::uint32_t gid, bool diagonal) {
    if (!map.inside(x, y)) return 0;
    const std::uint32_t target = map.at(layer, x, y);
    if (target == gid) return 0;
    std::deque<std::pair<int, int>> q{{x, y}};
    map.set(layer, x, y, gid);
    int n = 1;
    while (!q.empty()) {
        auto [cx, cy] = q.front();
        q.pop_front();
        for (int dy = -1; dy <= 1; ++dy) {
            for (int dx = -1; dx <= 1; ++dx) {
                if ((dx == 0 && dy == 0) || (!diagonal && dx != 0 && dy != 0)) continue;
                const int nx = cx + dx, ny = cy + dy;
                if (!map.inside(nx, ny) || map.at(layer, nx, ny) != target) continue;
                map.set(layer, nx, ny, gid);
                ++n;
                q.emplace_back(nx, ny);
            }
        }
    }
    return n;
}

int drawLine(const Map& map, Layer& layer, int x1, int y1, int x2, int y2, std::uint32_t gid) {
    int dx = std::abs(x2 - x1), dy = -std::abs(y2 - y1);
    int sx = x1 < x2 ? 1 : -1, sy = y1 < y2 ? 1 : -1, err = dx + dy, n = 0;
    while (true) {
        if (map.inside(x1, y1) && map.at(layer, x1, y1) != gid) {
            map.set(layer, x1, y1, gid);
            ++n;
        }
        if (x1 == x2 && y1 == y2) break;
        const int e2 = 2 * err;
        if (e2 >= dy) {
            err += dy;
            x1 += sx;
        }
        if (e2 <= dx) {
            err += dx;
            y1 += sy;
        }
    }
    return n;
}

int stamp(const Map& map, Layer& layer, int x, int y, int w, int h, const std::vector<std::uint32_t>& src, bool skipEmpty) {
    int n = 0;
    for (int j = 0; j < h; ++j) {
        for (int i = 0; i < w; ++i) {
            const std::uint32_t g = src[static_cast<std::size_t>(j) * w + i];
            if ((skipEmpty && g == 0) || !map.inside(x + i, y + j)) continue;
            if (map.at(layer, x + i, y + j) != g) {
                map.set(layer, x + i, y + j, g);
                ++n;
            }
        }
    }
    return n;
}

std::vector<std::uint32_t> copyRegion(const Map& map, const Layer& from, CellRect r) {
    std::vector<std::uint32_t> out(static_cast<std::size_t>(std::max(0, r.w)) * std::max(0, r.h), 0);
    for (int j = 0; j < r.h; ++j) {
        for (int i = 0; i < r.w; ++i) {
            if (map.inside(r.x + i, r.y + j)) out[static_cast<std::size_t>(j) * r.w + i] = map.at(from, r.x + i, r.y + j);
        }
    }
    return out;
}

int autotile(const Map& map, const Layer& src, Layer& dst, const std::function<bool(std::uint32_t)>& isTerrain, std::uint32_t base,
             CellRect region, bool edgesSolid) {
    const CellRect c = clip(map, region);
    const std::vector<std::uint32_t> before = src.cells;  // src and dst may be the same layer
    auto terrain = [&](int x, int y) {
        if (!map.inside(x, y)) return edgesSolid;
        return isTerrain(before[static_cast<std::size_t>(y) * map.width + x]);
    };
    int n = 0;
    for (int y = c.y; y < c.y + c.h; ++y) {
        for (int x = c.x; x < c.x + c.w; ++x) {
            if (!terrain(x, y)) continue;
            const std::uint32_t mask = (terrain(x, y - 1) ? 1u : 0u) | (terrain(x + 1, y) ? 2u : 0u) | (terrain(x, y + 1) ? 4u : 0u) |
                                       (terrain(x - 1, y) ? 8u : 0u);
            const std::uint32_t g = base + mask;
            if (map.at(dst, x, y) != g) {
                map.set(dst, x, y, g);
                ++n;
            }
        }
    }
    return n;
}

namespace {

// Bits N 1, NE 2, E 4, SE 8, S 16, SW 32, W 64, NW 128 for terrain neighbours.
int neighbourMask(const std::function<bool(int, int)>& terrain, int x, int y) {
    static const int dx[8] = {0, 1, 1, 1, 0, -1, -1, -1}, dy[8] = {-1, -1, 0, 1, 1, 1, 0, -1};
    int m = 0;
    for (int k = 0; k < 8; ++k) {
        if (terrain(x + dx[k], y + dy[k])) m |= 1 << k;
    }
    return m;
}

// Drops corner bits whose two neighbouring sides aren't both set.
int reduceBlob(int m) {
    const int n = 1, ne = 2, e = 4, se = 8, s = 16, sw = 32, w = 64, nw = 128;
    if (!((m & n) && (m & e))) m &= ~ne;
    if (!((m & s) && (m & e))) m &= ~se;
    if (!((m & s) && (m & w))) m &= ~sw;
    if (!((m & n) && (m & w))) m &= ~nw;
    return m;
}

}  // namespace

const std::vector<int>& blobMasks() {
    static const std::vector<int> masks = [] {
        std::vector<int> out;
        for (int m = 0; m < 256; ++m) {
            if (reduceBlob(m) == m) out.push_back(m);
        }
        return out;
    }();
    return masks;
}

int blobIndex(int mask) {
    const auto& ms = blobMasks();
    auto it = std::lower_bound(ms.begin(), ms.end(), mask);
    return it != ms.end() && *it == mask ? static_cast<int>(it - ms.begin()) : -1;
}

int autotileBlob(const Map& map, const Layer& src, Layer& dst, const std::function<bool(std::uint32_t)>& isTerrain, std::uint32_t base,
                 CellRect region, bool edgesSolid) {
    const CellRect c = clip(map, region);
    const std::vector<std::uint32_t> before = src.cells;
    std::function<bool(int, int)> terrain = [&](int x, int y) {
        if (!map.inside(x, y)) return edgesSolid;
        return isTerrain(before[static_cast<std::size_t>(y) * map.width + x]);
    };
    int n = 0;
    for (int y = c.y; y < c.y + c.h; ++y) {
        for (int x = c.x; x < c.x + c.w; ++x) {
            if (!terrain(x, y)) continue;
            const std::uint32_t g = base + static_cast<std::uint32_t>(blobIndex(reduceBlob(neighbourMask(terrain, x, y))));
            if (map.at(dst, x, y) != g) {
                map.set(dst, x, y, g);
                ++n;
            }
        }
    }
    return n;
}

int autotileWang(const Map& map, const Layer& src, Layer& dst, const std::function<bool(std::uint32_t)>& isTerrain, const Tileset& tileset,
                 const WangSet& set, int color, int other, CellRect region, bool edgesSolid, int* unmatched) {
    const CellRect c = clip(map, region);
    const std::vector<std::uint32_t> before = src.cells;
    std::function<bool(int, int)> terrain = [&](int x, int y) {
        if (!map.inside(x, y)) return edgesSolid;
        return isTerrain(before[static_cast<std::size_t>(y) * map.width + x]);
    };
    const bool useEdges = set.type != "corner", useCorners = set.type != "edge";
    auto probability = [&](int col) {
        return col >= 1 && col <= static_cast<int>(set.colors.size()) ? set.colors[static_cast<std::size_t>(col - 1)].probability : 1.0;
    };
    int n = 0, misses = 0;
    for (int y = c.y; y < c.y + c.h; ++y) {
        for (int x = c.x; x < c.x + c.w; ++x) {
            if (!terrain(x, y)) continue;
            const int m = reduceBlob(neighbourMask(terrain, x, y));
            std::array<int, 8> want{};
            for (int k = 0; k < 8; ++k) {
                const bool corner = k % 2 == 1;
                if ((corner && !useCorners) || (!corner && !useEdges)) continue;
                want[static_cast<std::size_t>(k)] = (m >> k & 1) ? color : other;
            }
            int bestTile = -1, bestMiss = 1 << 30;
            double bestProb = -1;
            for (const auto& [tile, id] : set.tiles) {
                int miss = 0;
                double prob = 1;
                for (std::size_t k = 0; k < 8; ++k) {
                    if (!want[k]) continue;
                    if (id[k] != want[k]) miss += id[k] == 0 ? 1 : 2;
                    else prob *= probability(id[k]);
                }
                if (miss < bestMiss || (miss == bestMiss && prob > bestProb)) {
                    bestMiss = miss;
                    bestProb = prob;
                    bestTile = tile;
                }
            }
            if (bestTile < 0 || bestTile >= tileset.tileCount) continue;
            if (bestMiss > 0) ++misses;
            const std::uint32_t g = tileset.firstGid + static_cast<std::uint32_t>(bestTile);
            if (map.at(dst, x, y) != g) {
                map.set(dst, x, y, g);
                ++n;
            }
        }
    }
    if (unmatched) *unmatched = misses;
    return n;
}

void wangTemplate(WangSet& set, const std::string& layout, int first, int color, int other) {
    if (layout == "blob47") {
        set.type = "mixed";
        const auto& ms = blobMasks();
        for (std::size_t i = 0; i < ms.size(); ++i) {
            std::array<int, 8> id{};
            for (int k = 0; k < 8; ++k) id[static_cast<std::size_t>(k)] = (ms[i] >> k & 1) ? color : other;
            set.tiles[first + static_cast<int>(i)] = id;
        }
    } else if (layout == "edge16") {
        set.type = "edge";
        for (int i = 0; i < 16; ++i) {
            std::array<int, 8> id{};
            for (int b = 0; b < 4; ++b) id[static_cast<std::size_t>(b * 2)] = (i >> b & 1) ? color : other;
            set.tiles[first + i] = id;
        }
    } else {  // corner16
        set.type = "corner";
        for (int i = 0; i < 16; ++i) {
            std::array<int, 8> id{};
            for (int b = 0; b < 4; ++b) id[static_cast<std::size_t>(b * 2 + 1)] = (i >> b & 1) ? color : other;
            set.tiles[first + i] = id;
        }
    }
}

// ---- randomness ---------------------------------------------------------------

Rng::Rng(std::uint64_t seed) : s_(seed) {
    s_ = next() ^ 0x632BE59BD9B4E019ull;  // scramble so neighbouring seeds give unrelated streams
}

std::uint64_t Rng::next() {  // splitmix64
    std::uint64_t z = (s_ += 0x9E3779B97F4A7C15ull);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
}

double Rng::uniform() { return static_cast<double>(next() >> 11) * (1.0 / 9007199254740992.0); }

int Rng::range(int lo, int hi) {
    if (hi <= lo) return lo;
    return lo + static_cast<int>(next() % static_cast<std::uint64_t>(hi - lo + 1));
}

int scatter(const Map& map, Layer& layer, const std::vector<std::uint32_t>& gids, const std::vector<double>& weights, CellRect region,
            double density, int count, std::uint64_t seed, const std::function<bool(int, int)>& allowed) {
    if (gids.empty()) return 0;
    Rng rng(seed);
    double total = 0;
    for (std::size_t i = 0; i < gids.size(); ++i) total += i < weights.size() ? std::max(0.0, weights[i]) : 1.0;
    auto pick = [&]() {
        double r = rng.uniform() * total;
        for (std::size_t i = 0; i < gids.size(); ++i) {
            r -= i < weights.size() ? std::max(0.0, weights[i]) : 1.0;
            if (r < 0) return gids[i];
        }
        return gids.back();
    };
    const CellRect c = clip(map, region);
    std::vector<std::pair<int, int>> cells;
    for (int y = c.y; y < c.y + c.h; ++y) {
        for (int x = c.x; x < c.x + c.w; ++x) {
            if (!allowed || allowed(x, y)) cells.emplace_back(x, y);
        }
    }
    int n = 0;
    if (count > 0) {
        // partial Fisher-Yates: `count` distinct cells
        const int k = std::min<int>(count, static_cast<int>(cells.size()));
        for (int i = 0; i < k; ++i) {
            const int j = rng.range(i, static_cast<int>(cells.size()) - 1);
            std::swap(cells[static_cast<std::size_t>(i)], cells[static_cast<std::size_t>(j)]);
            map.set(layer, cells[static_cast<std::size_t>(i)].first, cells[static_cast<std::size_t>(i)].second, pick());
            ++n;
        }
        return n;
    }
    for (auto [x, y] : cells) {
        if (rng.uniform() < density) {
            map.set(layer, x, y, pick());
            ++n;
        }
    }
    return n;
}

// ---- generators ---------------------------------------------------------------

std::vector<bool> caves(int w, int h, const CaveOptions& o) {
    Rng rng(o.seed);
    auto idx = [w](int x, int y) { return static_cast<std::size_t>(y) * w + x; };
    std::vector<bool> wall(static_cast<std::size_t>(w) * h);
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) wall[idx(x, y)] = x == 0 || y == 0 || x == w - 1 || y == h - 1 || rng.uniform() < o.fill;
    }
    for (int s = 0; s < o.steps; ++s) {
        std::vector<bool> next = wall;
        for (int y = 1; y < h - 1; ++y) {
            for (int x = 1; x < w - 1; ++x) {
                int n = 0;
                for (int dy = -1; dy <= 1; ++dy) {
                    for (int dx = -1; dx <= 1; ++dx) n += (dx || dy) && wall[idx(x + dx, y + dy)];
                }
                next[idx(x, y)] = n > 4 || (n == 4 && wall[idx(x, y)]);
            }
        }
        wall.swap(next);
    }
    if (o.connect) {
        std::vector<int> region(wall.size(), -1);
        int best = -1;
        std::size_t bestSize = 0;
        int id = 0;
        for (int y = 0; y < h; ++y) {
            for (int x = 0; x < w; ++x) {
                if (wall[idx(x, y)] || region[idx(x, y)] >= 0) continue;
                std::size_t size = 0;
                std::deque<std::pair<int, int>> q{{x, y}};
                region[idx(x, y)] = id;
                while (!q.empty()) {
                    auto [cx, cy] = q.front();
                    q.pop_front();
                    ++size;
                    const int d[4][2] = {{1, 0}, {-1, 0}, {0, 1}, {0, -1}};
                    for (auto& dd : d) {
                        const int nx = cx + dd[0], ny = cy + dd[1];
                        if (nx < 0 || ny < 0 || nx >= w || ny >= h || wall[idx(nx, ny)] || region[idx(nx, ny)] >= 0) continue;
                        region[idx(nx, ny)] = id;
                        q.emplace_back(nx, ny);
                    }
                }
                if (size > bestSize) {
                    bestSize = size;
                    best = id;
                }
                ++id;
            }
        }
        for (std::size_t i = 0; i < wall.size(); ++i) {
            if (!wall[i] && region[i] != best) wall[i] = true;
        }
    }
    return wall;
}

namespace {

struct Leaf {
    CellRect r;
    int a = -1, b = -1;  // children
    int room = -1;
};

}  // namespace

Rooms bspRooms(int w, int h, const RoomOptions& o) {
    Rng rng(o.seed);
    Rooms out;
    out.wall.assign(static_cast<std::size_t>(w) * h, true);
    const int minRoom = std::max(2, o.minRoom), maxRoom = std::max(minRoom, o.maxRoom);
    const int pad = std::max(0, o.padding);
    const int minLeaf = minRoom + 2 * pad;
    std::vector<Leaf> leaves{{CellRect{1, 1, w - 2, h - 2}}};
    // split until leaves are small enough
    for (std::size_t i = 0; i < leaves.size(); ++i) {
        CellRect r = leaves[i].r;
        const bool tooBig = r.w > maxRoom + 2 * pad || r.h > maxRoom + 2 * pad;
        if (!tooBig && rng.uniform() < 0.6) continue;
        bool vertical = r.w > r.h ? true : (r.h > r.w ? false : rng.uniform() < 0.5);
        if (r.w >= 1.25 * r.h) vertical = true;
        else if (r.h >= 1.25 * r.w) vertical = false;
        const int len = vertical ? r.w : r.h;
        if (len < 2 * minLeaf) continue;
        const int cut = rng.range(minLeaf, len - minLeaf);
        Leaf a, b;
        if (vertical) {
            a.r = {r.x, r.y, cut, r.h};
            b.r = {r.x + cut, r.y, r.w - cut, r.h};
        } else {
            a.r = {r.x, r.y, r.w, cut};
            b.r = {r.x, r.y + cut, r.w, r.h - cut};
        }
        leaves[i].a = static_cast<int>(leaves.size());
        leaves.push_back(a);
        leaves[i].b = static_cast<int>(leaves.size());
        leaves.push_back(b);
    }
    auto carve = [&](CellRect r) {
        const CellRect c{std::max(1, r.x), std::max(1, r.y), 0, 0};
        const int x1 = std::min(w - 1, r.x + r.w), y1 = std::min(h - 1, r.y + r.h);
        for (int y = c.y; y < y1; ++y) {
            for (int x = c.x; x < x1; ++x) out.wall[static_cast<std::size_t>(y) * w + x] = false;
        }
    };
    for (auto& l : leaves) {
        if (l.a >= 0) continue;
        const int maxW = std::min(maxRoom, l.r.w - 2 * pad), maxH = std::min(maxRoom, l.r.h - 2 * pad);
        if (maxW < minRoom || maxH < minRoom) continue;
        const int rw = rng.range(minRoom, maxW), rh = rng.range(minRoom, maxH);
        const int rx = rng.range(l.r.x + pad, l.r.x + l.r.w - pad - rw), ry = rng.range(l.r.y + pad, l.r.y + l.r.h - pad - rh);
        l.room = static_cast<int>(out.rooms.size());
        out.rooms.push_back({rx, ry, rw, rh});
        carve(out.rooms.back());
    }
    // connect siblings bottom-up with L-shaped corridors between a room of each side
    const int cw = std::max(1, o.corridorWidth);
    std::function<int(int)> anyRoom = [&](int i) -> int {
        const Leaf& l = leaves[static_cast<std::size_t>(i)];
        if (l.room >= 0) return l.room;
        if (l.a < 0) return -1;
        int r = rng.uniform() < 0.5 ? anyRoom(l.a) : anyRoom(l.b);
        if (r < 0) r = anyRoom(l.a);
        if (r < 0) r = anyRoom(l.b);
        return r;
    };
    for (std::size_t i = leaves.size(); i-- > 0;) {
        const Leaf& l = leaves[i];
        if (l.a < 0) continue;
        const int ra = anyRoom(l.a), rb = anyRoom(l.b);
        if (ra < 0 || rb < 0) continue;
        const CellRect A = out.rooms[static_cast<std::size_t>(ra)], B = out.rooms[static_cast<std::size_t>(rb)];
        const int ax = A.x + A.w / 2, ay = A.y + A.h / 2;  // centre to centre keeps corridors short
        const int bx = B.x + B.w / 2, by = B.y + B.h / 2;
        const int off = (cw - 1) / 2;
        if (rng.uniform() < 0.5) {
            carve({std::min(ax, bx) - off, ay - off, std::abs(bx - ax) + cw, cw});
            carve({bx - off, std::min(ay, by) - off, cw, std::abs(by - ay) + cw});
        } else {
            carve({ax - off, std::min(ay, by) - off, cw, std::abs(by - ay) + cw});
            carve({std::min(ax, bx) - off, by - off, std::abs(bx - ax) + cw, cw});
        }
    }
    return out;
}

namespace {

double hashNoise(std::uint64_t seed, int x, int y) {
    std::uint64_t h = seed ^ (static_cast<std::uint64_t>(static_cast<std::uint32_t>(x)) * 0x9E3779B185EBCA87ull) ^
                      (static_cast<std::uint64_t>(static_cast<std::uint32_t>(y)) * 0xC2B2AE3D27D4EB4Full);
    h ^= h >> 33;
    h *= 0xFF51AFD7ED558CCDull;
    h ^= h >> 33;
    h *= 0xC4CEB9FE1A85EC53ull;
    h ^= h >> 33;
    return static_cast<double>(h >> 11) * (1.0 / 9007199254740992.0);
}

double smooth(double t) { return t * t * (3 - 2 * t); }

double valueNoise(std::uint64_t seed, double x, double y) {
    const int x0 = static_cast<int>(std::floor(x)), y0 = static_cast<int>(std::floor(y));
    const double fx = smooth(x - x0), fy = smooth(y - y0);
    const double a = hashNoise(seed, x0, y0), b = hashNoise(seed, x0 + 1, y0);
    const double c = hashNoise(seed, x0, y0 + 1), d = hashNoise(seed, x0 + 1, y0 + 1);
    return (a + (b - a) * fx) * (1 - fy) + (c + (d - c) * fx) * fy;
}

}  // namespace

std::vector<double> noise(int w, int h, const NoiseOptions& o) {
    std::vector<double> out(static_cast<std::size_t>(w) * h);
    const int octaves = std::clamp(o.octaves, 1, 10);
    const double scale = std::max(0.5, o.scale);
    double lo = 1e9, hi = -1e9;
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            double amp = 1, freq = 1 / scale, sum = 0, norm = 0;
            for (int k = 0; k < octaves; ++k) {
                sum += amp * valueNoise(o.seed + static_cast<std::uint64_t>(k) * 1013, x * freq, y * freq);
                norm += amp;
                amp *= o.persistence;
                freq *= 2;
            }
            const double v = sum / norm;
            out[static_cast<std::size_t>(y) * w + x] = v;
            lo = std::min(lo, v);
            hi = std::max(hi, v);
        }
    }
    // stretch to the full 0..1 range so thresholds mean the same at any size
    if (hi - lo > 1e-9) {
        for (auto& v : out) v = (v - lo) / (hi - lo);
    }
    return out;
}

std::vector<CellRect> mergeRects(int w, int h, const std::vector<bool>& solid) {
    std::vector<CellRect> done, open;
    for (int y = 0; y < h; ++y) {
        std::vector<CellRect> runs;
        for (int x = 0; x < w;) {
            if (!solid[static_cast<std::size_t>(y) * w + x]) {
                ++x;
                continue;
            }
            int x1 = x;
            while (x1 < w && solid[static_cast<std::size_t>(y) * w + x1]) ++x1;
            runs.push_back({x, y, x1 - x, 1});
            x = x1;
        }
        std::vector<CellRect> next;
        for (auto& run : runs) {
            auto it = std::find_if(open.begin(), open.end(), [&](const CellRect& o) { return o.x == run.x && o.w == run.w; });
            if (it != open.end()) {
                CellRect grown = *it;
                grown.h += 1;
                next.push_back(grown);
                open.erase(it);
            } else {
                next.push_back(run);
            }
        }
        for (auto& o : open) done.push_back(o);
        open = std::move(next);
    }
    for (auto& o : open) done.push_back(o);
    std::sort(done.begin(), done.end(), [](const CellRect& a, const CellRect& b) { return a.y != b.y ? a.y < b.y : a.x < b.x; });
    return done;
}

// ---- rendering ----------------------------------------------------------------

Color placeholderColor(std::uint32_t gid) {
    gid &= kGidMask;
    // golden-angle hues, two lightness bands
    const double hue = std::fmod(gid * 137.508, 360.0);
    const double s = 0.55, l = (gid % 2) ? 0.55 : 0.42;
    const double c = (1 - std::abs(2 * l - 1)) * s, hp = hue / 60.0, x = c * (1 - std::abs(std::fmod(hp, 2.0) - 1));
    double r = 0, g = 0, b = 0;
    if (hp < 1) r = c, g = x;
    else if (hp < 2) r = x, g = c;
    else if (hp < 3) g = c, b = x;
    else if (hp < 4) g = x, b = c;
    else if (hp < 5) r = x, b = c;
    else r = c, b = x;
    const double m = l - c / 2;
    auto to8 = [](double v) { return static_cast<std::uint8_t>(std::clamp(v, 0.0, 1.0) * 255 + 0.5); };
    return {to8(r + m), to8(g + m), to8(b + m), 255};
}

namespace {

void over(std::uint8_t* d, const std::uint8_t* s, double opacity) {
    const double sa = s[3] / 255.0 * opacity;
    if (sa <= 0) return;
    const double da = d[3] / 255.0, oa = sa + da * (1 - sa);
    for (int c = 0; c < 3; ++c) d[c] = static_cast<std::uint8_t>((s[c] * sa + d[c] * da * (1 - sa)) / oa + 0.5);
    d[3] = static_cast<std::uint8_t>(oa * 255 + 0.5);
}

// Multiplies a colour by a "#rrggbb" tint (straight through when there is none).
struct Tint {
    bool on = false;
    int r = 255, g = 255, b = 255, a = 255;
    explicit Tint(const std::string& text) {
        if (text.empty()) return;
        if (auto c = Color::parse(text)) {
            on = true;
            r = c->r;
            g = c->g;
            b = c->b;
            a = c->a;
        }
    }
    void apply(std::uint8_t* px) const {
        px[0] = static_cast<std::uint8_t>(px[0] * r / 255);
        px[1] = static_cast<std::uint8_t>(px[1] * g / 255);
        px[2] = static_cast<std::uint8_t>(px[2] * b / 255);
        px[3] = static_cast<std::uint8_t>(px[3] * a / 255);
    }
};

// The tile an animated tile shows `time` ms in.
std::uint32_t animatedGid(const Map& map, std::uint32_t raw, int time) {
    const std::uint32_t gid = raw & kGidMask;
    const Tileset* t = map.tilesetFor(gid);
    if (!t || t->animations.empty()) return raw;
    auto it = t->animations.find(static_cast<int>(gid - t->firstGid));
    if (it == t->animations.end() || it->second.empty()) return raw;
    int total = 0;
    for (const auto& f : it->second) total += std::max(1, f.duration);
    int at = time % std::max(1, total);
    for (const auto& f : it->second) {
        at -= std::max(1, f.duration);
        if (at < 0) return (raw & ~kGidMask) | (t->firstGid + static_cast<std::uint32_t>(std::max(0, f.tile)));
    }
    return raw;
}

// Draws a tile with its top-left at (px, py), stretched to w x h (0 = the tile's own size).
void drawTile(Image& out, const Map& map, std::uint32_t raw, int px, int py, double opacity, const Tint& tint, int w = 0, int h = 0) {
    const std::uint32_t gid = raw & kGidMask;
    const Tileset* t = map.tilesetFor(gid);
    if (!t || !t->pixels) {
        const Color c = placeholderColor(gid);
        std::uint8_t fill[4] = {c.r, c.g, c.b, 255};
        std::uint8_t edge[4] = {static_cast<std::uint8_t>(c.r * 3 / 4), static_cast<std::uint8_t>(c.g * 3 / 4), static_cast<std::uint8_t>(c.b * 3 / 4), 255};
        if (tint.on) {
            tint.apply(fill);
            tint.apply(edge);
        }
        const int tw = w > 0 ? w : map.tileWidth, th = h > 0 ? h : map.tileHeight;
        for (int y = 0; y < th; ++y) {
            for (int x = 0; x < tw; ++x) {
                if (!out.contains(px + x, py + y)) continue;
                const bool border = x == 0 || y == 0 || x == tw - 1 || y == th - 1;
                over(out.pixel(px + x, py + y), border ? edge : fill, opacity);
            }
        }
        return;
    }
    const int id = static_cast<int>(gid - t->firstGid);
    const int sx = t->tileX(id), sy = t->tileY(id);
    const int dw = w > 0 ? w : t->tileWidth, dh = h > 0 ? h : t->tileHeight;
    const bool fh = raw & kFlipH, fv = raw & kFlipV, fd = raw & kFlipD;
    for (int y = 0; y < dh; ++y) {
        for (int x = 0; x < dw; ++x) {
            int u = x * t->tileWidth / dw, v = y * t->tileHeight / dh;
            if (fd) std::swap(u, v);
            if (fh) u = t->tileWidth - 1 - u;
            if (fv) v = t->tileHeight - 1 - v;
            if (!t->pixels->contains(sx + u, sy + v) || !out.contains(px + x, py + y)) continue;
            if (tint.on) {
                std::uint8_t c[4];
                std::copy_n(t->pixels->pixel(sx + u, sy + v), 4, c);
                tint.apply(c);
                over(out.pixel(px + x, py + y), c, opacity);
            } else {
                over(out.pixel(px + x, py + y), t->pixels->pixel(sx + u, sy + v), opacity);
            }
        }
    }
}

// A tile in a map cell: Tiled anchors tiles taller than the grid at the bottom-left of the cell.
void drawCellTile(Image& out, const Map& map, std::uint32_t raw, int px, int py, double opacity, const Tint& tint) {
    const Tileset* t = map.tilesetFor(raw & kGidMask);
    const int oy = t && t->pixels ? py + map.tileHeight - t->tileHeight : py;
    drawTile(out, map, raw, px, oy, opacity, tint);
}

void drawImageLayer(Image& out, const Layer& l, const Tint& tint) {
    if (!l.pixels || l.pixels->empty()) return;
    const Image& img = *l.pixels;
    const int ox = static_cast<int>(std::lround(l.offsetX)), oy = static_cast<int>(std::lround(l.offsetY));
    const int iw = img.width(), ih = img.height();
    int x0 = ox, y0 = oy;
    if (l.repeatX) x0 = ((ox % iw) + iw) % iw - iw;
    if (l.repeatY) y0 = ((oy % ih) + ih) % ih - ih;
    const int x1 = l.repeatX ? out.width() : ox + iw, y1 = l.repeatY ? out.height() : oy + ih;
    for (int by = y0; by < y1; by += ih) {
        for (int bx = x0; bx < x1; bx += iw) {
            for (int y = 0; y < ih; ++y) {
                const int dy = by + y;
                if (dy < 0 || dy >= out.height()) continue;
                for (int x = 0; x < iw; ++x) {
                    const int dx = bx + x;
                    if (dx < 0 || dx >= out.width()) continue;
                    std::uint8_t c[4];
                    std::copy_n(img.pixel(x, y), 4, c);
                    if (tint.on) tint.apply(c);
                    over(out.pixel(dx, dy), c, l.opacity);
                }
            }
            if (!l.repeatX) break;
        }
        if (!l.repeatY) break;
    }
}

Color typeColor(const std::string& type) {
    std::uint32_t h = 2166136261u;
    for (unsigned char c : type) h = (h ^ c) * 16777619u;
    Color c = placeholderColor(h % 997 + 1);
    return c;
}

// An object's outline points in map px: rects and polygons turn with their rotation around (x, y).
std::vector<raster::Point> objectOutline(const MapObject& ob) {
    std::vector<raster::Point> pts;
    if (ob.shape == "polygon" || ob.shape == "polyline") {
        for (const auto& p : ob.points) pts.push_back({p.x, p.y});
    } else if (ob.shape == "ellipse") {
        for (const auto& p : raster::ellipsePoints(ob.width / 2, ob.height / 2, ob.width / 2, ob.height / 2)) pts.push_back(p);
    } else {
        pts = {{0, 0}, {ob.width, 0}, {ob.width, ob.height}, {0, ob.height}};
    }
    const double a = ob.rotation * 3.14159265358979323846 / 180, ca = std::cos(a), sa = std::sin(a);
    for (auto& p : pts) p = {ob.x + p.x * ca - p.y * sa, ob.y + p.x * sa + p.y * ca};
    return pts;
}

}  // namespace

std::vector<ObjPoint> objectWorldPoints(const MapObject& ob) {
    std::vector<ObjPoint> out;
    for (const auto& p : objectOutline(ob)) out.push_back({p.x, p.y});
    return out;
}

bool orthogonal(const Map& map) { return map.orientation.empty() || map.orientation == "orthogonal"; }

namespace {
bool shifted(const Map& m, int i) { return (m.staggerIndex == "even") == (((i % 2) + 2) % 2 == 0); }
double hexSide(const Map& m) { return m.orientation == "hexagonal" ? m.hexSideLength : 0; }
}  // namespace

std::array<int, 2> mapPixelSize(const Map& m) {
    const int tw = m.tileWidth, th = m.tileHeight;
    if (m.orientation == "isometric") return {(m.width + m.height) * tw / 2, (m.width + m.height) * th / 2};
    if (m.orientation == "staggered" || m.orientation == "hexagonal") {
        const double s = hexSide(m);
        if (m.staggerAxis == "x") {
            const double colW = (tw + s) / 2;
            return {static_cast<int>(std::ceil(m.width * colW + (tw - colW))), m.height * th + th / 2};
        }
        const double rowH = (th + s) / 2;
        return {m.width * tw + tw / 2, static_cast<int>(std::ceil(m.height * rowH + (th - rowH)))};
    }
    return {m.width * tw, m.height * th};
}

std::array<double, 2> cellOrigin(const Map& m, int x, int y) {
    const double tw = m.tileWidth, th = m.tileHeight;
    if (m.orientation == "isometric") return {(x - y) * tw / 2 + m.height * tw / 2 - tw / 2, (x + y) * th / 2};
    if (m.orientation == "staggered" || m.orientation == "hexagonal") {
        const double s = hexSide(m);
        if (m.staggerAxis == "x") return {x * (tw + s) / 2, y * th + (shifted(m, x) ? th / 2 : 0)};
        return {x * tw + (shifted(m, y) ? tw / 2 : 0), y * (th + s) / 2};
    }
    return {x * tw, y * th};
}

std::vector<ObjPoint> cellOutline(const Map& m, int x, int y) {
    const auto o = cellOrigin(m, x, y);
    const double tw = m.tileWidth, th = m.tileHeight, X = o[0], Y = o[1];
    if (m.orientation == "isometric" || m.orientation == "staggered")
        return {{X + tw / 2, Y}, {X + tw, Y + th / 2}, {X + tw / 2, Y + th}, {X, Y + th / 2}};
    if (m.orientation == "hexagonal") {
        const double s = m.hexSideLength;
        if (m.staggerAxis == "x") {
            const double e = (tw - s) / 2;
            return {{X + e, Y}, {X + e + s, Y}, {X + tw, Y + th / 2}, {X + e + s, Y + th}, {X + e, Y + th}, {X, Y + th / 2}};
        }
        const double e = (th - s) / 2;
        return {{X + tw / 2, Y}, {X + tw, Y + e}, {X + tw, Y + e + s}, {X + tw / 2, Y + th}, {X, Y + e + s}, {X, Y + e}};
    }
    return {{X, Y}, {X + tw, Y}, {X + tw, Y + th}, {X, Y + th}};
}

namespace {
bool insidePolygon(const std::vector<ObjPoint>& poly, double px, double py) {
    bool in = false;
    for (std::size_t i = 0, j = poly.size() - 1; i < poly.size(); j = i++) {
        if ((poly[i].y > py) != (poly[j].y > py) && px < (poly[j].x - poly[i].x) * (py - poly[i].y) / (poly[j].y - poly[i].y) + poly[i].x) in = !in;
    }
    return in;
}
}  // namespace

std::array<int, 2> cellAt(const Map& m, double px, double py) {
    const double tw = m.tileWidth, th = m.tileHeight;
    if (m.orientation == "isometric") {
        const double rx = px - m.height * tw / 2;
        return {static_cast<int>(std::floor(py / th + rx / tw)), static_cast<int>(std::floor(py / th - rx / tw))};
    }
    if (orthogonal(m)) return {static_cast<int>(std::floor(px / tw)), static_cast<int>(std::floor(py / th))};
    // staggered and hexagonal: a rough guess, then the cell around it whose shape holds the point
    const double s = hexSide(m);
    int gx = 0, gy = 0;
    if (m.staggerAxis == "x") gx = static_cast<int>(std::floor(px / ((tw + s) / 2))), gy = static_cast<int>(std::floor(py / th));
    else gx = static_cast<int>(std::floor(px / tw)), gy = static_cast<int>(std::floor(py / ((th + s) / 2)));
    std::array<int, 2> best{gx, gy};
    double bestD = 1e300;
    for (int dy = -2; dy <= 2; ++dy)
        for (int dx = -2; dx <= 2; ++dx) {
            const int cx = gx + dx, cy = gy + dy;
            if (insidePolygon(cellOutline(m, cx, cy), px, py)) return {cx, cy};
            const auto o = cellOrigin(m, cx, cy);
            const double d = std::hypot((o[0] + tw / 2 - px) / tw, (o[1] + th / 2 - py) / th);
            if (d < bestD) bestD = d, best = {cx, cy};
        }
    return best;
}

std::vector<std::array<int, 2>> cellNeighbours(const Map& m, int x, int y) {
    if (m.orientation == "hexagonal" || m.orientation == "staggered") {
        // neighbours across each edge, in the shifted-row (or column) layout
        std::vector<std::array<int, 2>> out;
        if (m.staggerAxis == "x") {
            const int up = shifted(m, x) ? 0 : -1;
            out = {{x, y - 1}, {x, y + 1}, {x - 1, y + up}, {x - 1, y + up + 1}, {x + 1, y + up}, {x + 1, y + up + 1}};
        } else {
            const int left = shifted(m, y) ? 0 : -1;
            out = {{x - 1, y}, {x + 1, y}, {x + left, y - 1}, {x + left + 1, y - 1}, {x + left, y + 1}, {x + left + 1, y + 1}};
        }
        if (m.orientation == "staggered") {  // diamonds touch four neighbours: the diagonal ones
            std::vector<std::array<int, 2>> d;
            for (const auto& c : out)
                if (m.staggerAxis == "x" ? c[0] != x : c[1] != y) d.push_back(c);
            return d;
        }
        return out;
    }
    return {{x, y - 1}, {x + 1, y}, {x, y + 1}, {x - 1, y}};
}

Image render(const Map& map, const RenderOptions& o) {
    const auto pxSize = mapPixelSize(map);
    const int pw = pxSize[0], ph = pxSize[1];
    Color bg{0, 0, 0, 0};
    if (auto c = Color::parse(map.background); c && !map.background.empty()) bg = *c;
    Image img(std::max(1, pw), std::max(1, ph), bg);
    auto wanted = [&](std::size_t i) {
        const Layer& l = map.layers[i];
        if (o.layers.empty()) return l.visible;
        for (const auto& k : o.layers) {
            if (map.layerIndex(k) == static_cast<int>(i)) return true;
        }
        return false;
    };
    for (std::size_t i = 0; i < map.layers.size(); ++i) {
        const Layer& l = map.layers[i];
        if (!wanted(i)) continue;
        const Tint tint(l.tint);
        const int ox = static_cast<int>(std::lround(l.offsetX)), oy = static_cast<int>(std::lround(l.offsetY));
        if (l.kind == LayerKind::Image) {
            drawImageLayer(img, l, tint);
        } else if (l.kind == LayerKind::Objects) {
            if (!o.objects) continue;
            for (const auto& ob : l.objects) {
                if (!(ob.gid & kGidMask)) continue;
                const int w = static_cast<int>(std::lround(ob.width)), h = static_cast<int>(std::lround(ob.height));
                drawTile(img, map, animatedGid(map, ob.gid, o.time), static_cast<int>(std::lround(ob.x)) + ox, static_cast<int>(std::lround(ob.y)) + oy,
                         l.opacity, tint, w, h);
            }
        } else if (!l.collision) {  // collision grids are drawn as an overlay below
            for (int y = 0; y < map.height; ++y) {
                for (int x = 0; x < map.width; ++x) {
                    const std::uint32_t g = map.at(l, x, y);
                    if (!(g & kGidMask)) continue;
                    const auto at = cellOrigin(map, x, y);
                    if (!orthogonal(map)) {
                        const Tileset* ts = map.tilesetFor(g & kGidMask);
                        if (!ts || !ts->pixels) {  // a colour tile in the cell's own shape
                            Color c = placeholderColor(g & kGidMask);
                            c.a = static_cast<std::uint8_t>(std::lround(255 * std::clamp(l.opacity, 0.0, 1.0)));
                            std::vector<raster::Point> pts;
                            for (const auto& p : cellOutline(map, x, y)) pts.push_back({p.x + ox, p.y + oy});
                            raster::fillPolygon(img, pts, c);
                            for (std::size_t k = 0; k < pts.size(); ++k)
                                raster::drawLine(img, pts[k].x, pts[k].y, pts[(k + 1) % pts.size()].x, pts[(k + 1) % pts.size()].y,
                                                 Color{static_cast<std::uint8_t>(c.r * 3 / 4), static_cast<std::uint8_t>(c.g * 3 / 4), static_cast<std::uint8_t>(c.b * 3 / 4), c.a}, 1);
                            continue;
                        }
                    }
                    drawCellTile(img, map, animatedGid(map, g, o.time), static_cast<int>(std::lround(at[0])) + ox, static_cast<int>(std::lround(at[1])) + oy, l.opacity,
                                 tint);
                }
            }
        }
    }
    const double s = std::clamp(o.scale, 0.05, 16.0);
    if (std::abs(s - 1.0) > 1e-9) {
        img = raster::resize(img, std::max(1, static_cast<int>(std::lround(pw * s))), std::max(1, static_cast<int>(std::lround(ph * s))),
                             s > 1 ? raster::Filter::Nearest : raster::Filter::Bilinear);
    }
    const double tw = map.tileWidth * s, th = map.tileHeight * s;
    if (o.collision) {
        for (std::size_t i = 0; i < map.layers.size(); ++i) {
            const Layer& l = map.layers[i];
            if (l.kind != LayerKind::Tiles || !l.collision || !wanted(i)) continue;
            for (int y = 0; y < map.height; ++y) {
                for (int x = 0; x < map.width; ++x) {
                    if (!(map.at(l, x, y) & kGidMask)) continue;
                    if (orthogonal(map)) {
                        raster::fillRect(img, x * tw, y * th, tw, th, Color{230, 40, 40, 110});
                    } else {
                        std::vector<raster::Point> pts;
                        for (const auto& p : cellOutline(map, x, y)) pts.push_back({p.x * s, p.y * s});
                        raster::fillPolygon(img, pts, Color{230, 40, 40, 110});
                    }
                }
            }
        }
    }
    if (o.grid && !orthogonal(map)) {
        const Color gc{0, 0, 0, 60};
        for (int y = 0; y < map.height; ++y)
            for (int x = 0; x < map.width; ++x) {
                const auto pts = cellOutline(map, x, y);
                for (std::size_t i = 0; i < pts.size(); ++i) {
                    const auto& a = pts[i];
                    const auto& b = pts[(i + 1) % pts.size()];
                    raster::drawLine(img, a.x * s, a.y * s, b.x * s, b.y * s, gc, 1);
                }
            }
    } else if (o.grid) {
        const Color gc{0, 0, 0, 60};
        for (int x = 0; x <= map.width; ++x) raster::fillRect(img, x * tw, 0, 1, img.height(), gc);
        for (int y = 0; y <= map.height; ++y) raster::fillRect(img, 0, y * th, img.width(), 1, gc);
    }
    if (o.objects) {
        for (std::size_t i = 0; i < map.layers.size(); ++i) {
            const Layer& l = map.layers[i];
            if (l.kind != LayerKind::Objects || !wanted(i)) continue;
            for (const auto& ob : l.objects) {
                if (ob.gid & kGidMask) continue;  // drawn as a tile above
                Color c = typeColor(ob.type.empty() ? ob.name : ob.type);
                if (l.collision || ob.type == "collision") c = {230, 40, 40, 255};
                Color fill = c;
                fill.a = 70;
                const double lx = l.offsetX, ly = l.offsetY;
                const double x = (ob.x + lx) * s, y = (ob.y + ly) * s, w = ob.width * s, h = ob.height * s;
                if (ob.shape == "point" || (w <= 0 && h <= 0 && ob.points.empty())) {
                    const double r = std::max(3.0, std::min(tw, th) * 0.25);
                    raster::fillEllipse(img, x, y, r, r, c);
                    raster::strokeEllipse(img, x, y, r, r, Color{255, 255, 255, 255}, 1);
                } else if (ob.shape == "ellipse" && std::abs(ob.rotation) < 1e-9) {
                    raster::fillEllipse(img, x + w / 2, y + h / 2, w / 2, h / 2, fill);
                    raster::strokeEllipse(img, x + w / 2, y + h / 2, w / 2, h / 2, c, std::max(1.0, s));
                } else if (ob.shape == "rect" && std::abs(ob.rotation) < 1e-9) {
                    raster::fillRect(img, x, y, w, h, fill);
                    raster::strokeRect(img, x + 0.5, y + 0.5, std::max(0.0, w - 1), std::max(0.0, h - 1), c, std::max(1.0, s));
                } else {
                    std::vector<raster::Point> pts = objectOutline(ob);
                    for (auto& p : pts) p = {(p.x + lx) * s, (p.y + ly) * s};
                    if (pts.empty()) continue;
                    const bool closed = ob.shape != "polyline";
                    if (closed && pts.size() >= 3) raster::fillPolygon(img, pts, fill);
                    for (std::size_t k = 0; k + 1 < pts.size() + (closed ? 1 : 0); ++k) {
                        const auto& a = pts[k];
                        const auto& b = pts[(k + 1) % pts.size()];
                        raster::drawLine(img, a.x, a.y, b.x, b.y, c, std::max(1.0, s));
                    }
                }
            }
        }
    }
    return img;
}

Image placeholderTileset(int tiles, int columns, int tw, int th, const std::string& style, const std::vector<Color>& colors) {
    tiles = std::max(1, tiles);
    columns = columns > 0 ? columns : std::min(tiles, 8);
    const int rows = (tiles + columns - 1) / columns;
    Image img(columns * tw, rows * th);
    const Color body = colors.empty() ? Color{96, 160, 72, 255} : colors[0];
    const Color rim{static_cast<std::uint8_t>(body.r / 3), static_cast<std::uint8_t>(body.g / 3), static_cast<std::uint8_t>(body.b / 3), 255};
    const Color ground = colors.size() > 1 ? colors[1] : Color{176, 140, 96, 255};
    const int bw = std::max(1, std::min(tw, th) / 6);
    for (int i = 0; i < tiles; ++i) {
        const int ox = (i % columns) * tw, oy = (i / columns) * th;
        Color c = i < static_cast<int>(colors.size()) ? colors[static_cast<std::size_t>(i)] : placeholderColor(static_cast<std::uint32_t>(i + 1));
        if (style == "autotile") {
            // body colour with a dark rim on every side that has no neighbour (mask bits N1 E2 S4 W8)
            const int m = i % 16;
            for (int y = 0; y < th; ++y) {
                for (int x = 0; x < tw; ++x) {
                    bool edge = (!(m & 1) && y < bw) || (!(m & 2) && x >= tw - bw) || (!(m & 4) && y >= th - bw) || (!(m & 8) && x < bw);
                    img.set(ox + x, oy + y, edge ? rim : body);
                }
            }
            continue;
        }
        if (style == "blob") {
            // 47 tiles in blobMasks() order: rims on open sides, notches on open inner corners
            const int m = blobMasks()[static_cast<std::size_t>(i % 47)];
            for (int y = 0; y < th; ++y) {
                for (int x = 0; x < tw; ++x) {
                    const bool top = y < bw, right = x >= tw - bw, bottom = y >= th - bw, left = x < bw;
                    bool edge = (!(m & 1) && top) || (!(m & 4) && right) || (!(m & 16) && bottom) || (!(m & 64) && left);
                    edge = edge || ((m & 1) && (m & 4) && !(m & 2) && top && right) || ((m & 4) && (m & 16) && !(m & 8) && bottom && right) ||
                           ((m & 16) && (m & 64) && !(m & 32) && bottom && left) || ((m & 64) && (m & 1) && !(m & 128) && top && left);
                    img.set(ox + x, oy + y, edge ? rim : body);
                }
            }
            continue;
        }
        if (style == "corner") {
            // 16 Wang corner tiles: quarter q is terrain when bit NE 1, SE 2, SW 4, NW 8 is set, else ground
            const int m = i % 16;
            for (int y = 0; y < th; ++y) {
                for (int x = 0; x < tw; ++x) {
                    const bool east = x >= tw / 2, south = y >= th / 2;
                    const int bit = east ? (south ? 2 : 1) : (south ? 4 : 8);
                    img.set(ox + x, oy + y, (m & bit) ? body : ground);
                }
            }
            continue;
        }
        const Color edge{static_cast<std::uint8_t>(c.r * 3 / 4), static_cast<std::uint8_t>(c.g * 3 / 4), static_cast<std::uint8_t>(c.b * 3 / 4), c.a};
        for (int y = 0; y < th; ++y) {
            for (int x = 0; x < tw; ++x) img.set(ox + x, oy + y, (x == 0 || y == 0 || x == tw - 1 || y == th - 1) ? edge : c);
        }
        // a small mark of i's bits along the top so tiles can be told apart even in similar hues
        for (int b = 0; b < 8 && (b + 1) * 3 < tw; ++b) {
            if (i >> b & 1) {
                for (int y = 2; y < std::min(th - 1, 4); ++y) img.set(ox + 2 + b * 3, oy + y, Color{255, 255, 255, 200});
            }
        }
    }
    return img;
}

// ---- documents ----------------------------------------------------------------

std::string LevelDoc::summary() const {
    std::size_t tileLayers = 0, objectLayers = 0, imageLayers = 0;
    for (const auto& l : map.layers) (l.kind == LayerKind::Tiles ? tileLayers : l.kind == LayerKind::Objects ? objectLayers : imageLayers)++;
    std::string s = std::to_string(map.width) + "x" + std::to_string(map.height) + " cells of " + std::to_string(map.tileWidth) + "x" +
                    std::to_string(map.tileHeight) + " px, " + std::to_string(tileLayers) + " tile layer" + (tileLayers == 1 ? "" : "s") +
                    ", " + std::to_string(objectLayers) + " object layer" + (objectLayers == 1 ? "" : "s") + ", ";
    if (imageLayers) s += std::to_string(imageLayers) + " image layer" + (imageLayers == 1 ? "" : "s") + ", ";
    s += std::to_string(map.objectCount()) + " object" + (map.objectCount() == 1 ? "" : "s") + ", " + std::to_string(map.tilesets.size()) +
         " tileset" + (map.tilesets.size() == 1 ? "" : "s");
    if (!path.empty()) s += ", " + path;
    return s;
}

std::string WorldDoc::summary() const {
    std::string s = "world of " + std::to_string(entries.size()) + " level" + (entries.size() == 1 ? "" : "s");
    if (!path.empty()) s += ", " + path;
    return s;
}

std::unique_ptr<Attachment> LevelDoc::preview() const {
    RenderOptions o;
    const int big = std::max(map.width * map.tileWidth, map.height * map.tileHeight);
    if (big > 0 && big < 512) o.scale = std::max(1.0, std::floor(512.0 / big));
    return std::make_unique<Attachment>(Attachment{"image/png", raster::previewPng(render(map, o), 1024), "preview.png"});
}

Result LevelEdit::apply() {
    before_ = doc_->map;
    Result r;
    try {
        r = fn_(*doc_);
    } catch (const std::exception& e) {
        r = Result::failure(e.what());
    }
    if (!r.ok) revert();
    return r;
}

void LevelEdit::revert() { doc_->map = before_; }

}  // namespace wetwaresuite::levels
