// office.doc_* commands: text documents as numbered blocks (1 = first) that
// are added, edited, moved and deleted one at a time, then saved or exported.
#include <algorithm>
#include <cmath>

#include "wetwaresuite/raster/codec.hpp"
#include "commands.hpp"
#include "doc_cmd.hpp"
#include "text.hpp"

namespace wetwaresuite::office {

namespace {

using Edit = std::function<Result(TextDoc&, const Params&, const std::string&)>;

ActionFactory edit(Session& s, Edit fn) { return docCommand<TextDoc>(s, std::move(fn), true); }
ActionFactory read(Session& s, Edit fn) { return docCommand<TextDoc>(s, std::move(fn), false); }

ParamSpec doc() { return docSpec("document", "doc1"); }

const std::vector<std::string>& blockKinds() {
    static const std::vector<std::string> k = {"paragraph", "heading", "list", "numbered", "table", "image", "code", "quote", "pagebreak", "markdown"};
    return k;
}

struct PaperSize {
    const char* name;
    double w, h;
};
const std::vector<PaperSize>& papers() {
    static const std::vector<PaperSize> p = {{"a4", 595.28, 841.89}, {"letter", 612, 792}, {"legal", 612, 1008}, {"a5", 419.53, 595.28}, {"a3", 841.89, 1190.55}};
    return p;
}
std::vector<std::string> paperNames() {
    std::vector<std::string> n;
    for (const auto& p : papers()) n.push_back(p.name);
    return n;
}

bool parseFamily(const std::string& s, pdf::Family& f) {
    const std::string l = lower(s);
    if (l == "sans" || l == "helvetica" || l == "arial") f = pdf::Family::Sans;
    else if (l == "serif" || l == "times") f = pdf::Family::Serif;
    else if (l == "mono" || l == "courier") f = pdf::Family::Mono;
    else return false;
    return true;
}

const char* familyName(pdf::Family f) { return f == pdf::Family::Serif ? "serif" : f == pdf::Family::Mono ? "mono" : "sans"; }

// Text with hard line breaks: each line is inline Markdown.
Runs inlineLines(const std::string& text) {
    Runs out;
    auto lines = splitLines(text);
    for (std::size_t i = 0; i < lines.size(); ++i) {
        if (i) {
            Run br;
            br.text = "\n";
            out.push_back(br);
        }
        Runs r = parseInline(lines[i]);
        out.insert(out.end(), r.begin(), r.end());
    }
    mergeRuns(out);
    return out;
}

std::vector<std::string> splitCells(const std::string& line) {
    std::vector<std::string> cells;
    std::string t = trimCopy(line);
    if (t.find('|') != std::string::npos) {
        if (!t.empty() && t.front() == '|') t = t.substr(1);
        if (!t.empty() && t.back() == '|') t.pop_back();
        std::string cur;
        for (std::size_t i = 0; i < t.size(); ++i) {
            if (t[i] == '\\' && i + 1 < t.size() && t[i + 1] == '|') {
                cur += '|';
                ++i;
            } else if (t[i] == '|') {
                cells.push_back(trimCopy(cur));
                cur.clear();
            } else {
                cur += t[i];
            }
        }
        cells.push_back(trimCopy(cur));
        return cells;
    }
    const char sep = t.find('\t') != std::string::npos ? '\t' : ',';
    std::string cur;
    bool quoted = false;
    for (std::size_t i = 0; i < t.size(); ++i) {
        char c = t[i];
        if (quoted) {
            if (c == '"' && i + 1 < t.size() && t[i + 1] == '"') {
                cur += '"';
                ++i;
            } else if (c == '"') {
                quoted = false;
            } else {
                cur += c;
            }
        } else if (c == '"' && trimCopy(cur).empty()) {
            quoted = true;
            cur.clear();
        } else if (c == sep) {
            cells.push_back(trimCopy(cur));
            cur.clear();
        } else {
            cur += c;
        }
    }
    cells.push_back(trimCopy(cur));
    return cells;
}

bool separatorRow(const std::string& line) {
    std::string t = trimCopy(line);
    if (t.empty()) return false;
    for (char c : t) {
        if (c != '|' && c != '-' && c != ':' && c != ' ') return false;
    }
    return t.find('-') != std::string::npos;
}


// A block's content the way doc_edit text= takes it back.
std::string editableText(const Block& b) {
    if (b.kind == Block::List) return bulletText(b.items, b.ordered);
    if (b.kind == Block::Table) {
        std::string t;
        for (const auto& row : b.rows) {
            std::string line;
            for (const auto& c : row) line += (line.empty() ? "| " : " | ") + inlineMarkdown(c);
            t += line + " |\n";
        }
        return t;
    }
    if (b.kind == Block::Code) return plainText(b.runs);
    return inlineMarkdown(b.runs);
}

// Builds the blocks for doc_add / doc_edit from kind + text (+ image path).
bool makeBlocks(const std::string& kind, const Params& p, std::vector<Block>& out, std::string& error) {
    const std::string text = textParam(p, "text", kind != "code");
    Block b;
    if (kind == "markdown") {
        TextDocument d = readMarkdown(text, fs::current_path().string());
        out = d.blocks;
        if (out.empty()) error = "the Markdown text has no blocks";
        return !out.empty();
    }
    if (kind == "pagebreak") {
        b.kind = Block::PageBreak;
    } else if (kind == "image") {
        if (!has(p, "path")) {
            error = "kind=image needs path=<png/jpg file>";
            return false;
        }
        setImage(b, readBytes(stringParam(p, "path")));
        b.width = numberParam(p, "width", 0);
        b.alt = has(p, "alt") ? stringParam(p, "alt") : text;
    } else {
        if (!hasText(p, "text")) {
            error = "kind=" + kind + " needs text=";
            return false;
        }
        if (kind == "heading") {
            b.kind = Block::Heading;
            b.level = static_cast<int>(std::clamp<std::int64_t>(intParam(p, "level", 1), 1, 6));
            std::string t = trimCopy(text);
            while (!t.empty() && t[0] == '#') t = trimCopy(t.substr(1));
            b.runs = parseInline(t);
        } else if (kind == "paragraph") {
            b.kind = Block::Paragraph;
            b.runs = inlineLines(text);
        } else if (kind == "quote") {
            b.kind = Block::Quote;
            std::string t;
            for (const auto& l : splitLines(text)) t += (t.empty() ? "" : "\n") + (startsWith(trimCopy(l), ">") ? trimCopy(trimCopy(l).substr(1)) : l);
            b.runs = inlineLines(t);
        } else if (kind == "code") {
            b.kind = Block::Code;
            Run r;
            r.text = text;
            r.code = true;
            b.runs.push_back(r);
        } else if (kind == "list" || kind == "numbered") {
            b.kind = Block::List;
            b.ordered = kind == "numbered";
            b.items = parseBullets(text);
            if (b.items.empty()) {
                error = "no list items in text (one per line)";
                return false;
            }
            if (has(p, "numbering")) {
                const std::string n = stringParam(p, "numbering");
                b.ordered = n != "bullet";
                b.numbering = n == "bullet" || n == "decimal" ? "" : n;
            }
            if (has(p, "start")) b.start = static_cast<int>(std::clamp<std::int64_t>(intParam(p, "start"), 0, 100000));
        } else if (kind == "table") {
            b.kind = Block::Table;
            b.header = boolParam(p, "header", true);
            for (const auto& line : splitLines(text)) {
                if (trimCopy(line).empty() || separatorRow(line)) continue;
                std::vector<Runs> row;
                for (const auto& c : splitCells(line)) row.push_back(parseInline(c));
                b.rows.push_back(std::move(row));
            }
            if (b.rows.empty()) {
                error = "no table rows in text (one row per line, cells split by | or tabs or commas)";
                return false;
            }
        } else {
            error = "unknown kind " + kind;
            return false;
        }
    }
    out.push_back(std::move(b));
    return true;
}

ParamSpec numberingSpec() {
    return str("numbering", "list numbering", false, {}, {"bullet", "decimal", "lowerLetter", "upperLetter", "lowerRoman", "upperRoman"});
}

std::string clip(const std::string& s, std::size_t n) {
    if (s.size() <= n) return s;
    std::size_t cut = n;
    while (cut > 0 && (static_cast<unsigned char>(s[cut]) & 0xC0) == 0x80) --cut;
    return s.substr(0, cut) + "...";
}

// Line breaks become " / ", tabs " | ", runs of spaces one space.
std::string oneLine(const std::string& s) {
    std::string out;
    for (char c : s) {
        if (c == '\r') continue;
        if (c == '\n' || c == '\t') {
            while (!out.empty() && out.back() == ' ') out.pop_back();
            if (!out.empty()) out += c == '\n' ? " / " : " | ";
        } else if (c != ' ' || (!out.empty() && out.back() != ' ')) {
            out += c;
        }
    }
    while (!out.empty() && out.back() == ' ') out.pop_back();
    return out;
}

std::string describe(const Block& b, std::size_t index) {
    std::string kind = kindName(b.kind);
    if (b.kind == Block::Heading) kind += " " + std::to_string(b.level);
    if (b.kind == Block::List) kind = b.ordered ? "numbered list" : "list";
    std::string s = std::to_string(index + 1) + ". " + kind;
    switch (b.kind) {
        case Block::List: s += " (" + std::to_string(b.items.size()) + " items)"; break;
        case Block::Table: {
            std::size_t cols = 0;
            for (const auto& r : b.rows) cols = std::max(cols, r.size());
            s += " (" + std::to_string(b.rows.size()) + "x" + std::to_string(cols) + ")";
            break;
        }
        case Block::Image:
            if (b.pixels) s += " (" + std::to_string(b.pixels->pixels.width()) + "x" + std::to_string(b.pixels->pixels.height()) + " px)";
            break;
        default: break;
    }
    std::string t = oneLine(b.kind == Block::Image ? b.alt : blockText(b));
    if (!t.empty()) s += ": " + clip(t, 90);
    return s;
}

// Markdown of one block (for get_text).
std::string blockMarkdown(const Block& b) {
    TextDocument one;
    one.blocks.push_back(b);
    std::vector<MediaFile> media;
    std::string md = writeMarkdown(one, "media/", media);
    while (!md.empty() && md.back() == '\n') md.pop_back();
    return md;
}

struct Counts {
    long words = 0, chars = 0, charsNoSpaces = 0, paragraphs = 0;
};

void countText(const std::string& t, Counts& c) {
    bool inWord = false;
    for (unsigned cp : pdf::utf8Decode(t)) {
        ++c.chars;
        const bool space = cp == ' ' || cp == '\n' || cp == '\t' || cp == '\r' || cp == 0xA0;
        if (!space) ++c.charsNoSpaces;
        if (!space && !inWord) ++c.words;
        inWord = !space;
    }
}

Counts countDocument(const TextDocument& d) {
    Counts c;
    for (const auto& b : d.blocks) {
        switch (b.kind) {
            case Block::List:
                for (const auto& it : b.items) {
                    countText(plainText(it.runs), c);
                    ++c.paragraphs;
                }
                break;
            case Block::Table:
                for (const auto& r : b.rows)
                    for (const auto& cell : r) countText(plainText(cell), c);
                ++c.paragraphs;
                break;
            case Block::Heading:
            case Block::Paragraph:
            case Block::Quote:
            case Block::Code:
                countText(plainText(b.runs), c);
                ++c.paragraphs;
                break;
            default: break;
        }
    }
    return c;
}

bool blockArg(const TextDocument& d, const Params& p, const char* key, std::size_t& index, std::string& error) {
    const std::int64_t n = intParam(p, key, 0);
    const auto size = static_cast<std::int64_t>(d.blocks.size());
    if (n < 1 || n > size) {
        error = d.blocks.empty() ? std::string("the document is empty") : std::string(key) + " must be 1.." + std::to_string(size) + " (office.doc_info lists the blocks)";
        return false;
    }
    index = static_cast<std::size_t>(n - 1);
    return true;
}

std::string marginsText(const PageSetup& pg) {
    auto mm = [](double v) { return textNumber(std::round(v * 25.4 / 72 * 10) / 10); };
    if (pg.top == pg.bottom && pg.top == pg.left && pg.top == pg.right) return mm(pg.top) + " mm";
    return mm(pg.top) + "/" + mm(pg.right) + "/" + mm(pg.bottom) + "/" + mm(pg.left) + " mm (top/right/bottom/left)";
}

bool applySetup(TextDocument& d, const Params& p, std::string& error) {
    if (has(p, "page")) {
        const std::string name = lower(stringParam(p, "page"));
        bool found = false;
        for (const auto& ps : papers()) {
            if (name == ps.name) {
                d.page.width = ps.w;
                d.page.height = ps.h;
                found = true;
            }
        }
        if (!found) {
            error = "unknown page size " + name;
            return false;
        }
    }
    if (has(p, "landscape")) {
        const double a = std::min(d.page.width, d.page.height), b = std::max(d.page.width, d.page.height);
        d.page.width = boolParam(p, "landscape") ? b : a;
        d.page.height = boolParam(p, "landscape") ? a : b;
    }
    PageSetup pg = d.page;
    if (has(p, "margin")) pg.top = pg.bottom = pg.left = pg.right = numberParam(p, "margin") * 72 / 25.4;
    const std::pair<const char*, double*> sides[] = {{"margin_top", &pg.top}, {"margin_bottom", &pg.bottom}, {"margin_left", &pg.left}, {"margin_right", &pg.right}};
    for (const auto& [key, v] : sides) {
        if (has(p, key)) *v = numberParam(p, key) * 72 / 25.4;
    }
    if (!(pg.top >= 0 && pg.bottom >= 0 && pg.left >= 0 && pg.right >= 0 && pg.left + pg.right < pg.width - 72 && pg.top + pg.bottom < pg.height - 72)) {
        error = "margins are out of range for this page (at least 1 inch must be left for text)";
        return false;
    }
    d.page.top = pg.top, d.page.bottom = pg.bottom, d.page.left = pg.left, d.page.right = pg.right;
    if (has(p, "font") && !parseFamily(stringParam(p, "font"), d.page.font)) {
        error = "font must be sans, serif or mono";
        return false;
    }
    if (has(p, "font_size")) {
        const double s = numberParam(p, "font_size");
        if (!(s >= 5 && s <= 36)) {
            error = "font_size must be 5..36 pt";
            return false;
        }
        d.page.fontSize = s;
    }
    if (has(p, "page_numbers")) d.page.pageNumbers = boolParam(p, "page_numbers");
    if (has(p, "title")) d.title = stringParam(p, "title");
    if (has(p, "author")) d.author = stringParam(p, "author");
    return true;
}

std::vector<ParamSpec> setupSpecs() {
    return {str("page", "paper size", false, {}, paperNames()), flag("landscape", "landscape orientation"),
            number("margin", "page margin in mm (all sides, default 25.4)"), number("margin_top", "top margin in mm"),
            number("margin_bottom", "bottom margin in mm"), number("margin_left", "left margin in mm"), number("margin_right", "right margin in mm"), str("font", "body font", false, {}, {"sans", "serif", "mono"}),
            number("font_size", "body text size in pt (default 11)"), flag("page_numbers", "\"n / N\" page numbers in the footer"),
            str("title", "document title (file metadata)"), str("author", "author (file metadata)")};
}

std::vector<ParamSpec> join(std::vector<ParamSpec> a, const std::vector<ParamSpec>& b) {
    a.insert(a.end(), b.begin(), b.end());
    return a;
}

Result exportTo(TextDocument& d, const Params& p, const std::string& path) {
    const std::string ext = extOf(path);
    if (ext == "png" || ext == "jpg" || ext == "jpeg") {
        auto pages = layoutDocument(d);
        const int n = static_cast<int>(intParam(p, "page", 1));
        if (n < 1 || n > static_cast<int>(pages.size())) return Result::failure("page must be 1.." + std::to_string(pages.size()));
        const double dpi = std::clamp(numberParam(p, "dpi", 110), 20.0, 600.0);
        raster::Image img = pdf::renderPage(pages[static_cast<std::size_t>(n - 1)], dpi / 72.0);
        writeBytes(path, ext == "png" ? raster::encodePng(img) : raster::encodeJpeg(img, 90));
        return Result::success("exported page " + std::to_string(n) + " of " + std::to_string(pages.size()) + " to " + path, path).with("path", path);
    }
    saveTextDocument(d, path);
    std::string note;
    if (ext == "pdf") note = " (" + std::to_string(layoutDocument(d).size()) + " pages)";
    return Result::success("exported " + path + note, path).with("path", path);
}

}  // namespace

static void registerDocToolCommands(Session& session);


namespace {

// Which character of a block's editable text (inline Markdown and all) sits at a point on its page,
// read off the same layout the page picture is drawn from, so a click puts the caret on that letter.
// `beforeText`: the click was left of the line's first word (on a bullet or task box).
int charAtPoint(const TextDocument& doc, const BlockBox& box, double x, double y, const std::string& editable, bool* beforeText = nullptr) {
    const auto pages = layoutDocument(doc);
    if (box.page < 1 || box.page > static_cast<int>(pages.size())) return static_cast<int>(editable.size());
    struct Line { double y = 0, size = 0; std::vector<const pdf::TextOp*> ops; };
    std::vector<Line> lines;
    for (const auto& op : pages[static_cast<std::size_t>(box.page - 1)].ops) {
        const auto* t = std::get_if<pdf::TextOp>(&op);
        if (!t || t->text.empty() || t->y < box.y0 - 1 || t->y - t->size * 0.8 > box.y1 + 1) continue;
        auto it = std::find_if(lines.begin(), lines.end(), [&](const Line& l) { return std::abs(l.y - t->y) < 0.5; });
        if (it == lines.end()) lines.push_back({t->y, t->size, {}}), it = lines.end() - 1;
        it->size = std::max(it->size, t->size);
        it->ops.push_back(t);
    }
    if (lines.empty()) return static_cast<int>(editable.size());
    std::sort(lines.begin(), lines.end(), [](const Line& a, const Line& b) { return a.y < b.y; });
    // the click's line, then its letter: count plain characters before it
    std::size_t li = 0;
    for (std::size_t i = 0; i < lines.size(); ++i)
        if (y > lines[i].y - lines[i].size * 1.0) li = i;
    auto utf8Len = [](const std::string& s) {
        std::size_t n = 0;
        for (unsigned char c : s) n += (c & 0xC0) != 0x80;
        return n;
    };
    std::size_t plain = 0;
    for (std::size_t i = 0; i < li; ++i) {
        auto row = lines[i].ops;
        std::sort(row.begin(), row.end(), [](const pdf::TextOp* a, const pdf::TextOp* b) { return a->x < b->x; });
        double end = -1;
        for (const auto* t : row) {
            if (end >= 0 && t->x > end + 0.05) plain += 1;
            plain += utf8Len(t->text);
            end = t->x + pdf::textWidth(t->font, t->text, t->size);
        }
        plain += 1;  // the space the wrap ate
    }
    auto& ops = lines[li].ops;
    std::sort(ops.begin(), ops.end(), [](const pdf::TextOp* a, const pdf::TextOp* b) { return a->x < b->x; });
    if (beforeText) *beforeText = !ops.empty() && x < ops.front()->x - 1;
    // words are separate runs; a gap between two runs is the space the layout left out
    double prevEnd = -1;
    for (const auto* t : ops) {
        const bool gap = prevEnd >= 0 && t->x > prevEnd + 0.05;
        if (x <= t->x) {
            if (gap && x - prevEnd < (t->x - prevEnd) / 2) break;  // nearer the end of the last word
            plain += gap ? 1 : 0;
            break;
        }
        plain += gap ? 1 : 0;
        // walk the run letter by letter to the nearest edge
        std::size_t k = 0, best = 0;
        double bestD = std::abs(x - t->x);
        for (std::size_t pos = 0; pos <= t->text.size();) {
            const double edge = t->x + pdf::textWidth(t->font, t->text.substr(0, pos), t->size);
            if (std::abs(x - edge) < bestD) bestD = std::abs(x - edge), best = k;
            if (pos == t->text.size()) break;
            ++pos;
            while (pos < t->text.size() && (static_cast<unsigned char>(t->text[pos]) & 0xC0) == 0x80) ++pos;
            ++k;
        }
        plain += best;
        const std::size_t len = utf8Len(t->text);
        if (best < len) break;
        prevEnd = t->x + pdf::textWidth(t->font, t->text, t->size);
    }
    // plain letters -> the editable text, stepping over Markdown markers
    std::size_t i = 0, seen = 0;
    while (i < editable.size()) {
        if (i == 0 || editable[i - 1] == '\n') {  // a list line: its indent isn't drawn, nor a task's "- [ ] " (a box is)
            while (i < editable.size() && editable[i] == ' ') ++i;
            if (editable.compare(i, 6, "- \xE2\x98\x90 ") == 0 || editable.compare(i, 6, "- \xE2\x98\x91 ") == 0) i += 6;
            if (i >= editable.size()) break;
        }
        if (editable.compare(i, 3, "<u>") == 0) { i += 3; continue; }
        if (editable.compare(i, 4, "</u>") == 0) { i += 4; continue; }
        if (editable.compare(i, 2, "~~") == 0) { i += 2; continue; }
        const char c = editable[i];
        const bool wiki = c == '[' && (editable.compare(i, 2, "[[") == 0 || (i > 0 && editable[i - 1] == '['));  // [[note]] is drawn as is
        if (c == '*' || c == '`' || (c == '[' && !wiki)) { ++i; continue; }
        if (c == ']' && i + 1 < editable.size() && editable[i + 1] == '(') {
            const auto close = editable.find(')', i);
            i = close == std::string::npos ? editable.size() : close + 1;
            continue;
        }
        if (seen == plain) break;
        ++i;
        while (i < editable.size() && (static_cast<unsigned char>(editable[i]) & 0xC0) == 0x80) ++i;
        ++seen;
    }
    return static_cast<int>(i);
}

}  // namespace

void registerDocCommands(Session& session) {
    auto& reg = session.registry();

    reg.add({"office.doc_new", "New Document",
             "Create a document and return its id (doc1, ...). markdown= fills it straight away (# headings, **bold**, *italic*, "
             "[links](url), - lists, 1. numbered lists, | tables |, > quotes, ``` code, ![images](file.png), <!-- pagebreak -->).",
             join({str("markdown", "initial content as Markdown"), str("markdown_file", "read the Markdown from this file (images relative to it)")}, setupSpecs()), false, true},
            [&session](const Params& p) -> std::unique_ptr<Action> {
                return std::make_unique<FunctionAction>([&session, p]() -> Result {
                    try {
                        auto d = std::make_shared<TextDoc>();
                        if (hasText(p, "markdown")) {
                            std::string base = fs::current_path().string();
                            if (!has(p, "markdown")) base = pathFromUtf8(stringParam(p, "markdown_file")).parent_path().string();
                            TextDocument md = readMarkdown(textParam(p, "markdown"), base);
                            d->data.blocks = md.blocks;
                            d->data.title = md.title;
                            d->data.author = md.author;
                        }
                        std::string error;
                        if (!applySetup(d->data, p, error)) return Result::failure(error);
                        return created(session, d, "created");
                    } catch (const std::exception& e) {
                        return Result::failure(e.what());
                    }
                });
            });

    reg.add({"office.doc_open", "Open Document",
             "Open a document: .docx and .odt (styles, run and paragraph formats, lists, tables with merged cells, images, headers "
             "and footers, footnotes, comments, tracked changes, table of contents), .md, .html, .txt; .doc, .rtf and others through LibreOffice.",
             {str("path", "file to open", true)}, false, true},
            [&session](const Params& p) -> std::unique_ptr<Action> {
                return std::make_unique<FunctionAction>([&session, p]() -> Result {
                    try {
                        auto d = std::make_shared<TextDoc>();
                        d->path = stringParam(p, "path");
                        d->data = loadTextDocument(d->path);
                        return created(session, d, "opened");
                    } catch (const std::exception& e) {
                        return Result::failure(e.what());
                    }
                });
            });

    reg.add({"office.doc_save", "Save Document",
             "Save the document; the extension picks the format: .docx (Word), .md (images go to <name>_media/), .html (one "
             "self-contained file), .odt (OpenDocument; tracked changes are saved accepted), .txt, .pdf; .rtf, .doc through LibreOffice. "
             "Without path it saves back to its own file.",
             {doc(), str("path", "output file (default: the document's own path)")}, false, false},
            read(session, [](TextDoc& d, const Params& p, const std::string& id) -> Result {
                const std::string path = has(p, "path") ? stringParam(p, "path") : d.path;
                if (path.empty()) return Result::failure(id + " has never been saved; give path=<file.docx>");
                saveTextDocument(d.data, path);
                const std::string ext = extOf(path);
                if (ext != "pdf" && ext != "txt") d.path = path;
                return Result::success("saved " + path, path).with("path", path);
            }));

    reg.add({"office.doc_export", "Export Document",
             "Export to another file without changing which file the document is saved to: .pdf (laid out here: fonts, word wrap, "
             "lists, tables, images, headers, footers, footnotes, table of contents), .docx, .odt, .md, .html, .txt, .png/.jpg (one page, "
             "page= and dpi=); .rtf/.doc through LibreOffice.",
             {doc(), fileSpec("document"), str("path", "output file; the extension picks the format", true), integer("page", "png/jpg: which page", false, std::int64_t{1}),
              number("dpi", "png/jpg resolution", false, 110.0)},
             false, false},
            read(session, [](TextDoc& d, const Params& p, const std::string&) { return exportTo(d.data, p, stringParam(p, "path")); }));

    reg.add({"office.doc_close", "Close Document", "Close a document without saving (undo reopens it).", {doc()}, true},
            [&session](const Params& p) -> std::unique_ptr<Action> {
                std::string id, error;
                if (!findDoc<TextDoc>(session, p, id, error)) return std::make_unique<FunctionAction>([error] { return Result::failure(error); });
                return std::make_unique<CloseAction>(session, id);
            });

    reg.add({"office.doc_info", "Document Info",
             "Describe a document: title, author, page setup, page count, word count and every block with its number (the numbers "
             "doc_edit, doc_delete, doc_move and doc_add at= use).",
             {doc(), fileSpec("document"), integer("max_blocks", "list at most this many blocks", false, std::int64_t{200})}, false, true},
            read(session, [](TextDoc& d, const Params& p, const std::string& id) -> Result {
                const auto pages = layoutDocument(d.data);
                const Counts c = countDocument(d.data);
                std::string paper = "custom";
                for (const auto& ps : papers()) {
                    if ((std::fabs(ps.w - d.data.page.width) < 1 && std::fabs(ps.h - d.data.page.height) < 1) ||
                        (std::fabs(ps.h - d.data.page.width) < 1 && std::fabs(ps.w - d.data.page.height) < 1))
                        paper = ps.name;
                }
                std::string t = id + ": " + (d.data.title.empty() ? std::string("(no title)") : "\"" + d.data.title + "\"") +
                                (d.data.author.empty() ? "" : " by " + d.data.author) + (d.path.empty() || d.path == id ? "" : ", " + d.path);
                t += "\n" + paper + (d.data.page.width > d.data.page.height ? " landscape" : "") + ", margins " + marginsText(d.data.page) + ", " + familyName(d.data.page.font) + " " + textNumber(d.data.page.fontSize) + " pt; " + std::to_string(pages.size()) + " page(s), " +
                     std::to_string(c.words) + " words, " + std::to_string(d.data.blocks.size()) + " blocks:";
                const auto maxBlocks = static_cast<std::size_t>(std::max<std::int64_t>(1, intParam(p, "max_blocks", 200)));
                for (std::size_t i = 0; i < d.data.blocks.size() && i < maxBlocks; ++i) t += "\n  " + describe(d.data.blocks[i], i);
                if (d.data.blocks.size() > maxBlocks) t += "\n  ...";
                return Result::success(t)
                    .with("doc", id)
                    .with("blocks", static_cast<std::int64_t>(d.data.blocks.size()))
                    .with("pages", static_cast<std::int64_t>(pages.size()))
                    .with("words", static_cast<std::int64_t>(c.words))
                    .with("title", d.data.title);
            }));

    reg.add({"office.doc_get_text", "Get Text",
             "Read the document's content as Markdown (default) or plain text, each block prefixed with its number [n] unless "
             "numbered=false. from/to pick a block range.",
             {doc(), fileSpec("document"), str("format", "markdown keeps styles, text is plain", false, std::string("markdown"), {"markdown", "text"}),
              flag("numbered", "prefix each block with [n]", true), integer("from", "first block (1-based)"), integer("to", "last block")},
             false, true},
            read(session, [](TextDoc& d, const Params& p, const std::string&) -> Result {
                const bool md = stringParam(p, "format", "markdown") == "markdown", numbered = boolParam(p, "numbered", true);
                const std::size_t n = d.data.blocks.size();
                std::size_t from = static_cast<std::size_t>(std::clamp<std::int64_t>(intParam(p, "from", 1), 1, std::max<std::int64_t>(1, static_cast<std::int64_t>(n))));
                std::size_t to = static_cast<std::size_t>(std::clamp<std::int64_t>(intParam(p, "to", static_cast<std::int64_t>(n)), 0, static_cast<std::int64_t>(n)));
                std::string out;
                for (std::size_t i = from - 1; i < to; ++i) {
                    const Block& b = d.data.blocks[i];
                    std::string body = md ? blockMarkdown(b) : (b.kind == Block::Image ? "[image: " + b.alt + "]" : b.kind == Block::PageBreak ? "[page break]" : blockText(b));
                    if (!out.empty()) out += "\n\n";
                    out += (numbered ? "[" + std::to_string(i + 1) + "] " : "") + body;
                }
                if (out.empty()) out = "(the document is empty)";
                return Result::success(out, out);
            }));

    reg.add({"office.doc_add", "Add Block",
             "Add content. kind=paragraph|heading|quote take inline Markdown text (**bold**, *italic*, <u>underline</u>, `code`, "
             "[link](url)); list/numbered take one item per line (indent 2 spaces for sub-items); table takes one row per line with "
             "cells split by |, tabs or commas (first row is the header); code keeps text as-is; image needs path=; pagebreak needs "
             "nothing; markdown adds every block of a Markdown text. at= inserts before that block number (default: at the end).",
             {doc(), str("kind", "what to add", false, std::string("paragraph"), blockKinds()), str("text", "the content"), str("text_file", "read the content from this file"),
              integer("level", "heading level 1-6", false, std::int64_t{1}), integer("at", "insert before this block number (default: append)"),
              str("path", "image file (kind=image)"), number("width", "image width in pt (default: natural size, at most the text width)"),
              str("alt", "image description"), flag("header", "table: the first row is a header", true), numberingSpec(),
              integer("start", "numbered list: first number (default 1)"), previewSpec()},
             true},
            edit(session, [](TextDoc& d, const Params& p, const std::string&) -> Result {
                std::vector<Block> blocks;
                std::string error;
                if (!makeBlocks(stringParam(p, "kind", "paragraph"), p, blocks, error)) return Result::failure(error);
                if (d.data.trackChanges) {  // the new text is an insertion
                    const int id = doccmd::newChange(d.data, true);
                    for (auto& b : blocks) doccmd::markBlock(d.data, b, id);
                }
                std::size_t at = d.data.blocks.size();
                if (has(p, "at")) {
                    const std::int64_t a = intParam(p, "at");
                    if (a < 1 || a > static_cast<std::int64_t>(d.data.blocks.size()) + 1)
                        return Result::failure("at must be 1.." + std::to_string(d.data.blocks.size() + 1));
                    at = static_cast<std::size_t>(a - 1);
                }
                d.data.blocks.insert(d.data.blocks.begin() + static_cast<std::ptrdiff_t>(at), blocks.begin(), blocks.end());
                std::string msg = blocks.size() == 1 ? "added " + describe(d.data.blocks[at], at)
                                                     : "added " + std::to_string(blocks.size()) + " blocks (" + std::to_string(at + 1) + "-" + std::to_string(at + blocks.size()) + ")";
                Result r = Result::success(msg, static_cast<std::int64_t>(at + 1));
                r.with("block", static_cast<std::int64_t>(at + 1)).with("blocks", static_cast<std::int64_t>(d.data.blocks.size()));
                maybePreview(r, d, p);
                return r;
            }));

    reg.add({"office.doc_edit", "Edit Block",
             "Change a block: new text (same rules as doc_add for its kind), a different kind (e.g. paragraph -> heading), a heading "
             "level, or for images a new path/width/alt. Fields left out stay as they are.",
             {doc(), integer("block", "block number (office.doc_info lists them)", true), str("text", "new content"), str("text_file", "read the new content from this file"),
              str("kind", "change the block into this kind", false, {}, blockKinds()), integer("level", "heading level 1-6"),
              str("path", "image file"), number("width", "image width in pt"), str("alt", "image description"), flag("header", "table: first row is a header"),
              numberingSpec(), integer("start", "numbered list: first number"), previewSpec()},
             true},
            edit(session, [](TextDoc& d, const Params& p, const std::string&) -> Result {
                std::size_t i;
                std::string error;
                if (!blockArg(d.data, p, "block", i, error)) return Result::failure(error);
                Block& b = d.data.blocks[i];
                std::string kind = has(p, "kind") ? stringParam(p, "kind") : std::string(kindName(b.kind));
                if (!has(p, "kind")) {
                    if (b.kind == Block::List) kind = b.ordered ? "numbered" : "list";
                    else if (b.kind == Block::PageBreak) kind = "pagebreak";
                }
                if (kind == "markdown") return Result::failure("use doc_delete + doc_add kind=markdown to replace a block with Markdown");
                if (hasText(p, "text") || has(p, "kind") || (kind == "image" && has(p, "path"))) {
                    Params q = p;
                    if (!hasText(p, "text")) q["text"] = editableText(b);  // converting kinds keeps the text
                    if (kind == "heading" && !has(p, "level")) q["level"] = static_cast<std::int64_t>(b.kind == Block::Heading ? b.level : 1);
                    if (kind == "image" && !has(p, "path")) {
                        if (b.kind != Block::Image) return Result::failure("kind=image needs path=");
                        if (has(p, "alt")) b.alt = stringParam(p, "alt");
                        if (has(p, "width")) b.width = numberParam(p, "width");
                    } else {
                        if (kind == "table" && !has(p, "header")) q["header"] = b.kind == Block::Table ? b.header : true;
                        if (b.kind == Block::List && (kind == "list" || kind == "numbered")) {
                            if (!has(p, "numbering") && kind == (b.ordered ? "numbered" : "list") && !b.numbering.empty()) q["numbering"] = b.numbering;
                            if (!has(p, "start")) q["start"] = static_cast<std::int64_t>(b.start);
                        }
                        std::vector<Block> made;
                        if (!makeBlocks(kind, q, made, error)) return Result::failure(error);
                        const bool textKind = made.front().kind == Block::Paragraph || made.front().kind == Block::Heading || made.front().kind == Block::Quote;
                        if (d.data.trackChanges && textKind && b.kind == made.front().kind && hasText(p, "text")) {
                            // the edit is recorded: what changed is deleted, the new text inserted
                            const int del = doccmd::newChange(d.data, false), ins = doccmd::newChange(d.data, true);
                            Runs merged = doccmd::trackedReplace(d.data, b.runs, made.front().runs, del, ins);
                            made.front().runs = merged;
                            made.front().fmt = b.fmt;
                            made.front().style = b.style;
                            d.data.changes.erase(std::remove_if(d.data.changes.begin(), d.data.changes.end(),
                                                                [&](const TrackedChange& c) {
                                                                    if (c.id != del && c.id != ins) return false;
                                                                    for (const auto& r : merged) {
                                                                        if (r.change == c.id) return false;
                                                                    }
                                                                    return true;
                                                                }),
                                                 d.data.changes.end());
                        } else if (d.data.trackChanges) {
                            doccmd::markBlock(d.data, made.front(), doccmd::newChange(d.data, true));
                        }
                        made.front().fmt = made.front().kind == b.kind ? b.fmt : made.front().fmt;
                        if (made.front().kind == b.kind && made.front().style.empty()) made.front().style = b.style;
                        b = made.front();
                    }
                } else {
                    if (has(p, "level")) {
                        if (b.kind != Block::Heading) return Result::failure("level only applies to headings (add kind=heading)");
                        b.level = static_cast<int>(std::clamp<std::int64_t>(intParam(p, "level"), 1, 6));
                    }
                    if (has(p, "alt")) b.alt = stringParam(p, "alt");
                    if (has(p, "width")) b.width = numberParam(p, "width");
                    if (has(p, "header")) b.header = boolParam(p, "header");
                    if (b.kind == Block::List && has(p, "numbering")) {
                        const std::string n = stringParam(p, "numbering");
                        b.ordered = n != "bullet";
                        b.numbering = n == "bullet" || n == "decimal" ? "" : n;
                    }
                    if (b.kind == Block::List && has(p, "start")) b.start = static_cast<int>(std::clamp<std::int64_t>(intParam(p, "start"), 0, 100000));
                }
                Result r = Result::success("block " + describe(b, i));
                maybePreview(r, d, p);
                return r;
            }));

    reg.add({"office.doc_delete", "Delete Blocks", "Delete a block, or count blocks starting there.",
             {doc(), integer("block", "first block to delete", true), integer("count", "how many", false, std::int64_t{1}), previewSpec()}, true},
            edit(session, [](TextDoc& d, const Params& p, const std::string&) -> Result {
                std::size_t i;
                std::string error;
                if (!blockArg(d.data, p, "block", i, error)) return Result::failure(error);
                const auto count = static_cast<std::size_t>(std::clamp<std::int64_t>(intParam(p, "count", 1), 1, static_cast<std::int64_t>(d.data.blocks.size() - i)));
                std::string first = describe(d.data.blocks[i], i);
                if (d.data.trackChanges) {
                    // text is marked deleted (accept removes it); images, page breaks and contents go now
                    const int id = doccmd::newChange(d.data, false);
                    std::vector<Block> kept;
                    for (std::size_t k = 0; k < d.data.blocks.size(); ++k) {
                        Block& b = d.data.blocks[k];
                        if (k >= i && k < i + count && !doccmd::markBlock(d.data, b, id)) continue;
                        kept.push_back(std::move(b));
                    }
                    d.data.blocks = std::move(kept);
                    Result r = Result::success("marked " + (count == 1 ? first : std::to_string(count) + " blocks from " + std::to_string(i + 1)) +
                                               " as deleted (tracked change " + std::to_string(id) + "; office.doc_accept removes it)");
                    r.with("blocks", static_cast<std::int64_t>(d.data.blocks.size())).with("change", static_cast<std::int64_t>(id));
                    maybePreview(r, d, p);
                    return r;
                }
                d.data.blocks.erase(d.data.blocks.begin() + static_cast<std::ptrdiff_t>(i), d.data.blocks.begin() + static_cast<std::ptrdiff_t>(i + count));
                Result r = Result::success(count == 1 ? "deleted " + first : "deleted " + std::to_string(count) + " blocks from " + std::to_string(i + 1));
                r.with("blocks", static_cast<std::int64_t>(d.data.blocks.size()));
                maybePreview(r, d, p);
                return r;
            }));

    reg.add({"office.doc_move", "Move Block", "Move a block so it ends up at position `to`.",
             {doc(), integer("block", "block to move", true), integer("to", "its new number", true), previewSpec()}, true},
            edit(session, [](TextDoc& d, const Params& p, const std::string&) -> Result {
                std::size_t i, j;
                std::string error;
                if (!blockArg(d.data, p, "block", i, error) || !blockArg(d.data, p, "to", j, error)) return Result::failure(error);
                Block b = d.data.blocks[i];
                d.data.blocks.erase(d.data.blocks.begin() + static_cast<std::ptrdiff_t>(i));
                d.data.blocks.insert(d.data.blocks.begin() + static_cast<std::ptrdiff_t>(j), b);
                Result r = Result::success("moved block " + std::to_string(i + 1) + " to " + describe(d.data.blocks[j], j));
                maybePreview(r, d, p);
                return r;
            }));

    registerDocToolCommands(session);
    doccmd::registerDocFormatCommands(session);
    doccmd::registerDocTableCommands(session);
    doccmd::registerDocReviewCommands(session);
}

// Find and replace, word count, preview and page setup.
static void registerDocToolCommands(Session& session) {
    auto& reg = session.registry();

    reg.add({"office.doc_find_replace", "Find and Replace",
             "Find text everywhere in the document (headings, paragraphs, lists, tables, quotes, code) and replace it, keeping each "
             "run's style. Without replace it only counts and lists where the text is. A match must sit inside one style run.",
             {doc(), str("find", "text to find", true), str("replace", "replacement (may be empty)"), flag("match_case", "case-sensitive", false),
              flag("whole_word", "only whole words", false), previewSpec()},
             true},
            edit(session, [](TextDoc& d, const Params& p, const std::string&) -> Result {
                const std::string find = stringParam(p, "find");
                if (find.empty()) return Result::failure("find is empty");
                const bool doReplace = has(p, "replace"), mc = boolParam(p, "match_case", false), whole = boolParam(p, "whole_word", false);
                const std::string rep = stringParam(p, "replace");
                int total = 0;
                std::vector<std::size_t> where;
                auto wordChar = [](char c) { return std::isalnum(static_cast<unsigned char>(c)) || c == '_' || (static_cast<unsigned char>(c) & 0x80); };
                auto inRun = [&](std::string& s) {
                    std::string hay = mc ? s : lower(s), needle = mc ? find : lower(find), out;
                    std::size_t pos = 0, from = 0;
                    int n = 0;
                    for (;;) {
                        std::size_t at = hay.find(needle, from);
                        if (at == std::string::npos) break;
                        const bool okL = !whole || at == 0 || !wordChar(hay[at - 1]);
                        const bool okR = !whole || at + needle.size() >= hay.size() || !wordChar(hay[at + needle.size()]);
                        if (okL && okR) {
                            out += s.substr(pos, at - pos) + rep;
                            pos = at + find.size();
                            ++n;
                        }
                        from = at + std::max<std::size_t>(1, needle.size());
                    }
                    if (n && doReplace) s = out + s.substr(pos);
                    return n;
                };
                const bool track = d.data.trackChanges && doReplace;
                int del = 0, ins = 0;
                auto inRuns = [&](Runs& runs) {
                    int n = 0;
                    if (!track) {
                        for (auto& r : runs) {
                            const TrackedChange* ch = r.change ? d.data.change(r.change) : nullptr;
                            if (!(ch && !ch->insert)) n += inRun(r.text);
                        }
                        return n;
                    }
                    // tracked: each match becomes a deletion followed by the inserted replacement
                    Runs out;
                    for (auto& r : runs) {
                        const TrackedChange* ch = r.change ? d.data.change(r.change) : nullptr;
                        if ((ch && !ch->insert) || r.text.empty()) {
                            out.push_back(r);
                            continue;
                        }
                        if (ch) {  // text that is still a pending insertion is simply changed
                            n += inRun(r.text);
                            out.push_back(r);
                            continue;
                        }
                        Runs one = {r};
                        auto found = doccmd::matches(one, find, mc, whole);
                        if (found.empty()) {
                            out.push_back(r);
                            continue;
                        }
                        if (!del) del = doccmd::newChange(d.data, false), ins = rep.empty() ? 0 : doccmd::newChange(d.data, true);
                        std::size_t pos = 0;
                        for (const auto& [a, b] : found) {
                            Run before = r, gone = r, added = r;
                            before.text = r.text.substr(pos, a - pos);
                            gone.text = r.text.substr(a, b - a);
                            gone.change = del;
                            added.text = rep;
                            added.change = ins;
                            if (!before.text.empty()) out.push_back(before);
                            out.push_back(gone);
                            if (!rep.empty()) out.push_back(added);
                            pos = b;
                            ++n;
                        }
                        Run after = r;
                        after.text = r.text.substr(pos);
                        if (!after.text.empty()) out.push_back(after);
                    }
                    runs = std::move(out);
                    mergeRuns(runs);
                    return n;
                };
                for (std::size_t i = 0; i < d.data.blocks.size(); ++i) {
                    Block& b = d.data.blocks[i];
                    int n = inRuns(b.runs);
                    for (auto& it : b.items) n += inRuns(it.runs);
                    for (auto& row : b.rows)
                        for (auto& c : row) n += inRuns(c);
                    if (!track) n += inRun(b.alt);
                    if (n) where.push_back(i + 1);
                    total += n;
                }
                std::string blocks;
                for (std::size_t i = 0; i < where.size() && i < 30; ++i) blocks += (blocks.empty() ? "" : ", ") + std::to_string(where[i]);
                if (where.size() > 30) blocks += ", ...";
                std::string msg = !total ? "\"" + find + "\" not found"
                                         : std::string(doReplace ? "replaced " : "found ") + std::to_string(total) + " match(es) of \"" + find + "\"" +
                                               (doReplace ? " with \"" + rep + "\"" : "") + " in block(s) " + blocks +
                                               (track ? " (tracked changes " + std::to_string(del) + (ins ? " and " + std::to_string(ins) : std::string()) + ")" : "");
                Result r = Result::success(msg, static_cast<std::int64_t>(total));
                r.with("count", static_cast<std::int64_t>(total));
                maybePreview(r, d, p);
                return r;
            }));

    reg.add({"office.doc_word_count", "Word Count", "Count words, characters (with and without spaces), paragraphs and pages, with a reading time.",
             {doc(), fileSpec("document")}, false, true},
            read(session, [](TextDoc& d, const Params&, const std::string&) -> Result {
                const Counts c = countDocument(d.data);
                const std::size_t pages = layoutDocument(d.data).size();
                const long minutes = std::max(1L, std::lround(static_cast<double>(c.words) / 230.0));
                std::string t = std::to_string(c.words) + " words, " + std::to_string(c.chars) + " characters (" + std::to_string(c.charsNoSpaces) +
                                " without spaces), " + std::to_string(c.paragraphs) + " paragraphs, " + std::to_string(pages) + " page(s); about " +
                                std::to_string(minutes) + " min to read";
                return Result::success(t, static_cast<std::int64_t>(c.words))
                    .with("words", static_cast<std::int64_t>(c.words))
                    .with("characters", static_cast<std::int64_t>(c.chars))
                    .with("characters_no_spaces", static_cast<std::int64_t>(c.charsNoSpaces))
                    .with("paragraphs", static_cast<std::int64_t>(c.paragraphs))
                    .with("pages", static_cast<std::int64_t>(pages));
            }));

    reg.add({"office.doc_hit", "Block At Point",
             "Which block is at a spot on a page (x, y in points from the top left, as doc_preview draws it): its number, kind and "
             "its text the way doc_edit text= takes it. Lets a click on the page edit that block.",
             {doc(), integer("page", "page number", false, std::int64_t{1}), number("x", "points from the left"), number("y", "points from the top"),
              number("sheet_y", "instead of page and y: pixels down the document's picture (its pages one under another)"),
              number("sheet_x", "instead of x: pixels across the document's picture; with x or sheet_x, char= says which character of text is there")},
             false, true},
            read(session, [](TextDoc& d, const Params& p, const std::string&) -> Result {
                int pg = static_cast<int>(intParam(p, "page", 1));
                double y = numberParam(p, "y");
                const double pageStep = d.data.page.height * kSheetScale + kSheetGap;
                if (has(p, "sheet_y")) {
                    const double sy = std::max(0.0, numberParam(p, "sheet_y"));
                    pg = 1 + static_cast<int>(sy / pageStep);
                    y = (sy - (pg - 1) * pageStep) / kSheetScale;
                } else if (!has(p, "y")) {
                    return Result::failure("give y (points from the top of the page) or sheet_y");
                }
                const BlockBox* best = nullptr;
                double bestDist = 1e18;
                const auto boxes = blockBoxes(d.data);
                for (const auto& bx : boxes) {
                    if (bx.page != pg) continue;
                    const double dist = y < bx.y0 ? bx.y0 - y : y > bx.y1 ? y - bx.y1 : 0;
                    if (dist < bestDist) bestDist = dist, best = &bx;
                }
                if (!best || bestDist > 20) return Result::failure("nothing there; click on some text, or add a block");
                const Block& b = d.data.blocks[static_cast<std::size_t>(best->block - 1)];
                std::string kind = kindName(b.kind);
                if (b.kind == Block::List) kind = b.ordered ? "numbered" : "list";
                else if (b.kind == Block::PageBreak) kind = "pagebreak";
                const std::string text = editableText(b);
                Result r = Result::success(describe(b, static_cast<std::size_t>(best->block - 1)), text);
                r.with("block", static_cast<std::int64_t>(best->block)).with("kind", kind).with("text", text)
                 .with("page", static_cast<std::int64_t>(pg)).with("top", best->y0).with("bottom", best->y1)
                 .with("sheet_top", (pg - 1) * pageStep + best->y0 * kSheetScale).with("sheet_bottom", (pg - 1) * pageStep + best->y1 * kSheetScale);
                // where the text column is and how big its words are, so a window can type over the block in place
                double size = d.data.page.fontSize;
                if (b.kind == Block::Heading) size *= b.level <= 1 ? 2.0 : b.level == 2 ? 1.6 : b.level == 3 ? 1.3 : 1.1;
                r.with("sheet_left", d.data.page.left * kSheetScale).with("sheet_right", (d.data.page.width - d.data.page.right) * kSheetScale)
                 .with("font_px", size * kSheetScale).with("level", static_cast<std::int64_t>(b.level));
                if (has(p, "sheet_x") || has(p, "x")) {
                    const double x = has(p, "sheet_x") ? numberParam(p, "sheet_x") / kSheetScale : numberParam(p, "x");
                    bool before = false;
                    const int at = charAtPoint(d.data, *best, x, y, text, &before);
                    r.with("char", static_cast<std::int64_t>(at));
                    // on a notes [[link]]: its name, so a window can follow it
                    {
                        const std::size_t a = static_cast<std::size_t>(at);
                        const auto open = text.rfind("[[", a);
                        const auto close = text.find("]]", open == std::string::npos ? a : open);
                        if (open != std::string::npos && close != std::string::npos && close + 2 >= a && text.find('\n', open) > close) {
                            std::string name = text.substr(open + 2, close - open - 2);
                            name = name.substr(0, name.find_first_of("|#"));
                            if (!name.empty()) r.with("wiki", name);
                        }
                    }
                    // a click on a task box: which item, so a window can tick it
                    if (b.kind == Block::List && before) {
                        const std::size_t ls = text.rfind('\n', at > 0 ? static_cast<std::size_t>(at - 1) : 0);
                        std::size_t start = at == 0 || ls == std::string::npos ? 0 : ls + 1;
                        while (start < text.size() && text[start] == ' ') ++start;
                        if (text.compare(start, 5, "- \xE2\x98\x90") == 0 || text.compare(start, 5, "- \xE2\x98\x91") == 0) {
                            const auto item = std::count(text.begin(), text.begin() + static_cast<std::ptrdiff_t>(start), '\n');
                            r.with("task", static_cast<std::int64_t>(item + 1)).with("done", text.compare(start, 5, "- \xE2\x98\x91") == 0);
                        }
                    }
                }
                return r;
            }));

    reg.add({"office.doc_preview", "Preview Document", "Render one page as a PNG, exactly as the PDF export lays it out.",
             {doc(), fileSpec("document"), integer("page", "page number", false, std::int64_t{1}), number("scale", "pixels per point", false, 1.25)}, false, true},
            read(session, [](TextDoc& d, const Params& p, const std::string&) -> Result {
                auto pages = layoutDocument(d.data);
                const int n = static_cast<int>(intParam(p, "page", 1));
                if (n < 1 || n > static_cast<int>(pages.size())) return Result::failure("page must be 1.." + std::to_string(pages.size()));
                raster::Image img = pdf::renderPage(pages[static_cast<std::size_t>(n - 1)], std::clamp(numberParam(p, "scale", 1.25), 0.25, 4.0));
                Result r = Result::success("page " + std::to_string(n) + " of " + std::to_string(pages.size()));
                r.with("pages", static_cast<std::int64_t>(pages.size()));
                r.attach(png(img, "page" + std::to_string(n) + ".png"));
                return r;
            }));

    reg.add({"office.doc_setup", "Page Setup", "Change the paper size, orientation, margins, body font and size, page numbers, title or author.",
             join({doc()}, join(setupSpecs(), {previewSpec()})), true},
            edit(session, [](TextDoc& d, const Params& p, const std::string&) -> Result {
                std::string error;
                if (!applySetup(d.data, p, error)) return Result::failure(error);
                Result r = Result::success("page setup: " + textNumber(std::round(d.data.page.width)) + " x " + textNumber(std::round(d.data.page.height)) +
                                           " pt, margins " + marginsText(d.data.page) + ", " +
                                           familyName(d.data.page.font) + " " + textNumber(d.data.page.fontSize) + " pt; " +
                                           std::to_string(layoutDocument(d.data).size()) + " page(s)");
                maybePreview(r, d, p);
                return r;
            }));
}

namespace doccmd {

ActionFactory edit(Session& s, Edit fn) { return docCommand<TextDoc>(s, std::move(fn), true); }
ActionFactory read(Session& s, Edit fn) { return docCommand<TextDoc>(s, std::move(fn), false); }
ParamSpec doc() { return docSpec("document", "doc1"); }
std::string describe(const Block& b, std::size_t index) { return office::describe(b, index); }
bool blockArg(const TextDocument& d, const Params& p, const char* key, std::size_t& index, std::string& error) {
    return office::blockArg(d, p, key, index, error);
}
Runs inlineLines(const std::string& text) { return office::inlineLines(text); }

}  // namespace doccmd

}  // namespace wetwaresuite::office
