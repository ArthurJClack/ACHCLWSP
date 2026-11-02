// Text: fonts, style resolution and the line breaker that flows stories
// through columns and linked frames.
#include <algorithm>
#include <map>
#include <mutex>

#include "internal.hpp"

namespace wetwaresuite::layout {

// ---- faces ---------------------------------------------------------------------

double Face::advance(unsigned cp) const {
    if (pdfdoc::winAnsi(cp) < 0 && pdfdoc::needsUnicode(cp)) return pdfdoc::unicodeFontFor(cp)->advance(cp);  // set in a Unicode font
    if (ttf) return ttf->advance(cp);
    const double w = pdfdoc::standardWidth(font, pdfdoc::winAnsi(cp));
    return w > 0 ? w : 500;
}

pdfdoc::VMetrics Face::metrics() const { return ttf ? ttf->metrics() : pdfdoc::standardMetrics(font); }

std::shared_ptr<pdfdoc::TrueType> Face::outlines() const { return ttf ? ttf : pdfdoc::systemFont(family, bold, italic); }

unsigned encodable(unsigned cp) {
    if (cp == '\t') return ' ';
    return pdfdoc::winAnsi(cp) >= 0 || pdfdoc::needsUnicode(cp) ? cp : '?';
}

std::shared_ptr<const Face> resolveFace(const Layout& doc, const std::string& familyName, bool bold, bool italic) {
    static std::mutex mutex;
    static std::map<std::string, std::shared_ptr<const Face>> faces;
    static std::map<std::string, std::shared_ptr<pdfdoc::TrueType>> files;
    std::lock_guard<std::mutex> lock(mutex);

    for (const auto& fam : doc.fonts) {
        if (lower(fam.name) != lower(familyName)) continue;
        std::string path = fam.regular;
        if (bold && italic && !fam.boldItalic.empty()) path = fam.boldItalic;
        else if (bold && !fam.bold.empty()) path = fam.bold;
        else if (italic && !fam.italic.empty()) path = fam.italic;
        const std::string key = "ttf:" + path;
        auto it = faces.find(key);
        if (it != faces.end()) return it->second;
        std::shared_ptr<pdfdoc::TrueType> tt;
        auto fit = files.find(path);
        if (fit != files.end()) {
            tt = fit->second;
        } else {
            try {
                tt = pdfdoc::TrueType::load(path);
            } catch (const std::exception&) {
            }
            files[path] = tt;
        }
        if (!tt) break;  // unreadable: fall back to Helvetica below
        auto face = std::make_shared<Face>();
        face->key = key;
        face->standard = false;
        face->ttf = tt;
        face->bold = bold;
        face->italic = italic;
        face->label = fam.name + (bold ? " bold" : "") + (italic ? " italic" : "");
        faces[key] = face;
        return face;
    }
    const std::string family = pdfdoc::standardFamily(familyName).value_or("helvetica");
    const pdfdoc::Standard std = pdfdoc::standardFont(family, bold, italic);
    const std::string key = pdfdoc::baseFontName(std);
    auto it = faces.find(key);
    if (it != faces.end()) return it->second;
    auto face = std::make_shared<Face>();
    face->key = key;
    face->standard = true;
    face->font = std;
    face->family = family;
    face->bold = bold;
    face->italic = italic;
    face->label = key;
    faces[key] = face;
    return face;
}

// ---- styles ----------------------------------------------------------------------

namespace {

struct ParaStyleResolved {
    CharProps chars;
    ParaProps para;
};

CharProps rootChars() {
    CharProps c;
    c.font = "helvetica";
    c.size = 10;
    c.bold = false;
    c.italic = false;
    c.underline = false;
    c.color = "black";
    c.tracking = 0;
    return c;
}

ParaProps rootPara() {
    ParaProps p;
    p.leading = 0;
    p.align = Align::Left;
    p.spaceBefore = 0;
    p.spaceAfter = 0;
    p.indent = 0;
    p.leftIndent = 0;
    p.breakBefore = false;
    return p;
}

// Style chain from the root to `name` (cycles and unknown names stop it).
std::vector<const Style*> chain(const Layout& doc, const std::string& name) {
    std::vector<const Style*> out;
    std::string cur = name;
    for (int guard = 0; guard < 16 && !cur.empty(); ++guard) {
        const Style* found = nullptr;
        for (const auto& s : doc.styles) {
            if (s.name == cur) found = &s;
        }
        if (!found || std::find(out.begin(), out.end(), found) != out.end()) break;
        out.push_back(found);
        cur = found->basedOn;
    }
    std::reverse(out.begin(), out.end());
    return out;
}

ParaStyleResolved resolveParagraph(const Layout& doc, const Paragraph& p) {
    ParaStyleResolved r{rootChars(), rootPara()};
    for (const Style* s : chain(doc, p.style.empty() ? "body" : p.style)) {
        r.chars.merge(s->chars);
        r.para.merge(s->para);
    }
    r.para.merge(p.props);
    return r;
}

struct CharStyle {
    std::shared_ptr<const Face> face;
    double size = 3.5;  // mm
    std::string color;
    bool underline = false;
    double tracking = 0;  // mm
    double ascent = 0, descent = 0;  // mm, descent positive
};

struct Ch {
    unsigned cp = ' ';
    int style = 0;
    double adv = 0;  // mm including tracking
};

bool breakAfter(unsigned cp) { return cp == '-' || cp == 0x2013 || cp == 0x2014 || cp == '/'; }

Align resolveAlign(const Layout& doc, Align a, int pageIndex) {
    if (a != Align::Inside && a != Align::Outside) return a;
    const bool leftPage = doc.facing && pageIndex % 2 == 1;
    if (a == Align::Outside) return leftPage ? Align::Left : Align::Right;
    return leftPage ? Align::Right : Align::Left;
}

int pageOfFrame(const Layout& doc, const std::string& id, int fallback) {
    for (std::size_t i = 0; i < doc.pages.size(); ++i) {
        for (const auto& f : doc.pages[i].frames) {
            if (f.id == id) return static_cast<int>(i);
        }
    }
    return fallback;
}

struct PreparedPara {
    ParaStyleResolved style;
    std::vector<CharStyle> styles;
    std::vector<Ch> chars;
};

PreparedPara prepare(const Layout& doc, const Paragraph& p) {
    PreparedPara out;
    out.style = resolveParagraph(doc, p);
    auto add = [&](const CharProps& c) {
        CharStyle cs;
        cs.face = resolveFace(doc, c.font.value_or("helvetica"), c.bold.value_or(false), c.italic.value_or(false));
        cs.size = std::max(0.1, c.size.value_or(10)) * kPtToMm;
        cs.color = c.color.value_or("black");
        cs.underline = c.underline.value_or(false);
        cs.tracking = c.tracking.value_or(0) / 1000.0 * cs.size;
        const auto m = cs.face->metrics();
        cs.ascent = m.ascent / 1000.0 * cs.size;
        cs.descent = -m.descent / 1000.0 * cs.size;
        for (std::size_t i = 0; i < out.styles.size(); ++i) {
            const auto& o = out.styles[i];
            if (o.face == cs.face && o.size == cs.size && o.color == cs.color && o.underline == cs.underline && o.tracking == cs.tracking) {
                return static_cast<int>(i);
            }
        }
        out.styles.push_back(cs);
        return static_cast<int>(out.styles.size() - 1);
    };
    add(out.style.chars);  // style 0: the paragraph's own, for empty lines
    for (const auto& r : p.runs) {
        CharProps c = out.style.chars;
        for (const Style* s : chain(doc, r.style)) c.merge(s->chars);
        c.merge(r.props);
        const int idx = add(c);
        const CharStyle& cs = out.styles[static_cast<std::size_t>(idx)];
        for (unsigned cp : pdfdoc::utf8Decode(r.text)) {
            if (cp == '\n' || cp == '\r') cp = ' ';
            cp = encodable(cp);
            out.chars.push_back({cp, idx, cs.face->advance(cp) / 1000.0 * cs.size + cs.tracking});
        }
    }
    return out;
}

struct Composed {
    std::size_t end = 0;      // first char of the next line
    std::size_t visible = 0;  // end of the line without trailing spaces
    double width = 0;         // without trailing spaces
    int spaces = 0;           // inner spaces
    double ascent = 0, descent = 0, size = 0;
};

Composed compose(const PreparedPara& pp, std::size_t start, double avail) {
    Composed c;
    const auto& ch = pp.chars;
    std::size_t i = start;
    double width = 0;
    std::size_t lastBreak = 0;
    bool haveBreak = false;
    while (i < ch.size()) {
        const double w = ch[i].adv;
        if (ch[i].cp == ' ') {
            width += w;
            ++i;
            lastBreak = i;
            haveBreak = true;
            continue;
        }
        if (width + w > avail + 1e-9 && i > start) {
            i = haveBreak ? lastBreak : i;
            break;
        }
        width += w;
        ++i;
        if (breakAfter(ch[i - 1].cp) && i < ch.size()) {
            lastBreak = i;
            haveBreak = true;
        }
    }
    c.end = i;
    std::size_t vis = c.end;
    while (vis > start && ch[vis - 1].cp == ' ') --vis;
    c.visible = vis;
    for (std::size_t k = start; k < vis; ++k) {
        c.width += ch[k].adv;
        if (ch[k].cp == ' ') ++c.spaces;
        const auto& s = pp.styles[static_cast<std::size_t>(ch[k].style)];
        c.ascent = std::max(c.ascent, s.ascent);
        c.descent = std::max(c.descent, s.descent);
        c.size = std::max(c.size, s.size);
    }
    if (c.size == 0) {  // empty line: the paragraph's own style
        const auto& s = pp.styles[0];
        c.ascent = s.ascent;
        c.descent = s.descent;
        c.size = s.size;
    }
    return c;
}

Line build(const PreparedPara& pp, std::size_t start, const Composed& c, double left, double avail, Align align, bool lastLine) {
    Line line;
    double offset = 0, wordSpace = 0;
    const double extra = std::max(0.0, avail - c.width);
    if (align == Align::Right) offset = extra;
    else if (align == Align::Center) offset = extra / 2;
    else if (align == Align::Justify && !lastLine && c.spaces > 0) wordSpace = extra / c.spaces;
    line.left = left;
    double x = left + offset;
    std::size_t k = start;
    while (k < c.visible) {
        const int style = pp.chars[k].style;
        GlyphRun run;
        const auto& s = pp.styles[static_cast<std::size_t>(style)];
        run.face = s.face;
        run.size = s.size;
        run.color = s.color;
        run.underline = s.underline;
        run.tracking = s.tracking;
        run.wordSpace = wordSpace;
        run.x = x;
        while (k < c.visible && pp.chars[k].style == style) {
            run.text += pdfdoc::utf8Encode(pp.chars[k].cp);
            run.width += pp.chars[k].adv + (pp.chars[k].cp == ' ' ? wordSpace : 0);
            ++k;
        }
        x += run.width;
        line.runs.push_back(std::move(run));
    }
    return line;
}

}  // namespace

// ---- flow ------------------------------------------------------------------------

std::vector<const Frame*> storyChain(const Layout& doc, const std::string& storyId) {
    std::vector<const Frame*> frames;
    std::map<std::string, const Frame*> byId;
    for (const auto& p : doc.pages) {
        for (const auto& f : p.frames) {
            if (f.type == FrameType::Text && f.story == storyId) {
                frames.push_back(&f);
                byId[f.id] = &f;
            }
        }
    }
    for (const auto& m : doc.masters) {
        for (const auto& f : m.frames) {
            if (f.type == FrameType::Text && f.story == storyId) {
                frames.push_back(&f);
                byId[f.id] = &f;
            }
        }
    }
    if (frames.size() < 2) return frames;
    // the head is the frame nobody links to
    const Frame* head = nullptr;
    for (const Frame* f : frames) {
        bool linked = false;
        for (const Frame* g : frames) linked = linked || g->next == f->id;
        if (!linked) {
            head = f;
            break;
        }
    }
    std::vector<const Frame*> out;
    for (const Frame* f = head; f && out.size() < frames.size(); f = byId.count(f->next) ? byId[f->next] : nullptr) {
        if (std::find(out.begin(), out.end(), f) != out.end()) break;
        out.push_back(f);
    }
    for (const Frame* f : frames) {  // unlinked leftovers keep page order
        if (std::find(out.begin(), out.end(), f) == out.end()) out.push_back(f);
    }
    return out;
}

StoryLayout layoutStory(const Layout& doc, const Story& story, const std::vector<const Frame*>& chain, int pageIndex) {
    StoryLayout out;
    out.total = story.length();
    std::size_t placed = 0;
    std::size_t para = 0, charPos = 0;  // where the text continues
    std::vector<PreparedPara> prepared;
    prepared.reserve(story.paragraphs.size());
    for (const auto& p : story.paragraphs) prepared.push_back(prepare(doc, p));

    for (const Frame* frame : chain) {
        FrameText& ft = out.frames[frame->id];
        if (para >= prepared.size()) continue;
        const double innerW = frame->w - 2 * frame->inset, innerH = frame->h - 2 * frame->inset;
        const int cols = std::max(1, frame->columns);
        const double colW = (innerW - frame->gutter * (cols - 1)) / cols;
        if (colW <= 0.5 || innerH <= 0.5) continue;
        const int page = pageOfFrame(doc, frame->id, pageIndex);
        const double top = frame->inset, bottom = frame->inset + innerH;
        std::vector<int> lineCol;
        std::vector<double> lineBottom;
        int col = 0;
        bool colTop = true;
        double baseline = 0, pendingGap = 0;
        bool frameEmpty = true;
        while (para < prepared.size() && col < cols) {
            const PreparedPara& pp = prepared[para];
            if (charPos == 0 && pp.style.para.breakBefore.value_or(false) && !frameEmpty) break;
            const double colX = frame->inset + col * (colW + frame->gutter);
            const bool first = charPos == 0;
            const double leftIndent = pp.style.para.leftIndent.value_or(0);
            const double indent = first ? pp.style.para.indent.value_or(0) : 0;
            const double avail = std::max(1.0, colW - leftIndent - indent);
            std::size_t start = charPos;
            if (!first) {
                while (start < pp.chars.size() && pp.chars[start].cp == ' ') ++start;
            }
            const Composed c = compose(pp, start, avail);
            const double setLeading = pp.style.para.leading.value_or(0) * kPtToMm;
            const double leading = setLeading >= c.size * 0.99 ? setLeading : 1.2 * c.size;
            double bl;
            if (colTop) bl = top + c.ascent;
            else bl = baseline + leading + (first ? pendingGap + pp.style.para.spaceBefore.value_or(0) * kPtToMm : 0);
            if (bl + c.descent > bottom + 1e-6) {
                ++col;
                colTop = true;
                continue;
            }
            const bool last = c.end >= pp.chars.size();
            Align align = resolveAlign(doc, pp.style.para.align.value_or(Align::Left), page);
            Line line = build(pp, start, c, colX + leftIndent + indent, avail, align, last);
            line.baseline = bl;
            ft.lines.push_back(std::move(line));
            lineCol.push_back(col);
            lineBottom.push_back(bl + c.descent);
            baseline = bl;
            colTop = false;
            frameEmpty = false;
            ft.chars += c.end - charPos;
            if (last) {
                ft.chars += 1;  // the paragraph break
                pendingGap = pp.style.para.spaceAfter.value_or(0) * kPtToMm;
                ++para;
                charPos = 0;
            } else {
                charPos = c.end;
            }
        }
        if (frame->valign != VAlign::Top) {
            for (int k = 0; k < cols; ++k) {
                double lowest = -1;
                for (std::size_t i = 0; i < lineCol.size(); ++i) {
                    if (lineCol[i] == k) lowest = std::max(lowest, lineBottom[i]);
                }
                if (lowest < 0) continue;
                double shift = bottom - lowest;
                if (frame->valign == VAlign::Middle) shift /= 2;
                for (std::size_t i = 0; i < lineCol.size(); ++i) {
                    if (lineCol[i] == k) ft.lines[i].baseline += shift;
                }
            }
        }
        placed += ft.chars;
    }
    out.overset = out.total > placed ? out.total - placed : 0;
    return out;
}

std::map<std::string, StoryLayout> layoutAll(const Layout& doc) {
    std::map<std::string, StoryLayout> out;
    std::map<std::string, bool> onPages;
    for (const auto& p : doc.pages) {
        for (const auto& f : p.frames) {
            if (f.type == FrameType::Text && !f.story.empty()) onPages[f.story] = true;
        }
    }
    for (const auto& [id, used] : onPages) {
        auto it = doc.stories.find(id);
        if (it == doc.stories.end()) continue;
        std::vector<const Frame*> chain;
        for (const Frame* f : storyChain(doc, id)) {
            bool master = false;
            for (const auto& m : doc.masters) {
                for (const auto& g : m.frames) master = master || &g == f;
            }
            if (!master) chain.push_back(f);
        }
        out[id] = layoutStory(doc, it->second, chain, 0);
    }
    return out;
}

Story fieldsFilled(const Layout& doc, const Story& story, int pageIndex) {
    Story s = story;
    auto replaceAll = [](std::string& text, const std::string& from, const std::string& to) {
        for (std::size_t pos = text.find(from); pos != std::string::npos; pos = text.find(from, pos + to.size())) {
            text.replace(pos, from.size(), to);
        }
    };
    for (auto& p : s.paragraphs) {
        for (auto& r : p.runs) {
            replaceAll(r.text, "{page}", std::to_string(pageIndex + 1));
            replaceAll(r.text, "{pages}", std::to_string(doc.pages.size()));
            replaceAll(r.text, "{title}", doc.title);
        }
    }
    return s;
}

double textWidth(const Layout& doc, const std::string& text, const std::string& font, double sizePt, bool bold, bool italic) {
    auto face = resolveFace(doc, font, bold, italic);
    double w = 0;
    for (unsigned cp : pdfdoc::utf8Decode(text)) w += face->advance(encodable(cp));
    return w / 1000.0 * sizePt * kPtToMm;
}

}  // namespace wetwaresuite::layout
