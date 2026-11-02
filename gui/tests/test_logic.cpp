// The GUI's window-free logic, tested against made-up and real (photo, paint) commands.
#include <algorithm>
#include <cmath>

#include "check.hpp"
#include "wetwaresuite/gui/logic.hpp"
#include "wetwaresuite/paint/paint.hpp"
#include "wetwaresuite/photo/photo.hpp"
#include "wetwaresuite/raster/document.hpp"

using namespace wetwaresuite;
using namespace wetwaresuite::gui;

namespace {

CommandInfo cmd(std::string id, std::string title, std::string description, std::vector<ParamSpec> params = {}) {
    return {std::move(id), std::move(title), std::move(description), std::move(params), true, false};
}

Field* fieldNamed(Form& f, const std::string& name) {
    for (auto& x : f.fields) {
        if (x.spec.name == name) return &x;
    }
    return nullptr;
}

void testSearch() {
    std::vector<CommandInfo> cmds = {
        cmd("photo.resize", "Resize image", "Scale an image to a new size."),
        cmd("photo.crop", "Crop", "Keep a rectangle of the image."),
        cmd("paint.stroke", "Brush stroke", "Paint a brush stroke through a list of points."),
        cmd("photo.adjust", "Adjust", "Brightness, contrast, saturation and more."),
    };
    auto hits = search("resize", cmds);
    CHECK(!hits.empty() && hits[0].command->id == "photo.resize");
    // a word only in the description still finds it
    hits = search("brightness", cmds);
    CHECK(hits.size() == 1 && hits[0].command->id == "photo.adjust");
    // letters in order: "brsh" finds "Brush stroke"
    hits = search("brsh", cmds);
    CHECK(!hits.empty() && hits[0].command->id == "paint.stroke");
    // every word must match
    CHECK(search("crop brightness", cmds).empty());
    CHECK(matchScore("zzz", cmds[0]) == 0);
    // case does not matter, title beats description
    CHECK(matchScore("CROP", cmds[1]) > matchScore("rectangle", cmds[1]));
    // empty query lists everything in order
    hits = search("", cmds);
    CHECK(hits.size() == cmds.size() && hits[0].command->id == "photo.resize");
    CHECK(search("", cmds, 2).size() == 2);
    // tool prefix finds the tool's commands
    hits = search("paint", cmds);
    CHECK(!hits.empty() && hits[0].command->id == "paint.stroke");
}

void testLabelsAndRanges() {
    CHECK(labelFor("max_size") == "Max size");
    CHECK(labelFor("doc") == "Document");
    CHECK(labelFor("output_dir") == "Output dir");
    CHECK(labelFor("dpi") == "DPI");
    CHECK(labelFor("svg_path") == "SVG path");
    double lo = 0, hi = 0;
    CHECK(parseRange("edge hardness 0..100 (overrides the preset)", lo, hi) && lo == 0 && hi == 100);
    CHECK(parseRange("JPEG quality 1..100", lo, hi) && lo == 1 && hi == 100);
    CHECK(parseRange("tilt -45.5..45.5 degrees", lo, hi) && lo == -45.5 && hi == 45.5);
    CHECK(!parseRange("width in px", lo, hi));
    CHECK(!parseRange("weird 5..1", lo, hi));
}

void testKinds() {
    CHECK(kindFor({"doc", ParamType::String, "id of an open image, e.g. img1"}, "photo.crop") == FieldKind::Document);
    CHECK(kindFor({"color", ParamType::String, "paint colour (#rrggbb, #rrggbbaa, rgb(r,g,b) or a name)"}, "paint.fill") == FieldKind::Color);
    CHECK(kindFor({"path", ParamType::String, "path to the image file"}, "photo.open") == FieldKind::OpenFile);
    CHECK(kindFor({"path", ParamType::String, "file to write, e.g. out.png"}, "photo.save") == FieldKind::SaveFile);
    CHECK(kindFor({"output", ParamType::String, "GIF file to write"}, "animate.export_gif") == FieldKind::SaveFile);
    CHECK(kindFor({"input_dir", ParamType::String, "folder with the source images"}, "photo.batch") == FieldKind::Folder);
    CHECK(kindFor({"filter", ParamType::String, "resampling filter", false, {"bilinear", "nearest"}}, "photo.resize") == FieldKind::Choice);
    CHECK(kindFor({"name", ParamType::String, "layer name"}, "paint.layer_add") == FieldKind::Text);
    CHECK(kindFor({"auto", ParamType::Bool, "auto contrast"}, "photo.levels") == FieldKind::Bool);
    CHECK(kindFor({"points", ParamType::NumberList, "x,y pairs"}, "paint.stroke") == FieldKind::NumberList);
    CHECK(kindFor({"width", ParamType::Int, "width"}, "photo.new") == FieldKind::Integer);
}

void testForms() {
    CommandInfo info = cmd("demo.new", "New", "Does a thing.",
                           {{"doc", ParamType::String, "id of an open image, e.g. img1", true},
                            {"width", ParamType::Int, "width in px", true},
                            {"quality", ParamType::Int, "JPEG quality 1..100", false, {}, std::int64_t{90}},
                            {"amount", ParamType::Number, "strength 0..1", false},
                            {"mode", ParamType::String, "how", false, {"fast", "good"}, std::string("good")},
                            {"color", ParamType::String, "colour (#rrggbb)", false, {}, std::string("#ff0000")},
                            {"flip", ParamType::Bool, "mirror it", false, {}, false},
                            {"points", ParamType::NumberList, "x,y pairs", false},
                            {"preview", ParamType::Bool, "attach a PNG preview", false, {}, false}});
    Form form = buildForm(info, {"img1", "img2"}, "img2");
    CHECK(form.fields.size() == 9);
    CHECK(fieldNamed(form, "doc")->text == "img2");
    CHECK(fieldNamed(form, "width")->number == 1920);  // a sensible start for a required size
    // long command descriptions: a short lead, the rest behind "More", no "Returns ..." for scripts
    std::string lead, rest;
    splitDescription("Start a 2D sketch on a plane (e.g. xy). Add lines with Sketch Line. Then extrude it. Returns the feature name.", lead, rest);
    CHECK(lead == "Start a 2D sketch on a plane (e.g. xy). Add lines with Sketch Line.");
    CHECK(rest == "Then extrude it.");
    splitDescription("Open an image.", lead, rest);
    CHECK(lead == "Open an image." && rest.empty());
    splitDescription("Create an empty map document and return its id (e.g. map1).", lead, rest);
    CHECK(lead == "Create an empty map document.");
    // a shape on a page starts small enough to see whole, pixel art smaller still
    Form rect = buildForm(cmd("vector.rect", "Rectangle", "", {{"width", ParamType::Number, "width", true}}), {}, "");
    CHECK(fieldNamed(rect, "width")->number == 200);
    Form sprite = buildForm(cmd("forge.sprite_new", "Sprite", "", {{"width", ParamType::Int, "width", true}}), {}, "");
    CHECK(fieldNamed(sprite, "width")->number == 32);
    CHECK(fieldNamed(form, "quality")->number == 90 && fieldNamed(form, "quality")->min == 1.0);
    CHECK(!fieldNamed(form, "amount")->set);  // optional, no default: the command decides
    CHECK(fieldNamed(form, "mode")->text == "good");
    CHECK(fieldNamed(form, "color")->kind == FieldKind::Color);
    CHECK(fieldNamed(form, "preview")->hidden);

    Params p;
    std::string error;
    CHECK(toParams(form, p, error));
    CHECK(std::get<std::string>(p.at("doc")) == "img2");
    CHECK(std::get<std::int64_t>(p.at("width")) == 1920);
    CHECK(std::get<std::int64_t>(p.at("quality")) == 90);
    CHECK(!p.count("amount") && !p.count("points") && !p.count("preview"));
    CHECK(std::get<bool>(p.at("flip")) == false);

    CHECK(setFieldText(*fieldNamed(form, "amount"), "0.5"));
    CHECK(setFieldText(*fieldNamed(form, "flip"), "yes"));
    CHECK(!setFieldText(*fieldNamed(form, "width"), "wide"));
    CHECK(!setFieldText(*fieldNamed(form, "mode"), "slow"));
    CHECK(setFieldText(*fieldNamed(form, "points"), "[10, 20; 30 40]"));
    CHECK(toParams(form, p, error));
    CHECK(std::get<double>(p.at("amount")) == 0.5);
    CHECK(std::get<bool>(p.at("flip")));
    CHECK((std::get<NumberList>(p.at("points")) == NumberList{10, 20, 30, 40}));

    fieldNamed(form, "points")->text = "10, twenty";
    CHECK(!toParams(form, p, error) && error.find("Points") != std::string::npos);
    fieldNamed(form, "points")->text.clear();
    fieldNamed(form, "doc")->text.clear();
    CHECK(!toParams(form, p, error) && error == "Please fill in \"Document\".");

    // no documents open: the document field starts empty
    Form empty = buildForm(info);
    CHECK(fieldNamed(empty, "doc")->text.empty() && !fieldNamed(empty, "doc")->set);
}

void testRealCommands() {
    Session session;
    photo::registerCommands(session);
    paint::registerCommands(session);
    const auto all = session.registry().list();

    auto photoPicks = beginnerCommands("photo", all);
    CHECK(photoPicks.size() == 8);
    CHECK(photoPicks[0] == "photo.open" && photoPicks[1] == "photo.new" && photoPicks[2] == "photo.save");
    CHECK(std::find(photoPicks.begin(), photoPicks.end(), "photo.resize") != photoPicks.end());
    auto paintPicks = beginnerCommands("paint", all);
    CHECK(!paintPicks.empty() && paintPicks.size() <= 8);
    CHECK(std::find(paintPicks.begin(), paintPicks.end(), "paint.stroke") != paintPicks.end());
    for (const auto& id : paintPicks) CHECK(id.rfind("paint.", 0) == 0);
    CHECK(toolCommands("paint", all).size() > paintPicks.size());

    // a form built from a real command runs through Session::run
    const CommandInfo* newInfo = session.registry().find("photo.new");
    Form f = buildForm(*newInfo);
    setFieldText(*fieldNamed(f, "width"), "64");
    setFieldText(*fieldNamed(f, "height"), "32");
    Params p;
    std::string error;
    CHECK(toParams(f, p, error));
    Result r = session.run("photo.new", p);
    CHECK(r.ok);
    const std::string doc = r.field("doc") ? toText(*r.field("doc")) : "";
    CHECK(doc == "img1");

    // the stroke form picks the open image and passes colour as hex
    Form stroke = buildForm(*session.registry().find("paint.stroke"), session.workspace().ids());
    CHECK(fieldNamed(stroke, "doc")->text == "img1");
    CHECK(fieldNamed(stroke, "color")->kind == FieldKind::Color);
    CHECK(fieldNamed(stroke, "hardness")->max == 100.0);
    setFieldText(*fieldNamed(stroke, "points"), "2,2, 30,20");
    CHECK(toParams(stroke, p, error));
    CHECK(session.run("paint.stroke", p).ok);
    CHECK(session.history().canUndo());

    // errors from the core read as plain words
    Result bad = session.run("photo.new", {{"width", std::int64_t{10}}});
    CHECK(!bad.ok && friendlyError(bad.message) == "Please fill in \"Height\".");

    // opening a .png tries photo first
    auto cands = openCandidates("/tmp/cat.PNG", {"paint", "photo"}, all);
    CHECK(!cands.empty() && cands[0] == "photo.open");
    CHECK(openCandidates("x.png", {"paint"}, all).empty());
    // a tool's other openers count too, and the one naming the extension goes first
    std::vector<CommandInfo> office = {
        {"office.doc_open", "Open Document", "Open a .docx or .md file", {ParamSpec{"path"}}, false},
        {"office.sheet_open", "Open Spreadsheet", "Open .xlsx or .csv", {ParamSpec{"path"}}, false},
        {"office.sheet_close", "Close", "close", {}, false},
        {"office.reopen", "Reopen", "no path here", {}, false}};
    cands = openCandidates("/x/budget.csv", {"office"}, office);
    CHECK(cands.size() == 2 && cands[0] == "office.sheet_open" && cands[1] == "office.doc_open");
    // "<tool>.<thing>_open" commands that name the extension are tried first
    {
        std::vector<CommandInfo> game = all;
        game.push_back({"forge.sprite_open", "Open sprite", "Open an Aseprite .aseprite/.ase file.",
                        {{"path", ParamType::String, "file", false}}, false, true});
        game.push_back({"forge.other_open", "Open other", "Open an aseprite thing.",
                        {{"path", ParamType::String, "file", true}, {"size", ParamType::Int, "px", true}}, false, true});
        auto sprite = openCandidates("hero.aseprite", {"photo", "forge"}, game);
        CHECK(!sprite.empty() && sprite[0] == "forge.sprite_open");
        CHECK(std::find(sprite.begin(), sprite.end(), "forge.other_open") == sprite.end());
        CHECK(openCandidates("hero.ase", {"forge"}, game).size() == 1);
    }
}

void testColoursRecentMessages() {
    auto c = parseHex("#ff8000");
    CHECK(c && c->r == 1.0f && c->b == 0.0f && c->a == 1.0f);
    CHECK(parseHex("#f80") && toHex(*parseHex("#f80")) == "#ff8800");
    CHECK(toHex(*parseHex("#11223344")) == "#11223344");
    CHECK(parseHex("transparent")->a == 0.0f);
    CHECK(!parseHex("#12345") && !parseHex("#gg0000") && !parseHex("reddish"));
    CHECK(toHex(*parseHex("White")) == "#ffffff" && toHex(*parseHex("red")) == "#ff0000");  // the swatch shows a named colour
    CHECK(toHex(*parseHex("rgb(0, 128, 255)")) == "#0080ff");
    CHECK(toHex(fromRgb24(0x3d7eff)) == "#3d7eff");

    std::vector<std::string> recent = {"a", "b", "c"};
    recent = pushRecent(recent, "b");
    CHECK((recent == std::vector<std::string>{"b", "a", "c"}));
    recent = pushRecent(recent, "d", 3);
    CHECK((recent == std::vector<std::string>{"d", "b", "a"}));
    CHECK((parseRecent("x\r\ny\n\nx\n") == std::vector<std::string>{"x", "y"}));
    CHECK(parseRecent(formatRecent(recent)) == recent);

    CHECK(friendlyError("missing parameter 'max_size' for photo.batch") == "Please fill in \"Max size\".");
    CHECK(friendlyError("parameter 'filter' for photo.resize must be one of: bilinear, nearest") ==
          "\"Filter\" has to be one of: bilinear, nearest.");
    CHECK(friendlyError("parameter 'width' for photo.new must be integer") == "\"Width\" needs a whole number.");
    CHECK(friendlyError("point is outside the image") == "Point is outside the image.");
    CHECK(sentence("grayscale applied") == "Grayscale applied." && sentence("Done!") == "Done!");
    Result r = Result::success("ok");
    r.with("doc", std::string("img1")).with("max_size", std::int64_t{5});
    CHECK((resultLines(r) == std::vector<std::string>{"Document: img1", "Max size: 5"}));
    CHECK(toolTitle("forge") == "Asset forge" && toolTitle("photo") == "Photo");
    CHECK(!toolBlurb("paint").empty() && !toolBlurb("unknown").empty());
}

const NumberList* listOf(const Params& p, const std::string& key) {
    auto it = p.find(key);
    return it == p.end() ? nullptr : std::get_if<NumberList>(&it->second);
}

void testThinStroke() {
    CHECK(thinStroke({}, 1).empty());
    CHECK((thinStroke({5, 5}, 1) == std::vector<double>{5, 5}));
    // 1000 points 0.1 px apart: about one per px is kept, ends included
    std::vector<double> line;
    for (int i = 0; i < 1000; ++i) line.insert(line.end(), {i * 0.1, 20.0});
    auto thin = thinStroke(line, 1);
    CHECK(thin.size() / 2 >= 95 && thin.size() / 2 <= 102);
    CHECK(thin[0] == 0 && thin[1] == 20 && std::abs(thin[thin.size() - 2] - 99.9) < 1e-9);
    // a cap on the count keeps evenly spaced points
    thin = thinStroke(line, 1, 10);
    CHECK(thin.size() == 20);
    CHECK(thin[0] == 0 && std::abs(thin[18] - 99.9) < 1e-9);
    // repeated points collapse to one
    CHECK(thinStroke({3, 3, 3, 3, 3, 3}, 1).size() == 2);
}

void testHealSource() {
    HealSource src;
    double ox = 0, oy = 0;
    CHECK(!src.offsetFor(1, 1, ox, oy));
    src.pick(100, 50);
    // the first stroke fixes the offset from where it starts
    CHECK(src.offsetFor(40, 50, ox, oy) && ox == 60 && oy == 0);
    // later strokes keep it, like an aligned clone brush
    CHECK(src.offsetFor(10, 10, ox, oy) && ox == 60 && oy == 0);
    src.pick(0, 0);
    CHECK(src.offsetFor(10, 10, ox, oy) && ox == -10 && oy == -10);
    src.forget();
    CHECK(!src.picked && !src.offsetFor(1, 1, ox, oy));
}

void testRetouchParams() {
    Params p;
    std::string error;
    // smooth heal: a stroke with half the brush size as radius, no source
    CHECK(healStrokeParams("img1", {10, 10, 20, 10}, 10, 100, 100, nullptr, p, error));
    CHECK(toText(p.at("shape")) == "stroke" && toText(p.at("doc")) == "img1");
    CHECK(std::get<double>(p.at("radius")) == 5.0);
    CHECK(!p.count("from_x") && listOf(p, "points") && listOf(p, "points")->size() == 4);
    // tiny brushes still send a usable radius
    CHECK(healStrokeParams("img1", {10, 10}, 0.5, 100, 100, nullptr, p, error) && std::get<double>(p.at("radius")) == 1.0);
    // with a source: from_x/from_y are the middle of the area plus the offset
    const double offset[2] = {40, -5};
    CHECK(healStrokeParams("img1", {10, 10, 20, 10}, 10, 100, 100, offset, p, error));
    // area spans x 5..24, y 5..14: middle (14, 9)
    CHECK(std::get<std::int64_t>(p.at("from_x")) == 54 && std::get<std::int64_t>(p.at("from_y")) == 4);
    // a source that runs off the picture is refused in plain words
    const double far[2] = {90, 0};
    CHECK(!healStrokeParams("img1", {10, 10, 20, 10}, 10, 100, 100, far, p, error));
    CHECK(error.find("edge") != std::string::npos);
    // nothing painted, or off the picture
    CHECK(!removeStrokeParams("img1", {}, 10, 100, 100, p, error) && !error.empty());
    CHECK(!removeStrokeParams("img1", {500, 500, 600, 600}, 10, 100, 100, p, error));
    CHECK(error.find("outside") != std::string::npos);
    // long strokes are thinned to a sane count
    std::vector<double> scribble;
    for (int i = 0; i < 5000; ++i) scribble.insert(scribble.end(), {50 + 40 * std::cos(i * 0.01), 50 + 40 * std::sin(i * 0.013)});
    CHECK(removeStrokeParams("img1", scribble, 6, 100, 100, p, error));
    CHECK(listOf(p, "points")->size() / 2 <= 400);
}

// The params the canvas sends really run, change the picture and undo in one step.
void testBrushTools() {
    Session session;
    photo::registerCommands(session);
    paint::registerCommands(session);
    CHECK(session.run("photo.new", {{"width", std::int64_t{60}}, {"height", std::int64_t{40}}, {"background", std::string("#808080")}}).ok);
    auto doc = session.workspace().getAs<raster::ImageDoc>("img1");
    CHECK(doc != nullptr);
    if (!doc) return;
    BrushToolSettings set;
    set.size = 8;
    std::string command, error;
    Params p;
    // clone without a source says what to do
    CHECK(!brushToolParams("clone", "img1", {10, 10, 20, 10}, {}, set, nullptr, command, p, error) && error.find("Alt+click") != std::string::npos);
    CHECK(!brushToolParams("dodge", "img1", {10}, {}, set, nullptr, command, p, error));
    // every tool's params run as they are
    HealSource src;
    src.pick(40, 30);
    double off[2];
    CHECK(src.offsetFor(10, 10, off[0], off[1]));
    for (const char* tool : {"clone", "dodge", "burn", "sponge", "smudge", "blur", "sharpen"}) {
        CHECK(brushToolParams(tool, "img1", {10, 10, 20, 10}, {1, 0.5}, set, off, command, p, error));
        Result r = session.run(command, p);
        CHECK(r.ok);
    }
    // dodge brightens, burn darkens where they paint
    set.strength = 80;
    CHECK(brushToolParams("dodge", "img1", {5, 30, 15, 30}, {}, set, nullptr, command, p, error) && session.run(command, p).ok);
    CHECK(doc->image.layers[0].image.at(10, 30).r > 0x80);
    CHECK(brushToolParams("burn", "img1", {45, 30, 55, 30}, {}, set, nullptr, command, p, error) && session.run(command, p).ok);
    CHECK(doc->image.layers[0].image.at(50, 30).r < 0x80);
    CHECK(std::get<std::string>(p.at("range")) == "midtones" && command == "paint.dodge");
}

void testRetouchCommands() {
    Session session;
    photo::registerCommands(session);
    paint::registerCommands(session);
    CHECK(session.run("photo.new", {{"width", std::int64_t{80}}, {"height", std::int64_t{60}}}).ok);
    // a dark blemish
    CHECK(session.run("paint.stroke", {{"doc", std::string("img1")}, {"points", NumberList{30, 30, 34, 30}}, {"size", 6.0},
                                        {"color", std::string("#000000")}})
              .ok);
    auto doc = session.workspace().getAs<raster::ImageDoc>("img1");
    CHECK(doc != nullptr);
    if (!doc) return;
    doc->takeChanges();
    auto red = [&] { return static_cast<int>(doc->image.layers[0].image.pixel(32, 30)[0]); };
    CHECK(red() < 60);
    const auto steps = session.history().labels().size();

    Params p;
    std::string error;
    CHECK(healStrokeParams("img1", {29, 30, 35, 30}, 12, 80, 60, nullptr, p, error));
    Result r = session.run("photo.heal", p);
    CHECK(r.ok);
    CHECK(red() > 200);
    CHECK(session.history().labels().size() == steps + 1);
    const auto changes = doc->takeChanges();
    CHECK(changes.all || !changes.rect.empty());
    CHECK(session.history().undo() && red() < 60);

    // healing with a source copies from the clean spot to the right
    HealSource src;
    src.pick(60, 30);
    double off[2] = {0, 0};
    CHECK(src.offsetFor(29, 30, off[0], off[1]));
    CHECK(healStrokeParams("img1", {29, 30, 35, 30}, 12, 80, 60, off, p, error));
    CHECK(session.run("photo.heal", p).ok && red() > 200);
    CHECK(session.history().undo());

    // remove object
    CHECK(removeStrokeParams("img1", {29, 30, 35, 30}, 12, 80, 60, p, error));
    CHECK(session.run("photo.remove_object", p).ok && red() > 200);
    CHECK(session.history().labels().size() == steps + 1);
    CHECK(session.history().undo() && red() < 60);

    // too big an area says so in plain words
    CHECK(session.run("photo.new", {{"width", std::int64_t{2400}}, {"height", std::int64_t{2400}}}).ok);
    CHECK(removeStrokeParams("img2", {200, 1200, 2200, 1200}, 2300, 2400, 2400, p, error));
    Result big = session.run("photo.remove_object", p);
    CHECK(!big.ok && friendlyError(big.message).find("limit") != std::string::npos);
}

}  // namespace

int main() {
    testSearch();
    testLabelsAndRanges();
    testKinds();
    testForms();
    testRealCommands();
    testColoursRecentMessages();
    testThinStroke();
    testHealSource();
    testRetouchParams();
    testRetouchCommands();
    testBrushTools();
    return check::summary("gui_logic");
}
