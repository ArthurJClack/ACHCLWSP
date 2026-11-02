// Fonts for PDF output and previews: metrics of the standard PDF fonts
// (Helvetica, Times, Courier with their bold/italic faces), the WinAnsi
// encoding they use, TrueType files (metrics, outlines, embedding) and a
// lookup of metric-compatible system fonts so previews look like the PDF.
#pragma once

#include <filesystem>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace wetwaresuite::pdfdoc {

// ---- text encoding ----------------------------------------------------------

std::vector<unsigned> utf8Decode(const std::string& text);  // stray bytes are read as Latin-1
std::string utf8Encode(unsigned codepoint);
int winAnsi(unsigned codepoint);    // WinAnsi (CP1252) code 32..255, or -1 when it has none
unsigned unicodeOf(int winAnsiCode);  // inverse of winAnsi()
std::string toWinAnsi(const std::string& utf8);  // unmapped characters become '?'

// ---- the standard fonts ----------------------------------------------------

enum class Standard {
    Helvetica, HelveticaBold, HelveticaOblique, HelveticaBoldOblique,
    TimesRoman, TimesBold, TimesItalic, TimesBoldItalic,
    Courier, CourierBold, CourierOblique, CourierBoldOblique,
};

// "helvetica", "times" or "courier" for a family name or alias (sans, arial, serif, roman, mono, ...).
std::optional<std::string> standardFamily(const std::string& name);
Standard standardFont(const std::string& family, bool bold, bool italic);  // family from standardFamily()
const char* baseFontName(Standard font);  // "Helvetica-Bold", ...
bool isBold(Standard font);
bool isItalic(Standard font);
double standardWidth(Standard font, int winAnsiCode);  // advance in 1/1000 em

struct VMetrics {
    double ascent = 750, descent = -250, capHeight = 700;  // 1/1000 em, descent negative
};
VMetrics standardMetrics(Standard font);

// ---- TrueType ---------------------------------------------------------------

struct Pt {
    double x = 0, y = 0;
};
using Contours = std::vector<std::vector<Pt>>;  // closed outlines

class TrueType {
public:
    // Reads a .ttf (or the first face of a .ttc). Throws std::runtime_error.
    static std::shared_ptr<TrueType> load(const std::filesystem::path& path);
    // The same from the file's bytes; `name` stands in for the path in messages.
    static std::shared_ptr<TrueType> fromBytes(std::string bytes, const std::string& name);
    ~TrueType();
    TrueType(const TrueType&) = delete;
    TrueType& operator=(const TrueType&) = delete;

    const std::filesystem::path& path() const;
    const std::string& bytes() const;
    std::string postscriptName() const;  // safe for a PDF name
    bool embeddable() const;  // TrueType outlines (glyf), not CFF
    bool hasGlyph(unsigned codepoint) const;
    double advance(unsigned codepoint) const;  // 1/1000 em
    VMetrics metrics() const;
    double italicAngle() const;
    bool fixedPitch() const;
    void bbox(double out[4]) const;  // 1/1000 em
    // Glyph outline in 1/1000 em, y up, curves flattened. Cached.
    const Contours& outline(unsigned codepoint) const;
    // The glyph as SVG path data (M L Q C Z, curves kept), `size` px per em, origin on the
    // baseline at (x, y), y down. Empty for a blank glyph like space.
    std::string svgPath(unsigned codepoint, double x, double y, double size) const;
    double kern(unsigned left, unsigned right) const;  // extra advance between the pair, 1/1000 em
    int glyphIndex(unsigned codepoint) const;  // 0 (the missing-glyph box) when the font has none
    int glyphCount() const;
    double glyphAdvance(int glyph) const;      // 1/1000 em
    // A copy of the font that keeps only these glyphs' outlines (plus glyph 0 and the parts of
    // composite glyphs); glyph numbers stay the same. Much smaller for embedding.
    std::string subset(const std::set<int>& glyphs) const;
    // The same, renumbered 0..n-1 so tables sized per glyph shrink too (big CJK fonts): oldOf[new]
    // is each glyph's old number, glyph 0 first. Its cmap is empty: pick glyphs by their new numbers.
    std::string subsetCompact(const std::set<int>& glyphs, std::vector<int>& oldOf) const;

private:
    TrueType();
    std::string subsetImpl(const std::set<int>& glyphs, std::vector<int>* compact) const;
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// A system font for previews (and optional embedding) of a standard family:
// Liberation/Arimo/Tinos/Cousine (metric-compatible), Arial/Times New Roman/Courier New,
// then DejaVu. Searched once in the usual font folders and $WETWARESUITE_FONT_DIR; null when none.
std::shared_ptr<TrueType> systemFont(const std::string& family, bool bold, bool italic);

// Roboto Medium (Apache 2.0), built in so text looks the same on every machine. It covers
// Latin, Greek and Cyrillic.
std::shared_ptr<TrueType> builtinSans();
// True when WinAnsi has no byte for the character but a Unicode font has a glyph for it, so PDF
// writers set it in that font (see UnicodeFallback in writer.hpp).
bool needsUnicode(unsigned codepoint);

// The fonts behind needsUnicode, in the order they are tried: 0 is the built-in sans; 1.. are
// system fonts found when a character needs them: Chinese, Japanese and Korean (Microsoft YaHei,
// MS Gothic, Malgun Gothic, PingFang, Arial Unicode, WenQuanYi, Droid Sans Fallback, Noto Sans
// SC/JP/KR...), other scripts (Arial, Tahoma, Nirmala, DejaVu, FreeSans...), symbols and
// black-and-white emoji (Segoe UI Emoji, Noto Emoji, Symbola). Only fonts with TrueType outlines,
// since those embed as small subsets. WETWARESUITE_FALLBACK_FONT (font files separated by ; or :)
// and the fonts in $WETWARESUITE_FONT_DIR come first.
//   unicodeFontIndex  the index for a character, -1 when no font has it
//   unicodeFontAt     the font at an index (null past the end)
int unicodeFontIndex(unsigned codepoint);
std::shared_ptr<TrueType> unicodeFontAt(int index);
std::shared_ptr<TrueType> unicodeFontFor(unsigned codepoint);  // null when none

}  // namespace wetwaresuite::pdfdoc
