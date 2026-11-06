// Animated GIF writer: a shared "most used colours" palette and GIF's flavour of LZW.
#include <algorithm>
#include <array>
#include <cstdint>
#include <map>
#include <stdexcept>
#include <vector>

#include "wetwaresuite/raster/codec.hpp"

namespace wetwaresuite::raster {

namespace {

class BitWriter {
public:
    std::string bytes;
    void put(int code, int bits) {
        acc_ |= static_cast<std::uint32_t>(code) << n_;
        n_ += bits;
        while (n_ >= 8) {
            bytes += static_cast<char>(acc_ & 0xFF);
            acc_ >>= 8;
            n_ -= 8;
        }
    }
    void flush() {
        if (n_ > 0) bytes += static_cast<char>(acc_ & 0xFF);
        acc_ = 0;
        n_ = 0;
    }

private:
    std::uint32_t acc_ = 0;
    int n_ = 0;
};

std::string lzw(const std::vector<std::uint8_t>& idx, int minBits) {
    const int clear = 1 << minBits, end = clear + 1;
    BitWriter w;
    // (prefix code << 8 | next index) -> code; a flat table is much quicker than a map
    std::vector<int> table(4096 * 256, -1);
    std::vector<int> used;
    int next = end + 1, bits = minBits + 1;
    w.put(clear, bits);
    if (idx.empty()) {
        w.put(end, bits);
        w.flush();
        return w.bytes;
    }
    int prefix = idx[0];
    for (std::size_t i = 1; i < idx.size(); ++i) {
        const int c = idx[i];
        const int key = prefix << 8 | c;
        if (table[static_cast<std::size_t>(key)] >= 0) {
            prefix = table[static_cast<std::size_t>(key)];
            continue;
        }
        w.put(prefix, bits);
        if (next < 4096) {
            table[static_cast<std::size_t>(key)] = next++;
            used.push_back(key);
            if (next > (1 << bits) && bits < 12) ++bits;
        } else {
            w.put(clear, bits);
            for (int k : used) table[static_cast<std::size_t>(k)] = -1;
            used.clear();
            next = end + 1;
            bits = minBits + 1;
        }
        prefix = c;
    }
    w.put(prefix, bits);
    w.put(end, bits);
    w.flush();
    return w.bytes;
}

int bin(Color c) { return (c.r >> 3) << 10 | (c.g >> 3) << 5 | (c.b >> 3); }

}  // namespace

std::string encodeGif(const std::vector<Image>& frames, int delayCs) {
    if (frames.empty()) throw std::runtime_error("no frames to write");
    const int w = frames[0].width(), h = frames[0].height();
    if (w <= 0 || h <= 0 || w > 65535 || h > 65535) throw std::runtime_error("bad GIF size");
    // count colours in 15-bit bins over (a sample of) every frame
    std::vector<std::uint32_t> count(32768, 0);
    std::vector<std::array<std::uint64_t, 3>> sum(32768, {0, 0, 0});
    bool transparent = false;
    const int stepPx = std::max(1, static_cast<int>(static_cast<std::int64_t>(w) * h * static_cast<std::int64_t>(frames.size()) / 4000000));
    for (const Image& f : frames) {
        if (f.width() != w || f.height() != h) throw std::runtime_error("GIF frames must all be the same size");
        for (int y = 0; y < h; y += 1) {
            for (int x = (y * 7) % stepPx; x < w; x += stepPx) {
                const Color c = f.at(x, y);
                if (c.a < 128) {
                    transparent = true;
                    continue;
                }
                const int b = bin(c);
                ++count[static_cast<std::size_t>(b)];
                sum[static_cast<std::size_t>(b)][0] += c.r;
                sum[static_cast<std::size_t>(b)][1] += c.g;
                sum[static_cast<std::size_t>(b)][2] += c.b;
            }
        }
    }
    std::vector<int> bins;
    for (int b = 0; b < 32768; ++b) {
        if (count[static_cast<std::size_t>(b)]) bins.push_back(b);
    }
    std::sort(bins.begin(), bins.end(), [&](int a, int b) { return count[static_cast<std::size_t>(a)] > count[static_cast<std::size_t>(b)]; });
    const std::size_t room = transparent ? 255 : 256;
    if (bins.size() > room) bins.resize(room);
    std::vector<Color> palette;
    if (transparent) palette.push_back(Color{0, 0, 0, 0});
    for (int b : bins) {
        const auto n = count[static_cast<std::size_t>(b)];
        const auto& s = sum[static_cast<std::size_t>(b)];
        palette.push_back(Color{static_cast<std::uint8_t>(s[0] / n), static_cast<std::uint8_t>(s[1] / n), static_cast<std::uint8_t>(s[2] / n), 255});
    }
    if (palette.empty()) palette.push_back(Color{0, 0, 0, 255});
    // every bin maps to its nearest palette colour
    std::vector<std::uint8_t> lookup(32768, 0);
    const std::size_t first = transparent ? 1 : 0;
    for (int b = 0; b < 32768; ++b) {
        const int r = (b >> 10 & 31) * 8 + 4, g = (b >> 5 & 31) * 8 + 4, bl = (b & 31) * 8 + 4;
        int best = static_cast<int>(first), bestD = 1 << 30;
        for (std::size_t i = first; i < palette.size(); ++i) {
            const int dr = r - palette[i].r, dg = g - palette[i].g, db = bl - palette[i].b;
            const int d = 2 * dr * dr + 4 * dg * dg + 3 * db * db;
            if (d < bestD) bestD = d, best = static_cast<int>(i);
        }
        lookup[static_cast<std::size_t>(b)] = static_cast<std::uint8_t>(best);
    }
    int bits = 1;
    while ((1 << bits) < static_cast<int>(palette.size())) ++bits;
    std::string out = "GIF89a";
    auto word = [&](int v) {
        out += static_cast<char>(v & 0xFF);
        out += static_cast<char>(v >> 8 & 0xFF);
    };
    word(w);
    word(h);
    out += static_cast<char>(0x80 | 0x70 | (bits - 1));
    out += '\0';
    out += '\0';
    for (int i = 0; i < (1 << bits); ++i) {
        const Color c = i < static_cast<int>(palette.size()) ? palette[static_cast<std::size_t>(i)] : Color{0, 0, 0, 255};
        out += static_cast<char>(c.r);
        out += static_cast<char>(c.g);
        out += static_cast<char>(c.b);
    }
    out += "\x21\xFF\x0BNETSCAPE2.0\x03\x01";
    word(0);
    out += '\0';
    std::vector<std::uint8_t> idx(static_cast<std::size_t>(w) * h);
    for (const Image& f : frames) {
        out += "\x21\xF9\x04";
        out += static_cast<char>(transparent ? 0x09 : 0x04);  // keep (or clear, with transparency) between frames
        word(std::max(2, delayCs));
        out += '\0';
        out += '\0';
        out += ',';
        word(0);
        word(0);
        word(w);
        word(h);
        out += '\0';
        for (int y = 0; y < h; ++y) {
            for (int x = 0; x < w; ++x) {
                const Color c = f.at(x, y);
                idx[static_cast<std::size_t>(y) * w + x] = transparent && c.a < 128 ? 0 : lookup[static_cast<std::size_t>(bin(c))];
            }
        }
        const int minBits = std::max(2, bits);
        out += static_cast<char>(minBits);
        const std::string data = lzw(idx, minBits);
        for (std::size_t i = 0; i < data.size(); i += 255) {
            const std::size_t n = std::min<std::size_t>(255, data.size() - i);
            out += static_cast<char>(n);
            out.append(data, i, n);
        }
        out += '\0';
    }
    out += ';';
    return out;
}

}  // namespace wetwaresuite::raster
