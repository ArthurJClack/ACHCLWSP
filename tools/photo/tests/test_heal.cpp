// Retouching: healing brush, seamless clone and content-aware fill.
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>

#include "check.hpp"
#include "heal.hpp"
#include "wetwaresuite/photo/photo.hpp"
#include "wetwaresuite/raster/document.hpp"

using namespace wetwaresuite;
using raster::Color;
using raster::Image;

namespace {

Image& layerOf(Session& s, const std::string& id) {
    return s.workspace().getAs<raster::ImageDoc>(id)->image.layers[0].image;
}

int maxDiff(Color a, Color b) {
    return std::max({std::abs(a.r - b.r), std::abs(a.g - b.g), std::abs(a.b - b.b), std::abs(a.a - b.a)});
}

std::string newDoc(Session& s, int w, int h, const char* color) {
    Result r = s.run("photo.new", {{"width", std::int64_t{w}}, {"height", std::int64_t{h}}, {"background", std::string(color)}});
    CHECK(r.ok);
    return r.ok ? toText(*r.field("doc")) : "";
}

void punchHole(Image& img, int x0, int y0, int w, int h, Color c) {
    for (int y = y0; y < y0 + h; ++y)
        for (int x = x0; x < x0 + w; ++x) img.set(x, y, c);
}

void testMasks() {
    auto r = photo::rectMask(20, 10, 15, 5, 10, 10);
    CHECK(r.count() == 25);  // clipped to the image
    auto e = photo::ellipseMask(100, 100, 0, 0, 100, 100);
    CHECK(std::abs(static_cast<double>(e.count()) - 3.14159 * 2500) < 100);
    auto st = photo::strokeMask(100, 100, {10, 50, 90, 50}, 5);
    CHECK(st.at(50, 50) && st.at(10, 46) && !st.at(50, 56) && !st.at(2, 50));
    CHECK(std::abs(static_cast<double>(st.count()) - (80 * 10 + 3.14159 * 25)) < 40);
    CHECK(photo::strokeMask(50, 50, {25, 25}, 3).at(25, 25));  // a single dab
}

void testHealFlat() {
    Session s;
    photo::registerCommands(s);
    const Color flat{120, 80, 200, 255};
    std::string id = newDoc(s, 64, 64, "#7850c8");
    punchHole(layerOf(s, id), 20, 20, 10, 10, {0, 0, 0, 255});

    // texture from a clean spot
    Result healed = s.run("photo.heal", {{"doc", id}, {"x", std::int64_t{18}}, {"y", std::int64_t{18}}, {"width", std::int64_t{14}},
                                         {"height", std::int64_t{14}}, {"from_x", std::int64_t{48}}, {"from_y", std::int64_t{48}}});
    CHECK(healed.ok);
    int worst = 0;
    for (int y = 0; y < 64; ++y)
        for (int x = 0; x < 64; ++x) worst = std::max(worst, maxDiff(layerOf(s, id).at(x, y), flat));
    CHECK(worst <= 2);
    // undo brings the hole back
    CHECK(s.history().undo());
    CHECK(maxDiff(layerOf(s, id).at(25, 25), {0, 0, 0, 255}) == 0);

    // smooth fill with an ellipse and no source
    Result smooth = s.run("photo.heal", {{"doc", id}, {"shape", std::string("ellipse")}, {"x", std::int64_t{14}}, {"y", std::int64_t{14}},
                                         {"width", std::int64_t{22}}, {"height", std::int64_t{22}}});
    CHECK(smooth.ok);
    CHECK(maxDiff(layerOf(s, id).at(25, 25), flat) <= 2);
    CHECK(maxDiff(layerOf(s, id).at(21, 21), flat) <= 2);

    // a brush stroke over a scratch
    for (int x = 5; x < 60; ++x) layerOf(s, id).set(x, 50, {255, 255, 255, 255});
    Result stroke = s.run("photo.heal", {{"doc", id}, {"shape", std::string("stroke")}, {"points", NumberList{5, 50, 59, 50}},
                                         {"radius", 3.0}, {"from_x", std::int64_t{32}}, {"from_y", std::int64_t{40}}});
    CHECK(stroke.ok);
    CHECK(maxDiff(layerOf(s, id).at(30, 50), flat) <= 2);

    // plain-word errors
    CHECK(!s.run("photo.heal", {{"doc", id}, {"x", std::int64_t{0}}, {"y", std::int64_t{0}}, {"width", std::int64_t{64}},
                                {"height", std::int64_t{64}}})
               .ok);  // whole image
    CHECK(!s.run("photo.heal", {{"doc", id}, {"x", std::int64_t{100}}, {"y", std::int64_t{100}}, {"width", std::int64_t{5}},
                                {"height", std::int64_t{5}}})
               .ok);  // outside
    CHECK(!s.run("photo.heal", {{"doc", id}, {"x", std::int64_t{10}}, {"y", std::int64_t{10}}, {"width", std::int64_t{10}},
                                {"height", std::int64_t{10}}, {"from_x", std::int64_t{2}}, {"from_y", std::int64_t{30}}})
               .ok);  // source runs off the image
    CHECK(!s.run("photo.heal", {{"doc", id}, {"shape", std::string("stroke")}, {"points", NumberList{1, 2, 3}}}).ok);
}

void testSeamlessClone() {
    Session s;
    photo::registerCommands(s);
    std::string target = newDoc(s, 80, 60, "#000000");
    Image& t = layerOf(s, target);
    for (int y = 0; y < 60; ++y)
        for (int x = 0; x < 80; ++x) t.set(x, y, {static_cast<std::uint8_t>(x * 3), static_cast<std::uint8_t>(y * 4), 100, 255});
    const Image before = t;
    std::string source = newDoc(s, 40, 40, "#fa1414");
    punchHole(layerOf(s, source), 19, 19, 3, 3, {200, 0, 0, 255});  // a darker dot in the middle

    Result r = s.run("photo.clone_seamless", {{"doc", target}, {"source_doc", source}, {"shape", std::string("rect")},
                                              {"from_x", std::int64_t{10}}, {"from_y", std::int64_t{10}}, {"width", std::int64_t{21}},
                                              {"height", std::int64_t{21}}, {"to_x", std::int64_t{30}}, {"to_y", std::int64_t{20}}});
    CHECK(r.ok);
    // outside the area nothing changed; just inside it matches the boundary (no seam)
    int outsideChanged = 0, seam = 0;
    for (int y = 0; y < 60; ++y) {
        for (int x = 0; x < 80; ++x) {
            const bool inside = x >= 30 && x < 51 && y >= 20 && y < 41;
            if (!inside && maxDiff(t.at(x, y), before.at(x, y)) != 0) ++outsideChanged;
            const bool edge = inside && (x == 30 || x == 50 || y == 20 || y == 40);
            if (edge) seam = std::max(seam, maxDiff(t.at(x, y), before.at(x, y)));
        }
    }
    CHECK(outsideChanged == 0);
    CHECK(seam <= 8);  // one pixel step of the target gradient is 3..4
    // the detail came across: the dot is ~50 darker in red than its surroundings
    const int dot = t.at(40, 30).r, around = t.at(44, 30).r;
    CHECK(std::abs((around - dot) - (50 + 12)) <= 10);  // 50 from the dot, plus 4 px of the 3/px gradient
    // and the area took on the target's colours, not the source's red
    CHECK(std::abs(t.at(44, 30).g - before.at(44, 30).g) <= 6);

    // same-image clone works too, and errors are plain
    CHECK(s.run("photo.clone_seamless", {{"doc", target}, {"from_x", std::int64_t{0}}, {"from_y", std::int64_t{0}}, {"width", std::int64_t{10}},
                                         {"height", std::int64_t{10}}, {"to_x", std::int64_t{60}}, {"to_y", std::int64_t{40}}})
              .ok);
    CHECK(!s.run("photo.clone_seamless", {{"doc", target}, {"source_doc", source}, {"from_x", std::int64_t{35}}, {"from_y", std::int64_t{0}},
                                          {"width", std::int64_t{10}}, {"height", std::int64_t{10}}, {"to_x", std::int64_t{0}}, {"to_y", std::int64_t{0}}})
               .ok);
    CHECK(!s.run("photo.clone_seamless", {{"doc", target}, {"source_doc", std::string("img9")}, {"from_x", std::int64_t{0}},
                                          {"from_y", std::int64_t{0}}, {"width", std::int64_t{5}}, {"height", std::int64_t{5}},
                                          {"to_x", std::int64_t{0}}, {"to_y", std::int64_t{0}}})
               .ok);
}

Color stripe(int y, int x) {
    (void)x;
    return (y / 4) % 2 ? Color{230, 200, 40, 255} : Color{30, 60, 150, 255};
}

void testInpaintStripes() {
    Session s;
    photo::registerCommands(s);
    std::string id = newDoc(s, 96, 80, "#000000");
    Image& img = layerOf(s, id);
    for (int y = 0; y < 80; ++y)
        for (int x = 0; x < 96; ++x) img.set(x, y, stripe(y, x));
    punchHole(img, 36, 26, 24, 22, {255, 0, 0, 255});  // the "object"

    auto t0 = std::chrono::steady_clock::now();
    Result r = s.run("photo.remove_object", {{"doc", id}, {"x", std::int64_t{36}}, {"y", std::int64_t{26}}, {"width", std::int64_t{24}},
                                             {"height", std::int64_t{22}}});
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
    std::printf("remove_object 24x22: %lld ms\n", static_cast<long long>(ms));
    CHECK(r.ok);
    int good = 0, total = 0;
    for (int y = 26; y < 48; ++y) {
        for (int x = 36; x < 60; ++x) {
            ++total;
            if (maxDiff(img.at(x, y), stripe(y, x)) <= 10) ++good;
        }
    }
    std::printf("stripes continued: %d of %d pixels\n", good, total);
    CHECK(good * 100 >= total * 95);
    CHECK(s.history().undo());
    CHECK(maxDiff(img.at(40, 30), {255, 0, 0, 255}) == 0);

    // vertical stripes through a stroke-shaped gap
    for (int y = 0; y < 80; ++y)
        for (int x = 0; x < 96; ++x) img.set(x, y, stripe(x, y));
    for (int x = 10; x < 86; ++x)
        for (int y = 38; y < 43; ++y) img.set(x, y, {0, 0, 0, 255});
    Result st = s.run("photo.remove_object", {{"doc", id}, {"shape", std::string("stroke")}, {"points", NumberList{10, 40, 85, 40}},
                                              {"radius", 4.0}, {"patch", std::int64_t{7}}});
    CHECK(st.ok);
    good = total = 0;
    for (int x = 10; x < 86; ++x) {
        for (int y = 38; y < 43; ++y) {
            ++total;
            if (maxDiff(img.at(x, y), stripe(x, y)) <= 10) ++good;
        }
    }
    std::printf("vertical stripes continued: %d of %d pixels\n", good, total);
    CHECK(good * 100 >= total * 95);

    // a big hole (a whole person) is filled on a smaller copy and refined back up
    {
        std::string big = newDoc(s, 640, 480, "#000000");
        Image& bi = layerOf(s, big);
        for (int y = 0; y < 480; ++y)
            for (int x = 0; x < 640; ++x) bi.set(x, y, stripe(y, x));
        punchHole(bi, 200, 100, 260, 280, {255, 0, 0, 255});
        auto b0 = std::chrono::steady_clock::now();
        Result br = s.run("photo.remove_object", {{"doc", big}, {"x", std::int64_t{200}}, {"y", std::int64_t{100}}, {"width", std::int64_t{260}},
                                                  {"height", std::int64_t{280}}});
        auto bms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - b0).count();
        CHECK(br.ok);
        int bg = 0, bt = 0, red = 0;
        for (int y = 100; y < 380; ++y)
            for (int x = 200; x < 460; ++x) {
                ++bt;
                if (maxDiff(bi.at(x, y), stripe(y, x)) <= 30) ++bg;
                if (maxDiff(bi.at(x, y), {255, 0, 0, 255}) <= 30) ++red;
            }
        std::printf("remove_object 260x280: %lld ms, stripes %d of %d\n", static_cast<long long>(bms), bg, bt);
        CHECK(red == 0);
        CHECK(bg * 100 >= bt * 70);
    }
    // too big is refused in plain words
    std::string huge = newDoc(s, 2200, 2100, "#808080");
    Result tooBig = s.run("photo.remove_object", {{"doc", huge}, {"x", std::int64_t{10}}, {"y", std::int64_t{10}}, {"width", std::int64_t{2150}},
                                                  {"height", std::int64_t{2000}}});
    CHECK(!tooBig.ok && tooBig.message.find("limit") != std::string::npos);
    CHECK(!s.run("photo.remove_object", {{"doc", id}, {"x", std::int64_t{10}}, {"y", std::int64_t{10}}, {"width", std::int64_t{5}},
                                         {"height", std::int64_t{5}}, {"patch", std::int64_t{99}}})
               .ok);
}

void testInpaintSpeed() {
    // a medium area on a noisy photo-like picture stays quick
    Image img(320, 240);
    std::uint32_t seed = 1;
    for (int y = 0; y < 240; ++y) {
        for (int x = 0; x < 320; ++x) {
            seed = seed * 1664525u + 1013904223u;
            auto n = static_cast<std::uint8_t>(seed >> 27);
            img.set(x, y, {static_cast<std::uint8_t>(100 + n), static_cast<std::uint8_t>(120 + n), static_cast<std::uint8_t>(80 + x / 8), 255});
        }
    }
    auto mask = photo::ellipseMask(320, 240, 110, 70, 100, 100);
    std::string error;
    auto t0 = std::chrono::steady_clock::now();
    CHECK(photo::inpaint(img, mask, 9, error));
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
    std::printf("inpaint %zu px: %lld ms\n", mask.count(), static_cast<long long>(ms));

    // a linear ramp is harmonic, so a smooth fill must rebuild it exactly
    Image big(600, 600);
    for (int y = 0; y < 600; ++y)
        for (int x = 0; x < 600; ++x) big.set(x, y, {static_cast<std::uint8_t>(x * 2 / 5), static_cast<std::uint8_t>(y / 3), 130, 255});
    auto hole = photo::ellipseMask(600, 600, 100, 100, 400, 400);
    photo::SolveStats st;
    t0 = std::chrono::steady_clock::now();
    CHECK(photo::poissonBlend(big, nullptr, hole, 0, 0, st, error));
    ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
    std::printf("poisson %zu px: %lld ms, %d iterations, converged %d\n", hole.count(), static_cast<long long>(ms), st.iterations,
                st.converged ? 1 : 0);
    CHECK(maxDiff(big.at(300, 300), {120, 100, 130, 255}) <= 3);
    CHECK(maxDiff(big.at(200, 350), {80, 116, 130, 255}) <= 3);
    int worst = 0;
    for (int y = 0; y < 600; ++y)
        for (int x = 0; x < 600; ++x)
            if (hole.at(x, y))
                worst = std::max(worst, maxDiff(big.at(x, y), {static_cast<std::uint8_t>(x * 2 / 5), static_cast<std::uint8_t>(y / 3), 130, 255}));
    std::printf("poisson worst error: %d\n", worst);
    CHECK(worst <= 3);
}

}  // namespace

int main() {
    testMasks();
    testHealFlat();
    testSeamlessClone();
    testInpaintStripes();
    testInpaintSpeed();
    return check::summary("photo_heal");
}
