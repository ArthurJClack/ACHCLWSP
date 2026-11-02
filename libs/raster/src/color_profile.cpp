#include "wetwaresuite/raster/color_profile.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>

#include "wetwaresuite/raster/codec.hpp"
#include "wetwaresuite/raster/parallel.hpp"

namespace wetwaresuite::raster {

namespace {

constexpr int kCurve = 4096;

std::uint32_t be32(const std::string& s, std::size_t at) {
    if (at + 4 > s.size()) return 0;
    auto b = [&](std::size_t i) { return static_cast<std::uint32_t>(static_cast<unsigned char>(s[at + i])); };
    return (b(0) << 24) | (b(1) << 16) | (b(2) << 8) | b(3);
}
std::uint32_t be16(const std::string& s, std::size_t at) {
    if (at + 2 > s.size()) return 0;
    return (static_cast<std::uint32_t>(static_cast<unsigned char>(s[at])) << 8) | static_cast<unsigned char>(s[at + 1]);
}
std::uint32_t le32(const std::string& s, std::size_t at) {
    if (at + 4 > s.size()) return 0;
    auto b = [&](std::size_t i) { return static_cast<std::uint32_t>(static_cast<unsigned char>(s[at + i])); };
    return b(0) | (b(1) << 8) | (b(2) << 16) | (b(3) << 24);
}
double s15(const std::string& s, std::size_t at) { return static_cast<std::int32_t>(be32(s, at)) / 65536.0; }

float linearToSrgb(float v) { return v <= 0.0031308f ? v * 12.92f : 1.055f * std::pow(v, 1 / 2.4f) - 0.055f; }

// XYZ (D50, as ICC profiles store it) -> linear sRGB, Bradford-adapted
constexpr float kXyzToSrgb[9] = {3.1338561f, -1.6168667f, -0.4906146f, -0.9787684f, 1.9161415f,
                                 0.0334540f, 0.0719453f,  -0.2289914f, 1.4052427f};

struct Tag {
    std::size_t offset = 0, size = 0;
    bool found = false;
};

Tag findTag(const std::string& icc, const char* sig) {
    const std::uint32_t want = (static_cast<std::uint32_t>(sig[0]) << 24) | (static_cast<std::uint32_t>(sig[1]) << 16) |
                               (static_cast<std::uint32_t>(sig[2]) << 8) | static_cast<std::uint32_t>(sig[3]);
    const std::uint32_t n = std::min<std::uint32_t>(be32(icc, 128), 200);
    for (std::uint32_t i = 0; i < n; ++i) {
        const std::size_t e = 132 + std::size_t{i} * 12;
        if (be32(icc, e) != want) continue;
        Tag t{be32(icc, e + 4), be32(icc, e + 8), true};
        if (t.offset + t.size > icc.size() || t.size < 8) return {};
        return t;
    }
    return {};
}

std::string readDescription(const std::string& icc) {
    const Tag t = findTag(icc, "desc");
    if (!t.found) return "";
    const std::string type = icc.substr(t.offset, 4);
    if (type == "desc") {
        const std::size_t n = std::min<std::size_t>(be32(icc, t.offset + 8), t.size - 12);
        std::string s = icc.substr(t.offset + 12, n);
        while (!s.empty() && s.back() == '\0') s.pop_back();
        return s;
    }
    if (type == "mluc" && be32(icc, t.offset + 8) > 0) {
        const std::size_t len = be32(icc, t.offset + 20), at = t.offset + be32(icc, t.offset + 24);
        std::string s;
        for (std::size_t i = 0; i + 1 < len && at + i + 1 < icc.size(); i += 2) {
            const std::uint32_t c = be16(icc, at + i);
            if (c == 0) break;
            s += c < 128 ? static_cast<char>(c) : '?';
        }
        return s;
    }
    return "";
}

bool readXyz(const std::string& icc, const char* sig, float* out) {
    const Tag t = findTag(icc, sig);
    if (!t.found || t.size < 20 || icc.compare(t.offset, 4, "XYZ ") != 0) return false;
    for (int i = 0; i < 3; ++i) out[i] = static_cast<float>(s15(icc, t.offset + 8 + 4 * static_cast<std::size_t>(i)));
    return true;
}

bool readCurve(const std::string& icc, const char* sig, std::vector<float>& out) {
    const Tag t = findTag(icc, sig);
    if (!t.found) return false;
    out.assign(kCurve, 0);
    if (icc.compare(t.offset, 4, "curv") == 0) {
        const std::uint32_t n = be32(icc, t.offset + 8);
        if (t.offset + 12 + std::size_t{n} * 2 > icc.size()) return false;
        if (n == 0) {
            for (int i = 0; i < kCurve; ++i) out[static_cast<std::size_t>(i)] = i / float(kCurve - 1);
        } else if (n == 1) {
            const float g = static_cast<float>(be16(icc, t.offset + 12)) / 256.0f;
            for (int i = 0; i < kCurve; ++i) out[static_cast<std::size_t>(i)] = std::pow(i / float(kCurve - 1), g);
        } else {
            for (int i = 0; i < kCurve; ++i) {
                const double x = i / double(kCurve - 1) * (n - 1);
                const std::size_t a = std::min<std::size_t>(static_cast<std::size_t>(x), n - 2);
                const double f = x - static_cast<double>(a);
                const double va = be16(icc, t.offset + 12 + a * 2), vb = be16(icc, t.offset + 12 + (a + 1) * 2);
                out[static_cast<std::size_t>(i)] = static_cast<float>((va + (vb - va) * f) / 65535.0);
            }
        }
        return true;
    }
    if (icc.compare(t.offset, 4, "para") == 0) {
        const std::uint32_t type = be16(icc, t.offset + 8);
        static constexpr int counts[] = {1, 3, 4, 5, 7};
        if (type > 4) return false;
        double p[7] = {1, 1, 0, 0, 0, 0, 0};
        for (int i = 0; i < counts[type]; ++i) p[i] = s15(icc, t.offset + 12 + 4 * static_cast<std::size_t>(i));
        const double g = p[0], a = p[1], b = p[2], c = p[3], d = p[4], e = p[5], f = p[6];
        for (int i = 0; i < kCurve; ++i) {
            const double x = i / double(kCurve - 1);
            double y = 0;
            switch (type) {
                case 0: y = std::pow(x, g); break;
                case 1: y = x >= -b / a ? std::pow(std::max(0.0, a * x + b), g) : 0; break;
                case 2: y = x >= -b / a ? std::pow(std::max(0.0, a * x + b), g) + c : c; break;
                case 3: y = x >= d ? std::pow(std::max(0.0, a * x + b), g) : c * x; break;
                default: y = x >= d ? std::pow(std::max(0.0, a * x + b), g) + e : c * x + f; break;
            }
            out[static_cast<std::size_t>(i)] = static_cast<float>(std::clamp(y, 0.0, 1.0));
        }
        return true;
    }
    return false;  // 'mAB ' and friends: not a simple curve
}

// One 8-bit RGB triple through the profile -> sRGB, as floats 0..255.
struct Converter {
    std::array<std::array<float, 256>, 3> lin{};
    float m[9] = {};  // device linear -> sRGB linear
    bool gray = false;
    std::vector<float> encode;  // linear 0..1 (kCurve steps) -> sRGB 0..255

    explicit Converter(const ColorProfile& p) {
        gray = p.gray;
        for (int c = 0; c < 3; ++c) {
            const auto& curve = p.curve[gray ? 0 : c];
            for (int v = 0; v < 256; ++v) lin[static_cast<std::size_t>(c)][static_cast<std::size_t>(v)] = curve[static_cast<std::size_t>(v * (kCurve - 1) / 255)];
        }
        for (int r = 0; r < 3; ++r)
            for (int c = 0; c < 3; ++c) {
                float s = 0;
                for (int k = 0; k < 3; ++k) s += kXyzToSrgb[r * 3 + k] * p.matrix[k * 3 + c];
                m[r * 3 + c] = s;
            }
        encode.resize(kCurve * 4 + 1);  // finer steps so dark tones don't band
        for (std::size_t i = 0; i < encode.size(); ++i) encode[i] = linearToSrgb(static_cast<float>(i) / float(kCurve * 4)) * 255.0f;
    }
    float enc(float v) const {
        v = std::clamp(v, 0.0f, 1.0f) * float(kCurve * 4);
        const std::size_t a = static_cast<std::size_t>(v);
        if (a >= encode.size() - 1) return encode.back();
        const float f = v - static_cast<float>(a);
        return encode[a] + (encode[a + 1] - encode[a]) * f;
    }
    void apply(std::uint8_t* px, float* out) const {
        const float r = lin[0][px[0]], g = lin[1][px[1]], b = lin[2][px[2]];
        if (gray) {
            out[0] = enc(r), out[1] = enc(g), out[2] = enc(b);
            return;
        }
        out[0] = enc(m[0] * r + m[1] * g + m[2] * b);
        out[1] = enc(m[3] * r + m[4] * g + m[5] * b);
        out[2] = enc(m[6] * r + m[7] * g + m[8] * b);
    }
};

std::string pngChunk(const std::string& bytes, const char* type) {
    if (bytes.size() < 8 || bytes.compare(1, 3, "PNG") != 0) return "";
    for (std::size_t at = 8; at + 8 <= bytes.size();) {
        const std::size_t len = be32(bytes, at);
        if (at + 12 + len > bytes.size()) break;
        if (bytes.compare(at + 4, 4, type) == 0) return bytes.substr(at + 8, len);
        if (bytes.compare(at + 4, 4, "IDAT") == 0) break;  // profile chunks come before the pixels
        at += 12 + len;
    }
    return "";
}

bool isTiff(const std::string& b) {
    return b.size() >= 8 && (b.compare(0, 4, "II*\0", 4) == 0 || b.compare(0, 4, "MM\0*", 4) == 0);
}

// first-IFD tag value(s) of a classic TIFF: returns the offset of the data and its count
bool tiffTag(const std::string& b, std::uint32_t want, std::uint32_t& type, std::uint32_t& count, std::size_t& dataAt) {
    const bool le = b[0] == 'I';
    auto u16 = [&](std::size_t at) { return le ? (at + 2 <= b.size() ? static_cast<std::uint32_t>(static_cast<unsigned char>(b[at])) | (static_cast<std::uint32_t>(static_cast<unsigned char>(b[at + 1])) << 8) : 0u) : be16(b, at); };
    auto u32 = [&](std::size_t at) { return le ? le32(b, at) : be32(b, at); };
    const std::size_t ifd = u32(4);
    const std::uint32_t n = u16(ifd);
    for (std::uint32_t i = 0; i < n && i < 1000; ++i) {
        const std::size_t e = ifd + 2 + std::size_t{i} * 12;
        if (e + 12 > b.size()) return false;
        if (u16(e) != want) continue;
        type = u16(e + 2), count = u32(e + 4);
        static constexpr std::uint32_t sizes[] = {0, 1, 1, 2, 4, 8, 1, 1, 2, 4, 8, 4, 8};
        const std::size_t bytes = std::size_t{count} * (type < 13 ? sizes[type] : 1);
        dataAt = bytes <= 4 ? e + 8 : u32(e + 8);
        return dataAt + bytes <= b.size();
    }
    return false;
}

}  // namespace

std::string findIccProfile(const std::string& b) {
    if (b.size() >= 8 && b.compare(1, 3, "PNG") == 0) {
        const std::string chunk = pngChunk(b, "iCCP");
        const std::size_t nul = chunk.find('\0');
        if (nul == std::string::npos || nul + 2 > chunk.size()) return "";
        try {
            return inflateZlib(chunk.substr(nul + 2));
        } catch (const std::exception&) {
            return "";
        }
    }
    if (b.size() >= 4 && static_cast<unsigned char>(b[0]) == 0xFF && static_cast<unsigned char>(b[1]) == 0xD8) {
        std::vector<std::string> parts(256);
        int total = 0;
        for (std::size_t at = 2; at + 4 <= b.size();) {
            if (static_cast<unsigned char>(b[at]) != 0xFF) break;
            const unsigned marker = static_cast<unsigned char>(b[at + 1]);
            if (marker == 0xDA || marker == 0xD9) break;
            const std::size_t len = be16(b, at + 2);
            if (len < 2 || at + 2 + len > b.size()) break;
            if (marker == 0xE2 && len > 16 && b.compare(at + 4, 12, std::string("ICC_PROFILE\0", 12)) == 0) {
                const int seq = static_cast<unsigned char>(b[at + 16]);
                total = static_cast<unsigned char>(b[at + 17]);
                parts[static_cast<std::size_t>(seq)] = b.substr(at + 18, len - 16);
            }
            at += 2 + len;
        }
        std::string icc;
        for (int i = 1; i <= total; ++i) icc += parts[static_cast<std::size_t>(i)];
        return icc;
    }
    if (isTiff(b)) {
        std::uint32_t type = 0, count = 0;
        std::size_t at = 0;
        if (tiffTag(b, 34675, type, count, at)) return b.substr(at, count);
        return "";
    }
    if (b.size() >= 12 && b.compare(0, 4, "RIFF") == 0 && b.compare(8, 4, "WEBP") == 0) {
        for (std::size_t at = 12; at + 8 <= b.size();) {
            const std::size_t len = le32(b, at + 4);
            if (at + 8 + len > b.size()) break;
            if (b.compare(at, 4, "ICCP") == 0) return b.substr(at + 8, len);
            at += 8 + len + (len & 1);
        }
    }
    return "";
}

ColorProfile parseIccProfile(const std::string& icc) {
    ColorProfile p;
    if (icc.size() < 132 || icc.compare(36, 4, "acsp") != 0) return p;
    p.present = true;
    p.description = readDescription(icc);
    if (icc.compare(20, 4, "XYZ ") != 0) return p;  // Lab PCS: LUT profiles only
    const std::string space = icc.substr(16, 4);
    if (space == "GRAY") {
        p.gray = p.convertible = readCurve(icc, "kTRC", p.curve[0]);
    } else if (space == "RGB ") {
        float r[3], g[3], b[3];
        p.convertible = readXyz(icc, "rXYZ", r) && readXyz(icc, "gXYZ", g) && readXyz(icc, "bXYZ", b) &&
                        readCurve(icc, "rTRC", p.curve[0]) && readCurve(icc, "gTRC", p.curve[1]) && readCurve(icc, "bTRC", p.curve[2]);
        for (int i = 0; i < 3; ++i) p.matrix[i * 3 + 0] = r[i], p.matrix[i * 3 + 1] = g[i], p.matrix[i * 3 + 2] = b[i];
    }
    if (!p.convertible) return p;
    // close enough to sRGB already? push a handful of colours through and compare
    Converter conv(p);
    float worst = 0;
    for (int r = 0; r < 256; r += 51)
        for (int g = 0; g < 256; g += 51)
            for (int b = 0; b < 256; b += 51) {
                std::uint8_t px[3] = {static_cast<std::uint8_t>(r), static_cast<std::uint8_t>(g), static_cast<std::uint8_t>(b)};
                float out[3];
                conv.apply(px, out);
                if (p.gray) {  // gray: compare against sRGB's own curve
                    for (int c = 0; c < 3; ++c) worst = std::max(worst, std::abs(out[c] - px[c]));
                } else {
                    worst = std::max({worst, std::abs(out[0] - float(r)), std::abs(out[1] - float(g)), std::abs(out[2] - float(b))});
                }
            }
    p.srgbLike = worst < 1.5f;
    return p;
}

bool convertToSrgb(Image& img, const ColorProfile& p) {
    if (!p.convertible || p.srgbLike || img.empty()) return false;
    const Converter conv(p);
    const int W = img.width();
    parallelRows(img.height(), static_cast<std::size_t>(W) * 20, [&](int y0, int y1) {
        for (int y = y0; y < y1; ++y) {
            std::uint8_t* row = img.pixel(0, y);
            for (int x = 0; x < W; ++x) {
                float out[3];
                conv.apply(row + x * 4, out);
                for (int c = 0; c < 3; ++c) row[x * 4 + c] = static_cast<std::uint8_t>(std::clamp(out[c] + 0.5f, 0.0f, 255.0f));
            }
        }
    });
    return true;
}

int fileBitDepth(const std::string& b) {
    if (b.size() >= 33 && b.compare(1, 3, "PNG") == 0) return static_cast<unsigned char>(b[24]) == 16 ? 16 : 8;
    if (b.size() >= 26 && b.compare(0, 4, "8BPS") == 0) return static_cast<int>(be16(b, 22));
    if (b.size() >= 10 && (b.compare(0, 10, "#?RADIANCE") == 0 || b.compare(0, 6, "#?RGBE") == 0)) return 32;
    if (isTiff(b)) {
        std::uint32_t type = 0, count = 0;
        std::size_t at = 0;
        if (!tiffTag(b, 258, type, count, at) || count == 0) return 8;
        const bool le = b[0] == 'I';
        const std::uint32_t bps = le ? static_cast<std::uint32_t>(static_cast<unsigned char>(b[at])) | (static_cast<std::uint32_t>(static_cast<unsigned char>(b[at + 1])) << 8)
                                     : be16(b, at);
        return bps >= 16 ? static_cast<int>(bps) : 8;
    }
    return 8;
}

FileColorInfo describeColor(const std::string& b) {
    FileColorInfo info;
    info.bits = fileBitDepth(b);
    const ColorProfile p = parseIccProfile(findIccProfile(b));
    if (p.present) info.profile = p.description.empty() ? "unnamed profile" : p.description;
    info.converted = p.convertible && !p.srgbLike;
    return info;
}

}  // namespace wetwaresuite::raster
