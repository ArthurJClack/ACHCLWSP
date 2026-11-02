#include "wetwaresuite/raster/codec.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cctype>
#include <cstring>
#include <fstream>
#include <stdexcept>

#include "wetwaresuite/raster/color_profile.hpp"
#include "wetwaresuite/raster/ops.hpp"
#include "wetwaresuite/raster/raw.hpp"

#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
#pragma GCC diagnostic ignored "-Wsign-compare"
#pragma GCC diagnostic ignored "-Wunused-but-set-variable"
#endif
#if defined(_MSC_VER)
#pragma warning(push, 0)
#endif
#define STB_IMAGE_IMPLEMENTATION
#define STB_IMAGE_WRITE_IMPLEMENTATION
#define STBI_WINDOWS_UTF8
#define STBIW_WINDOWS_UTF8
#include "stb_image.h"
#include "stb_image_write.h"
#if defined(_MSC_VER)
#pragma warning(pop)
#endif
#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic pop
#endif

namespace wetwaresuite::raster {

namespace {

std::string ext(const std::filesystem::path& path) {
    std::string e = path.extension().string();
    if (!e.empty() && e[0] == '.') e.erase(0, 1);
    std::transform(e.begin(), e.end(), e.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return e;
}

bool contains(const std::vector<std::string>& list, const std::string& value) {
    return std::find(list.begin(), list.end(), value) != list.end();
}

Image fromStb(unsigned char* data, int w, int h) {
    if (!data) throw std::runtime_error(std::string("cannot decode image: ") + stbi_failure_reason());
    Image img(w, h);
    std::memcpy(img.data(), data, img.bytes());
    stbi_image_free(data);
    return img;
}

void appendBytes(void* context, void* data, int size) {
    static_cast<std::string*>(context)->append(static_cast<const char*>(data), static_cast<std::size_t>(size));
}

std::string readFile(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) throw std::runtime_error("cannot open " + path.string());
    return std::string(std::istreambuf_iterator<char>(in), {});
}

void writeFile(const std::filesystem::path& path, const std::string& bytes) {
    std::ofstream out(path, std::ios::binary);
    if (!out) throw std::runtime_error("cannot write " + path.string());
    out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    if (!out) throw std::runtime_error("failed writing " + path.string());
}

// Binary PPM: stb can read it but not write it.
std::string encodePpm(const Image& img) {
    std::string out = "P6\n" + std::to_string(img.width()) + " " + std::to_string(img.height()) + "\n255\n";
    out.reserve(out.size() + static_cast<std::size_t>(img.width()) * img.height() * 3);
    for (int y = 0; y < img.height(); ++y) {
        for (int x = 0; x < img.width(); ++x) out.append(reinterpret_cast<const char*>(img.pixel(x, y)), 3);
    }
    return out;
}

}  // namespace

const std::vector<std::string>& readableExtensions() {
    static const std::vector<std::string> list = [] {
        std::vector<std::string> l = {"png", "jpg", "jpeg", "bmp", "tga", "gif", "psd", "hdr", "pic", "ppm", "pgm", "pnm", "tif", "tiff", "dng"};
        if (webpSupported()) l.push_back("webp");
        return l;
    }();
    return list;
}

const std::vector<std::string>& writableExtensions() {
    static const std::vector<std::string> list = [] {
        std::vector<std::string> l = {"png", "jpg", "jpeg", "bmp", "tga", "ppm", "tif", "tiff"};
        if (webpSupported()) l.push_back("webp");
        return l;
    }();
    return list;
}

bool canRead(const std::filesystem::path& path) { return contains(readableExtensions(), ext(path)); }
bool canWrite(const std::filesystem::path& path) { return contains(writableExtensions(), ext(path)); }

namespace {
Image decodeRaw(const std::string& bytes) {
    if (bytes.size() >= 4 && (bytes.compare(0, 4, "II*\0", 4) == 0 || bytes.compare(0, 4, "MM\0*", 4) == 0 ||
                              bytes.compare(0, 4, "II+\0", 4) == 0 || bytes.compare(0, 4, "MM\0+", 4) == 0)) {
        return decodeTiff(bytes);
    }
    if (bytes.size() >= 12 && bytes.compare(0, 4, "RIFF") == 0 && bytes.compare(8, 4, "WEBP") == 0) return decodeWebp(bytes);
    int w = 0, h = 0, n = 0;
    auto* data = stbi_load_from_memory(reinterpret_cast<const stbi_uc*>(bytes.data()), static_cast<int>(bytes.size()),
                                       &w, &h, &n, 4);
    return fromStb(data, w, h);
}

std::uint32_t crc32(const std::string& data, std::size_t from) {
    static const auto table = [] {
        std::array<std::uint32_t, 256> t{};
        for (std::uint32_t i = 0; i < 256; ++i) {
            std::uint32_t c = i;
            for (int k = 0; k < 8; ++k) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            t[i] = c;
        }
        return t;
    }();
    std::uint32_t c = 0xFFFFFFFFu;
    for (std::size_t i = from; i < data.size(); ++i) c = table[(c ^ static_cast<unsigned char>(data[i])) & 0xFF] ^ (c >> 8);
    return c ^ 0xFFFFFFFFu;
}

void putBe32(std::string& s, std::uint32_t v) {
    for (int i = 3; i >= 0; --i) s += static_cast<char>((v >> (8 * i)) & 0xFF);
}

void pngChunk(std::string& out, const char* type, const std::string& data) {
    putBe32(out, static_cast<std::uint32_t>(data.size()));
    const std::size_t start = out.size();
    out += type;
    out += data;
    putBe32(out, crc32(out, start));
}
}  // namespace

Image decode(const std::string& bytes) {
    if (isDng(bytes)) return developDng(bytes);  // developed straight to sRGB
    Image img = decodeRaw(bytes);
    // wide-gamut files (Display P3 phone photos, Adobe RGB, ProPhoto) are shown and edited as sRGB
    convertToSrgb(img, parseIccProfile(findIccProfile(bytes)));
    return img;
}

std::string encodePng16(const Image& img) {
    if (img.empty()) throw std::runtime_error("image is empty");
    const int W = img.width(), H = img.height();
    bool opaque = true;
    for (int y = 0; y < H && opaque; ++y)
        for (int x = 0; x < W; ++x)
            if (img.pixel(x, y)[3] != 255) {
                opaque = false;
                break;
            }
    const int ch = opaque ? 3 : 4, bpp = ch * 2;
    const std::size_t rowBytes = static_cast<std::size_t>(W) * static_cast<std::size_t>(bpp);
    std::string raw;
    raw.reserve((rowBytes + 1) * static_cast<std::size_t>(H));
    std::string row(rowBytes, '\0');
    for (int y = 0; y < H; ++y) {
        const std::uint8_t* p = img.pixel(0, y);
        for (int x = 0; x < W; ++x)
            for (int c = 0; c < ch; ++c) {
                const std::size_t at = static_cast<std::size_t>(x) * static_cast<std::size_t>(bpp) + static_cast<std::size_t>(c) * 2;
                row[at] = row[at + 1] = static_cast<char>(p[x * 4 + c]);  // v * 257, big-endian
            }
        raw += '\1';  // Sub filter: smaller files for smooth pictures
        for (std::size_t i = 0; i < rowBytes; ++i) {
            const unsigned char left = i >= static_cast<std::size_t>(bpp) ? static_cast<unsigned char>(row[i - static_cast<std::size_t>(bpp)]) : 0;
            raw += static_cast<char>(static_cast<unsigned char>(row[i]) - left);
        }
    }
    std::string out = "\x89PNG\r\n\x1a\n";
    std::string ihdr;
    putBe32(ihdr, static_cast<std::uint32_t>(W));
    putBe32(ihdr, static_cast<std::uint32_t>(H));
    ihdr += static_cast<char>(16);
    ihdr += static_cast<char>(opaque ? 2 : 6);
    ihdr += std::string(3, '\0');
    pngChunk(out, "IHDR", ihdr);
    pngChunk(out, "sRGB", std::string(1, '\0'));
    pngChunk(out, "IDAT", deflateZlib(raw, 6));
    pngChunk(out, "IEND", "");
    return out;
}

bool savesBits(const std::filesystem::path& path, int bits) {
    if (bits == 8) return true;
    const std::string e = ext(path);
    return bits == 16 && (e == "png" || e == "tif" || e == "tiff");
}

Image load(const std::filesystem::path& path) {
    if (!std::filesystem::exists(path)) throw std::runtime_error("file not found: " + path.string());
    return decode(readFile(path));
}

std::string encodePng(const Image& img) {
    std::string out;
    if (!stbi_write_png_to_func(appendBytes, &out, img.width(), img.height(), 4, img.data(), img.width() * 4)) {
        throw std::runtime_error("PNG encoding failed");
    }
    return out;
}

std::string encodeJpeg(const Image& img, int quality) {
    // JPEG has no alpha: flatten onto white first
    Image flat(img.width(), img.height(), {255, 255, 255, 255});
    blendOnto(flat, img, 0, 0, Blend::Normal, 1.0);
    std::string out;
    if (!stbi_write_jpg_to_func(appendBytes, &out, flat.width(), flat.height(), 4, flat.data(), std::clamp(quality, 1, 100))) {
        throw std::runtime_error("JPEG encoding failed");
    }
    return out;
}

void save(const Image& img, const std::filesystem::path& path, int quality, int bits) {
    if (img.empty()) throw std::runtime_error("image is empty");
    const std::string e = ext(path);
    const bool deep = bits == 16 && savesBits(path, 16);
    std::string bytes;
    if (e == "png") {
        bytes = deep ? encodePng16(img) : encodePng(img);
    } else if (e == "jpg" || e == "jpeg") {
        bytes = encodeJpeg(img, quality);
    } else if (e == "bmp") {
        if (!stbi_write_bmp_to_func(appendBytes, &bytes, img.width(), img.height(), 4, img.data())) {
            throw std::runtime_error("BMP encoding failed");
        }
    } else if (e == "tga") {
        if (!stbi_write_tga_to_func(appendBytes, &bytes, img.width(), img.height(), 4, img.data())) {
            throw std::runtime_error("TGA encoding failed");
        }
    } else if (e == "ppm") {
        bytes = encodePpm(img);
    } else if (e == "tif" || e == "tiff") {
        bytes = encodeTiff(img, deep ? 16 : 8);
    } else if (e == "webp") {
        bytes = encodeWebp(img, quality);
    } else {
        throw std::runtime_error("can't write ." + e + " images yet (supported: png, jpg, bmp, tga, ppm, tif, webp, ora)");
    }
    writeFile(path, bytes);
}

std::string inflateZlib(const std::string& data) {
    int len = 0;
    char* out = stbi_zlib_decode_malloc_guesssize_headerflag(data.data(), static_cast<int>(data.size()),
                                                            static_cast<int>(std::min<std::size_t>(data.size() * 4 + 64, 1u << 30)), &len, 1);
    if (!out) out = stbi_zlib_decode_noheader_malloc(data.data(), static_cast<int>(data.size()), &len);
    if (!out) throw std::runtime_error("corrupt compressed data");
    std::string s(out, static_cast<std::size_t>(len));
    STBI_FREE(out);
    return s;
}

std::string deflateZlib(const std::string& data, int level) {
    int len = 0;
    unsigned char* z = stbi_zlib_compress(reinterpret_cast<unsigned char*>(const_cast<char*>(data.data())), static_cast<int>(data.size()),
                                          &len, std::clamp(level, 1, 9));
    if (!z) throw std::runtime_error("compression failed");
    std::string s(reinterpret_cast<char*>(z), static_cast<std::size_t>(len));
    STBIW_FREE(z);
    return s;
}

std::string previewPng(const Image& img, int maxSize) {
    if (img.width() <= maxSize && img.height() <= maxSize) return encodePng(img);
    double scale = static_cast<double>(maxSize) / std::max(img.width(), img.height());
    int w = std::max(1, static_cast<int>(img.width() * scale));
    int h = std::max(1, static_cast<int>(img.height() * scale));
    return encodePng(resize(img, w, h, Filter::Bilinear));
}

}  // namespace wetwaresuite::raster
