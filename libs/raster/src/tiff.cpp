// Baseline TIFF: reads 1/2/4/8/16-bit and 32-bit float images (gray, RGB, palette, CMYK, with
// or without alpha), uncompressed, PackBits, LZW or Deflate, in strips or tiles, chunky or planar;
// writes 8-bit RGB(A) with Deflate. The first page of a multi-page file is read.
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>

#include "wetwaresuite/raster/codec.hpp"

#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#endif
#include "stb_image.h"
#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic pop
#endif

// Defined (and exported) by stb_image_write in codec.cpp.
extern "C" unsigned char* stbi_zlib_compress(unsigned char* data, int data_len, int* out_len, int quality);

namespace wetwaresuite::raster {

namespace {

[[noreturn]] void bad(const std::string& why) { throw std::runtime_error("TIFF: " + why); }

struct Reader {
    const std::string& b;
    bool le = true;
    std::uint32_t u8(std::size_t at) const {
        if (at >= b.size()) bad("file is cut short");
        return static_cast<unsigned char>(b[at]);
    }
    std::uint32_t u16(std::size_t at) const { return le ? u8(at) | (u8(at + 1) << 8) : (u8(at) << 8) | u8(at + 1); }
    std::uint32_t u32(std::size_t at) const {
        return le ? u16(at) | (u16(at + 2) << 16) : (u16(at) << 16) | u16(at + 2);
    }
};

struct Entry {
    std::uint32_t type = 0, count = 0;
    std::size_t at = 0;  // where the values are (inline or at the offset)
};

std::size_t typeSize(std::uint32_t type) {
    switch (type) {
        case 1: case 2: case 6: case 7: return 1;  // BYTE ASCII SBYTE UNDEFINED
        case 3: case 8: return 2;                  // SHORT SSHORT
        case 4: case 9: case 11: case 13: return 4; // LONG SLONG FLOAT IFD
        case 5: case 10: case 12: return 8;        // RATIONAL SRATIONAL DOUBLE
        default: return 0;
    }
}

std::vector<std::uint32_t> values(const Reader& r, const Entry& e) {
    std::vector<std::uint32_t> out;
    const std::size_t sz = typeSize(e.type);
    if (!sz || e.count > (1u << 26)) return out;
    out.reserve(e.count);
    for (std::uint32_t i = 0; i < e.count; ++i) {
        const std::size_t at = e.at + i * sz;
        out.push_back(sz == 1 ? r.u8(at) : sz == 2 ? r.u16(at) : r.u32(at));
    }
    return out;
}

std::string packBits(const std::string& in, std::size_t expect) {
    std::string out;
    out.reserve(expect);
    std::size_t i = 0;
    while (i < in.size() && out.size() < expect) {
        const auto n = static_cast<signed char>(in[i++]);
        if (n >= 0) {
            const std::size_t len = static_cast<std::size_t>(n) + 1;
            out.append(in, i, std::min(len, in.size() - i));
            i += len;
        } else if (n != -128) {
            if (i >= in.size()) break;
            out.append(static_cast<std::size_t>(1 - n), in[i++]);
        }
    }
    return out;
}

// TIFF LZW: MSB-first codes, 256 = clear, 257 = end, code width grows one code early.
std::string lzw(const std::string& in, std::size_t expect) {
    std::string out;
    out.reserve(expect);
    std::vector<std::string> table;
    auto reset = [&] {
        table.assign(258, {});
        for (int i = 0; i < 256; ++i) table[static_cast<std::size_t>(i)] = std::string(1, static_cast<char>(i));
    };
    reset();
    std::uint32_t buf = 0;
    int bits = 0, width = 9;
    std::size_t pos = 0;
    int prev = -1;
    // old-style (LSB-first) LZW from very old writers starts with 0x00 0x01
    if (in.size() >= 2 && in[0] == 0 && (in[1] & 1)) bad("old-style LZW isn't supported");
    for (;;) {
        while (bits < width) {
            if (pos >= in.size()) return out;
            buf = (buf << 8) | static_cast<unsigned char>(in[pos++]);
            bits += 8;
        }
        const int code = static_cast<int>((buf >> (bits - width)) & ((1u << width) - 1));
        bits -= width;
        if (code == 257) break;
        if (code == 256) {
            reset();
            width = 9;
            prev = -1;
            continue;
        }
        std::string entry;
        if (code < static_cast<int>(table.size())) {
            entry = table[static_cast<std::size_t>(code)];
            if (prev >= 0 && table.size() < 4096) table.push_back(table[static_cast<std::size_t>(prev)] + entry[0]);
        } else if (prev >= 0 && code == static_cast<int>(table.size())) {
            const std::string& p = table[static_cast<std::size_t>(prev)];
            entry = p + p[0];
            if (table.size() < 4096) table.push_back(entry);
        } else {
            bad("corrupt LZW data");
        }
        out += entry;
        if (out.size() >= expect) break;
        prev = code;
        const std::size_t next = table.size() + 1;
        if (next >= 2048) width = 12;
        else if (next >= 1024) width = 11;
        else if (next >= 512) width = 10;
    }
    return out;
}

std::string inflate(const std::string& in, std::size_t expect) {
    int len = 0;
    char* out = stbi_zlib_decode_malloc_guesssize_headerflag(in.data(), static_cast<int>(in.size()),
                                                            static_cast<int>(std::min<std::size_t>(expect + 64, 1u << 30)), &len, 1);
    if (!out) out = stbi_zlib_decode_noheader_malloc(in.data(), static_cast<int>(in.size()), &len);
    if (!out) bad("corrupt Deflate data");
    std::string s(out, static_cast<std::size_t>(len));
    std::free(out);
    return s;
}

}  // namespace

Image decodeTiff(const std::string& bytes) {
    if (bytes.size() < 8) bad("not a TIFF file");
    Reader r{bytes, bytes[0] == 'I'};
    if (!((bytes[0] == 'I' && bytes[1] == 'I') || (bytes[0] == 'M' && bytes[1] == 'M'))) bad("not a TIFF file");
    const std::uint32_t magic = r.u16(2);
    if (magic == 43) bad("BigTIFF isn't supported");
    if (magic != 42) bad("not a TIFF file");
    const std::size_t ifd = r.u32(4);
    const std::uint32_t n = r.u16(ifd);
    std::vector<std::pair<std::uint32_t, Entry>> tags;
    for (std::uint32_t i = 0; i < n; ++i) {
        const std::size_t at = ifd + 2 + i * 12;
        Entry e;
        e.type = r.u16(at + 2);
        e.count = r.u32(at + 4);
        const std::size_t total = typeSize(e.type) * e.count;
        e.at = total <= 4 ? at + 8 : r.u32(at + 8);
        tags.push_back({r.u16(at), e});
    }
    auto find = [&](std::uint32_t tag) -> const Entry* {
        for (const auto& [t, e] : tags) {
            if (t == tag) return &e;
        }
        return nullptr;
    };
    auto list = [&](std::uint32_t tag) { const Entry* e = find(tag); return e ? values(r, *e) : std::vector<std::uint32_t>{}; };
    auto one = [&](std::uint32_t tag, std::uint32_t fallback) {
        auto v = list(tag);
        return v.empty() ? fallback : v[0];
    };

    const std::uint32_t W = one(256, 0), H = one(257, 0);
    if (W == 0 || H == 0 || W > 65535 || H > 65535 || static_cast<std::uint64_t>(W) * H > (1ull << 28)) bad("bad image size");
    const std::uint32_t spp = one(277, 1);
    auto bpsList = list(258);
    const std::uint32_t bps = bpsList.empty() ? 1 : bpsList[0];
    const std::uint32_t compression = one(259, 1);
    const std::uint32_t photometric = one(262, spp >= 3 ? 2 : 1);
    const std::uint32_t planar = one(284, 1);
    const std::uint32_t predictor = one(317, 1);
    const std::uint32_t sampleFormat = one(339, 1);
    const auto extra = list(338);
    if (spp < 1 || spp > 8) bad("unsupported samples per pixel");
    if (!(bps == 1 || bps == 2 || bps == 4 || bps == 8 || bps == 16 || (bps == 32 && sampleFormat == 3))) {
        bad(std::to_string(bps) + "-bit samples aren't supported");
    }
    if (compression == 7 || compression == 6) bad("JPEG-compressed TIFF isn't supported (save it as an uncompressed, LZW or Deflate TIFF)");
    if (!(compression == 1 || compression == 5 || compression == 8 || compression == 32946 || compression == 32773)) {
        bad("compression " + std::to_string(compression) + " isn't supported");
    }
    if (photometric == 6) bad("YCbCr TIFF isn't supported");

    const bool tiled = find(322) != nullptr;
    const std::uint32_t cw = tiled ? one(322, 0) : W;
    const std::uint32_t ch = tiled ? one(323, 0) : std::min(one(278, H), H);
    if (cw == 0 || ch == 0) bad("bad strip or tile size");
    const auto offsets = list(tiled ? 324 : 273);
    const auto counts = list(tiled ? 325 : 279);
    if (offsets.empty()) bad("no image data");

    const std::uint32_t planes = planar == 2 ? spp : 1;
    const std::uint32_t chunkSpp = planar == 2 ? 1 : spp;
    const std::size_t chunkRow = (static_cast<std::size_t>(cw) * chunkSpp * bps + 7) / 8;
    const std::size_t imageRow = (static_cast<std::size_t>(W) * chunkSpp * bps + 7) / 8;
    const std::uint32_t across = (W + cw - 1) / cw, down = (H + ch - 1) / ch;
    std::vector<std::string> plane(planes, std::string(imageRow * H, '\0'));

    for (std::uint32_t p = 0; p < planes; ++p) {
        for (std::uint32_t ty = 0; ty < down; ++ty) {
            for (std::uint32_t tx = 0; tx < across; ++tx) {
                const std::size_t idx = (static_cast<std::size_t>(p) * down + ty) * across + tx;
                if (idx >= offsets.size()) continue;
                const std::size_t off = offsets[idx];
                std::size_t len = idx < counts.size() ? counts[idx] : bytes.size() - std::min(off, bytes.size());
                if (off >= bytes.size()) continue;
                len = std::min(len, bytes.size() - off);
                const std::string raw = bytes.substr(off, len);
                const std::size_t expect = chunkRow * ch;
                std::string data = compression == 1       ? raw
                                   : compression == 5     ? lzw(raw, expect)
                                   : compression == 32773 ? packBits(raw, expect)
                                                          : inflate(raw, expect);
                if (data.size() < expect) data.resize(expect, '\0');
                if (predictor == 2 && (bps == 8 || bps == 16)) {
                    const std::size_t step = chunkSpp * (bps / 8);
                    for (std::uint32_t y = 0; y < ch; ++y) {
                        auto* row = reinterpret_cast<unsigned char*>(&data[y * chunkRow]);
                        if (bps == 8) {
                            for (std::size_t i = step; i < chunkRow; ++i) row[i] = static_cast<unsigned char>(row[i] + row[i - step]);
                        } else {
                            for (std::size_t i = step; i + 1 < chunkRow; i += 2) {
                                auto get = [&](std::size_t k) {
                                    return r.le ? row[k] | (row[k + 1] << 8) : (row[k] << 8) | row[k + 1];
                                };
                                const unsigned v = (get(i) + get(i - step)) & 0xFFFF;
                                if (r.le) {
                                    row[i] = static_cast<unsigned char>(v);
                                    row[i + 1] = static_cast<unsigned char>(v >> 8);
                                } else {
                                    row[i] = static_cast<unsigned char>(v >> 8);
                                    row[i + 1] = static_cast<unsigned char>(v);
                                }
                            }
                        }
                    }
                } else if (predictor == 3) {
                    bad("floating-point predictor isn't supported");
                }
                // copy the chunk's rows into the plane (tiles may hang over the right/bottom edge)
                const std::size_t x0bits = static_cast<std::size_t>(tx) * cw * chunkSpp * bps;
                for (std::uint32_t y = 0; y < ch; ++y) {
                    const std::size_t iy = static_cast<std::size_t>(ty) * ch + y;
                    if (iy >= H) break;
                    if (x0bits % 8 == 0) {
                        const std::size_t dst = iy * imageRow + x0bits / 8;
                        const std::size_t n = std::min(chunkRow, imageRow - x0bits / 8);
                        std::memcpy(&plane[p][dst], &data[y * chunkRow], n);
                    }
                }
            }
        }
    }

    // a sample as 0..1
    auto sampleAt = [&](std::uint32_t x, std::uint32_t y, std::uint32_t s) -> double {
        const std::string& pl = plane[planar == 2 ? s : 0];
        const std::size_t si = planar == 2 ? x : static_cast<std::size_t>(x) * spp + s;
        const auto* row = reinterpret_cast<const unsigned char*>(pl.data()) + y * imageRow;
        switch (bps) {
            case 8: return row[si] / 255.0;
            case 16: {
                const unsigned v = r.le ? row[si * 2] | (row[si * 2 + 1] << 8) : (row[si * 2] << 8) | row[si * 2 + 1];
                return v / 65535.0;
            }
            case 32: {
                std::uint32_t v = r.le ? row[si * 4] | (row[si * 4 + 1] << 8) | (row[si * 4 + 2] << 16) | (std::uint32_t{row[si * 4 + 3]} << 24)
                                       : (std::uint32_t{row[si * 4]} << 24) | (row[si * 4 + 1] << 16) | (row[si * 4 + 2] << 8) | row[si * 4 + 3];
                float f;
                std::memcpy(&f, &v, 4);
                return std::isfinite(f) ? std::clamp(static_cast<double>(f), 0.0, 1.0) : 0.0;
            }
            default: {  // 1, 2, 4 bits, MSB first
                const std::size_t bit = si * bps;
                const unsigned v = (row[bit / 8] >> (8 - bps - bit % 8)) & ((1u << bps) - 1);
                return v / static_cast<double>((1u << bps) - 1);
            }
        }
    };
    auto rawAt = [&](std::uint32_t x, std::uint32_t y) -> unsigned {  // palette index
        return static_cast<unsigned>(std::lround(sampleAt(x, y, 0) * ((1u << std::min(bps, 16u)) - 1)));
    };

    const auto colormap = list(320);
    const std::uint32_t colorSamples = photometric == 2 ? 3 : photometric == 5 ? 4 : 1;
    const bool hasAlpha = spp > colorSamples && !extra.empty() && (extra[0] == 1 || extra[0] == 2);
    const bool premultiplied = hasAlpha && extra[0] == 1;
    if (photometric == 2 && spp < 3) bad("RGB image with fewer than 3 samples");
    if (photometric == 3 && colormap.size() < 3u * (1u << std::min(bps, 16u))) bad("palette image without a colour map");

    Image img(static_cast<int>(W), static_cast<int>(H));
    auto to8 = [](double v) { return static_cast<std::uint8_t>(std::lround(std::clamp(v, 0.0, 1.0) * 255)); };
    for (std::uint32_t y = 0; y < H; ++y) {
        for (std::uint32_t x = 0; x < W; ++x) {
            double rr = 0, gg = 0, bb = 0, a = 1;
            if (photometric == 3) {
                const std::size_t entries = std::size_t{1} << std::min(bps, 16u);
                const std::size_t i = std::min<std::size_t>(rawAt(x, y), entries - 1);
                rr = colormap[i] / 65535.0;
                gg = colormap[entries + i] / 65535.0;
                bb = colormap[2 * entries + i] / 65535.0;
            } else if (photometric == 2) {
                rr = sampleAt(x, y, 0);
                gg = sampleAt(x, y, 1);
                bb = sampleAt(x, y, 2);
            } else if (photometric == 5 && spp >= 4) {
                const double c = sampleAt(x, y, 0), m = sampleAt(x, y, 1), ye = sampleAt(x, y, 2), k = sampleAt(x, y, 3);
                rr = (1 - c) * (1 - k);
                gg = (1 - m) * (1 - k);
                bb = (1 - ye) * (1 - k);
            } else {
                double v = sampleAt(x, y, 0);
                if (photometric == 0) v = 1 - v;
                rr = gg = bb = v;
            }
            if (hasAlpha) {
                a = sampleAt(x, y, colorSamples);
                if (premultiplied && a > 0) {
                    rr /= a;
                    gg /= a;
                    bb /= a;
                }
            }
            std::uint8_t* px = img.pixel(static_cast<int>(x), static_cast<int>(y));
            px[0] = to8(rr);
            px[1] = to8(gg);
            px[2] = to8(bb);
            px[3] = to8(a);
        }
    }
    return img;
}

std::string encodeTiff(const Image& img, int bits) {
    if (img.empty()) throw std::runtime_error("image is empty");
    const int W = img.width(), H = img.height();
    bool opaque = true;
    for (int y = 0; y < H && opaque; ++y) {
        for (int x = 0; x < W; ++x) {
            if (img.pixel(x, y)[3] != 255) {
                opaque = false;
                break;
            }
        }
    }
    const int spp = opaque ? 3 : 4;
    const bool deep = bits == 16;
    const int bps = deep ? 2 : 1;  // bytes per sample
    const int rowsPerStrip = std::max(1, std::min(H, 65536 / std::max(1, W * spp * bps)));
    const std::size_t rowBytes = static_cast<std::size_t>(W) * spp * bps;

    std::vector<std::string> strips;
    for (int y0 = 0; y0 < H; y0 += rowsPerStrip) {
        std::string raw;
        for (int y = y0; y < std::min(H, y0 + rowsPerStrip); ++y) {
            std::string row(rowBytes, '\0');
            if (deep) {  // 16-bit little-endian samples (v * 257), predictor on whole samples
                std::vector<std::uint16_t> v(static_cast<std::size_t>(W) * spp);
                for (int x = 0; x < W; ++x) {
                    const std::uint8_t* p = img.pixel(x, y);
                    for (int s = 0; s < spp; ++s) v[static_cast<std::size_t>(x) * spp + s] = static_cast<std::uint16_t>(p[s] * 257);
                }
                for (std::size_t i = v.size(); i-- > static_cast<std::size_t>(spp);) v[i] = static_cast<std::uint16_t>(v[i] - v[i - spp]);
                for (std::size_t i = 0; i < v.size(); ++i) {
                    row[i * 2] = static_cast<char>(v[i] & 0xFF);
                    row[i * 2 + 1] = static_cast<char>(v[i] >> 8);
                }
            } else {
                for (int x = 0; x < W; ++x) {
                    const std::uint8_t* p = img.pixel(x, y);
                    for (int s = 0; s < spp; ++s) row[static_cast<std::size_t>(x) * spp + s] = static_cast<char>(p[s]);
                }
                for (std::size_t i = rowBytes; i-- > static_cast<std::size_t>(spp);) {  // horizontal predictor
                    row[i] = static_cast<char>(static_cast<unsigned char>(row[i]) - static_cast<unsigned char>(row[i - spp]));
                }
            }
            raw += row;
        }
        int len = 0;
        unsigned char* z = stbi_zlib_compress(reinterpret_cast<unsigned char*>(raw.data()), static_cast<int>(raw.size()), &len, 6);
        if (!z) throw std::runtime_error("TIFF compression failed");
        strips.emplace_back(reinterpret_cast<char*>(z), static_cast<std::size_t>(len));
        std::free(z);
    }

    std::string out = "II";
    auto put16 = [&](std::string& s, std::uint32_t v) {
        s += static_cast<char>(v & 0xFF);
        s += static_cast<char>((v >> 8) & 0xFF);
    };
    auto put32 = [&](std::string& s, std::uint32_t v) {
        put16(s, v & 0xFFFF);
        put16(s, v >> 16);
    };
    put16(out, 42);
    put32(out, 0);  // IFD offset, patched below

    // strip data first, then the out-of-line tag values, then the IFD
    std::vector<std::uint32_t> offsets, counts;
    for (const auto& s : strips) {
        offsets.push_back(static_cast<std::uint32_t>(out.size()));
        counts.push_back(static_cast<std::uint32_t>(s.size()));
        out += s;
        if (out.size() % 2) out += '\0';
    }
    auto block = [&](const std::vector<std::uint32_t>& v, bool shorts) {
        const std::uint32_t at = static_cast<std::uint32_t>(out.size());
        for (auto x : v) shorts ? put16(out, x) : put32(out, x);
        if (out.size() % 2) out += '\0';
        return at;
    };
    const std::uint32_t bpsAt = block(std::vector<std::uint32_t>(static_cast<std::size_t>(spp), deep ? 16u : 8u), true);
    const std::uint32_t offAt = strips.size() > 1 ? block(offsets, false) : offsets[0];
    const std::uint32_t cntAt = strips.size() > 1 ? block(counts, false) : counts[0];
    const std::uint32_t resAt = block({72, 1}, false);
    const std::string software = "wetwaresuite";
    const std::uint32_t swAt = static_cast<std::uint32_t>(out.size());
    out += software;
    out += '\0';
    if (out.size() % 2) out += '\0';

    struct Tag {
        std::uint16_t tag, type;
        std::uint32_t count, value;
    };
    std::vector<Tag> tags = {
        {256, 4, 1, static_cast<std::uint32_t>(W)},
        {257, 4, 1, static_cast<std::uint32_t>(H)},
        {258, 3, static_cast<std::uint32_t>(spp), bpsAt},
        {259, 3, 1, 8},  // Deflate
        {262, 3, 1, 2},  // RGB
        {273, 4, static_cast<std::uint32_t>(strips.size()), offAt},
        {277, 3, 1, static_cast<std::uint32_t>(spp)},
        {278, 4, 1, static_cast<std::uint32_t>(rowsPerStrip)},
        {279, 4, static_cast<std::uint32_t>(strips.size()), cntAt},
        {282, 5, 1, resAt},
        {283, 5, 1, resAt},
        {284, 3, 1, 1},
        {296, 3, 1, 2},  // inches
        {305, 2, static_cast<std::uint32_t>(software.size() + 1), swAt},
        {317, 3, 1, 2},  // horizontal predictor
    };
    if (!opaque) tags.push_back({338, 3, 1, 2});  // unassociated alpha
    const std::uint32_t ifd = static_cast<std::uint32_t>(out.size());
    put16(out, static_cast<std::uint32_t>(tags.size()));
    for (const auto& t : tags) {
        put16(out, t.tag);
        put16(out, t.type);
        put32(out, t.count);
        if (t.type == 3 && t.count == 1) {
            put16(out, t.value);
            put16(out, 0);
        } else {
            put32(out, t.value);
        }
    }
    put32(out, 0);
    for (int i = 0; i < 4; ++i) out[4 + static_cast<std::size_t>(i)] = static_cast<char>((ifd >> (8 * i)) & 0xFF);
    return out;
}

}  // namespace wetwaresuite::raster
