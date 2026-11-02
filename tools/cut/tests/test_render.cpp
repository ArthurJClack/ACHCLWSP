// The compositor and the mixer, checked by pixels and samples: no ffmpeg, no
// sound card. Media is made here (PNGs, WAVs, mattes, titles) and video files
// are stood in for by a frame hook.
#include <algorithm>
#include <chrono>
#include <fstream>
#include <thread>

#include "support.hpp"
#include "wetwaresuite/audio/analysis.hpp"

namespace cuttest {

namespace {

const raster::Color kRed{255, 0, 0, 255}, kGreen{0, 255, 0, 255}, kBlue{0, 0, 255, 255}, kWhite{255, 255, 255, 255}, kBlack{0, 0, 0, 255};

// A new 64x36 timeline with one full-frame still on v1; returns its id.
std::string stillDoc(Session& s, const std::string& png, const std::string& extra = {}) {
    Result r = run(s, "cut.new width=64 height=36 fps=10");
    const std::string doc = toText(r.value);
    run(s, "cut.add_image doc=" + doc + " path=" + q(png) + " duration=4 " + extra);
    return doc;
}

std::shared_ptr<Timeline> doc(Session& s, const std::string& id) { return s.workspace().getAs<Timeline>(id); }

Px mid(const Timeline& tl, double t) { return px(frame(tl, t), 32, 18); }

int countColour(const raster::Image& img, int r, int g, int b, int tol = 60) {
    int n = 0;
    for (int y = 0; y < img.height(); ++y) {
        for (int x = 0; x < img.width(); ++x) n += close(px(img, x, y), r, g, b, tol);
    }
    return n;
}

void testLayers(Session& s, const fs::path& dir) {
    CHECK(run(s, "cut.new width=64 height=36 fps=10 background=black").ok);
    auto tl = doc(s, "cut1");
    CHECK(run(s, "cut.add_color doc=cut1 color=red duration=4").ok);
    raster::Image img = frame(*tl, 1);
    CHECK(img.width() == 64 && img.height() == 36 && close(px(img, 32, 18), 255, 0, 0, 2));
    const std::string split = writePng(dir / "split.png", 64, 36, [](int x, int) { return x < 32 ? kBlue : kGreen; });
    CHECK(run(s, "cut.add_image doc=cut1 path=" + q(split) + " track=v2 start=0 duration=4").ok);  // c2
    img = frame(*tl, 1);
    CHECK(close(px(img, 8, 18), 0, 0, 255, 2) && close(px(img, 56, 18), 0, 255, 0, 2));
    // a half-size render keeps the picture
    img = frame(*tl, 1, 32);
    CHECK(img.width() == 32 && img.height() == 18 && close(px(img, 4, 9), 0, 0, 255, 4));

    // opacity and blend modes
    CHECK(run(s, "cut.set_clip doc=cut1 clip=c2 opacity=0.5").ok);
    CHECK(close(px(frame(*tl, 1), 8, 18), 128, 0, 128, 12));
    CHECK(run(s, "cut.set_clip doc=cut1 clip=c2 opacity=1 blend=screen").ok);
    img = frame(*tl, 1);
    CHECK(close(px(img, 8, 18), 255, 0, 255, 6) && close(px(img, 56, 18), 255, 255, 0, 6));
    CHECK(run(s, "cut.set_clip doc=cut1 clip=c2 blend=multiply").ok);
    CHECK(close(px(frame(*tl, 1), 8, 18), 0, 0, 0, 6));
    CHECK(run(s, "cut.set_clip doc=cut1 clip=c2 blend=difference").ok);
    CHECK(close(px(frame(*tl, 1), 8, 18), 255, 0, 255, 6));
    CHECK(run(s, "cut.set_clip doc=cut1 clip=c2 blend=normal").ok);

    // picture-in-picture in a corner, then turned, cropped, mirrored
    CHECK(run(s, "cut.transform doc=cut1 clip=c2 pip=top-right size=0.25 margin=0").ok);
    img = frame(*tl, 1);
    CHECK(close(px(img, 61, 4), 0, 255, 0, 6) && close(px(img, 51, 4), 0, 0, 255, 6) && close(px(img, 32, 18), 255, 0, 0, 2) &&
          close(px(img, 56, 20), 255, 0, 0, 2));
    {
        // the box keeps the picture's own shape: a 4:3 source gets a 4:3 box, flush with the margin
        Clip* c2 = tl->findClip("c2");
        const int sw = c2->sourceWidth, sh = c2->sourceHeight;
        c2->sourceWidth = 640, c2->sourceHeight = 480;
        CHECK(run(s, "cut.transform doc=cut1 clip=c2 pip=bottom-right size=0.5 margin=2").ok);
        CHECK(near(c2->width, 32) && near(c2->height, 24) && near(c2->x, 64 - 32 - 2) && near(c2->y, 36 - 24 - 2));
        CHECK(run(s, "undo").ok);
        c2 = tl->findClip("c2");
        c2->sourceWidth = sw, c2->sourceHeight = sh;
    }
    CHECK(run(s, "cut.transform doc=cut1 clip=c2 rotation=180").ok);
    img = frame(*tl, 1);
    CHECK(close(px(img, 51, 4), 0, 255, 0, 10) && close(px(img, 61, 4), 0, 0, 255, 10));
    CHECK(run(s, "cut.transform doc=cut1 clip=c2 pip=full").ok);
    CHECK(close(px(frame(*tl, 1), 8, 18), 0, 0, 255, 2));
    CHECK(run(s, "cut.transform doc=cut1 clip=c2 scale=0.5").ok);
    img = frame(*tl, 1);
    CHECK(close(px(img, 2, 2), 255, 0, 0, 2) && close(px(img, 24, 18), 0, 0, 255, 6) && close(px(img, 40, 18), 0, 255, 0, 6));
    CHECK(run(s, "cut.transform doc=cut1 clip=c2 scale=1 crop_right=50").ok);
    img = frame(*tl, 1);
    CHECK(close(px(img, 8, 18), 0, 0, 255, 2) && close(px(img, 56, 18), 255, 0, 0, 2));
    CHECK(run(s, "cut.transform doc=cut1 clip=c2 crop_right=0").ok);
    CHECK(run(s, "cut.effect doc=cut1 clip=c2 flip_h=true").ok);
    CHECK(close(px(frame(*tl, 1), 8, 18), 0, 255, 0, 2));
    CHECK(run(s, "cut.effect doc=cut1 clip=c2 reset=true").ok && close(px(frame(*tl, 1), 8, 18), 0, 0, 255, 2));
    // a picture box: fit letterboxes, x/y place it
    CHECK(run(s, "cut.transform doc=cut1 clip=c2 x=0 y=0 width=32 height=18").ok);
    img = frame(*tl, 1);
    CHECK(close(px(img, 4, 4), 0, 0, 255, 6) && close(px(img, 28, 4), 0, 255, 0, 6) && close(px(img, 40, 24), 255, 0, 0, 2));

    // keyframes: x slides the box across
    CHECK(run(s, "cut.keyframe doc=cut1 clip=c2 property=x t=0 value=0").ok);
    Result r = run(s, "cut.keyframe doc=cut1 clip=c2 property=x t=2 value=32");
    CHECK(r.ok && num(r, "keyframes") == 2);
    CHECK(close(px(frame(*tl, 0), 4, 4), 0, 0, 255, 6) && close(px(frame(*tl, 2), 4, 4), 255, 0, 0, 2) && close(px(frame(*tl, 2), 36, 4), 0, 0, 255, 6));
    CHECK(close(px(frame(*tl, 1), 4, 4), 255, 0, 0, 2) && close(px(frame(*tl, 1), 20, 4), 0, 0, 255, 6));  // halfway: x=16
    CHECK(!run(s, "cut.keyframe doc=cut1 clip=c2 property=opacity t=1 value=3").ok);   // out of range
    CHECK(!run(s, "cut.keyframe doc=cut1 clip=c2 property=x t=9 value=1").ok);         // past the clip
    CHECK(!run(s, "cut.keyframe doc=cut1 clip=c2 property=sparkle t=1 value=1").ok);
    CHECK(run(s, "cut.remove_keyframe doc=cut1 clip=c2 property=x all=true").ok);
    CHECK(run(s, "cut.transform doc=cut1 clip=c2 pip=full").ok);
    // opacity keyframes with ease, and hold
    CHECK(run(s, "cut.keyframe doc=cut1 clip=c2 property=opacity t=0 value=0").ok);
    CHECK(run(s, "cut.keyframe doc=cut1 clip=c2 property=opacity t=2 value=1").ok);
    CHECK(close(px(frame(*tl, 0), 8, 18), 255, 0, 0, 4) && close(px(frame(*tl, 1), 8, 18), 128, 0, 128, 14) && close(px(frame(*tl, 3), 8, 18), 0, 0, 255, 4));
    CHECK(run(s, "cut.keyframe doc=cut1 clip=c2 property=opacity t=0 value=0 ease=hold").ok);
    CHECK(close(px(frame(*tl, 1.5), 8, 18), 255, 0, 0, 4));
    CHECK(run(s, "cut.keyframe doc=cut1 clip=c2 property=opacity t=0 value=0 ease=ease_in").ok);
    CHECK(px(frame(*tl, 1), 8, 18).b < 110);  // slower at first than linear
    r = run(s, "cut.keyframes doc=cut1 clip=c2");
    CHECK(r.ok && num(r, "count") == 2 && contains(r.message, "ease_in"));
    CHECK(run(s, "cut.remove_keyframe doc=cut1 clip=c2 property=opacity t=2").ok);
    CHECK(run(s, "cut.remove_keyframe doc=cut1 clip=c2 all=true").ok && tl->findClip("c2")->keys.empty());
    // hidden and solo... a hidden track shows what's below
    CHECK(run(s, "cut.set_track doc=cut1 track=v2 hidden=true").ok && close(mid(*tl, 1), 255, 0, 0, 2));
    CHECK(run(s, "cut.set_track doc=cut1 track=v2 hidden=false").ok);
    // fades on pictures
    CHECK(run(s, "cut.set_track doc=cut1 track=v2 hidden=true").ok);
    CHECK(run(s, "cut.set_clip doc=cut1 clip=c1 fade_in=2").ok && close(mid(*tl, 0), 0, 0, 0, 2));
    CHECK(close(mid(*tl, 1), 128, 0, 0, 12));
}

void testColour(Session& s, const fs::path& dir) {
    const std::string gray = solidPng(dir / "gray.png", {64, 64, 64, 255});
    const std::string orange = solidPng(dir / "orange.png", {200, 100, 50, 255});
    std::string d = stillDoc(s, gray);
    auto tl = doc(s, d);
    CHECK(close(mid(*tl, 1), 64, 64, 64, 2));
    auto grade = [&](const std::string& args) { return run(s, "cut.filter doc=" + d + " clip=c1 reset=true " + args).ok; };
    CHECK(grade("exposure=1") && close(mid(*tl, 1), 128, 128, 128, 10));
    CHECK(grade("brightness=0.25") && mid(*tl, 1).r > 110);
    CHECK(grade("contrast=2") && mid(*tl, 1).r < 30);
    CHECK(grade("gamma=2") && mid(*tl, 1).r > 100);
    CHECK(grade("temperature=100") && mid(*tl, 1).r > mid(*tl, 1).b + 20);
    CHECK(grade("tint=100") && mid(*tl, 1).g < mid(*tl, 1).r - 10);
    CHECK(grade("wheel_gain=0.5") && close(mid(*tl, 1), 96, 96, 96, 10));
    CHECK(grade("wheel_gain=0.5,0,0") && mid(*tl, 1).r > mid(*tl, 1).g + 20);
    CHECK(grade("wheel_lift=0.3") && mid(*tl, 1).r > 80);
    CHECK(grade("wheel_gamma=0.5") && mid(*tl, 1).r > 80);
    CHECK(grade("curve=0,1,1,0") && close(mid(*tl, 1), 191, 191, 191, 10));
    CHECK(grade("curve_b=0,0,0.25,0.75,1,1") && mid(*tl, 1).b > mid(*tl, 1).r + 40);
    CHECK(!run(s, "cut.filter doc=" + d + " clip=c1 curve=0,0,2,1").ok);
    CHECK(!run(s, "cut.filter doc=" + d + " clip=c1 wheel_lift=4").ok);
    CHECK(grade("") && close(mid(*tl, 1), 64, 64, 64, 2));
    // keyframed exposure
    CHECK(run(s, "cut.keyframe doc=" + d + " clip=c1 property=exposure t=0 value=0").ok);
    CHECK(run(s, "cut.keyframe doc=" + d + " clip=c1 property=exposure t=2 value=1").ok);
    CHECK(close(mid(*tl, 0), 64, 64, 64, 4) && close(mid(*tl, 2), 128, 128, 128, 10) && mid(*tl, 1).r > 70 && mid(*tl, 1).r < 120);

    d = stillDoc(s, orange);
    tl = doc(s, d);
    CHECK(grade("saturation=0") && std::abs(mid(*tl, 1).r - mid(*tl, 1).b) < 6);
    CHECK(grade("grayscale=true") && std::abs(mid(*tl, 1).r - mid(*tl, 1).g) < 6);
    CHECK(grade("saturation=2") && mid(*tl, 1).r - mid(*tl, 1).b > 150);
    // a .cube LUT that swaps red and blue
    std::string cube = "TITLE \"swap\"\nLUT_3D_SIZE 2\n";
    for (int b = 0; b < 2; ++b)
        for (int g = 0; g < 2; ++g)
            for (int r = 0; r < 2; ++r) cube += std::to_string(b) + " " + std::to_string(g) + " " + std::to_string(r) + "\n";
    {
        std::ofstream(dir / "swap.cube") << cube;
    }
    Lut3d lut = parseCube(cube);
    CHECK(lut.size == 2 && lut.table.size() == 24 && lut.table[3] == 0.0f && lut.table[5] == 1.0f);
    CHECK(grade("lut=" + q(dir / "swap.cube")) && close(mid(*tl, 1), 50, 100, 200, 8));
    CHECK(!run(s, "cut.filter doc=" + d + " clip=c1 lut=" + q(dir / "nope.cube")).ok);
    CHECK(grade("") && close(mid(*tl, 1), 200, 100, 50, 2));
    auto curve = curveTable({0, 0, 1, 1});
    CHECK(curve.size() == 256 && near(curve[128], 128 / 255.0, 0.01));

    // blur, vignette
    const std::string split = writePng(dir / "split2.png", 64, 36, [](int x, int) { return x < 32 ? kBlue : kGreen; });
    d = stillDoc(s, split);
    tl = doc(s, d);
    CHECK(run(s, "cut.effect doc=" + d + " clip=c1 blur=6").ok);
    raster::Image img = frame(*tl, 1);
    CHECK(px(img, 31, 18).g > 50 && px(img, 31, 18).b > 50 && close(px(img, 2, 18), 0, 0, 255, 30));
    const std::string white = solidPng(dir / "white.png", kWhite);
    d = stillDoc(s, white);
    tl = doc(s, d);
    CHECK(run(s, "cut.effect doc=" + d + " clip=c1 vignette=1").ok);
    img = frame(*tl, 1);
    CHECK(px(img, 0, 0).r < px(img, 32, 18).r - 60 && px(img, 32, 18).r > 230);
    CHECK(run(s, "cut.effect doc=" + d + " clip=c1 reset=true sharpen=2").ok && close(mid(*tl, 1), 255, 255, 255, 2));
}

void testKeyAndTransitions(Session& s, const fs::path& dir) {
    // green screen: a red square on green over a blue background
    const std::string screen = writePng(dir / "screen.png", 64, 36, [](int x, int y) {
        if (x >= 24 && x < 40 && y >= 12 && y < 24) return kRed;
        if (x < 4) return raster::Color{90, 200, 80, 255};  // green spill on something light
        return kGreen;
    });
    Result r = run(s, "cut.new width=64 height=36 fps=10");
    const std::string d = toText(r.value);
    auto tl = doc(s, d);
    CHECK(run(s, "cut.add_color doc=" + d + " color=blue duration=4").ok);
    CHECK(run(s, "cut.add_image doc=" + d + " path=" + q(screen) + " track=v2 start=0 duration=4").ok);
    CHECK(close(mid(*tl, 1), 255, 0, 0, 2) && close(px(frame(*tl, 1), 50, 4), 0, 255, 0, 2));
    CHECK(run(s, "cut.chroma_key doc=" + d + " clip=c2 color=#00ff00 similarity=0.3 spill=1").ok);
    raster::Image img = frame(*tl, 1);
    CHECK(close(px(img, 50, 4), 0, 0, 255, 10) && close(px(img, 32, 18), 255, 0, 0, 10));
    CHECK(px(img, 1, 18).g <= std::max(px(img, 1, 18).r, px(img, 1, 18).b) + 2);  // spill: no green left on top
    CHECK(run(s, "cut.chroma_key doc=" + d + " clip=c2 off=true").ok && close(px(frame(*tl, 1), 50, 4), 0, 255, 0, 2));

    // transitions between a red and a blue matte
    r = run(s, "cut.new width=64 height=36 fps=10");
    const std::string t = toText(r.value);
    tl = doc(s, t);
    CHECK(run(s, "cut.add_color doc=" + t + " color=red duration=2").ok);
    CHECK(run(s, "cut.add_color doc=" + t + " color=blue duration=2").ok);
    r = run(s, "cut.transition doc=" + t + " clip=c2 type=crossfade duration=1");
    CHECK(r.ok && near(tl->findClip("c2")->start, 1) && near(tl->duration(), 3));
    CHECK(close(mid(*tl, 0.5), 255, 0, 0, 2) && close(mid(*tl, 1.5), 128, 0, 128, 14) && close(mid(*tl, 2.5), 0, 0, 255, 2));
    CHECK(run(s, "cut.transition doc=" + t + " clip=c2 type=fadeblack duration=1").ok);
    CHECK(mid(*tl, 1.5).r < 30 && mid(*tl, 1.5).b < 30);
    CHECK(run(s, "cut.transition doc=" + t + " clip=c2 type=fadewhite duration=1").ok);
    CHECK(mid(*tl, 1.5).g > 220);
    CHECK(run(s, "cut.transition doc=" + t + " clip=c2 type=wipeleft duration=1").ok);
    img = frame(*tl, 1.5);
    CHECK(px(img, 2, 18).r != px(img, 61, 18).r);
    for (const char* type : {"slideleft", "pushup", "zoomin", "circleopen", "radial", "pixelize", "smoothleft", "dissolve"}) {
        CHECK(run(s, "cut.transition doc=" + t + " clip=c2 type=" + type + " duration=1").ok);
        img = frame(*tl, 1.5);
        const bool between = std::string(type) == "pixelize" || countColour(img, 255, 0, 0, 2) < 64 * 36 && countColour(img, 0, 0, 255, 2) < 64 * 36;
        if (!between) std::printf("  %s isn't between the clips at its middle\n", type);
        CHECK(between);
        CHECK(close(mid(*tl, 0.5), 255, 0, 0, 2) && close(mid(*tl, 2.5), 0, 0, 255, 2));
    }
    CHECK(!run(s, "cut.transition doc=" + t + " clip=c2 type=wobble").ok);
    CHECK(!run(s, "cut.transition doc=" + t + " clip=c2 duration=5").ok);
    CHECK(!run(s, "cut.transition doc=" + t + " clip=c1").ok);  // nothing before it
    CHECK(run(s, "cut.transition doc=" + t + " clip=c2 type=none").ok && tl->findClip("c2")->transition.empty());
}

void testTitles(Session& s, const fs::path& dir) {
    Result r = run(s, "cut.new width=160 height=90 fps=10");
    const std::string d = toText(r.value);
    auto tl = doc(s, d);
    CHECK(run(s, "cut.add_color doc=" + d + " color=black duration=5").ok);
    r = run(s, "cut.add_title doc=" + d + " text=HI font_size=40 start=0 duration=3");
    CHECK(r.ok && tl->findClip("c2")->kind == "title");
    CHECK(brightCount(frame(*tl, 1)) > 40 && brightCount(frame(*tl, 4)) == 0);
    CHECK(run(s, "cut.set_clip doc=" + d + " clip=c2 outline=3 outline_color=red").ok);
    CHECK(countColour(frame(*tl, 1), 255, 0, 0, 40) > 20);
    CHECK(run(s, "cut.set_clip doc=" + d + " clip=c2 outline=0 shadow=4 shadow_color=blue").ok);
    CHECK(countColour(frame(*tl, 1), 0, 0, 255, 120) > 10);
    CHECK(run(s, "cut.set_clip doc=" + d + " clip=c2 shadow=0 box=true box_color=#00ff00").ok);
    CHECK(countColour(frame(*tl, 1), 0, 255, 0, 20) > 50);
    CHECK(run(s, "cut.set_clip doc=" + d + " clip=c2 box=false position=top-left").ok);
    raster::Image img = frame(*tl, 1);
    int top = 0, bottom = 0;
    for (int y = 0; y < 90; ++y)
        for (int x = 0; x < 160; ++x) (y < 45 ? top : bottom) += px(img, x, y).r > 200;
    CHECK(top > 20 && bottom == 0);
    // animations: typewriter shows more letters later, fade in starts faint
    CHECK(run(s, "cut.set_clip doc=" + d + " clip=c2 text=\"HELLO WORLD\" font_size=20 position=center anim_in=typewriter anim_duration=2").ok);
    CHECK(brightCount(frame(*tl, 0.3)) < brightCount(frame(*tl, 2.5)) && brightCount(frame(*tl, 2.5)) > 30);
    CHECK(run(s, "cut.set_clip doc=" + d + " clip=c2 anim_in=fade anim_out=slide_down anim_duration=1").ok);
    CHECK(brightCount(frame(*tl, 0.1)) < brightCount(frame(*tl, 1.5)) / 2);
    CHECK(run(s, "cut.set_clip doc=" + d + " clip=c2 anim_in=zoom").ok && brightCount(frame(*tl, 1.5)) > 30);
    // a keyframed title moves
    CHECK(run(s, "cut.set_clip doc=" + d + " clip=c2 anim_in=none anim_out=none").ok);
    CHECK(run(s, "cut.keyframe doc=" + d + " clip=c2 property=opacity t=0 value=0").ok && brightCount(frame(*tl, 0)) == 0);
    // a lower third with its accent bar and second line
    r = run(s, "cut.add_lower_third doc=" + d + " text=\"Ada Lovelace\" subtitle=Mathematician accent_color=#ff0000 start=0 duration=4");
    CHECK(r.ok);
    img = frame(*tl, 2);
    CHECK(countColour(img, 255, 0, 0, 40) > 10);
    int lower = 0;
    for (int y = 45; y < 90; ++y)
        for (int x = 0; x < 160; ++x) lower += px(img, x, y).r > 200 && px(img, x, y).g > 200;
    CHECK(lower > 20);

    // captions: import, see them, export
    const fs::path srt = dir / "c.srt";
    {
        std::ofstream(srt) << "1\n00:00:00,500 --> 00:00:01,500\nHello\n\n2\n00:00:02,000 --> 00:00:03,000\nAgain\n";
    }
    r = run(s, "cut.new width=160 height=90 fps=10");
    const std::string c = toText(r.value);
    tl = doc(s, c);
    CHECK(run(s, "cut.add_color doc=" + c + " color=black duration=4").ok);
    r = run(s, "cut.import_captions doc=" + c + " path=" + q(srt) + " font_size=16");
    CHECK(r.ok && num(r, "captions") == 2);
    CHECK(brightCount(frame(*tl, 1)) > 10 && brightCount(frame(*tl, 1.75)) == 0);
    CHECK(run(s, "cut.add_caption doc=" + c + " text=Third start=3.2 duration=0.5").ok);
    const fs::path out = dir / "out.srt";
    r = run(s, "cut.export_captions doc=" + c + " output=" + q(out));
    CHECK(r.ok && num(r, "captions") == 3);
    auto back = parseCaptions(readTextFile(out));
    CHECK(back.size() == 3 && back[0].text == "Hello" && near(back[2].start, 3.2, 0.01));
    CHECK(!run(s, "cut.export_captions doc=" + c + " output=" + q(out)).ok);  // exists
    CHECK(run(s, "cut.import_captions doc=" + c + " path=" + q(srt) + " replace=true").ok);
    r = run(s, "cut.export_captions doc=" + c + " output=" + q(dir / "out.vtt"));
    CHECK(r.ok && num(r, "captions") == 2);
    // a frame and a WAV export, no ffmpeg
    r = run(s, "cut.export_frame doc=" + c + " t=1 output=" + q(dir / "f.png"));
    CHECK(r.ok && num(r, "width") == 160 && raster::load(dir / "f.png").height() == 90);
    r = run(s, "cut.export_frame doc=" + c + " t=1 width=80 output=" + q(dir / "f.jpg"));
    CHECK(r.ok && num(r, "width") == 80);
    CHECK(!run(s, "cut.export_frame doc=" + c + " t=1 output=" + q(dir / "f.mp4")).ok);
}

void testVideoHook(Session& s) {
    // video files come from a hook: the frame's red channel is the source time x 20
    setVideoFrameSource([](const std::string&, double t, int w, int h, raster::Image& out) {
        out = raster::Image(w, h, {static_cast<std::uint8_t>(std::clamp(t * 20, 0.0, 255.0)), 0, 0, 255});
        return true;
    });
    Result r = run(s, "cut.new width=64 height=36 fps=10");
    const std::string d = toText(r.value);
    auto tl = doc(s, d);
    Clip v;
    v.kind = "video";
    v.source = "/virtual/clip.mp4";
    v.out = 10;
    v.sourceDuration = 10;
    v.sourceWidth = 64;
    v.sourceHeight = 36;
    v.sourceFps = 10;
    v.id = tl->newClipId();
    tl->ensureTrack("v1").clips.push_back(v);
    CHECK(close(mid(*tl, 1), 20, 0, 0, 3) && close(mid(*tl, 5), 100, 0, 0, 3));
    CHECK(run(s, "cut.clip_speed doc=" + d + " clip=c1 speed=2").ok && near(tl->duration(), 5));
    CHECK(close(mid(*tl, 1), 40, 0, 0, 3));
    CHECK(run(s, "cut.clip_speed doc=" + d + " clip=c1 reverse=true").ok);
    CHECK(close(mid(*tl, 1), 160, 0, 0, 4));
    CHECK(run(s, "cut.clip_speed doc=" + d + " clip=c1 duration=10 reverse=false").ok && near(tl->findClip("c1")->speed, 1));
    // freeze frame: the frame at 1s holds for 2s, the rest follows
    r = run(s, "cut.freeze_frame doc=" + d + " clip=c1 at=1 duration=2");
    CHECK(r.ok && near(tl->duration(), 12));
    CHECK(close(mid(*tl, 1.5), 20, 0, 0, 3) && close(mid(*tl, 2.9), 20, 0, 0, 3) && close(mid(*tl, 3.5), 30, 0, 0, 3));
    CHECK(close(mid(*tl, 0.5), 10, 0, 0, 3));
    CHECK(run(s, "undo").ok && near(tl->duration(), 10));
    // nesting keeps the picture; unnesting puts it back
    r = run(s, "cut.nest doc=" + d + " clips=c1 name=Scene");
    CHECK(r.ok && tl->findClip(toText(r.value))->kind == "nest" && close(mid(*tl, 1), 20, 0, 0, 3));
    const std::string nest = toText(r.value);
    CHECK(run(s, "cut.set_clip doc=" + d + " clip=" + nest + " in=2").ok);  // trim the nest's start
    CHECK(close(mid(*tl, 1), 60, 0, 0, 3));
    CHECK(run(s, "cut.unnest doc=" + d + " clip=" + nest).ok);
    CHECK(!tl->findClip(nest) && tl->track("v1")->clips.size() == 1 && close(mid(*tl, 1), 60, 0, 0, 3));
    setVideoFrameSource({});
}

// What the window uses: files opened as timelines, dropped files, thumbnails, waveforms and the player.
void testWindowHooks(Session& s, const fs::path& dir) {
    // a still opens as a timeline its size; a sound file as a timeline with it on a1
    const std::string png = solidPng(dir / "wide.png", kGreen, 120, 40);
    Result r = run(s, "cut.open path=" + q(png));
    CHECK(r.ok && num(r, "width") == 120 && num(r, "height") == 40 && near(num(r, "duration"), 5));
    auto still = doc(s, toText(r.value));
    const std::string stillId = toText(r.value);
    CHECK(still && still->track("v1") && still->track("v1")->clips.size() == 1 && still->path.empty());
    const std::string wav = writeTone(dir / "beep.wav", 2, 440, 0.5);
    r = run(s, "cut.open path=" + q(wav));
    CHECK(r.ok && near(num(r, "duration"), 2, 0.01));
    auto sound = doc(s, toText(r.value));
    CHECK(sound && sound->track("a1") && !sound->track("v1"));
    const fs::path junk = dir / "notes.xyz";
    std::ofstream(junk) << "not media";
    CHECK(!run(s, "cut.open path=" + q(junk)).ok);

    // dropping files: pictures on a video track, sound on an audio track, odd paths escaped
    wetwaresuite::TimelineEdit drop;
    drop.kind = TimelineEditKind::AddFile;
    drop.name = (dir / "a b$#.png").generic_string();
    CHECK(still->timelineCommand(drop) == "cut.add_clip path=" + dir.generic_string() + "/a\\ b\\$\\#.png");  // not `dir / ...`: windows reads \\ as a folder
    CHECK(cli::split(still->timelineCommand(drop))[1] == "path=" + drop.name);
    drop.name = wav;
    drop.track = "a1";
    CHECK(still->timelineCommand(drop) == "cut.add_audio path=" + wav);
    // ... and they land on the timeline
    CHECK(run(s, still->timelineCommand(drop) + " doc=" + toText(r.value)).ok);  // into the sound timeline
    CHECK(sound->track("a1")->clips.size() == 2 && near(sound->duration(), 4, 0.01));

    // the clip panel: a picture's settings (no sound ones), each one a cut.set_clip
    {
        const std::string id = still->tracks.front().clips.front().id;
        const auto props = still->clipProperties(id);
        auto has = [&](const char* name) {
            return std::any_of(props.begin(), props.end(), [&](const ClipProperty& p) { return p.name == name; });
        };
        CHECK(has("opacity") && has("fade_in") && has("grayscale") && !has("volume") && !has("speed"));
        CHECK(sound->clipProperties(sound->track("a1")->clips.front().id).size() == 7);  // speed, fades, volume, pan, mute, noise cleanup
        CHECK(still->clipProperties("c99").empty());
        wetwaresuite::TimelineEdit set{TimelineEditKind::SetProperty, id};
        set.name = "opacity";
        set.at = 0.5;
        CHECK(still->timelineCommand(set) == "cut.set_clip clip=" + id + " opacity=0.5000");
        set.name = "grayscale";
        set.at = 1;
        CHECK(still->timelineCommand(set) == "cut.set_clip clip=" + id + " grayscale=true");
        set.name = "bad name=1";
        CHECK(still->timelineCommand(set).empty());
        set.name = "opacity";
        set.at = 0.25;
        CHECK(run(s, still->timelineCommand(set) + " doc=" + stillId).ok && near(still->findClip(id)->opacity, 0.25));
    }

    // the layout says where each clip's media comes from
    TimelineLayout layout;
    CHECK(still->timeline(layout) && layout.tracks.size() == 1);
    CHECK(layout.tracks[0].clips[0].picture && !layout.tracks[0].clips[0].sound && near(layout.tracks[0].clips[0].speed, 0));

    // thumbnails and waveforms come in the background
    auto strip = [](const Timeline& tl, const std::string& clip) {
        MediaStrip m;
        for (int i = 0; i < 500; ++i) {
            if (!tl.clipMedia(clip, m) || !m.pending) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        return m;
    };
    MediaStrip m = strip(*still, still->track("v1")->clips[0].id);
    CHECK(!m.pending && m.frames.size() == 1 && m.frameHeight == 54 && m.frameWidth == 162);
    CHECK(m.frames[0].size() == static_cast<std::size_t>(m.frameWidth * m.frameHeight * 4) && m.frames[0][1] == 255);
    m = strip(*sound, sound->track("a1")->clips[0].id);
    CHECK(!m.pending && m.frames.empty() && near(m.peaksPerSecond, 50) && m.peaks.size() == 100);
    CHECK(near(*std::max_element(m.peaks.begin(), m.peaks.end()), 1, 1e-4) && m.peaks[50] > 0.9f);
    MediaStrip none;
    CHECK(!still->clipMedia("c99", none));

    // video: frames along the source from the hook, and a player that keeps a copy of the edit
    setVideoFrameSource([](const std::string&, double t, int w, int h, raster::Image& out) {
        out = raster::Image(w, h, {static_cast<std::uint8_t>(std::clamp(t * 20, 0.0, 255.0)), 0, 0, 255});
        return true;
    });
    r = run(s, "cut.new width=64 height=36 fps=10");
    auto tl = doc(s, toText(r.value));
    Clip v;
    v.kind = "video";
    v.source = (dir / "virtual.mp4").generic_string();
    v.out = 10;
    v.sourceDuration = 10;
    v.sourceWidth = 64;
    v.sourceHeight = 36;
    v.id = tl->newClipId();
    tl->ensureTrack("v1").clips.push_back(v);
    m = strip(*tl, v.id);
    CHECK(!m.pending && m.frames.size() == 10 && m.frameWidth == 96 && near(m.frameTimes[0], 0.5) && near(m.frameTimes[9], 9.5));
    CHECK(m.frames[3][0] == 70 && m.peaks.empty());  // 3.5 s x 20
    auto player = tl->player();
    CHECK(player && near(player->fps(), 10));
    int w = 0, h = 0;
    std::vector<std::uint8_t> rgba;
    CHECK(player->frame(2, 32, w, h, rgba) && w == 32 && h == 18 && rgba.size() == 32u * 18 * 4);
    CHECK(std::abs(rgba[(9 * 32 + 16) * 4] - 40) <= 3);
    tl->tracks.clear();  // editing the document doesn't touch what's playing
    CHECK(player->frame(4, 64, w, h, rgba) && w == 64 && std::abs(rgba[(18 * 64 + 32) * 4] - 80) <= 3);
    CHECK(!tl->player()->frame(1, 64, w, h, rgba));  // nothing left to play
    setVideoFrameSource({});
}

}  // namespace

std::string readTextFile(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(in), {});
}

void testRender() {
    const fs::path dir = tempDir("render");
    Session s;
    registerCommands(s);
    testLayers(s, dir);
    testColour(s, dir);
    testKeyAndTransitions(s, dir);
    testTitles(s, dir);
    testVideoHook(s);
    testWindowHooks(s, dir);
    fs::remove_all(dir);
}

void testMix() {
    const fs::path dir = tempDir("mix");
    Session s;
    registerCommands(s);
    const std::string tone = writeTone(dir / "tone.wav", 3, 440, 0.5);
    // a source that gets louder: silence then a tone
    audio::Buffer ramp(2, 48000 * 2, 48000);
    for (std::size_t i = 48000; i < ramp.frames(); ++i) ramp.channels[0][i] = ramp.channels[1][i] = 0.5f * static_cast<float>(std::sin(i * 0.05));
    audio::save(ramp, dir / "ramp.wav");

    Result r = run(s, "cut.new width=64 height=36 fps=10");
    auto tl = doc(s, toText(r.value));
    const std::string d = toText(r.value);
    r = run(s, "cut.add_audio doc=" + d + " path=" + q(tone) + " duration=2");
    CHECK(r.ok && near(tl->duration(), 2));
    CHECK(hasSound(*tl));
    audio::Buffer m = mixAudio(*tl, 0, 2);
    CHECK(m.frames() == 96000 && near(rms(m, 0, 96000), 0.354, 0.01));
    CHECK(run(s, "cut.set_clip doc=" + d + " clip=c1 volume=0.5").ok && near(rms(mixAudio(*tl, 0, 2), 0, 96000), 0.177, 0.01));
    CHECK(run(s, "cut.set_clip doc=" + d + " clip=c1 volume=1 pan=-1").ok);
    m = mixAudio(*tl, 0, 2);
    CHECK(rms(m, 0, 96000, 0) > 0.3 && rms(m, 0, 96000, 1) < 0.01);
    CHECK(run(s, "cut.set_clip doc=" + d + " clip=c1 pan=0 fade_in=1").ok);
    m = mixAudio(*tl, 0, 2);
    CHECK(rms(m, 0, 4800) < 0.05 && near(rms(m, 72000, 96000), 0.354, 0.01));
    CHECK(run(s, "cut.set_clip doc=" + d + " clip=c1 fade_in=0").ok);
    // keyframed volume
    CHECK(run(s, "cut.keyframe doc=" + d + " clip=c1 property=volume t=0 value=0").ok);
    CHECK(run(s, "cut.keyframe doc=" + d + " clip=c1 property=volume t=2 value=1").ok);
    m = mixAudio(*tl, 0, 2);
    CHECK(rms(m, 0, 9600) < 0.05 && rms(m, 86400, 96000) > 0.3);
    CHECK(run(s, "cut.remove_keyframe doc=" + d + " clip=c1 all=true").ok);
    // mute, track mute and solo, master volume
    CHECK(run(s, "cut.set_clip doc=" + d + " clip=c1 mute=true").ok && rms(mixAudio(*tl, 0, 2), 0, 96000) < 1e-6 && !hasSound(*tl));
    CHECK(run(s, "cut.set_clip doc=" + d + " clip=c1 mute=false").ok);
    CHECK(run(s, "cut.set_track doc=" + d + " track=a1 muted=true").ok && rms(mixAudio(*tl, 0, 2), 0, 96000) < 1e-6);
    CHECK(run(s, "cut.set_track doc=" + d + " track=a1 muted=false volume=0.5").ok && near(rms(mixAudio(*tl, 0, 2), 0, 96000), 0.177, 0.01));
    CHECK(run(s, "cut.set_track doc=" + d + " track=a1 volume=1").ok);
    CHECK(run(s, "cut.add_audio doc=" + d + " path=" + q(tone) + " track=a2 start=0 duration=2").ok);
    CHECK(run(s, "cut.set_track doc=" + d + " track=a2 solo=true").ok && near(rms(mixAudio(*tl, 0, 2), 0, 96000), 0.354, 0.01));
    CHECK(run(s, "cut.set_track doc=" + d + " track=a2 solo=false").ok && near(rms(mixAudio(*tl, 0, 2), 0, 96000), 0.707, 0.02));
    CHECK(run(s, "cut.settings doc=" + d + " master_volume=0.5").ok && near(rms(mixAudio(*tl, 0, 2), 0, 96000), 0.354, 0.01));
    CHECK(run(s, "cut.settings doc=" + d + " master_volume=1").ok && run(s, "cut.delete_clip doc=" + d + " clip=c2").ok);

    // speed keeps the pitch: the same number of zero crossings per second
    CHECK(run(s, "cut.clip_speed doc=" + d + " clip=c1 speed=2").ok && near(tl->duration(), 1));
    m = mixAudio(*tl, 0, 1);
    int crossings = 0;
    for (std::size_t i = 4800; i < 43200; ++i) crossings += (m.channels[0][i - 1] < 0) != (m.channels[0][i] < 0);
    CHECK(std::abs(crossings - 880 * 0.8) < 60);
    CHECK(run(s, "cut.clip_speed doc=" + d + " clip=c1 speed=1").ok);
    // reverse: the loud part comes first
    r = run(s, "cut.add_audio doc=" + d + " path=" + q(dir / "ramp.wav") + " track=a3 start=0");
    const std::string rampId = toText(r.value);
    CHECK(run(s, "cut.set_track doc=" + d + " track=a3 solo=true").ok);
    m = mixAudio(*tl, 0, 2);
    CHECK(rms(m, 0, 48000) < 0.01 && rms(m, 48000, 96000) > 0.3);
    CHECK(run(s, "cut.set_clip doc=" + d + " clip=" + rampId + " reverse=true").ok);
    m = mixAudio(*tl, 0, 2);
    CHECK(rms(m, 0, 48000) > 0.3 && rms(m, 48000, 96000) < 0.01);
    CHECK(run(s, "cut.delete_clip doc=" + d + " clip=" + rampId).ok && run(s, "cut.set_track doc=" + d + " track=a3 solo=false").ok);

    // an audio crossfade: equal power, no dip in the middle
    r = run(s, "cut.add_audio doc=" + d + " path=" + q(tone) + " duration=2");  // after c1 on a1
    const std::string second = toText(r.value);
    CHECK(run(s, "cut.transition doc=" + d + " clip=" + second + " duration=1").ok);
    CHECK(near(tl->duration(), 3) && tl->findClip(second)->transition == "crossfade");
    m = mixAudio(*tl, 0, 3);
    CHECK(rms(m, 64800, 79200) > 0.3);  // around 1.5 s

    // normalize and loudness
    r = run(s, "cut.normalize doc=" + d + " clip=c1 peak=-6");
    CHECK(r.ok && near(tl->findClip("c1")->volume, std::pow(10, -6 / 20.0) / 0.5, 0.02));
    CHECK(!run(s, "cut.normalize doc=" + d + " clip=c1 peak=3").ok);
    r = run(s, "cut.loudness doc=" + d + " measure_only=true");
    CHECK(r.ok && num(r, "lufs") < 0 && num(r, "lufs") > -30);
    r = run(s, "cut.loudness doc=" + d + " target=-20 ceiling=0");
    CHECK(r.ok);
    m = mixAudio(*tl, 0, tl->duration());
    CHECK(near(audio::integratedLoudness(m, audio::all(m)), -20, 0.6));
    r = run(s, "cut.loudness doc=" + d + " target=-3 ceiling=-1");  // too loud: the peak holds it back
    CHECK(r.ok && text(r, "limited") == "true");

    // the mix exported as WAV without ffmpeg
    r = run(s, "cut.render doc=" + d + " output=" + q(dir / "mix.wav"));
    CHECK(r.ok && text(r, "mode") == "mix" && near(audio::load(dir / "mix.wav").seconds(), 3, 0.01));
    r = run(s, "cut.render doc=" + d + " output=" + q(dir / "part.wav") + " start=1 end=2");
    CHECK(r.ok && near(audio::load(dir / "part.wav").seconds(), 1, 0.01));
    fs::remove_all(dir);
}

}  // namespace cuttest
