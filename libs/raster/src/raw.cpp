#include "wetwaresuite/raster/raw.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <map>
#include <stdexcept>
#include <vector>

#include "wetwaresuite/raster/codec.hpp"
#include "wetwaresuite/raster/ops.hpp"
#include "wetwaresuite/raster/parallel.hpp"

namespace wetwaresuite::raster {

namespace {

[[noreturn]] void bad(const std::string& why) { throw std::runtime_error("raw: " + why); }

// ---------------------------------------------------------------------------------------------
// TIFF structure (DNG is TIFF with extra tags)

struct Tiff {
    const std::string& b;
    bool le = true;
    std::uint32_t u8(std::size_t at) const {
        if (at >= b.size()) bad("file is cut short");
        return static_cast<unsigned char>(b[at]);
    }
    std::uint32_t u16(std::size_t at) const { return le ? u8(at) | (u8(at + 1) << 8) : (u8(at) << 8) | u8(at + 1); }
    std::uint32_t u32(std::size_t at) const { return le ? u16(at) | (u16(at + 2) << 16) : (u16(at) << 16) | u16(at + 2); }
};

struct Entry {
    std::uint32_t type = 0, count = 0;
    std::size_t at = 0;
};
using Ifd = std::map<std::uint32_t, Entry>;

std::size_t typeSize(std::uint32_t t) {
    switch (t) {
        case 1: case 2: case 6: case 7: return 1;
        case 3: case 8: return 2;
        case 4: case 9: case 11: case 13: return 4;
        case 5: case 10: case 12: return 8;
        default: return 0;
    }
}

Ifd readIfd(const Tiff& t, std::size_t at, std::size_t* next = nullptr) {
    Ifd ifd;
    const std::uint32_t n = t.u16(at);
    if (n > 4000) bad("broken directory");
    for (std::uint32_t i = 0; i < n; ++i) {
        const std::size_t e = at + 2 + std::size_t{i} * 12;
        Entry en{t.u16(e + 2), t.u32(e + 4), 0};
        const std::size_t bytes = typeSize(en.type) * en.count;
        en.at = bytes <= 4 ? e + 8 : t.u32(e + 8);
        if (en.at + bytes > t.b.size()) continue;  // skip broken tags, keep going
        ifd[t.u16(e)] = en;
    }
    if (next) *next = t.u32(at + 2 + std::size_t{n} * 12);
    return ifd;
}

double number(const Tiff& t, const Entry& e, std::uint32_t i) {
    const std::size_t at = e.at + typeSize(e.type) * i;
    switch (e.type) {
        case 1: case 7: return t.u8(at);
        case 3: return t.u16(at);
        case 8: return static_cast<std::int16_t>(t.u16(at));
        case 4: return t.u32(at);
        case 9: return static_cast<std::int32_t>(t.u32(at));
        case 5: {
            const double d = t.u32(at + 4);
            return d == 0 ? 0 : t.u32(at) / d;
        }
        case 10: {
            const double d = static_cast<std::int32_t>(t.u32(at + 4));
            return d == 0 ? 0 : static_cast<std::int32_t>(t.u32(at)) / d;
        }
        default: return 0;
    }
}

std::vector<double> numbers(const Tiff& t, const Ifd& ifd, std::uint32_t tag) {
    std::vector<double> v;
    auto it = ifd.find(tag);
    if (it == ifd.end() || it->second.count > (1u << 24)) return v;
    for (std::uint32_t i = 0; i < it->second.count; ++i) v.push_back(number(t, it->second, i));
    return v;
}
double number(const Tiff& t, const Ifd& ifd, std::uint32_t tag, double fallback) {
    auto it = ifd.find(tag);
    return it == ifd.end() || it->second.count == 0 ? fallback : number(t, it->second, 0);
}
std::string text(const Tiff& t, const Ifd& ifd, std::uint32_t tag) {
    auto it = ifd.find(tag);
    if (it == ifd.end()) return "";
    std::string s = t.b.substr(it->second.at, it->second.count);
    while (!s.empty() && (s.back() == '\0' || s.back() == ' ')) s.pop_back();
    return s;
}

enum Tag : std::uint32_t {
    NewSubFileType = 254, Width = 256, Height = 257, BitsPerSample = 258, Compression = 259, Photometric = 262,
    Make = 271, Model = 272, StripOffsets = 273, Orientation = 274, SamplesPerPixel = 277, RowsPerStrip = 278,
    StripByteCounts = 279, TileWidth = 322, TileLength = 323, TileOffsets = 324, TileByteCounts = 325, SubIfds = 330,
    CfaRepeatPatternDim = 33421, CfaPattern = 33422, DngVersion = 50706, UniqueCameraModel = 50708,
    LinearizationTable = 50712, BlackLevelRepeatDim = 50713, BlackLevel = 50714, WhiteLevel = 50717,
    DefaultCropOrigin = 50719, DefaultCropSize = 50720, ColorMatrix1 = 50721, ColorMatrix2 = 50722, AsShotNeutral = 50728,
    BaselineExposure = 50730, CalibrationIlluminant1 = 50778, CalibrationIlluminant2 = 50779, ActiveArea = 50829,
};

struct Dng {
    Ifd main;        // IFD0 (camera info, orientation)
    Ifd raw;         // the full-size raw image
    bool cfa = true;
};

Dng findRaw(const Tiff& t) {
    Dng d;
    std::size_t next = 0;
    d.main = readIfd(t, t.u32(4), &next);
    if (!d.main.count(DngVersion)) bad("not a DNG file");
    std::vector<Ifd> all = {d.main};
    for (double off : numbers(t, d.main, SubIfds)) all.push_back(readIfd(t, static_cast<std::size_t>(off)));
    for (int guard = 0; next && next < t.b.size() && guard < 8; ++guard) all.push_back(readIfd(t, next, &next));
    const Ifd* best = nullptr;
    double bestArea = 0;
    for (const auto& ifd : all) {
        if (number(t, ifd, NewSubFileType, 0) != 0) continue;  // previews, masks
        const double ph = number(t, ifd, Photometric, 0);
        if (ph != 32803 && ph != 34892) continue;
        const double area = number(t, ifd, Width, 0) * number(t, ifd, Height, 0);
        if (area > bestArea) best = &ifd, bestArea = area;
    }
    if (!best) bad("no raw image inside this DNG");
    d.raw = *best;
    d.cfa = number(t, d.raw, Photometric, 0) == 32803;
    return d;
}

// ---------------------------------------------------------------------------------------------
// Lossless JPEG

struct Huffman {
    std::vector<std::uint16_t> fast;  // 16-bit peek -> (length << 8 | symbol), 0 = no code
    void build(const std::uint8_t counts[16], const std::vector<std::uint8_t>& symbols) {
        fast.assign(1u << 16, 0);
        std::uint32_t code = 0;
        std::size_t k = 0;
        for (int len = 1; len <= 16; ++len) {
            for (int i = 0; i < counts[len - 1]; ++i, ++code) {
                if (k >= symbols.size()) bad("broken huffman table");
                const std::uint32_t first = code << (16 - len), n = 1u << (16 - len);
                if (first + n > fast.size()) bad("broken huffman table");
                for (std::uint32_t j = 0; j < n; ++j) fast[first + j] = static_cast<std::uint16_t>((len << 8) | symbols[k]);
                ++k;
            }
            code <<= 1;
        }
    }
};

struct Bits {
    const std::string& d;
    std::size_t pos;
    std::uint64_t acc = 0;
    int n = 0;
    bool marker = false;
    void fill() {
        while (n <= 48) {
            std::uint32_t byte = 0;
            if (!marker && pos < d.size()) {
                byte = static_cast<unsigned char>(d[pos]);
                if (byte == 0xFF) {
                    const std::uint32_t nx = pos + 1 < d.size() ? static_cast<unsigned char>(d[pos + 1]) : 0xD9;
                    if (nx == 0) {
                        pos += 2;
                    } else {
                        marker = true;  // a marker: feed zeros from here
                        byte = 0;
                    }
                } else {
                    ++pos;
                }
            }
            acc |= std::uint64_t{byte} << (56 - n);
            n += 8;
        }
    }
    std::uint32_t peek16() {
        if (n < 16) fill();
        return static_cast<std::uint32_t>(acc >> 48);
    }
    void skip(int k) {
        acc <<= k;
        n -= k;
    }
    std::uint32_t get(int k) {
        if (k == 0) return 0;
        if (n < k) fill();
        const auto v = static_cast<std::uint32_t>(acc >> (64 - k));
        skip(k);
        return v;
    }
    void restart() {  // skip to just after the next RSTn marker
        acc = 0, n = 0, marker = false;
        while (pos + 1 < d.size() && !(static_cast<unsigned char>(d[pos]) == 0xFF && (static_cast<unsigned char>(d[pos + 1]) & 0xF8) == 0xD0)) ++pos;
        pos = std::min(d.size(), pos + 2);
    }
};

}  // namespace

void decodeLosslessJpeg(const std::string& d, std::vector<std::uint16_t>& out, int& width, int& height, int& comps) {
    auto u8 = [&](std::size_t at) -> std::uint32_t {
        if (at >= d.size()) bad("lossless JPEG is cut short");
        return static_cast<unsigned char>(d[at]);
    };
    auto u16 = [&](std::size_t at) { return (u8(at) << 8) | u8(at + 1); };
    if (u16(0) != 0xFFD8) bad("not a JPEG stream");
    std::array<Huffman, 4> tables;
    int precision = 0, restartInterval = 0, predictor = 1, pointTransform = 0;
    std::array<int, 4> compTable{};
    width = height = comps = 0;
    std::size_t at = 2;
    for (;;) {
        while (u8(at) != 0xFF) ++at;  // tolerate fill bytes
        while (u8(at) == 0xFF) ++at;
        const std::uint32_t m = u8(at++);
        if (m == 0xD9) bad("no scan in lossless JPEG");
        const std::size_t len = u16(at);
        const std::size_t end = at + len;
        if (m == 0xC4) {
            std::size_t p = at + 2;
            while (p < end) {
                const std::uint32_t id = u8(p) & 3;
                std::uint8_t counts[16];
                std::size_t total = 0;
                for (int i = 0; i < 16; ++i) total += counts[i] = static_cast<std::uint8_t>(u8(p + 1 + static_cast<std::size_t>(i)));
                std::vector<std::uint8_t> sym(total);
                for (std::size_t i = 0; i < total; ++i) sym[i] = static_cast<std::uint8_t>(u8(p + 17 + i));
                tables[id].build(counts, sym);
                p += 17 + total;
            }
        } else if (m == 0xC3) {
            precision = static_cast<int>(u8(at + 2));
            height = static_cast<int>(u16(at + 3));
            width = static_cast<int>(u16(at + 5));
            comps = static_cast<int>(u8(at + 7));
            if (comps < 1 || comps > 4 || width < 1 || height < 1 || precision < 2 || precision > 16) bad("unsupported lossless JPEG");
        } else if ((m >= 0xC0 && m <= 0xCF) && m != 0xC4 && m != 0xC8 && m != 0xCC) {
            bad("lossy JPEG inside the raw (only lossless DNGs develop)");
        } else if (m == 0xDD) {
            restartInterval = static_cast<int>(u16(at + 2));
        } else if (m == 0xDA) {
            const int ns = static_cast<int>(u8(at + 2));
            if (ns != comps) bad("scan components don't match");
            for (int i = 0; i < ns; ++i) compTable[static_cast<std::size_t>(i)] = static_cast<int>(u8(at + 4 + 2 * static_cast<std::size_t>(i)) >> 4);
            predictor = static_cast<int>(u8(at + 3 + 2 * static_cast<std::size_t>(ns)));
            pointTransform = static_cast<int>(u8(at + 5 + 2 * static_cast<std::size_t>(ns)) & 15);
            at = end;
            break;
        }
        at = end;
    }
    if (!width) bad("lossless JPEG without a frame header");
    for (int c = 0; c < comps; ++c)
        if (tables[static_cast<std::size_t>(compTable[static_cast<std::size_t>(c)])].fast.empty()) bad("missing huffman table");
    const std::size_t rowLen = static_cast<std::size_t>(width) * static_cast<std::size_t>(comps);
    out.assign(rowLen * static_cast<std::size_t>(height), 0);
    Bits bits{d, at};
    const int initial = 1 << (precision - pointTransform - 1);
    long mcus = 0;
    bool freshRow = true;  // first row (of the image or after a restart): predict from the left
    for (int y = 0; y < height; ++y) {
        std::uint16_t* row = out.data() + rowLen * static_cast<std::size_t>(y);
        const std::uint16_t* up = y ? row - rowLen : nullptr;
        for (int x = 0; x < width; ++x) {
            if (restartInterval && mcus && mcus % restartInterval == 0) {
                bits.restart();
                freshRow = true;
            }
            for (int c = 0; c < comps; ++c) {
                const Huffman& h = tables[static_cast<std::size_t>(compTable[static_cast<std::size_t>(c)])];
                const std::uint16_t e = h.fast[bits.peek16()];
                if (!e) bad("bad huffman code in lossless JPEG");
                bits.skip(e >> 8);
                const int ssss = e & 0xFF;
                int diff = 0;
                if (ssss == 16) {
                    diff = 32768;
                } else if (ssss) {
                    diff = static_cast<int>(bits.get(ssss));
                    if (diff < (1 << (ssss - 1))) diff -= (1 << ssss) - 1;
                }
                const std::size_t i = static_cast<std::size_t>(x) * static_cast<std::size_t>(comps) + static_cast<std::size_t>(c);
                int pred;
                if (freshRow) {
                    pred = x == 0 ? initial : row[i - static_cast<std::size_t>(comps)];
                } else if (x == 0) {
                    pred = up[i];
                } else {
                    const int a = row[i - static_cast<std::size_t>(comps)], b = up[i], cc = up[i - static_cast<std::size_t>(comps)];
                    switch (predictor) {
                        case 1: pred = a; break;
                        case 2: pred = b; break;
                        case 3: pred = cc; break;
                        case 4: pred = a + b - cc; break;
                        case 5: pred = a + ((b - cc) >> 1); break;
                        case 6: pred = b + ((a - cc) >> 1); break;
                        case 7: pred = (a + b) >> 1; break;
                        default: pred = a; break;
                    }
                }
                row[i] = static_cast<std::uint16_t>((pred + diff) & 0xFFFF);  // DNG never uses a point transform
            }
            ++mcus;
        }
        freshRow = false;
    }
}

namespace {

// ---------------------------------------------------------------------------------------------
// Reading the sensor data

struct Sensor {
    int w = 0, h = 0, spp = 1;
    std::vector<std::uint16_t> v;  // w * h * spp
};

void unpack(const std::string& src, std::size_t at, std::size_t bytes, int bps, bool le, std::uint16_t* dst, std::size_t count) {
    if (bps == 16) {
        for (std::size_t i = 0; i < count && at + i * 2 + 1 < at + bytes && at + i * 2 + 1 < src.size(); ++i) {
            const auto a = static_cast<unsigned char>(src[at + i * 2]), b = static_cast<unsigned char>(src[at + i * 2 + 1]);
            dst[i] = static_cast<std::uint16_t>(le ? a | (b << 8) : (a << 8) | b);
        }
    } else if (bps == 8) {
        for (std::size_t i = 0; i < count && i < bytes && at + i < src.size(); ++i) dst[i] = static_cast<unsigned char>(src[at + i]);
    } else {  // packed, most significant bit first
        std::uint64_t acc = 0;
        int n = 0;
        std::size_t p = at;
        const std::size_t end = std::min(src.size(), at + bytes);
        for (std::size_t i = 0; i < count; ++i) {
            while (n < bps) {
                acc = (acc << 8) | (p < end ? static_cast<unsigned char>(src[p]) : 0u);
                ++p, n += 8;
            }
            dst[i] = static_cast<std::uint16_t>((acc >> (n - bps)) & ((1u << bps) - 1));
            n -= bps;
        }
    }
}

Sensor readSensor(const Tiff& t, const Ifd& ifd) {
    Sensor s;
    s.w = static_cast<int>(number(t, ifd, Width, 0));
    s.h = static_cast<int>(number(t, ifd, Height, 0));
    s.spp = static_cast<int>(number(t, ifd, SamplesPerPixel, 1));
    const int bps = static_cast<int>(number(t, ifd, BitsPerSample, 16));
    const int comp = static_cast<int>(number(t, ifd, Compression, 1));
    if (s.w < 1 || s.h < 1 || s.w > 60000 || s.h > 60000 || s.spp < 1 || s.spp > 4) bad("odd raw size");
    if (static_cast<double>(s.w) * s.h > 400e6) bad("raw is too big");
    if (comp != 1 && comp != 7) bad(comp == 34892 ? "lossy DNGs aren't supported yet" : "unsupported DNG compression " + std::to_string(comp));
    if (bps < 8 || bps > 16) bad("unsupported bits per sample");
    s.v.assign(static_cast<std::size_t>(s.w) * static_cast<std::size_t>(s.h) * static_cast<std::size_t>(s.spp), 0);

    const bool tiled = ifd.count(TileOffsets);
    const std::vector<double> offs = numbers(t, ifd, tiled ? TileOffsets : StripOffsets);
    const std::vector<double> lens = numbers(t, ifd, tiled ? TileByteCounts : StripByteCounts);
    if (offs.empty() || offs.size() != lens.size()) bad("raw has no image data");
    const int tw = tiled ? static_cast<int>(number(t, ifd, TileWidth, s.w)) : s.w;
    const int th = tiled ? static_cast<int>(number(t, ifd, TileLength, s.h)) : static_cast<int>(number(t, ifd, RowsPerStrip, s.h));
    if (tw < 1 || th < 1) bad("bad tile size");
    const int across = (s.w + tw - 1) / tw;
    for (std::size_t k = 0; k < offs.size(); ++k) {
        const int tx = static_cast<int>(k % static_cast<std::size_t>(across)) * tw, ty = static_cast<int>(k / static_cast<std::size_t>(across)) * th;
        if (ty >= s.h) break;
        const auto at = static_cast<std::size_t>(offs[k]), bytes = static_cast<std::size_t>(lens[k]);
        if (at >= t.b.size()) bad("raw data is cut short");
        std::vector<std::uint16_t> tile;
        const std::size_t tileRow = static_cast<std::size_t>(tw) * static_cast<std::size_t>(s.spp);
        if (comp == 7) {
            int jw = 0, jh = 0, jc = 0;
            decodeLosslessJpeg(t.b.substr(at, std::min(bytes, t.b.size() - at)), tile, jw, jh, jc);
        } else {
            tile.assign(tileRow * static_cast<std::size_t>(th), 0);
            if (bps == 8 || bps == 16) {
                unpack(t.b, at, bytes, bps, t.le, tile.data(), tile.size());
            } else {  // packed rows start on byte boundaries
                const std::size_t rowBytes = (tileRow * static_cast<std::size_t>(bps) + 7) / 8;
                for (int r = 0; r < th; ++r)
                    unpack(t.b, at + rowBytes * static_cast<std::size_t>(r), rowBytes, bps, t.le, tile.data() + tileRow * static_cast<std::size_t>(r), tileRow);
            }
        }
        // samples run row by row through the tile, whatever shape the JPEG said it was
        for (std::size_t i = 0; i < tile.size(); ++i) {
            const int y = ty + static_cast<int>(i / tileRow);
            const int xs = static_cast<int>(i % tileRow);
            const int x = tx + xs / s.spp;
            if (y >= s.h || y >= ty + th) break;
            if (x >= s.w) continue;
            s.v[(static_cast<std::size_t>(y) * static_cast<std::size_t>(s.w) + static_cast<std::size_t>(x)) * static_cast<std::size_t>(s.spp) +
                static_cast<std::size_t>(xs % s.spp)] = tile[i];
        }
    }
    return s;
}

// ---------------------------------------------------------------------------------------------
// Colour

using M3 = std::array<double, 9>;

M3 mul(const M3& a, const M3& b) {
    M3 r{};
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j)
            for (int k = 0; k < 3; ++k) r[static_cast<std::size_t>(i * 3 + j)] += a[static_cast<std::size_t>(i * 3 + k)] * b[static_cast<std::size_t>(k * 3 + j)];
    return r;
}
bool invert(const M3& m, M3& out) {
    const double a = m[0], b = m[1], c = m[2], d = m[3], e = m[4], f = m[5], g = m[6], h = m[7], i = m[8];
    const double det = a * (e * i - f * h) - b * (d * i - f * g) + c * (d * h - e * g);
    if (std::abs(det) < 1e-12) return false;
    out = {(e * i - f * h) / det, (c * h - b * i) / det, (b * f - c * e) / det, (f * g - d * i) / det, (a * i - c * g) / det,
           (c * d - a * f) / det, (d * h - e * g) / det, (b * g - a * h) / det, (a * e - b * d) / det};
    return true;
}
// linear sRGB (D65) -> XYZ
constexpr M3 kXyzFromSrgb = {0.4124564, 0.3575761, 0.1804375, 0.2126729, 0.7151522, 0.0721750, 0.0193339, 0.1191920, 0.9503041};

// chromaticity of a light at this colour temperature (daylight locus, Planckian below 4000 K)
void whiteXy(double T, double tint, double& x, double& y) {
    T = std::clamp(T, 1700.0, 25000.0);
    if (T >= 4000) {
        x = T <= 7000 ? -4.6070e9 / (T * T * T) + 2.9678e6 / (T * T) + 0.09911e3 / T + 0.244063
                      : -2.0064e9 / (T * T * T) + 1.9018e6 / (T * T) + 0.24748e3 / T + 0.237040;
        y = -3 * x * x + 2.870 * x - 0.275;
    } else {
        x = -0.2661239e9 / (T * T * T) - 0.2343589e6 / (T * T) + 0.8776956e3 / T + 0.179910;
        y = T >= 2222 ? -0.9549476 * x * x * x - 1.37418593 * x * x + 2.09137015 * x - 0.16748867
                      : -1.1063814 * x * x * x - 1.34811020 * x * x + 2.18555832 * x - 0.20219683;
    }
    y += tint * 0.0002;  // + magenta: we're correcting a greener light
}

struct Colour {
    M3 camToSrgb{1, 0, 0, 0, 1, 0, 0, 0, 1};
    M3 xyzToCam{};
    bool hasMatrix = false;
    std::array<double, 3> asShot{1, 1, 1};  // camera neutral

    std::array<double, 3> neutralFor(double T, double tint) const {
        if (!hasMatrix) return {1, 1, 1};
        double x, y;
        whiteXy(T, tint, x, y);
        const double X = x / y, Y = 1, Z = (1 - x - y) / y;
        std::array<double, 3> n{};
        for (int c = 0; c < 3; ++c) n[static_cast<std::size_t>(c)] = xyzToCam[static_cast<std::size_t>(c * 3)] * X + xyzToCam[static_cast<std::size_t>(c * 3 + 1)] * Y + xyzToCam[static_cast<std::size_t>(c * 3 + 2)] * Z;
        const double g = std::max(1e-6, n[1]);
        for (auto& v : n) v = std::max(1e-6, v / g);
        return n;
    }
};

Colour readColour(const Tiff& t, const Dng& d) {
    Colour c;
    auto pick = [&](std::uint32_t tag) { return numbers(t, d.main, tag).size() >= 9 ? numbers(t, d.main, tag) : numbers(t, d.raw, tag); };
    std::vector<double> cm1 = pick(ColorMatrix1), cm2 = pick(ColorMatrix2);
    const double il2 = number(t, d.main, CalibrationIlluminant2, 0);
    // prefer the daylight (D65 = 21, D55 = 20, D50 = 23) matrix
    std::vector<double> cm = cm2.size() >= 9 && (il2 == 21 || il2 == 20 || il2 == 23 || cm1.size() < 9) ? cm2 : cm1;
    if (cm.size() >= 9) {
        for (std::size_t i = 0; i < 9; ++i) c.xyzToCam[i] = cm[i];
        // camera <- linear sRGB, rows scaled so sRGB white lands on equal camera values (dcraw's way)
        M3 camFromRgb = mul(c.xyzToCam, kXyzFromSrgb);
        for (int r = 0; r < 3; ++r) {
            double sum = 0;
            for (int k = 0; k < 3; ++k) sum += camFromRgb[static_cast<std::size_t>(r * 3 + k)];
            if (std::abs(sum) > 1e-9)
                for (int k = 0; k < 3; ++k) camFromRgb[static_cast<std::size_t>(r * 3 + k)] /= sum;
        }
        M3 inv;
        if (invert(camFromRgb, inv)) {
            c.camToSrgb = inv;
            c.hasMatrix = true;
        }
    }
    std::vector<double> asShot = pick(AsShotNeutral);
    if (asShot.size() >= 3 && asShot[0] > 0 && asShot[1] > 0 && asShot[2] > 0) {
        for (std::size_t i = 0; i < 3; ++i) c.asShot[i] = asShot[i] / asShot[1];
    } else if (c.hasMatrix) {
        c.asShot = c.neutralFor(6504, 0);  // D65, what the matrix normalisation assumes
    }
    return c;
}

float srgbEncode(float v) { return v <= 0.0031308f ? v * 12.92f : 1.055f * std::pow(v, 1 / 2.4f) - 0.055f; }

}  // namespace

const std::vector<std::string>& rawExtensions() {
    static const std::vector<std::string> e = {"dng"};
    return e;
}

bool isDng(const std::string& b) {
    if (b.size() < 16 || !(b.compare(0, 4, "II*\0", 4) == 0 || b.compare(0, 4, "MM\0*", 4) == 0)) return false;
    try {
        Tiff t{b, b[0] == 'I'};
        return readIfd(t, t.u32(4)).count(DngVersion) > 0;
    } catch (const std::exception&) {
        return false;
    }
}

RawInfo dngInfo(const std::string& bytes) {
    Tiff t{bytes, bytes.size() > 0 && bytes[0] == 'I'};
    const Dng d = findRaw(t);
    RawInfo info;
    const std::vector<double> crop = numbers(t, d.raw, DefaultCropSize);
    info.width = crop.size() >= 2 ? static_cast<int>(crop[0]) : static_cast<int>(number(t, d.raw, Width, 0));
    info.height = crop.size() >= 2 ? static_cast<int>(crop[1]) : static_cast<int>(number(t, d.raw, Height, 0));
    info.bits = static_cast<int>(number(t, d.raw, BitsPerSample, 16));
    const std::string model = text(t, d.main, UniqueCameraModel);
    info.camera = !model.empty() ? model : text(t, d.main, Make) + " " + text(t, d.main, Model);
    if (d.cfa) {
        const std::vector<double> pat = numbers(t, d.raw, CfaPattern);
        static const char names[] = "RGBCMY";
        for (double p : pat) info.layout += p >= 0 && p < 6 ? names[static_cast<int>(p)] : '?';
    } else {
        info.layout = "linear";
    }
    const Colour c = readColour(t, d);
    for (std::size_t i = 0; i < 3; ++i) info.asShotNeutral[i] = c.asShot[i];
    info.baselineExposure = number(t, d.main, BaselineExposure, 0);
    return info;
}

Image developDng(const std::string& bytes, const RawDevelop& set) {
    Tiff t{bytes, bytes.size() > 0 && bytes[0] == 'I'};
    const Dng d = findRaw(t);
    Sensor s = readSensor(t, d.raw);

    // levels: linearisation table, black (maybe per CFA position), white
    const std::vector<double> lin = numbers(t, d.raw, LinearizationTable);
    const int bps = static_cast<int>(number(t, d.raw, BitsPerSample, 16));
    std::vector<double> black = numbers(t, d.raw, BlackLevel);
    const std::vector<double> blackDim = numbers(t, d.raw, BlackLevelRepeatDim);
    const int bdy = blackDim.size() >= 2 ? std::max(1, static_cast<int>(blackDim[0])) : 1, bdx = blackDim.size() >= 2 ? std::max(1, static_cast<int>(blackDim[1])) : 1;
    if (black.empty()) black = {0};
    const double white = number(t, d.raw, WhiteLevel, lin.empty() ? (1 << bps) - 1 : 65535);

    // active area (top, left, bottom, right) then the default crop inside it
    int ax0 = 0, ay0 = 0, ax1 = s.w, ay1 = s.h;
    if (const auto aa = numbers(t, d.raw, ActiveArea); aa.size() >= 4) {
        ay0 = std::clamp(static_cast<int>(aa[0]), 0, s.h), ax0 = std::clamp(static_cast<int>(aa[1]), 0, s.w);
        ay1 = std::clamp(static_cast<int>(aa[2]), ay0, s.h), ax1 = std::clamp(static_cast<int>(aa[3]), ax0, s.w);
    }
    const int W = ax1 - ax0, H = ay1 - ay0;
    if (W < 2 || H < 2) bad("raw has no active area");

    // CFA layout
    int cfa[2][2] = {{0, 1}, {1, 2}};
    if (d.cfa) {
        const std::vector<double> dim = numbers(t, d.raw, CfaRepeatPatternDim), pat = numbers(t, d.raw, CfaPattern);
        if (dim.size() >= 2 && (dim[0] != 2 || dim[1] != 2)) bad("only 2x2 Bayer sensors develop (not X-Trans)");
        if (pat.size() >= 4) {
            for (int i = 0; i < 4; ++i) {
                const int c = static_cast<int>(pat[static_cast<std::size_t>(i)]);
                if (c < 0 || c > 2) bad("unsupported colour filter (only RGB Bayer)");
                cfa[i / 2][i % 2] = c;
            }
        }
        // the pattern is relative to the active area's origin
        if (ax0 % 2 || ay0 % 2) {
            int shifted[2][2];
            for (int y = 0; y < 2; ++y)
                for (int x = 0; x < 2; ++x) shifted[y][x] = cfa[(y + ay0) % 2][(x + ax0) % 2];
            std::copy(&shifted[0][0], &shifted[0][0] + 4, &cfa[0][0]);
        }
    }

    const Colour colour = readColour(t, d);
    std::array<double, 3> neutral = colour.asShot;
    const std::string& wb = set.whiteBalance;
    static const std::map<std::string, std::pair<double, double>> presets = {
        {"daylight", {5500, 0}}, {"cloudy", {6500, 10}}, {"shade", {7500, 10}}, {"tungsten", {2850, 0}}, {"fluorescent", {3800, 21}}, {"flash", {5500, 0}}};
    if (wb == "custom") {
        neutral = colour.neutralFor(set.temperature, set.tint);
    } else if (presets.count(wb)) {
        neutral = colour.neutralFor(presets.at(wb).first, presets.at(wb).second);
    } else if (wb != "as_shot" && wb != "auto" && !wb.empty()) {
        bad("white balance must be as_shot, auto, daylight, cloudy, shade, tungsten, fluorescent, flash or custom");
    }

    // normalise to 0..1 camera values in a float plane per sample
    const std::size_t spp = static_cast<std::size_t>(s.spp);
    std::vector<float> plane(static_cast<std::size_t>(W) * static_cast<std::size_t>(H) * spp);
    parallelRows(H, static_cast<std::size_t>(W) * spp * 4, [&](int y0, int y1) {
        for (int y = y0; y < y1; ++y)
            for (int x = 0; x < W; ++x)
                for (std::size_t c = 0; c < spp; ++c) {
                    double v = s.v[(static_cast<std::size_t>(y + ay0) * static_cast<std::size_t>(s.w) + static_cast<std::size_t>(x + ax0)) * spp + c];
                    if (!lin.empty()) v = lin[std::min(lin.size() - 1, static_cast<std::size_t>(v))];
                    const std::size_t bi = black.size() >= static_cast<std::size_t>(bdx * bdy) * spp
                                               ? ((static_cast<std::size_t>((y + ay0) % bdy) * static_cast<std::size_t>(bdx) + static_cast<std::size_t>((x + ax0) % bdx)) * spp + c) % black.size()
                                               : 0;
                    const double b = black[bi];
                    plane[(static_cast<std::size_t>(y) * static_cast<std::size_t>(W) + static_cast<std::size_t>(x)) * spp + c] =
                        static_cast<float>(std::clamp((v - b) / std::max(1.0, white - b), 0.0, 1.0));
                }
    });
    s.v.clear();
    s.v.shrink_to_fit();
    auto colourAt = [&](int x, int y) { return cfa[y & 1][x & 1]; };

    if (wb == "auto") {  // grey world over the mid tones
        double sum[3] = {0, 0, 0};
        long cnt[3] = {0, 0, 0};
        for (int y = 0; y < H; y += 3)
            for (int x = 0; x < W; x += 3) {
                if (d.cfa) {
                    const float v = plane[static_cast<std::size_t>(y) * static_cast<std::size_t>(W) + static_cast<std::size_t>(x)];
                    if (v > 0.02f && v < 0.9f) sum[colourAt(x, y)] += v, ++cnt[colourAt(x, y)];
                } else if (spp >= 3) {
                    for (int c = 0; c < 3; ++c) {
                        const float v = plane[(static_cast<std::size_t>(y) * static_cast<std::size_t>(W) + static_cast<std::size_t>(x)) * spp + static_cast<std::size_t>(c)];
                        if (v > 0.02f && v < 0.9f) sum[c] += v, ++cnt[c];
                    }
                }
            }
        if (cnt[0] && cnt[1] && cnt[2]) {
            const double g = sum[1] / static_cast<double>(cnt[1]);
            for (int c = 0; c < 3; ++c) neutral[static_cast<std::size_t>(c)] = std::max(1e-3, sum[c] / static_cast<double>(cnt[c]) / g);
        }
    }
    float mult[3];
    for (int c = 0; c < 3; ++c) mult[c] = static_cast<float>(1.0 / neutral[static_cast<std::size_t>(c)]);
    const float minMult = std::min({mult[0], mult[1], mult[2]});
    for (float& m : mult) m /= minMult;  // smallest multiplier = 1 so clipped areas stay white

    // demosaic (Malvar-He-Cutler, or 2x2 binning for half size) into camera RGB
    const int OW = set.halfSize && d.cfa ? W / 2 : W, OH = set.halfSize && d.cfa ? H / 2 : H;
    std::vector<float> rgb(static_cast<std::size_t>(OW) * static_cast<std::size_t>(OH) * 3);
    auto P = [&](int x, int y) {
        x = x < 0 ? -x : x >= W ? 2 * W - 2 - x : x;
        y = y < 0 ? -y : y >= H ? 2 * H - 2 - y : y;
        // mirroring by 2 keeps the CFA colour the same
        if (x < 0 || x >= W) x = std::clamp(x, 0, W - 1);
        if (y < 0 || y >= H) y = std::clamp(y, 0, H - 1);
        const int c = colourAt(x, y);
        return plane[static_cast<std::size_t>(y) * static_cast<std::size_t>(W) + static_cast<std::size_t>(x)] * mult[c];
    };
    if (!d.cfa) {
        for (std::size_t i = 0; i < static_cast<std::size_t>(W) * static_cast<std::size_t>(H); ++i)
            for (std::size_t c = 0; c < 3; ++c) rgb[i * 3 + c] = std::min(1.0f, plane[i * spp + std::min(c, spp - 1)] * mult[c]);
    } else if (set.halfSize) {
        parallelRows(OH, static_cast<std::size_t>(OW) * 12, [&](int y0, int y1) {
            for (int y = y0; y < y1; ++y)
                for (int x = 0; x < OW; ++x) {
                    float acc[3] = {0, 0, 0}, n[3] = {0, 0, 0};
                    for (int dy = 0; dy < 2; ++dy)
                        for (int dx = 0; dx < 2; ++dx) {
                            const int c = colourAt(2 * x + dx, 2 * y + dy);
                            acc[c] += std::min(1.0f, P(2 * x + dx, 2 * y + dy)), n[c] += 1;
                        }
                    for (int c = 0; c < 3; ++c) rgb[(static_cast<std::size_t>(y) * static_cast<std::size_t>(OW) + static_cast<std::size_t>(x)) * 3 + static_cast<std::size_t>(c)] = n[c] ? acc[c] / n[c] : 0;
                }
        });
    } else {
        parallelRows(H, static_cast<std::size_t>(W) * 40, [&](int y0, int y1) {
            for (int y = y0; y < y1; ++y)
                for (int x = 0; x < W; ++x) {
                    const int c = colourAt(x, y);
                    const float C = P(x, y);
                    const float n4 = P(x - 1, y) + P(x + 1, y) + P(x, y - 1) + P(x, y + 1);
                    const float d4 = P(x - 1, y - 1) + P(x + 1, y - 1) + P(x - 1, y + 1) + P(x + 1, y + 1);
                    const float far4 = P(x - 2, y) + P(x + 2, y) + P(x, y - 2) + P(x, y + 2);
                    float out[3];
                    out[c] = C;
                    if (c == 1) {
                        // green site: which colour sits left/right?
                        const int rowColour = colourAt(x + 1, y);
                        const float h2 = P(x - 2, y) + P(x + 2, y), v2 = P(x, y - 2) + P(x, y + 2);
                        const float horiz = (5 * C + 4 * (P(x - 1, y) + P(x + 1, y)) - h2 - d4 + 0.5f * v2) / 8;
                        const float vert = (5 * C + 4 * (P(x, y - 1) + P(x, y + 1)) - v2 - d4 + 0.5f * h2) / 8;
                        out[rowColour] = horiz;
                        out[2 - rowColour] = vert;
                    } else {
                        out[1] = (4 * C + 2 * n4 - far4) / 8;
                        out[2 - c] = (6 * C + 2 * d4 - 1.5f * far4) / 8;
                    }
                    for (int k = 0; k < 3; ++k)
                        rgb[(static_cast<std::size_t>(y) * static_cast<std::size_t>(W) + static_cast<std::size_t>(x)) * 3 + static_cast<std::size_t>(k)] = std::clamp(out[k], 0.0f, 1.0f);
                }
        });
    }
    plane.clear();
    plane.shrink_to_fit();

    // camera RGB -> linear sRGB -> develop settings -> sRGB
    const float gain = static_cast<float>(std::pow(2.0, set.exposure + number(t, d.main, BaselineExposure, 0)));
    const float hl = static_cast<float>(std::clamp(set.highlights, -100.0, 100.0) / 100);
    const float sh = static_cast<float>(std::clamp(set.shadows, -100.0, 100.0) / 100);
    const float con = static_cast<float>(std::clamp(set.contrast, -100.0, 100.0) / 100);
    const float sat = static_cast<float>(1 + std::clamp(set.saturation, -100.0, 100.0) / 100);
    // tone curve on display values, as a table
    std::vector<std::uint8_t> tone(4097);
    for (int i = 0; i <= 4096; ++i) {
        float v = i / 4096.0f * 2;  // linear 0..2 so highlights have room to roll off
        // highlight shoulder: values above 0.75 compress smoothly toward 1 (more with negative highlights)
        const float knee = 0.75f + 0.2f * std::max(0.0f, hl);
        if (v > knee) {
            const float room = 1 - knee, over = v - knee;
            const float strength = 1 + std::max(0.0f, -hl) * 2;
            v = knee + room * (1 - std::exp(-over * strength / room));
            if (hl > 0) v = std::min(1.0f, v * (1 + 0.15f * hl));
        }
        v = srgbEncode(std::clamp(v, 0.0f, 1.0f));
        if (sh != 0) v = std::clamp(v + sh * 0.35f * v * (1 - v) * (1 - v) * 2, 0.0f, 1.0f);
        const float k = 0.12f + con * 0.6f;  // gentle S by default, like every raw developer
        v = std::clamp(v + k * (v - 0.5f) * (1 - std::abs(2 * v - 1)), 0.0f, 1.0f);
        tone[static_cast<std::size_t>(i)] = static_cast<std::uint8_t>(std::lround(v * 255));
    }
    Image img(OW, OH, {0, 0, 0, 255});
    const M3& m = colour.camToSrgb;
    parallelRows(OH, static_cast<std::size_t>(OW) * 30, [&](int y0, int y1) {
        for (int y = y0; y < y1; ++y) {
            std::uint8_t* row = img.pixel(0, y);
            for (int x = 0; x < OW; ++x) {
                const float* c = &rgb[(static_cast<std::size_t>(y) * static_cast<std::size_t>(OW) + static_cast<std::size_t>(x)) * 3];
                float r = static_cast<float>(m[0] * c[0] + m[1] * c[1] + m[2] * c[2]) * gain;
                float g = static_cast<float>(m[3] * c[0] + m[4] * c[1] + m[5] * c[2]) * gain;
                float b = static_cast<float>(m[6] * c[0] + m[7] * c[1] + m[8] * c[2]) * gain;
                if (sat != 1) {
                    const float l = 0.2126f * r + 0.7152f * g + 0.0722f * b;
                    r = l + (r - l) * sat, g = l + (g - l) * sat, b = l + (b - l) * sat;
                }
                auto T = [&](float v) { return tone[static_cast<std::size_t>(std::clamp(v * 2048.0f, 0.0f, 4096.0f))]; };
                row[x * 4] = T(r), row[x * 4 + 1] = T(g), row[x * 4 + 2] = T(b);
            }
        }
    });
    rgb.clear();

    // default crop (in active-area pixels)
    const std::vector<double> co = numbers(t, d.raw, DefaultCropOrigin), cs = numbers(t, d.raw, DefaultCropSize);
    if (co.size() >= 2 && cs.size() >= 2) {
        const double f = static_cast<double>(OW) / W;
        const int cx = static_cast<int>(co[0] * f), cy = static_cast<int>(co[1] * f);
        const int cw = std::min(OW - cx, static_cast<int>(cs[0] * f)), ch = std::min(OH - cy, static_cast<int>(cs[1] * f));
        if (cx >= 0 && cy >= 0 && cw > 0 && ch > 0 && (cw != OW || ch != OH)) img = crop(img, cx, cy, cw, ch);
    }
    switch (static_cast<int>(number(t, d.main, Orientation, 1))) {
        case 2: img = flipHorizontal(img); break;
        case 3: img = rotate90(img, 2); break;
        case 4: img = flipVertical(img); break;
        case 5: img = flipHorizontal(rotate90(img, 1)); break;
        case 6: img = rotate90(img, 1); break;
        case 7: img = flipHorizontal(rotate90(img, 3)); break;
        case 8: img = rotate90(img, 3); break;
        default: break;
    }
    return img;
}

}  // namespace wetwaresuite::raster
