#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "wetwaresuite/core/cli.hpp"
#include "wetwaresuite/layout/layout.hpp"
#include "wetwaresuite/mcp/server.hpp"
#include "wetwaresuite/pdfdoc/fonts.hpp"
#include "wetwaresuite/raster/codec.hpp"
#include "check.hpp"

using namespace wetwaresuite;
using namespace wetwaresuite::layout;

namespace {

bool near(double a, double b, double tol = 1e-6) { return std::fabs(a - b) <= tol; }

double num(const Result& r, const std::string& key) {
    const Value* v = r.field(key);
    if (!v) return std::nan("");
    if (auto d = std::get_if<double>(v)) return *d;
    if (auto i = std::get_if<std::int64_t>(v)) return static_cast<double>(*i);
    if (auto b = std::get_if<bool>(v)) return *b ? 1 : 0;
    return std::nan("");
}

Result run(Session& s, const std::string& line) {
    cli::Batch batch(s);
    Result r = batch.runLine(line);
    if (!r.ok) std::printf("  (%s -> %s)\n", line.c_str(), r.message.c_str());
    return r;
}

std::string slurp(const std::filesystem::path& p) {
    std::ifstream in(p, std::ios::binary);
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

std::shared_ptr<LayoutDoc> docOf(Session& s, const std::string& id = "lay1") { return s.workspace().getAs<LayoutDoc>(id); }

std::filesystem::path tempDir() {
    auto dir = std::filesystem::temp_directory_path() / "wetwaresuite_layout_test";
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);
    return dir;
}

std::string q(const std::filesystem::path& p) { return "\"" + p.generic_string() + "\""; }

void testMarkup() {
    auto paras = parseText("Hello **bold** and *it* here\\nSecond line", false);
    CHECK(paras.size() == 2);
    CHECK(paras[0].runs.size() == 5);
    CHECK(paras[0].runs[1].text == "bold" && paras[0].runs[1].props.bold.value_or(false));
    CHECK(paras[0].runs[3].text == "it" && paras[0].runs[3].props.italic.value_or(false));
    CHECK(paras[1].text() == "Second line");
    CHECK(parseText("5 * 3 = 15", false)[0].text() == "5 * 3 = 15");  // a lone asterisk stays
    auto md = parseText("# Title\n\nSome text\nwrapped here.\n\n- one\n- two\n\n> quote\n\n```\ncode line\n```\n\n[a link](http://x)", true);
    CHECK(md.size() == 7);
    CHECK(md[0].style == "h1" && md[0].text() == "Title");
    CHECK(md[1].style == "body" && md[1].text() == "Some text wrapped here.");
    CHECK(md[2].style == "bullet" && md[2].text() == "\xE2\x80\xA2 one");
    CHECK(md[4].style == "quote");
    CHECK(md[5].style == "code" && md[5].text() == "code line");
    CHECK(md[6].text() == "a link");
}

void testModel() {
    CHECK(findPaper("a4")->width == 210);
    CHECK(findPaper("card")->height == 55);
    auto book = newLayout("book_6x9");
    CHECK(book->facing && book->masters.size() == 1 && book->pages[0].master == "A");
    book->pages.push_back(book->pages[0]);
    const Box right = marginBox(*book, 0), left = marginBox(*book, 1);
    CHECK(near(right.x, book->margins.left) && near(left.x, book->margins.right));  // inside margin swaps sides
    CHECK(near(right.w, left.w));
    auto poster = newLayout("poster_a2");
    CHECK(poster->width == 420 && poster->height == 594);
    bool threw = false;
    try {
        newLayout("nonsense");
    } catch (const std::exception&) {
        threw = true;
    }
    CHECK(threw);
    // colours
    auto p = resolvePaint(*poster, "cmyk(0,100,100,0)");
    CHECK(p && p->cmyk && near(p->m, 1) && p->rgb.r == 255 && p->rgb.g == 0);
    CHECK(resolvePaint(*poster, "black")->cmyk);
    CHECK(resolvePaint(*poster, "#336699") && !resolvePaint(*poster, "#336699")->cmyk);
    CHECK(resolvePaint(*poster, "none")->none);
    CHECK(!resolvePaint(*poster, "not-a-colour"));
    // JSON round trip
    poster->title = "Test";
    poster->swatches.push_back({"brand", true, 0, 60, 100, 0, cmykToRgb(0, 0.6, 1, 0)});
    Frame f;
    f.id = "f9";
    f.type = FrameType::Text;
    f.x = 10;
    f.y = 20;
    f.w = 100;
    f.h = 50;
    f.rotation = 15;
    f.story = "t9";
    f.columns = 2;
    poster->pages[0].frames.push_back(f);
    poster->stories["t9"].paragraphs = parseText("Hi **there**", false);
    auto back = fromJson(toJson(*poster));
    CHECK(back->title == "Test" && back->pages.size() == 1 && back->pages[0].frames.size() == 1);
    CHECK(back->pages[0].frames[0].rotation == 15 && back->pages[0].frames[0].columns == 2);
    CHECK(back->stories["t9"].paragraphs[0].runs[1].props.bold.value_or(false));
    CHECK(back->findSwatch("brand") && back->findSwatch("brand")->cmyk);
    CHECK(toJson(*back) == toJson(*poster));
}

void testTextFlow() {
    auto doc = newLayout("");
    auto addFrame = [&](const std::string& id, double x, double y, double w, double h, const std::string& story) {
        Frame f;
        f.id = id;
        f.type = FrameType::Text;
        f.x = x;
        f.y = y;
        f.w = w;
        f.h = h;
        f.story = story;
        doc->pages[0].frames.push_back(f);
    };
    std::string longText;
    for (int i = 0; i < 60; ++i) longText += "The quick brown fox jumps over the lazy dog. ";
    doc->stories["t1"].paragraphs = parseText(longText, false);
    addFrame("a", 10, 10, 60, 30, "t1");
    auto chain = storyChain(*doc, "t1");
    StoryLayout sl = layoutStory(*doc, doc->stories["t1"], chain);
    CHECK(sl.overset > 0);
    const std::size_t firstOverset = sl.overset;
    // body is 10/13 pt: a 30 mm frame holds 6 lines
    CHECK(sl.frames["a"].lines.size() == 6);
    for (const auto& line : sl.frames["a"].lines) {
        double w = 0;
        for (const auto& r : line.runs) w += r.width;
        CHECK(w <= 60 + 1e-6);
    }
    // link a second, bigger frame: less (or nothing) overset
    addFrame("b", 10, 50, 180, 200, "t1");
    doc->pages[0].frames[0].next = "b";
    chain = storyChain(*doc, "t1");
    CHECK(chain.size() == 2 && chain[0]->id == "a");
    sl = layoutStory(*doc, doc->stories["t1"], chain);
    CHECK(sl.overset == 0 && sl.overset < firstOverset);
    CHECK(sl.frames["a"].chars + sl.frames["b"].chars == sl.total);
    // columns: two columns hold twice the lines of one
    Frame& b = doc->pages[0].frames[1];
    b.columns = 2;
    b.gutter = 10;
    sl = layoutStory(*doc, doc->stories["t1"], storyChain(*doc, "t1"));
    bool rightColumn = false;
    for (const auto& line : sl.frames["b"].lines) rightColumn = rightColumn || line.left > 90;
    CHECK(rightColumn || sl.frames["b"].lines.size() < 30);
    // justify stretches inner lines to the full width; right alignment pushes text over
    doc->stories["t2"].paragraphs = parseText(longText.substr(0, 400), false);
    doc->stories["t2"].paragraphs[0].props.align = Align::Justify;
    addFrame("c", 0, 0, 80, 200, "t2");
    sl = layoutStory(*doc, doc->stories["t2"], storyChain(*doc, "t2"));
    const auto& lines = sl.frames["c"].lines;
    CHECK(lines.size() > 2);
    double w0 = 0;
    for (const auto& r : lines[0].runs) w0 += r.width;
    CHECK(near(w0, 80, 1e-6));
    CHECK(lines[0].runs[0].wordSpace > 0);
    CHECK(lines.back().runs[0].wordSpace == 0);
    doc->stories["t2"].paragraphs[0].props.align = Align::Right;
    doc->stories["t2"].paragraphs[0].runs = {{"short", {}, {}}};
    sl = layoutStory(*doc, doc->stories["t2"], storyChain(*doc, "t2"));
    CHECK(sl.frames["c"].lines.size() == 1 && sl.frames["c"].lines[0].runs[0].x > 60);
    // bigger type, fewer lines; text width follows the font metrics
    CHECK(textWidth(*doc, "WWW", "helvetica", 10) > textWidth(*doc, "iii", "helvetica", 10));
    CHECK(near(textWidth(*doc, "M", "courier", 72), 0.6 * 25.4, 1e-9));
    CHECK(textWidth(*doc, "Hello", "times", 12, true) > textWidth(*doc, "Hello", "times", 12));
}

void testCommands(const std::filesystem::path& dir) {
    Session s;
    registerCommands(s);
    Result r = run(s, "layout.new size=A5 margins=10 title=Flyer");
    CHECK(r.ok && toText(r.value) == "lay1");
    auto doc = docOf(s);
    CHECK(doc && doc->width == 148 && doc->margins.top == 10);
    CHECK(!run(s, "layout.new size=B9").ok);
    CHECK(run(s, "layout.new preset=card orientation=portrait").ok && docOf(s, "lay2")->width == 55);

    // frames and undo
    r = run(s, "layout.frame_add doc=lay1 type=text text=\"Hello **world**\" size=24 align=center");
    CHECK(r.ok && toText(r.value) == "f1" && num(r, "overset") == 0);
    CHECK(doc->pages[0].frames.size() == 1 && near(doc->pages[0].frames[0].w, 128));  // the margin box
    CHECK(run(s, "layout.frame_set doc=lay1 frame=f1 x=20 y=30 w=50 h=20 rotation=10 fill=yellow").ok);
    CHECK(near(doc->pages[0].frames[0].x, 20) && doc->pages[0].frames[0].fill == "yellow");
    s.history().undo();
    CHECK(near(doc->pages[0].frames[0].x, 10));
    s.history().redo();
    CHECK(near(doc->pages[0].frames[0].x, 20));
    CHECK(!run(s, "layout.frame_set doc=lay1 frame=f1 fill=notacolour").ok);
    CHECK(!run(s, "layout.frame_set doc=lay1 frame=nope x=1").ok);
    CHECK(!run(s, "layout.frame_add doc=lay1 type=rect page=9").ok);
    CHECK(run(s, "layout.frame_add doc=lay1 type=rect x=0 y=0 w=20 h=10 fill=cmyk(0,100,0,0) name=box").ok);
    CHECK(doc->findFrame("box") && doc->findFrame("box")->id == "f2");
    // the window's view: frames with boxes, and retyping a frame keeps its look
    r = run(s, "layout.frames doc=lay1 page=1");
    CHECK(r.ok && toText(*r.field("ids")) == "f1,f2" && toText(*r.field("types")) == "text,rect");
    CHECK(std::get<NumberList>(*r.field("boxes")).size() == 8 && near(std::get<NumberList>(*r.field("boxes"))[0], 20));
    r = run(s, "layout.info doc=lay1 frame=f1");
    CHECK(r.ok && toText(*r.field("markup")) == "Hello **world**");
    CHECK(run(s, "layout.frame_text doc=lay1 frame=f1 text=\"Goodbye *moon*\nsecond line\"").ok);
    {
        const Story& st = doc->stories[doc->findFrame("f1")->story];
        CHECK(st.paragraphs.size() == 2 && st.paragraphs[0].text() == "Goodbye moon" && st.paragraphs[1].text() == "second line");
        CHECK(st.paragraphs[0].runs.back().props.italic.value_or(false) && st.paragraphs[0].runs.front().props.size.value_or(0) == 24);
        CHECK(st.paragraphs[1].runs.front().props.size.value_or(0) == 24);
    }
    s.history().undo();
    CHECK(doc->stories[doc->findFrame("f1")->story].paragraphs[0].text() == "Hello world");
    CHECK(!run(s, "layout.frame_text doc=lay1 frame=f2 text=x").ok);
    // formatting overrides reach every run
    CHECK(run(s, "layout.frame_set doc=lay1 frame=f1 font=times italic=true color=navy").ok);
    for (const auto& run : doc->stories[doc->findFrame("f1")->story].paragraphs[0].runs) {
        CHECK(run.props.font.value_or("") == "times" && run.props.italic.value_or(false));
    }
    CHECK(!run(s, "layout.frame_set doc=lay1 frame=f2 text=hi").ok);  // not a text frame

    // styles and colours
    CHECK(run(s, "layout.color_define doc=lay1 name=brand cmyk=0,60,100,0").ok);
    CHECK(doc->findSwatch("brand") && doc->findSwatch("brand")->cmyk);
    CHECK(!run(s, "layout.color_define doc=lay1 name=bad cmyk=1,2").ok);
    CHECK(run(s, "layout.style_define doc=lay1 name=title size=30 bold=true color=brand align=center").ok);
    CHECK(run(s, "layout.style_define doc=lay1 name=hot kind=character color=red bold=true").ok);
    CHECK(!run(s, "layout.style_define doc=lay1 name=hot2 kind=character leading=12").ok);
    CHECK(run(s, "layout.frame_add doc=lay1 type=text x=10 y=60 w=120 h=40 text=\"Big news today\\\\nThe news is big\"").ok);  // f3
    CHECK(doc->stories[doc->findFrame("f3")->story].paragraphs.size() == 2);
    CHECK(run(s, "layout.style_apply doc=lay1 frame=f3 style=title paragraphs=1").ok);
    CHECK(doc->stories[doc->findFrame("f3")->story].paragraphs[0].style == "title");
    r = run(s, "layout.style_apply doc=lay1 frame=f3 style=hot match=news");
    CHECK(r.ok && num(r, "count") == 2);
    CHECK(!run(s, "layout.style_apply doc=lay1 frame=f3 style=hot match=zebra").ok);

    // overset, linking, unlinking
    std::string lots;
    for (int i = 0; i < 40; ++i) lots += "Lorem ipsum dolor sit amet, consectetur adipiscing elit. ";
    r = run(s, "layout.frame_add doc=lay1 type=text x=10 y=110 w=60 h=20 text=\"" + lots + "\"");
    CHECK(r.ok && num(r, "overset") > 0);  // f4
    CHECK(run(s, "layout.page_add doc=lay1").ok && doc->pages.size() == 2);
    CHECK(run(s, "layout.frame_add doc=lay1 type=text page=2").ok);  // f5
    r = run(s, "layout.frame_link doc=lay1 from=f4 to=f5");
    CHECK(r.ok && num(r, "overset") == 0);
    CHECK(doc->findFrame("f5")->story == doc->findFrame("f4")->story);
    CHECK(!run(s, "layout.frame_link doc=lay1 from=f4 to=f1").ok);  // already linked
    CHECK(run(s, "layout.frame_unlink doc=lay1 frame=f4").ok);
    CHECK(doc->findFrame("f5")->story != doc->findFrame("f4")->story);
    s.history().undo();
    CHECK(doc->findFrame("f5")->story == doc->findFrame("f4")->story);

    // pages
    CHECK(run(s, "layout.page_add doc=lay1 count=2 after=0 size=A4").ok);
    CHECK(doc->pages.size() == 4 && doc->pages[0].width == 210 && doc->pages[2].width == 148);
    CHECK(run(s, "layout.page_move doc=lay1 page=1 to=4").ok && doc->pages[3].width == 210);
    CHECK(run(s, "layout.page_delete doc=lay1 page=1 count=1").ok && doc->pages.size() == 3);
    CHECK(!run(s, "layout.page_delete doc=lay1 page=1 count=9").ok && doc->pages.size() == 3);
    CHECK(run(s, "layout.page_set doc=lay1 page=3 size=A5 orientation=landscape").ok && doc->pages[2].width == 210);

    // masters: page numbers on every page
    r = run(s, "layout.master_set doc=lay1 footer=\"Page {page} of {pages}\" align=right");
    CHECK(r.ok && doc->findMaster("A") && doc->pages[1].master == "A");
    const Frame& foot = doc->findMaster("A")->frames[0];
    Story filled = fieldsFilled(*doc, doc->stories[foot.story], 1);
    CHECK(filled.paragraphs[0].text() == "Page 2 of 3");
    CHECK(run(s, "layout.frame_add doc=lay1 type=rect master=A x=0 y=0 w=148 h=5 fill=brand").ok);
    CHECK(doc->findMaster("A")->frames.size() == 2);

    // align and distribute
    run(s, "layout.frame_add doc=lay1 type=rect page=1 x=10 y=10 w=10 h=10 name=r1");
    run(s, "layout.frame_add doc=lay1 type=rect page=1 x=50 y=20 w=20 h=10 name=r2");
    run(s, "layout.frame_add doc=lay1 type=rect page=1 x=100 y=30 w=10 h=10 name=r3");
    const std::string ids = doc->findFrame("r1")->id + "," + doc->findFrame("r2")->id + "," + doc->findFrame("r3")->id;
    CHECK(run(s, "layout.align doc=lay1 frames=" + ids + " edge=top").ok);
    CHECK(near(doc->findFrame("r3")->y, 10));
    CHECK(run(s, "layout.distribute doc=lay1 frames=" + ids + " axis=horizontal").ok);
    CHECK(near(doc->findFrame("r2")->x, 50));  // span 10..110 holds 40 mm of frames: two 30 mm gaps
    CHECK(run(s, "layout.align doc=lay1 frames=" + doc->findFrame("r1")->id + " edge=center to=page").ok);
    CHECK(near(doc->findFrame("r1")->x + 5, doc->pages[0].width / 2));
    CHECK(run(s, "layout.distribute doc=lay1 frames=" + ids + " axis=vertical gap=2").ok);

    // duplicate and order
    r = run(s, "layout.frame_duplicate doc=lay1 frame=f3 dx=0 dy=50");
    CHECK(r.ok);
    const std::string dup = toText(r.value);
    CHECK(doc->findFrame(dup) && doc->findFrame(dup)->story != doc->findFrame("f3")->story);
    CHECK(run(s, "layout.frame_order doc=lay1 frame=f1 order=front").ok);
    {
        int pg = -1;
        doc->findFrame("f1", &pg);
        CHECK(pg >= 0 && doc->pages[static_cast<std::size_t>(pg)].frames.back().id == "f1");
    }
    CHECK(run(s, "layout.frame_delete doc=lay1 frame=" + dup).ok && !doc->findFrame(dup));

    // info and preview
    r = run(s, "layout.info doc=lay1");
    CHECK(r.ok && r.message.find("f1 text") != std::string::npos && r.message.find("master A") != std::string::npos);
    r = run(s, "layout.info doc=lay1 frame=f3");
    CHECK(r.ok && r.message.find("[1] title") != std::string::npos);
    r = run(s, "layout.preview doc=lay1 page=2");
    CHECK(r.ok && r.attachments.size() == 1 && r.attachments[0].mime == "image/png");
    auto img = raster::decode(r.attachments[0].bytes);
    CHECK(img.width() > 100 && img.height() > 100);
    CHECK(run(s, "layout.frame_set doc=lay1 frame=f1 x=15 preview=true").attachments.size() == 1);

    // save / open / close
    const auto file = dir / "flyer.wwlay";
    CHECK(run(s, "layout.save doc=lay1 path=" + q(file)).ok && std::filesystem::exists(file));
    CHECK(!run(s, "layout.save doc=lay1 path=" + q(dir / "x.pdf")).ok);
    r = run(s, "layout.open path=" + q(file));
    CHECK(r.ok);
    auto reopened = docOf(s, toText(r.value));
    CHECK(reopened && toJson(*reopened) == toJson(*doc));
    CHECK(run(s, "layout.close doc=" + toText(r.value)).ok && !docOf(s, toText(r.value)));
    s.history().undo();
    CHECK(docOf(s, "lay3") != nullptr);
    CHECK(!run(s, "layout.open path=" + q(dir / "missing.wwlay")).ok);
    // .achlay files with the old "achclwss-layout" id, from before the rename, still open
    std::string old = toJson(*doc);
    const std::string id = "\"wetwaresuite-layout\"";
    CHECK(old.find(id) != std::string::npos);
    old.replace(old.find(id), id.size(), "\"achclwss-layout\"");
    {
        std::ofstream out(dir / "old.achlay", std::ios::binary);
        out << old;
    }
    r = run(s, "layout.open path=" + q(dir / "old.achlay"));
    CHECK(r.ok);
    if (r.ok) CHECK(toJson(*docOf(s, toText(r.value))) == toJson(*doc));
}

void testPicturesAndOutput(const std::filesystem::path& dir) {
    Session s;
    registerCommands(s);
    // a 600 x 300 px picture: 50.8 mm wide at 300 dpi
    raster::Image pic(600, 300, {200, 30, 30, 255});
    for (int y = 0; y < 300; ++y) {
        for (int x = 0; x < 300; ++x) pic.set(x, y, {30, 30, 200, 255});
    }
    const auto png = dir / "pic.png";
    const auto jpg = dir / "pic.jpg";
    raster::save(pic, png);
    raster::save(pic, jpg, 90);
    raster::Image alpha(40, 40, {0, 200, 0, 100});
    const auto apng = dir / "alpha.png";
    raster::save(alpha, apng);
    {
        std::ofstream svg(dir / "art.svg");
        svg << "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"100\" height=\"50\"><rect x=\"0\" y=\"0\" width=\"50\" "
               "height=\"50\" fill=\"#ff0000\"/><circle cx=\"75\" cy=\"25\" r=\"20\" fill=\"#0000ff\" stroke=\"#000\" stroke-width=\"2\"/></svg>";
    }

    CHECK(run(s, "layout.new size=A4 bleed=3").ok);
    auto doc = docOf(s);
    Result r = run(s, "layout.place_image doc=lay1 path=" + q(png));
    CHECK(r.ok && near(num(r, "dpi"), 300, 0.5));
    const std::string f1 = toText(r.value);
    CHECK(near(doc->findFrame(f1)->w, 50.8, 1e-6));
    r = run(s, "layout.place_image doc=lay1 path=" + q(jpg) + " x=0 y=150 w=210 h=100 fit=fill");
    CHECK(r.ok && num(r, "dpi") < 100);  // stretched to the page width
    const std::string f2 = toText(r.value);
    CHECK(effectiveDpi(*doc->findFrame(f2)) < 100);
    r = run(s, "layout.place_image doc=lay1 path=" + q(dir / "art.svg") + " x=20 y=60 w=80 h=40 fit=fit");
    CHECK(r.ok && doc->findFrame(toText(r.value))->type == FrameType::Vector);
    CHECK(run(s, "layout.place_image doc=lay1 path=" + q(apng) + " x=120 y=60 w=20 h=20").ok);
    CHECK(!run(s, "layout.place_image doc=lay1 path=" + q(dir / "nope.png")).ok);
    // fit modes
    Frame f = *doc->findFrame(f1);
    f.w = 100;
    f.h = 100;
    PictureInfo info = pictureInfo(png.string());
    f.fit = Fit::Fit;
    Box b = pictureBox(f, info);
    CHECK(near(b.w, 100) && near(b.h, 50) && near(b.y, 25));
    f.fit = Fit::Fill;
    b = pictureBox(f, info);
    CHECK(near(b.h, 100) && near(b.w, 200) && near(b.x, -50));
    f.fit = Fit::Manual;
    f.imageScale = 50;
    b = pictureBox(f, info);
    CHECK(near(b.w, 25.4));

    // preflight: low resolution, bleed, overset, missing file
    run(s, "layout.frame_add doc=lay1 type=text x=150 y=10 w=30 h=8 text=\"" + std::string(300, 'x') + " and more words\"");
    run(s, "layout.frame_add doc=lay1 type=rect x=0 y=270 w=100 h=27 fill=brand");
    CHECK(!run(s, "layout.frame_add doc=lay1 type=rect x=0 y=270 w=100 h=27 fill=brand").ok);  // no such swatch yet
    run(s, "layout.frame_add doc=lay1 type=rect x=0 y=270 w=100 h=27 fill=orange");
    r = run(s, "layout.preflight doc=lay1");
    CHECK(r.ok && num(r, "errors") >= 2);
    CHECK(r.message.find("[low_res]") != std::string::npos);
    CHECK(r.message.find("[overset]") != std::string::npos);
    CHECK(r.message.find("[bleed]") != std::string::npos);
    CHECK(r.message.find("[rgb]") != std::string::npos);

    // PDF export
    const auto pdf = dir / "out.pdf";
    r = run(s, "layout.export doc=lay1 path=" + q(pdf) + " bleed=true marks=true");
    CHECK(r.ok && std::filesystem::exists(pdf));
    const std::string bytes = slurp(pdf);
    CHECK(bytes.rfind("%PDF-1.4", 0) == 0 && bytes.find("%%EOF") != std::string::npos);
    CHECK(bytes.find("/TrimBox") != std::string::npos && bytes.find("/BleedBox") != std::string::npos);
    CHECK(bytes.find("/DCTDecode") != std::string::npos);  // the JPEG went in as is
    CHECK(bytes.find("/SMask") != std::string::npos);      // the PNG with alpha got a soft mask
    CHECK(bytes.find("/BaseFont /Helvetica") != std::string::npos);
    // uncompressed content shows the operators: CMYK fill and the image
    run(s, "layout.color_define doc=lay1 name=brand cmyk=0,60,100,0");
    run(s, "layout.frame_set doc=lay1 frame=" + doc->pages[0].frames.back().id + " fill=brand");
    PdfOptions o;
    o.compress = false;
    o.marks = true;
    PdfReport rep;
    const std::string plain = toPdf(*doc, o, &rep);
    CHECK(plain.find("0 0.6 1 0 k") != std::string::npos);
    CHECK(plain.find("1 1 1 1 K") != std::string::npos);  // registration-colour crop marks
    CHECK(plain.find(" Do") != std::string::npos && plain.find(" Tj") != std::string::npos);
    CHECK(rep.pages == 1 && !rep.fonts.empty());
    if (auto tt = pdfdoc::systemFont("helvetica", false, false); tt && tt->embeddable()) {
        o.embedFonts = true;
        const std::string embedded = toPdf(*doc, o, &rep);
        CHECK(embedded.find("/FontFile2") != std::string::npos && !rep.embedded.empty());
    }

    // PNG pages
    run(s, "layout.page_add doc=lay1");
    r = run(s, "layout.export doc=lay1 path=" + q(dir / "page.png") + " dpi=30");
    CHECK(r.ok && std::filesystem::exists(dir / "page-1.png") && std::filesystem::exists(dir / "page-2.png"));
    auto p1 = raster::load(dir / "page-1.png");
    CHECK(p1.width() == static_cast<int>(std::lround(210 * 30 / 25.4)));
    // the red/blue picture landed where it was placed (top-left margin area)
    const auto c = p1.at(static_cast<int>(20 * 30 / 25.4), static_cast<int>(20 * 30 / 25.4));
    CHECK(c.b > 150 && c.r < 100);
    r = run(s, "layout.export doc=lay1 path=" + q(dir / "bleed.png") + " dpi=30 bleed=true pages=1");
    CHECK(r.ok && raster::load(dir / "bleed.png").width() == static_cast<int>(std::lround(216 * 30 / 25.4)));
    CHECK(!run(s, "layout.export doc=lay1 path=" + q(dir / "x.doc")).ok);
    CHECK(!run(s, "layout.export doc=lay1 path=" + q(dir / "x.pdf") + " pages=7").ok);
}

void testBookletAndCards(const std::filesystem::path& dir) {
    Session s;
    registerCommands(s);
    {
        std::ofstream md(dir / "book.md");
        md << "# Book title\n\nIntro paragraph.\n\n";
        for (int ch = 1; ch <= 4; ++ch) {
            md << "# Chapter " << ch << "\n\n";
            for (int p = 0; p < 12; ++p) {
                for (int w = 0; w < 30; ++w) md << "word" << w << (w % 7 == 0 ? " **strong**" : "") << " ";
                md << "\n\n";
            }
            md << "- a bullet\n- another\n\n";
        }
    }
    CHECK(run(s, "layout.new preset=book_6x9 title=Guide").ok);
    auto doc = docOf(s);
    CHECK(run(s, "layout.style_define doc=lay1 name=h1 break_before=true").ok);
    Result r = run(s, "layout.place_text doc=lay1 path=" + q(dir / "book.md"));
    CHECK(r.ok && num(r, "overset") == 0 && num(r, "pages_added") >= 4);
    const auto pages = doc->pages.size();
    CHECK(pages >= 5);
    // every chapter starts a new frame (page)
    auto layouts = layoutAll(*doc);
    int chapterStarts = 0;
    for (const auto& pg : doc->pages) {
        for (const auto& f : pg.frames) {
            const auto& ft = layouts[f.story].frames[f.id];
            if (!ft.lines.empty() && !ft.lines[0].runs.empty() && ft.lines[0].runs[0].text.rfind("Chapter", 0) == 0) ++chapterStarts;
        }
    }
    CHECK(chapterStarts == 4);
    for (const auto& pg : doc->pages) CHECK(pg.master == "A");
    // page numbers come out on the outside: right on odd pages, left on even pages
    const Frame& footer = *std::find_if(doc->findMaster("A")->frames.begin(), doc->findMaster("A")->frames.end(),
                                        [](const Frame& f) { return f.name == "footer"; });
    for (int pi : {0, 1}) {
        const Frame placed = [&] {
            Frame f = footer;
            if (pi == 1) f.x = doc->pages[1].width - footer.x - footer.w;
            return f;
        }();
        StoryLayout sl = layoutStory(*doc, fieldsFilled(*doc, doc->stories[footer.story], pi), {&placed}, pi);
        const auto& run0 = sl.frames[footer.id].lines.at(0).runs.at(0);
        CHECK(run0.text == std::to_string(pi + 1));
        if (pi == 0) CHECK(run0.x > footer.w / 2);
        else CHECK(run0.x < footer.w / 2);
    }
    CHECK(run(s, "layout.export doc=lay1 path=" + q(dir / "book.pdf")).ok);
    CHECK(slurp(dir / "book.pdf").find("/Count " + std::to_string(pages)) != std::string::npos);
    // undo removes the whole placement in one step
    s.history().undo();
    CHECK(doc->pages.size() == 1 && doc->pages[0].frames.empty());

    // business cards 10-up on A4
    CHECK(run(s, "layout.new preset=card").ok);
    run(s, "layout.frame_add doc=lay2 type=text text=\"Ada Lovelace\" size=12 bold=true");
    r = run(s, "layout.impose doc=lay2 path=" + q(dir / "cards.pdf") + " sheet=A4");
    CHECK(r.ok && num(r, "per_sheet") == 10 && num(r, "cols") == 2 && num(r, "rows") == 5);
    CHECK(slurp(dir / "cards.pdf").find("/MediaBox [0 0 595.2756 841.8898]") != std::string::npos);
    r = run(s, "layout.impose doc=lay2 path=" + q(dir / "cards2.pdf") + " sheet=A4 gap=6");
    CHECK(r.ok && num(r, "per_sheet") < 10);
    CHECK(!run(s, "layout.impose doc=lay2 path=" + q(dir / "cards3.pdf") + " sheet=A7 cols=5").ok);
    // Cyrillic and Greek are set in the built-in font and embedded; latin-only cards were not
    CHECK(slurp(dir / "cards.pdf").find("/Type0") == std::string::npos);
    run(s, "layout.frame_add doc=lay2 type=text text=\"\xD0\x90\xD0\xB4\xD0\xB0 \xCE\xA9\" size=12");
    CHECK(run(s, "layout.export doc=lay2 path=" + q(dir / "cyr.pdf")).ok);
    {
        const std::string cyr = slurp(dir / "cyr.pdf");
        CHECK(cyr.find("/Subtype /Type0") != std::string::npos && cyr.find("/FU ") != std::string::npos);
        CHECK(cyr.find("/ToUnicode") != std::string::npos);
    }
}

void testFonts(const std::filesystem::path& dir) {
    auto tt = pdfdoc::systemFont("times", false, false);
    if (!tt || !tt->embeddable()) return;
    Session s;
    registerCommands(s);
    run(s, "layout.new size=A6");
    CHECK(run(s, "layout.font_add doc=lay1 name=brand path=" + q(tt->path())).ok);
    CHECK(!run(s, "layout.font_add doc=lay1 name=helvetica path=" + q(tt->path())).ok);
    CHECK(!run(s, "layout.font_add doc=lay1 name=x path=" + q(dir / "nofont.ttf")).ok);
    CHECK(run(s, "layout.frame_add doc=lay1 type=text text=\"Custom font\" font=brand size=20").ok);
    CHECK(!run(s, "layout.frame_add doc=lay1 type=text text=x font=wingdings").ok);
    auto doc = docOf(s);
    PdfOptions o;
    PdfReport rep;
    const std::string pdf = toPdf(*doc, o, &rep);
    CHECK(pdf.find("/FontFile2") != std::string::npos && pdf.find("/Subtype /TrueType") != std::string::npos);
    CHECK(rep.embedded.size() == 1);
    CHECK(run(s, "layout.preview doc=lay1").ok);
}

void testMcp() {
    Session session;
    registerCommands(session);
    mcp::Server server(session, "test", "0");
    auto call = [&](const std::string& tool, const std::string& args) {
        return server.handle(R"({"jsonrpc":"2.0","id":1,"method":"tools/call","params":{"name":")" + tool + R"(","arguments":)" + args + "}}")
            .value_or("");
    };
    std::string list = server.handle(R"({"jsonrpc":"2.0","id":1,"method":"tools/list"})").value_or("");
    for (const char* tool : {"layout_new", "layout_frame_add", "layout_frame_set", "layout_place_text", "layout_place_image",
                             "layout_preflight", "layout_export", "layout_impose", "layout_preview", "layout_master_set",
                             "layout_style_define", "layout_frame_link", "layout_align", "layout_distribute"}) {
        CHECK(list.find(std::string("\"") + tool + "\"") != std::string::npos);
    }
    std::string r = call("layout_new", R"({"preset":"flyer_a5"})");
    CHECK(r.find("\"doc\":\"lay1\"") != std::string::npos);
    r = call("layout_frame_add", R"({"doc":"lay1","type":"text","text":"Line one\nLine two","size":18,"preview":true})");
    CHECK(r.find("\"type\":\"image\"") != std::string::npos);
    CHECK(r.find("\"frame\":\"f1\"") != std::string::npos);
    CHECK(session.workspace().getAs<LayoutDoc>("lay1")->stories.begin()->second.paragraphs.size() == 2);
    r = call("layout_preview", R"({"doc":"lay1"})");
    CHECK(r.find("\"type\":\"image\"") != std::string::npos);
    r = call("undo", "{}");
    CHECK(r.find("undone") != std::string::npos);
    CHECK(session.workspace().getAs<LayoutDoc>("lay1")->pages[0].frames.empty());
    r = call("layout_preflight", R"({"doc":"lay1"})");
    CHECK(r.find("\"errors\":0") != std::string::npos);
}

}  // namespace

int main() {
    const auto dir = tempDir();
    testMarkup();
    testModel();
    testTextFlow();
    testCommands(dir);
    testPicturesAndOutput(dir);
    testBookletAndCards(dir);
    testFonts(dir);
    testMcp();
    std::filesystem::remove_all(dir);
    return check::summary("layout");
}
