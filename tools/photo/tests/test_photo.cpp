#include <fstream>
#include <filesystem>

#include "wetwaresuite/photo/photo.hpp"
#include "wetwaresuite/raster/codec.hpp"
#include "wetwaresuite/raster/document.hpp"
#include "wetwaresuite/raster/ops.hpp"
#include "wetwaresuite/raster/selection.hpp"
#include "check.hpp"

using namespace wetwaresuite;
namespace fs = std::filesystem;

namespace {

std::string docOf(const Result& r) {
    const Value* v = r.field("doc");
    return v ? toText(*v) : "";
}

std::int64_t intField(const Result& r, const char* key) {
    const Value* v = r.field(key);
    return v ? std::get<std::int64_t>(*v) : -1;
}


using raster::Color;

Params P(std::initializer_list<std::pair<const std::string, Value>> kv) { return Params(kv); }

Color px(Session& s, const std::string& id, int x, int y, int layer = -1) {
    auto doc = s.workspace().getAs<raster::ImageDoc>(id);
    if (layer < 0) return doc->flatten().at(x, y);
    return doc->image.layers[static_cast<std::size_t>(layer)].image.at(x, y);
}

void testSelections(const fs::path& dir) {
    Session s;
    photo::registerCommands(s);
    const std::string id = docOf(s.run("photo.new", P({{"width", std::int64_t{100}}, {"height", std::int64_t{80}}, {"background", std::string("#808080")}})));
    auto doc = s.workspace().getAs<raster::ImageDoc>(id);

    // rect: adjustments stay inside
    Result r = s.run("photo.select", P({{"doc", id}, {"shape", std::string("rect")}, {"x", 10.0}, {"y", 10.0}, {"width", 30.0}, {"height", 20.0}}));
    CHECK(r.ok && intField(r, "width") == 30 && intField(r, "height") == 20);
    CHECK(s.run("photo.adjust", P({{"doc", id}, {"kind", std::string("brightness")}, {"amount", 50.0}})).ok);
    CHECK(px(s, id, 15, 15).r > 140 && px(s, id, 50, 50).r == 128);
    CHECK(s.history().undo() && px(s, id, 15, 15).r == 128);
    CHECK(s.history().undo() && !doc->hasSelection());  // selecting is a step too
    CHECK(s.history().redo() && doc->hasSelection());

    // add an ellipse, subtract, intersect, invert
    CHECK(s.run("photo.select", P({{"doc", id}, {"shape", std::string("ellipse")}, {"x", 60.0}, {"y", 40.0}, {"width", 30.0}, {"height", 30.0}, {"mode", std::string("add")}})).ok);
    CHECK(doc->selection.at(75, 55) == 255 && doc->selection.at(15, 15) == 255 && doc->selection.at(5, 5) == 0);
    CHECK(doc->selection.at(61, 41) == 0);  // ellipse corners stay out
    CHECK(s.run("photo.select", P({{"doc", id}, {"shape", std::string("rect")}, {"x", 0.0}, {"y", 0.0}, {"width", 20.0}, {"height", 80.0}, {"mode", std::string("subtract")}})).ok);
    CHECK(doc->selection.at(15, 15) == 0 && doc->selection.at(25, 15) == 255);
    CHECK(s.run("photo.select", P({{"doc", id}, {"shape", std::string("invert")}})).ok);
    CHECK(doc->selection.at(15, 15) == 255 && doc->selection.at(25, 15) == 0);
    CHECK(s.run("photo.select", P({{"doc", id}, {"shape", std::string("none")}})).ok && !doc->hasSelection());

    // lasso with antialiased edges
    CHECK(s.run("photo.select", P({{"doc", id}, {"shape", std::string("lasso")}, {"points", NumberList{10, 10, 90, 10, 50, 70}}})).ok);
    CHECK(doc->selection.at(50, 20) == 255 && doc->selection.at(5, 60) == 0);
    bool soft = false;
    for (int x = 0; x < 100; ++x) soft = soft || (doc->selection.at(x, 40) > 0 && doc->selection.at(x, 40) < 255);
    CHECK(soft);
    CHECK(!s.run("photo.select", P({{"doc", id}, {"shape", std::string("lasso")}, {"points", NumberList{1, 2}}})).ok);

    // fill, then the wand finds the filled area
    CHECK(s.run("photo.select", P({{"doc", id}, {"shape", std::string("rect")}, {"x", 20.0}, {"y", 20.0}, {"width", 20.0}, {"height", 20.0}})).ok);
    CHECK(s.run("photo.fill_selection", P({{"doc", id}, {"color", std::string("#ff0000")}})).ok);
    CHECK(px(s, id, 25, 25) == Color{255, 0, 0, 255} && px(s, id, 10, 10).r == 128);
    r = s.run("photo.select", P({{"doc", id}, {"shape", std::string("wand")}, {"x", 30.0}, {"y", 30.0}, {"tolerance", std::int64_t{10}}}));
    CHECK(r.ok && intField(r, "x") == 20 && intField(r, "width") == 20);
    r = s.run("photo.select", P({{"doc", id}, {"shape", std::string("color")}, {"color", std::string("#808080")}}));
    CHECK(r.ok && doc->selection.at(25, 25) == 0 && doc->selection.at(5, 5) == 255);

    // grow, shrink, feather, border
    CHECK(s.run("photo.select", P({{"doc", id}, {"shape", std::string("rect")}, {"x", 40.0}, {"y", 30.0}, {"width", 20.0}, {"height", 20.0}})).ok);
    CHECK(s.run("photo.select_modify", P({{"doc", id}, {"action", std::string("grow")}, {"amount", 5.0}})).ok);
    CHECK(doc->selection.at(36, 30) == 255 && doc->selection.at(34, 30) == 0);
    CHECK(s.run("photo.select_modify", P({{"doc", id}, {"action", std::string("shrink")}, {"amount", 10.0}})).ok);
    CHECK(doc->selection.at(44, 40) == 0 && doc->selection.at(50, 40) == 255);
    CHECK(s.run("photo.select_modify", P({{"doc", id}, {"action", std::string("feather")}, {"amount", 4.0}})).ok);
    CHECK(doc->selection.at(45, 40) > 0 && doc->selection.at(45, 40) < 255);
    CHECK(s.run("photo.select_modify", P({{"doc", id}, {"action", std::string("border")}, {"amount", 4.0}})).ok);
    CHECK(doc->selection.at(50, 40) < 128);

    // copy to a layer, cut, delete
    CHECK(s.run("photo.select", P({{"doc", id}, {"shape", std::string("rect")}, {"x", 20.0}, {"y", 20.0}, {"width", 10.0}, {"height", 10.0}})).ok);
    r = s.run("photo.copy_to_layer", P({{"doc", id}, {"cut", true}}));
    CHECK(r.ok && doc->image.layers.size() == 2 && doc->active == 1);
    CHECK(px(s, id, 22, 22, 1) == Color{255, 0, 0, 255} && px(s, id, 22, 22, 0).a == 0 && px(s, id, 35, 35, 1).a == 0);
    CHECK(s.run("photo.delete_selected", P({{"doc", id}})).ok && px(s, id, 22, 22, 1).a == 0);
    CHECK(s.history().undo() && px(s, id, 22, 22, 1).a == 255);

    // free transform: move the selected pixels right by 30
    r = s.run("photo.transform", P({{"doc", id}, {"move_x", 30.0}}));
    CHECK(r.ok && px(s, id, 55, 25, 1) == Color{255, 0, 0, 255} && px(s, id, 22, 22, 1).a == 0);
    CHECK(doc->selection.at(55, 25) == 255 && doc->selection.at(22, 22) == 0);  // the selection moved too
    CHECK(s.history().undo() && px(s, id, 22, 22, 1).a == 255 && doc->selection.at(22, 22) == 255);
    // whole layer, quarter turn about the canvas middle
    CHECK(s.run("photo.select", P({{"doc", id}, {"shape", std::string("none")}})).ok);
    CHECK(s.run("photo.transform", P({{"doc", id}, {"angle", 90.0}, {"pivot_x", 50.0}, {"pivot_y", 40.0}, {"smooth", false}})).ok);
    // (22,22) turns clockwise about (50,40) to (68,12)
    CHECK(px(s, id, 67, 13, 1).r == 255 && px(s, id, 22, 22, 1).a == 0);
    CHECK(s.history().undo());
    // perspective onto the same corners changes nothing
    CHECK(s.run("photo.transform", P({{"doc", id}, {"corners", NumberList{20, 20, 30, 20, 30, 30, 20, 30}}})).ok);
    CHECK(px(s, id, 25, 25, 1) == Color{255, 0, 0, 255});
    CHECK(!s.run("photo.transform", P({{"doc", id}, {"corners", NumberList{1, 2, 3}}})).ok);
    // from + corners: the square's corners go 10 px right
    CHECK(s.run("photo.transform", P({{"doc", id}, {"from", NumberList{20, 20, 30, 20, 30, 30, 20, 30}},
                                      {"corners", NumberList{30, 20, 40, 20, 40, 30, 30, 30}}})).ok);
    CHECK(px(s, id, 35, 25, 1) == Color{255, 0, 0, 255} && px(s, id, 22, 25, 1).a == 0);
    CHECK(s.history().undo());
    CHECK(!s.run("photo.transform", P({{"doc", id}, {"scale", 0.0}})).ok);
    // the box the window's free-transform handles sit on: the layer's visible pixels (no selection now)
    {
        const std::size_t steps = s.history().labels().size();
        Result box = s.run("photo.transform_box", P({{"doc", id}}));
        CHECK(box.ok && box.field("x0") && std::get<std::int64_t>(*box.field("x0")) == 20 && std::get<std::int64_t>(*box.field("x1")) == 30);
        CHECK(std::get<std::int64_t>(*box.field("y0")) == 20 && std::get<std::int64_t>(*box.field("y1")) == 30);
        CHECK(s.history().labels().size() == steps);  // not an undo step
    }

    // layer masks and clipping
    CHECK(s.run("photo.select", P({{"doc", id}, {"shape", std::string("rect")}, {"x", 0.0}, {"y", 0.0}, {"width", 25.0}, {"height", 80.0}})).ok);
    r = s.run("photo.mask", P({{"doc", id}, {"action", std::string("add")}, {"layer", std::int64_t{1}}}));
    CHECK(r.ok && !doc->hasSelection() && !doc->image.layers[1].mask.empty());
    CHECK(px(s, id, 22, 22) == Color{255, 0, 0, 255} && px(s, id, 27, 27).a == 0);  // right part hidden (the cut left a hole below)
    CHECK(px(s, id, 27, 27, 1).r == 255);                                              // but not erased
    CHECK(s.run("photo.mask", P({{"doc", id}, {"action", std::string("disable")}, {"layer", std::int64_t{1}}})).ok);
    CHECK(px(s, id, 27, 27).r == 255);
    CHECK(s.history().undo() && px(s, id, 27, 27).a == 0);
    CHECK(s.run("photo.mask", P({{"doc", id}, {"action", std::string("invert")}, {"layer", std::int64_t{1}}})).ok);
    CHECK(px(s, id, 27, 27).r == 255 && px(s, id, 22, 22).a == 0);
    CHECK(s.run("photo.mask", P({{"doc", id}, {"action", std::string("apply")}, {"layer", std::int64_t{1}}})).ok);
    CHECK(doc->image.layers[1].mask.empty() && px(s, id, 22, 22, 1).a == 0);
    CHECK(s.history().undo() && !doc->image.layers[1].mask.empty());
    CHECK(!s.run("photo.mask", P({{"doc", id}, {"action", std::string("invert")}, {"layer", std::int64_t{0}}})).ok);  // no mask there

    // a third layer filled blue, clipped to layer 1: shows only over the red square
    CHECK(s.run("photo.select", P({{"doc", id}, {"shape", std::string("all")}})).ok);
    CHECK(s.run("photo.copy_to_layer", P({{"doc", id}, {"layer", std::int64_t{0}}})).ok);  // layer 1 = copy of background
    CHECK(s.run("photo.select", P({{"doc", id}, {"shape", std::string("none")}})).ok);
    CHECK(s.run("photo.fill_selection", P({{"doc", id}, {"color", std::string("#0000ff")}, {"layer", std::int64_t{1}}})).ok);
    // stack now: 0 background (gray, with a hole), 1 blue, 2 red square (masked)
    CHECK(s.run("photo.clip", P({{"doc", id}, {"layer", std::int64_t{1}}})).ok);
    CHECK(px(s, id, 50, 50) == Color{0, 0, 255, 255});      // clipped to the gray background: shows
    CHECK(px(s, id, 22, 22).b == 0);                        // over the hole only the red square
    CHECK(!s.run("photo.clip", P({{"doc", id}, {"layer", std::int64_t{0}}})).ok);

    // masks and clipping survive .ora
    CHECK(s.run("photo.save", P({{"doc", id}, {"path", (dir / "masked.ora").string()}})).ok);
    const std::string again = docOf(s.run("photo.open", P({{"path", (dir / "masked.ora").string()}})));
    auto doc2 = s.workspace().getAs<raster::ImageDoc>(again);
    CHECK(doc2 && doc2->image.layers.size() == 3 && doc2->image.layers[1].clip && !doc2->image.layers[2].mask.empty());
    CHECK(doc2 && doc2->image.layers[2].mask == doc->image.layers[2].mask);
    CHECK(doc2 && doc2->flatten().at(50, 50) == doc->flatten().at(50, 50) && doc2->flatten().at(22, 22) == doc->flatten().at(22, 22));

    // curves and colour balance
    const std::string g = docOf(s.run("photo.new", P({{"width", std::int64_t{8}}, {"height", std::int64_t{8}}, {"background", std::string("#406080")}})));
    CHECK(s.run("photo.curves", P({{"doc", g}, {"preset", std::string("invert")}})).ok);
    CHECK(px(s, g, 1, 1) == Color{191, 159, 127, 255});
    CHECK(s.run("photo.curves", P({{"doc", g}, {"points", NumberList{0, 0, 255, 128}}, {"channel", std::string("red")}})).ok);
    CHECK(px(s, g, 1, 1).r == 96 && px(s, g, 1, 1).g == 159);
    CHECK(!s.run("photo.curves", P({{"doc", g}, {"points", NumberList{1, 2, 3}}})).ok);
    CHECK(!s.run("photo.curves", P({{"doc", g}})).ok);
    const Color before = px(s, g, 1, 1);
    CHECK(s.run("photo.color_balance", P({{"doc", g}, {"midtones", NumberList{40, 0, -40}}, {"preserve_luminosity", false}})).ok);
    CHECK(px(s, g, 1, 1).r > before.r && px(s, g, 1, 1).b < before.b);
    CHECK(!s.run("photo.color_balance", P({{"doc", g}, {"midtones", NumberList{1, 2}}})).ok);

    // monotone curve: never overshoots between points
    auto lut = raster::curveTable({{0, 0}, {100, 200}, {110, 205}, {255, 255}});
    bool monotone = true;
    for (int i = 1; i < 256; ++i) monotone = monotone && lut[static_cast<std::size_t>(i)] >= lut[static_cast<std::size_t>(i - 1)];
    CHECK(monotone && lut[0] == 0 && lut[100] == 200 && lut[255] == 255);

    // any-angle rotation
    const std::string rot = docOf(s.run("photo.new", P({{"width", std::int64_t{100}}, {"height", std::int64_t{50}}})));
    r = s.run("photo.rotate", P({{"doc", rot}, {"angle", 30.0}}));
    CHECK(r.ok && intField(r, "width") == 112 && intField(r, "height") == 93);  // 100cos30+50sin30, 100sin30+50cos30
    CHECK(px(s, rot, 1, 1).a == 0);  // empty corner stays clear
    CHECK(s.history().undo());
    r = s.run("photo.rotate", P({{"doc", rot}, {"angle", -3.0}, {"fit", std::string("crop")}}));
    CHECK(r.ok && intField(r, "width") < 100 && intField(r, "height") < 50 && intField(r, "width") > 90);
    CHECK(px(s, rot, 0, 0).a == 255);  // straighten leaves no empty corners
    CHECK(s.history().undo());
    r = s.run("photo.rotate", P({{"doc", rot}, {"angle", 10.0}, {"fit", std::string("keep")}, {"background", std::string("#00ff00")}}));
    CHECK(r.ok && intField(r, "width") == 100 && px(s, rot, 0, 0) == Color{0, 255, 0, 255});

    // crop to the selection
    CHECK(s.run("photo.select", P({{"doc", rot}, {"shape", std::string("ellipse")}, {"x", 10.0}, {"y", 5.0}, {"width", 40.0}, {"height", 30.0}})).ok);
    r = s.run("photo.crop_selection", P({{"doc", rot}}));
    CHECK(r.ok && intField(r, "width") >= 40 && intField(r, "width") <= 42 && intField(r, "height") >= 30 && intField(r, "height") <= 32);
    auto rdoc = s.workspace().getAs<raster::ImageDoc>(rot);
    CHECK(rdoc->selection.width == rdoc->image.width && rdoc->selection.at(20, 15) == 255);
}

}  // namespace

int main() {
    auto dir = fs::temp_directory_path() / "wetwaresuite_test_photo";
    fs::remove_all(dir);
    fs::create_directories(dir / "in");

    raster::Image src(200, 100, {200, 100, 50, 255});
    for (int x = 0; x < 100; ++x) src.set(x, 50, {0, 0, 0, 255});
    raster::save(src, dir / "in" / "a.png");
    raster::save(src, dir / "in" / "b.jpg");

    Session s;
    photo::registerCommands(s);

    Result opened = s.run("photo.open", {{"path", (dir / "in" / "a.png").string()}});
    CHECK(opened.ok);
    std::string id = docOf(opened);
    CHECK(id == "img1");
    CHECK(intField(opened, "width") == 200);

    CHECK(!s.run("photo.open", {{"path", (dir / "nope.png").string()}}).ok);
    CHECK(!s.run("photo.open", {{"path", (dir / "x.xyz").string()}}).ok);

    Result resized = s.run("photo.resize", {{"doc", id}, {"width", std::int64_t{100}}});
    CHECK(resized.ok && intField(resized, "height") == 50);
    CHECK(s.history().undo());
    CHECK(intField(s.run("photo.info", {{"doc", id}}), "width") == 200);
    CHECK(s.history().redo());
    CHECK(intField(s.run("photo.info", {{"doc", id}}), "width") == 100);

    CHECK(s.run("photo.resize", {{"doc", id}, {"percent", 200.0}}).ok);
    CHECK(intField(s.run("photo.info", {{"doc", id}}), "width") == 200);
    CHECK(!s.run("photo.resize", {{"doc", id}}).ok);  // nothing to resize to

    Result cropped = s.run("photo.crop", {{"doc", id}, {"x", std::int64_t{10}}, {"y", std::int64_t{10}},
                                          {"width", std::int64_t{50}}, {"height", std::int64_t{40}}});
    CHECK(cropped.ok && intField(cropped, "width") == 50 && intField(cropped, "height") == 40);
    CHECK(!s.run("photo.crop", {{"doc", id}, {"x", std::int64_t{999}}, {"y", std::int64_t{0}},
                               {"width", std::int64_t{5}}, {"height", std::int64_t{5}}}).ok);

    CHECK(intField(s.run("photo.rotate", {{"doc", id}, {"angle", std::int64_t{90}}}), "width") == 40);
    CHECK(s.run("photo.rotate", {{"doc", id}, {"angle", std::int64_t{45}}}).ok);  // any angle now
    CHECK(s.history().undo());
    CHECK(s.run("photo.flip", {{"doc", id}, {"direction", std::string("horizontal")}}).ok);
    CHECK(!s.run("photo.flip", {{"doc", id}, {"direction", std::string("sideways")}}).ok);  // not in choices

    Result before = s.run("photo.pick_color", {{"doc", id}, {"x", std::int64_t{1}}, {"y", std::int64_t{1}}});
    CHECK(s.run("photo.adjust", {{"doc", id}, {"kind", std::string("brightness")}, {"amount", 30.0}}).ok);
    Result after = s.run("photo.pick_color", {{"doc", id}, {"x", std::int64_t{1}}, {"y", std::int64_t{1}}});
    CHECK(intField(after, "r") > intField(before, "r"));
    CHECK(s.run("photo.effect", {{"doc", id}, {"kind", std::string("grayscale")}}).ok);
    Result gray = s.run("photo.pick_color", {{"doc", id}, {"x", std::int64_t{1}}, {"y", std::int64_t{1}}});
    CHECK(intField(gray, "r") == intField(gray, "g"));
    CHECK(s.run("photo.levels", {{"doc", id}, {"auto", true}}).ok);
    CHECK(s.run("photo.effect", {{"doc", id}, {"kind", std::string("blur")}, {"amount", 4.0}, {"preview", true}}).attachments.size() == 1);
    CHECK(!s.run("photo.adjust", {{"doc", id}, {"kind", std::string("brightness")}, {"amount", 1.0}, {"layer", std::int64_t{5}}}).ok);

    Result preview = s.run("photo.preview", {{"doc", id}, {"max_size", std::int64_t{32}}});
    CHECK(preview.ok && preview.attachments.size() == 1 && preview.attachments[0].mime == "image/png");
    CHECK(raster::decode(preview.attachments[0].bytes).height() == 32);

    CHECK(s.run("photo.save", {{"doc", id}, {"path", (dir / "out.jpg").string()}, {"quality", std::int64_t{80}}}).ok);
    CHECK(raster::load(dir / "out.jpg").width() == 40);
    CHECK(s.run("photo.save", {{"doc", id}, {"path", (dir / "out.ora").string()}}).ok);
    CHECK(!s.run("photo.save", {{"doc", id}, {"path", (dir / "out.xyz").string()}}).ok);

    Result created = s.run("photo.new", {{"width", std::int64_t{10}}, {"height", std::int64_t{10}}, {"background", std::string("red")}});
    CHECK(created.ok && docOf(created) == "img2");
    CHECK(!s.run("photo.new", {{"width", std::int64_t{10}}, {"height", std::int64_t{10}}, {"background", std::string("blurple")}}).ok);
    CHECK(s.run("photo.close", {{"doc", std::string("img2")}}).ok);
    CHECK(!s.run("photo.info", {{"doc", std::string("img2")}}).ok);
    CHECK(s.run("photo.save", {{"doc", id}, {"path", (dir / "out16.png").string()}, {"bits", std::int64_t{16}}}).ok);
    CHECK(!s.run("photo.save", {{"doc", id}, {"path", (dir / "out12.png").string()}, {"bits", std::int64_t{12}}}).ok);
    Result deep = s.run("photo.open", {{"path", (dir / "out16.png").string()}});
    CHECK(deep.ok && intField(deep, "bits") == 16);
    Result notRaw = s.run("photo.develop_raw", {{"doc", id}, {"exposure", 1.0}});
    CHECK(!notRaw.ok && notRaw.message.find(".dng") != std::string::npos);

    Result batch = s.run("photo.batch", {{"input_dir", (dir / "in").string()}, {"output_dir", (dir / "out").string()},
                                         {"format", std::string("png")}, {"max_size", std::int64_t{50}}});
    CHECK(batch.ok && intField(batch, "processed") == 2);
    CHECK(raster::load(dir / "out" / "a.png").width() == 50);
    CHECK(raster::load(dir / "out" / "b.png").height() == 25);
    Result again = s.run("photo.batch", {{"input_dir", (dir / "in").string()}, {"output_dir", (dir / "out").string()},
                                         {"format", std::string("png")}});
    CHECK(again.ok && intField(again, "skipped") == 2 && intField(again, "processed") == 0);
    Result inPlace = s.run("photo.batch", {{"input_dir", (dir / "in").string()}, {"output_dir", (dir / "in").string()},
                                           {"format", std::string("png")}, {"max_size", std::int64_t{10}}, {"overwrite", true}});
    CHECK(raster::load(dir / "in" / "a.png").width() == 200);  // the original is never replaced
    CHECK(inPlace.ok && intField(inPlace, "processed") == 1);   // b.jpg -> b.png is fine
    CHECK(s.run("photo.save", {{"doc", id}, {"path", (dir / "new" / "deep" / "x.png").string()}}).ok);

    // an SVG opens as pixels, scaled up when small
    {
        std::ofstream(dir / "logo.svg") << "<svg xmlns='http://www.w3.org/2000/svg' width='64' height='32'>"
                                           "<rect x='0' y='0' width='32' height='32' fill='#ff0000'/></svg>";
        Result svg = s.run("photo.open", {{"path", (dir / "logo.svg").string()}});
        CHECK(svg.ok && intField(svg, "width") == 1024 && intField(svg, "height") == 512);
        const std::string sid = docOf(svg);
        CHECK(s.run("photo.save", {{"doc", sid}, {"path", (dir / "logo.png").string()}}).ok);
        CHECK(fs::exists(dir / "logo.png"));
        const raster::Image back = raster::load(dir / "logo.png");
        CHECK(back.at(100, 100).r == 255 && back.at(100, 100).g == 0);
    }

    fs::remove_all(dir);
    testSelections(dir);
    return check::summary("photo");
}
