// TextDocument model helpers plus Markdown, HTML and plain-text files.
#include <algorithm>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <sstream>
#include <stdexcept>

#include "wetwaresuite/office/doc.hpp"
#include "wetwaresuite/raster/codec.hpp"
#include "text.hpp"

namespace wetwaresuite::office {

namespace fs = std::filesystem;

const char* kindName(Block::Kind kind) {
    switch (kind) {
        case Block::Heading: return "heading";
        case Block::Paragraph: return "paragraph";
        case Block::List: return "list";
        case Block::Table: return "table";
        case Block::Image: return "image";
        case Block::PageBreak: return "page_break";
        case Block::Code: return "code";
        case Block::Quote: return "quote";
        case Block::Toc: return "toc";
    }
    return "paragraph";
}

void mergeRuns(Runs& runs) {
    Runs out;
    for (auto& r : runs) {
        if (r.text.empty() && r.footnote.empty()) continue;
        if (!out.empty() && out.back().sameStyle(r) && r.footnote.empty()) out.back().text += r.text;
        else out.push_back(std::move(r));
    }
    runs = std::move(out);
}

std::string plainText(const Runs& runs) {
    std::string s;
    for (const auto& r : runs) s += r.text;
    return s;
}

std::string blockText(const Block& b) {
    switch (b.kind) {
        case Block::List: {
            std::string s;
            for (const auto& it : b.items) s += (s.empty() ? "" : "\n") + std::string(static_cast<std::size_t>(it.level) * 2, ' ') + plainText(it.runs);
            return s;
        }
        case Block::Table: {
            std::string s;
            for (const auto& row : b.rows) {
                std::string line;
                for (std::size_t c = 0; c < row.size(); ++c) line += (c ? "\t" : "") + plainText(row[c]);
                s += (s.empty() ? "" : "\n") + line;
            }
            return s;
        }
        case Block::Image: return b.alt;
        case Block::PageBreak: return "";
        case Block::Toc: return plainText(b.runs).empty() ? std::string("Contents") : plainText(b.runs);
        default: return plainText(b.runs);
    }
}

void setImage(Block& b, const std::string& bytes) {
    const bool png = bytes.size() > 8 && bytes.compare(0, 8, "\x89PNG\r\n\x1a\n") == 0;
    const bool jpeg = bytes.size() > 3 && static_cast<unsigned char>(bytes[0]) == 0xFF && static_cast<unsigned char>(bytes[1]) == 0xD8;
    b.kind = Block::Image;
    if (png || jpeg) {
        b.image = bytes;
        b.pixels = pdf::loadImageData(bytes);
    } else {
        raster::Image img = raster::decode(bytes);
        b.image = raster::encodePng(img);
        b.pixels = pdf::loadImageData(b.image);
    }
}

// ---- inline Markdown -------------------------------------------------------------

namespace {

bool isPunct(char c) { return std::ispunct(static_cast<unsigned char>(c)) != 0; }
bool isAlnum(char c) { return std::isalnum(static_cast<unsigned char>(c)) != 0 || (static_cast<unsigned char>(c) >= 0x80); }

// The look set by <span style=...>, <mark>, <sup>, <sub>, <s> and ~~ around text.
struct SpanLook {
    std::string tag;
    bool strike = false;
    int vertical = 0;
    std::uint32_t color = 0, highlight = 0;
    double size = 0;
    std::string font;
};

// "color:#c00000;background:#ffff00;font-size:14pt;font-family:serif" -> look
void parseCss(const std::string& css, SpanLook& look) {
    std::size_t i = 0;
    while (i < css.size()) {
        std::size_t semi = css.find(';', i);
        if (semi == std::string::npos) semi = css.size();
        const std::string decl = css.substr(i, semi - i);
        i = semi + 1;
        const auto colon = decl.find(':');
        if (colon == std::string::npos) continue;
        const std::string key = lower(trimCopy(decl.substr(0, colon)));
        const std::string v = trimCopy(decl.substr(colon + 1));
        std::uint32_t c = 0;
        if (key == "color" && parseColor(v, c)) look.color = c;
        else if ((key == "background" || key == "background-color") && parseColor(v, c)) look.highlight = c;
        else if (key == "font-size") {
            double pt = std::atof(v.c_str());
            if (endsWith(v, "px")) pt *= 0.75;
            if (pt > 0) look.size = pt;
        } else if (key == "font-family") {
            const std::string f = lower(v);
            look.font = f.find("mono") != std::string::npos || f.find("courier") != std::string::npos ? "mono"
                        : f.find("sans") != std::string::npos || f.find("arial") != std::string::npos || f.find("helvetica") != std::string::npos ? "sans"
                        : f.find("serif") != std::string::npos || f.find("times") != std::string::npos || f.find("georgia") != std::string::npos ? "serif"
                                                                                                                                     : "";
        } else if (key == "text-decoration" && v.find("line-through") != std::string::npos) {
            look.strike = true;
        } else if (key == "vertical-align") {
            look.vertical = v == "super" ? 1 : v == "sub" ? -1 : 0;
        }
    }
}

struct InlineParser {
    const std::string& s;
    Runs out;
    std::string buf;
    bool bold = false, italic = false, underline = false, strike = false;
    std::string link;
    std::vector<SpanLook> looks;

    void style(Run& r) const {
        r.bold = bold;
        r.italic = italic;
        r.underline = underline;
        r.strike = strike;
        r.link = link;
        for (const auto& l : looks) {
            if (l.strike) r.strike = true;
            if (l.vertical) r.vertical = l.vertical;
            if (l.color) r.color = l.color;
            if (l.highlight) r.highlight = l.highlight;
            if (l.size > 0) r.size = l.size;
            if (!l.font.empty()) r.font = l.font;
        }
    }

    void flush() {
        if (buf.empty()) return;
        Run r;
        r.text = buf;
        style(r);
        out.push_back(std::move(r));
        buf.clear();
    }

    // <span ...>, <mark>, <sup>, <sub>, <s>, <del>, <strike> and their closing tags; false when it isn't one.
    bool lookTag(std::size_t& i) {
        const std::size_t gt = s.find('>', i);
        if (gt == std::string::npos) return false;
        const std::string inside = s.substr(i + 1, gt - i - 1);
        const std::string l = lower(inside);
        const bool closing = !l.empty() && l[0] == '/';
        std::string name = closing ? l.substr(1) : l;
        const auto sp = name.find(' ');
        if (sp != std::string::npos) name = name.substr(0, sp);
        static const char* tags[] = {"span", "mark", "sup", "sub", "s", "del", "strike", "font"};
        if (std::find_if(std::begin(tags), std::end(tags), [&](const char* t) { return name == t; }) == std::end(tags)) return false;
        flush();
        if (closing) {
            for (std::size_t k = looks.size(); k-- > 0;) {
                if (looks[k].tag == name) {
                    looks.erase(looks.begin() + static_cast<std::ptrdiff_t>(k));
                    break;
                }
            }
        } else {
            SpanLook look;
            look.tag = name;
            if (name == "mark") look.highlight = 0xFFFF00 | kColorSet;
            else if (name == "sup") look.vertical = 1;
            else if (name == "sub") look.vertical = -1;
            else if (name == "s" || name == "del" || name == "strike") look.strike = true;
            const auto st = l.find("style=");
            if (st != std::string::npos && st + 6 < inside.size()) {
                const char q = inside[st + 6];
                const std::size_t start = st + 7, end = q == '"' || q == '\'' ? inside.find(q, start) : std::string::npos;
                if (end != std::string::npos) parseCss(inside.substr(start, end - start), look);
            }
            const auto co = l.find("color=");
            if (name == "font" && co != std::string::npos) {
                std::string v = inside.substr(co + 6);
                if (!v.empty() && (v[0] == '"' || v[0] == '\'')) v = v.substr(1, v.find(v[0], 1) - 1);
                std::uint32_t c = 0;
                if (parseColor(v, c)) look.color = c;
            }
            looks.push_back(look);
        }
        i = gt;
        return true;
    }

    // finds the closing ']' for the '[' at i
    std::size_t closeBracket(std::size_t i) const {
        int depth = 0;
        for (std::size_t k = i; k < s.size(); ++k) {
            if (s[k] == '\\') {
                ++k;
                continue;
            }
            if (s[k] == '[') ++depth;
            if (s[k] == ']' && --depth == 0) return k;
        }
        return std::string::npos;
    }

    std::size_t closeParen(std::size_t i) const {
        int depth = 0;
        for (std::size_t k = i; k < s.size(); ++k) {
            if (s[k] == '(') ++depth;
            if (s[k] == ')' && --depth == 0) return k;
        }
        return std::string::npos;
    }

    static std::string linkTarget(std::string t) {
        t = trimCopy(t);
        auto sp = t.find_first_of(" \t");
        if (sp != std::string::npos) t = t.substr(0, sp);  // drop a "title"
        if (t.size() >= 2 && t.front() == '<' && t.back() == '>') t = t.substr(1, t.size() - 2);
        return t;
    }

    bool hasCloser(const std::string& marker, std::size_t from) const {
        std::size_t k = s.find(marker, from);
        while (k != std::string::npos) {
            if (k > 0 && s[k - 1] != ' ' && s[k - 1] != '\\') return true;
            k = s.find(marker, k + 1);
        }
        return false;
    }

    void run() {
        for (std::size_t i = 0; i < s.size(); ++i) {
            const char c = s[i];
            if (c == '\\' && i + 1 < s.size() && isPunct(s[i + 1])) {
                buf += s[++i];
                continue;
            }
            if (c == '`') {
                std::size_t n = 0;
                while (i + n < s.size() && s[i + n] == '`') ++n;
                std::string fence(n, '`');
                std::size_t close = s.find(fence, i + n);
                if (close != std::string::npos) {
                    flush();
                    Run r;
                    r.text = s.substr(i + n, close - i - n);
                    if (r.text.size() >= 2 && r.text.front() == ' ' && r.text.back() == ' ') r.text = r.text.substr(1, r.text.size() - 2);
                    style(r);
                    r.code = true;
                    r.underline = false;
                    out.push_back(r);
                    i = close + n - 1;
                    continue;
                }
                buf += fence;
                i += n - 1;
                continue;
            }
            if (c == '^' && i + 1 < s.size() && s[i + 1] == '[') {
                const std::size_t close = closeBracket(i + 1);
                if (close != std::string::npos) {
                    flush();
                    Run r;
                    style(r);
                    r.footnote = s.substr(i + 2, close - i - 2);
                    out.push_back(r);
                    i = close;
                    continue;
                }
            }
            if ((c == '[' || (c == '!' && i + 1 < s.size() && s[i + 1] == '[')) && link.empty()) {
                const bool image = c == '!';
                std::size_t open = image ? i + 1 : i;
                std::size_t close = closeBracket(open);
                if (close != std::string::npos && close + 1 < s.size() && s[close + 1] == '(') {
                    std::size_t paren = closeParen(close + 1);
                    if (paren != std::string::npos) {
                        std::string inner = s.substr(open + 1, close - open - 1);
                        std::string target = linkTarget(s.substr(close + 2, paren - close - 2));
                        flush();
                        if (image) {
                            buf += inner;  // inline images become their description
                        } else {
                            InlineParser sub{inner};
                            sub.bold = bold;
                            sub.italic = italic;
                            sub.underline = underline;
                            sub.strike = strike;
                            sub.looks = looks;
                            sub.link = target;
                            sub.run();
                            for (auto& r : sub.out) out.push_back(std::move(r));
                        }
                        i = paren;
                        continue;
                    }
                }
            }
            if (c == '<') {
                auto lowerAt = [&](const char* tag) { return lower(s.substr(i, std::strlen(tag))) == tag; };
                if (lowerAt("<u>")) {
                    flush();
                    underline = true;
                    i += 2;
                    continue;
                }
                if (lowerAt("</u>")) {
                    flush();
                    underline = false;
                    i += 3;
                    continue;
                }
                if (lowerAt("<br>") || lowerAt("<br/>") || lowerAt("<br />")) {
                    buf += '\n';
                    i = s.find('>', i);
                    continue;
                }
                if (lookTag(i)) continue;
                std::size_t gt = s.find('>', i);
                if (gt != std::string::npos && link.empty()) {
                    std::string inside = s.substr(i + 1, gt - i - 1);
                    if ((startsWith(inside, "http://") || startsWith(inside, "https://") || startsWith(inside, "mailto:")) &&
                        inside.find(' ') == std::string::npos) {
                        flush();
                        Run r;
                        style(r);
                        r.text = startsWith(inside, "mailto:") ? inside.substr(7) : inside;
                        r.link = inside;
                        out.push_back(r);
                        i = gt;
                        continue;
                    }
                }
            }
            if (c == '~' && i + 1 < s.size() && s[i + 1] == '~') {
                if (strike || hasCloser("~~", i + 2)) {
                    flush();
                    strike = !strike;
                }
                ++i;
                continue;
            }
            if (c == '*' || c == '_') {
                std::size_t n = 0;
                while (i + n < s.size() && s[i + n] == c && n < 3) ++n;
                const char before = i > 0 ? s[i - 1] : ' ';
                const char after = i + n < s.size() ? s[i + n] : ' ';
                if (c == '_' && isAlnum(before) && isAlnum(after)) {  // snake_case stays literal
                    buf.append(n, c);
                    i += n - 1;
                    continue;
                }
                const std::string marker(n, c);
                const bool closing = (n == 3 && bold && italic) || (n == 2 && bold) || (n == 1 && italic);
                if (closing && before != ' ') {
                    flush();
                    if (n >= 2) bold = false;
                    if (n != 2) italic = false;
                    i += n - 1;
                    continue;
                }
                if (!closing && after != ' ' && hasCloser(marker, i + n)) {
                    flush();
                    if (n >= 2) bold = true;
                    if (n != 2) italic = true;
                    i += n - 1;
                    continue;
                }
                buf.append(n, c);
                i += n - 1;
                continue;
            }
            buf += c;
        }
        flush();
    }
};

std::string escapeMd(const std::string& t) {
    std::string out;
    for (std::size_t i = 0; i < t.size(); ++i) {
        char c = t[i];
        if (t.compare(i, 2, "[[") == 0) {  // a notes [[wiki link]] stays as written
            const auto close = t.find("]]", i + 2);
            if (close != std::string::npos && t.find_first_of("[\n", i + 2) >= close) {
                out += t.substr(i, close + 2 - i);
                i = close + 1;
                continue;
            }
        }
        if (c == '\\' || c == '`' || c == '*' || c == '[' || c == ']' || (c == '~' && i + 1 < t.size() && t[i + 1] == '~') ||
            (c == '^' && i + 1 < t.size() && t[i + 1] == '[') || (c == '<' && i + 1 < t.size() && std::isalpha(static_cast<unsigned char>(t[i + 1]))) ||
            (c == '_' && (i == 0 || !isAlnum(t[i - 1]) || i + 1 >= t.size() || !isAlnum(t[i + 1])))) {
            out += '\\';
        }
        out += c;
    }
    return out;
}

}  // namespace

Runs parseInline(const std::string& markdown) {
    InlineParser p{markdown};
    p.run();
    mergeRuns(p.out);
    return p.out;
}

std::string inlineMarkdown(const Runs& runs) {
    std::string out;
    for (std::size_t i = 0; i < runs.size(); ++i) {
        const Run& r = runs[i];
        if (!r.footnote.empty()) {
            out += "^[" + r.footnote + "]";
            continue;
        }
        if (r.text.empty()) continue;
        if (!r.link.empty()) {
            // neighbours with the same link form one [text](url)
            Runs group;
            std::size_t j = i;
            while (j < runs.size() && runs[j].link == r.link) {
                Run g = runs[j++];
                g.link.clear();
                group.push_back(g);
            }
            std::string inner = inlineMarkdown(group);
            std::size_t a = 0, b = inner.size();
            while (a < b && inner[a] == ' ') ++a;
            while (b > a && inner[b - 1] == ' ') --b;
            out += inner.substr(0, a) + "[" + inner.substr(a, b - a) + "](" + r.link + ")" + inner.substr(b);
            i = j - 1;
            continue;
        }
        std::string t = r.text;
        // keep spaces outside the markers
        std::size_t a = 0, b = t.size();
        while (a < b && t[a] == ' ') ++a;
        while (b > a && t[b - 1] == ' ') --b;
        std::string lead = t.substr(0, a), core = t.substr(a, b - a), trail = t.substr(b);
        if (core.empty()) {
            out += t;
            continue;
        }
        std::string body;
        if (r.code) {
            std::string fence = core.find('`') != std::string::npos ? "`` " : "`";
            std::string close = fence == "`" ? "`" : " ``";
            body = fence + core + close;
        } else {
            body = escapeMd(core);
            std::string hb;
            for (char ch : body) hb += ch == '\n' ? std::string("\\\n") : std::string(1, ch);
            body = hb;
        }
        if (r.bold && r.italic) body = "***" + body + "***";
        else if (r.bold) body = "**" + body + "**";
        else if (r.italic) body = "*" + body + "*";
        if (r.underline) body = "<u>" + body + "</u>";
        if (r.strike) body = "~~" + body + "~~";
        if (r.vertical) body = (r.vertical > 0 ? "<sup>" : "<sub>") + body + (r.vertical > 0 ? "</sup>" : "</sub>");
        std::string css;
        if (r.color) css += "color:" + colorName(r.color) + ";";
        if (r.highlight) css += "background:" + colorName(r.highlight) + ";";
        if (r.size > 0) css += "font-size:" + textNumber(r.size) + "pt;";
        if (!r.font.empty()) css += "font-family:" + r.font + ";";
        if (!css.empty()) {
            css.pop_back();
            body = "<span style=\"" + css + "\">" + body + "</span>";
        }
        out += lead + body + trail;
    }
    return out;
}

// ---- Markdown ----------------------------------------------------------------------

namespace {

std::string readBinary(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    if (!in) throw std::runtime_error("cannot open " + p.string());
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

// Image bytes for a src: a data: URI or a local file relative to baseDir. Empty when unavailable.
std::string loadImageSource(const std::string& src, const std::string& baseDir) {
    if (startsWith(src, "data:")) {
        auto comma = src.find(',');
        if (comma == std::string::npos) return {};
        std::string meta = src.substr(5, comma - 5);
        if (meta.find(";base64") == std::string::npos) return {};
        return base64Decode(src.substr(comma + 1));
    }
    if (startsWith(src, "http://") || startsWith(src, "https://")) return {};
    std::string path = src;
    if (startsWith(path, "file://")) path = path.substr(7);
    // %20 and friends
    std::string decoded;
    for (std::size_t i = 0; i < path.size(); ++i) {
        if (path[i] == '%' && i + 2 < path.size() && std::isxdigit(static_cast<unsigned char>(path[i + 1])) &&
            std::isxdigit(static_cast<unsigned char>(path[i + 2]))) {
            decoded += static_cast<char>(std::stoi(path.substr(i + 1, 2), nullptr, 16));
            i += 2;
        } else {
            decoded += path[i];
        }
    }
    fs::path p = pathFromUtf8(decoded);
    if (p.is_relative() && !baseDir.empty()) p = pathFromUtf8(baseDir) / p;
    std::error_code ec;
    if (!fs::is_regular_file(p, ec)) return {};
    try {
        return readBinary(p);
    } catch (const std::exception&) {
        return {};
    }
}

bool isRule(const std::string& t) {
    if (t.size() < 3) return false;
    char c = t[0];
    if (c != '-' && c != '*' && c != '_') return false;
    int n = 0;
    for (char ch : t) {
        if (ch == c) ++n;
        else if (ch != ' ') return false;
    }
    return n >= 3;
}

// "- item", "* item", "+ item", "1. item", "2) item": marker found -> indent, ordered, text.
bool listMarker(const std::string& line, int& indent, bool& ordered, std::string& text) {
    std::size_t i = 0;
    int ind = 0;
    while (i < line.size() && (line[i] == ' ' || line[i] == '\t')) {
        ind += line[i] == '\t' ? 4 : 1;
        ++i;
    }
    if (i >= line.size()) return false;
    std::size_t j = i;
    if (line[j] == '-' || line[j] == '*' || line[j] == '+') {
        ++j;
        ordered = false;
    } else {
        while (j < line.size() && std::isdigit(static_cast<unsigned char>(line[j])) && j - i < 9) ++j;
        if (j == i || j >= line.size() || (line[j] != '.' && line[j] != ')')) return false;
        ++j;
        ordered = true;
    }
    if (j < line.size() && line[j] != ' ' && line[j] != '\t') return false;
    while (j < line.size() && (line[j] == ' ' || line[j] == '\t')) ++j;
    indent = ind;
    text = line.substr(j);
    // task list boxes read as plain text
    if (startsWith(text, "[ ] ") || startsWith(text, "[x] ") || startsWith(text, "[X] ")) text = (text[1] == ' ' ? "\xE2\x98\x90 " : "\xE2\x98\x91 ") + text.substr(4);
    return true;
}

std::vector<std::string> tableCells(std::string line) {
    line = trimCopy(line);
    if (startsWith(line, "|")) line.erase(0, 1);
    if (endsWith(line, "|") && !endsWith(line, "\\|")) line.pop_back();
    std::vector<std::string> cells;
    std::string cur;
    for (std::size_t i = 0; i < line.size(); ++i) {
        if (line[i] == '\\' && i + 1 < line.size() && line[i + 1] == '|') {
            cur += '|';
            ++i;
        } else if (line[i] == '|') {
            cells.push_back(trimCopy(cur));
            cur.clear();
        } else {
            cur += line[i];
        }
    }
    cells.push_back(trimCopy(cur));
    return cells;
}

bool isTableSeparator(const std::string& line) {
    std::string t = trimCopy(line);
    if (t.find('-') == std::string::npos || t.find('|') == std::string::npos) {
        if (t.find('-') == std::string::npos) return false;
    }
    for (char c : t) {
        if (c != '|' && c != '-' && c != ':' && c != ' ') return false;
    }
    return t.find("--") != std::string::npos || t.find("-|") != std::string::npos;
}

// Moves "[^label]: text" definitions into the references as inline notes "^[text]".
std::vector<std::string> inlineFootnotes(const std::vector<std::string>& lines) {
    std::map<std::string, std::string> defs;
    std::vector<std::string> body;
    for (std::size_t i = 0; i < lines.size(); ++i) {
        const std::string t = trimCopy(lines[i]);
        if (startsWith(t, "[^")) {
            const auto close = t.find("]:");
            if (close != std::string::npos) {
                std::string text = trimCopy(t.substr(close + 2));
                while (i + 1 < lines.size() && (startsWith(lines[i + 1], "    ") || startsWith(lines[i + 1], "\t")) && !trimCopy(lines[i + 1]).empty()) {
                    text += " " + trimCopy(lines[++i]);
                }
                defs[t.substr(2, close - 2)] = text;
                continue;
            }
        }
        body.push_back(lines[i]);
    }
    if (defs.empty()) return body;
    for (auto& line : body) {
        std::string out;
        for (std::size_t i = 0; i < line.size(); ++i) {
            if (line[i] == '[' && i + 1 < line.size() && line[i + 1] == '^' && (i == 0 || line[i - 1] != '\\')) {
                const auto close = line.find(']', i);
                if (close != std::string::npos) {
                    auto it = defs.find(line.substr(i + 2, close - i - 2));
                    if (it != defs.end()) {
                        out += "^[" + it->second + "]";
                        i = close;
                        continue;
                    }
                }
            }
            out += line[i];
        }
        line = out;
    }
    return body;
}

// The other way: inline notes "^[text]" become numbered references, with the notes listed in `notes`.
std::string footnoteRefs(const std::string& md, std::vector<std::string>& notes) {
    std::string out;
    bool fenced = false;
    for (std::size_t i = 0; i < md.size(); ++i) {
        if ((i == 0 || md[i - 1] == '\n') && md.compare(i, 3, "```") == 0) fenced = !fenced;
        if (fenced) {
            out += md[i];
            continue;
        }
        if (md[i] == '`') {  // code spans stay as they are
            std::size_t n = 0;
            while (i + n < md.size() && md[i + n] == '`') ++n;
            const std::size_t close = md.find(std::string(n, '`'), i + n);
            if (close != std::string::npos && md.find('\n', i) > close) {
                out += md.substr(i, close + n - i);
                i = close + n - 1;
                continue;
            }
        }
        if (md[i] == '\\' && i + 1 < md.size()) {
            out += md[i];
            out += md[++i];
            continue;
        }
        if (md[i] == '^' && i + 1 < md.size() && md[i + 1] == '[') {
            int depth = 0;
            std::size_t k = i + 1;
            for (; k < md.size(); ++k) {
                if (md[k] == '\\') {
                    ++k;
                    continue;
                }
                if (md[k] == '[') ++depth;
                if (md[k] == ']' && --depth == 0) break;
            }
            if (k < md.size()) {
                notes.push_back(md.substr(i + 2, k - i - 2));
                out += "[^" + std::to_string(notes.size()) + "]";
                i = k;
                continue;
            }
        }
        out += md[i];
    }
    return out;
}

bool isPageBreakLine(const std::string& t) {
    std::string l = lower(trimCopy(t));
    return l == "<!-- pagebreak -->" || l == "\\pagebreak" || l == "\\newpage" || l == "<div style=\"page-break-after: always\"></div>" ||
           l == "<div style=\"page-break-before: always\"></div>";
}

}  // namespace

TextDocument readMarkdown(const std::string& text, const std::string& baseDir) {
    TextDocument doc;
    std::vector<std::string> lines = inlineFootnotes(splitLines(text));
    std::size_t i = 0;
    // YAML front matter title
    if (!lines.empty() && trimCopy(lines[0]) == "---") {
        std::size_t k = 1;
        while (k < lines.size() && trimCopy(lines[k]) != "---" && trimCopy(lines[k]) != "...") ++k;
        if (k < lines.size()) {
            for (std::size_t m = 1; m < k; ++m) {
                std::string l = lines[m];
                auto colon = l.find(':');
                if (colon == std::string::npos) continue;
                std::string key = lower(trimCopy(l.substr(0, colon))), v = trimCopy(l.substr(colon + 1));
                if (v.size() >= 2 && (v.front() == '"' || v.front() == '\'') && v.back() == v.front()) v = v.substr(1, v.size() - 2);
                if (key == "title") doc.title = v;
                if (key == "author") doc.author = v;
            }
            i = k + 1;
        }
    }
    std::string para;
    auto flushPara = [&] {
        std::string t = trimCopy(para);
        para.clear();
        if (t.empty()) return;
        // a paragraph that is just an image becomes an image block
        if (startsWith(t, "![")) {
            auto close = t.find("](");
            if (close != std::string::npos && t.back() == ')' && t.find(')', close) == t.size() - 1) {
                std::string alt = t.substr(2, close - 2);
                std::string src = t.substr(close + 2, t.size() - close - 3);
                auto sp = src.find(" \"");
                if (sp != std::string::npos) src = src.substr(0, sp);
                std::string bytes = loadImageSource(trimCopy(src), baseDir);
                if (!bytes.empty()) {
                    Block b;
                    try {
                        setImage(b, bytes);
                        b.alt = alt;
                        doc.blocks.push_back(std::move(b));
                        return;
                    } catch (const std::exception&) {
                    }
                }
            }
        }
        Block b;
        b.kind = Block::Paragraph;
        b.runs = parseInline(t);
        doc.blocks.push_back(std::move(b));
    };
    auto addLine = [&](const std::string& raw) {
        std::string l = raw;
        bool hard = endsWith(l, "  ") || (endsWith(l, "\\") && !endsWith(l, "\\\\"));
        if (endsWith(l, "\\")) l.pop_back();
        l = trimCopy(l);
        if (!para.empty()) para += para.back() == '\n' ? "" : " ";
        para += l;
        if (hard) para += "<br>";
    };

    while (i < lines.size()) {
        const std::string& line = lines[i];
        const std::string t = trimCopy(line);
        if (t.empty()) {
            flushPara();
            ++i;
            continue;
        }
        if (isPageBreakLine(t)) {
            flushPara();
            Block b;
            b.kind = Block::PageBreak;
            doc.blocks.push_back(b);
            ++i;
            continue;
        }
        if (lower(t) == "[toc]" || lower(t) == "<!-- toc -->" || lower(t) == "\\toc" || lower(t) == "\\tableofcontents") {
            flushPara();
            Block b;
            b.kind = Block::Toc;
            b.level = 3;
            b.runs = parseInline("Contents");
            doc.blocks.push_back(b);
            ++i;
            continue;
        }
        if (startsWith(t, "<!--")) {  // other comments are skipped
            flushPara();
            while (i < lines.size() && lines[i].find("-->") == std::string::npos) ++i;
            ++i;
            continue;
        }
        // fenced code
        if (startsWith(t, "```") || startsWith(t, "~~~")) {
            flushPara();
            const std::string fence = t.substr(0, 3);
            std::string code;
            ++i;
            bool first = true;
            while (i < lines.size() && !startsWith(trimCopy(lines[i]), fence)) {
                code += (first ? "" : "\n") + lines[i];
                first = false;
                ++i;
            }
            ++i;
            Block b;
            b.kind = Block::Code;
            Run r;
            r.text = code;
            r.code = true;
            b.runs.push_back(r);
            doc.blocks.push_back(std::move(b));
            continue;
        }
        // ATX heading
        if (t[0] == '#') {
            std::size_t n = 0;
            while (n < t.size() && t[n] == '#') ++n;
            if (n <= 6 && (n == t.size() || t[n] == ' ')) {
                flushPara();
                std::string h = trimCopy(t.substr(n));
                while (!h.empty() && h.back() == '#') h.pop_back();
                Block b;
                b.kind = Block::Heading;
                b.level = static_cast<int>(n);
                b.runs = parseInline(trimCopy(h));
                doc.blocks.push_back(std::move(b));
                ++i;
                continue;
            }
        }
        // setext heading
        if (!para.empty() && (t.find_first_not_of('=') == std::string::npos || (t.find_first_not_of('-') == std::string::npos && t.size() >= 2))) {
            Block b;
            b.kind = Block::Heading;
            b.level = t[0] == '=' ? 1 : 2;
            b.runs = parseInline(trimCopy(para));
            para.clear();
            doc.blocks.push_back(std::move(b));
            ++i;
            continue;
        }
        if (isRule(t)) {  // horizontal rules have no block of their own
            flushPara();
            ++i;
            continue;
        }
        // block quote
        if (t[0] == '>') {
            flushPara();
            std::string q;
            while (i < lines.size() && !trimCopy(lines[i]).empty() && trimCopy(lines[i])[0] == '>') {
                std::string l = trimCopy(lines[i]).substr(1);
                if (!l.empty() && l[0] == ' ') l.erase(0, 1);
                q += (q.empty() ? "" : " ") + l;
                ++i;
            }
            Block b;
            b.kind = Block::Quote;
            b.runs = parseInline(q);
            doc.blocks.push_back(std::move(b));
            continue;
        }
        // table
        if (t.find('|') != std::string::npos && i + 1 < lines.size() && isTableSeparator(lines[i + 1])) {
            flushPara();
            Block b;
            b.kind = Block::Table;
            b.header = true;
            std::vector<Runs> head;
            for (const auto& c : tableCells(t)) head.push_back(parseInline(c));
            b.rows.push_back(head);
            i += 2;
            while (i < lines.size() && trimCopy(lines[i]).find('|') != std::string::npos && !trimCopy(lines[i]).empty()) {
                std::vector<Runs> row;
                for (const auto& c : tableCells(lines[i])) row.push_back(parseInline(c));
                row.resize(head.size());
                b.rows.push_back(row);
                ++i;
            }
            doc.blocks.push_back(std::move(b));
            continue;
        }
        // list
        int indent;
        bool ordered;
        std::string itemText;
        if (listMarker(line, indent, ordered, itemText) && (para.empty() || indent == 0)) {
            flushPara();
            Block b;
            b.kind = Block::List;
            b.ordered = ordered;
            std::vector<int> indents;  // indentation of each open level
            while (i < lines.size()) {
                const std::string& l = lines[i];
                if (trimCopy(l).empty()) {
                    // a blank line ends the list unless another item follows
                    std::size_t k = i + 1;
                    while (k < lines.size() && trimCopy(lines[k]).empty()) ++k;
                    int ind2;
                    bool ord2;
                    std::string t2;
                    if (k < lines.size() && listMarker(lines[k], ind2, ord2, t2) && (ind2 > 0 || ord2 == b.ordered)) {
                        i = k;
                        continue;
                    }
                    break;
                }
                int ind;
                bool ord;
                std::string txt;
                if (listMarker(l, ind, ord, txt)) {
                    if (indents.empty() || ind > indents.back()) {
                        if (!indents.empty() || b.items.empty()) indents.push_back(ind);
                        else indents.push_back(ind);
                    } else {
                        while (indents.size() > 1 && ind < indents.back()) indents.pop_back();
                    }
                    if (indents.size() == 1 && ord != b.ordered && !b.items.empty()) break;  // a different list starts
                    if (b.items.empty() && ord) b.start = std::max(0, std::atoi(trimCopy(l).c_str()));  // "3. third" starts at 3
                    ListItem it;
                    it.level = static_cast<int>(indents.size()) - 1;
                    it.runs = parseInline(txt);
                    b.items.push_back(std::move(it));
                    ++i;
                    continue;
                }
                // continuation line of the last item
                if (!b.items.empty() && (l[0] == ' ' || l[0] == '\t')) {
                    Runs more = parseInline(" " + trimCopy(l));
                    for (auto& r : more) b.items.back().runs.push_back(r);
                    mergeRuns(b.items.back().runs);
                    ++i;
                    continue;
                }
                break;
            }
            doc.blocks.push_back(std::move(b));
            continue;
        }
        addLine(line);
        ++i;
    }
    flushPara();
    if (doc.title.empty()) {
        for (const auto& b : doc.blocks) {
            if (b.kind == Block::Heading && b.level == 1) {
                doc.title = plainText(b.runs);
                break;
            }
        }
    }
    return doc;
}

std::string writeMarkdown(const TextDocument& doc, const std::string& mediaPrefix, std::vector<MediaFile>& media) {
    std::string out;
    auto sep = [&] {
        if (!out.empty()) out += "\n";
    };
    for (const auto& b : doc.blocks) {
        switch (b.kind) {
            case Block::Heading:
                sep();
                out += std::string(static_cast<std::size_t>(std::clamp(b.level, 1, 6)), '#') + " " + inlineMarkdown(b.runs) + "\n";
                break;
            case Block::Paragraph:
                sep();
                out += inlineMarkdown(b.runs) + "\n";
                break;
            case Block::Quote:
                sep();
                out += "> " + inlineMarkdown(b.runs) + "\n";
                break;
            case Block::Code:
                sep();
                out += "```\n" + plainText(b.runs) + "\n```\n";
                break;
            case Block::PageBreak:
                sep();
                out += "<!-- pagebreak -->\n";
                break;
            case Block::Toc:
                sep();
                out += "[TOC]\n";
                break;
            case Block::List: {
                sep();
                std::vector<int> counters(10, 0);
                counters[0] = b.start - 1;
                for (const auto& it : b.items) {
                    const int lv = std::clamp(it.level, 0, 9);
                    for (int k = lv + 1; k < 10; ++k) counters[static_cast<std::size_t>(k)] = 0;
                    std::string marker = b.ordered ? std::to_string(++counters[static_cast<std::size_t>(lv)]) + ". " : "- ";
                    std::string item = inlineMarkdown(it.runs);
                    // task boxes go back to Markdown's own [ ] / [x]
                    if (item.rfind("\xE2\x98\x90", 0) == 0) item = "[ ]" + item.substr(3);
                    else if (item.rfind("\xE2\x98\x91", 0) == 0) item = "[x]" + item.substr(3);
                    out += std::string(static_cast<std::size_t>(lv) * 4, ' ') + marker + item + "\n";
                }
                break;
            }
            case Block::Table: {
                if (b.rows.empty()) break;
                sep();
                std::size_t cols = 0;
                for (const auto& r : b.rows) cols = std::max(cols, r.size());
                auto cell = [](const Runs& r) {
                    std::string s = inlineMarkdown(r), o;
                    for (std::size_t k = 0; k < s.size(); ++k) {
                        const char c = s[k];
                        if (c == '\\' && k + 1 < s.size() && s[k + 1] == '\n') continue;  // a hard break is <br> here
                        if (c == '|') o += "\\|";
                        else if (c == '\n') o += "<br>";
                        else o += c;
                    }
                    return o;
                };
                for (std::size_t r = 0; r < b.rows.size(); ++r) {
                    std::string line = "|";
                    for (std::size_t c = 0; c < cols; ++c) line += " " + (c < b.rows[r].size() ? cell(b.rows[r][c]) : std::string()) + " |";
                    out += line + "\n";
                    if (r == 0) {
                        std::string s = "|";
                        for (std::size_t c = 0; c < cols; ++c) s += " --- |";
                        out += s + "\n";
                    }
                }
                break;
            }
            case Block::Image: {
                sep();
                const bool jpeg = b.image.size() > 2 && static_cast<unsigned char>(b.image[0]) == 0xFF;
                std::string path = mediaPrefix + "image" + std::to_string(media.size() + 1) + (jpeg ? ".jpg" : ".png");
                media.push_back({path, b.image});
                std::string p;
                for (char c : path) p += c == ' ' ? std::string("%20") : std::string(1, c);
                out += "![" + b.alt + "](" + p + ")\n";
                break;
            }
        }
    }
    std::vector<std::string> notes;
    out = footnoteRefs(out, notes);
    if (!notes.empty()) {
        out += "\n";
        for (std::size_t n = 0; n < notes.size(); ++n) out += "[^" + std::to_string(n + 1) + "]: " + notes[n] + "\n";
    }
    return out;
}

// ---- plain text ------------------------------------------------------------------------

std::string writeText(const TextDocument& doc) {
    std::string out;
    for (const auto& b : doc.blocks) {
        if (!out.empty()) out += "\n";
        switch (b.kind) {
            case Block::Heading: {
                std::string t = plainText(b.runs);
                out += t + "\n";
                if (b.level <= 2) out += std::string(pdf::utf8Decode(t).size(), b.level == 1 ? '=' : '-') + "\n";
                break;
            }
            case Block::List: {
                int n[10] = {0};
                for (const auto& it : b.items) {
                    int lv = std::clamp(it.level, 0, 9);
                    for (int k = lv + 1; k < 10; ++k) n[k] = 0;
                    out += std::string(static_cast<std::size_t>(lv) * 2, ' ') + (b.ordered ? std::to_string(++n[lv]) + ". " : "- ") +
                           plainText(it.runs) + "\n";
                }
                break;
            }
            case Block::PageBreak: out += "\f\n"; break;
            case Block::Toc: {
                out += blockText(b) + "\n";
                for (const auto& [lv, t] : outline(doc, b.level)) out += std::string(static_cast<std::size_t>(lv - 1) * 2, ' ') + t + "\n";
                break;
            }
            case Block::Image: out += "[image" + (b.alt.empty() ? std::string() : ": " + b.alt) + "]\n"; break;
            case Block::Quote: out += "    " + plainText(b.runs) + "\n"; break;
            default: out += blockText(b) + "\n";
        }
    }
    return out;
}

// ---- HTML -----------------------------------------------------------------------------

namespace {

std::string htmlEscape(const std::string& s) {
    std::string o;
    for (char c : s) {
        switch (c) {
            case '&': o += "&amp;"; break;
            case '<': o += "&lt;"; break;
            case '>': o += "&gt;"; break;
            case '"': o += "&quot;"; break;
            default: o += c;
        }
    }
    return o;
}

std::string htmlRuns(const Runs& runs, std::vector<std::string>* notes = nullptr) {
    std::string o;
    for (const auto& r : runs) {
        if (!r.footnote.empty()) {
            if (notes) {
                notes->push_back(r.footnote);
                const std::string n = std::to_string(notes->size());
                o += "<sup class=\"fn\"><a href=\"#fn" + n + "\" id=\"fnref" + n + "\">" + n + "</a></sup>";
            }
            continue;
        }
        std::string t = htmlEscape(r.text), br;
        for (char c : t) br += c == '\n' ? std::string("<br>") : std::string(1, c);
        t = br;
        if (r.code) t = "<code>" + t + "</code>";
        if (r.underline) t = "<u>" + t + "</u>";
        if (r.strike) t = "<s>" + t + "</s>";
        if (r.vertical) t = (r.vertical > 0 ? "<sup>" : "<sub>") + t + (r.vertical > 0 ? "</sup>" : "</sub>");
        if (r.italic) t = "<em>" + t + "</em>";
        if (r.bold) t = "<strong>" + t + "</strong>";
        std::string css;
        if (r.color) css += "color:" + colorName(r.color) + ";";
        if (r.highlight) css += "background:" + colorName(r.highlight) + ";";
        if (r.size > 0) css += "font-size:" + textNumber(r.size) + "pt;";
        if (r.font == "serif") css += "font-family:Georgia,serif;";
        else if (r.font == "mono") css += "font-family:Courier,monospace;";
        else if (r.font == "sans") css += "font-family:Helvetica,Arial,sans-serif;";
        if (!css.empty()) t = "<span style=\"" + css + "\">" + t + "</span>";
        if (!r.link.empty()) t = "<a href=\"" + htmlEscape(r.link) + "\">" + t + "</a>";
        o += t;
    }
    return o;
}

std::string htmlParaStyle(const ParaFormat& f) {
    std::string css;
    if (f.align) css += std::string("text-align:") + (f.align == 'c' ? "center" : f.align == 'r' ? "right" : f.align == 'j' ? "justify" : "left") + ";";
    if (f.indent) css += "margin-left:" + textNumber(f.indent) + "pt;";
    if (f.indentRight) css += "margin-right:" + textNumber(f.indentRight) + "pt;";
    if (f.firstLine) css += "text-indent:" + textNumber(f.firstLine) + "pt;";
    if (f.before >= 0) css += "margin-top:" + textNumber(f.before) + "pt;";
    if (f.after >= 0) css += "margin-bottom:" + textNumber(f.after) + "pt;";
    if (f.lineSpacing > 0) css += "line-height:" + textNumber(f.lineSpacing) + ";";
    if (f.pageBreakBefore) css += "page-break-before:always;";
    return css.empty() ? std::string() : " style=\"" + css + "\"";
}

}  // namespace

std::string writeHtml(const TextDocument& doc) {
    std::string o = "<!DOCTYPE html>\n<html>\n<head>\n<meta charset=\"utf-8\">\n<meta name=\"viewport\" content=\"width=device-width, initial-scale=1\">\n<title>" +
                    htmlEscape(doc.title.empty() ? "TextDocument" : doc.title) + "</title>\n";
    if (!doc.author.empty()) o += "<meta name=\"author\" content=\"" + htmlEscape(doc.author) + "\">\n";
    o += "<style>\nbody{font-family:" + std::string(doc.page.font == pdf::Family::Serif ? "Georgia,'Times New Roman',serif" : "Helvetica,Arial,sans-serif") +
         ";max-width:46em;margin:2em auto;padding:0 1em;line-height:1.45;color:#222}\n"
         "table{border-collapse:collapse;margin:1em 0}td,th{border:1px solid #999;padding:.3em .6em;vertical-align:top}th{background:#eee}\n"
         "pre{background:#f4f4f4;padding:.8em;overflow:auto}code{font-family:Courier,monospace}blockquote{border-left:4px solid #ccc;margin-left:0;padding-left:1em;color:#555}\n"
         "img{max-width:100%}\n@media print{.page-break{page-break-after:always}}\n</style>\n</head>\n<body>\n";
    std::vector<std::string> notes;
    int headingNo = 0;
    for (const auto& b : doc.blocks) {
        const std::string ps = htmlParaStyle(b.fmt);
        switch (b.kind) {
            case Block::Heading: {
                std::string n = std::to_string(std::clamp(b.level, 1, 6));
                o += "<h" + n + " id=\"h" + std::to_string(++headingNo) + "\"" + ps + ">" + htmlRuns(b.runs, &notes) + "</h" + n + ">\n";
                break;
            }
            case Block::Paragraph: {
                std::string cls;
                if (!b.style.empty() && b.style != "Normal") cls = " class=\"" + htmlEscape(b.style) + "\"";
                if (b.style == "Title") o += "<h1 class=\"Title\"" + ps + ">" + htmlRuns(b.runs, &notes) + "</h1>\n";
                else o += "<p" + cls + ps + ">" + htmlRuns(b.runs, &notes) + "</p>\n";
                break;
            }
            case Block::Toc: {
                o += "<nav class=\"toc\"><p><strong>" + htmlRuns(b.runs) + "</strong></p>\n";
                int k = 0;
                for (const auto& x : doc.blocks) {
                    if (x.kind != Block::Heading) continue;
                    ++k;
                    if (x.level > b.level) continue;
                    o += "<p style=\"margin:0 0 0 " + std::to_string((x.level - 1) * 18) + "pt\"><a href=\"#h" + std::to_string(k) + "\">" +
                         htmlEscape(plainText(x.runs)) + "</a></p>\n";
                }
                o += "</nav>\n";
                break;
            }
            case Block::Quote: o += "<blockquote><p" + ps + ">" + htmlRuns(b.runs, &notes) + "</p></blockquote>\n"; break;
            case Block::Code: o += "<pre><code>" + htmlEscape(plainText(b.runs)) + "</code></pre>\n"; break;
            case Block::PageBreak: o += "<div class=\"page-break\" style=\"page-break-after: always\"></div>\n"; break;
            case Block::List: {
                const std::string tag = b.ordered ? "ol" : "ul";
                std::string listType;
                if (b.numbering == "lowerLetter") listType = " type=\"a\"";
                else if (b.numbering == "upperLetter") listType = " type=\"A\"";
                else if (b.numbering == "lowerRoman") listType = " type=\"i\"";
                else if (b.numbering == "upperRoman") listType = " type=\"I\"";
                if (b.ordered && b.start != 1) listType += " start=\"" + std::to_string(b.start) + "\"";
                int depth = -1;
                for (const auto& it : b.items) {
                    const int lv = std::min(std::max(0, it.level), depth + 1);
                    if (lv > depth) {
                        o += "<" + tag + (depth < 0 ? listType : std::string()) + ">\n";
                        depth = lv;
                    } else {
                        o += "</li>\n";
                        while (depth > lv) {
                            o += "</" + tag + "></li>\n";
                            --depth;
                        }
                    }
                    o += "<li>" + htmlRuns(it.runs, &notes);
                }
                if (depth >= 0) o += "</li>\n";
                while (depth > 0) {
                    o += "</" + tag + "></li>\n";
                    --depth;
                }
                if (depth == 0) o += "</" + tag + ">\n";
                break;
            }
            case Block::Table: {
                o += "<table>\n";
                auto spanAt = [&](int r, int c) -> const CellSpan* {
                    for (const auto& m : b.merges) {
                        if (r >= m.row && r < m.row + m.rows && c >= m.col && c < m.col + m.cols) return &m;
                    }
                    return nullptr;
                };
                for (std::size_t r = 0; r < b.rows.size(); ++r) {
                    const bool head = r == 0 && b.header;
                    o += "<tr>";
                    for (std::size_t c = 0; c < b.rows[r].size(); ++c) {
                        const CellSpan* m = spanAt(static_cast<int>(r), static_cast<int>(c));
                        if (m && (m->row != static_cast<int>(r) || m->col != static_cast<int>(c))) continue;
                        std::string attrs;
                        if (m && m->rows > 1) attrs += " rowspan=\"" + std::to_string(m->rows) + "\"";
                        if (m && m->cols > 1) attrs += " colspan=\"" + std::to_string(m->cols) + "\"";
                        if (r < b.cellFormats.size() && c < b.cellFormats[r].size()) {
                            const CellFormat& f = b.cellFormats[r][c];
                            std::string css;
                            if (f.fill) css += "background:" + colorName(f.fill) + ";";
                            if (f.align) css += std::string("text-align:") + (f.align == 'c' ? "center" : f.align == 'r' ? "right" : "left") + ";";
                            if (f.valign) css += std::string("vertical-align:") + (f.valign == 'm' ? "middle" : "bottom") + ";";
                            if (!css.empty()) attrs += " style=\"" + css + "\"";
                        }
                        o += (head ? "<th" : "<td") + attrs + ">" + htmlRuns(b.rows[r][c], &notes) + (head ? "</th>" : "</td>");
                    }
                    o += "</tr>\n";
                }
                o += "</table>\n";
                break;
            }
            case Block::Image: {
                const bool jpeg = b.image.size() > 2 && static_cast<unsigned char>(b.image[0]) == 0xFF;
                o += "<p><img src=\"data:image/" + std::string(jpeg ? "jpeg" : "png") + ";base64," + base64Encode(b.image) + "\" alt=\"" + htmlEscape(b.alt) + "\"";
                if (b.width > 0) o += " style=\"width:" + textNumber(std::round(b.width * 4 / 3)) + "px\"";
                o += "></p>\n";
                break;
            }
        }
    }
    if (!notes.empty()) {
        o += "<section class=\"footnotes\"><hr>\n<ol>\n";
        for (std::size_t n = 0; n < notes.size(); ++n) {
            o += "<li id=\"fn" + std::to_string(n + 1) + "\">" + htmlRuns(parseInline(notes[n])) + " <a href=\"#fnref" + std::to_string(n + 1) + "\">&#8617;</a></li>\n";
        }
        o += "</ol>\n</section>\n";
    }
    o += "</body>\n</html>\n";
    return o;
}

namespace {

std::string decodeHtmlEntities(const std::string& s) {
    static const std::map<std::string, unsigned> named = {
        {"amp", '&'},     {"lt", '<'},       {"gt", '>'},      {"quot", '"'},    {"apos", '\''},   {"nbsp", 0xA0},  {"copy", 0xA9},
        {"reg", 0xAE},    {"trade", 0x2122}, {"mdash", 0x2014}, {"ndash", 0x2013}, {"hellip", 0x2026}, {"lsquo", 0x2018}, {"rsquo", 0x2019},
        {"ldquo", 0x201C}, {"rdquo", 0x201D}, {"bull", 0x2022}, {"euro", 0x20AC}, {"pound", 0xA3},  {"yen", 0xA5},    {"deg", 0xB0},
        {"middot", 0xB7}, {"times", 0xD7},   {"eacute", 0xE9}, {"egrave", 0xE8}, {"agrave", 0xE0}, {"uuml", 0xFC},   {"ouml", 0xF6},
        {"auml", 0xE4},   {"szlig", 0xDF},   {"ccedil", 0xE7}, {"laquo", 0xAB},  {"raquo", 0xBB},  {"sect", 0xA7},   {"para", 0xB6},
    };
    std::string out;
    for (std::size_t i = 0; i < s.size(); ++i) {
        if (s[i] != '&') {
            out += s[i];
            continue;
        }
        auto semi = s.find(';', i);
        if (semi == std::string::npos || semi - i > 10) {
            out += '&';
            continue;
        }
        std::string ent = s.substr(i + 1, semi - i - 1);
        unsigned cp = 0;
        if (!ent.empty() && ent[0] == '#') {
            cp = (ent.size() > 1 && (ent[1] == 'x' || ent[1] == 'X')) ? static_cast<unsigned>(std::strtoul(ent.c_str() + 2, nullptr, 16))
                                                                      : static_cast<unsigned>(std::strtoul(ent.c_str() + 1, nullptr, 10));
        } else if (auto it = named.find(ent); it != named.end()) {
            cp = it->second;
        }
        if (!cp) {
            out += '&';
            continue;
        }
        out += pdf::utf8Encode(cp);
        i = semi;
    }
    return out;
}

struct HtmlTag {
    std::string name;  // lower case, without '/'
    bool closing = false, selfClosing = false;
    std::map<std::string, std::string> attrs;
};

// Splits HTML into text and tags; script/style/head content is dropped.
struct HtmlReader {
    TextDocument doc;
    std::string baseDir;
    Runs runs;
    int bold = 0, italic = 0, underline = 0, code = 0, pre = 0, strike = 0, sup = 0, sub = 0, mark = 0;
    std::vector<std::string> links;
    enum Ctx { None, Para, Heading, Quote };
    int headingLevel = 0;
    bool inQuote = false;
    struct ListCtx {
        bool ordered;
        Block block;
    };
    std::vector<bool> listStack;  // ordered flags of open lists
    Block list;
    bool inList = false;
    bool inItem = false;
    // tables
    bool inTable = false;
    int tableDepth = 0;
    Block table;
    std::vector<Runs> row;
    bool inCell = false;
    bool headerRow = false;

    void text(const std::string& raw) {
        std::string t = decodeHtmlEntities(raw);
        if (!pre) {
            std::string c;
            for (char ch : t) {
                bool ws = ch == ' ' || ch == '\n' || ch == '\t' || ch == '\r';
                if (ws) {
                    bool prevWs = !c.empty() ? c.back() == ' ' : (!runs.empty() && !runs.back().text.empty() && (runs.back().text.back() == ' ' || runs.back().text.back() == '\n'));
                    if (!prevWs && !(c.empty() && runs.empty())) c += ' ';
                } else {
                    c += ch;
                }
            }
            t = c;
        }
        if (t.empty()) return;
        Run r;
        r.text = t;
        r.bold = bold > 0;
        r.italic = italic > 0;
        r.underline = underline > 0;
        r.code = code > 0 && !pre;
        r.strike = strike > 0;
        r.vertical = sup > 0 ? 1 : sub > 0 ? -1 : 0;
        if (mark > 0) r.highlight = 0xFFFF00 | kColorSet;
        if (!links.empty()) r.link = links.back();
        runs.push_back(r);
    }

    Runs takeRuns() {
        mergeRuns(runs);
        // trim the ends
        while (!runs.empty() && trimCopy(runs.front().text).empty() && runs.front().text.find('\n') == std::string::npos) runs.erase(runs.begin());
        if (!runs.empty()) {
            auto& f = runs.front().text;
            f.erase(0, f.find_first_not_of(' '));
            auto& l = runs.back().text;
            while (!l.empty() && l.back() == ' ') l.pop_back();
        }
        mergeRuns(runs);
        Runs out = std::move(runs);
        runs.clear();
        return out;
    }

    void flushBlock() {
        if (inCell) return;  // cell content is collected on </td>
        if (inList && inItem) {
            Runs r = takeRuns();
            if (!r.empty()) {
                ListItem it;
                it.level = static_cast<int>(listStack.size()) - 1;
                it.runs = r;
                list.items.push_back(it);
            }
            return;
        }
        Runs r = takeRuns();
        if (r.empty()) return;
        Block b;
        if (pre) {
            b.kind = Block::Code;
            Run cr;
            cr.text = plainText(r);
            while (!cr.text.empty() && cr.text.back() == '\n') cr.text.pop_back();
            if (!cr.text.empty() && cr.text.front() == '\n') cr.text.erase(0, 1);
            cr.code = true;
            b.runs = {cr};
        } else if (headingLevel) {
            b.kind = Block::Heading;
            b.level = headingLevel;
            b.runs = r;
        } else if (inQuote) {
            b.kind = Block::Quote;
            b.runs = r;
        } else {
            b.runs = r;
        }
        doc.blocks.push_back(std::move(b));
    }

    void tag(const HtmlTag& t) {
        const std::string& n = t.name;
        if (n == "b" || n == "strong") bold += t.closing ? -1 : 1;
        else if (n == "i" || n == "em") italic += t.closing ? -1 : 1;
        else if (n == "u" || n == "ins") underline += t.closing ? -1 : 1;
        else if (n == "s" || n == "del" || n == "strike") strike += t.closing ? -1 : 1;
        else if (n == "sup") sup += t.closing ? -1 : 1;
        else if (n == "sub") sub += t.closing ? -1 : 1;
        else if (n == "mark") mark += t.closing ? -1 : 1;
        else if (n == "code" || n == "kbd" || n == "samp" || n == "tt") code += t.closing ? -1 : 1;
        else if (n == "a") {
            if (t.closing) {
                if (!links.empty()) links.pop_back();
            } else {
                auto it = t.attrs.find("href");
                links.push_back(it == t.attrs.end() ? std::string() : it->second);
            }
        } else if (n == "br") {
            Run r;
            r.text = "\n";
            r.bold = bold > 0;
            runs.push_back(r);
        } else if (n == "p" || n == "div" || n == "section" || n == "article" || n == "header" || n == "footer" || n == "main" || n == "dd" || n == "dt" ||
                   n == "figure" || n == "figcaption" || n == "hr" || n == "nav" || n == "aside") {
            flushBlock();
            if (!t.closing && (n == "div" || n == "p")) {
                auto st = t.attrs.find("style");
                if (st != t.attrs.end() && lower(st->second).find("page-break") != std::string::npos) {
                    Block b;
                    b.kind = Block::PageBreak;
                    doc.blocks.push_back(b);
                } else if (t.attrs.count("class") && t.attrs.at("class").find("page-break") != std::string::npos) {
                    Block b;
                    b.kind = Block::PageBreak;
                    doc.blocks.push_back(b);
                }
            }
        } else if (n.size() == 2 && n[0] == 'h' && n[1] >= '1' && n[1] <= '6') {
            flushBlock();
            headingLevel = t.closing ? 0 : n[1] - '0';
        } else if (n == "blockquote") {
            flushBlock();
            inQuote = !t.closing;
        } else if (n == "pre") {
            flushBlock();
            pre += t.closing ? -1 : 1;
            if (pre < 0) pre = 0;
        } else if (n == "ul" || n == "ol") {
            if (inCell) return;
            if (!t.closing) {
                if (inList) flushBlock();
                else {
                    flushBlock();
                    list = Block();
                    list.kind = Block::List;
                    list.ordered = n == "ol";
                    inList = true;
                }
                listStack.push_back(n == "ol");
                inItem = false;
            } else {
                flushBlock();
                if (!listStack.empty()) listStack.pop_back();
                inItem = !listStack.empty();
                if (listStack.empty() && inList) {
                    inList = false;
                    inItem = false;
                    if (!list.items.empty()) doc.blocks.push_back(std::move(list));
                    list = Block();
                }
            }
        } else if (n == "li") {
            if (inCell) return;
            flushBlock();
            inItem = !t.closing || !listStack.empty();
        } else if (n == "table") {
            if (t.closing) {
                if (--tableDepth == 0 && inTable) {
                    if (!row.empty()) table.rows.push_back(row);
                    row.clear();
                    inTable = false;
                    if (!table.rows.empty()) {
                        std::size_t cols = 0;
                        for (const auto& r : table.rows) cols = std::max(cols, r.size());
                        for (auto& r : table.rows) r.resize(cols);
                        doc.blocks.push_back(std::move(table));
                    }
                    table = Block();
                }
            } else {
                if (tableDepth++ == 0) {
                    flushBlock();
                    inTable = true;
                    table = Block();
                    table.kind = Block::Table;
                    table.header = false;
                }
            }
        } else if (n == "tr" && inTable && tableDepth == 1) {
            if (!row.empty()) table.rows.push_back(row);
            row.clear();
        } else if ((n == "td" || n == "th") && inTable && tableDepth == 1) {
            if (t.closing) {
                if (inCell) {
                    inCell = false;
                    row.push_back(takeRuns());
                }
            } else {
                if (inCell) row.push_back(takeRuns());
                runs.clear();
                inCell = true;
                if (n == "th" && table.rows.empty()) table.header = true;
            }
        } else if (n == "img" && !t.closing) {
            auto src = t.attrs.find("src");
            if (src == t.attrs.end()) return;
            std::string bytes = loadImageSource(src->second, baseDir);
            auto alt = t.attrs.find("alt");
            if (bytes.empty()) {
                if (alt != t.attrs.end()) text(alt->second);
                return;
            }
            if (inCell || (inList && inItem)) {
                if (alt != t.attrs.end()) text(alt->second);
                return;
            }
            flushBlock();
            Block b;
            try {
                setImage(b, bytes);
            } catch (const std::exception&) {
                return;
            }
            if (alt != t.attrs.end()) b.alt = alt->second;
            auto w = t.attrs.find("width");
            if (w != t.attrs.end()) b.width = std::atof(w->second.c_str()) * 0.75;
            doc.blocks.push_back(std::move(b));
        }
    }
};

}  // namespace

TextDocument readHtml(const std::string& html, const std::string& baseDir) {
    HtmlReader r;
    r.baseDir = baseDir;
    std::size_t i = 0;
    std::string pending;
    bool inHead = false;
    auto flushText = [&] {
        if (!pending.empty() && !inHead) r.text(pending);
        pending.clear();
    };
    while (i < html.size()) {
        if (html[i] != '<') {
            pending += html[i++];
            continue;
        }
        if (html.compare(i, 4, "<!--") == 0) {
            auto e = html.find("-->", i);
            i = e == std::string::npos ? html.size() : e + 3;
            continue;
        }
        if (html.compare(i, 2, "<!") == 0 || html.compare(i, 2, "<?") == 0) {
            auto e = html.find('>', i);
            i = e == std::string::npos ? html.size() : e + 1;
            continue;
        }
        // parse the tag
        std::size_t j = i + 1;
        HtmlTag t;
        if (j < html.size() && html[j] == '/') {
            t.closing = true;
            ++j;
        }
        std::size_t ns = j;
        while (j < html.size() && (std::isalnum(static_cast<unsigned char>(html[j])) || html[j] == '-' || html[j] == ':')) ++j;
        if (j == ns) {
            pending += html[i++];
            continue;
        }
        t.name = lower(html.substr(ns, j - ns));
        // attributes
        while (j < html.size() && html[j] != '>') {
            if (html[j] == '/' && j + 1 < html.size() && html[j + 1] == '>') {
                t.selfClosing = true;
                ++j;
                continue;
            }
            if (std::isspace(static_cast<unsigned char>(html[j]))) {
                ++j;
                continue;
            }
            std::size_t ks = j;
            while (j < html.size() && !std::isspace(static_cast<unsigned char>(html[j])) && html[j] != '=' && html[j] != '>' && html[j] != '/') ++j;
            std::string key = lower(html.substr(ks, j - ks));
            std::string value;
            while (j < html.size() && std::isspace(static_cast<unsigned char>(html[j]))) ++j;
            if (j < html.size() && html[j] == '=') {
                ++j;
                while (j < html.size() && std::isspace(static_cast<unsigned char>(html[j]))) ++j;
                if (j < html.size() && (html[j] == '"' || html[j] == '\'')) {
                    char q = html[j++];
                    std::size_t vs = j;
                    while (j < html.size() && html[j] != q) ++j;
                    value = html.substr(vs, j - vs);
                    if (j < html.size()) ++j;
                } else {
                    std::size_t vs = j;
                    while (j < html.size() && !std::isspace(static_cast<unsigned char>(html[j])) && html[j] != '>') ++j;
                    value = html.substr(vs, j - vs);
                }
            }
            if (j == ks) {
                ++j;
                continue;
            }
            if (!key.empty()) t.attrs[key] = decodeHtmlEntities(value);
        }
        i = j < html.size() ? j + 1 : html.size();
        flushText();
        if (t.name == "script" || t.name == "style" || t.name == "noscript" || t.name == "svg" || t.name == "template") {
            if (!t.closing && !t.selfClosing) {
                std::string close = "</" + t.name;
                std::size_t e = lower(html.substr(i)).find(close);
                i = e == std::string::npos ? html.size() : i + e;
                auto gt = html.find('>', i);
                i = gt == std::string::npos ? html.size() : gt + 1;
            }
            continue;
        }
        if (t.name == "head") {
            inHead = !t.closing;
            continue;
        }
        if (t.name == "title" && !t.closing) {
            std::size_t e = lower(html.substr(i)).find("</title");
            if (e != std::string::npos) r.doc.title = trimCopy(decodeHtmlEntities(html.substr(i, e)));
            continue;
        }
        if (inHead) {
            if (t.name == "meta" && t.attrs.count("name") && lower(t.attrs["name"]) == "author") r.doc.author = t.attrs["content"];
            continue;
        }
        r.tag(t);
    }
    flushText();
    r.flushBlock();
    if (r.inList && !r.list.items.empty()) r.doc.blocks.push_back(std::move(r.list));
    return r.doc;
}

}  // namespace wetwaresuite::office
