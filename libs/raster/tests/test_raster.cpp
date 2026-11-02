#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <functional>
#include <random>
#include <string>
#include <vector>

#include "wetwaresuite/raster/codec.hpp"
#include "wetwaresuite/raster/color_profile.hpp"
#include "wetwaresuite/raster/document.hpp"
#include "wetwaresuite/raster/ops.hpp"
#include "wetwaresuite/raster/ora.hpp"
#include "wetwaresuite/raster/paint.hpp"
#include "wetwaresuite/raster/parallel.hpp"
#include "wetwaresuite/raster/pyramid.hpp"
#include "wetwaresuite/raster/simd.hpp"
#include "check.hpp"

using namespace wetwaresuite::raster;
namespace fs = std::filesystem;

namespace {

double alphaSum(const Image& img) {
    double s = 0;
    for (int y = 0; y < img.height(); ++y) {
        for (int x = 0; x < img.width(); ++x) s += img.at(x, y).a / 255.0;
    }
    return s;
}

// Random pixels with plenty of the edge cases: alpha 0 and 255, black and white channels.
Image noisy(int w, int h, unsigned seed) {
    std::mt19937 rng(seed);
    Image img(w, h);
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            auto pick = [&] {
                const unsigned v = rng() % 10;
                return static_cast<std::uint8_t>(v == 0 ? 0 : v == 1 ? 255 : rng() % 256);
            };
            img.set(x, y, {pick(), pick(), pick(), pick()});
        }
    }
    return img;
}

bool samePixels(const Image& a, const Image& b) {
    return a.width() == b.width() && a.height() == b.height() && std::equal(a.data(), a.data() + a.bytes(), b.data());
}

// Runs fn on the scalar loops and on the SIMD loops; both must give the same bytes.
bool simdMatches(const std::function<Image()>& fn) {
    const bool was = setSimdEnabled(false);
    const Image scalar = fn();
    setSimdEnabled(true);
    const Image vec = fn();
    setSimdEnabled(was);
    return samePixels(scalar, vec);
}

void testSimd() {
    CHECK(std::string(simdName()) == (simdAvailable() ? std::string(simdName()) : std::string("scalar")));
    const Image a = noisy(67, 41, 1), b = noisy(67, 41, 2), c = noisy(23, 17, 3);
    for (const auto& name : blendNames()) {
        const Blend mode = *parseBlend(name);
        for (double op : {1.0, 0.63, 0.004}) {
            CHECK(simdMatches([&] {
                Image d = a;
                blendOnto(d, b, 0, 0, mode, op);
                blendOnto(d, c, 30, -5, mode, op);  // offset and clipped
                return d;
            }));
        }
    }
    for (int r : {1, 3, 8, 40}) {
        CHECK(simdMatches([&] {
            Image d = a;
            boxBlur(d, r);
            return d;
        }));
    }
    for (const auto& f : filterNames()) {
        const Filter filter = *parseFilter(f);
        for (auto [w, h] : {std::pair{33, 20}, std::pair{150, 97}, std::pair{11, 60}, std::pair{5, 3}}) {
            CHECK(simdMatches([&] { return resize(a, w, h, filter); }));
        }
    }
    for (auto kind : {Adjust::Saturation, Adjust::Vibrance}) {
        for (double amount : {-100.0, -35.0, 40.0, 100.0}) {
            CHECK(simdMatches([&] {
                Image d = a;
                adjust(d, kind, amount);
                return d;
            }));
        }
    }
    CHECK(simdMatches([&] {
        Image d = a;
        unsharpMask(d, 3, 1.5);
        return d;
    }));
    CHECK(simdMatches([&] {
        LayeredImage li;
        li.width = a.width();
        li.height = a.height();
        li.layers.push_back({"a", a});
        li.layers.push_back({"b", b, 0.5, Blend::Overlay});
        Pyramid pyr;
        pyr.build(li);
        Image all(64, 64);
        int y = 0;
        for (int i = 1; i < pyr.levels(); ++i) {  // every level, stacked into one picture
            const Image& l = pyr.level(i);
            for (int row = 0; row < l.height() && y < 64; ++row, ++y) std::copy_n(l.pixel(0, row), static_cast<std::size_t>(l.width()) * 4, all.pixel(0, y));
        }
        return all;
    }));
    // the SIMD switch is a real switch
    const bool was = setSimdEnabled(false);
    CHECK(!simdEnabled());
    setSimdEnabled(true);
    CHECK(simdEnabled() == simdAvailable());
    setSimdEnabled(was);
}

#include "tiff_samples.inc"

// --- colour profiles ---------------------------------------------------------------------
void be32(std::string& s, std::uint32_t v) {
    for (int i = 3; i >= 0; --i) s += static_cast<char>((v >> (8 * i)) & 0xFF);
}
std::string xyzTag(double x, double y, double z) {
    std::string t = "XYZ ";
    be32(t, 0);
    for (double v : {x, y, z}) be32(t, static_cast<std::uint32_t>(static_cast<std::int32_t>(std::lround(v * 65536))));
    return t;
}
// sRGB's own curve as an ICC parametric curve (type 3)
std::string srgbCurve() {
    std::string t = "para";
    be32(t, 0);
    t += std::string("\0\x03\0\0", 4);
    for (double v : {2.4, 1 / 1.055, 0.055 / 1.055, 1 / 12.92, 0.04045}) be32(t, static_cast<std::uint32_t>(std::lround(v * 65536)));
    return t;
}
// A tiny RGB matrix/TRC profile with these D50 primaries and the sRGB tone curve.
std::string makeProfile(const std::string& name, const double prim[9]) {
    std::vector<std::pair<std::string, std::string>> tags;
    std::string desc = "desc";
    be32(desc, 0);
    be32(desc, static_cast<std::uint32_t>(name.size() + 1));
    desc += name;
    desc += '\0';
    tags.push_back({"desc", desc});
    tags.push_back({"rXYZ", xyzTag(prim[0], prim[1], prim[2])});
    tags.push_back({"gXYZ", xyzTag(prim[3], prim[4], prim[5])});
    tags.push_back({"bXYZ", xyzTag(prim[6], prim[7], prim[8])});
    for (const char* t : {"rTRC", "gTRC", "bTRC"}) tags.push_back({t, srgbCurve()});
    std::string head(128, '\0');
    head.replace(16, 4, "RGB ");
    head.replace(20, 4, "XYZ ");
    head.replace(36, 4, "acsp");
    std::string table, data;
    be32(table, static_cast<std::uint32_t>(tags.size()));
    const std::size_t dataStart = 128 + 4 + tags.size() * 12;
    for (const auto& [sig, body] : tags) {
        table += sig;
        be32(table, static_cast<std::uint32_t>(dataStart + data.size()));
        be32(table, static_cast<std::uint32_t>(body.size()));
        data += body;
        while (data.size() % 4) data += '\0';
    }
    std::string icc = head + table + data;
    icc[0] = static_cast<char>(icc.size() >> 24), icc[1] = static_cast<char>(icc.size() >> 16);
    icc[2] = static_cast<char>(icc.size() >> 8), icc[3] = static_cast<char>(icc.size());
    return icc;
}
std::uint32_t crc(const std::string& d) {
    std::uint32_t c = 0xFFFFFFFFu;
    for (unsigned char ch : d) {
        c ^= ch;
        for (int k = 0; k < 8; ++k) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
    }
    return c ^ 0xFFFFFFFFu;
}
// The PNG with an iCCP chunk added right after IHDR.
std::string withIccp(const std::string& png, const std::string& icc) {
    std::string body = "iCCP";
    body += std::string("test\0\0", 6);
    body += deflateZlib(icc);
    std::string chunk;
    be32(chunk, static_cast<std::uint32_t>(body.size() - 4));
    chunk += body;
    be32(chunk, crc(body));
    return png.substr(0, 33) + chunk + png.substr(33);
}

void testColorProfiles(const fs::path& dir) {
    const double p3[9] = {0.5151, 0.2412, -0.0011, 0.2920, 0.6922, 0.0419, 0.1571, 0.0666, 0.7841};
    const double srgb[9] = {0.4361, 0.2225, 0.0139, 0.3851, 0.7169, 0.0971, 0.1431, 0.0606, 0.7141};
    const std::string p3icc = makeProfile("Display P3", p3);
    const ColorProfile prof = parseIccProfile(p3icc);
    CHECK(prof.present && prof.convertible && !prof.srgbLike && prof.description == "Display P3");
    CHECK(parseIccProfile(makeProfile("sRGB IEC61966-2.1", srgb)).srgbLike);
    CHECK(!parseIccProfile("junk").present);

    Image img(4, 4, {200, 100, 50, 255});
    img.set(0, 0, {255, 255, 255, 255});
    img.set(1, 0, {0, 0, 0, 255});
    const std::string png = withIccp(encodePng(img), p3icc);
    CHECK(findIccProfile(png) == p3icc);
    const Image got = decode(png);
    // P3 orange is more saturated than the same numbers in sRGB; white and black stay put
    CHECK(got.at(2, 2).r > 210 && got.at(2, 2).b < 50 && got.at(0, 0) == (Color{255, 255, 255, 255}) && got.at(1, 0) == (Color{0, 0, 0, 255}));
    CHECK(decode(encodePng(img)).at(2, 2) == (Color{200, 100, 50, 255}));  // untagged files are left alone
    const FileColorInfo info = describeColor(png);
    CHECK(info.profile == "Display P3" && info.converted && info.bits == 8);

    // JPEG: the profile rides in APP2 segments
    std::string jpg = encodeJpeg(img, 95);
    std::string app2 = std::string("\xFF\xE2", 2);
    const std::string seg = std::string("ICC_PROFILE\0", 12) + std::string("\x01\x01", 2) + p3icc;
    app2 += static_cast<char>((seg.size() + 2) >> 8);
    app2 += static_cast<char>((seg.size() + 2) & 0xFF);
    app2 += seg;
    jpg.insert(2, app2);
    CHECK(findIccProfile(jpg) == p3icc && decode(jpg).at(2, 2).r > decode(encodeJpeg(img, 95)).at(2, 2).r + 5);

    // 16-bit saves round-trip and say they're 16-bit
    Image grad(64, 8);
    for (int y = 0; y < 8; ++y)
        for (int x = 0; x < 64; ++x) grad.set(x, y, {static_cast<std::uint8_t>(x * 4), 7, 200, static_cast<std::uint8_t>(y == 0 ? 128 : 255)});
    const std::string p16 = encodePng16(grad);
    CHECK(fileBitDepth(p16) == 16 && decode(p16).at(33, 3) == grad.at(33, 3) && decode(p16).at(5, 0).a == 128);
    save(grad, dir / "deep.tif", 90, 16);
    CHECK(load(dir / "deep.tif").at(40, 4) == grad.at(40, 4));
    {
        std::ifstream in(dir / "deep.tif", std::ios::binary);
        CHECK(fileBitDepth(std::string((std::istreambuf_iterator<char>(in)), {})) == 16);
    }
    CHECK(savesBits("a.png", 16) && savesBits("a.TIF", 16) && !savesBits("a.jpg", 16));
    // a doc opened from a 16-bit file saves 16-bit unless told otherwise
    save(grad, dir / "deep.png", 90, 16);
    auto doc = openImage(dir / "deep.png");
    CHECK(doc->sourceBits == 16);
    saveImage(*doc, dir / "again.png");
    std::vector<std::string> notes;
    saveImage(*doc, dir / "again.jpg", 90, &notes);
    std::ifstream again(dir / "again.png", std::ios::binary);
    CHECK(fileBitDepth(std::string((std::istreambuf_iterator<char>(again)), {})) == 16 && notes.size() == 1);
}

void testTiffAndWebp(const fs::path& dir) {
    for (const auto& t : tiffSamples()) {
        Image img = decode(t.bytes);
        const bool ok = img.width() == 7 && img.height() == 5 && img.at(1, 1) == t.left && img.at(5, 2) == t.right;
        if (!ok) std::printf("  tiff sample %s decoded wrong\n", t.name);
        CHECK(ok);
    }
    // round trip, with and without alpha, through a file
    Image src(37, 23, {10, 20, 30, 255});
    for (int y = 0; y < 23; ++y)
        for (int x = 0; x < 37; ++x) {
            std::uint8_t* p = src.pixel(x, y);
            p[0] = static_cast<std::uint8_t>(x * 7);
            p[1] = static_cast<std::uint8_t>(y * 11);
            p[2] = static_cast<std::uint8_t>((x * y) & 255);
        }
    save(src, dir / "rt.tif");
    Image back = load(dir / "rt.tif");
    CHECK(back.width() == 37 && back.height() == 23 && std::equal(src.data(), src.data() + src.bytes(), back.data()));
    src.pixel(3, 4)[3] = 77;
    const std::string alpha = encodeTiff(src);
    back = decodeTiff(alpha);
    CHECK(std::equal(src.data(), src.data() + src.bytes(), back.data()));
    CHECK(canRead("x.TIFF") && canWrite("y.tif"));
    bool threw = false;
    try {
        decodeTiff("II*\0garbage");
    } catch (const std::runtime_error&) {
        threw = true;
    }
    CHECK(threw);

    if (webpSupported()) {
        CHECK(canRead("a.webp") && canWrite("b.WEBP"));
        save(src, dir / "rt.webp", 100);  // lossless
        back = load(dir / "rt.webp");
        CHECK(back.width() == 37 && std::equal(src.data(), src.data() + src.bytes(), back.data()));
        // lossy: a smooth picture comes back close
        Image smooth(64, 48);
        for (int y = 0; y < 48; ++y)
            for (int x = 0; x < 64; ++x) {
                std::uint8_t* p = smooth.pixel(x, y);
                p[0] = static_cast<std::uint8_t>(x * 4);
                p[1] = static_cast<std::uint8_t>(y * 5);
                p[2] = 128;
                p[3] = 255;
            }
        const std::string lossy = encodeWebp(smooth, 80);
        back = decode(lossy);
        int worst = 0;
        for (int y = 0; y < 48; ++y)
            for (int x = 0; x < 64; ++x)
                for (int c = 0; c < 3; ++c) worst = std::max(worst, std::abs(back.pixel(x, y)[c] - smooth.pixel(x, y)[c]));
        CHECK(back.width() == 64 && worst < 24 && lossy.size() < smooth.bytes() / 4);
    } else {
        CHECK(!canRead("a.webp"));
    }
}

}  // namespace

int main() {
    // colours
    CHECK(Color::parse("#ff0000") == (Color{255, 0, 0, 255}));
    CHECK(Color::parse("#0f08") == (Color{0, 255, 0, 136}));
    CHECK(Color::parse("rgb(1, 2, 3)") == (Color{1, 2, 3, 255}));
    CHECK(Color::parse("rgba(10,20,30,0.5)")->a == 128);
    CHECK(Color::parse("Transparent")->a == 0);
    CHECK(!Color::parse("#12").has_value());
    CHECK(!Color::parse("nope").has_value());
    CHECK(Color{1, 2, 3, 4}.hex() == "#01020304");

    // compositing
    Image base(2, 1, {0, 0, 255, 255});
    Image top(2, 1, {255, 0, 0, 255});
    blendOnto(base, top, 0, 0, Blend::Normal, 0.5);
    CHECK(std::abs(base.at(0, 0).r - 128) <= 1 && std::abs(base.at(0, 0).b - 127) <= 1);
    Image white(1, 1, {255, 255, 255, 255});
    blendOnto(white, Image(1, 1, {100, 100, 100, 255}), 0, 0, Blend::Multiply, 1.0);
    CHECK(white.at(0, 0).r == 100);
    Image clear(1, 1);
    blendOnto(clear, Image(1, 1, {10, 20, 30, 255}), 0, 0, Blend::Multiply, 1.0);
    CHECK(clear.at(0, 0) == (Color{10, 20, 30, 255}));  // nothing under it: source shows as is

    // geometry
    Image grad(40, 20);
    for (int y = 0; y < 20; ++y) {
        for (int x = 0; x < 40; ++x) grad.set(x, y, {static_cast<std::uint8_t>(x * 6), static_cast<std::uint8_t>(y * 12), 0, 255});
    }
    CHECK(resize(grad, 10, 5).width() == 10);
    CHECK(resize(grad, 400, 200).height() == 200);
    CHECK(resize(grad, 7, 3, Filter::Nearest).at(0, 0) == grad.at(0, 0));
    CHECK(crop(grad, 30, 10, 50, 50).width() == 10);
    CHECK(rotate90(grad, 1).width() == 20 && rotate90(grad, 1).at(19, 0) == grad.at(0, 0));
    CHECK(rotate90(grad, 2).at(0, 0) == grad.at(39, 19));
    CHECK(flipHorizontal(grad).at(0, 0) == grad.at(39, 0));
    CHECK(flipVertical(grad).at(0, 0) == grad.at(0, 19));
    CHECK(resizeCanvas(grad, 60, 20, 0, 0).at(45, 0).a == 0);

    // adjustments
    Image gray(1, 1, {100, 100, 100, 255});
    adjust(gray, Adjust::Brightness, 20);
    CHECK(gray.at(0, 0).r == 151);
    Image colourful(1, 1, {200, 50, 50, 255});
    adjust(colourful, Adjust::Saturation, -100);
    CHECK(colourful.at(0, 0).r == colourful.at(0, 0).g);
    Image hue(1, 1, {255, 0, 0, 255});
    adjust(hue, Adjust::Hue, 120);
    CHECK(hue.at(0, 0).g == 255 && hue.at(0, 0).r == 0);
    Image inv(1, 1, {0, 10, 255, 77});
    applyEffect(inv, Effect::Invert, 0);
    CHECK(inv.at(0, 0) == (Color{255, 245, 0, 77}));
    Image blurred = grad;
    applyEffect(blurred, Effect::Blur, 6);
    CHECK(blurred.at(20, 10).r > 0 && blurred.at(0, 0).r != grad.at(39, 0).r);
    CHECK(parseAdjust("contrast").has_value() && !parseAdjust("x").has_value());

    // painting
    Image canvas(100, 100);
    Brush brush;
    brush.color = {255, 0, 0, 255};
    brush.size = 10;
    brush.hardness = 1.0;
    stroke(canvas, brush, {{10, 50, 1}, {90, 50, 1}});
    CHECK(canvas.at(50, 50).a == 255 && canvas.at(50, 50).r == 255);
    CHECK(canvas.at(50, 10).a == 0);
    double painted = alphaSum(canvas);
    CHECK(painted > 800 && painted < 1000);  // ~80 x 10 + round caps
    brush.erase = true;
    stroke(canvas, brush, {{50, 50, 1}});
    CHECK(canvas.at(50, 50).a == 0);

    Image half(10, 10);
    brush.erase = false;
    brush.opacity = 0.5;
    stroke(half, brush, {{5, 5, 1}, {5, 5, 1}, {5, 5, 1}});
    CHECK(std::abs(half.at(5, 5).a - 128) <= 1);  // repeated dabs never exceed stroke opacity

    Image box(20, 20, {255, 255, 255, 255});
    fillRect(box, 5, 5, 10, 10, {0, 0, 0, 255});
    CHECK(box.at(10, 10) == (Color{0, 0, 0, 255}));
    CHECK(box.at(2, 2) == (Color{255, 255, 255, 255}));
    Image poly(20, 20);
    fillRect(poly, 2.5, 2.5, 10, 10, {0, 0, 0, 255});
    CHECK(std::abs(alphaSum(poly) - 100) < 1.0);  // antialiased area is exact
    Image circle(50, 50);
    fillEllipse(circle, 25, 25, 20, 20, {0, 0, 255, 255});
    CHECK(std::abs(alphaSum(circle) - 3.14159 * 400) < 15);

    Image fill(10, 10, {255, 255, 255, 255});
    drawLine(fill, 5, 0, 5, 10, {0, 0, 0, 255}, 1);
    CHECK(floodFill(fill, 0, 0, {255, 0, 0, 255}, 10) > 30);
    CHECK(fill.at(0, 0) == (Color{255, 0, 0, 255}));
    CHECK(fill.at(9, 9) == (Color{255, 255, 255, 255}));  // other side of the line untouched

    // files
    auto dir = fs::temp_directory_path() / "wetwaresuite_test_raster";
    fs::create_directories(dir);
    testTiffAndWebp(dir);
    testColorProfiles(dir);
    save(grad, dir / "grad.png");
    Image back = load(dir / "grad.png");
    CHECK(back.width() == 40 && back.at(7, 3) == grad.at(7, 3));
    save(grad, dir / "grad.jpg", 95);
    CHECK(load(dir / "grad.jpg").width() == 40);
    save(grad, dir / "grad.bmp");
    save(grad, dir / "grad.tga");
    CHECK(load(dir / "grad.tga").at(1, 1) == grad.at(1, 1));
    save(grad, dir / "grad.ppm");
    CHECK(load(dir / "grad.ppm").at(5, 5).r == grad.at(5, 5).r);
    bool threw = false;
    try {
        load(dir / "missing.png");
    } catch (const std::exception&) {
        threw = true;
    }
    CHECK(threw);

    auto zip = writeZip({{"a.txt", "hello"}, {"dir/b.bin", std::string("\0\1\2", 3)}});
    auto entries = readZip(zip);
    CHECK(entries.size() == 2 && entries[0].data == "hello" && entries[1].name == "dir/b.bin");

    LayeredImage layered{40, 20, {}};
    layered.layers.push_back({"Background", grad});
    layered.layers.push_back({"Ink & \"notes\"", canvas.empty() ? grad : resize(canvas, 40, 20), 0.5, Blend::Multiply, false});
    saveOra(layered, dir / "test.ora");
    LayeredImage ora = loadOra(dir / "test.ora");
    CHECK(ora.width == 40 && ora.height == 20);
    CHECK(ora.layers.size() == 2);
    CHECK(ora.layers[0].name == "Background");
    CHECK(ora.layers[1].name == "Ink & \"notes\"");
    CHECK(ora.layers[1].blend == Blend::Multiply && !ora.layers[1].visible);
    CHECK(std::abs(ora.layers[1].opacity - 0.5) < 1e-6);
    CHECK(ora.layers[0].image.at(3, 3) == grad.at(3, 3));

    auto doc = openImage(dir / "test.ora");
    CHECK(doc->image.layers.size() == 2 && doc->active == 1);
    CHECK(doc->preview()->bytes.size() > 100);
    saveImage(*doc, dir / "flat.png");
    CHECK(load(dir / "flat.png").width() == 40);

    fs::remove_all(dir);

    // lanczos and area resize
    {
        Image flatGrey(64, 48, {120, 80, 40, 255});
        Image up = resize(flatGrey, 150, 101, Filter::Lanczos), down = resize(flatGrey, 13, 9, Filter::Lanczos);
        CHECK(up.width() == 150 && up.height() == 101 && down.width() == 13);
        CHECK(up.at(75, 50) == (Color{120, 80, 40, 255}) && down.at(0, 0) == (Color{120, 80, 40, 255}));
        Image checker(64, 64);
        for (int y = 0; y < 64; ++y) {
            for (int x = 0; x < 64; ++x) checker.set(x, y, ((x + y) & 1) ? Color{255, 255, 255, 255} : Color{0, 0, 0, 255});
        }
        CHECK(std::abs(resize(checker, 16, 16, Filter::Lanczos).at(8, 8).r - 128) <= 8);  // no aliasing when shrinking
        CHECK(std::abs(resize(checker, 32, 32, Filter::Area).at(5, 5).r - 128) <= 1);
        CHECK(parseFilter("lanczos") == Filter::Lanczos && !parseFilter("bicubic"));
    }

    // same output on any thread count
    {
        Image src(301, 207);
        for (int y = 0; y < src.height(); ++y) {
            for (int x = 0; x < src.width(); ++x) {
                src.set(x, y, {static_cast<std::uint8_t>(x * 7), static_cast<std::uint8_t>(y * 3), static_cast<std::uint8_t>(x ^ y),
                               static_cast<std::uint8_t>((x * y) & 255)});
            }
        }
        auto run = [&](int threads) {
            const int before = setThreadCount(threads);
            Image blurred = src;
            boxBlur(blurred, 5);
            Image sat = src;
            adjust(sat, Adjust::Hue, 40);
            std::vector<Layer> layers = {{"a", src}, {"b", blurred, 0.6, Blend::Overlay}};
            std::vector<Image> out = {blurred, sat, resize(src, 130, 90), resize(src, 700, 500, Filter::Lanczos),
                                      resize(src, 40, 30), composite(layers, src.width(), src.height())};
            setThreadCount(before);
            return out;
        };
        auto one = run(1), many = run(7);
        for (std::size_t i = 0; i < one.size(); ++i) {
            CHECK(one[i].bytes() == many[i].bytes() && std::equal(one[i].data(), one[i].data() + one[i].bytes(), many[i].data()));
        }
        // blur keeps flat colour flat and clear areas clear
        Image solid(50, 40, {10, 200, 30, 255});
        boxBlur(solid, 7);
        CHECK(solid.at(0, 0) == (Color{10, 200, 30, 255}) && solid.at(25, 20) == (Color{10, 200, 30, 255}));
        Image clearImg(20, 20);
        boxBlur(clearImg, 3);
        CHECK(clearImg.at(10, 10).a == 0);
    }

    // mipmap pyramid: a local update matches a full rebuild
    {
        LayeredImage li;
        li.width = 300;
        li.height = 170;
        li.layers.push_back({"bg", Image(300, 170, {255, 255, 255, 255})});
        li.layers.push_back({"ink", Image(300, 170), 0.8, Blend::Multiply});
        Pyramid pyr;
        pyr.build(li);
        CHECK(pyr.levels() == 10 && pyr.level(1).width() == 150 && pyr.level(2).height() == 43);
        CHECK(pyr.level(pyr.levels() - 1).width() == 1);
        fillRect(li.layers[1].image, 41, 33, 20, 17, {200, 10, 10, 255});
        CHECK(pyr.update(li, {41, 33, 61, 50}));
        CHECK(pyr.changed(1).x0 == 20 && pyr.changed(1).x1 == 31);
        Pyramid fresh;
        fresh.build(li);
        for (int i = 0; i < pyr.levels(); ++i) {
            const Image &a = pyr.level(i), &b = fresh.level(i);
            CHECK(std::equal(a.data(), a.data() + a.bytes(), b.data()));
        }
        CHECK(pyr.levelFor(1.0, 4096) == 0 && pyr.levelFor(0.5, 4096) == 1 && pyr.levelFor(0.3, 4096) == 1);
        CHECK(pyr.levelFor(0.24, 4096) == 2 && pyr.levelFor(4.0, 100) == 2);
        li.width = 10;
        CHECK(!pyr.update(li, {0, 0, 5, 5}));  // size changed: caller rebuilds
    }

    // dirty-region undo keeps only changed tiles; structural edits keep the whole image
    {
        auto d = newImage(1000, 700, {255, 255, 255, 255});
        d->image.layers.push_back({"ink", Image(1000, 700)});
        const LayeredImage before = d->image;
        (void)d->takeChanges();
        ImageEdit local(d, [](ImageDoc& doc) {
            fillRect(doc.image.layers[1].image, 70, 70, 10, 100, {0, 0, 0, 255});  // crosses two tile rows
            doc.image.layers[0].name = "Paper";
            return wetwaresuite::Result::success("");
        });
        CHECK(local.apply().ok);
        CHECK(!local.keptWholeImage() && local.undoBytes() > 0 && local.undoBytes() < 6 * 64 * 64 * 4);
        auto ch = d->takeChanges();
        CHECK(!ch.all && ch.rect.x0 == 64 && ch.rect.y0 == 64 && ch.rect.x1 == 128 && ch.rect.y1 == 192);
        local.revert();
        CHECK(local.undoBytes() == 0 && d->image.layers[0].name == "Background");
        CHECK(std::equal(before.layers[1].image.data(), before.layers[1].image.data() + before.layers[1].image.bytes(),
                         d->image.layers[1].image.data()));
        ImageEdit failing(d, [](ImageDoc& doc) {
            doc.image.layers[1].image.fill({1, 2, 3, 4});
            return wetwaresuite::Result::failure("nope");
        });
        CHECK(!failing.apply().ok && d->image.layers[1].image.at(5, 5).a == 0 && failing.undoBytes() == 0);
        ImageEdit grow(d, [](ImageDoc& doc) {
            for (auto& l : doc.image.layers) l.image = resizeCanvas(l.image, 1200, 700);
            doc.image.width = 1200;
            return wetwaresuite::Result::success("");
        });
        CHECK(grow.apply().ok && grow.keptWholeImage() && d->takeChanges().all);
        grow.revert();
        CHECK(d->image.width == 1000 && d->image.layers[1].image.width() == 1000);
        ImageEdit declared(
            d,
            [](ImageDoc& doc) {
                doc.touch(1, 10, 10, 8, 8);
                fillRect(doc.image.layers[1].image, 10, 10, 8, 8, {9, 9, 9, 255});
                doc.touch(1, 12, 12, 8, 8);  // overlapping areas still undo to the original
                fillRect(doc.image.layers[1].image, 12, 12, 8, 8, {7, 7, 7, 255});
                return wetwaresuite::Result::success("");
            },
            ImageEdit::Mode::Declared);
        CHECK(declared.apply().ok && declared.undoBytes() < 2000 && d->image.layers[1].image.at(15, 15).r == 7);
        declared.revert();
        CHECK(d->image.layers[1].image.at(15, 15).a == 0 && d->image.layers[1].image.at(11, 11).a == 0);
    }
    testSimd();
    return check::summary("raster");
}
