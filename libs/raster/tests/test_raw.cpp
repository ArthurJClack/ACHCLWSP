// Camera raw: builds small DNGs (uncompressed and lossless-JPEG tiles) from a known scene and
// checks the develop gets colours, white balance, exposure, crop and orientation right.
#include <cmath>
#include <cstdio>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "wetwaresuite/raster/codec.hpp"
#include "wetwaresuite/raster/raw.hpp"
#include "check.hpp"

using namespace wetwaresuite::raster;

namespace {

void put16(std::string& s, std::uint32_t v) {
    s += static_cast<char>(v & 0xFF);
    s += static_cast<char>((v >> 8) & 0xFF);
}
void put32(std::string& s, std::uint32_t v) {
    put16(s, v & 0xFFFF);
    put16(s, v >> 16);
}

// --- a tiny lossless JPEG encoder (predictor 1, every category coded in 5 bits) ---------------
struct BitOut {
    std::string out;
    std::uint32_t acc = 0;
    int n = 0;
    void put(std::uint32_t v, int bits) {
        for (int i = bits - 1; i >= 0; --i) {
            acc = (acc << 1) | ((v >> i) & 1);
            if (++n == 8) {
                out += static_cast<char>(acc);
                if ((acc & 0xFF) == 0xFF) out += '\0';
                acc = 0, n = 0;
            }
        }
    }
    void flush() {
        while (n) put(1, 1);
    }
};

std::string encodeLj92(const std::vector<std::uint16_t>& v, int width, int height, int comps) {
    std::string j = "\xFF\xD8";
    j += "\xFF\xC4";
    j += '\0', j += static_cast<char>(36), j += '\0';
    for (int i = 0; i < 16; ++i) j += static_cast<char>(i == 4 ? 17 : 0);
    for (int i = 0; i <= 16; ++i) j += static_cast<char>(i);
    j += "\xFF\xC3";
    j += '\0', j += static_cast<char>(8 + 3 * comps), j += static_cast<char>(16);
    j += static_cast<char>(height >> 8), j += static_cast<char>(height & 0xFF), j += static_cast<char>(width >> 8), j += static_cast<char>(width & 0xFF);
    j += static_cast<char>(comps);
    for (int c = 0; c < comps; ++c) j += static_cast<char>(c + 1), j += static_cast<char>(0x11), j += '\0';
    j += "\xFF\xDA";
    j += '\0', j += static_cast<char>(6 + 2 * comps), j += static_cast<char>(comps);
    for (int c = 0; c < comps; ++c) j += static_cast<char>(c + 1), j += '\0';
    j += static_cast<char>(1), j += '\0', j += '\0';
    BitOut b;
    const std::size_t rowLen = static_cast<std::size_t>(width * comps);
    for (int y = 0; y < height; ++y)
        for (int x = 0; x < width; ++x)
            for (int c = 0; c < comps; ++c) {
                const std::size_t i = static_cast<std::size_t>(y) * rowLen + static_cast<std::size_t>(x * comps + c);
                int pred = y == 0 ? (x == 0 ? 1 << 15 : v[i - static_cast<std::size_t>(comps)]) : (x == 0 ? v[i - rowLen] : v[i - static_cast<std::size_t>(comps)]);
                int diff = static_cast<std::int16_t>(static_cast<std::uint16_t>(v[i] - pred));
                int ssss = 0;
                for (int a = std::abs(diff); a; a >>= 1) ++ssss;
                if (diff == -32768) {
                    b.put(16, 5);
                    continue;
                }
                b.put(static_cast<std::uint32_t>(ssss), 5);
                if (ssss) b.put(static_cast<std::uint32_t>(diff >= 0 ? diff : diff + (1 << ssss) - 1), ssss);
            }
    b.flush();
    j += b.out;
    j += "\xFF\xD9";
    return j;
}

// --- DNG writer ---------------------------------------------------------------------------
struct TagValue {
    std::uint16_t type;
    std::vector<std::uint32_t> v;  // for rationals: num, den pairs
};

std::string writeDng(int W, int H, const std::vector<std::uint16_t>& cfa, bool lj92, std::map<std::uint16_t, TagValue> extra) {
    std::string out = "II";
    put16(out, 42);
    put32(out, 0);
    std::map<std::uint16_t, TagValue> tags = {
        {254, {4, {0}}}, {256, {4, {static_cast<std::uint32_t>(W)}}}, {257, {4, {static_cast<std::uint32_t>(H)}}}, {258, {3, {16}}},
        {259, {3, {lj92 ? 7u : 1u}}}, {262, {3, {32803}}}, {277, {3, {1}}}, {33421, {3, {2, 2}}}, {33422, {1, {0, 1, 1, 2}}},
        {50706, {1, {1, 4, 0, 0}}}, {50714, {4, {256}}}, {50717, {4, {4095}}}, {50708, {2, {}}},
    };
    const std::string model = "Test Cam";
    for (char c : model) tags[50708].v.push_back(static_cast<unsigned char>(c));
    tags[50708].v.push_back(0);
    for (auto& [k, val] : extra) tags[k] = val;
    if (lj92) {
        const int T = 16;  // 16x16 tiles, coded as 8 two-component columns
        std::vector<std::uint32_t> offs, lens;
        for (int ty = 0; ty < H; ty += T)
            for (int tx = 0; tx < W; tx += T) {
                std::vector<std::uint16_t> tile(static_cast<std::size_t>(T * T), 0);
                for (int y = 0; y < T; ++y)
                    for (int x = 0; x < T; ++x)
                        if (ty + y < H && tx + x < W) tile[static_cast<std::size_t>(y * T + x)] = cfa[static_cast<std::size_t>((ty + y) * W + tx + x)];
                const std::string j = encodeLj92(tile, T / 2, T, 2);
                offs.push_back(static_cast<std::uint32_t>(out.size()));
                lens.push_back(static_cast<std::uint32_t>(j.size()));
                out += j;
                if (out.size() % 2) out += '\0';
            }
        tags[322] = {3, {static_cast<std::uint32_t>(T)}};
        tags[323] = {3, {static_cast<std::uint32_t>(T)}};
        tags[324] = {4, offs};
        tags[325] = {4, lens};
    } else {
        const std::uint32_t at = static_cast<std::uint32_t>(out.size());
        for (auto v : cfa) put16(out, v);
        tags[273] = {4, {at}};
        tags[278] = {4, {static_cast<std::uint32_t>(H)}};
        tags[279] = {4, {static_cast<std::uint32_t>(cfa.size() * 2)}};
    }
    auto size = [](std::uint16_t t) { return t == 3 ? 2u : (t == 4 || t == 9) ? 4u : (t == 5 || t == 10) ? 4u : 1u; };
    // out-of-line values first
    std::map<std::uint16_t, std::uint32_t> where;
    for (auto& [k, val] : tags) {
        const std::uint32_t bytes = size(val.type) * static_cast<std::uint32_t>(val.v.size());
        if (bytes <= 4) continue;
        where[k] = static_cast<std::uint32_t>(out.size());
        for (auto x : val.v) size(val.type) == 1 ? void(out += static_cast<char>(x)) : size(val.type) == 2 ? put16(out, x) : put32(out, x);
        if (out.size() % 2) out += '\0';
    }
    const std::uint32_t ifd = static_cast<std::uint32_t>(out.size());
    put16(out, static_cast<std::uint32_t>(tags.size()));
    for (auto& [k, val] : tags) {
        put16(out, k);
        put16(out, val.type);
        const bool rational = val.type == 5 || val.type == 10;
        put32(out, static_cast<std::uint32_t>(rational ? val.v.size() / 2 : val.v.size()));
        if (where.count(k)) {
            put32(out, where[k]);
        } else {
            std::string inl;
            for (auto x : val.v) size(val.type) == 1 ? void(inl += static_cast<char>(x)) : size(val.type) == 2 ? put16(inl, x) : put32(inl, x);
            inl.resize(4, '\0');
            out += inl;
        }
    }
    put32(out, 0);
    for (int i = 0; i < 4; ++i) out[4 + static_cast<std::size_t>(i)] = static_cast<char>((ifd >> (8 * i)) & 0xFF);
    return out;
}

// scene in linear light: left half grey ramp, right half red / green / blue / white patches
void scene(int x, int y, int W, int H, double rgb[3]) {
    if (x < W / 2) {
        const double g = 0.05 + 0.5 * y / H;
        rgb[0] = rgb[1] = rgb[2] = g;
    } else {
        const int band = y * 4 / H;
        const double c[4][3] = {{0.5, 0.05, 0.05}, {0.05, 0.4, 0.05}, {0.05, 0.05, 0.5}, {0.6, 0.6, 0.6}};
        for (int k = 0; k < 3; ++k) rgb[k] = c[band][k];
    }
}

std::vector<std::uint16_t> mosaic(int W, int H, const double neutral[3]) {
    std::vector<std::uint16_t> v(static_cast<std::size_t>(W * H));
    const int pat[2][2] = {{0, 1}, {1, 2}};
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x) {
            double rgb[3];
            scene(x, y, W, H, rgb);
            const int c = pat[y & 1][x & 1];
            v[static_cast<std::size_t>(y * W + x)] = static_cast<std::uint16_t>(std::lround(256 + rgb[c] * neutral[c] * (4095 - 256)));
        }
    return v;
}

bool neutral(Color c, int tol = 6) { return std::abs(c.r - c.g) <= tol && std::abs(c.g - c.b) <= tol; }

}  // namespace

int main() {
    const int W = 64, H = 48;
    const double flat[3] = {1, 1, 1};
    const std::vector<std::uint16_t> cfa = mosaic(W, H, flat);
    const std::string dng = writeDng(W, H, cfa, false, {});
    CHECK(isDng(dng) && !isDng(encodeTiff(Image(4, 4, {1, 2, 3, 255}))));
    const RawInfo info = dngInfo(dng);
    CHECK(info.width == W && info.height == H && info.layout == "RGGB" && info.camera == "Test Cam" && info.bits == 16);

    const Image img = developDng(dng);
    CHECK(img.width() == W && img.height() == H);
    CHECK(neutral(img.at(10, 10)) && neutral(img.at(10, 40)) && img.at(10, 40).r > img.at(10, 10).r + 40);  // grey ramp stays grey
    const Color red = img.at(50, 4), green = img.at(50, 16), blue = img.at(50, 28), white = img.at(50, 40);
    CHECK(red.r > red.g + 80 && red.r > red.b + 80);
    CHECK(green.g > green.r + 60 && blue.b > blue.r + 80 && neutral(white));

    // decode() and load() go through the develop too
    CHECK(decode(dng).at(50, 4) == red);

    // lossless JPEG tiles develop to exactly the same picture
    const std::string lj = writeDng(W, H, cfa, true, {});
    CHECK(developDng(lj).at(50, 4) == red && developDng(lj).at(33, 21) == img.at(33, 21));
    {
        std::vector<std::uint16_t> v = {100, 200, 65535, 0, 7, 7, 1000, 999};
        std::vector<std::uint16_t> back;
        int w = 0, h = 0, c = 0;
        decodeLosslessJpeg(encodeLj92(v, 2, 2, 2), back, w, h, c);
        CHECK(w == 2 && h == 2 && c == 2 && back == v);
    }

    // as-shot white balance: a camera that sees red at half strength and blue at 0.8
    const double tinted[3] = {0.5, 1, 0.8};
    const std::string wb = writeDng(W, H, mosaic(W, H, tinted), false, {{50728, {5, {1, 2, 1, 1, 4, 5}}}});
    const Image balanced = developDng(wb);
    CHECK(neutral(balanced.at(10, 30)) && neutral(balanced.at(50, 40)));
    RawDevelop daylight;
    daylight.whiteBalance = "auto";
    CHECK(neutral(developDng(wb, daylight).at(10, 30), 12));

    // exposure, half size, contrast, saturation
    RawDevelop brighter;
    brighter.exposure = 1;
    CHECK(developDng(dng, brighter).at(10, 10).r > img.at(10, 10).r + 15);
    RawDevelop half;
    half.halfSize = true;
    CHECK(developDng(dng, half).width() == W / 2 && developDng(dng, half).height() == H / 2);
    RawDevelop grey;
    grey.saturation = -100;
    CHECK(neutral(developDng(dng, grey).at(50, 4)));
    RawDevelop lift;
    lift.shadows = 100;
    CHECK(developDng(dng, lift).at(10, 2).r > img.at(10, 2).r);
    RawDevelop bad;
    bad.whiteBalance = "purple";
    bool threw = false;
    try {
        developDng(dng, bad);
    } catch (const std::exception&) {
        threw = true;
    }
    CHECK(threw);

    // orientation 6 (rotate 90) and a default crop
    const Image turned = developDng(writeDng(W, H, cfa, false, {{274, {3, {6}}}}));
    CHECK(turned.width() == H && turned.height() == W);
    const Image cropped = developDng(writeDng(W, H, cfa, false, {{50719, {4, {4, 2}}}, {50720, {4, {40, 30}}}}));
    CHECK(cropped.width() == 40 && cropped.height() == 30);

    // a colour matrix (sRGB's own, so colours should barely move) is used without breaking neutrals
    // XYZ -> sRGB matrix as SRATIONAL x/10000
    const int m[9] = {32406, -15372, -4986, -9689, 18758, 415, 557, -2040, 10570};
    TagValue cm{10, {}};
    for (int k : m) cm.v.push_back(static_cast<std::uint32_t>(k)), cm.v.push_back(10000);
    const Image matrixed = developDng(writeDng(W, H, cfa, false, {{50721, cm}, {50778, {3, {21}}}, {50728, {5, {1, 1, 1, 1, 1, 1}}}}));
    CHECK(neutral(matrixed.at(10, 30)) && std::abs(matrixed.at(50, 4).r - red.r) < 12);

    // X-Trans style patterns are refused with a clear message
    threw = false;
    try {
        developDng(writeDng(W, H, cfa, false, {{33421, {3, {6, 6}}}}));
    } catch (const std::exception& e) {
        threw = std::string(e.what()).find("X-Trans") != std::string::npos;
    }
    CHECK(threw);
    return check::summary("raw");
}
