// Every layout.* command. Edits run inside a LayoutEdit, so each is one undo
// step and a failed edit leaves the document untouched; reading, preview and
// export commands stay out of the history.
#include <algorithm>
#include <cmath>
#include <fstream>
#include <set>
#include <sstream>

#include "wetwaresuite/pdfdoc/fonts.hpp"
#include "wetwaresuite/raster/codec.hpp"
#include "internal.hpp"

namespace wetwaresuite::layout {

namespace {

using Edit = std::function<Result(LayoutDoc&, const Params&)>;

bool has(const Params& p, const std::string& key) {
    auto it = p.find(key);
    return it != p.end() && !std::holds_alternative<std::monostate>(it->second);
}

ParamSpec docParam() { return {"doc", ParamType::String, "id of an open layout, e.g. lay1", true}; }
ParamSpec frameParam(const std::string& name = "frame") {
    return {name, ParamType::String, "frame id (f3) or name (layout.info lists them)", true};
}
ParamSpec previewParam() { return {"preview", ParamType::Bool, "attach a PNG preview of the page", false, {}, false}; }
ParamSpec num(const std::string& name, const std::string& help) { return {name, ParamType::Number, help, false}; }
ParamSpec str(const std::string& name, const std::string& help, std::vector<std::string> choices = {}) {
    return {name, ParamType::String, help, false, std::move(choices)};
}
ParamSpec flag(const std::string& name, const std::string& help) { return {name, ParamType::Bool, help, false}; }

std::string paperList() {
    std::string s;
    for (const auto& p : paperSizes()) s += (s.empty() ? "" : ", ") + p.name;
    return s;
}

std::string mm(double v) { return layout::num(v, 2); }

// Page preview attachment, about 800 px on the long side.
Attachment pagePng(const LayoutDoc& doc, int page, double dpi = 0, bool guides = true, bool bleed = false) {
    const Page& pg = doc.pages[static_cast<std::size_t>(page)];
    if (dpi <= 0) dpi = std::clamp(800.0 / std::max(pg.width, pg.height) * 25.4, 10.0, 600.0);
    RenderOptions o;
    o.dpi = dpi;
    o.guides = guides;
    o.bleed = bleed;
    return {"image/png", raster::encodePng(renderPage(doc, page, o)), "page" + std::to_string(page + 1) + ".png"};
}

Result& withPreview(Result& r, const LayoutDoc& doc, const Params& p, int page) {
    if (boolParam(p, "preview") && !doc.pages.empty()) {
        r.attach(pagePng(doc, std::clamp(page, 0, static_cast<int>(doc.pages.size()) - 1)));
    }
    return r;
}

ActionFactory layoutCommand(Session& session, Edit edit, bool undoable = true) {
    return [&session, edit, undoable](const Params& p) -> std::unique_ptr<Action> {
        const std::string id = stringParam(p, "doc");
        auto doc = session.workspace().getAs<LayoutDoc>(id);
        if (!doc) {
            return std::make_unique<FunctionAction>(
                [id] { return Result::failure("no open layout called '" + id + "' (use layout.new or layout.open first)"); });
        }
        if (!undoable) {
            return std::make_unique<FunctionAction>([doc, edit, p] {
                try {
                    return edit(*doc, p);
                } catch (const std::exception& e) {
                    return Result::failure(e.what());
                }
            });
        }
        return std::make_unique<LayoutEdit>(doc, [edit, p](LayoutDoc& d) { return edit(d, p); });
    };
}

Result docCreated(Session& session, std::shared_ptr<LayoutDoc> doc, const std::string& verb) {
    const std::string id = session.workspace().add(doc, "lay");
    Result r = Result::success(verb + " " + id + " (" + doc->summary() + ")", id);
    r.with("doc", id).with("pages", static_cast<std::int64_t>(doc->pages.size())).with("size", NumberList{doc->width, doc->height});
    return r;
}

class CloseAction : public Action {
public:
    CloseAction(Session& session, std::string id) : session_(session), id_(std::move(id)) {}
    Result apply() override {
        doc_ = session_.workspace().take(id_);
        if (!doc_) return Result::failure("no open document called '" + id_ + "'");
        return Result::success("closed " + id_);
    }
    void revert() override {
        if (doc_) session_.workspace().put(id_, doc_);
    }

private:
    Session& session_;
    std::string id_;
    std::shared_ptr<Document> doc_;
};

// 1-based page param -> 0-based index, or -1 with an error.
int pageArg(const LayoutDoc& doc, const Params& p, std::string& error, const std::string& key = "page", int fallback = 1) {
    const auto n = has(p, key) ? intParam(p, key) : fallback;
    if (n < 1 || n > static_cast<std::int64_t>(doc.pages.size())) {
        error = "page " + std::to_string(n) + " doesn't exist (the document has " + std::to_string(doc.pages.size()) + " pages)";
        return -1;
    }
    return static_cast<int>(n - 1);
}

// "1-3,5" or "all" -> sorted 0-based indices within [0, count).
bool rangeArg(const std::string& text, int count, std::vector<int>& out, std::string& error) {
    out.clear();
    if (text.empty() || lower(text) == "all") {
        for (int i = 0; i < count; ++i) out.push_back(i);
        return true;
    }
    std::stringstream in(text);
    std::string item;
    std::set<int> picked;
    while (std::getline(in, item, ',')) {
        if (item.empty()) continue;
        try {
            const auto dash = item.find('-');
            int a = std::stoi(item.substr(0, dash));
            int b = dash == std::string::npos ? a : std::stoi(item.substr(dash + 1));
            if (a > b) std::swap(a, b);
            if (a < 1 || b > count) {
                error = "range " + item + " is outside 1-" + std::to_string(count);
                return false;
            }
            for (int i = a; i <= b; ++i) picked.insert(i - 1);
        } catch (const std::exception&) {
            error = "can't read range '" + item + "' (use e.g. 1-3,5)";
            return false;
        }
    }
    out.assign(picked.begin(), picked.end());
    return true;
}

bool sizeArgs(const Params& p, double& w, double& h, std::string& error) {
    if (has(p, "size")) {
        auto paper = findPaper(stringParam(p, "size"));
        if (!paper) {
            error = "unknown size '" + stringParam(p, "size") + "' (" + paperList() + ", or width= and height= in mm)";
            return false;
        }
        w = paper->width;
        h = paper->height;
    }
    if (has(p, "width")) w = numberParam(p, "width");
    if (has(p, "height")) h = numberParam(p, "height");
    const std::string o = stringParam(p, "orientation");
    if ((o == "landscape" && w < h) || (o == "portrait" && w > h)) std::swap(w, h);
    if (!(w >= 10 && h >= 10 && w <= 5000 && h <= 5000)) {
        error = "page size must be between 10 and 5000 mm";
        return false;
    }
    return true;
}

std::vector<ParamSpec> sizeParams() {
    return {str("size", "paper size: " + paperList()), num("width", "page width in mm"), num("height", "page height in mm"),
            str("orientation", "portrait or landscape (swaps width and height if needed)", {"portrait", "landscape"})};
}

bool marginsArg(const Params& p, Margins& m, std::string& error) {
    if (!has(p, "margins")) return true;
    const NumberList v = listParam(p, "margins");
    if (v.size() == 1) m = {v[0], v[0], v[0], v[0]};
    else if (v.size() == 2) m = {v[0], v[0], v[1], v[1]};
    else if (v.size() == 4) m = {v[0], v[2], v[3], v[1]};  // CSS order: top, right, bottom, left
    else {
        error = "margins takes 1, 2 (vertical, horizontal) or 4 numbers (top, right, bottom, left) in mm";
        return false;
    }
    return true;
}

ParamSpec marginsParam() {
    return {"margins", ParamType::NumberList,
            "margins in mm: one number, vertical,horizontal, or top,right,bottom,left (with facing pages left = inside, right = outside)",
            false};
}

// ---- frame properties ---------------------------------------------------------------

std::vector<ParamSpec> charParams() {
    return {str("font", "font family: helvetica, times, courier (or sans, serif, mono) or a family added with layout.font_add"),
            num("size", "font size in pt"),
            flag("bold", "bold"),
            flag("italic", "italic"),
            flag("underline", "underline"),
            str("color", "text colour: swatch name, #hex, a name, or cmyk(c,m,y,k) in percent"),
            num("tracking", "extra letter spacing in 1/1000 em (e.g. 50; negative tightens)")};
}

std::vector<ParamSpec> paraParams() {
    return {num("leading", "line spacing in pt, baseline to baseline (0 = auto, 1.2 x size)"),
            str("align", "alignment; inside/outside follow the spine on facing pages",
                {"left", "center", "right", "justify", "inside", "outside"}),
            num("space_before", "space above each paragraph, pt"),
            num("space_after", "space below each paragraph, pt"),
            num("indent", "first-line indent, mm (negative for hanging bullets)"),
            num("left_indent", "left indent, mm"),
            flag("break_before", "start each paragraph in the next linked frame (chapters on a new page)")};
}

bool charFromParams(const Params& p, CharProps& c, std::string& error) {
    if (has(p, "font")) c.font = stringParam(p, "font");
    if (has(p, "size")) {
        const double s = numberParam(p, "size");
        if (!(s >= 1 && s <= 2000)) {
            error = "size must be 1 to 2000 pt";
            return false;
        }
        c.size = s;
    }
    if (has(p, "bold")) c.bold = boolParam(p, "bold");
    if (has(p, "italic")) c.italic = boolParam(p, "italic");
    if (has(p, "underline")) c.underline = boolParam(p, "underline");
    if (has(p, "color")) c.color = stringParam(p, "color");
    if (has(p, "tracking")) c.tracking = numberParam(p, "tracking");
    return true;
}

bool paraFromParams(const Params& p, ParaProps& pp, std::string& error) {
    if (has(p, "leading")) {
        const double l = numberParam(p, "leading");
        if (l < 0 || l > 3000) {
            error = "leading must be 0 to 3000 pt";
            return false;
        }
        pp.leading = l;
    }
    if (has(p, "align")) pp.align = parseAlign(stringParam(p, "align"));
    if (has(p, "space_before")) pp.spaceBefore = numberParam(p, "space_before");
    if (has(p, "space_after")) pp.spaceAfter = numberParam(p, "space_after");
    if (has(p, "indent")) pp.indent = numberParam(p, "indent");
    if (has(p, "left_indent")) pp.leftIndent = numberParam(p, "left_indent");
    if (has(p, "break_before")) pp.breakBefore = boolParam(p, "break_before");
    return true;
}

bool checkColour(const LayoutDoc& doc, const std::string& spec, std::string& error) {
    if (!resolvePaint(doc, spec)) {
        error = "unknown colour '" + spec + "' (a swatch, #hex, a name like navy, or cmyk(0,60,100,0))";
        return false;
    }
    return true;
}

bool checkFont(const LayoutDoc& doc, const std::string& font, std::string& error) {
    if (doc.findFont(font) || pdfdoc::standardFamily(font)) return true;
    error = "unknown font '" + font + "' (helvetica, times, courier, or add a TrueType family with layout.font_add)";
    return false;
}

std::vector<ParamSpec> frameParams() {
    std::vector<ParamSpec> s = {
        num("x", "left edge, mm from the page's left trim edge"),
        num("y", "top edge, mm from the page's top trim edge"),
        num("w", "width, mm (lines: x distance to the end point)"),
        num("h", "height, mm (lines: y distance to the end point)"),
        num("rotation", "degrees clockwise around the frame's centre"),
        str("name", "a label to find the frame by"),
        str("fill", "background colour (none, swatch, #hex, name, cmyk(c,m,y,k))"),
        str("stroke", "outline colour"),
        num("stroke_width", "outline width, pt"),
        num("opacity", "0..1"),
        num("radius", "rect corner radius, mm"),
        {"columns", ParamType::Int, "text columns in the frame", false},
        num("gutter", "space between columns, mm"),
        num("inset", "padding inside a text frame, mm"),
        str("valign", "vertical text position in the frame", {"top", "middle", "bottom"}),
        str("text", "text content; new lines (or \\n) split paragraphs; **bold**, *italic*, __underline__, `code`"),
        str("append", "text added as new paragraphs at the end of the story"),
        flag("markdown", "read text as Markdown (# headings, - lists, > quotes)"),
        str("style", "paragraph style for every paragraph (body, h1, h2, h3, quote, bullet, caption, headline, subhead, ...)"),
        str("path", "image (png, jpg, ...) or SVG file for picture frames"),
        str("fit", "how a picture fills its frame: fit (whole picture), fill (cover, crops), stretch, manual",
            {"fit", "fill", "stretch", "manual"}),
        num("image_scale", "manual fit: percent of the 300 dpi size"),
        num("image_x", "manual fit: picture offset in the frame, mm"),
        num("image_y", "manual fit: picture offset in the frame, mm"),
        flag("mirror", "master frames: mirror on left-hand pages of facing documents"),
    };
    for (auto& c : charParams()) s.push_back(c);
    for (auto& c : paraParams()) s.push_back(c);
    return s;
}

Story& storyOf(LayoutDoc& doc, Frame& f) {
    if (f.story.empty()) f.story = doc.newStoryId();
    return doc.stories[f.story];
}

// Applies every frame property present in `p`. Returns false with a message on bad input.
bool applyFrame(LayoutDoc& doc, Frame& f, const Params& p, std::string& error) {
    for (const char* k : {"x", "y", "w", "h", "rotation"}) {
        if (!has(p, k)) continue;
        const double v = numberParam(p, k);
        if (!std::isfinite(v) || std::abs(v) > 10000) {
            error = std::string(k) + " is out of range";
            return false;
        }
    }
    if (has(p, "x")) f.x = numberParam(p, "x");
    if (has(p, "y")) f.y = numberParam(p, "y");
    if (has(p, "w")) f.w = numberParam(p, "w");
    if (has(p, "h")) f.h = numberParam(p, "h");
    if (f.type != FrameType::Line && (f.w <= 0 || f.h <= 0)) {
        error = "w and h must be positive";
        return false;
    }
    if (has(p, "rotation")) f.rotation = numberParam(p, "rotation");
    if (has(p, "name")) f.name = stringParam(p, "name");
    for (const char* k : {"fill", "stroke"}) {
        if (!has(p, k)) continue;
        const std::string v = stringParam(p, k);
        if (!checkColour(doc, v, error)) return false;
        (std::string(k) == "fill" ? f.fill : f.stroke) = lower(v) == "none" ? "" : v;
    }
    if (has(p, "stroke_width")) f.strokeWidth = std::clamp(numberParam(p, "stroke_width"), 0.0, 500.0);
    if (has(p, "opacity")) f.opacity = std::clamp(numberParam(p, "opacity"), 0.0, 1.0);
    if (has(p, "radius")) f.radius = std::max(0.0, numberParam(p, "radius"));
    if (has(p, "mirror")) f.mirror = boolParam(p, "mirror");
    if (has(p, "columns")) f.columns = static_cast<int>(std::clamp<std::int64_t>(intParam(p, "columns"), 1, 20));
    if (has(p, "gutter")) f.gutter = std::clamp(numberParam(p, "gutter"), 0.0, 500.0);
    if (has(p, "inset")) f.inset = std::clamp(numberParam(p, "inset"), 0.0, 500.0);
    if (has(p, "valign")) f.valign = parseVAlign(stringParam(p, "valign")).value_or(VAlign::Top);
    if (has(p, "path")) {
        if (f.type != FrameType::Image && f.type != FrameType::Vector) {
            error = "path is for image and vector frames";
            return false;
        }
        const std::string path = stringParam(p, "path");
        const PictureInfo info = pictureInfo(path);
        if (!info.ok) {
            error = info.error;
            return false;
        }
        f.path = path;
        f.type = info.vector ? FrameType::Vector : FrameType::Image;
    }
    if (has(p, "fit")) f.fit = parseFit(stringParam(p, "fit")).value_or(Fit::Fit);
    if (has(p, "image_scale")) f.imageScale = std::clamp(numberParam(p, "image_scale"), 0.1, 100000.0);
    if (has(p, "image_x")) f.imageX = numberParam(p, "image_x");
    if (has(p, "image_y")) f.imageY = numberParam(p, "image_y");

    const bool textChange = has(p, "text") || has(p, "append") || has(p, "style");
    CharProps cp;
    ParaProps pp;
    if (!charFromParams(p, cp, error) || !paraFromParams(p, pp, error)) return false;
    if (cp.color && !checkColour(doc, *cp.color, error)) return false;
    if (cp.font && !checkFont(doc, *cp.font, error)) return false;
    const bool formatting = cp.font || cp.size || cp.bold || cp.italic || cp.underline || cp.color || cp.tracking || pp.leading ||
                            pp.align || pp.spaceBefore || pp.spaceAfter || pp.indent || pp.leftIndent || pp.breakBefore;
    if ((textChange || formatting) && f.type != FrameType::Text) {
        error = "text and text formatting are for text frames (this is a " + std::string(toString(f.type)) + " frame)";
        return false;
    }
    if (f.type != FrameType::Text) return true;
    const std::string style = stringParam(p, "style");
    if (!style.empty()) {
        const Style* s = doc.findStyle(style);
        if (!s || s->character) {
            error = "no paragraph style called '" + style + "' (layout.style_define makes one)";
            return false;
        }
    }
    Story& story = storyOf(doc, f);
    const bool markdown = boolParam(p, "markdown");
    if (has(p, "text")) story.paragraphs = parseText(stringParam(p, "text"), markdown, style);
    if (has(p, "append")) {
        for (auto& para : parseText(stringParam(p, "append"), markdown, style)) story.paragraphs.push_back(para);
    }
    if (story.paragraphs.empty()) story.paragraphs = parseText("", false);
    for (auto& para : story.paragraphs) {
        if (!style.empty() && !has(p, "text") && !has(p, "append")) para.style = style;
        para.props.merge(pp);
        for (auto& r : para.runs) r.props.merge(cp);
    }
    return true;
}

Frame* frameArg(LayoutDoc& doc, const Params& p, std::string& error, int* page = nullptr, const std::string& key = "frame") {
    const std::string id = stringParam(p, key);
    Frame* f = doc.findFrame(id, page);
    if (!f) error = "no frame called '" + id + "' (layout.info lists them)";
    return f;
}

std::vector<Frame>* containerOf(LayoutDoc& doc, const std::string& frameId) {
    for (auto& pg : doc.pages) {
        for (auto& f : pg.frames) {
            if (f.id == frameId) return &pg.frames;
        }
    }
    for (auto& m : doc.masters) {
        for (auto& f : m.frames) {
            if (f.id == frameId) return &m.frames;
        }
    }
    return nullptr;
}

// Removes a frame, keeping its story's chain linked around it.
void removeFrame(LayoutDoc& doc, const std::string& id) {
    auto* list = containerOf(doc, id);
    if (!list) return;
    auto it = std::find_if(list->begin(), list->end(), [&](const Frame& f) { return f.id == id; });
    const Frame gone = *it;
    list->erase(it);
    if (gone.type != FrameType::Text) return;
    bool used = false;
    auto relink = [&](std::vector<Frame>& frames) {
        for (auto& f : frames) {
            if (f.next == id) f.next = gone.next;
            used = used || (f.type == FrameType::Text && f.story == gone.story);
        }
    };
    for (auto& pg : doc.pages) relink(pg.frames);
    for (auto& m : doc.masters) relink(m.frames);
    if (!used) doc.stories.erase(gone.story);
}

std::string excerpt(const Story& s, std::size_t max = 48) {
    std::string t;
    for (const auto& p : s.paragraphs) {
        if (!t.empty()) t += " / ";
        t += p.text();
        if (t.size() > max * 2) break;
    }
    auto cps = pdfdoc::utf8Decode(t);
    std::string out;
    for (std::size_t i = 0; i < cps.size() && i < max; ++i) out += pdfdoc::utf8Encode(cps[i]);
    if (cps.size() > max) out += "...";
    return out;
}

std::string describeFrame(const LayoutDoc& doc, const Frame& f, const std::map<std::string, StoryLayout>* layouts) {
    std::string s = f.id + " " + toString(f.type);
    if (!f.name.empty()) s += " \"" + f.name + "\"";
    s += " at " + mm(f.x) + "," + mm(f.y) + " size " + mm(f.w) + "x" + mm(f.h);
    if (f.rotation != 0) s += " rot " + mm(f.rotation);
    if (!f.fill.empty()) s += " fill " + f.fill;
    if (!f.stroke.empty()) s += " stroke " + f.stroke + " " + mm(f.strokeWidth) + "pt";
    if (f.opacity != 1) s += " opacity " + mm(f.opacity);
    if (f.type == FrameType::Text) {
        s += " story " + f.story;
        if (f.columns > 1) s += " " + std::to_string(f.columns) + " cols";
        if (!f.next.empty()) s += " -> " + f.next;
        auto st = doc.stories.find(f.story);
        if (st != doc.stories.end()) s += " \"" + excerpt(st->second) + "\"";
        if (layouts) {
            auto it = layouts->find(f.story);
            if (it != layouts->end()) {
                auto ft = it->second.frames.find(f.id);
                if (ft != it->second.frames.end()) s += " " + std::to_string(ft->second.lines.size()) + " lines";
                bool last = false;
                auto chain = storyChain(doc, f.story);
                last = !chain.empty() && chain.back()->id == f.id;
                if (last && it->second.overset) s += " OVERSET " + std::to_string(it->second.overset) + " chars";
            }
        }
    }
    if (f.type == FrameType::Image || f.type == FrameType::Vector) {
        s += " " + (f.path.empty() ? std::string("(empty)") : f.path) + " fit=" + toString(f.fit);
        if (f.type == FrameType::Image && !f.path.empty()) {
            const double dpi = effectiveDpi(f);
            if (dpi > 0) s += " " + layout::num(dpi, 0) + " dpi" + (dpi < 300 ? " (low)" : "");
            else s += " (missing)";
        }
    }
    return s;
}

// A story as the little markup text= takes: a paragraph a line, **bold**, *italic*, __underline__.
std::string storyMarkup(const Story& st) {
    std::string out;
    for (std::size_t i = 0; i < st.paragraphs.size(); ++i) {
        if (i) out += "\n";
        for (const auto& r : st.paragraphs[i].runs) {
            std::string open, close;
            if (r.props.bold.value_or(false)) open += "**", close = "**" + close;
            if (r.props.italic.value_or(false)) open += "*", close = "*" + close;
            if (r.props.underline.value_or(false)) open += "__", close = "__" + close;
            out += open + r.text + close;
        }
    }
    return out;
}

Result edited(LayoutDoc& doc, const Params& p, const std::string& message, int page, const std::string& frame = {}) {
    Result r = Result::success(message, frame.empty() ? Value{} : Value{frame});
    // no "doc" field when a frame is involved, so a batch `-> name` captures the frame id
    if (frame.empty()) r.with("doc", stringParam(p, "doc"));
    else r.with("frame", frame);
    if (page >= 0) r.with("page", static_cast<std::int64_t>(page + 1));
    // report overset of the frame's story, the thing people most often need to know
    if (!frame.empty()) {
        if (const Frame* f = doc.findFrame(frame); f && f->type == FrameType::Text) {
            const auto all = layoutAll(doc);
            auto it = all.find(f->story);
            const std::size_t over = it == all.end() ? 0 : it->second.overset;
            r.with("overset", static_cast<std::int64_t>(over));
            if (over) r.message += " (" + std::to_string(over) + " characters overset)";
        }
    }
    return withPreview(r, doc, p, page);
}

Frame newFrame(LayoutDoc& doc, FrameType type, const Box& box) {
    Frame f;
    f.id = doc.newFrameId();
    f.type = type;
    f.x = box.x;
    f.y = box.y;
    f.w = box.w;
    f.h = box.h;
    if (type == FrameType::Text) {
        f.story = doc.newStoryId();
        doc.stories[f.story].paragraphs = parseText("", false);
        f.gutter = doc.gutter;
    }
    if (type == FrameType::Rect || type == FrameType::Ellipse) f.fill = "#cccccc";
    return f;
}

// ---- place_text -------------------------------------------------------------------------

int pageIndexOf(const LayoutDoc& doc, const std::string& frameId) {
    int page = -1;
    doc.findFrame(frameId, &page);
    return page;
}

// Adds pages and linked frames after the story's last frame until the story fits.
int autoflow(LayoutDoc& doc, const std::string& storyId, const std::string& master, int columns, int& pagesAdded) {
    int framesAdded = 0;
    for (int guard = 0; guard < 2000; ++guard) {
        auto chain = storyChain(doc, storyId);
        if (chain.empty()) break;
        const StoryLayout sl = layoutStory(doc, doc.stories[storyId], chain, 0);
        if (sl.overset == 0) break;
        const Frame* last = chain.back();
        auto lt = sl.frames.find(last->id);
        if (framesAdded > 0 && (lt == sl.frames.end() || lt->second.chars == 0)) break;  // nothing fits in a fresh frame
        const int lastPage = pageIndexOf(doc, last->id);
        if (lastPage < 0) break;
        const std::string lastId = last->id;
        Page page;
        page.width = doc.pages[static_cast<std::size_t>(lastPage)].width;
        page.height = doc.pages[static_cast<std::size_t>(lastPage)].height;
        page.master = master;
        doc.pages.insert(doc.pages.begin() + lastPage + 1, page);
        ++pagesAdded;
        Frame f;
        f.id = doc.newFrameId();
        f.type = FrameType::Text;
        const Box b = marginBox(doc, lastPage + 1);
        f.x = b.x;
        f.y = b.y;
        f.w = b.w;
        f.h = b.h;
        f.columns = columns;
        f.gutter = doc.gutter;
        f.story = storyId;
        doc.pages[static_cast<std::size_t>(lastPage) + 1].frames.push_back(f);
        doc.findFrame(lastId)->next = f.id;
        ++framesAdded;
    }
    return framesAdded;
}

// ---- styles -----------------------------------------------------------------------------

std::set<std::size_t> paragraphSet(const std::string& spec, std::size_t count, std::string& error) {
    std::vector<int> idx;
    std::set<std::size_t> out;
    if (!rangeArg(spec, static_cast<int>(count), idx, error)) return out;
    for (int i : idx) out.insert(static_cast<std::size_t>(i));
    return out;
}

// Splits runs so `match` occurrences become their own runs, then styles them.
int styleMatches(Paragraph& para, const std::string& match, const std::string& style) {
    int hits = 0;
    std::vector<Run> out;
    for (const auto& r : para.runs) {
        std::size_t pos = 0;
        while (true) {
            const auto at = r.text.find(match, pos);
            if (at == std::string::npos) break;
            if (at > pos) out.push_back({r.text.substr(pos, at - pos), r.style, r.props});
            out.push_back({match, style, r.props});
            ++hits;
            pos = at + match.size();
        }
        if (pos < r.text.size()) out.push_back({r.text.substr(pos), r.style, r.props});
    }
    para.runs = out;
    if (para.runs.empty()) para.runs.push_back({});
    return hits;
}

// ---- align / distribute -------------------------------------------------------------------

std::vector<Frame*> frameList(LayoutDoc& doc, const std::string& spec, std::string& error) {
    std::vector<Frame*> out;
    std::stringstream in(spec);
    std::string id;
    while (std::getline(in, id, ',')) {
        if (id.empty()) continue;
        Frame* f = doc.findFrame(id);
        if (!f) {
            error = "no frame called '" + id + "'";
            return {};
        }
        out.push_back(f);
    }
    if (out.empty()) error = "list the frames, e.g. frames=f1,f2,f3";
    return out;
}

}  // namespace

static void registerDocuments(Session& session) {
    auto& reg = session.registry();
    // ---- documents ----------------------------------------------------------------------

    {
        std::vector<ParamSpec> s = {
            str("preset", "starting point: poster_a2, poster_a3, flyer_a5, flyer_a4 (landscape, 3 columns), card (85x55 mm), "
                          "a4_report (page numbers), book_6x9 (facing pages, running head, page numbers), letter",
                presetNames()),
        };
        for (auto& x : sizeParams()) s.push_back(x);
        s.push_back({"pages", ParamType::Int, "number of pages", false, {}, std::int64_t{1}});
        s.push_back(marginsParam());
        s.push_back(num("bleed", "bleed beyond the trim on every side, mm (3 is standard)"));
        s.push_back({"columns", ParamType::Int, "column guides (and autoflow frames) per page", false});
        s.push_back(num("gutter", "space between columns, mm"));
        s.push_back(flag("facing", "facing pages (books, magazines): margins become inside/outside"));
        s.push_back(str("title", "document title (PDF title and the {title} field)"));
        reg.add({"layout.new", "New Layout",
                 "Create a print layout and return its id (e.g. lay1). Sizes are mm; pick a preset or size=A4/A5/letter/card/... "
                 "or width= and height=. Then add frames (layout.frame_add, layout.place_text, layout.place_image) and export "
                 "with layout.export. Coordinates are mm from the page's top-left trim corner, y down.",
                 s, false, true},
                [&session](const Params& p) -> std::unique_ptr<Action> {
                    return std::make_unique<FunctionAction>([&session, p] {
                        try {
                            auto doc = newLayout(stringParam(p, "preset"));
                            std::string error;
                            double w = doc->width, h = doc->height;
                            if (!sizeArgs(p, w, h, error) || !marginsArg(p, doc->margins, error)) return Result::failure(error);
                            doc->width = w;
                            doc->height = h;
                            if (has(p, "bleed")) doc->bleed = std::clamp(numberParam(p, "bleed"), 0.0, 50.0);
                            if (has(p, "columns")) doc->columns = static_cast<int>(std::clamp<std::int64_t>(intParam(p, "columns"), 1, 20));
                            if (has(p, "gutter")) doc->gutter = std::clamp(numberParam(p, "gutter"), 0.0, 200.0);
                            if (has(p, "facing")) doc->facing = boolParam(p, "facing");
                            doc->title = stringParam(p, "title");
                            const auto pages = std::clamp<std::int64_t>(intParam(p, "pages", 1), 1, 5000);
                            const std::string master = doc->pages[0].master;
                            doc->pages.clear();
                            for (std::int64_t i = 0; i < pages; ++i) doc->pages.push_back({w, h, master, {}});
                            // presets with masters placed their header/footer for the preset size: refit them
                            for (auto& m : doc->masters) {
                                for (auto& f : m.frames) {
                                    f.x = doc->margins.left;
                                    f.w = w - doc->margins.left - doc->margins.right;
                                    if (f.name == "footer") f.y = h - doc->margins.bottom + 3;
                                    if (f.name == "header") f.y = std::max(2.0, doc->margins.top - 9);
                                }
                            }
                            return docCreated(session, doc, "created");
                        } catch (const std::exception& e) {
                            return Result::failure(e.what());
                        }
                    });
                });
    }

    reg.add({"layout.open", "Open Layout", "Open a .wwlay layout file (older .achlay files open too) and return its id.",
             {{"path", ParamType::String, "path to a .wwlay file", true}}, false, true},
            [&session](const Params& p) -> std::unique_ptr<Action> {
                return std::make_unique<FunctionAction>([&session, p] {
                    try {
                        return docCreated(session, openLayout(stringParam(p, "path")), "opened");
                    } catch (const std::exception& e) {
                        return Result::failure(e.what());
                    }
                });
            });

    reg.add({"layout.save", "Save Layout",
             "Save the layout as .wwlay (JSON: pages, frames, stories, styles, colours; pictures stay linked by path). "
             "For print files use layout.export.",
             {docParam(), {"path", ParamType::String, "output .wwlay file", true}}, false},
            layoutCommand(session, [](LayoutDoc& doc, const Params& p) -> Result {
                const std::string file = stringParam(p, "path");
                const std::string ext = lower(std::filesystem::path(file).extension().string());
                if (ext != ".wwlay" && ext != ".achlay") return Result::failure("layouts save as .wwlay; use layout.export for pdf or png");
                saveLayout(doc, file);
                Result r = Result::success("saved " + file, file);
                r.with("path", file);
                return r;
            }, false));

    reg.add({"layout.close", "Close Layout", "Close a layout (undo reopens it).", {docParam()}, true},
            [&session](const Params& p) -> std::unique_ptr<Action> {
                const std::string id = stringParam(p, "doc");
                if (!session.workspace().getAs<LayoutDoc>(id)) {
                    return std::make_unique<FunctionAction>([id] { return Result::failure("no open layout called '" + id + "'"); });
                }
                return std::make_unique<CloseAction>(session, id);
            });

    reg.add({"layout.info", "Layout Info",
             "Describe a layout: page size, margins, bleed, every page with its frames (position, size, text excerpt, "
             "picture dpi, overset), stories, styles, colours, fonts and masters. page= or frame= narrows it.",
             {docParam(), {"page", ParamType::Int, "only this page (1-based)", false}, str("frame", "only this frame, with its full text")},
             false, true},
            layoutCommand(session, [](LayoutDoc& doc, const Params& p) -> Result {
                const auto layouts = layoutAll(doc);
                std::string text;
                if (has(p, "frame")) {
                    std::string error;
                    int page = -1;
                    Frame* f = frameArg(doc, p, error, &page);
                    if (!f) return Result::failure(error);
                    text = (page >= 0 ? "page " + std::to_string(page + 1) : std::string("master")) + ": " + describeFrame(doc, *f, &layouts);
                    Result r = Result::success("", f->id);
                    if (f->type == FrameType::Text) {
                        const Story& st = doc.stories[f->story];
                        std::size_t i = 0;
                        for (const auto& para : st.paragraphs) {
                            text += "\n  [" + std::to_string(++i) + "] " + (para.style.empty() ? "body" : para.style) + ": " + para.text();
                        }
                        auto it = layouts.find(f->story);
                        if (it != layouts.end()) r.with("overset", static_cast<std::int64_t>(it->second.overset));
                        r.with("paragraphs", static_cast<std::int64_t>(st.paragraphs.size())).with("markup", storyMarkup(st));
                    }
                    if (f->type == FrameType::Image) r.with("dpi", effectiveDpi(*f));
                    r.message = text;
                    r.with("doc", stringParam(p, "doc")).with("frame", f->id).with("type", std::string(toString(f->type)));
                    r.with("box", NumberList{f->x, f->y, f->w, f->h});
                    return r;
                }
                const std::string id = stringParam(p, "doc");
                text = id + ": " + doc.summary() + "\n" + (doc.title.empty() ? "" : "title: " + doc.title + "\n");
                text += "page " + mm(doc.width) + " x " + mm(doc.height) + " mm, margins top " + mm(doc.margins.top) + " bottom " +
                        mm(doc.margins.bottom) + (doc.facing ? " inside " : " left ") + mm(doc.margins.left) +
                        (doc.facing ? " outside " : " right ") + mm(doc.margins.right) + ", bleed " + mm(doc.bleed) + ", " +
                        std::to_string(doc.columns) + " column(s) gutter " + mm(doc.gutter) + (doc.facing ? ", facing pages" : "");
                std::vector<int> pages;
                if (has(p, "page")) {
                    std::string error;
                    const int pg = pageArg(doc, p, error);
                    if (pg < 0) return Result::failure(error);
                    pages.push_back(pg);
                } else {
                    for (std::size_t i = 0; i < doc.pages.size(); ++i) pages.push_back(static_cast<int>(i));
                }
                std::size_t frames = 0;
                for (int i : pages) {
                    const Page& pg = doc.pages[static_cast<std::size_t>(i)];
                    text += "\npage " + std::to_string(i + 1) + " (" + mm(pg.width) + " x " + mm(pg.height) + " mm" +
                            (pg.master.empty() ? "" : ", master " + pg.master) + "):";
                    if (pg.frames.empty()) text += " empty";
                    for (const auto& f : pg.frames) {
                        text += "\n  " + describeFrame(doc, f, &layouts);
                        ++frames;
                    }
                }
                std::size_t overset = 0;
                if (!has(p, "page")) {
                    for (const auto& m : doc.masters) {
                        text += "\nmaster " + m.name + ":";
                        for (const auto& f : m.frames) text += "\n  " + describeFrame(doc, f, nullptr);
                    }
                    text += "\nstories:";
                    for (const auto& [sid, sl] : layouts) {
                        text += "\n  " + sid + ": " + std::to_string(storyChain(doc, sid).size()) + " frame(s), " + std::to_string(sl.total) +
                                " chars" + (sl.overset ? ", OVERSET " + std::to_string(sl.overset) : "");
                        overset += sl.overset;
                    }
                    std::string ps, cs;
                    for (const auto& s : doc.styles) (s.character ? cs : ps) += (s.character ? (cs.empty() ? "" : ", ") : (ps.empty() ? "" : ", ")) + s.name;
                    text += "\nparagraph styles: " + ps + "\ncharacter styles: " + cs;
                    if (!doc.swatches.empty()) {
                        text += "\ncolours:";
                        for (const auto& s : doc.swatches) {
                            text += " " + s.name + "=" + (s.cmyk ? "cmyk(" + mm(s.c) + "," + mm(s.m) + "," + mm(s.y) + "," + mm(s.k) + ")" : s.rgb.hex().substr(0, 7));
                        }
                    }
                    for (const auto& f : doc.fonts) text += "\nfont " + f.name + ": " + f.regular;
                }
                Result r = Result::success(text, id);
                r.with("doc", id)
                    .with("pages", static_cast<std::int64_t>(doc.pages.size()))
                    .with("frames", static_cast<std::int64_t>(frames))
                    .with("size", NumberList{doc.width, doc.height})
                    .with("bleed", doc.bleed)
                    .with("overset", static_cast<std::int64_t>(overset));
                return r;
            }, false));

    {
        std::vector<ParamSpec> s = {docParam(), str("title", "document title ({title} field, PDF title)"), marginsParam(),
                                    num("bleed", "bleed, mm"), {"columns", ParamType::Int, "column guides per page", false},
                                    num("gutter", "column gutter, mm"), flag("facing", "facing pages")};
        s.push_back(previewParam());
        reg.add({"layout.doc_set", "Document Setup", "Change the title, margins, bleed, column guides or facing pages.", s, true},
                layoutCommand(session, [](LayoutDoc& doc, const Params& p) -> Result {
                    std::string error;
                    if (!marginsArg(p, doc.margins, error)) return Result::failure(error);
                    if (has(p, "title")) doc.title = stringParam(p, "title");
                    if (has(p, "bleed")) doc.bleed = std::clamp(numberParam(p, "bleed"), 0.0, 50.0);
                    if (has(p, "columns")) doc.columns = static_cast<int>(std::clamp<std::int64_t>(intParam(p, "columns"), 1, 20));
                    if (has(p, "gutter")) doc.gutter = std::clamp(numberParam(p, "gutter"), 0.0, 200.0);
                    if (has(p, "facing")) doc.facing = boolParam(p, "facing");
                    return edited(doc, p, "updated document setup", 0);
                }));
    }

}

static void registerPages(Session& session) {
    auto& reg = session.registry();
    // ---- pages --------------------------------------------------------------------------------

    {
        std::vector<ParamSpec> s = {docParam(), {"count", ParamType::Int, "pages to add", false, {}, std::int64_t{1}},
                                    {"after", ParamType::Int, "insert after this page (0 = at the start; default: at the end)", false},
                                    str("master", "master page for the new pages (default: the previous page's; none for none)")};
        for (auto& x : sizeParams()) s.push_back(x);
        s.push_back(previewParam());
        reg.add({"layout.page_add", "Add Pages", "Insert pages (default size: the document's).", s, true},
                layoutCommand(session, [](LayoutDoc& doc, const Params& p) -> Result {
                    const auto count = std::clamp<std::int64_t>(intParam(p, "count", 1), 1, 5000);
                    const auto after = has(p, "after") ? intParam(p, "after") : static_cast<std::int64_t>(doc.pages.size());
                    if (after < 0 || after > static_cast<std::int64_t>(doc.pages.size())) return Result::failure("after= must be 0 to " + std::to_string(doc.pages.size()));
                    double w = doc.width, h = doc.height;
                    std::string error;
                    if (!sizeArgs(p, w, h, error)) return Result::failure(error);
                    std::string master = after > 0 ? doc.pages[static_cast<std::size_t>(after - 1)].master : (doc.pages.empty() ? "" : doc.pages[0].master);
                    if (has(p, "master")) {
                        master = stringParam(p, "master");
                        if (lower(master) == "none") master.clear();
                        else if (!doc.findMaster(master)) return Result::failure("no master called '" + master + "' (layout.master_set makes one)");
                    }
                    for (std::int64_t i = 0; i < count; ++i) doc.pages.insert(doc.pages.begin() + after, Page{w, h, master, {}});
                    Result r = edited(doc, p, "added " + std::to_string(count) + " page(s); now " + std::to_string(doc.pages.size()), static_cast<int>(after));
                    r.value = static_cast<std::int64_t>(after + 1);
                    r.with("pages", static_cast<std::int64_t>(doc.pages.size()));
                    return r;
                }));
    }

    reg.add({"layout.page_delete", "Delete Pages",
             "Delete pages and their frames; text in deleted frames flows on into the story's other frames.",
             {docParam(), {"page", ParamType::Int, "first page to delete (1-based)", true},
              {"count", ParamType::Int, "how many", false, {}, std::int64_t{1}}},
             true},
            layoutCommand(session, [](LayoutDoc& doc, const Params& p) -> Result {
                std::string error;
                const int first = pageArg(doc, p, error);
                if (first < 0) return Result::failure(error);
                const int count = static_cast<int>(std::clamp<std::int64_t>(intParam(p, "count", 1), 1, static_cast<std::int64_t>(doc.pages.size()) - first));
                if (count >= static_cast<int>(doc.pages.size())) return Result::failure("a layout needs at least one page");
                for (int i = 0; i < count; ++i) {
                    std::vector<std::string> ids;
                    for (const auto& f : doc.pages[static_cast<std::size_t>(first)].frames) ids.push_back(f.id);
                    for (const auto& id : ids) removeFrame(doc, id);
                    doc.pages.erase(doc.pages.begin() + first);
                }
                Result r = Result::success("deleted " + std::to_string(count) + " page(s); now " + std::to_string(doc.pages.size()));
                r.with("doc", stringParam(p, "doc")).with("pages", static_cast<std::int64_t>(doc.pages.size()));
                return r;
            }));

    reg.add({"layout.page_move", "Move Page", "Move a page to another position (stories keep flowing in frame link order).",
             {docParam(), {"page", ParamType::Int, "page to move (1-based)", true}, {"to", ParamType::Int, "its new position (1-based)", true}},
             true},
            layoutCommand(session, [](LayoutDoc& doc, const Params& p) -> Result {
                std::string error;
                const int from = pageArg(doc, p, error);
                const int to = from < 0 ? -1 : pageArg(doc, p, error, "to");
                if (from < 0 || to < 0) return Result::failure(error);
                Page pg = doc.pages[static_cast<std::size_t>(from)];
                doc.pages.erase(doc.pages.begin() + from);
                doc.pages.insert(doc.pages.begin() + to, pg);
                return edited(doc, p, "moved page " + std::to_string(from + 1) + " to " + std::to_string(to + 1), to);
            }));

    {
        std::vector<ParamSpec> s = {docParam(), {"page", ParamType::Int, "page (1-based); 0 = every page", true},
                                    str("master", "master page name, or none")};
        for (auto& x : sizeParams()) s.push_back(x);
        s.push_back(previewParam());
        reg.add({"layout.page_set", "Page Setup", "Change a page's size, orientation or master page.", s, true},
                layoutCommand(session, [](LayoutDoc& doc, const Params& p) -> Result {
                    std::vector<int> pages;
                    if (intParam(p, "page") == 0) {
                        for (std::size_t i = 0; i < doc.pages.size(); ++i) pages.push_back(static_cast<int>(i));
                    } else {
                        std::string error;
                        const int pg = pageArg(doc, p, error);
                        if (pg < 0) return Result::failure(error);
                        pages.push_back(pg);
                    }
                    for (int i : pages) {
                        Page& pg = doc.pages[static_cast<std::size_t>(i)];
                        double w = pg.width, h = pg.height;
                        std::string error;
                        if (!sizeArgs(p, w, h, error)) return Result::failure(error);
                        pg.width = w;
                        pg.height = h;
                        if (has(p, "master")) {
                            std::string m = stringParam(p, "master");
                            if (lower(m) == "none") m.clear();
                            else if (!doc.findMaster(m)) return Result::failure("no master called '" + m + "'");
                            pg.master = m;
                        }
                    }
                    if (intParam(p, "page") == 0) {
                        doc.width = doc.pages[0].width;
                        doc.height = doc.pages[0].height;
                    }
                    return edited(doc, p, "updated " + std::to_string(pages.size()) + " page(s)", pages[0]);
                }));
    }

    reg.add({"layout.master_set", "Master Page",
             "Create or change a master page: a running header and/or footer drawn on every page that uses it. Text may use "
             "{page}, {pages} and {title}; align=outside puts page numbers on the outer edge of facing pages. Other frames can "
             "be put on a master with layout.frame_add master=A. apply_to picks the pages that use it.",
             {docParam(), str("master", "master name (default A)"), str("header", "header text, e.g. {title} (empty removes it)"),
              str("footer", "footer text, e.g. Page {page} of {pages} (empty removes it)"),
              str("align", "header/footer alignment", {"left", "center", "right", "inside", "outside"}),
              str("font", "header/footer font"), num("size", "header/footer size, pt"), str("color", "header/footer colour"),
              str("apply_to", "pages that use this master: all, none, or a range like 2-20 (default: all when the master is new)"),
              flag("remove", "delete the master (pages using it get none)"), previewParam()},
             true},
            layoutCommand(session, [](LayoutDoc& doc, const Params& p) -> Result {
                const std::string name = stringParam(p, "master", "A");
                if (name.empty() || lower(name) == "none") return Result::failure("give the master a name, e.g. A");
                if (boolParam(p, "remove")) {
                    auto it = std::find_if(doc.masters.begin(), doc.masters.end(), [&](const Master& m) { return m.name == name; });
                    if (it == doc.masters.end()) return Result::failure("no master called '" + name + "'");
                    for (const auto& f : it->frames) {
                        if (f.type == FrameType::Text) doc.stories.erase(f.story);
                    }
                    doc.masters.erase(it);
                    for (auto& pg : doc.pages) {
                        if (pg.master == name) pg.master.clear();
                    }
                    return edited(doc, p, "removed master " + name, 0);
                }
                bool created = false;
                if (!doc.findMaster(name)) {
                    doc.masters.push_back({name, {}});
                    created = true;
                }
                std::string error;
                CharProps cp;
                if (!charFromParams(p, cp, error)) return Result::failure(error);
                if (cp.font && !checkFont(doc, *cp.font, error)) return Result::failure(error);
                if (cp.color && !checkColour(doc, *cp.color, error)) return Result::failure(error);
                std::vector<std::string> ids;
                for (const char* part : {"header", "footer"}) {
                    if (!has(p, part)) continue;
                    Master* m = doc.findMaster(name);
                    auto it = std::find_if(m->frames.begin(), m->frames.end(), [&](const Frame& f) { return f.name == part; });
                    const std::string text = stringParam(p, part);
                    if (text.empty()) {
                        if (it != m->frames.end()) {
                            doc.stories.erase(it->story);
                            m->frames.erase(it);
                        }
                        continue;
                    }
                    if (it == m->frames.end()) {
                        const double y = std::string(part) == "header" ? std::max(2.0, doc.margins.top - 9) : doc.height - doc.margins.bottom + 3;
                        Frame f = newFrame(doc, FrameType::Text, {doc.margins.left, y, doc.width - doc.margins.left - doc.margins.right, 6});
                        f.name = part;
                        f.mirror = doc.facing;
                        m->frames.push_back(f);
                        it = m->frames.end() - 1;
                    }
                    Story& st = doc.stories[it->story];
                    st.paragraphs = parseText(text, false, "folio");
                    for (auto& para : st.paragraphs) {
                        para.props.align = parseAlign(stringParam(p, "align", doc.facing ? "outside" : "center"));
                        for (auto& r : para.runs) r.props.merge(cp);
                    }
                    ids.push_back(it->id);
                }
                if (has(p, "align") && !has(p, "header") && !has(p, "footer")) {
                    for (auto& f : doc.findMaster(name)->frames) {
                        if (f.type != FrameType::Text) continue;
                        for (auto& para : doc.stories[f.story].paragraphs) para.props.align = parseAlign(stringParam(p, "align"));
                    }
                }
                std::vector<int> pages;
                const std::string apply = stringParam(p, "apply_to", created ? "all" : "");
                if (lower(apply) == "none") {
                    for (auto& pg : doc.pages) {
                        if (pg.master == name) pg.master.clear();
                    }
                } else if (!apply.empty()) {
                    if (!rangeArg(apply, static_cast<int>(doc.pages.size()), pages, error)) return Result::failure(error);
                    for (int i : pages) doc.pages[static_cast<std::size_t>(i)].master = name;
                }
                std::string list;
                for (const auto& id : ids) list += (list.empty() ? "" : ",") + id;
                Result r = edited(doc, p, (created ? "created master " : "updated master ") + name +
                                              (pages.empty() ? "" : ", used by " + std::to_string(pages.size()) + " page(s)"), 0);
                r.value = name;
                r.with("master", name).with("frames", list);
                return r;
            }));

}

static void registerFrames(Session& session) {
    auto& reg = session.registry();
    // ---- frames ---------------------------------------------------------------------------------

    {
        std::vector<ParamSpec> s = {docParam(),
                                    {"type", ParamType::String, "frame type", true, {"text", "image", "rect", "ellipse", "line", "vector"}},
                                    {"page", ParamType::Int, "page (1-based)", false, {}, std::int64_t{1}},
                                    str("master", "put the frame on this master page instead of a page")};
        for (auto& x : frameParams()) s.push_back(x);
        s.push_back(previewParam());
        reg.add({"layout.frame_add", "Add Frame",
                 "Add a frame and return its id. Without x/y/w/h it fills the page's margin box. text frames take text= (with "
                 "**bold**/*italic* markup) and formatting (style, font, size, align, ...); image frames take path= (png, jpg, "
                 "...; svg becomes vector art) and fit=; rect/ellipse/line take fill, stroke and stroke_width (pt). "
                 "A line runs from (x, y) to (x + w, y + h).",
                 s, true},
                layoutCommand(session, [](LayoutDoc& doc, const Params& p) -> Result {
                    const auto type = parseFrameType(stringParam(p, "type"));
                    if (!type) return Result::failure("unknown frame type");
                    std::string error;
                    int page = -1;
                    Master* master = nullptr;
                    if (has(p, "master")) {
                        master = doc.findMaster(stringParam(p, "master"));
                        if (!master) return Result::failure("no master called '" + stringParam(p, "master") + "' (layout.master_set makes one)");
                    } else {
                        page = pageArg(doc, p, error);
                        if (page < 0) return Result::failure(error);
                    }
                    Box box = marginBox(doc, std::max(page, 0));
                    if (*type == FrameType::Line) box = {box.x, box.y, box.w, 0};
                    Frame f = newFrame(doc, *type, box);
                    if (*type == FrameType::Image && has(p, "path") && !has(p, "w") && !has(p, "h")) {
                        const PictureInfo info = pictureInfo(stringParam(p, "path"));
                        if (info.ok) {  // natural size, shrunk to fit the margins
                            const double s = std::min({1.0, box.w / info.naturalW, box.h / info.naturalH});
                            f.w = info.naturalW * s;
                            f.h = info.naturalH * s;
                        }
                    }
                    if (*type == FrameType::Text && has(p, "columns") == false) f.columns = 1;
                    if (has(p, "fill") || has(p, "stroke")) {
                        if (*type == FrameType::Rect || *type == FrameType::Ellipse) f.fill.clear();
                    }
                    if (!applyFrame(doc, f, p, error)) return Result::failure(error);
                    const std::string id = f.id;
                    (master ? master->frames : doc.pages[static_cast<std::size_t>(page)].frames).push_back(f);
                    Result r = edited(doc, p, "added " + std::string(toString(*type)) + " frame " + id + (master ? " on master " + master->name : " on page " + std::to_string(page + 1)), std::max(page, 0), id);
                    if (*type == FrameType::Image && !f.path.empty()) {
                        const double dpi = effectiveDpi(f);
                        r.with("dpi", dpi);
                        if (dpi > 0 && dpi < 300) r.message += "; image prints at " + layout::num(dpi, 0) + " dpi (under 300)";
                    }
                    return r;
                }));
    }

    {
        std::vector<ParamSpec> s = {docParam(), frameParam(), {"page", ParamType::Int, "move the frame to this page (1-based)", false}};
        for (auto& x : frameParams()) s.push_back(x);
        s.push_back(previewParam());
        reg.add({"layout.frame_set", "Change Frame",
                 "Change a frame: position/size (mm), rotation, colours, columns, its text (text= replaces the whole story, "
                 "append= adds paragraphs), formatting applied to all of its text (font, size, bold, align, leading, style, ...), "
                 "or its picture (path, fit, image_scale/x/y).",
                 s, true},
                layoutCommand(session, [](LayoutDoc& doc, const Params& p) -> Result {
                    std::string error;
                    int page = -1;
                    Frame* f = frameArg(doc, p, error, &page);
                    if (!f) return Result::failure(error);
                    if (!applyFrame(doc, *f, p, error)) return Result::failure(error);
                    const std::string id = f->id;
                    if (has(p, "page")) {
                        const int to = pageArg(doc, p, error);
                        if (to < 0) return Result::failure(error);
                        if (page < 0) return Result::failure("frames on masters can't move to a page");
                        if (to != page) {
                            auto& from = doc.pages[static_cast<std::size_t>(page)].frames;
                            auto it = std::find_if(from.begin(), from.end(), [&](const Frame& g) { return g.id == id; });
                            Frame moved = *it;
                            from.erase(it);
                            doc.pages[static_cast<std::size_t>(to)].frames.push_back(moved);
                            page = to;
                        }
                    }
                    return edited(doc, p, "changed " + id, std::max(page, 0), id);
                }));
    }

    reg.add({"layout.frame_text", "Retype Frame",
             "Retype a text frame's words (a paragraph a line, **bold**, *italic*, __underline__) keeping its look: each "
             "paragraph keeps the style, size, font and colour of the one it replaces. layout.info frame= gives the text as markup.",
             {docParam(), frameParam(), str("text", "the new words"), previewParam()}, true},
            layoutCommand(session, [](LayoutDoc& doc, const Params& p) -> Result {
                std::string error;
                int page = -1;
                Frame* f = frameArg(doc, p, error, &page);
                if (!f) return Result::failure(error);
                if (f->type != FrameType::Text) return Result::failure(f->id + " isn't a text frame");
                Story& st = doc.stories[f->story];
                const std::vector<Paragraph> old = st.paragraphs;
                std::vector<Paragraph> fresh = parseText(stringParam(p, "text", ""), false);
                if (fresh.empty()) fresh.emplace_back();
                for (std::size_t i = 0; i < fresh.size(); ++i) {
                    if (old.empty()) break;
                    const Paragraph& was = old[std::min(i, old.size() - 1)];
                    fresh[i].style = was.style;
                    fresh[i].props = was.props;
                    // the look of the run it replaces (first run of that paragraph), the new emphasis on top
                    CharProps base = was.runs.empty() ? CharProps{} : was.runs.front().props;
                    base.bold.reset(), base.italic.reset(), base.underline.reset();
                    const bool wasBold = !was.runs.empty() && was.runs.front().props.bold.value_or(false) &&
                                         std::all_of(was.runs.begin(), was.runs.end(), [](const Run& r) { return r.props.bold.value_or(false); });
                    for (auto& r : fresh[i].runs) {
                        CharProps mine = base;
                        mine.merge(r.props);
                        if (wasBold && !r.props.bold) mine.bold.reset();
                        r.props = mine;
                        if (r.style.empty() && !was.runs.empty()) r.style = was.runs.front().style;
                    }
                }
                st.paragraphs = std::move(fresh);
                return edited(doc, p, "retyped " + f->id, std::max(page, 0), f->id);
            }));

    reg.add({"layout.frames", "List Frames",
             "The frames on one page, for a window to draw handles on: ids, types and boxes (mm, x y w h each, in the order "
             "drawn, bottom first).",
             {docParam(), {"page", ParamType::Int, "page (1-based)", false, {}, std::int64_t{1}}}, false, true},
            layoutCommand(session, [](LayoutDoc& doc, const Params& p) -> Result {
                std::string error;
                const int page = pageArg(doc, p, error);
                if (page < 0) return Result::failure(error);
                const Page& pg = doc.pages[static_cast<std::size_t>(page)];
                std::string ids, types;
                NumberList boxes;
                for (const auto& f : pg.frames) {
                    ids += (ids.empty() ? "" : ",") + f.id;
                    types += (types.empty() ? "" : ",") + std::string(toString(f.type));
                    boxes.insert(boxes.end(), {f.x, f.y, f.w, f.h});
                }
                Result r = Result::success(std::to_string(pg.frames.size()) + " frame(s) on page " + std::to_string(page + 1));
                r.with("doc", stringParam(p, "doc")).with("page", static_cast<std::int64_t>(page + 1)).with("pages", static_cast<std::int64_t>(doc.pages.size()))
                    .with("ids", ids).with("types", types).with("boxes", boxes).with("size", NumberList{pg.width, pg.height});
                return r;
            }, false));

    reg.add({"layout.frame_delete", "Delete Frame", "Delete a frame; a linked text frame's text flows on into the rest of its chain.",
             {docParam(), frameParam(), previewParam()}, true},
            layoutCommand(session, [](LayoutDoc& doc, const Params& p) -> Result {
                std::string error;
                int page = -1;
                Frame* f = frameArg(doc, p, error, &page);
                if (!f) return Result::failure(error);
                const std::string id = f->id;
                removeFrame(doc, id);
                Result r = Result::success("deleted " + id);
                r.with("doc", stringParam(p, "doc"));
                return withPreview(r, doc, p, std::max(page, 0));
            }));

    reg.add({"layout.frame_duplicate", "Duplicate Frame",
             "Copy a frame (text frames get their own copy of the text) and return the new id; offset with dx/dy (mm) or put it on another page.",
             {docParam(), frameParam(), num("dx", "horizontal offset, mm (default 5)"), num("dy", "vertical offset, mm (default 5)"),
              {"page", ParamType::Int, "put the copy on this page (1-based, same position unless dx/dy given)", false}, previewParam()},
             true},
            layoutCommand(session, [](LayoutDoc& doc, const Params& p) -> Result {
                std::string error;
                int page = -1;
                Frame* f = frameArg(doc, p, error, &page);
                if (!f) return Result::failure(error);
                Frame copy = *f;
                std::vector<Frame>* list = containerOf(doc, f->id);
                copy.id = doc.newFrameId();
                if (!copy.name.empty()) copy.name += " copy";
                copy.next.clear();
                const bool otherPage = has(p, "page");
                copy.x += numberParam(p, "dx", otherPage ? 0 : 5);
                copy.y += numberParam(p, "dy", otherPage ? 0 : 5);
                if (copy.type == FrameType::Text) {
                    const Story st = doc.stories[f->story];
                    copy.story = doc.newStoryId();
                    doc.stories[copy.story] = st;
                }
                if (otherPage) {
                    const int to = pageArg(doc, p, error);
                    if (to < 0) return Result::failure(error);
                    list = &doc.pages[static_cast<std::size_t>(to)].frames;
                    page = to;
                }
                const std::string id = copy.id;
                list->push_back(copy);
                return edited(doc, p, "duplicated as " + id, std::max(page, 0), id);
            }));

    reg.add({"layout.frame_order", "Arrange Frame", "Bring a frame to the front, send it to the back, or move it one step.",
             {docParam(), frameParam(), {"order", ParamType::String, "where to", true, {"front", "back", "forward", "backward"}}, previewParam()},
             true},
            layoutCommand(session, [](LayoutDoc& doc, const Params& p) -> Result {
                std::string error;
                int page = -1;
                Frame* f = frameArg(doc, p, error, &page);
                if (!f) return Result::failure(error);
                const std::string id = f->id;
                auto& list = *containerOf(doc, id);
                auto it = std::find_if(list.begin(), list.end(), [&](const Frame& g) { return g.id == id; });
                std::size_t i = static_cast<std::size_t>(it - list.begin());
                Frame moved = *it;
                list.erase(it);
                const std::string order = stringParam(p, "order");
                std::size_t to = order == "front" ? list.size() : order == "back" ? 0 : order == "forward" ? std::min(i + 1, list.size()) : (i > 0 ? i - 1 : 0);
                list.insert(list.begin() + static_cast<std::ptrdiff_t>(to), moved);
                return edited(doc, p, "moved " + id + " " + order, std::max(page, 0), id);
            }));

    reg.add({"layout.frame_link", "Link Text Frames",
             "Make a story flow on from one text frame into another (threading). Text already in the target frame is "
             "appended to the story. The overset count in the reply says whether it fits now.",
             {docParam(), frameParam("from"), frameParam("to"), previewParam()}, true},
            layoutCommand(session, [](LayoutDoc& doc, const Params& p) -> Result {
                std::string error;
                int page = -1;
                Frame* from = frameArg(doc, p, error, &page, "from");
                if (!from) return Result::failure(error);
                Frame* to = frameArg(doc, p, error, nullptr, "to");
                if (!to) return Result::failure(error);
                if (from->type != FrameType::Text || to->type != FrameType::Text) return Result::failure("only text frames link");
                if (from == to) return Result::failure("a frame can't link to itself");
                if (from->story == to->story) return Result::failure(to->id + " already shows this story");
                if (!from->next.empty()) return Result::failure(from->id + " already links to " + from->next + " (layout.frame_unlink first)");
                // the target must be the head of its own chain; its chain joins the end of ours
                for (const Frame* g : storyChain(doc, to->story)) {
                    if (g->next == to->id) return Result::failure(to->id + " is in the middle of another chain");
                }
                const std::string fromId = from->id, toId = to->id, oldStory = to->story, story = from->story;
                Story& target = doc.stories[oldStory];
                bool hasText = false;
                for (const auto& para : target.paragraphs) hasText = hasText || !para.text().empty();
                if (hasText) {
                    for (auto& para : target.paragraphs) doc.stories[story].paragraphs.push_back(para);
                }
                for (auto& pg : doc.pages) {
                    for (auto& f : pg.frames) {
                        if (f.story == oldStory) f.story = story;
                    }
                }
                doc.stories.erase(oldStory);
                doc.findFrame(fromId)->next = toId;
                return edited(doc, p, "linked " + fromId + " -> " + toId, std::max(page, 0), fromId);
            }));

    reg.add({"layout.frame_unlink", "Unlink Text Frame",
             "Cut the link after a text frame: the story ends in it, and the frames that followed become empty.",
             {docParam(), frameParam(), previewParam()}, true},
            layoutCommand(session, [](LayoutDoc& doc, const Params& p) -> Result {
                std::string error;
                int page = -1;
                Frame* f = frameArg(doc, p, error, &page);
                if (!f) return Result::failure(error);
                if (f->next.empty()) return Result::failure(f->id + " isn't linked to another frame");
                const std::string id = f->id;
                std::string next = f->next;
                f->next.clear();
                const std::string fresh = doc.newStoryId();
                doc.stories[fresh].paragraphs = parseText("", false);
                for (int guard = 0; !next.empty() && guard < 100000; ++guard) {
                    Frame* g = doc.findFrame(next);
                    if (!g) break;
                    g->story = fresh;
                    next = g->next;
                }
                return edited(doc, p, "unlinked after " + id, std::max(page, 0), id);
            }));

}

static void registerStylesAndColours(Session& session) {
    auto& reg = session.registry();
    // ---- styles and colours ---------------------------------------------------------------------

    {
        std::vector<ParamSpec> s = {docParam(), {"name", ParamType::String, "style name", true},
                                    str("kind", "paragraph (default) or character", {"paragraph", "character"}),
                                    str("based_on", "style to inherit from (paragraph styles default to body)")};
        for (auto& x : charParams()) s.push_back(x);
        for (auto& x : paraParams()) s.push_back(x);
        s.push_back(previewParam());
        reg.add({"layout.style_define", "Define Style",
                 "Create or change a paragraph or character style. Only the given fields change; the rest inherit from "
                 "based_on. Every paragraph using the style reflows. Built in: body, h1, h2, h3, quote, bullet, code, caption, "
                 "folio, headline, subhead (paragraph) and strong, emphasis (character).",
                 s, true},
                layoutCommand(session, [](LayoutDoc& doc, const Params& p) -> Result {
                    const std::string name = stringParam(p, "name");
                    if (name.empty()) return Result::failure("give the style a name");
                    Style* style = nullptr;
                    for (auto& st : doc.styles) {
                        if (st.name == name) style = &st;
                    }
                    const bool created = !style;
                    if (!style) {
                        doc.styles.push_back({});
                        style = &doc.styles.back();
                        style->name = name;
                        style->character = stringParam(p, "kind") == "character";
                        if (!style->character) style->basedOn = name == "body" ? "" : "body";
                    } else if (has(p, "kind") && (stringParam(p, "kind") == "character") != style->character) {
                        return Result::failure("'" + name + "' is a " + (style->character ? "character" : "paragraph") + " style");
                    }
                    if (has(p, "based_on")) {
                        const std::string base = stringParam(p, "based_on");
                        if (!base.empty() && !doc.findStyle(base)) return Result::failure("no style called '" + base + "'");
                        if (base == name) return Result::failure("a style can't be based on itself");
                        style->basedOn = base;
                    }
                    std::string error;
                    CharProps cp;
                    ParaProps pp;
                    if (!charFromParams(p, cp, error) || !paraFromParams(p, pp, error)) return Result::failure(error);
                    if (cp.font && !checkFont(doc, *cp.font, error)) return Result::failure(error);
                    if (cp.color && !checkColour(doc, *cp.color, error)) return Result::failure(error);
                    if (style->character && (pp.leading || pp.align || pp.spaceBefore || pp.spaceAfter || pp.indent || pp.leftIndent || pp.breakBefore)) {
                        return Result::failure("character styles only take font, size, bold, italic, underline, color and tracking");
                    }
                    style->chars.merge(cp);
                    style->para.merge(pp);
                    Result r = edited(doc, p, (created ? "defined " : "updated ") + std::string(style->character ? "character" : "paragraph") + " style " + name, 0);
                    r.value = name;
                    return r;
                }));
    }

    reg.add({"layout.style_apply", "Apply Style",
             "Apply a paragraph style to paragraphs of a frame's story (paragraphs=all or e.g. 1,3-5), or a character style "
             "to every occurrence of match= text (or to whole paragraphs). clear=true also drops local formatting.",
             {docParam(), {"style", ParamType::String, "style name", true}, frameParam(),
              str("paragraphs", "which paragraphs, 1-based: all (default), 2, or 1,3-5"),
              str("match", "character styles: only this text"), flag("clear", "remove local formatting overrides"), previewParam()},
             true},
            layoutCommand(session, [](LayoutDoc& doc, const Params& p) -> Result {
                std::string error;
                int page = -1;
                Frame* f = frameArg(doc, p, error, &page);
                if (!f) return Result::failure(error);
                if (f->type != FrameType::Text) return Result::failure(f->id + " isn't a text frame");
                const Style* style = doc.findStyle(stringParam(p, "style"));
                if (!style) return Result::failure("no style called '" + stringParam(p, "style") + "'");
                Story& st = doc.stories[f->story];
                const auto which = paragraphSet(stringParam(p, "paragraphs", "all"), st.paragraphs.size(), error);
                if (!error.empty()) return Result::failure(error);
                int hits = 0;
                for (std::size_t i : which) {
                    Paragraph& para = st.paragraphs[i];
                    if (boolParam(p, "clear")) {
                        para.props = {};
                        for (auto& r : para.runs) r.props = {};
                    }
                    if (!style->character) {
                        para.style = style->name;
                        ++hits;
                    } else if (has(p, "match")) {
                        const std::string m = stringParam(p, "match");
                        if (m.empty()) return Result::failure("match can't be empty");
                        hits += styleMatches(para, m, style->name);
                    } else {
                        for (auto& r : para.runs) r.style = style->name;
                        ++hits;
                    }
                }
                if (hits == 0) return Result::failure(has(p, "match") ? "'" + stringParam(p, "match") + "' doesn't occur in that text" : "no paragraphs matched");
                const std::string id = f->id;
                Result r = edited(doc, p, "applied " + style->name + " (" + std::to_string(hits) + (style->character && has(p, "match") ? " matches)" : " paragraphs)"), std::max(page, 0), id);
                r.with("count", static_cast<std::int64_t>(hits));
                return r;
            }));

    reg.add({"layout.color_define", "Define Colour",
             "Create or change a named colour swatch. cmyk= (percent, e.g. 0,60,100,0) makes a print colour exported as "
             "DeviceCMYK; color= (#hex or name) makes an RGB one. Use the name anywhere a colour is asked for.",
             {docParam(), {"name", ParamType::String, "swatch name, e.g. brand", true},
              {"cmyk", ParamType::NumberList, "cyan, magenta, yellow, black in percent", false}, str("color", "RGB colour: #hex or a name"),
              previewParam()},
             true},
            layoutCommand(session, [](LayoutDoc& doc, const Params& p) -> Result {
                const std::string name = stringParam(p, "name");
                if (name.empty() || name.find('(') != std::string::npos || name[0] == '#') return Result::failure("swatch names can't start with # or contain (");
                Swatch sw;
                sw.name = name;
                if (has(p, "cmyk")) {
                    const NumberList v = listParam(p, "cmyk");
                    if (v.size() != 4) return Result::failure("cmyk takes four numbers in percent, e.g. 0,60,100,0");
                    for (double x : v) {
                        if (x < 0 || x > 100) return Result::failure("cmyk values are percent, 0 to 100");
                    }
                    sw.cmyk = true;
                    sw.c = v[0];
                    sw.m = v[1];
                    sw.y = v[2];
                    sw.k = v[3];
                    sw.rgb = cmykToRgb(v[0] / 100, v[1] / 100, v[2] / 100, v[3] / 100);
                } else if (has(p, "color")) {
                    auto paint = resolvePaint(doc, stringParam(p, "color"));
                    if (!paint || paint->none) return Result::failure("unknown colour '" + stringParam(p, "color") + "'");
                    sw = {name, paint->cmyk, paint->c * 100, paint->m * 100, paint->y * 100, paint->k * 100, paint->rgb};
                } else {
                    return Result::failure("give cmyk= or color=");
                }
                bool replaced = false;
                for (auto& s : doc.swatches) {
                    if (s.name == name) {
                        s = sw;
                        replaced = true;
                    }
                }
                if (!replaced) doc.swatches.push_back(sw);
                Result r = edited(doc, p, (replaced ? "updated colour " : "defined colour ") + name + " (" +
                                              (sw.cmyk ? "cmyk " + mm(sw.c) + "," + mm(sw.m) + "," + mm(sw.y) + "," + mm(sw.k) : sw.rgb.hex().substr(0, 7)) + ")",
                                  0);
                r.value = name;
                r.with("rgb", sw.rgb.hex().substr(0, 7));
                return r;
            }));

    reg.add({"layout.font_add", "Add Font",
             "Register a TrueType (.ttf) family under a name usable as font= anywhere. The files are embedded in exported "
             "PDFs. Give the regular file and optionally bold, italic and bold_italic files.",
             {docParam(), {"name", ParamType::String, "family name to use, e.g. brand", true},
              {"path", ParamType::String, "regular .ttf file", true}, str("bold", "bold .ttf"), str("italic", "italic .ttf"),
              str("bold_italic", "bold italic .ttf")},
             true},
            layoutCommand(session, [](LayoutDoc& doc, const Params& p) -> Result {
                const std::string name = stringParam(p, "name");
                if (pdfdoc::standardFamily(name)) return Result::failure("'" + name + "' is a standard font name; pick another");
                FontFamily fam{name, stringParam(p, "path"), stringParam(p, "bold"), stringParam(p, "italic"), stringParam(p, "bold_italic")};
                std::string ps;
                for (const auto& path : {fam.regular, fam.bold, fam.italic, fam.boldItalic}) {
                    if (path.empty()) continue;
                    auto tt = pdfdoc::TrueType::load(path);
                    if (!tt->embeddable()) return Result::failure(path + " has PostScript (CFF) outlines; only TrueType .ttf fonts can be embedded");
                    if (!tt->hasGlyph('a')) return Result::failure(path + " has no Latin letters");
                    if (ps.empty()) ps = tt->postscriptName();
                }
                bool replaced = false;
                for (auto& f : doc.fonts) {
                    if (lower(f.name) == lower(name)) {
                        f = fam;
                        replaced = true;
                    }
                }
                if (!replaced) doc.fonts.push_back(fam);
                Result r = Result::success("added font " + name + " (" + ps + ")", name);
                r.with("doc", stringParam(p, "doc")).with("font", ps);
                return r;
            }));

}

static void registerPlacing(Session& session) {
    auto& reg = session.registry();
    // ---- placing ---------------------------------------------------------------------------

    reg.add({"layout.place_text", "Place Text",
             "Flow a .txt or .md file into the layout. With autoflow (default) it fills a frame in the margins of the start "
             "page, then adds pages with linked frames until all the text fits: a whole booklet from one Markdown file. "
             "Markdown # ## ### become h1/h2/h3, lists bullets, > quotes, ``` code; **bold** and *italic* work in both.",
             {docParam(), {"path", ParamType::String, ".txt or .md file", true},
              {"page", ParamType::Int, "start page (1-based)", false, {}, std::int64_t{1}},
              str("frame", "existing text frame to fill (its story is replaced)"),
              flag("markdown", "read as Markdown (default: true for .md/.markdown)"),
              str("style", "paragraph style for plain paragraphs (default body)"),
              {"autoflow", ParamType::Bool, "add pages and linked frames until the text fits", false, {}, true},
              {"columns", ParamType::Int, "columns in new frames (default: the document's column guides)", false},
              str("master", "master page for added pages (default: the start page's)"), previewParam()},
             true},
            layoutCommand(session, [](LayoutDoc& doc, const Params& p) -> Result {
                const std::string path = stringParam(p, "path");
                const std::string text = readFile(path);
                const std::string ext = lower(std::filesystem::path(path).extension().string());
                const bool md = has(p, "markdown") ? boolParam(p, "markdown") : (ext == ".md" || ext == ".markdown");
                const std::string style = stringParam(p, "style");
                if (!style.empty() && (!doc.findStyle(style) || doc.findStyle(style)->character)) return Result::failure("no paragraph style called '" + style + "'");
                std::string error;
                const int columns = static_cast<int>(std::clamp<std::int64_t>(intParam(p, "columns", doc.columns), 1, 20));
                std::string frameId;
                int page = 0;
                if (has(p, "frame")) {
                    Frame* f = frameArg(doc, p, error, &page);
                    if (!f) return Result::failure(error);
                    if (f->type != FrameType::Text) return Result::failure(f->id + " isn't a text frame");
                    if (page < 0) return Result::failure("place text on page frames, not masters");
                    frameId = f->id;
                    doc.stories[f->story].paragraphs = parseText(text, md, style);
                } else {
                    page = pageArg(doc, p, error);
                    if (page < 0) return Result::failure(error);
                    Frame f = newFrame(doc, FrameType::Text, marginBox(doc, page));
                    f.columns = columns;
                    doc.stories[f.story].paragraphs = parseText(text, md, style);
                    frameId = f.id;
                    doc.pages[static_cast<std::size_t>(page)].frames.push_back(f);
                }
                const std::string story = doc.findFrame(frameId)->story;
                int pagesAdded = 0, framesAdded = 0;
                if (boolParam(p, "autoflow", true)) {
                    std::string master = doc.pages[static_cast<std::size_t>(page)].master;
                    if (has(p, "master")) {
                        master = stringParam(p, "master");
                        if (lower(master) == "none") master.clear();
                        else if (!doc.findMaster(master)) return Result::failure("no master called '" + master + "'");
                    }
                    framesAdded = autoflow(doc, story, master, columns, pagesAdded);
                }
                const auto sl = layoutStory(doc, doc.stories[story], storyChain(doc, story), 0);
                Result r = Result::success("placed " + std::to_string(doc.stories[story].paragraphs.size()) + " paragraphs from " + path + " in " +
                                               frameId + (pagesAdded ? ", added " + std::to_string(pagesAdded) + " page(s)" : "") +
                                               (sl.overset ? "; " + std::to_string(sl.overset) + " characters overset" : "; all text fits"),
                                           frameId);
                r.with("frame", frameId)
                    .with("story", story)
                    .with("paragraphs", static_cast<std::int64_t>(doc.stories[story].paragraphs.size()))
                    .with("pages_added", static_cast<std::int64_t>(pagesAdded))
                    .with("frames_added", static_cast<std::int64_t>(framesAdded))
                    .with("pages", static_cast<std::int64_t>(doc.pages.size()))
                    .with("overset", static_cast<std::int64_t>(sl.overset));
                return withPreview(r, doc, p, page);
            }));

    reg.add({"layout.place_image", "Place Image",
             "Place a picture (png, jpg, bmp, gif, tga, psd, ...; .svg becomes vector art) in a new frame, or into an existing "
             "frame with frame=. Without w/h the frame takes the picture's 300 dpi size, shrunk to fit the margins. The reply "
             "gives the effective dpi at print size and warns under 300.",
             {docParam(), {"path", ParamType::String, "picture file", true},
              {"page", ParamType::Int, "page (1-based)", false, {}, std::int64_t{1}}, str("frame", "existing image frame to put it in"),
              num("x", "left, mm (default: left margin)"), num("y", "top, mm (default: top margin)"), num("w", "width, mm"),
              num("h", "height, mm"),
              {"fit", ParamType::String, "fit (whole picture), fill (cover the frame, cropping), stretch, manual", false,
               {"fit", "fill", "stretch", "manual"}, std::string("fill")},
              str("name", "frame label"), previewParam()},
             true},
            layoutCommand(session, [](LayoutDoc& doc, const Params& p) -> Result {
                const std::string path = stringParam(p, "path");
                const PictureInfo info = pictureInfo(path);
                if (!info.ok) return Result::failure(info.error);
                std::string error;
                int page = -1;
                Frame* target = nullptr;
                if (has(p, "frame")) {
                    target = frameArg(doc, p, error, &page);
                    if (!target) return Result::failure(error);
                    if (target->type != FrameType::Image && target->type != FrameType::Vector) return Result::failure(target->id + " isn't a picture frame");
                } else {
                    page = pageArg(doc, p, error);
                    if (page < 0) return Result::failure(error);
                    const Box m = marginBox(doc, page);
                    Frame f = newFrame(doc, info.vector ? FrameType::Vector : FrameType::Image, m);
                    f.x = numberParam(p, "x", m.x);
                    f.y = numberParam(p, "y", m.y);
                    const double maxW = has(p, "x") ? doc.pages[static_cast<std::size_t>(page)].width - f.x : m.w;
                    const double maxH = has(p, "y") ? doc.pages[static_cast<std::size_t>(page)].height - f.y : m.h;
                    const double s = std::min({1.0, std::max(1.0, maxW) / info.naturalW, std::max(1.0, maxH) / info.naturalH});
                    f.w = info.naturalW * s;
                    f.h = info.naturalH * s;
                    if (has(p, "w") && !has(p, "h")) {
                        f.w = numberParam(p, "w");
                        f.h = f.w * info.naturalH / info.naturalW;
                    } else if (has(p, "h") && !has(p, "w")) {
                        f.h = numberParam(p, "h");
                        f.w = f.h * info.naturalW / info.naturalH;
                    } else if (has(p, "w") && has(p, "h")) {
                        f.w = numberParam(p, "w");
                        f.h = numberParam(p, "h");
                    }
                    if (!(f.w > 0 && f.h > 0)) return Result::failure("w and h must be positive");
                    doc.pages[static_cast<std::size_t>(page)].frames.push_back(f);
                    target = &doc.pages[static_cast<std::size_t>(page)].frames.back();
                }
                target->path = path;
                target->type = info.vector ? FrameType::Vector : FrameType::Image;
                target->fit = parseFit(stringParam(p, "fit", "fill")).value_or(Fit::Fill);
                if (has(p, "name")) target->name = stringParam(p, "name");
                const std::string id = target->id;
                const double dpi = effectiveDpi(*target);
                Result r = edited(doc, p, "placed " + path + " in " + id + (info.vector ? " (vector)" : " at " + layout::num(dpi, 0) + " dpi"), std::max(page, 0), id);
                if (!info.vector) {
                    r.with("dpi", dpi).with("pixels", NumberList{static_cast<double>(info.pxWidth), static_cast<double>(info.pxHeight)});
                    if (dpi < 300) r.message += " - under 300 dpi, it may print soft; make the frame smaller or use a bigger picture";
                }
                const Frame& placed = *doc.findFrame(id);
                r.with("box", NumberList{placed.x, placed.y, placed.w, placed.h});
                return r;
            }));

}

static void registerArranging(Session& session) {
    auto& reg = session.registry();
    // ---- arranging -------------------------------------------------------------------------

    reg.add({"layout.align", "Align Frames",
             "Line frames up: their left/right/top/bottom edges or centres, relative to each other (selection), the page, or "
             "the margins.",
             {docParam(), {"frames", ParamType::String, "comma-separated frame ids, e.g. f1,f2,f3", true},
              {"edge", ParamType::String, "what to line up", true, {"left", "right", "top", "bottom", "center", "middle"}},
              {"to", ParamType::String, "relative to", false, {"selection", "page", "margins"}, std::string("selection")}, previewParam()},
             true},
            layoutCommand(session, [](LayoutDoc& doc, const Params& p) -> Result {
                std::string error;
                auto frames = frameList(doc, stringParam(p, "frames"), error);
                if (frames.empty()) return Result::failure(error);
                int page = -1;
                doc.findFrame(frames[0]->id, &page);
                const std::string to = stringParam(p, "to", "selection");
                Box ref;
                if (to == "selection") {
                    double x0 = 1e18, y0 = 1e18, x1 = -1e18, y1 = -1e18;
                    for (const Frame* f : frames) {
                        const Box b = frameBounds(*f);
                        x0 = std::min(x0, b.x);
                        y0 = std::min(y0, b.y);
                        x1 = std::max(x1, b.x + b.w);
                        y1 = std::max(y1, b.y + b.h);
                    }
                    ref = {x0, y0, x1 - x0, y1 - y0};
                } else if (to == "page") {
                    const Page& pg = doc.pages[static_cast<std::size_t>(std::max(page, 0))];
                    ref = {0, 0, pg.width, pg.height};
                } else {
                    ref = marginBox(doc, std::max(page, 0));
                }
                const std::string edge = stringParam(p, "edge");
                for (Frame* f : frames) {
                    const Box b = frameBounds(*f);
                    if (edge == "left") f->x += ref.x - b.x;
                    else if (edge == "right") f->x += (ref.x + ref.w) - (b.x + b.w);
                    else if (edge == "center") f->x += (ref.x + ref.w / 2) - (b.x + b.w / 2);
                    else if (edge == "top") f->y += ref.y - b.y;
                    else if (edge == "bottom") f->y += (ref.y + ref.h) - (b.y + b.h);
                    else f->y += (ref.y + ref.h / 2) - (b.y + b.h / 2);
                }
                return edited(doc, p, "aligned " + std::to_string(frames.size()) + " frame(s) " + edge + " to " + to, std::max(page, 0));
            }));

    reg.add({"layout.distribute", "Distribute Frames",
             "Space frames evenly along an axis: equal gaps between the first and last (3+ frames), or a fixed gap= in mm "
             "starting from the first.",
             {docParam(), {"frames", ParamType::String, "comma-separated frame ids", true},
              {"axis", ParamType::String, "direction", true, {"horizontal", "vertical"}}, num("gap", "fixed gap between frames, mm"),
              previewParam()},
             true},
            layoutCommand(session, [](LayoutDoc& doc, const Params& p) -> Result {
                std::string error;
                auto frames = frameList(doc, stringParam(p, "frames"), error);
                if (frames.empty()) return Result::failure(error);
                const bool horizontal = stringParam(p, "axis") == "horizontal";
                if (frames.size() < 2 || (!has(p, "gap") && frames.size() < 3)) return Result::failure("distribute needs 3 frames (or 2 with gap=)");
                auto start = [&](const Frame* f) {
                    const Box b = frameBounds(*f);
                    return horizontal ? b.x : b.y;
                };
                auto extent = [&](const Frame* f) {
                    const Box b = frameBounds(*f);
                    return horizontal ? b.w : b.h;
                };
                std::sort(frames.begin(), frames.end(), [&](const Frame* a, const Frame* b) { return start(a) < start(b); });
                double gap = numberParam(p, "gap");
                if (!has(p, "gap")) {
                    double total = 0;
                    for (const Frame* f : frames) total += extent(f);
                    const double span = start(frames.back()) + extent(frames.back()) - start(frames.front());
                    gap = (span - total) / static_cast<double>(frames.size() - 1);
                }
                double pos = start(frames.front()) + extent(frames.front()) + gap;
                for (std::size_t i = 1; i < frames.size(); ++i) {
                    const double delta = pos - start(frames[i]);
                    (horizontal ? frames[i]->x : frames[i]->y) += delta;
                    pos += extent(frames[i]) + gap;
                }
                int page = -1;
                doc.findFrame(frames[0]->id, &page);
                Result r = edited(doc, p, "distributed " + std::to_string(frames.size()) + " frames, gap " + mm(gap) + " mm", std::max(page, 0));
                r.with("gap", gap);
                return r;
            }));

}

static void registerCheckingAndOutput(Session& session) {
    auto& reg = session.registry();
    // ---- checking and output ------------------------------------------------------------------

    reg.add({"layout.preflight", "Preflight",
             "Check the layout before printing: overset text, missing pictures and fonts, images under min_dpi at print size, "
             "artwork touching the page edge without bleed, items off the page, text outside the margins, RGB colours and "
             "unembedded fonts. errors=0 means it's ready to export.",
             {docParam(), {"min_dpi", ParamType::Number, "lowest acceptable image resolution", false, {}, 300.0}}, false, true},
            layoutCommand(session, [](LayoutDoc& doc, const Params& p) -> Result {
                const auto issues = preflight(doc, numberParam(p, "min_dpi", 300));
                std::int64_t errors = 0, warnings = 0, infos = 0;
                std::string text;
                for (const auto& i : issues) {
                    if (i.severity == "error") ++errors;
                    else if (i.severity == "warning") ++warnings;
                    else ++infos;
                    text += "\n  " + i.severity + " [" + i.kind + "] " + i.where + ": " + i.message;
                }
                const std::string head = errors == 0 && warnings == 0 ? "preflight passed" : "preflight: " + std::to_string(errors) + " error(s), " + std::to_string(warnings) + " warning(s)";
                Result r = Result::success(head + (infos ? ", " + std::to_string(infos) + " note(s)" : "") + text, errors == 0);
                r.with("doc", stringParam(p, "doc")).with("errors", errors).with("warnings", warnings).with("notes", infos).with("ok", errors == 0);
                return r;
            }, false));

    reg.add({"layout.export", "Export Layout",
             "Write a print PDF (bleed, crop marks, CMYK colours, optional embedded fonts) or PNG/JPEG pages at dpi. "
             "Several pages as images write name-1.png, name-2.png, ... (or use {page} in the path).",
             {docParam(), {"path", ParamType::String, "output .pdf, .png, .jpg (or .bmp/.tga)", true},
              {"bleed", ParamType::Bool, "include the bleed area (PDF: BleedBox; images: extra border)", false, {}, false},
              {"marks", ParamType::Bool, "PDF: crop marks and a page label outside the bleed", false, {}, false},
              str("pages", "pages to export: all (default) or e.g. 1-3,5"),
              {"dpi", ParamType::Number, "image exports: resolution", false, {}, 300.0},
              {"embed_fonts", ParamType::Bool, "PDF: embed TrueType substitutes for the standard fonts", false, {}, false},
              {"guides", ParamType::Bool, "image exports: draw margins, columns and frame edges", false, {}, false},
              {"quality", ParamType::Int, "JPEG quality 1-100", false, {}, std::int64_t{92}}},
             false},
            layoutCommand(session, [](LayoutDoc& doc, const Params& p) -> Result {
                const std::string path = stringParam(p, "path");
                std::string ext = lower(std::filesystem::path(path).extension().string());
                std::vector<int> pages;
                std::string error;
                if (!rangeArg(stringParam(p, "pages", "all"), static_cast<int>(doc.pages.size()), pages, error)) return Result::failure(error);
                if (ext == ".pdf") {
                    PdfOptions o;
                    o.bleed = boolParam(p, "bleed");
                    o.marks = boolParam(p, "marks");
                    o.embedFonts = boolParam(p, "embed_fonts");
                    o.pages = pages;
                    PdfReport rep;
                    const std::string bytes = toPdf(doc, o, &rep);
                    std::ofstream out(std::filesystem::path(path), std::ios::binary);
                    if (!out || !(out << bytes)) return Result::failure("can't write " + path);
                    std::string fonts, embedded;
                    for (const auto& f : rep.fonts) fonts += (fonts.empty() ? "" : ", ") + f;
                    for (const auto& f : rep.embedded) embedded += (embedded.empty() ? "" : ", ") + f;
                    std::string msg = "exported " + std::to_string(rep.pages) + " page(s) to " + path + " (" + std::to_string(bytes.size() / 1024) + " KB" +
                                      (o.bleed ? ", bleed " + mm(doc.bleed) + " mm" : "") + (o.marks ? ", crop marks" : "") + ")";
                    if (!fonts.empty()) msg += "; fonts: " + fonts;
                    if (!embedded.empty()) msg += "; embedded: " + embedded;
                    for (const auto& w : rep.warnings) msg += "; " + w;
                    Result r = Result::success(msg, path);
                    r.with("path", path).with("pages", static_cast<std::int64_t>(rep.pages)).with("bytes", static_cast<std::int64_t>(bytes.size()));
                    r.with("fonts", fonts).with("embedded", embedded);
                    return r;
                }
                if (!raster::canWrite(path)) return Result::failure("can't export " + ext + " (use .pdf, .png, .jpg, .bmp or .tga)");
                RenderOptions o;
                o.dpi = std::clamp(numberParam(p, "dpi", 300), 10.0, 1200.0);
                o.bleed = boolParam(p, "bleed");
                o.guides = boolParam(p, "guides");
                std::vector<std::string> written;
                for (int i : pages) {
                    std::string out = path;
                    const auto at = out.find("{page}");
                    if (at != std::string::npos) {
                        out.replace(at, 6, std::to_string(i + 1));
                    } else if (pages.size() > 1) {
                        std::filesystem::path fp(path);
                        out = (fp.parent_path() / (fp.stem().string() + "-" + std::to_string(i + 1) + fp.extension().string())).string();
                    }
                    raster::save(renderPage(doc, i, o), out, static_cast<int>(std::clamp<std::int64_t>(intParam(p, "quality", 92), 1, 100)));
                    written.push_back(out);
                }
                std::string list;
                for (const auto& w : written) list += (list.empty() ? "" : ", ") + w;
                Result r = Result::success("exported " + std::to_string(written.size()) + " page image(s) at " + mm(o.dpi) + " dpi: " + list, written.front());
                r.with("path", written.front()).with("files", list).with("pages", static_cast<std::int64_t>(written.size()));
                return r;
            }, false));

    reg.add({"layout.impose", "Impose N-up",
             "Print several copies (or pages) per sheet: business cards 10-up on A4, labels, tickets. Writes a PDF with the "
             "items in a grid, centred, with crop marks on every cut line. gap >= 2 x bleed keeps each item's bleed; gap=0 "
             "butts them together (cut on the shared lines).",
             {docParam(), {"path", ParamType::String, "output .pdf", true},
              {"sheet", ParamType::String, "sheet size: " + paperList(), false, {}, std::string("A4")},
              str("orientation", "sheet orientation", {"portrait", "landscape"}), {"cols", ParamType::Int, "columns (default: as many as fit)", false},
              {"rows", ParamType::Int, "rows (default: as many as fit)", false}, num("gap", "space between items, mm (default 0)"),
              {"marks", ParamType::Bool, "crop marks", false, {}, true},
              {"repeat", ParamType::Bool, "true: fill each sheet with one page (front, then back); false: pages in turn", false, {}, true},
              str("pages", "pages to impose: all (default) or e.g. 1"),
              {"embed_fonts", ParamType::Bool, "embed TrueType substitutes for the standard fonts", false, {}, false}},
             false},
            layoutCommand(session, [](LayoutDoc& doc, const Params& p) -> Result {
                const std::string path = stringParam(p, "path");
                const std::string ext = lower(std::filesystem::path(path).extension().string());
                if (ext != ".pdf") return Result::failure("impose writes a .pdf");
                ImposeOptions o;
                auto sheet = findPaper(stringParam(p, "sheet", "A4"));
                if (!sheet) return Result::failure("unknown sheet size (" + paperList() + ")");
                o.sheetW = sheet->width;
                o.sheetH = sheet->height;
                const std::string orient = stringParam(p, "orientation");
                if ((orient == "landscape" && o.sheetW < o.sheetH) || (orient == "portrait" && o.sheetW > o.sheetH)) std::swap(o.sheetW, o.sheetH);
                o.cols = static_cast<int>(std::clamp<std::int64_t>(intParam(p, "cols", 0), 0, 100));
                o.rows = static_cast<int>(std::clamp<std::int64_t>(intParam(p, "rows", 0), 0, 100));
                o.gap = std::clamp(numberParam(p, "gap", 0), 0.0, 100.0);
                o.marks = boolParam(p, "marks", true);
                o.repeat = boolParam(p, "repeat", true);
                o.embedFonts = boolParam(p, "embed_fonts");
                std::string error;
                if (!rangeArg(stringParam(p, "pages", "all"), static_cast<int>(doc.pages.size()), o.pages, error)) return Result::failure(error);
                PdfReport rep;
                const std::string bytes = imposePdf(doc, o, &rep);
                std::ofstream out(std::filesystem::path(path), std::ios::binary);
                if (!out || !(out << bytes)) return Result::failure("can't write " + path);
                const int per = rep.cols * rep.rows;
                Result r = Result::success("imposed " + std::to_string(per) + "-up (" + std::to_string(rep.cols) + " x " + std::to_string(rep.rows) + ") on " +
                                               std::to_string(rep.pages) + " " + stringParam(p, "sheet", "A4") + " sheet(s): " + path,
                                           path);
                r.with("path", path).with("per_sheet", static_cast<std::int64_t>(per)).with("sheets", static_cast<std::int64_t>(rep.pages));
                r.with("cols", static_cast<std::int64_t>(rep.cols)).with("rows", static_cast<std::int64_t>(rep.rows));
                return r;
            }, false));

    reg.add({"layout.preview", "Preview Page",
             "Render a page to PNG so you can see it, with margins (magenta), column gutters (violet), frame edges (blue), "
             "the bleed (red, with bleed=true) and a red square on frames whose text is overset.",
             {docParam(), {"page", ParamType::Int, "page (1-based)", false, {}, std::int64_t{1}},
              num("dpi", "resolution (default: about 800 px on the long side)"),
              {"guides", ParamType::Bool, "draw guides", false, {}, true}, {"bleed", ParamType::Bool, "show the bleed area", false, {}, false}},
             false, true},
            layoutCommand(session, [](LayoutDoc& doc, const Params& p) -> Result {
                std::string error;
                const int page = pageArg(doc, p, error);
                if (page < 0) return Result::failure(error);
                Attachment a = pagePng(doc, page, numberParam(p, "dpi", 0), boolParam(p, "guides", true), boolParam(p, "bleed"));
                Result r = Result::success("page " + std::to_string(page + 1) + " of " + std::to_string(doc.pages.size()));
                r.with("doc", stringParam(p, "doc")).with("page", static_cast<std::int64_t>(page + 1));
                r.attach(std::move(a));
                return r;
            }, false));
}

void registerCommands(Session& session) {
    registerDocuments(session);
    registerPages(session);
    registerFrames(session);
    registerStylesAndColours(session);
    registerPlacing(session);
    registerArranging(session);
    registerCheckingAndOutput(session);
}

}  // namespace wetwaresuite::layout
