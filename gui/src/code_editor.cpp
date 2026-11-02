// The code editor widget: line numbers, syntax colours from code.highlight,
// selection, clipboard, auto-indent, comment toggling, error marks, several carets
// at once, folding by indentation and word wrap. It only keeps the text while you
// type; the code panel sends it to the document with code.set_text when you pause,
// so undo and scripts see every change.
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>

#include "imgui_internal.h"
#include "tool_panels.hpp"

namespace wetwaresuite::gui {

namespace {

bool continuation(char c) { return (static_cast<unsigned char>(c) & 0xC0) == 0x80; }

int nextCol(const std::string& s, int col) {
    const int n = static_cast<int>(s.size());
    if (col >= n) return n;
    ++col;
    while (col < n && continuation(s[static_cast<std::size_t>(col)])) ++col;
    return col;
}

int prevCol(const std::string& s, int col) {
    if (col <= 0) return 0;
    --col;
    while (col > 0 && continuation(s[static_cast<std::size_t>(col)])) --col;
    return col;
}

bool wordChar(char c) {
    const auto u = static_cast<unsigned char>(c);
    return std::isalnum(u) || c == '_' || u >= 0x80;
}

int wordLeft(const std::string& s, int col) {
    while (col > 0 && !wordChar(s[static_cast<std::size_t>(col - 1)])) col = prevCol(s, col);
    while (col > 0 && wordChar(s[static_cast<std::size_t>(col - 1)])) col = prevCol(s, col);
    return col;
}

int wordRight(const std::string& s, int col) {
    const int n = static_cast<int>(s.size());
    while (col < n && !wordChar(s[static_cast<std::size_t>(col)])) col = nextCol(s, col);
    while (col < n && wordChar(s[static_cast<std::size_t>(col)])) col = nextCol(s, col);
    return col;
}

// The line as drawn (tabs to the next multiple of 4) and where each byte lands in it.
struct Shown {
    std::string text;
    std::vector<int> at;  // byte offset in `text` for each byte of the line, plus one past the end
};

Shown shown(const std::string& line) {
    Shown d;
    d.at.resize(line.size() + 1);
    int column = 0;
    for (std::size_t i = 0; i < line.size(); ++i) {
        d.at[i] = static_cast<int>(d.text.size());
        if (line[i] == '\t') {
            const int spaces = 4 - column % 4;
            d.text.append(static_cast<std::size_t>(spaces), ' ');
            column += spaces;
        } else {
            d.text += line[i];
            if (!continuation(line[i])) ++column;
        }
    }
    d.at[line.size()] = static_cast<int>(d.text.size());
    return d;
}

float xAt(const Shown& d, int col) {
    col = std::clamp(col, 0, static_cast<int>(d.at.size()) - 1);
    const int end = d.at[static_cast<std::size_t>(col)];
    return end <= 0 ? 0.0f : ImGui::CalcTextSize(d.text.data(), d.text.data() + end).x;
}

int colAt(const std::string& line, float x) {
    const Shown d = shown(line);
    int col = 0;
    float prev = 0;
    while (col < static_cast<int>(line.size())) {
        const int next = nextCol(line, col);
        const float nx = xAt(d, next);
        if (x < (prev + nx) * 0.5f) return col;
        prev = nx;
        col = next;
    }
    return static_cast<int>(line.size());
}

int columns(const std::string& line) { return static_cast<int>(shown(line).text.size()); }

TextPos clampPos(const CodeEditor& ed, TextPos p) {
    p.line = std::clamp(p.line, 0, static_cast<int>(ed.lines.size()) - 1);
    p.col = std::clamp(p.col, 0, static_cast<int>(ed.lines[static_cast<std::size_t>(p.line)].size()));
    return p;
}

std::pair<TextPos, TextPos> ordered(const CodeEditor& ed) {
    return ed.anchor < ed.cursor ? std::make_pair(ed.anchor, ed.cursor) : std::make_pair(ed.cursor, ed.anchor);
}

bool hasSelection(const CodeEditor& ed) { return ed.anchor != ed.cursor; }

// ---- several carets ----

long offsetOf(const CodeEditor& ed, TextPos p) {
    long o = 0;
    for (int i = 0; i < p.line && i < static_cast<int>(ed.lines.size()); ++i) o += static_cast<long>(ed.lines[static_cast<std::size_t>(i)].size()) + 1;
    return o + p.col;
}

TextPos posOf(const CodeEditor& ed, long off) {
    TextPos p;
    for (std::size_t i = 0; i < ed.lines.size(); ++i) {
        const long len = static_cast<long>(ed.lines[i].size());
        if (off <= len || i + 1 == ed.lines.size()) {
            p.line = static_cast<int>(i);
            p.col = static_cast<int>(std::clamp(off, 0L, len));
            return p;
        }
        off -= len + 1;
    }
    return p;
}

long textSize(const CodeEditor& ed) {
    long n = 0;
    for (const auto& l : ed.lines) n += static_cast<long>(l.size()) + 1;
    return n;
}

// Drops carets that landed on (or inside the selection of) another one.
void mergeCarets(CodeEditor& ed) {
    if (ed.extra.empty()) return;
    std::vector<CodeEditor::Caret> keep;
    auto overlaps = [](const CodeEditor::Caret& a, const CodeEditor::Caret& b) {
        const TextPos a0 = a.cursor < a.anchor ? a.cursor : a.anchor;
        const TextPos a1 = a.cursor < a.anchor ? a.anchor : a.cursor;
        const TextPos b0 = b.cursor < b.anchor ? b.cursor : b.anchor;
        const TextPos b1 = b.cursor < b.anchor ? b.anchor : b.cursor;
        if (a0 == b0 || a1 == b1) return true;
        return a0 < b1 && b0 < a1;
    };
    const CodeEditor::Caret primary{ed.cursor, ed.anchor};
    for (const auto& c : ed.extra) {
        bool clash = overlaps(c, primary);
        for (const auto& k : keep) clash = clash || overlaps(c, k);
        if (!clash) keep.push_back(c);
    }
    std::sort(keep.begin(), keep.end(), [](const CodeEditor::Caret& a, const CodeEditor::Caret& b) { return a.cursor < b.cursor; });
    ed.extra = std::move(keep);
}

// Runs `op` once per caret with that caret as ed.cursor/anchor, last in the text first, so
// a change at one caret only moves the carets after it (which are shifted by its size change).
// op gets the caret's place in text order (0 = first).
template <class Op>
void eachCaret(CodeEditor& ed, Op op) {
    if (ed.extra.empty()) {
        op(0);
        return;
    }
    struct C {
        long cur, anc;
        bool primary;
    };
    std::vector<C> cs{{offsetOf(ed, ed.cursor), offsetOf(ed, ed.anchor), true}};
    for (const auto& e : ed.extra) cs.push_back({offsetOf(ed, e.cursor), offsetOf(ed, e.anchor), false});
    std::sort(cs.begin(), cs.end(), [](const C& a, const C& b) { return std::max(a.cur, a.anc) > std::max(b.cur, b.anc); });
    for (std::size_t k = 0; k < cs.size(); ++k) {
        ed.cursor = posOf(ed, cs[k].cur);
        ed.anchor = posOf(ed, cs[k].anc);
        const long before = textSize(ed);
        op(static_cast<int>(cs.size() - 1 - k));
        const long delta = textSize(ed) - before;
        cs[k].cur = offsetOf(ed, ed.cursor);
        cs[k].anc = offsetOf(ed, ed.anchor);
        for (std::size_t j = 0; j < k; ++j) {
            cs[j].cur += delta;
            cs[j].anc += delta;
        }
    }
    ed.extra.clear();
    for (const auto& c : cs) {
        const TextPos cu = posOf(ed, c.cur), an = posOf(ed, c.anc);
        if (c.primary) {
            ed.cursor = cu;
            ed.anchor = an;
        } else {
            ed.extra.push_back({cu, an});
        }
    }
    mergeCarets(ed);
}

// Every caret's selection in text order.
std::vector<std::pair<TextPos, TextPos>> allSelections(const CodeEditor& ed) {
    std::vector<std::pair<TextPos, TextPos>> out{ordered(ed)};
    for (const auto& e : ed.extra) out.push_back(e.cursor < e.anchor ? std::make_pair(e.cursor, e.anchor) : std::make_pair(e.anchor, e.cursor));
    std::sort(out.begin(), out.end());
    return out;
}

// ---- folding (by indentation, so it works for every language) ----

int indentColumns(const std::string& line) {
    int n = 0;
    for (char c : line) {
        if (c == ' ') ++n;
        else if (c == '\t') n += 4 - n % 4;
        else break;
    }
    return n;
}

bool blank(const std::string& line) { return line.find_first_not_of(" \t") == std::string::npos; }

bool foldable(const CodeEditor& ed, int i) {
    if (i < 0 || i >= static_cast<int>(ed.lines.size()) || blank(ed.lines[static_cast<std::size_t>(i)])) return false;
    const int base = indentColumns(ed.lines[static_cast<std::size_t>(i)]);
    for (std::size_t j = static_cast<std::size_t>(i) + 1; j < ed.lines.size(); ++j) {
        if (!blank(ed.lines[j])) return indentColumns(ed.lines[j]) > base;
    }
    return false;
}

// The last line of the block that starts at line i (i itself when nothing folds there).
int foldEnd(const CodeEditor& ed, int i) {
    if (!foldable(ed, i)) return i;
    const int base = indentColumns(ed.lines[static_cast<std::size_t>(i)]);
    int end = i;
    for (std::size_t j = static_cast<std::size_t>(i) + 1; j < ed.lines.size(); ++j) {
        if (blank(ed.lines[j])) continue;
        if (indentColumns(ed.lines[j]) <= base) break;
        end = static_cast<int>(j);
    }
    return end;
}

bool isFolded(const CodeEditor& ed, int i) { return std::binary_search(ed.folded.begin(), ed.folded.end(), i); }

void setFold(CodeEditor& ed, int i, bool fold) {
    auto it = std::lower_bound(ed.folded.begin(), ed.folded.end(), i);
    const bool has = it != ed.folded.end() && *it == i;
    if (fold && !has && foldable(ed, i)) ed.folded.insert(it, i);
    if (!fold && has) ed.folded.erase(it);
    ed.rowsVersion = ~0ull;
}

// Opens any folded block hiding a caret.
void unfoldCarets(CodeEditor& ed) {
    if (ed.folded.empty()) return;
    std::vector<int> lines{ed.cursor.line};
    for (const auto& e : ed.extra) lines.push_back(e.cursor.line);
    bool changed = false;
    for (std::size_t k = ed.folded.size(); k-- > 0;) {
        const int f = ed.folded[k], end = foldEnd(ed, f);
        for (int l : lines) {
            if (l > f && l <= end) {
                ed.folded.erase(ed.folded.begin() + static_cast<long>(k));
                changed = true;
                break;
            }
        }
    }
    if (changed) ed.rowsVersion = ~0ull;
}

// Lines added or removed: folds below the change move with their lines.
void followLineCount(CodeEditor& ed) {
    const auto n = ed.lines.size();
    if (ed.lastLineCount != 0 && n != ed.lastLineCount && !ed.folded.empty()) {
        const int delta = static_cast<int>(n) - static_cast<int>(ed.lastLineCount);
        const int at = ed.cursor.line - std::max(delta, 0);
        std::vector<int> moved;
        for (int f : ed.folded) {
            if (f <= at) moved.push_back(f);
            else if (delta > 0 || f > at - delta) moved.push_back(f + delta);
        }
        ed.folded = std::move(moved);
        std::sort(ed.folded.begin(), ed.folded.end());
        ed.folded.erase(std::unique(ed.folded.begin(), ed.folded.end()), ed.folded.end());
        ed.rowsVersion = ~0ull;
    }
    ed.lastLineCount = n;
}

void touched(CodeEditor& ed) {
    ++ed.version;
    ed.dirty = true;
    ed.spansStale = true;
    ed.lastEdit = tp::seconds();
    ed.found.clear();
}

void eraseRange(CodeEditor& ed, TextPos a, TextPos b) {
    auto& L = ed.lines;
    if (a.line == b.line) {
        L[static_cast<std::size_t>(a.line)].erase(static_cast<std::size_t>(a.col), static_cast<std::size_t>(b.col - a.col));
    } else {
        L[static_cast<std::size_t>(a.line)] = L[static_cast<std::size_t>(a.line)].substr(0, static_cast<std::size_t>(a.col)) +
                                              L[static_cast<std::size_t>(b.line)].substr(static_cast<std::size_t>(b.col));
        L.erase(L.begin() + a.line + 1, L.begin() + b.line + 1);
    }
    ed.cursor = ed.anchor = a;
    touched(ed);
}

void deleteSelection(CodeEditor& ed) {
    if (!hasSelection(ed)) return;
    auto [a, b] = ordered(ed);
    eraseRange(ed, a, b);
}

void insertText(CodeEditor& ed, const std::string& raw) {
    deleteSelection(ed);
    std::string t;
    t.reserve(raw.size());
    for (std::size_t i = 0; i < raw.size(); ++i) {
        if (raw[i] == '\r') {
            if (i + 1 < raw.size() && raw[i + 1] == '\n') continue;
            t += '\n';
        } else {
            t += raw[i];
        }
    }
    auto& L = ed.lines;
    std::string& line = L[static_cast<std::size_t>(ed.cursor.line)];
    const std::string tail = line.substr(static_cast<std::size_t>(ed.cursor.col));
    line.erase(static_cast<std::size_t>(ed.cursor.col));
    std::size_t start = 0;
    int row = ed.cursor.line;
    while (true) {
        const std::size_t nl = t.find('\n', start);
        const std::string piece = t.substr(start, nl == std::string::npos ? std::string::npos : nl - start);
        L[static_cast<std::size_t>(row)] += piece;
        ed.widest = std::max(ed.widest, columns(L[static_cast<std::size_t>(row)]) + static_cast<int>(tail.size()));
        if (nl == std::string::npos) break;
        L.insert(L.begin() + row + 1, std::string());
        ++row;
        start = nl + 1;
    }
    ed.cursor = {row, static_cast<int>(L[static_cast<std::size_t>(row)].size())};
    L[static_cast<std::size_t>(row)] += tail;
    ed.anchor = ed.cursor;
    touched(ed);
}

std::string indentOf(const std::string& line) {
    std::size_t n = 0;
    while (n < line.size() && (line[n] == ' ' || line[n] == '\t')) ++n;
    return line.substr(0, n);
}

std::string indentUnit(const CodeEditor& ed) { return ed.tabs ? "\t" : "    "; }

void newline(CodeEditor& ed) {
    deleteSelection(ed);
    const std::string& line = ed.lines[static_cast<std::size_t>(ed.cursor.line)];
    std::string indent = indentOf(line);
    if (indent.size() > static_cast<std::size_t>(ed.cursor.col)) indent.resize(static_cast<std::size_t>(ed.cursor.col));
    // one more level after an opening bracket or a Python-style colon
    std::string before = line.substr(0, static_cast<std::size_t>(ed.cursor.col));
    while (!before.empty() && (before.back() == ' ' || before.back() == '\t')) before.pop_back();
    const char last = before.empty() ? '\0' : before.back();
    const bool opens = last == '{' || last == '[' || last == '(' || (last == ':' && ed.language == "python");
    insertText(ed, "\n" + indent + (opens ? indentUnit(ed) : ""));
    const char next = static_cast<std::size_t>(ed.cursor.col) < ed.lines[static_cast<std::size_t>(ed.cursor.line)].size()
                          ? ed.lines[static_cast<std::size_t>(ed.cursor.line)][static_cast<std::size_t>(ed.cursor.col)]
                          : '\0';
    if (opens && ((last == '{' && next == '}') || (last == '[' && next == ']') || (last == '(' && next == ')'))) {
        const TextPos keep = ed.cursor;
        insertText(ed, "\n" + indent);
        ed.cursor = ed.anchor = keep;
    }
}

void indentLines(CodeEditor& ed, bool out) {
    auto [a, b] = ordered(ed);
    int last = b.line;
    if (b.col == 0 && b.line > a.line) --last;
    const std::string unit = indentUnit(ed);
    for (int i = a.line; i <= last; ++i) {
        std::string& line = ed.lines[static_cast<std::size_t>(i)];
        int delta = 0;
        if (!out) {
            if (line.empty()) continue;
            line.insert(0, unit);
            delta = static_cast<int>(unit.size());
        } else if (!line.empty() && line[0] == '\t') {
            line.erase(0, 1);
            delta = -1;
        } else {
            int n = 0;
            while (n < 4 && n < static_cast<int>(line.size()) && line[static_cast<std::size_t>(n)] == ' ') ++n;
            line.erase(0, static_cast<std::size_t>(n));
            delta = -n;
        }
        if (ed.anchor.line == i) ed.anchor.col = std::max(0, ed.anchor.col + delta);
        if (ed.cursor.line == i) ed.cursor.col = std::max(0, ed.cursor.col + delta);
    }
    touched(ed);
}

void toggleComment(CodeEditor& ed) {
    if (ed.comment.empty()) return;
    auto [a, b] = ordered(ed);
    int last = b.line;
    if (b.col == 0 && b.line > a.line) --last;
    const std::string mark = ed.comment + " ";
    bool all = true;
    for (int i = a.line; i <= last; ++i) {
        const std::string& line = ed.lines[static_cast<std::size_t>(i)];
        const std::string ind = indentOf(line);
        if (ind.size() == line.size()) continue;
        if (line.compare(ind.size(), ed.comment.size(), ed.comment) != 0) all = false;
    }
    for (int i = a.line; i <= last; ++i) {
        std::string& line = ed.lines[static_cast<std::size_t>(i)];
        const std::string ind = indentOf(line);
        if (ind.size() == line.size()) continue;
        if (all) {
            const std::size_t n = line.compare(ind.size(), mark.size(), mark) == 0 ? mark.size() : ed.comment.size();
            line.erase(ind.size(), n);
        } else {
            line.insert(ind.size(), mark);
        }
    }
    ed.cursor = clampPos(ed, ed.cursor);
    ed.anchor = clampPos(ed, ed.anchor);
    touched(ed);
}

ImU32 tokenColour(int kind, bool dark) {
    static const ImU32 darkColours[] = {
        IM_COL32(222, 224, 228, 255), IM_COL32(204, 132, 230, 255), IM_COL32(94, 196, 208, 255), IM_COL32(230, 168, 110, 255),
        IM_COL32(230, 168, 110, 255), IM_COL32(158, 206, 124, 255), IM_COL32(140, 146, 158, 255), IM_COL32(232, 196, 120, 255),
        IM_COL32(104, 178, 250, 255), IM_COL32(236, 118, 126, 255), IM_COL32(230, 168, 110, 255), IM_COL32(104, 178, 250, 255),
        IM_COL32(236, 118, 126, 255), IM_COL32(94, 196, 208, 255),
    };
    static const ImU32 lightColours[] = {
        IM_COL32(30, 30, 34, 255),  IM_COL32(150, 30, 170, 255), IM_COL32(0, 110, 130, 255),  IM_COL32(170, 85, 0, 255),
        IM_COL32(170, 85, 0, 255),  IM_COL32(40, 120, 20, 255),  IM_COL32(100, 106, 116, 255), IM_COL32(140, 100, 0, 255),
        IM_COL32(20, 90, 200, 255), IM_COL32(190, 30, 40, 255),  IM_COL32(170, 85, 0, 255),   IM_COL32(20, 90, 200, 255),
        IM_COL32(190, 30, 40, 255), IM_COL32(0, 110, 130, 255),
    };
    if (kind < 0 || kind > 13) kind = 0;
    return dark ? darkColours[kind] : lightColours[kind];
}

struct Layout {
    ImVec2 origin;  // screen position of row 0, column 0 of the text
    float lineH = 0, gutterW = 0, charW = 0;
    ImRect inner;   // visible text area (right of the gutter)
    int visibleLines = 1;
};

// Splits lines into screen rows: wrapped at `width` (at a space when there is one), none for
// lines inside a folded block.
void buildRows(CodeEditor& ed, float width) {
    const float w = ed.wrap ? width : -1;
    if (ed.rowsVersion == ed.version && ed.rowsWidth == w && ed.lineRow.size() == ed.lines.size()) return;
    ed.rows.clear();
    ed.lineRow.assign(ed.lines.size(), -1);
    const int n = static_cast<int>(ed.lines.size());
    for (int i = 0; i < n;) {
        const std::string& line = ed.lines[static_cast<std::size_t>(i)];
        ed.lineRow[static_cast<std::size_t>(i)] = static_cast<int>(ed.rows.size());
        const int len = static_cast<int>(line.size());
        if (w <= 0 || len == 0) {
            ed.rows.push_back({i, 0, len});
        } else {
            const Shown d = shown(line);
            int from = 0, lastSpace = -1;
            float x = 0, rowStart = 0;
            for (int col = 0; col < len;) {
                const int next = nextCol(line, col);
                const float nx = ImGui::CalcTextSize(d.text.data(), d.text.data() + d.at[static_cast<std::size_t>(next)]).x;
                if (nx - rowStart > w && col > from) {
                    const int cut = lastSpace > from ? lastSpace : col;
                    ed.rows.push_back({i, from, cut});
                    from = cut;
                    rowStart = cut == col ? x : ImGui::CalcTextSize(d.text.data(), d.text.data() + d.at[static_cast<std::size_t>(cut)]).x;
                    lastSpace = -1;
                }
                if (line[static_cast<std::size_t>(col)] == ' ' || line[static_cast<std::size_t>(col)] == '\t') lastSpace = next;
                x = nx;
                col = next;
            }
            ed.rows.push_back({i, from, len});
        }
        i = isFolded(ed, i) ? std::max(i, foldEnd(ed, i)) + 1 : i + 1;
    }
    if (ed.rows.empty()) ed.rows.push_back({0, 0, 0});
    ed.rowsVersion = ed.version;
    ed.rowsWidth = w;
}

// The row a position is drawn on (inside a folded block: the block's first line).
int rowOf(const CodeEditor& ed, TextPos p) {
    int line = std::clamp(p.line, 0, static_cast<int>(ed.lineRow.size()) - 1);
    while (line > 0 && ed.lineRow[static_cast<std::size_t>(line)] < 0) --line;
    int r = std::max(0, ed.lineRow[static_cast<std::size_t>(line)]);
    if (line != p.line) return r;
    while (r + 1 < static_cast<int>(ed.rows.size()) && ed.rows[static_cast<std::size_t>(r + 1)].line == line && ed.rows[static_cast<std::size_t>(r + 1)].from <= p.col) ++r;
    return r;
}

// x of a column on its row, from the row's left edge
float rowX(const CodeEditor& ed, int r, const Shown& d, int col) {
    const auto& row = ed.rows[static_cast<std::size_t>(r)];
    return xAt(d, col) - (row.from > 0 ? xAt(d, row.from) : 0.0f);
}

TextPos hit(const CodeEditor& ed, const Layout& lay, ImVec2 mouse) {
    const int r = std::clamp(static_cast<int>(std::floor((mouse.y - lay.origin.y) / lay.lineH)), 0, static_cast<int>(ed.rows.size()) - 1);
    const auto& row = ed.rows[static_cast<std::size_t>(r)];
    const std::string& line = ed.lines[static_cast<std::size_t>(row.line)];
    const float base = row.from > 0 ? xAt(shown(line), row.from) : 0.0f;
    TextPos p{row.line, colAt(line, mouse.x - lay.origin.x - lay.gutterW + base)};
    p.col = std::clamp(p.col, row.from, row.to);
    return p;
}

void selectWord(CodeEditor& ed) {
    const std::string& line = ed.lines[static_cast<std::size_t>(ed.cursor.line)];
    int a = ed.cursor.col, b = ed.cursor.col;
    while (a > 0 && wordChar(line[static_cast<std::size_t>(a - 1)])) a = prevCol(line, a);
    while (b < static_cast<int>(line.size()) && wordChar(line[static_cast<std::size_t>(b)])) b = nextCol(line, b);
    ed.anchor = {ed.cursor.line, a};
    ed.cursor = {ed.cursor.line, b};
}

// Mouse: click, shift+click, drag and double-click. True when the cursor moved.
bool handleMouse(CodeEditor& ed, const Layout& lay, ImGuiID id, bool hovered) {
    ImGuiContext& g = *ImGui::GetCurrentContext();
    ImGuiWindow* window = ImGui::GetCurrentWindow();
    const ImGuiIO& io = ImGui::GetIO();
    if (hovered) ImGui::SetMouseCursor(ImGuiMouseCursor_TextInput);
    if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        ImGui::SetActiveID(id, window);
        ImGui::SetFocusID(id, window);
        ImGui::FocusWindow(window);
        const TextPos at = hit(ed, lay, io.MousePos);
        if (io.KeyAlt && !io.KeyShift) {
            // another caret here (or take the one that is here away)
            const auto before = ed.extra.size();
            ed.extra.erase(std::remove_if(ed.extra.begin(), ed.extra.end(), [&](const CodeEditor::Caret& c) { return c.cursor == at; }), ed.extra.end());
            if (ed.extra.size() == before && at != ed.cursor) ed.extra.push_back({at, at});
            mergeCarets(ed);
            return true;
        }
        ed.extra.clear();
        ed.cursor = at;
        if (!io.KeyShift) ed.anchor = ed.cursor;
        if ((io.ConfigMacOSXBehaviors ? io.KeySuper : io.KeyCtrl) && !io.KeyShift) ed.wantDefinition = true;
        if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) selectWord(ed);
        else ed.dragging = true;
        ed.wantX = -1;
        return true;
    }
    if (g.ActiveId == id && ImGui::IsMouseClicked(ImGuiMouseButton_Left) && !hovered) {
        ImGui::ClearActiveID();
        ed.dragging = false;
    }
    if (ed.dragging) {
        if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
            ed.dragging = false;
            return false;
        }
        const TextPos p = hit(ed, lay, io.MousePos);
        if (io.MousePos.y < lay.inner.Min.y) ImGui::SetScrollY(ImGui::GetScrollY() - lay.lineH);
        if (io.MousePos.y > lay.inner.Max.y) ImGui::SetScrollY(ImGui::GetScrollY() + lay.lineH);
        if (p != ed.cursor) {
            ed.cursor = p;
            return true;
        }
    }
    return false;
}

void moveTo(CodeEditor& ed, TextPos p, bool select) {
    ed.cursor = clampPos(ed, p);
    if (!select) ed.anchor = ed.cursor;
}

// Cursor keys, at every caret. True when a caret moved.
bool handleMoves(CodeEditor& ed, const Layout& lay) {
    const ImGuiIO& io = ImGui::GetIO();
    const bool mac = io.ConfigMacOSXBehaviors;
    const bool word = mac ? io.KeyAlt : io.KeyCtrl;
    const bool jump = mac ? io.KeySuper : io.KeyCtrl;  // to the start or end of the file
    const bool shift = io.KeyShift;
    auto pressed = [](ImGuiKey k) { return ImGui::IsKeyPressed(k, true); };
    enum { None, Left, Right, Vertical, Home, End } key = None;
    if (pressed(ImGuiKey_LeftArrow)) key = Left;
    else if (pressed(ImGuiKey_RightArrow)) key = Right;
    else if (pressed(ImGuiKey_UpArrow) || pressed(ImGuiKey_DownArrow) || pressed(ImGuiKey_PageUp) || pressed(ImGuiKey_PageDown)) key = Vertical;
    else if (pressed(ImGuiKey_Home)) key = Home;
    else if (pressed(ImGuiKey_End)) key = End;
    if (key == None) return false;
    // Ctrl+Alt+Up/Down add a caret above or below instead (handled with the edits)
    if (key == Vertical && (mac ? io.KeySuper : io.KeyCtrl) && io.KeyAlt) return false;
    // an open completion list takes Up and Down
    if (key == Vertical && ed.menuOpen && (ImGui::IsKeyDown(ImGuiKey_UpArrow) || ImGui::IsKeyDown(ImGuiKey_DownArrow))) return false;
    int step = 1;
    if (ImGui::IsKeyDown(ImGuiKey_PageUp) || ImGui::IsKeyDown(ImGuiKey_PageDown)) step = std::max(1, lay.visibleLines - 2);
    if (ImGui::IsKeyDown(ImGuiKey_UpArrow) || ImGui::IsKeyDown(ImGuiKey_PageUp)) step = -step;
    const bool many = !ed.extra.empty();
    eachCaret(ed, [&](int) {
        const std::string& line = ed.lines[static_cast<std::size_t>(ed.cursor.line)];
        TextPos p = ed.cursor;
        bool keepX = false;
        if (key == Left) {
            if (hasSelection(ed) && !shift) p = ordered(ed).first;
            else if (p.col > 0) p.col = word ? wordLeft(line, p.col) : prevCol(line, p.col);
            else if (p.line > 0) p = {p.line - 1, static_cast<int>(ed.lines[static_cast<std::size_t>(p.line - 1)].size())};
        } else if (key == Right) {
            if (hasSelection(ed) && !shift) p = ordered(ed).second;
            else if (p.col < static_cast<int>(line.size())) p.col = word ? wordRight(line, p.col) : nextCol(line, p.col);
            else if (p.line + 1 < static_cast<int>(ed.lines.size())) p = {p.line + 1, 0};
        } else if (key == Vertical) {
            // by screen rows: wrapped rows count, folded blocks are stepped over
            const int r = rowOf(ed, p);
            if (many || ed.wantX < 0) ed.wantX = rowX(ed, r, shown(line), p.col);
            const int t = std::clamp(r + step, 0, static_cast<int>(ed.rows.size()) - 1);
            const auto& row = ed.rows[static_cast<std::size_t>(t)];
            const std::string& tl = ed.lines[static_cast<std::size_t>(row.line)];
            const float base = row.from > 0 ? xAt(shown(tl), row.from) : 0.0f;
            p = {row.line, std::clamp(colAt(tl, ed.wantX + base), row.from, row.to)};
            keepX = true;
        } else if (key == Home) {
            if (jump) {
                p = {0, 0};
            } else {
                const int first = static_cast<int>(indentOf(line).size());
                p.col = p.col == first ? 0 : first;  // smart home: the first word, then the very start
            }
        } else if (key == End) {
            if (jump) p = {static_cast<int>(ed.lines.size()) - 1, static_cast<int>(ed.lines.back().size())};
            else p.col = static_cast<int>(line.size());
        }
        if (!keepX) ed.wantX = -1;
        moveTo(ed, p, shift);
    });
    unfoldCarets(ed);
    return true;
}

std::string textBetween(const CodeEditor& ed, TextPos a, TextPos b) {
    if (a == b) return {};
    if (a.line == b.line) return ed.lines[static_cast<std::size_t>(a.line)].substr(static_cast<std::size_t>(a.col), static_cast<std::size_t>(b.col - a.col));
    std::string out = ed.lines[static_cast<std::size_t>(a.line)].substr(static_cast<std::size_t>(a.col));
    for (int i = a.line + 1; i < b.line; ++i) out += "\n" + ed.lines[static_cast<std::size_t>(i)];
    out += "\n" + ed.lines[static_cast<std::size_t>(b.line)].substr(0, static_cast<std::size_t>(b.col));
    return out;
}

void copySelection(const CodeEditor& ed) {
    if (!ed.extra.empty()) {
        // one line per caret, so pasting with the same number of carets puts each piece back
        std::string out;
        for (const auto& [a, b] : allSelections(ed)) out += (out.empty() ? "" : "\n") + (a == b ? ed.lines[static_cast<std::size_t>(a.line)] : textBetween(ed, a, b));
        ImGui::SetClipboardText(out.c_str());
        return;
    }
    if (hasSelection(ed)) ImGui::SetClipboardText(editorSelection(ed).c_str());
    else ImGui::SetClipboardText((ed.lines[static_cast<std::size_t>(ed.cursor.line)] + "\n").c_str());
}

void addCaretVertically(CodeEditor& ed, int dir) {
    // from the caret furthest that way
    TextPos from = ed.cursor;
    for (const auto& e : ed.extra) {
        if (dir < 0 ? e.cursor < from : from < e.cursor) from = e.cursor;
    }
    const int line = from.line + dir;
    if (line < 0 || line >= static_cast<int>(ed.lines.size())) return;
    const float x = xAt(shown(ed.lines[static_cast<std::size_t>(from.line)]), from.col);
    const TextPos at{line, colAt(ed.lines[static_cast<std::size_t>(line)], x)};
    ed.extra.push_back({at, at});
    mergeCarets(ed);
    ed.scrollTo = -1;
}

void selectAllMatches(CodeEditor& ed) {
    if (!hasSelection(ed)) selectWord(ed);
    auto [a, b] = ordered(ed);
    if (a.line != b.line || a == b) return;
    const std::string needle = textBetween(ed, a, b);
    ed.extra.clear();
    const TextPos keepA = ed.anchor, keepC = ed.cursor;
    for (int i = 0; i < static_cast<int>(ed.lines.size()); ++i) {
        const std::string& l = ed.lines[static_cast<std::size_t>(i)];
        for (std::size_t at = l.find(needle); at != std::string::npos; at = l.find(needle, at + needle.size())) {
            const TextPos s{i, static_cast<int>(at)}, e{i, static_cast<int>(at + needle.size())};
            if (s == a) continue;
            ed.extra.push_back({e, s});
        }
    }
    ed.anchor = keepA;
    ed.cursor = keepC;
    mergeCarets(ed);
}

std::string typedText(ImGuiIO& io, bool cmd) {
    std::string typed;
    if (!(cmd && !io.KeyAlt)) {
        for (ImWchar c : io.InputQueueCharacters) {
            if (c < 32 || c == 127) continue;
            const unsigned int u = c;
            if (u < 0x80) {
                typed += static_cast<char>(u);
            } else if (u < 0x800) {
                typed += static_cast<char>(0xC0 | (u >> 6));
                typed += static_cast<char>(0x80 | (u & 0x3F));
            } else {
                typed += static_cast<char>(0xE0 | (u >> 12));
                typed += static_cast<char>(0x80 | ((u >> 6) & 0x3F));
                typed += static_cast<char>(0x80 | (u & 0x3F));
            }
        }
    }
    io.InputQueueCharacters.resize(0);
    return typed;
}

void typeAtCaret(CodeEditor& ed, const std::string& typed) {
    // a closing bracket typed over the one already there just steps past it
    const std::string& line = ed.lines[static_cast<std::size_t>(ed.cursor.line)];
    if (!hasSelection(ed) && typed.size() == 1 && (typed[0] == ')' || typed[0] == ']' || typed[0] == '}') &&
        static_cast<std::size_t>(ed.cursor.col) < line.size() && line[static_cast<std::size_t>(ed.cursor.col)] == typed[0]) {
        moveTo(ed, {ed.cursor.line, ed.cursor.col + 1}, false);
        return;
    }
    insertText(ed, typed);
    // a closing bracket on an otherwise empty line goes back one level
    std::string& now = ed.lines[static_cast<std::size_t>(ed.cursor.line)];
    if (typed == "}" && indentOf(now).size() + 1 == now.size() && now.size() > 1) {
        const std::string unit = indentUnit(ed);
        if (now.compare(0, unit.size(), unit) == 0) {
            now.erase(0, unit.size());
            ed.cursor.col -= static_cast<int>(unit.size());
            ed.anchor = ed.cursor;
        }
    }
}

void backspaceAtCaret(CodeEditor& ed, bool word) {
    if (hasSelection(ed)) {
        deleteSelection(ed);
    } else if (ed.cursor.col > 0) {
        const std::string& line = ed.lines[static_cast<std::size_t>(ed.cursor.line)];
        int from = word ? wordLeft(line, ed.cursor.col) : prevCol(line, ed.cursor.col);
        // inside the indent, take back a whole level of spaces
        if (!word && !ed.tabs && indentOf(line).size() >= static_cast<std::size_t>(ed.cursor.col) && ed.cursor.col >= 4 && ed.cursor.col % 4 == 0 &&
            line.compare(static_cast<std::size_t>(ed.cursor.col - 4), 4, "    ") == 0) {
            from = ed.cursor.col - 4;
        }
        eraseRange(ed, {ed.cursor.line, from}, ed.cursor);
    } else if (ed.cursor.line > 0) {
        const int prev = ed.cursor.line - 1;
        eraseRange(ed, {prev, static_cast<int>(ed.lines[static_cast<std::size_t>(prev)].size())}, ed.cursor);
    }
}

void deleteAtCaret(CodeEditor& ed, bool word) {
    if (hasSelection(ed)) {
        deleteSelection(ed);
        return;
    }
    const std::string& line = ed.lines[static_cast<std::size_t>(ed.cursor.line)];
    if (ed.cursor.col < static_cast<int>(line.size())) {
        eraseRange(ed, ed.cursor, {ed.cursor.line, word ? wordRight(line, ed.cursor.col) : nextCol(line, ed.cursor.col)});
    } else if (ed.cursor.line + 1 < static_cast<int>(ed.lines.size())) {
        eraseRange(ed, ed.cursor, {ed.cursor.line + 1, 0});
    }
}

// Shortcuts and editing keys, at every caret. True when the text changed.
bool handleEdits(CodeEditor& ed) {
    ImGuiIO& io = ImGui::GetIO();
    const bool mac = io.ConfigMacOSXBehaviors;
    const bool cmd = mac ? io.KeySuper : io.KeyCtrl;
    const bool word = mac ? io.KeyAlt : io.KeyCtrl;
    auto pressed = [](ImGuiKey k) { return ImGui::IsKeyPressed(k, true); };
    const std::uint64_t before = ed.version;
    if (cmd && pressed(ImGuiKey_A)) {
        ed.extra.clear();
        ed.anchor = {0, 0};
        ed.cursor = {static_cast<int>(ed.lines.size()) - 1, static_cast<int>(ed.lines.back().size())};
        return false;
    }
    if (cmd && pressed(ImGuiKey_C)) {
        copySelection(ed);
        return false;
    }
    if (cmd && (pressed(ImGuiKey_Z) && !io.KeyShift)) {
        ed.wantUndo = true;
        return false;
    }
    if (cmd && (pressed(ImGuiKey_Y) || (io.KeyShift && pressed(ImGuiKey_Z)))) {
        ed.wantRedo = true;
        return false;
    }
    if (cmd && io.KeyAlt && (pressed(ImGuiKey_UpArrow) || pressed(ImGuiKey_DownArrow))) {
        addCaretVertically(ed, ImGui::IsKeyDown(ImGuiKey_UpArrow) ? -1 : 1);
        return false;
    }
    if (cmd && !io.KeyShift && pressed(ImGuiKey_D)) {
        editorAddNextMatch(ed);
        return false;
    }
    if (cmd && io.KeyShift && pressed(ImGuiKey_L)) {
        selectAllMatches(ed);
        return false;
    }
    if (cmd && pressed(ImGuiKey_Space)) {
        ed.wantComplete = true;
        io.InputQueueCharacters.resize(0);
        return false;
    }
    if (pressed(ImGuiKey_F12)) {
        ed.wantDefinition = true;
        return false;
    }
    if (io.KeyAlt && !cmd && pressed(ImGuiKey_Z)) {
        ed.wrap = !ed.wrap;
        io.InputQueueCharacters.resize(0);
        return false;
    }
    if (cmd && io.KeyShift && (pressed(ImGuiKey_LeftBracket) || pressed(ImGuiKey_RightBracket))) {
        const bool fold = ImGui::IsKeyDown(ImGuiKey_LeftBracket);
        // the block the caret is in (or starts on)
        int at = ed.cursor.line;
        if (fold && !foldable(ed, at)) {
            const int ind = indentColumns(ed.lines[static_cast<std::size_t>(at)]);
            while (at > 0 && (blank(ed.lines[static_cast<std::size_t>(at)]) || indentColumns(ed.lines[static_cast<std::size_t>(at)]) >= ind)) --at;
        }
        setFold(ed, at, fold);
        if (fold && foldEnd(ed, at) >= ed.cursor.line) moveTo(ed, {at, static_cast<int>(ed.lines[static_cast<std::size_t>(at)].size())}, false);
        return false;
    }
    if (ed.readOnly) {
        io.InputQueueCharacters.resize(0);
        return false;
    }
    if (cmd && pressed(ImGuiKey_X)) {
        copySelection(ed);
        eachCaret(ed, [&](int) {
            if (hasSelection(ed)) {
                deleteSelection(ed);
            } else if (ed.lines.size() > 1) {
                ed.lines.erase(ed.lines.begin() + ed.cursor.line);
                moveTo(ed, {std::min(ed.cursor.line, static_cast<int>(ed.lines.size()) - 1), 0}, false);
                touched(ed);
            } else {
                ed.lines[0].clear();
                moveTo(ed, {0, 0}, false);
                touched(ed);
            }
        });
        return true;
    }
    if (cmd && pressed(ImGuiKey_V)) {
        const char* clip = ImGui::GetClipboardText();
        if (!clip || !*clip) return false;
        const std::string text = clip;
        // as many lines as carets: one line each
        std::vector<std::string> pieces;
        if (!ed.extra.empty()) {
            std::size_t start = 0;
            while (true) {
                const std::size_t nl = text.find('\n', start);
                pieces.push_back(text.substr(start, nl == std::string::npos ? std::string::npos : nl - start));
                if (nl == std::string::npos) break;
                start = nl + 1;
            }
            if (pieces.size() != ed.extra.size() + 1) pieces.clear();
        }
        eachCaret(ed, [&](int i) { insertText(ed, pieces.empty() ? text : pieces[static_cast<std::size_t>(i)]); });
        return true;
    }
    if (cmd && io.KeyShift && pressed(ImGuiKey_D)) {  // duplicate the line
        eachCaret(ed, [&](int) {
            const std::string copy = ed.lines[static_cast<std::size_t>(ed.cursor.line)];
            ed.lines.insert(ed.lines.begin() + ed.cursor.line + 1, copy);
            moveTo(ed, {ed.cursor.line + 1, ed.cursor.col}, false);
            touched(ed);
        });
        return true;
    }
    if (cmd && pressed(ImGuiKey_Slash)) {
        eachCaret(ed, [&](int) { toggleComment(ed); });
        return true;
    }
    if (ed.menuOpen && (pressed(ImGuiKey_Tab) || pressed(ImGuiKey_Enter) || pressed(ImGuiKey_KeypadEnter))) {
        io.InputQueueCharacters.resize(0);
        return false;  // the completion list takes them
    }
    if (pressed(ImGuiKey_Tab)) {
        eachCaret(ed, [&](int) {
            if (io.KeyShift || (hasSelection(ed) && ed.anchor.line != ed.cursor.line)) indentLines(ed, io.KeyShift);
            else insertText(ed, ed.tabs ? "\t" : std::string(static_cast<std::size_t>(4 - columns(ed.lines[static_cast<std::size_t>(ed.cursor.line)].substr(0, static_cast<std::size_t>(ed.cursor.col))) % 4), ' '));
        });
        return true;
    }
    if (pressed(ImGuiKey_Enter) || pressed(ImGuiKey_KeypadEnter)) {
        eachCaret(ed, [&](int) { newline(ed); });
        return true;
    }
    if (pressed(ImGuiKey_Backspace)) {
        eachCaret(ed, [&](int) { backspaceAtCaret(ed, word); });
        return ed.version != before;
    }
    if (pressed(ImGuiKey_Delete)) {
        eachCaret(ed, [&](int) { deleteAtCaret(ed, word); });
        return ed.version != before;
    }
    const std::string typed = typedText(io, cmd);
    if (!typed.empty()) eachCaret(ed, [&](int) { typeAtCaret(ed, typed); });
    return ed.version != before;
}

void keepCursorVisible(const CodeEditor& ed, const Layout& lay) {
    const int r = rowOf(ed, ed.cursor);
    const float y = static_cast<float>(r) * lay.lineH;
    const float top = ImGui::GetScrollY(), height = lay.inner.GetHeight();
    if (y < top) ImGui::SetScrollY(y);
    else if (y + lay.lineH > top + height) ImGui::SetScrollY(y + lay.lineH - height);
    if (ed.wrap) return;
    const float x = xAt(shown(ed.lines[static_cast<std::size_t>(ed.cursor.line)]), ed.cursor.col);
    const float left = ImGui::GetScrollX(), width = lay.inner.GetWidth() - lay.lineH;
    if (x < left) ImGui::SetScrollX(std::max(0.0f, x - lay.lineH * 2));
    else if (x > left + width) ImGui::SetScrollX(x - width + lay.lineH * 2);
}

void drawRow(const CodeEditor& ed, const Layout& lay, int r, bool dark, ImDrawList* dl) {
    const auto& row = ed.rows[static_cast<std::size_t>(r)];
    const int i = row.line;
    const std::string& line = ed.lines[static_cast<std::size_t>(i)];
    const Shown d = shown(line);
    const float y = lay.origin.y + static_cast<float>(r) * lay.lineH;
    const float x0 = lay.origin.x + lay.gutterW - (row.from > 0 ? xAt(d, row.from) : 0.0f);
    const float pad = (lay.lineH - ImGui::GetFontSize()) * 0.5f;
    const bool lastRow = r + 1 >= static_cast<int>(ed.rows.size()) || ed.rows[static_cast<std::size_t>(r + 1)].line != i;
    if (ed.active && i == ed.cursor.line && !hasSelection(ed) && ed.extra.empty()) {
        dl->AddRectFilled({lay.inner.Min.x, y}, {lay.inner.Max.x, y + lay.lineH}, dark ? IM_COL32(255, 255, 255, 14) : IM_COL32(0, 0, 0, 12));
    }
    for (const auto& [mline, error] : ed.marks) {
        if (mline == i) {
            dl->AddRectFilled({lay.inner.Min.x, y}, {lay.inner.Max.x, y + lay.lineH}, error ? IM_COL32(230, 60, 60, 40) : IM_COL32(230, 180, 40, 36));
        }
    }
    for (const TextPos& f : ed.found) {
        if (f.line != i || f.col + ed.foundLen < row.from || f.col > row.to) continue;
        const float fx = x0 + xAt(d, std::max(f.col, row.from)), fx2 = x0 + xAt(d, std::min({f.col + ed.foundLen, static_cast<int>(line.size()), row.to}));
        dl->AddRectFilled({fx, y}, {fx2, y + lay.lineH}, IM_COL32(250, 200, 40, 90));
    }
    for (const auto& [a, b] : allSelections(ed)) {
        if (a == b || i < a.line || i > b.line) continue;
        const int from = std::max(row.from, i == a.line ? a.col : 0);
        const int to = std::min(row.to, i == b.line ? b.col : row.to);
        if (to < from || (i == a.line && a.col > row.to) || (i == b.line && b.col < row.from)) continue;
        const float sx = x0 + xAt(d, from);
        float ex = x0 + xAt(d, to) + (i < b.line && lastRow ? ImGui::GetFontSize() * 0.4f : 0.0f);
        if (ex <= sx) ex = sx + 2;
        dl->AddRectFilled({sx, y}, {ex, y + lay.lineH}, ImGui::GetColorU32(ImGuiCol_TextSelectedBg));
    }
    // coloured runs, only the part of the line on this row
    int done = row.from;
    const ImU32 plain = tokenColour(0, dark);
    auto run = [&](int from, int to, ImU32 colour) {
        from = std::max(from, row.from);
        to = std::min(to, row.to);
        if (to <= from) return;
        const int a = d.at[static_cast<std::size_t>(from)], b = d.at[static_cast<std::size_t>(to)];
        dl->AddText({x0 + xAt(d, from), y + pad}, colour, d.text.data() + a, d.text.data() + b);
    };
    const int n = static_cast<int>(line.size());
    if (static_cast<std::size_t>(i) < ed.spanStart.size()) {
        for (std::size_t s = static_cast<std::size_t>(ed.spanStart[static_cast<std::size_t>(i)]); s + 3 < ed.spans.size() && ed.spans[s] == i; s += 4) {
            const int col = std::clamp(ed.spans[s + 1], 0, n), len = ed.spans[s + 2];
            const int end = std::clamp(col + len, 0, n);
            if (end <= done) continue;
            if (col < done) {
                run(done, end, tokenColour(ed.spans[s + 3], dark));
            } else {
                run(done, col, plain);
                run(col, end, tokenColour(ed.spans[s + 3], dark));
            }
            done = std::max(done, end);
            if (done >= row.to) break;
        }
    }
    run(done, n, plain);
    // a folded block shows as a little box after its first line
    if (lastRow && isFolded(ed, i)) {
        const float fx = x0 + xAt(d, n) + lay.charW, w = lay.charW * 3.2f;
        dl->AddRectFilled({fx, y + 3}, {fx + w, y + lay.lineH - 3}, dark ? IM_COL32(255, 255, 255, 40) : IM_COL32(0, 0, 0, 30), 3.0f);
        dl->AddText({fx + lay.charW * 0.35f, y + pad - 1}, ImGui::GetColorU32(ImGuiCol_TextDisabled), "...");
    }
    if (!ed.active) return;
    auto caret = [&](TextPos c) {
        if (c.line != i || rowOf(ed, c) != r) return;
        const float cx = x0 + xAt(d, c.col);
        dl->AddLine({cx, y + 1}, {cx, y + lay.lineH - 1}, ImGui::GetColorU32(ImGuiCol_Text), 2.0f);
    };
    caret(ed.cursor);
    for (const auto& e : ed.extra) caret(e.cursor);
}

// Line numbers, and an arrow before each line where a block can fold.
void drawGutter(const CodeEditor& ed, const Layout& lay, int first, int last, ImDrawList* dl, int hoverRow) {
    const float left = lay.inner.Min.x - lay.gutterW;
    dl->AddRectFilled({left, lay.inner.Min.y}, {lay.inner.Min.x, lay.inner.Max.y}, ImGui::GetColorU32(ImGuiCol_FrameBg));
    const float pad = (lay.lineH - ImGui::GetFontSize()) * 0.5f;
    for (int r = first; r < last; ++r) {
        const auto& row = ed.rows[static_cast<std::size_t>(r)];
        if (row.from > 0) continue;
        const int i = row.line;
        const float y = lay.origin.y + static_cast<float>(r) * lay.lineH;
        const std::string num = std::to_string(i + 1);
        const float w = ImGui::CalcTextSize(num.c_str()).x;
        ImU32 colour = ImGui::GetColorU32(i == ed.cursor.line ? ImGuiCol_Text : ImGuiCol_TextDisabled);
        for (const auto& [mline, error] : ed.marks) {
            if (mline == i) colour = error ? IM_COL32(240, 80, 80, 255) : IM_COL32(230, 170, 30, 255);
        }
        dl->AddText({lay.inner.Min.x - w - lay.charW * 1.8f, y + pad}, colour, num.c_str());
        const bool folded = isFolded(ed, i);
        if (folded || (hoverRow >= 0 && foldable(ed, i))) {
            // an arrow: pointing right when folded, down when open (shown while the mouse is over the gutter)
            const float cx = lay.inner.Min.x - lay.charW * 0.95f, cy = y + lay.lineH * 0.5f, a = lay.charW * 0.38f;
            const ImU32 ac = ImGui::GetColorU32(r == hoverRow || folded ? ImGuiCol_Text : ImGuiCol_TextDisabled);
            if (folded) dl->AddTriangleFilled({cx - a * 0.6f, cy - a}, {cx - a * 0.6f, cy + a}, {cx + a * 0.8f, cy}, ac);
            else dl->AddTriangleFilled({cx - a, cy - a * 0.6f}, {cx + a, cy - a * 0.6f}, {cx, cy + a * 0.8f}, ac);
        }
    }
}

}  // namespace

std::string editorText(const CodeEditor& ed) {
    std::string out;
    std::size_t total = 0;
    for (const auto& l : ed.lines) total += l.size() + 1;
    out.reserve(total);
    for (std::size_t i = 0; i < ed.lines.size(); ++i) {
        if (i) out += '\n';
        out += ed.lines[i];
    }
    return out;
}

void setEditorText(CodeEditor& ed, const std::string& text) {
    ed.lines.clear();
    std::size_t start = 0;
    int tabbed = 0, spaced = 0;
    ed.widest = 0;
    while (true) {
        std::size_t nl = text.find('\n', start);
        std::string line = text.substr(start, nl == std::string::npos ? std::string::npos : nl - start);
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (!line.empty() && line[0] == '\t') ++tabbed;
        if (line.rfind("  ", 0) == 0) ++spaced;
        ed.widest = std::max(ed.widest, static_cast<int>(line.size()));
        ed.lines.push_back(std::move(line));
        if (nl == std::string::npos) break;
        start = nl + 1;
    }
    ed.tabs = tabbed > spaced;
    ed.cursor = clampPos(ed, ed.cursor);
    ed.anchor = clampPos(ed, ed.anchor);
    for (auto& e : ed.extra) {
        e.cursor = clampPos(ed, e.cursor);
        e.anchor = clampPos(ed, e.anchor);
    }
    mergeCarets(ed);
    ed.dirty = false;
    ed.spansStale = true;
    ++ed.version;
}

void editorSelect(CodeEditor& ed, TextPos from, TextPos to) {
    ed.extra.clear();
    ed.anchor = clampPos(ed, from);
    ed.cursor = clampPos(ed, to);
    ed.scrollTo = ed.cursor.line;
    unfoldCarets(ed);
}

void editorFoldAll(CodeEditor& ed, bool fold) {
    ed.folded.clear();
    if (fold) {
        for (int i = 0; i < static_cast<int>(ed.lines.size()); ++i) {
            if (foldable(ed, i)) ed.folded.push_back(i);
        }
        unfoldCarets(ed);
    }
    ed.rowsVersion = ~0ull;
}

void editorAddNextMatch(CodeEditor& ed) {
    if (!hasSelection(ed)) {
        selectWord(ed);
        return;
    }
    auto [a, b] = ordered(ed);
    if (a.line != b.line) return;
    const std::string needle = textBetween(ed, a, b);
    // after the last caret, wrapping round to the top
    TextPos from = b;
    for (const auto& e : ed.extra) {
        const TextPos end = e.cursor < e.anchor ? e.anchor : e.cursor;
        if (from < end) from = end;
    }
    const int n = static_cast<int>(ed.lines.size());
    for (int k = 0; k <= n; ++k) {
        const int i = (from.line + k) % n;
        const std::string& l = ed.lines[static_cast<std::size_t>(i)];
        std::size_t at = l.find(needle, k == 0 ? static_cast<std::size_t>(from.col) : 0);
        while (at != std::string::npos) {
            const TextPos s{i, static_cast<int>(at)}, e{i, static_cast<int>(at + needle.size())};
            bool taken = s == a;
            for (const auto& c : ed.extra) taken = taken || (c.cursor < c.anchor ? c.cursor : c.anchor) == s;
            if (!taken) {
                ed.extra.push_back({e, s});
                mergeCarets(ed);
                ed.scrollTo = -1;
                unfoldCarets(ed);
                return;
            }
            at = l.find(needle, at + 1);
        }
    }
}

void editorReplaceAtCaret(CodeEditor& ed, int fromCol, const std::string& text) {
    ed.extra.clear();
    const int col = std::clamp(fromCol, 0, ed.cursor.col);
    eraseRange(ed, {ed.cursor.line, col}, ed.cursor);
    insertText(ed, text);
}

std::string editorSelection(const CodeEditor& ed) {
    auto [a, b] = ordered(ed);
    return textBetween(ed, a, b);
}

bool drawEditor(CodeEditor& ed, ImFont* mono, bool dark, ImVec2 size) {
    if (ed.lines.empty()) ed.lines.emplace_back();
    ImGui::PushFont(mono, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {0, 0});
    ImGui::BeginChild("##editor", size, ImGuiChildFlags_Borders, (ed.wrap ? 0 : ImGuiWindowFlags_HorizontalScrollbar) | ImGuiWindowFlags_NoNav);
    ImGui::PopStyleVar();
    ImGuiContext& g = *ImGui::GetCurrentContext();
    ImGuiWindow* window = ImGui::GetCurrentWindow();
    const ImGuiID id = window->GetID("##text");

    Layout lay;
    lay.lineH = std::round(ImGui::GetFontSize() * 1.35f);
    lay.charW = ImGui::CalcTextSize("0").x;
    const int digits = std::max(3, static_cast<int>(std::to_string(ed.lines.size()).size()));
    lay.gutterW = lay.charW * static_cast<float>(digits + 3);
    lay.origin = ImGui::GetCursorScreenPos();
    lay.inner = window->InnerRect;
    lay.inner.Min.x += lay.gutterW;
    lay.visibleLines = std::max(1, static_cast<int>(lay.inner.GetHeight() / lay.lineH));
    ed.lineHeight = lay.lineH;
    followLineCount(ed);
    buildRows(ed, lay.inner.GetWidth() - lay.charW * 2);
    // scroll room: every row, and the widest line unless lines wrap
    ImGui::Dummy({ed.wrap ? 1.0f : lay.gutterW + lay.charW * static_cast<float>(ed.widest + 8),
                  lay.lineH * static_cast<float>(ed.rows.size()) + lay.inner.GetHeight() * 0.5f});
    ImRect bb(window->InnerRect.Min, window->InnerRect.Max);
    ImGui::ItemAdd(bb, id);
    const bool hovered = ImGui::ItemHoverable(bb, id, ImGuiItemFlags_None);
    if (ed.focusNext) {
        ImGui::SetActiveID(id, window);
        ImGui::SetFocusID(id, window);
        ImGui::FocusWindow(window);
        ed.focusNext = false;
    }
    // the gutter: clicking a line's arrow folds or opens its block
    const ImGuiIO& io = ImGui::GetIO();
    int gutterRow = -1;
    if (hovered && io.MousePos.x < lay.inner.Min.x) {
        gutterRow = std::clamp(static_cast<int>(std::floor((io.MousePos.y - lay.origin.y) / lay.lineH)), 0, static_cast<int>(ed.rows.size()) - 1);
    }
    bool moved = false;
    if (gutterRow >= 0 && ImGui::IsMouseClicked(ImGuiMouseButton_Left) && ed.rows[static_cast<std::size_t>(gutterRow)].from == 0 &&
        (isFolded(ed, ed.rows[static_cast<std::size_t>(gutterRow)].line) || foldable(ed, ed.rows[static_cast<std::size_t>(gutterRow)].line))) {
        const int line = ed.rows[static_cast<std::size_t>(gutterRow)].line;
        setFold(ed, line, !isFolded(ed, line));
        if (isFolded(ed, line)) {
            // carets inside go to the end of the folded line
            const int end = foldEnd(ed, line);
            auto pull = [&](TextPos& p) {
                if (p.line > line && p.line <= end) p = {line, static_cast<int>(ed.lines[static_cast<std::size_t>(line)].size())};
            };
            pull(ed.cursor);
            pull(ed.anchor);
            for (auto& e : ed.extra) {
                pull(e.cursor);
                pull(e.anchor);
            }
            mergeCarets(ed);
        }
        buildRows(ed, lay.inner.GetWidth() - lay.charW * 2);
    } else {
        moved = handleMouse(ed, lay, id, hovered);
    }
    ed.active = g.ActiveId == id;
    bool changed = false;
    if (ed.active) {
        for (ImGuiKey k : {ImGuiKey_LeftArrow, ImGuiKey_RightArrow, ImGuiKey_UpArrow, ImGuiKey_DownArrow, ImGuiKey_PageUp, ImGuiKey_PageDown,
                           ImGuiKey_Home, ImGuiKey_End, ImGuiKey_Enter, ImGuiKey_KeypadEnter, ImGuiKey_Tab, ImGuiKey_Backspace,
                           ImGuiKey_Delete, ImGuiKey_Escape, ImGuiKey_F12}) {
            ImGui::SetKeyOwner(k, id);
        }
        if (ImGui::IsKeyPressed(ImGuiKey_Escape, false) && !ed.menuOpen) {
            if (!ed.extra.empty()) ed.extra.clear();
            else ed.anchor = ed.cursor;
        }
        moved = handleMoves(ed, lay) || moved;
        changed = handleEdits(ed);
        if (changed) {
            followLineCount(ed);
            unfoldCarets(ed);
        }
        moved = moved || changed;
        buildRows(ed, lay.inner.GetWidth() - lay.charW * 2);
    }
    if (moved && !ed.dragging) keepCursorVisible(ed, lay);
    if (ed.scrollTo >= 0) {
        const int r = rowOf(ed, {ed.scrollTo, 0});
        ImGui::SetScrollY(std::max(0.0f, (static_cast<float>(r) - static_cast<float>(lay.visibleLines) / 3) * lay.lineH));
        ImGui::SetScrollX(0);
        ed.scrollTo = -1;
    }

    // the visible rows
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const int first = std::clamp(static_cast<int>((lay.inner.Min.y - lay.origin.y) / lay.lineH), 0, static_cast<int>(ed.rows.size()));
    const int last = std::min(static_cast<int>(ed.rows.size()), first + lay.visibleLines + 2);
    dl->PushClipRect(lay.inner.Min, lay.inner.Max, true);
    for (int r = first; r < last; ++r) drawRow(ed, lay, r, dark, dl);
    dl->PopClipRect();
    drawGutter(ed, lay, first, last, dl, gutterRow);
    if (gutterRow >= 0) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
    // which character the mouse is over, for hover notes
    ed.mouseOnText = false;
    if (hovered && gutterRow < 0 && !ed.rows.empty() && io.MousePos.y < lay.origin.y + lay.lineH * static_cast<float>(ed.rows.size())) {
        const TextPos at = hit(ed, lay, io.MousePos);
        const std::string& l = ed.lines[static_cast<std::size_t>(at.line)];
        const float endX = lay.origin.x + lay.gutterW + rowX(ed, rowOf(ed, at), shown(l), ed.rows[static_cast<std::size_t>(rowOf(ed, at))].to);
        if (io.MousePos.x < endX && at.col < static_cast<int>(l.size())) {
            ed.mouseOnText = true;
            ed.mousePos = at;
        }
    }

    const int cr = rowOf(ed, ed.cursor);
    const float cy = lay.origin.y + static_cast<float>(cr) * lay.lineH;
    const float cx = lay.origin.x + lay.gutterW + rowX(ed, cr, shown(ed.lines[static_cast<std::size_t>(ed.cursor.line)]), ed.cursor.col);
    ed.caretScreen = {cx, cy};
    if (ed.active && !ed.readOnly) {
        // ask the platform for typed text (SDL3 sends none otherwise) and place the IME box
        g.PlatformImeData.WantVisible = true;
        g.PlatformImeData.WantTextInput = true;
        g.PlatformImeData.InputPos = {cx, cy};
        g.PlatformImeData.InputLineHeight = lay.lineH;
        g.PlatformImeData.ViewportId = window->Viewport->ID;
    }
    ImGui::EndChild();
    ImGui::PopFont();
    return changed;
}

}  // namespace wetwaresuite::gui
