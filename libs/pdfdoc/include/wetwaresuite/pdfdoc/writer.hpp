// A small PDF writer: numbered objects, (compressed) streams, images, the
// standard fonts and embedded TrueType fonts. Callers build content streams
// with the PDF operators themselves; this keeps the file structure right.
#pragma once

#include <functional>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "wetwaresuite/pdfdoc/fonts.hpp"
#include "wetwaresuite/raster/image.hpp"

namespace wetwaresuite::pdfdoc {

class Writer {
public:
    int reserve();                          // a new object number to fill in later
    void set(int id, std::string body);     // body without "n 0 obj"/"endobj"
    int add(std::string body);
    // A stream object. `dict` holds extra entries (without << >> or /Length); data is
    // Flate-compressed when `compress` is true.
    int addStream(const std::string& dict, const std::string& data, bool compress = true);
    // The finished file. `info` may be 0.
    std::string finish(int root, int info, const std::string& version = "1.4") const;

private:
    std::vector<std::string> objects_;
};

std::string num(double v);                     // compact: up to 4 decimals, no trailing zeros
std::string literal(const std::string& bytes);  // (escaped string) of raw bytes
std::string text(const std::string& utf8);      // literal() of the WinAnsi bytes
std::string deflate(const std::string& data);   // zlib stream

// Image XObjects. RGB with an /SMask when any pixel is not opaque.
int addImage(Writer& w, const raster::Image& image);
// Embeds a baseline/progressive JPEG file unchanged (DCTDecode). Returns 0 if the bytes
// aren't a JPEG this can pass through (then decode and use addImage).
int addJpeg(Writer& w, const std::string& bytes, int* width = nullptr, int* height = nullptr);

// Font dictionaries using WinAnsiEncoding.
int addStandardFont(Writer& w, Standard font);
// Embeds the whole TrueType file. `widthOf` (1/1000 em per WinAnsi code) overrides the
// font's own advances, so a substitute font keeps another font's spacing.
int addTrueTypeFont(Writer& w, const TrueType& font, const std::function<double(int)>& widthOf = nullptr,
                    const std::string& nameSuffix = {});

// A Type0 font (Identity-H: each character is its 2-byte glyph number) embedding the part of
// `font` that `codepoints` need, with a ToUnicode map so the text copies and searches. Use it
// for text the WinAnsi fonts can't show (Greek, Cyrillic, ...). `addObject` adds one object body
// and returns its number, so writers other than Writer can use it too.
int addUnicodeFont(const std::function<int(std::string)>& addObject, const TrueType& font, const std::set<unsigned>& codepoints,
                   const std::string& nameSuffix = {});
int addUnicodeFont(Writer& w, const TrueType& font, const std::set<unsigned>& codepoints, const std::string& nameSuffix = {});
// <hex> string of the glyph numbers, for text shown with a font from addUnicodeFont.
std::string glyphText(const TrueType& font, const std::string& utf8);

// Document info text (/Title ...): WinAnsi when it fits, UTF-16 otherwise.
std::string infoText(const std::string& utf8);

// Lets text set in the standard fonts carry any language: show() writes the operators for one
// string, switching to a Unicode font for characters WinAnsi lacks (the built-in one as resource
// /FU, system fallbacks for CJK, emoji... as /FU1, /FU2 ...) and back to `fontResource` (e.g. "F1")
// after. Put resource() in the page's /Font dictionary and call finish() once before
// Writer::finish, which embeds just the characters used.
class UnicodeFallback {
public:
    explicit UnicodeFallback(Writer& w) : w_(w) {}
    std::string show(const std::string& utf8, const std::string& fontResource, double size);
    bool used() const { return !used_.empty(); }
    int id() const;                // the built-in font's object number, 0 when unused
    std::string resource() const;  // " /FU n 0 R /FU1 m 0 R ...", or empty when nothing needed it
    std::vector<std::pair<std::string, int>> resources() const;  // the same as (name, object) pairs
    void finish();
    static std::string resourceName(int font);  // "FU", "FU1", ...

private:
    struct Used {
        int id = 0;
        std::set<unsigned> chars;
        bool finished = false;
    };
    Writer& w_;
    mutable std::map<int, Used> used_;  // font index (see unicodeFontIndex) -> object and characters
};

}  // namespace wetwaresuite::pdfdoc
