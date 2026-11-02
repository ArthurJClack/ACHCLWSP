// Page layout for documents: word wrap with mixed styles, paragraph styles and
// formats (alignment, indents, spacing), headings that keep with the next
// lines, numbered lists, tables with merged cells and repeated header rows,
// images, headers and footers with page fields, footnotes at the foot of the
// page, a table of contents with page numbers, and tracked changes shown as
// markup.
#include <algorithm>
#include <cmath>

#include "wetwaresuite/office/doc.hpp"
#include "text.hpp"

#include "textlayout.hpp"

namespace wetwaresuite::office {

namespace textlayout {

using pdf::Color;
using pdf::Font;

namespace {

Color colorOf(std::uint32_t c) { return Color{static_cast<std::uint8_t>((c >> 16) & 255), static_cast<std::uint8_t>((c >> 8) & 255), static_cast<std::uint8_t>(c & 255), 255}; }

const Color kChange{0xB5, 0x12, 0x1B, 255};
const Color kCommentMark{0xFF, 0xF2, 0xCC, 255};

}  // namespace

std::vector<Piece> pieces(const Runs& runs, const Style& st) {
    std::vector<Piece> out;
    for (const auto& r : runs) {
        Piece base;
        base.font = st.font;
        if (r.font == "serif") base.font.family = pdf::Family::Serif;
        else if (r.font == "sans") base.font.family = pdf::Family::Sans;
        else if (r.font == "mono") base.font.family = pdf::Family::Mono;
        if (r.code) base.font.family = pdf::Family::Mono;
        base.font.bold = st.font.bold || r.bold;
        base.font.italic = st.font.italic || r.italic;
        double size = r.size > 0 ? r.size : st.size;
        if (r.code && r.size <= 0) size *= 0.92;
        base.size = size;
        base.color = hasColor(r.color) ? colorOf(r.color) : st.color;
        base.underline = st.underline || r.underline;
        base.strike = r.strike;
        if (hasColor(r.highlight)) base.highlight = colorOf(r.highlight);
        if (r.vertical) {
            base.size = size * 0.65;
            base.rise = r.vertical > 0 ? size * 0.33 : -size * 0.12;
        }
        if (!r.link.empty()) {
            base.link = r.link;
            base.color = st.link;
            base.underline = true;
        }
        if (st.doc && r.change) {
            const TrackedChange* ch = st.doc->change(r.change);
            if (ch && !st.doc->showMarkup && !ch->insert) continue;  // final view: deleted text is gone
            if (ch && st.doc->showMarkup) {
                base.color = kChange;
                if (ch->insert) base.underline = true;
                else base.strike = true;
            }
        }
        if (st.doc && st.doc->showMarkup && r.comment && !base.highlight) base.highlight = kCommentMark;
        if (!r.footnote.empty()) {
            // the layout numbers notes; the run's text holds the number by then
            Piece p = base;
            p.size = size * 0.65;
            p.rise = size * 0.33;
            p.text = r.text.empty() ? std::string("*") : r.text;
            p.note = r.footnote;
            p.width = pdf::textWidth(p.font, p.text, p.size);
            out.push_back(p);
            continue;
        }
        std::string cur;
        int kind = -1;  // 0 word, 1 spaces
        auto flush = [&] {
            if (cur.empty()) return;
            Piece p = base;
            p.text = cur;
            p.width = pdf::textWidth(p.font, cur, p.size);
            out.push_back(p);
            cur.clear();
        };
        for (char c : r.text) {
            if (c == '\n') {
                flush();
                Piece p = base;
                p.text = "\n";
                out.push_back(p);
                kind = -1;
                continue;
            }
            int k = (c == ' ' || c == '\t') ? 1 : 0;
            if (k != kind) flush();
            kind = k;
            cur += c == '\t' ? std::string("    ") : std::string(1, c);
        }
        flush();
    }
    return out;
}

// Splits a word that is wider than the line into pieces that fit.
static std::vector<Piece> splitWord(const Piece& p, double maxWidth) {
    std::vector<Piece> out;
    Piece cur = p;
    cur.text.clear();
    for (unsigned cp : pdf::utf8Decode(p.text)) {
        std::string ch = pdf::utf8Encode(cp);
        double w = pdf::textWidth(p.font, cur.text + ch, p.size);
        if (w > maxWidth && !cur.text.empty()) {
            cur.width = pdf::textWidth(p.font, cur.text, p.size);
            out.push_back(cur);
            cur.text.clear();
        }
        cur.text += ch;
    }
    if (!cur.text.empty()) {
        cur.width = pdf::textWidth(p.font, cur.text, p.size);
        out.push_back(cur);
    }
    return out;
}

std::vector<Line> wrap(const std::vector<Piece>& in, double maxWidth, double baseSize) {
    std::vector<Line> lines;
    Line line;
    line.size = baseSize;
    auto push = [&](bool hard) {
        // drop trailing spaces
        while (!line.pieces.empty() && line.pieces.back().space()) {
            line.width -= line.pieces.back().width;
            line.pieces.pop_back();
            line.xs.pop_back();
        }
        line.hardBreak = hard;
        lines.push_back(line);
        line = Line();
        line.size = baseSize;
    };
    std::vector<Piece> queue;
    for (const auto& p : in) {
        if (!p.space() && !p.newline() && p.width > maxWidth && !p.text.empty()) {
            for (auto& s : splitWord(p, maxWidth)) queue.push_back(s);
        } else {
            queue.push_back(p);
        }
    }
    for (std::size_t i = 0; i < queue.size(); ++i) {
        const Piece& p = queue[i];
        if (p.newline()) {
            push(true);
            continue;
        }
        if (p.space() && line.pieces.empty()) continue;
        // a footnote number stays with the word before it
        const bool glued = !p.note.empty() && !line.pieces.empty() && !line.pieces.back().space();
        if (!p.space() && !glued && line.width + p.width > maxWidth + 0.01 && !line.pieces.empty()) push(false);
        line.xs.push_back(line.width);
        line.pieces.push_back(p);
        line.width += p.width;
        if (p.rise == 0 && !p.text.empty()) line.size = std::max(line.size, p.size);
    }
    if (!line.pieces.empty() || lines.empty()) push(true);
    return lines;
}

void drawLine(pdf::Page& page, const Line& l, double x, double y) { drawLine(page, l, x, y, l.width, 'l', true); }

void drawLine(pdf::Page& page, const Line& l, double x, double y, double width, char align, bool last) {
    const double baseline = y + l.size * 0.92;
    double shift = 0, gap = 0;
    if (align == 'c') shift = (width - l.width) / 2;
    else if (align == 'r') shift = width - l.width;
    else if (align == 'j' && !last && !l.hardBreak && width > l.width) {
        int spaces = 0;
        for (const auto& p : l.pieces) spaces += p.space() ? 1 : 0;
        if (spaces) gap = (width - l.width) / spaces;
    }
    double extra = 0;
    for (std::size_t i = 0; i < l.pieces.size(); ++i) {
        const Piece& p = l.pieces[i];
        const double px = x + shift + l.xs[i] + extra;
        const double pw = p.width + (p.space() ? gap : 0);
        if (p.space()) extra += gap;
        if (p.highlight) page.rect(px, y + l.size * 0.92 - p.size * 0.95 - p.rise, pw, p.size * 1.2, *p.highlight);
        if (p.text.empty()) continue;
        if (p.space() && !p.underline && !p.strike) continue;
        if (p.strike) {
            const double sy = baseline - p.rise - p.size * 0.3;
            page.line(px, sy, px + pw, sy, p.color, std::max(0.5, p.size * 0.06));
        }
        if (p.space()) {
            if (p.underline) page.line(px, baseline + p.size * 0.12, px + pw, baseline + p.size * 0.12, p.color, std::max(0.5, p.size * 0.05));
            continue;
        }
        pdf::TextOp t;
        t.x = px;
        t.y = baseline - p.rise;
        t.font = p.font;
        t.size = p.size;
        t.color = p.color;
        t.text = p.text;
        t.underline = p.underline;
        t.link = p.link;
        page.ops.emplace_back(std::move(t));
    }
}

}  // namespace textlayout

namespace {

using pdf::Color;
using pdf::Font;
using namespace textlayout;

const Color kText{0x22, 0x22, 0x22, 255};
const Color kHeading{0x1F, 0x38, 0x64, 255};
const Color kMuted{0x55, 0x55, 0x55, 255};
const Color kBorder{0x99, 0x99, 0x99, 255};

Color colorOf(std::uint32_t c) { return Color{static_cast<std::uint8_t>((c >> 16) & 255), static_cast<std::uint8_t>((c >> 8) & 255), static_cast<std::uint8_t>(c & 255), 255}; }

// How a block's text is set: the style plus the block's own format.
struct Para {
    Style text;
    char align = 'l';
    double indent = 0, indentRight = 0, firstLine = 0, before = 0, after = 0, lineSpacing = 1.1;
    bool keepWithNext = false, pageBreakBefore = false;
};

struct Layout {
    const TextDocument& doc;
    std::vector<pdf::Page> pages;
    double y = 0;
    double left, right, top, bottom;
    bool pageHasContent = false;
    std::vector<int> headingPage;      // page (1-based) of each heading, in order
    const std::vector<int>* tocPages;  // from the previous pass, for the table of contents
    // footnotes waiting at the foot of the current page
    struct Note {
        std::string number, text;
    };
    std::vector<Note> notes;
    double notesHeight = 0;
    std::vector<BlockBox> boxes;  // where each block landed, for clicking on the page

    Layout(const TextDocument& d, const std::vector<int>* toc) : doc(d), tocPages(toc) {
        left = d.page.left;
        right = d.page.width - d.page.right;
        top = d.page.top;
        bottom = d.page.height - d.page.bottom;
        newPage();
    }

    pdf::Page& page() { return pages.back(); }
    double limit() const { return bottom - notesHeight; }

    Style noteStyle() const {
        Style s;
        s.font.family = doc.page.font;
        s.size = doc.page.fontSize * 0.8;
        s.color = kText;
        s.doc = &doc;
        return s;
    }

    std::vector<Line> noteLines(const Note& n) const {
        Run num;
        num.text = n.number + " ";
        num.vertical = 1;
        Runs runs = parseInline(n.text);
        runs.insert(runs.begin(), num);
        Style s = noteStyle();
        return wrap(pieces(runs, s), right - left, s.size);
    }

    double noteHeight(const Note& n) const {
        double h = 0;
        for (const auto& l : noteLines(n)) h += l.size * 1.25;
        return h + 2;
    }

    void drawNotes() {
        if (notes.empty()) return;
        double ny = bottom - notesHeight + 6;
        page().line(left, ny - 3, left + (right - left) * 0.3, ny - 3, kBorder, 0.5);
        for (const auto& n : notes) {
            for (const auto& l : noteLines(n)) {
                textlayout::drawLine(page(), l, left, ny);
                ny += l.size * 1.25;
            }
            ny += 2;
        }
        notes.clear();
        notesHeight = 0;
    }

    void newPage() {
        if (!pages.empty()) drawNotes();
        pdf::Page p;
        p.width = doc.page.width;
        p.height = doc.page.height;
        pages.push_back(std::move(p));
        y = top;
        pageHasContent = false;
    }

    void ensure(double h) {
        if (y + h > limit() + 0.01 && pageHasContent) newPage();
    }

    static double lineHeight(double size, double spacing) { return size * 1.2 * spacing; }

    Para para(const Block& b) const {
        const ResolvedStyle rs = resolveStyle(doc, blockStyleId(b));
        Para p;
        p.text.font = Font{rs.font, rs.bold, rs.italic};
        p.text.size = rs.size;
        p.text.color = hasColor(rs.color) ? colorOf(rs.color) : kText;
        p.text.underline = rs.underline;
        p.text.doc = &doc;
        p.align = rs.align;
        p.indent = rs.indent;
        p.indentRight = rs.indentRight;
        p.firstLine = rs.firstLine;
        p.before = rs.before;
        p.after = rs.after;
        p.lineSpacing = rs.lineSpacing;
        p.keepWithNext = rs.keepWithNext;
        const ParaFormat& f = b.fmt;
        if (f.align) p.align = f.align;
        if (f.indent) p.indent = f.indent;
        if (f.indentRight) p.indentRight = f.indentRight;
        if (f.firstLine) p.firstLine = f.firstLine;
        if (f.before >= 0) p.before = f.before;
        if (f.after >= 0) p.after = f.after;
        if (f.lineSpacing > 0) p.lineSpacing = f.lineSpacing;
        if (f.keepWithNext) p.keepWithNext = true;
        p.pageBreakBefore = f.pageBreakBefore;
        return p;
    }

    // Wraps runs for a paragraph format: the first line may be indented further (or hang out).
    std::vector<Line> lines(const Runs& runs, const Para& p, double width) const {
        std::vector<Piece> ps = pieces(runs, p.text);
        if (p.firstLine > 0) {
            Piece indent;
            indent.width = p.firstLine;
            ps.insert(ps.begin(), indent);
        }
        return wrap(ps, width, p.text.size);
    }

    // Footnote references on a line get their notes added to this page; returns the height they need.
    double notesOn(const Line& l, std::vector<Note>& found) const {
        double h = 0;
        for (const auto& pc : l.pieces) {
            if (pc.note.empty()) continue;
            Note n{pc.text, pc.note};
            h += noteHeight(n);
            found.push_back(n);
        }
        return h;
    }

    // Places wrapped lines, breaking pages between lines; footnotes go to the foot of the page their reference is on.
    void placeLines(const std::vector<Line>& ls, double x, double width, const Para& p) {
        for (std::size_t i = 0; i < ls.size(); ++i) {
            const Line& l = ls[i];
            const double h = lineHeight(l.size, p.lineSpacing);
            std::vector<Note> found;
            const double nh = notesOn(l, found);
            const double sep = !found.empty() && notes.empty() ? 8 : 0;
            if (y + h + nh + sep > limit() + 0.01 && pageHasContent) newPage();
            const double lx = i == 0 && p.firstLine < 0 ? x + p.firstLine : x;
            textlayout::drawLine(page(), l, lx, y, width - (lx - x), p.align, i + 1 == ls.size());
            for (auto& n : found) {
                notesHeight += noteHeight(n) + (notes.empty() ? 8 : 0);
                notes.push_back(n);
            }
            y += h;
            pageHasContent = true;
        }
    }

    void textBlock(const Block& b, const Block* next) {
        const Para p = para(b);
        if (p.pageBreakBefore && pageHasContent) newPage();
        if (pageHasContent) y += p.before;
        const double x = left + p.indent, width = right - left - p.indent - p.indentRight;
        auto ls = lines(b.runs, p, width);
        if (b.kind == Block::Heading) headingPage.push_back(0);
        if (p.keepWithNext) {
            double keep = 0;
            for (const auto& l : ls) keep += lineHeight(l.size, p.lineSpacing);
            if (next && (next->kind == Block::Paragraph || next->kind == Block::List || next->kind == Block::Quote)) keep += 2 * lineHeight(doc.page.fontSize, 1.1);
            ensure(keep);
        } else if (!ls.empty()) {
            ensure(lineHeight(ls[0].size, p.lineSpacing));
        }
        if (b.kind == Block::Heading) headingPage.back() = static_cast<int>(pages.size());
        if (b.kind == Block::Quote && b.style.empty()) {
            // a bar down the left of the quote
            const double startY = y;
            const std::size_t startPage = pages.size();
            placeLines(ls, x, width, p);
            if (pages.size() == startPage) page().rect(x - 16, startY, 3, y - startY, Color{0xCC, 0xCC, 0xCC, 255});
        } else {
            placeLines(ls, x, width, p);
        }
        y += p.after;
    }

    void list(const Block& b) {
        Para p = para(b);
        if (pageHasContent) y += p.before;
        std::vector<int> counters(10, 0);
        const std::string fmt = !b.numbering.empty() ? b.numbering : b.ordered ? "decimal" : "bullet";
        for (const auto& it : b.items) {
            const int lv = std::clamp(it.level, 0, 9);
            for (int k = lv + 1; k < 10; ++k) counters[static_cast<std::size_t>(k)] = 0;
            const double indent = p.indent + 20 + lv * 20;
            std::string marker;
            if (fmt != "bullet") {
                int& n = counters[static_cast<std::size_t>(lv)];
                n = n == 0 && lv == 0 ? b.start : n + 1;
                if (n == 0) n = 1;
                const std::string levelFmt = lv == 0 ? fmt : lv % 3 == 1 ? "lowerLetter" : lv % 3 == 2 ? "lowerRoman" : "decimal";
                marker = listLabel(levelFmt, n);
            } else {
                marker = lv % 2 == 0 ? "\xE2\x80\xA2" : "\xE2\x80\x93";  // • and –
            }
            // a task list item ("- [ ] milk" reads in as "\u2610 milk"): a drawn box, ticked when done
            int task = 0;  // 1 open, 2 done
            auto runs = it.runs;
            if (!runs.empty()) {
                std::string& t0 = runs[0].text;
                if (t0.rfind("\xE2\x98\x90", 0) == 0 || t0.rfind("\xE2\x98\x91", 0) == 0) {
                    task = t0[2] == '\x90' ? 1 : 2;
                    t0.erase(0, t0.size() > 3 && t0[3] == ' ' ? 4 : 3);
                }
            }
            auto ls = wrap(pieces(runs, p.text), right - left - indent - p.indentRight, p.text.size);
            const double h = lineHeight(p.text.size, p.lineSpacing);
            ensure(h);
            Font mf = p.text.font;
            if (task) {
                const double bs = p.text.size * 0.72, bx = left + indent - 6 - bs, by = y + p.text.size * 0.92 - bs;
                page().rect(bx, by, bs, bs, std::nullopt, kHeading, 0.9);
                if (task == 2) {
                    page().line(bx + bs * 0.2, by + bs * 0.55, bx + bs * 0.42, by + bs * 0.8, kHeading, 1.3);
                    page().line(bx + bs * 0.42, by + bs * 0.8, bx + bs * 0.85, by + bs * 0.2, kHeading, 1.3);
                }
            } else {
                const double mw = pdf::textWidth(mf, marker, p.text.size);
                page().text(left + indent - 6 - mw, y + p.text.size * 0.92, marker, mf, p.text.size, fmt != "bullet" ? p.text.color : kHeading);
            }
            Para itemPara = p;
            itemPara.firstLine = 0;
            placeLines(ls, left + indent, right - left - indent - p.indentRight, itemPara);
            y += 2;
        }
        y += std::max(4.0, p.after);
    }

    void code(const Block& b) {
        Para p = para(b);
        const double pad = 6;
        std::vector<Line> ls;
        for (const auto& raw : splitLines(plainText(b.runs))) {
            Run r;
            r.text = raw.empty() ? " " : raw;
            auto w = wrap(pieces({r}, p.text), right - left - 2 * pad, p.text.size);
            ls.insert(ls.end(), w.begin(), w.end());
        }
        if (ls.empty()) return;
        const double h = lineHeight(p.text.size, p.lineSpacing);
        ensure(h + 2 * pad);
        std::size_t bgIndex = page().ops.size();
        double startY = y;
        y += pad;
        auto background = [&](double endY) {
            pdf::RectOp r{left, startY, right - left, endY - startY, Color{0xF4, 0xF4, 0xF4, 255}, std::nullopt, 1};
            page().ops.insert(page().ops.begin() + static_cast<std::ptrdiff_t>(bgIndex), r);
        };
        for (const auto& l : ls) {
            if (y + h + pad > limit() && pageHasContent) {
                background(y + pad);
                newPage();
                bgIndex = page().ops.size();
                startY = y;
                y += pad;
            }
            textlayout::drawLine(page(), l, left + pad, y);
            y += h;
            pageHasContent = true;
        }
        y += pad;
        background(y);
        y += p.after;
    }

    void image(const Block& b) {
        if (!b.pixels || b.pixels->pixels.empty()) return;
        const double pw = b.pixels->pixels.width(), ph = b.pixels->pixels.height();
        double w = b.width > 0 ? b.width : pw * 0.75;
        w = std::min(w, right - left);
        double h = w * ph / pw;
        const double maxH = bottom - top;
        if (h > maxH) {
            h = maxH;
            w = h * pw / ph;
        }
        ensure(h);
        const char align = b.fmt.align ? b.fmt.align : 'c';
        const double x = align == 'l' ? left + b.fmt.indent : align == 'r' ? right - w : left + (right - left - w) / 2;
        page().image(x, y, w, h, b.pixels);
        y += h + (b.fmt.after >= 0 ? b.fmt.after : 10);
        pageHasContent = true;
    }

    void toc(const Block& b) {
        Block head;
        head.kind = Block::Paragraph;
        head.runs = b.runs.empty() ? parseInline("Contents") : b.runs;
        Para hp = para(head);
        hp.text.size = doc.page.fontSize * 1.3;
        hp.text.font.bold = true;
        hp.text.color = kHeading;
        hp.after = 6;
        if (pageHasContent) y += 8;
        placeLines(lines(head.runs, hp, right - left), left, right - left, hp);
        y += hp.after;
        const auto entries = outline(doc, std::clamp(b.level, 1, 6));
        // which heading (in document order) each entry is, for its page
        std::vector<int> index;
        int k = 0;
        for (const auto& x : doc.blocks) {
            if (x.kind != Block::Heading) continue;
            if (x.level <= b.level) index.push_back(k);
            ++k;
        }
        Para p = para(head);
        p.after = 0;
        for (std::size_t i = 0; i < entries.size(); ++i) {
            const auto& [lv, text] = entries[i];
            const int hi = i < index.size() ? index[i] : -1;
            const int pg = tocPages && hi >= 0 && static_cast<std::size_t>(hi) < tocPages->size() ? (*tocPages)[static_cast<std::size_t>(hi)] : 0;
            const std::string num = pg > 0 ? std::to_string(pg) : std::string("0");
            const double indent = (lv - 1) * 14.0;
            Style s = p.text;
            s.font.bold = lv == 1;
            const double numW = pdf::textWidth(s.font, num, s.size);
            Run r;
            r.text = text;
            auto ls = wrap(pieces({r}, s), right - left - indent - numW - 24, s.size);
            const double h = lineHeight(s.size, 1.25);
            ensure(h * static_cast<double>(ls.size()));
            for (std::size_t li = 0; li < ls.size(); ++li) {
                textlayout::drawLine(page(), ls[li], left + indent, y);
                if (li + 1 == ls.size()) {
                    // dot leaders up to the page number
                    const double from = left + indent + ls[li].width + 4, to = right - numW - 4;
                    const double dotW = std::max(1.0, pdf::textWidth(Font{s.font.family, false, false}, ". ", s.size * 0.8));
                    std::string dots;
                    for (double dx = from; dx + dotW <= to; dx += dotW) dots += ". ";
                    if (!dots.empty()) page().text(from, y + s.size * 0.92, dots, Font{s.font.family, false, false}, s.size * 0.8, kMuted);
                    page().text(right - numW, y + s.size * 0.92, num, s.font, s.size, s.color);
                }
                y += h;
                pageHasContent = true;
            }
        }
        y += 10;
    }

    void table(const Block& b) {
        std::size_t cols = 0;
        for (const auto& r : b.rows) cols = std::max(cols, r.size());
        if (!cols || b.rows.empty()) return;
        const std::size_t rows = b.rows.size();
        Para tp = para(b);
        Style s = tp.text;
        s.size = doc.page.fontSize * 0.95;
        const double pad = 4;
        const double W = right - left;
        // merged areas: what covers each cell
        std::vector<std::vector<const CellSpan*>> cover(rows, std::vector<const CellSpan*>(cols, nullptr));
        for (const auto& m : b.merges) {
            for (int r = m.row; r < m.row + m.rows && r < static_cast<int>(rows); ++r) {
                for (int c = m.col; c < m.col + m.cols && c < static_cast<int>(cols); ++c) {
                    if (r >= 0 && c >= 0) cover[static_cast<std::size_t>(r)][static_cast<std::size_t>(c)] = &m;
                }
            }
        }
        auto topLeft = [&](std::size_t r, std::size_t c) {
            const CellSpan* m = cover[r][c];
            return !m || (m->row == static_cast<int>(r) && m->col == static_cast<int>(c));
        };
        auto spanCols = [&](std::size_t r, std::size_t c) { return cover[r][c] ? static_cast<std::size_t>(std::max(1, cover[r][c]->cols)) : std::size_t{1}; };
        auto spanRows = [&](std::size_t r, std::size_t c) { return cover[r][c] ? static_cast<std::size_t>(std::max(1, cover[r][c]->rows)) : std::size_t{1}; };
        auto cellStyle = [&](std::size_t r) {
            Style cs = s;
            if (r == 0 && b.header) cs.font.bold = true;
            return cs;
        };
        auto fmtOf = [&](std::size_t r, std::size_t c) -> CellFormat {
            if (r < b.cellFormats.size() && c < b.cellFormats[r].size()) return b.cellFormats[r][c];
            return {};
        };
        // column widths
        std::vector<double> widths(cols, 0);
        double given = 0;
        for (std::size_t c = 0; c < cols && c < b.colWidths.size(); ++c) given += std::max(0.0, b.colWidths[c]);
        if (b.colWidths.size() >= cols && given > 0) {
            for (std::size_t c = 0; c < cols; ++c) widths[c] = W * std::max(0.0, b.colWidths[c]) / given;
        } else {
            std::vector<double> nat(cols, 0), mn(cols, 0);
            for (std::size_t r = 0; r < rows; ++r) {
                const Style cs = cellStyle(r);
                for (std::size_t c = 0; c < b.rows[r].size(); ++c) {
                    if (!topLeft(r, c) || spanCols(r, c) > 1) continue;
                    double lineW = 0;
                    for (const auto& p : pieces(b.rows[r][c], cs)) {
                        if (p.newline()) {
                            nat[c] = std::max(nat[c], lineW);
                            lineW = 0;
                            continue;
                        }
                        lineW += p.width;
                        if (!p.space()) mn[c] = std::max(mn[c], std::min(p.width, W / 3));
                    }
                    nat[c] = std::max(nat[c], lineW);
                }
            }
            double sumNat = 0, sumMin = 0;
            for (std::size_t c = 0; c < cols; ++c) {
                nat[c] += 2 * pad + 1;
                mn[c] += 2 * pad + 1;
                sumNat += nat[c];
                sumMin += mn[c];
            }
            if (sumNat <= W) {
                for (std::size_t c = 0; c < cols; ++c) widths[c] = nat[c] + (W - sumNat) / static_cast<double>(cols);
            } else if (sumMin < W) {
                double extra = W - sumMin, want = sumNat - sumMin;
                for (std::size_t c = 0; c < cols; ++c) widths[c] = mn[c] + (want > 0 ? extra * (nat[c] - mn[c]) / want : 0);
            } else {
                for (std::size_t c = 0; c < cols; ++c) widths[c] = W / static_cast<double>(cols);
            }
        }
        std::vector<double> xs(cols + 1, left);
        for (std::size_t c = 0; c < cols; ++c) xs[c + 1] = xs[c] + widths[c];
        // cell lines and row heights
        std::vector<std::vector<std::vector<Line>>> cells(rows, std::vector<std::vector<Line>>(cols));
        std::vector<std::vector<double>> contentH(rows, std::vector<double>(cols, 0));
        std::vector<double> rowH(rows, 0);
        for (std::size_t r = 0; r < rows; ++r) {
            const Style cs = cellStyle(r);
            for (std::size_t c = 0; c < cols; ++c) {
                if (!topLeft(r, c)) continue;
                const std::size_t c2 = std::min(cols, c + spanCols(r, c));
                const Runs runs = c < b.rows[r].size() ? b.rows[r][c] : Runs{};
                cells[r][c] = wrap(pieces(runs, cs), xs[c2] - xs[c] - 2 * pad, cs.size);
                double h = 0;
                for (const auto& l : cells[r][c]) h += lineHeight(l.size, 1.1);
                contentH[r][c] = h + 2 * pad;
                if (spanRows(r, c) == 1) rowH[r] = std::max(rowH[r], contentH[r][c]);
            }
            rowH[r] = std::max(rowH[r], lineHeight(cs.size, 1.1) + 2 * pad);
        }
        // a cell spanning rows that needs more room makes its last row taller
        for (std::size_t r = 0; r < rows; ++r) {
            for (std::size_t c = 0; c < cols; ++c) {
                if (!topLeft(r, c) || spanRows(r, c) == 1) continue;
                const std::size_t r2 = std::min(rows, r + spanRows(r, c));
                double sum = 0;
                for (std::size_t k = r; k < r2; ++k) sum += rowH[k];
                if (contentH[r][c] > sum) rowH[r2 - 1] += contentH[r][c] - sum;
            }
        }
        // rows that must stay on one page together (vertical merges)
        std::vector<std::size_t> groupEnd(rows);
        for (std::size_t r = 0; r < rows; ++r) groupEnd[r] = r;
        for (std::size_t r = 0; r < rows; ++r) {
            for (std::size_t c = 0; c < cols; ++c) {
                if (topLeft(r, c) && spanRows(r, c) > 1) groupEnd[r] = std::max(groupEnd[r], std::min(rows, r + spanRows(r, c)) - 1);
            }
        }
        const Color headFill{0xEE, 0xEE, 0xEE, 255}, bandFill{0xF6, 0xF8, 0xFB, 255};
        auto drawCell = [&](std::size_t r, std::size_t c, double top) {
            const std::size_t c2 = std::min(cols, c + spanCols(r, c)), r2 = std::min(rows, r + spanRows(r, c));
            double h = 0;
            for (std::size_t k = r; k < r2; ++k) h += rowH[k];
            const double x = xs[c], w = xs[c2] - xs[c];
            const CellFormat f = fmtOf(r, c);
            if (hasColor(f.fill)) page().rect(x, top, w, h, colorOf(f.fill));
            else if (r == 0 && b.header) page().rect(x, top, w, h, headFill);
            else if (b.banded && r % 2 == (b.header ? 0u : 1u)) page().rect(x, top, w, h, bandFill);
            if (b.borders) page().rect(x, top, w, h, std::nullopt, kBorder, 0.5);
            double ty = top + pad;
            if (f.valign == 'm' || f.valign == 'b') {
                const double free = h - contentH[r][c];
                ty += f.valign == 'm' ? free / 2 : free;
            }
            for (const auto& l : cells[r][c]) {
                textlayout::drawLine(page(), l, x + pad, ty, w - 2 * pad, f.align ? f.align : 'l', true);
                ty += lineHeight(l.size, 1.1);
            }
        };
        auto drawRow = [&](std::size_t r) {
            for (std::size_t c = 0; c < cols; ++c) {
                if (topLeft(r, c)) drawCell(r, c, y);
            }
            y += rowH[r];
            pageHasContent = true;
        };
        if (pageHasContent) y += tp.before > 0 ? tp.before : 0;
        for (std::size_t r = 0; r < rows;) {
            const std::size_t end = groupEnd[r];
            double h = 0;
            for (std::size_t k = r; k <= end; ++k) h += rowH[k];
            if (y + h > limit() && pageHasContent) {
                newPage();
                if (b.header && r > 0 && groupEnd[0] == 0) drawRow(0);
            }
            for (std::size_t k = r; k <= end; ++k) drawRow(k);
            r = end + 1;
        }
        y += 10;
    }

    void run() {
        for (std::size_t i = 0; i < doc.blocks.size(); ++i) {
            const Block& b = doc.blocks[i];
            const Block* next = i + 1 < doc.blocks.size() ? &doc.blocks[i + 1] : nullptr;
            const int startPage = static_cast<int>(pages.size());
            const double startY = y;
            switch (b.kind) {
                case Block::Heading:
                case Block::Paragraph:
                case Block::Quote: textBlock(b, next); break;
                case Block::List: list(b); break;
                case Block::Table: table(b); break;
                case Block::Image: image(b); break;
                case Block::Code: code(b); break;
                case Block::Toc: toc(b); break;
                case Block::PageBreak:
                    if (pageHasContent) newPage();
                    break;
            }
            const int endPage = static_cast<int>(pages.size());
            const int block = static_cast<int>(i) + 1;
            if (endPage == startPage) {
                boxes.push_back({block, startPage, startY, y});
            } else {
                // a block that moved to a new page, or ran over several: the part above
                // the break is mostly spacing, so it only counts when lines were left there
                if (bottom - startY > 24) boxes.push_back({block, startPage, startY, bottom});
                for (int pg = startPage + 1; pg < endPage; ++pg) boxes.push_back({block, pg, top, bottom});
                boxes.push_back({block, endPage, top, y});
            }
        }
        drawNotes();
    }

    // "left | centre | right" header or footer text, drawn with its middle at y.
    void band(pdf::Page& pg, const std::string& text, int pageNo, int pageCount, double midY) {
        if (trimCopy(text).empty()) return;
        std::vector<std::string> parts;
        std::string cur;
        for (std::size_t i = 0; i < text.size(); ++i) {
            if (text[i] == '\\' && i + 1 < text.size() && text[i + 1] == '|') {
                cur += '|';
                ++i;
            } else if (text[i] == '|') {
                parts.push_back(cur);
                cur.clear();
            } else {
                cur += text[i];
            }
        }
        parts.push_back(cur);
        Style s;
        s.font.family = doc.page.font;
        s.size = 9;
        s.color = Color{0x66, 0x66, 0x66, 255};
        const double W = right - left;
        const char aligns3[3] = {'l', 'c', 'r'};
        for (std::size_t k = 0; k < parts.size() && k < 3; ++k) {
            const std::string t = trimCopy(fillFields(parts[k], pageNo, pageCount, doc));
            if (t.empty()) continue;
            const char align = parts.size() == 1 ? 'c' : parts.size() == 2 ? (k == 0 ? 'l' : 'r') : aligns3[k];
            auto ls = wrap(pieces(parseInline(t), s), parts.size() == 1 ? W : W / 2, s.size);
            if (ls.empty()) continue;
            textlayout::drawLine(pg, ls[0], left, midY - s.size * 0.6, W, align, true);
        }
    }

    void headersAndFooters() {
        const int n = static_cast<int>(pages.size());
        const HeaderFooter& hf = doc.headerFooter;
        for (int i = 0; i < n; ++i) {
            pdf::Page& pg = pages[static_cast<std::size_t>(i)];
            const bool first = i == 0 && hf.differentFirst;
            const std::string& header = first ? hf.firstHeader : hf.header;
            const std::string& footer = first ? hf.firstFooter : hf.footer;
            band(pg, header, i + 1, n, top / 2);
            if (!footer.empty() || first) {
                band(pg, footer, i + 1, n, doc.page.height - doc.page.bottom / 2);
            } else if (doc.page.pageNumbers && n > 1) {
                std::string label = std::to_string(i + 1) + " / " + std::to_string(n);
                Font f{doc.page.font, false, false};
                double w = pdf::textWidth(f, label, 9);
                pg.text((doc.page.width - w) / 2, doc.page.height - doc.page.bottom / 2 + 3, label, f, 9, Color{0x88, 0x88, 0x88, 255});
            }
        }
    }
};

// Numbers the footnotes in document order: each reference run shows its number.
TextDocument numbered(const TextDocument& doc) {
    TextDocument d = doc;
    int n = 0;
    auto number = [&](Runs& runs) {
        for (auto& r : runs) {
            if (r.footnote.empty()) continue;
            const TrackedChange* ch = r.change ? doc.change(r.change) : nullptr;
            if (ch && !ch->insert && !doc.showMarkup) continue;
            r.text = std::to_string(++n);
        }
    };
    for (auto& b : d.blocks) {
        number(b.runs);
        for (auto& it : b.items) number(it.runs);
        for (auto& row : b.rows) {
            for (auto& c : row) number(c);
        }
    }
    return d;
}

bool hasToc(const TextDocument& doc) {
    return std::any_of(doc.blocks.begin(), doc.blocks.end(), [](const Block& b) { return b.kind == Block::Toc; });
}

}  // namespace

std::vector<int> headingPages(const TextDocument& doc) {
    const TextDocument d = numbered(doc);
    std::vector<int> pagesOf;
    for (int pass = 0; pass < 3; ++pass) {
        Layout L(d, pass ? &pagesOf : nullptr);
        L.run();
        const bool same = L.headingPage == pagesOf;
        pagesOf = L.headingPage;
        if (same || !hasToc(d)) break;
    }
    return pagesOf;
}

std::vector<pdf::Page> layoutDocument(const TextDocument& doc) {
    const TextDocument d = numbered(doc);
    std::vector<int> pagesOf;
    if (hasToc(d)) pagesOf = headingPages(doc);
    Layout L(d, &pagesOf);
    L.run();
    L.headersAndFooters();
    return L.pages;
}

std::vector<BlockBox> blockBoxes(const TextDocument& doc) {
    const TextDocument d = numbered(doc);
    std::vector<int> pagesOf;
    if (hasToc(d)) pagesOf = headingPages(doc);
    Layout L(d, &pagesOf);
    L.run();
    return L.boxes;
}

}  // namespace wetwaresuite::office
