// The layer engine: blend modes, groups, adjustment layers, layer styles, alpha lock, and
// layered ORA / PSD files keeping all of it.
#include <algorithm>
#include <cmath>
#include <filesystem>
#include <string>
#include <vector>

#include "wetwaresuite/raster/adjustments.hpp"
#include "wetwaresuite/raster/document.hpp"
#include "wetwaresuite/raster/ops.hpp"
#include "wetwaresuite/raster/ora.hpp"
#include "wetwaresuite/raster/paint.hpp"
#include "wetwaresuite/raster/psd.hpp"
#include "check.hpp"

using namespace wetwaresuite::raster;
namespace fs = std::filesystem;

namespace {

Color over(Blend mode, Color backdrop, Color source, double opacity = 1.0) {
    Image b(1, 1, backdrop), s(1, 1, source);
    blendOnto(b, s, 0, 0, mode, opacity);
    return b.at(0, 0);
}

bool near(Color c, int r, int g, int b, int tol = 2) {
    return std::abs(c.r - r) <= tol && std::abs(c.g - g) <= tol && std::abs(c.b - b) <= tol;
}

bool sameImage(const Image& a, const Image& b, int tol = 0) {
    if (a.width() != b.width() || a.height() != b.height()) return false;
    for (std::size_t i = 0; i < a.bytes(); ++i) {
        if (std::abs(a.data()[i] - b.data()[i]) > tol) return false;
    }
    return true;
}

Layer square(const std::string& name, int w, int h, int x, int y, int size, Color c) {
    Layer l(name, Image(w, h));
    fillRect(l.image, x, y, size, size, c);
    return l;
}

void testBlendModes() {
    CHECK(blendNames().size() == 27);
    for (const auto& n : blendNames()) CHECK(parseBlend(n) && toString(*parseBlend(n)) == n);
    CHECK(parseBlend("Soft Light") == Blend::SoftLight && parseBlend("linear-dodge") == Blend::Add && parseBlend("colour_burn") == Blend::ColorBurn);
    CHECK(!parseBlend("sparkle"));

    const Color gray{128, 128, 128, 255}, dark{64, 64, 64, 255}, light{192, 192, 192, 255};
    CHECK(near(over(Blend::Multiply, gray, dark), 32, 32, 32));
    CHECK(near(over(Blend::Screen, gray, dark), 160, 160, 160));
    CHECK(near(over(Blend::LinearBurn, gray, light), 65, 65, 65));         // 128 + 192 - 255
    CHECK(near(over(Blend::Subtract, gray, dark), 64, 64, 64));
    CHECK(near(over(Blend::Divide, dark, gray), 127, 127, 127));            // 64 / (128/255)
    CHECK(near(over(Blend::ColorDodge, dark, gray), 128, 128, 128));        // 64 / (1 - 128/255)
    CHECK(near(over(Blend::ColorBurn, light, gray), 129, 129, 129));        // 1 - (1-.753)/.502
    CHECK(near(over(Blend::Exclusion, gray, gray), 128, 128, 128));
    CHECK(near(over(Blend::HardMix, gray, gray), 255, 255, 255) && near(over(Blend::HardMix, dark, gray), 0, 0, 0));
    CHECK(near(over(Blend::PinLight, gray, dark), 128, 128, 128) && near(over(Blend::PinLight, light, dark), 128, 128, 128));
    CHECK(near(over(Blend::LinearLight, gray, light), 255, 255, 255, 2));
    CHECK(near(over(Blend::SoftLight, gray, gray), 128, 128, 128));
    CHECK(near(over(Blend::HardLight, dark, light), over(Blend::Overlay, light, dark).r, over(Blend::Overlay, light, dark).g,
                over(Blend::Overlay, light, dark).b));
    CHECK(over(Blend::VividLight, gray, light).r > 128 && over(Blend::VividLight, gray, dark).r < 128);
    // the colour modes
    const Color red{255, 0, 0, 255}, blue{0, 0, 255, 255};
    CHECK(near(over(Blend::Hue, gray, red), 128, 128, 128));  // gray has no saturation to give the hue
    const Color tinted = over(Blend::Color, gray, red);
    CHECK(tinted.r > tinted.g + 60 && tinted.g == tinted.b);
    const Color lum = over(Blend::Luminosity, red, gray);
    CHECK(lum.r > lum.g && std::abs(0.3 * lum.r + 0.59 * lum.g + 0.11 * lum.b - 128) < 3);
    CHECK(near(over(Blend::Saturation, red, gray), 77, 77, 77, 3));  // red with no saturation keeps its luminosity
    CHECK(over(Blend::DarkerColor, red, blue) == blue && over(Blend::LighterColor, red, blue) == red);
    // dissolve: about half the pixels at 50%, each either untouched or fully the source
    Image bg(100, 100, {255, 255, 255, 255}), src(100, 100, {0, 0, 0, 255});
    blendOnto(bg, src, 0, 0, Blend::Dissolve, 0.5);
    int black = 0, other = 0;
    for (int y = 0; y < 100; ++y) {
        for (int x = 0; x < 100; ++x) {
            const Color c = bg.at(x, y);
            if (c.r == 0) ++black;
            else if (c.r != 255) ++other;
        }
    }
    CHECK(black > 4000 && black < 6000 && other == 0);
    // every mode keeps a clear source invisible and opacity 0 does nothing
    for (const auto& n : blendNames()) {
        CHECK(over(*parseBlend(n), gray, {10, 200, 30, 0}) == gray);
        CHECK(over(*parseBlend(n), gray, dark, 0.0) == gray);
    }
}

void testGroups() {
    const int W = 40, H = 20;
    std::vector<Layer> layers;
    layers.push_back({"bg", Image(W, H, {255, 255, 255, 255})});
    layers.push_back({"red", Image(W, H, {255, 0, 0, 255})});
    layers.back().depth = 1;
    Layer g("group");
    g.kind = LayerKind::Group;
    g.passThrough = false;
    g.opacity = 0.5;
    layers.push_back(g);
    CHECK(groupStart(layers, 2) == 1 && groupStart(layers, 1) == 1);
    CHECK(near(composite(layers, W, H).at(5, 5), 255, 128, 128));
    layers[2].visible = false;
    CHECK(near(composite(layers, W, H).at(5, 5), 255, 255, 255));
    layers[2].visible = true;
    // a group mask hides its members on the left
    layers[2].opacity = 1;
    layers[2].mask = Mask(W, H, 255);
    for (int y = 0; y < H; ++y) std::fill_n(layers[2].mask.row(y), W / 2, 0);
    const Image masked = composite(layers, W, H);
    CHECK(near(masked.at(5, 5), 255, 255, 255) && near(masked.at(30, 5), 255, 0, 0));
    layers[2].mask = Mask();
    // pass-through: a multiply member multiplies with what's below the group too
    layers[0].image.fill({0, 0, 255, 255});
    layers[1].blend = Blend::Multiply;
    layers[2].passThrough = true;
    CHECK(near(composite(layers, W, H).at(5, 5), 0, 0, 0));
    layers[2].passThrough = false;  // isolated: multiply onto nothing is plain red
    CHECK(near(composite(layers, W, H).at(5, 5), 255, 0, 0));
    // nested groups and depth repair
    std::vector<Layer> nested = {{"a"}, {"b"}, {"inner"}, {"outer"}};
    nested[0].depth = 2, nested[1].depth = 2, nested[2].depth = 1, nested[3].depth = 0;
    nested[2].kind = nested[3].kind = LayerKind::Group;
    CHECK(groupStart(nested, 3) == 0 && groupStart(nested, 2) == 0);
    nested[3].kind = LayerKind::Pixel;  // outer is no longer a group: nothing can sit inside it
    normalizeDepths(nested);
    CHECK(nested[3].depth == 0 && nested[2].depth == 0 && nested[1].depth == 1 && nested[0].depth == 1);
    // region drawing matches the full picture
    layers[2].opacity = 0.7;
    layers.push_back(square("top", W, H, 5, 5, 10, {0, 255, 0, 255}));
    layers.back().clip = true;  // clips to the group below it
    const Image full = composite(layers, W, H);
    CHECK(sameImage(compositeRegion(layers, 3, 4, 20, 10), crop(full, 3, 4, 20, 10)));
}

void testAdjustments() {
    const int W = 20, H = 10;
    CHECK(adjustmentLayerTypes().size() == 26);
    std::vector<Layer> layers = {{"red", Image(W, H, {255, 0, 0, 255})}};
    Layer adj("Invert");
    adj.kind = LayerKind::Adjustment;
    adj.settings = {{"type", "invert"}};
    layers.push_back(adj);
    CHECK(near(composite(layers, W, H).at(3, 3), 0, 255, 255));
    layers[1].opacity = 0.5;
    CHECK(near(composite(layers, W, H).at(3, 3), 128, 128, 128));
    layers[1].opacity = 1;
    layers[1].mask = Mask(W, H, 0);
    for (int y = 0; y < H; ++y) std::fill_n(layers[1].mask.row(y), W / 2, 255);
    Image half = composite(layers, W, H);
    CHECK(near(half.at(2, 2), 0, 255, 255) && near(half.at(15, 2), 255, 0, 0));
    // the pixels below never change
    CHECK(layers[0].image.at(2, 2) == (Color{255, 0, 0, 255}));
    // clipped to a layer: only where that layer has pixels
    layers[1].mask = Mask();
    layers.insert(layers.begin() + 1, square("box", W, H, 0, 0, 5, {0, 0, 255, 255}));
    layers[2].clip = true;
    Image clipped = composite(layers, W, H);
    CHECK(near(clipped.at(2, 2), 255, 255, 0) && near(clipped.at(15, 2), 255, 0, 0));

    auto run = [](const std::string& type, Settings s, Color c) {
        Image img(1, 1, c);
        s["type"] = type;
        applyAdjustment(img, type, s);
        return img.at(0, 0);
    };
    CHECK(near(run("black_white", {}, {255, 0, 0, 255}), 102, 102, 102));
    CHECK(near(run("black_white", {{"reds", "100"}}, {255, 0, 0, 255}), 255, 255, 255));
    CHECK(near(run("threshold", {{"level", "100"}}, {120, 120, 120, 255}), 255, 255, 255));
    CHECK(near(run("posterize", {{"levels", "2"}}, {100, 200, 30, 255}), 0, 255, 0));
    CHECK(near(run("levels", {{"black", "50"}, {"white", "150"}}, {100, 100, 100, 255}), 128, 128, 128));
    CHECK(near(run("levels", {{"out_black", "100"}}, {0, 0, 0, 255}), 100, 100, 100));
    CHECK(near(run("curves", {{"preset", "invert"}}, {0, 100, 255, 255}), 255, 155, 0));
    CHECK(near(run("curves", {{"points", "0,0,255,128"}, {"channel", "red"}}, {255, 255, 255, 255}), 128, 255, 255));
    CHECK(near(run("gradient_map", {{"colors", "#ff0000,#0000ff"}}, {0, 0, 0, 255}), 255, 0, 0));
    CHECK(near(run("gradient_map", {{"colors", "#ff0000,#0000ff"}, {"reverse", "true"}}, {255, 255, 255, 255}), 255, 0, 0));
    CHECK(near(run("channel_mixer", {{"red", "0,0,100"}, {"blue", "100,0,0"}}, {200, 50, 10, 255}), 10, 50, 200));
    CHECK(near(run("channel_mixer", {{"red", "0,100,0"}, {"monochrome", "true"}}, {200, 50, 10, 255}), 50, 50, 50));
    CHECK(near(run("exposure", {{"exposure", "1"}}, {0, 0, 0, 255}), 0, 0, 0) && run("exposure", {{"exposure", "1"}}, {100, 100, 100, 255}).r > 130);
    const Color shifted = run("hue_saturation", {{"hue", "120"}}, {255, 0, 0, 255});
    CHECK(near(shifted, 0, 255, 0));
    CHECK(near(run("hue_saturation", {{"saturation", "-100"}}, {255, 0, 0, 255}), 128, 128, 128));
    CHECK(near(run("hue_saturation", {{"hue", "120"}, {"range", "blues"}}, {255, 0, 0, 255}), 255, 0, 0));  // reds untouched
    const Color colorized = run("hue_saturation", {{"colorize", "true"}, {"hue", "240"}, {"saturation", "100"}}, {128, 128, 128, 255});
    CHECK(colorized.b > colorized.r + 100);
    CHECK(run("photo_filter", {{"color", "#ff8800"}, {"density", "50"}}, {128, 128, 128, 255}).r > run("photo_filter", {{"color", "#ff8800"}, {"density", "50"}}, {128, 128, 128, 255}).b);
    CHECK(run("selective_color", {{"reds.cyan", "100"}}, {200, 40, 40, 255}).r < 200);  // relative: scales with the cyan already there
    CHECK(run("selective_color", {{"reds.cyan", "100"}, {"method", "absolute"}}, {255, 0, 0, 255}).r < 255);
    CHECK(run("selective_color", {{"reds.cyan", "100"}}, {0, 0, 255, 255}) == (Color{0, 0, 255, 255}));  // blues aren't reds
    CHECK(run("selective_color", {{"blacks.black", "100"}, {"method", "absolute"}}, {20, 20, 20, 255}).r < 20);
    CHECK(near(run("color_balance", {{"midtones", "100,0,0"}}, {128, 128, 128, 255}), run("color_balance", {{"midtones", "100,0,0"}}, {128, 128, 128, 255}).r, 128, 128, 60));
    CHECK(run("vibrance", {{"saturation", "-100"}}, {200, 100, 50, 255}).r == run("vibrance", {{"saturation", "-100"}}, {200, 100, 50, 255}).b);
    CHECK(run("brightness_contrast", {{"brightness", "20"}}, {100, 100, 100, 255}).r == 151);
    CHECK(checkAdjustment("posterize", {{"levels", "1"}}).find("2..255") != std::string::npos);
    CHECK(!checkAdjustment("wobble", {}).empty() && !checkAdjustment("curves", {}).empty());
    CHECK(!checkAdjustment("gradient_map", {{"colors", "#000"}}).empty() && checkAdjustment("gradient_map", {{"colors", "#000,rgb(1,2,3)"}}).empty());
    CHECK(checkAdjustment("hue_saturation", {{"hue", "30"}, {"range", "greens"}}).empty());
    CHECK(!allAdjustmentFields().empty());

    // smart filters: a blur layer softens the edge live and the pixels underneath stay sharp
    std::vector<Layer> sharp = {{"bw", Image(W, H, {0, 0, 0, 255})}};
    for (int y = 0; y < H; ++y)
        for (int x = W / 2; x < W; ++x) sharp[0].image.set(x, y, {255, 255, 255, 255});
    Layer blur("Blur");
    blur.kind = LayerKind::Adjustment;
    blur.settings = {{"type", "gaussian_blur"}, {"radius", "3"}};
    blur.clip = true;
    sharp.push_back(blur);
    const Color edge = composite(sharp, W, H).at(W / 2, 5);
    CHECK(edge.r > 30 && edge.r < 225);
    CHECK(sharp[0].image.at(W / 2, 5).r == 255);
    CHECK(run("find_edges", {}, {90, 90, 90, 255}).a == 255);
    CHECK(checkAdjustment("gaussian_blur", {{"radius", "300"}}).find("0..250") != std::string::npos);
    CHECK(checkAdjustment("sharpen", {{"amount", "120"}}).empty());
}

void testStyles() {
    const int W = 60, H = 60;
    std::vector<Layer> layers = {{"bg", Image(W, H, {255, 255, 255, 255})}, square("box", W, H, 25, 25, 10, {0, 0, 0, 255})};
    LayerStyle& st = layers[1].style;
    std::string error;
    CHECK(setStyleEffect(st, "drop_shadow", true));
    CHECK(setStyleValue(st, "drop_shadow", "angle", "90", error) && setStyleValue(st, "drop_shadow", "distance", "10", error) &&
          setStyleValue(st, "drop_shadow", "size", "2", error) && setStyleValue(st, "drop_shadow", "color", "#ff0000", error) &&
          setStyleValue(st, "drop_shadow", "opacity", "1", error) && setStyleValue(st, "drop_shadow", "blend", "normal", error));
    CHECK(!setStyleValue(st, "drop_shadow", "wobble", "1", error) && !setStyleValue(st, "sparkle", "size", "1", error));
    CHECK(!setStyleValue(st, "drop_shadow", "size", "big", error));
    Image pic = composite(layers, W, H);
    CHECK(near(pic.at(30, 40), 255, 0, 0, 40));   // below the box: the shadow
    CHECK(near(pic.at(30, 20), 255, 255, 255));    // above: nothing
    CHECK(near(pic.at(30, 30), 0, 0, 0));          // the box covers its own shadow
    setStyleEffect(st, "drop_shadow", false);
    setStyleEffect(st, "stroke", true);
    setStyleValue(st, "stroke", "color", "#0000ff", error);
    setStyleValue(st, "stroke", "size", "3", error);
    pic = composite(layers, W, H);
    CHECK(near(pic.at(30, 23), 0, 0, 255) && near(pic.at(30, 20), 255, 255, 255) && near(pic.at(30, 30), 0, 0, 0));
    setStyleValue(st, "stroke", "position", "inside", error);
    pic = composite(layers, W, H);
    CHECK(near(pic.at(30, 26), 0, 0, 255) && near(pic.at(30, 23), 255, 255, 255) && near(pic.at(30, 30), 0, 0, 0));
    setStyleEffect(st, "color_overlay", true);
    setStyleValue(st, "color_overlay", "color", "#00ff00", error);
    pic = composite(layers, W, H);
    CHECK(near(pic.at(30, 30), 0, 255, 0) && near(pic.at(10, 10), 255, 255, 255));
    // the rest just has to draw something sensible
    for (const char* e : {"inner_shadow", "outer_glow", "inner_glow", "bevel", "gradient_overlay", "drop_shadow"}) setStyleEffect(st, e, true);
    layers[1].fill = 0.5;
    pic = composite(layers, W, H);
    CHECK(pic.at(10, 10) == (Color{255, 255, 255, 255}));
    // a small redraw gives exactly the pixels of a full one, as the canvas needs
    CHECK(sameImage(compositeRegion(layers, 20, 22, 17, 19), crop(pic, 20, 22, 17, 19)));
    CHECK(styleReach(st) >= 10);
    // as text and back
    const std::string text = styleToText(st);
    LayerStyle back;
    CHECK(styleFromText(text, back, error) && back == st);
    CHECK(!styleFromText("sparkle:size=3", back, error));
    CHECK(styleToText(LayerStyle{}).empty());
}

void testLockAlpha() {
    auto d = newImage(20, 20, {255, 255, 255, 255});
    d->image.layers.push_back(square("ink", 20, 20, 0, 0, 10, {0, 0, 0, 255}));
    d->image.layers[1].lockAlpha = true;
    ImageEdit paint(d, [](ImageDoc& doc) {
        fillRect(doc.image.layers[1].image, 0, 0, 20, 20, {255, 0, 0, 255});
        return wetwaresuite::Result::success("");
    });
    CHECK(paint.apply().ok);
    CHECK(d->image.layers[1].image.at(5, 5) == (Color{255, 0, 0, 255}) && d->image.layers[1].image.at(15, 15).a == 0);
    ImageEdit erase(
        d,
        [](ImageDoc& doc) {
            doc.touch(1, 0, 0, 20, 20);
            doc.image.layers[1].image.fill({0, 0, 0, 0});
            return wetwaresuite::Result::success("");
        },
        ImageEdit::Mode::Declared);
    CHECK(erase.apply().ok && d->image.layers[1].image.at(5, 5) == (Color{255, 0, 0, 255}));
    erase.revert();
    paint.revert();
    CHECK(d->image.layers[1].image.at(5, 5) == (Color{0, 0, 0, 255}));
    // layer settings are part of undo
    ImageEdit meta(d, [](ImageDoc& doc) {
        doc.image.layers[1].kind = LayerKind::Text;
        doc.image.layers[1].settings["text"] = "hi";
        doc.image.layers[1].style.stroke.on = true;
        return wetwaresuite::Result::success("");
    });
    CHECK(meta.apply().ok && d->takeChanges().all);
    meta.revert();
    CHECK(d->image.layers[1].kind == LayerKind::Pixel && d->image.layers[1].settings.empty() && !d->image.layers[1].style.any());
    std::string error;
    d->image.layers.push_back(Layer("adj"));
    d->image.layers.back().kind = LayerKind::Adjustment;
    CHECK(!pixelLayer(*d, 2, error) && error.find("adjustment") != std::string::npos);
    d->image.layers[1].kind = LayerKind::Text;
    CHECK(pixelLayer(*d, 1, error) && d->image.layers[1].kind == LayerKind::Pixel);
}

LayeredImage sampleStack() {
    const int W = 32, H = 24;
    LayeredImage li;
    li.width = W, li.height = H;
    li.layers.push_back({"Background", Image(W, H, {240, 230, 220, 255})});
    Layer member = square("Grüne Ebene", W, H, 4, 4, 12, {0, 160, 40, 255});
    member.depth = 1;
    member.blend = Blend::SoftLight;
    member.opacity = 0.8;
    member.mask = Mask(W, H, 255);
    std::fill_n(member.mask.row(5), W, 0);
    li.layers.push_back(member);
    Layer clipped = square("clip", W, H, 0, 0, 32, {200, 0, 200, 255});
    clipped.depth = 1;
    clipped.clip = true;
    clipped.fill = 0.5;
    clipped.lockAlpha = true;
    li.layers.push_back(clipped);
    Layer group("Group 1");
    group.kind = LayerKind::Group;
    group.passThrough = false;
    group.opacity = 0.9;
    li.layers.push_back(group);
    li.layers.push_back(square("hidden", W, H, 20, 2, 6, {0, 0, 255, 255}));
    li.layers.back().visible = false;
    li.layers.back().blend = Blend::LinearBurn;
    return li;
}

void testFiles() {
    const fs::path dir = fs::temp_directory_path() / "ww_test_layers";
    fs::create_directories(dir);
    LayeredImage li = sampleStack();
    // ORA keeps everything, our extras included
    {
        LayeredImage ora = li;
        Layer adj("Levels");
        adj.kind = LayerKind::Adjustment;
        adj.settings = {{"type", "levels"}, {"black", "20"}};
        ora.layers.push_back(adj);
        Layer text = square("Title", li.width, li.height, 2, 2, 5, {0, 0, 0, 255});
        text.kind = LayerKind::Text;
        text.settings = {{"text", "two\nlines & \"quotes\""}, {"size", "12"}};
        text.style.dropShadow.on = true;
        text.style.dropShadow.distance = 3;
        ora.layers.push_back(text);
        saveOra(ora, dir / "stack.ora");
        LayeredImage back = loadOra(dir / "stack.ora");
        CHECK(back.layers.size() == ora.layers.size());
        bool same = back.layers.size() == ora.layers.size();
        for (std::size_t i = 0; same && i < back.layers.size(); ++i) {
            same = static_cast<const LayerInfo&>(back.layers[i]) == static_cast<const LayerInfo&>(ora.layers[i]) &&
                   back.layers[i].mask == ora.layers[i].mask;
        }
        CHECK(same);
        CHECK(back.layers[6].settings.at("text") == "two\nlines & \"quotes\"" && back.layers[5].image.empty());
        CHECK(sameImage(composite(back.layers, back.width, back.height), composite(ora.layers, ora.width, ora.height)));
    }
    // PSD keeps the layers, groups, masks and clipping; other apps read the same file
    {
        std::vector<std::string> notes;
        const std::string bytes = encodePsd(li, &notes);
        CHECK(notes.empty());
        LayeredImage back = decodePsd(bytes);
        CHECK(back.width == li.width && back.height == li.height && back.layers.size() == li.layers.size());
        if (back.layers.size() == li.layers.size()) {
            CHECK(back.layers[1].name == "Grüne Ebene" && back.layers[1].depth == 1 && back.layers[1].blend == Blend::SoftLight);
            CHECK(std::abs(back.layers[1].opacity - 0.8) < 0.01 && back.layers[1].mask == li.layers[1].mask);
            CHECK(back.layers[2].clip && back.layers[2].lockAlpha && std::abs(back.layers[2].fill - 0.5) < 0.01);
            CHECK(back.layers[3].kind == LayerKind::Group && !back.layers[3].passThrough && back.layers[3].depth == 0);
            CHECK(!back.layers[4].visible && back.layers[4].blend == Blend::LinearBurn);
            CHECK(sameImage(back.layers[1].image, li.layers[1].image));
            CHECK(sameImage(composite(back.layers, back.width, back.height), composite(li.layers, li.width, li.height), 3));
        }
        // through the document functions, as photo.save / photo.open use them
        auto doc = std::make_shared<ImageDoc>();
        doc->image = li;
        Layer adj("Curves");
        adj.kind = LayerKind::Adjustment;
        adj.settings = {{"type", "curves"}, {"preset", "contrast"}};
        doc->image.layers.push_back(adj);
        saveImage(*doc, dir / "stack.psd", 90, &notes);
        CHECK(notes.size() == 1 && notes[0].find("Curves") != std::string::npos);
        auto opened = openImage(dir / "stack.psd");
        CHECK(opened->image.layers.size() == doc->image.layers.size() && opened->image.layers[3].kind == LayerKind::Group);
        CHECK(!decodePsd(bytes).layers.empty());
        bool threw = false;
        try {
            (void)decodePsd("8BPS nonsense");
        } catch (const std::exception&) {
            threw = true;
        }
        CHECK(threw);
    }
    std::error_code ec;
    fs::remove_all(dir, ec);
}

}  // namespace

int main() {
    testBlendModes();
    testGroups();
    testAdjustments();
    testStyles();
    testLockAlpha();
    testFiles();
    return check::summary("raster_layers");
}
