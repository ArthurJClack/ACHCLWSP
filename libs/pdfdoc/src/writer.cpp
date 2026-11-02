#include "wetwaresuite/pdfdoc/writer.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <stdexcept>

#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
#pragma GCC diagnostic ignored "-Wsign-compare"
#pragma GCC diagnostic ignored "-Wunused-but-set-variable"
#endif
#if defined(_MSC_VER)
#pragma warning(push, 0)
#endif
// A private copy of stb_image_write for its zlib compressor (static, so it can't clash with libs/raster's).
#define STB_IMAGE_WRITE_STATIC
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"
#if defined(_MSC_VER)
#pragma warning(pop)
#endif
#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic pop
#endif

namespace wetwaresuite::pdfdoc {

int Writer::reserve() {
    objects_.emplace_back();
    return static_cast<int>(objects_.size());
}

void Writer::set(int id, std::string body) { objects_.at(static_cast<std::size_t>(id - 1)) = std::move(body); }

int Writer::add(std::string body) {
    objects_.push_back(std::move(body));
    return static_cast<int>(objects_.size());
}

int Writer::addStream(const std::string& dict, const std::string& data, bool compress) {
    std::string body = compress ? deflate(data) : data;
    std::string head = "<< " + dict + (dict.empty() ? "" : " ") + "/Length " + std::to_string(body.size()) +
                       (compress ? " /Filter /FlateDecode" : "") + " >>\nstream\n";
    return add(head + body + "\nendstream");
}

std::string Writer::finish(int root, int info, const std::string& version) const {
    std::string out = "%PDF-" + version + "\n%\xE2\xE3\xCF\xD3\n";
    std::vector<std::size_t> offsets;
    for (std::size_t i = 0; i < objects_.size(); ++i) {
        offsets.push_back(out.size());
        out += std::to_string(i + 1) + " 0 obj\n" + objects_[i] + "\nendobj\n";
    }
    const std::size_t xref = out.size();
    out += "xref\n0 " + std::to_string(objects_.size() + 1) + "\n0000000000 65535 f \n";
    for (auto off : offsets) {
        char buf[32];
        std::snprintf(buf, sizeof buf, "%010zu 00000 n \n", off);
        out += buf;
    }
    out += "trailer\n<< /Size " + std::to_string(objects_.size() + 1) + " /Root " + std::to_string(root) + " 0 R";
    if (info) out += " /Info " + std::to_string(info) + " 0 R";
    out += " >>\nstartxref\n" + std::to_string(xref) + "\n%%EOF\n";
    return out;
}

std::string num(double v) {
    if (!std::isfinite(v)) v = 0;
    if (std::abs(v) < 0.00005) return "0";
    char buf[48];
    std::snprintf(buf, sizeof buf, "%.4f", v);
    std::string s = buf;
    while (!s.empty() && s.back() == '0') s.pop_back();
    if (!s.empty() && s.back() == '.') s.pop_back();
    return s;
}

std::string literal(const std::string& bytes) {
    std::string out = "(";
    for (char ch : bytes) {
        const auto b = static_cast<unsigned char>(ch);
        if (b == '(' || b == ')' || b == '\\') {
            out += '\\';
            out += ch;
        } else if (b < 32 || b > 126) {
            char buf[8];
            std::snprintf(buf, sizeof buf, "\\%03o", b);
            out += buf;
        } else {
            out += ch;
        }
    }
    return out + ")";
}

std::string text(const std::string& utf8) { return literal(toWinAnsi(utf8)); }

std::string deflate(const std::string& data) {
    int len = 0;
    unsigned char* z = stbi_zlib_compress(reinterpret_cast<unsigned char*>(const_cast<char*>(data.data())),
                                          static_cast<int>(data.size()), &len, 6);
    if (!z) throw std::runtime_error("compression failed");
    std::string out(reinterpret_cast<char*>(z), static_cast<std::size_t>(len));
    STBIW_FREE(z);
    return out;
}

int addImage(Writer& w, const raster::Image& image) {
    const int width = image.width(), height = image.height();
    const std::size_t n = static_cast<std::size_t>(width) * static_cast<std::size_t>(height);
    std::string rgb(n * 3, '\0'), alpha(n, '\0');
    bool opaque = true;
    const std::uint8_t* px = image.data();
    for (std::size_t i = 0; i < n; ++i) {
        rgb[i * 3] = static_cast<char>(px[i * 4]);
        rgb[i * 3 + 1] = static_cast<char>(px[i * 4 + 1]);
        rgb[i * 3 + 2] = static_cast<char>(px[i * 4 + 2]);
        alpha[i] = static_cast<char>(px[i * 4 + 3]);
        opaque = opaque && px[i * 4 + 3] == 255;
    }
    const std::string size = "/Width " + std::to_string(width) + " /Height " + std::to_string(height) + " /BitsPerComponent 8";
    std::string dict = "/Type /XObject /Subtype /Image " + size + " /ColorSpace /DeviceRGB";
    if (!opaque) {
        int mask = w.addStream("/Type /XObject /Subtype /Image " + size + " /ColorSpace /DeviceGray", alpha);
        dict += " /SMask " + std::to_string(mask) + " 0 R";
    }
    return w.addStream(dict, rgb);
}

int addJpeg(Writer& w, const std::string& bytes, int* width, int* height) {
    auto at = [&](std::size_t i) { return i < bytes.size() ? static_cast<unsigned>(static_cast<unsigned char>(bytes[i])) : 0u; };
    if (bytes.size() < 4 || at(0) != 0xFF || at(1) != 0xD8) return 0;
    std::size_t i = 2;
    bool adobe = false;
    while (i + 9 < bytes.size()) {
        if (at(i) != 0xFF) return 0;
        const unsigned marker = at(i + 1);
        if (marker == 0xD8 || (marker >= 0xD0 && marker <= 0xD7) || marker == 0x01 || marker == 0xFF) {
            i += marker == 0xFF ? 1 : 2;
            continue;
        }
        const std::size_t len = (at(i + 2) << 8) | at(i + 3);
        if (marker == 0xEE && bytes.compare(i + 4, 5, "Adobe") == 0) adobe = true;
        if (marker == 0xC0 || marker == 0xC1 || marker == 0xC2) {
            const unsigned bits = at(i + 4);
            const int h = static_cast<int>((at(i + 5) << 8) | at(i + 6));
            const int wd = static_cast<int>((at(i + 7) << 8) | at(i + 8));
            const unsigned comps = at(i + 9);
            if (bits != 8 || wd <= 0 || h <= 0) return 0;
            std::string cs;
            if (comps == 1) cs = "/DeviceGray";
            else if (comps == 3) cs = "/DeviceRGB";
            else if (comps == 4) cs = std::string("/DeviceCMYK") + (adobe ? " /Decode [1 0 1 0 1 0 1 0]" : "");
            else return 0;
            if (width) *width = wd;
            if (height) *height = h;
            return w.addStream("/Type /XObject /Subtype /Image /Width " + std::to_string(wd) + " /Height " + std::to_string(h) +
                                   " /BitsPerComponent 8 /ColorSpace " + cs + " /Filter /DCTDecode",
                               bytes, false);
        }
        if (len < 2) return 0;
        i += 2 + len;
    }
    return 0;
}

int addStandardFont(Writer& w, Standard font) {
    return w.add(std::string("<< /Type /Font /Subtype /Type1 /BaseFont /") + baseFontName(font) + " /Encoding /WinAnsiEncoding >>");
}

int addTrueTypeFont(Writer& w, const TrueType& font, const std::function<double(int)>& widthOf, const std::string& nameSuffix) {
    if (!font.embeddable()) throw std::runtime_error(font.path().string() + " has no TrueType outlines (only .ttf fonts can be embedded)");
    const std::string name = font.postscriptName() + nameSuffix;
    const VMetrics m = font.metrics();
    double box[4];
    font.bbox(box);
    const double angle = font.italicAngle();
    int flags = 32;  // nonsymbolic
    if (font.fixedPitch()) flags |= 1;
    if (angle != 0) flags |= 64;
    const int file = w.addStream("/Length1 " + std::to_string(font.bytes().size()), font.bytes());
    const int desc = w.add("<< /Type /FontDescriptor /FontName /" + name + " /Flags " + std::to_string(flags) + " /FontBBox [" +
                           num(box[0]) + " " + num(box[1]) + " " + num(box[2]) + " " + num(box[3]) + "] /ItalicAngle " + num(angle) +
                           " /Ascent " + num(m.ascent) + " /Descent " + num(m.descent) + " /CapHeight " + num(m.capHeight) +
                           " /StemV 80 /FontFile2 " + std::to_string(file) + " 0 R >>");
    std::string widths;
    for (int c = 32; c <= 255; ++c) {
        const unsigned u = unicodeOf(c);
        double adv = widthOf ? widthOf(c) : (u ? font.advance(u) : 0);
        widths += (c == 32 ? "" : " ") + num(std::round(adv));
    }
    return w.add("<< /Type /Font /Subtype /TrueType /BaseFont /" + name + " /FirstChar 32 /LastChar 255 /Widths [" + widths +
                 "] /Encoding /WinAnsiEncoding /FontDescriptor " + std::to_string(desc) + " 0 R >>");
}

namespace {

// The Type0 font dictionary; the objects it points to are added through `addObject`.
std::string unicodeFontDict(const std::function<int(std::string)>& addObject, const TrueType& font, const std::set<unsigned>& codepoints,
                            const std::string& nameSuffix) {
    if (!font.embeddable()) throw std::runtime_error(font.path().string() + " has no TrueType outlines (only .ttf fonts can be embedded)");
    std::map<int, unsigned> glyphs;  // glyph -> first code point using it
    for (unsigned cp : codepoints) {
        const int g = font.glyphIndex(cp);
        if (g && !glyphs.count(g)) glyphs[g] = cp;
    }
    std::set<int> keep;
    for (const auto& [g, cp] : glyphs) keep.insert(g);
    // six letters before the name mark a subset, as the PDF spec asks
    std::string tag;
    unsigned h = 2166136261u;
    for (int g : keep) h = (h ^ static_cast<unsigned>(g)) * 16777619u;
    for (int i = 0; i < 6; ++i, h /= 26) tag += static_cast<char>('A' + h % 26);
    const std::string name = tag + "+" + font.postscriptName() + nameSuffix;

    auto stream = [](const std::string& dict, const std::string& data) {
        const std::string z = deflate(data);
        return "<< " + dict + (dict.empty() ? "" : " ") + "/Length " + std::to_string(z.size()) + " /Filter /FlateDecode >>\nstream\n" +
               z + "\nendstream";
    };
    // big fonts (CJK has tens of thousands of glyphs) get renumbered, so the per-glyph tables shrink
    // too; the text keeps using the old numbers as CIDs and a CIDToGIDMap points them at the new ones
    std::vector<int> oldOf;
    const bool compact = font.glyphCount() > 2000 && !keep.empty();
    const std::string sub = compact ? font.subsetCompact(keep, oldOf) : font.subset(keep);
    const int file = addObject(stream("/Length1 " + std::to_string(sub.size()), sub));
    std::string cidToGid = "/Identity";
    if (compact) {
        std::string map(static_cast<std::size_t>(*keep.rbegin() + 1) * 2, '\0');
        for (std::size_t n = 1; n < oldOf.size(); ++n) {
            const auto cid = static_cast<std::size_t>(oldOf[n]);
            if (cid * 2 + 1 >= map.size()) continue;
            map[cid * 2] = static_cast<char>((n >> 8) & 0xFF);
            map[cid * 2 + 1] = static_cast<char>(n & 0xFF);
        }
        cidToGid = std::to_string(addObject(stream("", map))) + " 0 R";
    }
    const VMetrics m = font.metrics();
    double box[4];
    font.bbox(box);
    const double angle = font.italicAngle();
    int flags = 4;  // symbolic: glyphs are picked by number
    if (font.fixedPitch()) flags |= 1;
    if (angle != 0) flags |= 64;
    const int desc = addObject("<< /Type /FontDescriptor /FontName /" + name + " /Flags " + std::to_string(flags) + " /FontBBox [" +
                               num(box[0]) + " " + num(box[1]) + " " + num(box[2]) + " " + num(box[3]) + "] /ItalicAngle " + num(angle) +
                               " /Ascent " + num(m.ascent) + " /Descent " + num(m.descent) + " /CapHeight " + num(m.capHeight) +
                               " /StemV 80 /FontFile2 " + std::to_string(file) + " 0 R >>");
    std::string widths = "[";
    for (const auto& [g, cp] : glyphs) widths += " " + std::to_string(g) + " [" + num(std::round(font.glyphAdvance(g))) + "]";
    widths += " ]";
    const int cid = addObject("<< /Type /Font /Subtype /CIDFontType2 /BaseFont /" + name +
                              " /CIDSystemInfo << /Registry (Adobe) /Ordering (Identity) /Supplement 0 >> /FontDescriptor " +
                              std::to_string(desc) + " 0 R /DW " + num(std::round(font.glyphAdvance(0))) + " /W " + widths +
                              " /CIDToGIDMap " + cidToGid + " >>");

    auto hex4 = [](unsigned v) {
        char buf[8];
        std::snprintf(buf, sizeof buf, "%04X", v & 0xFFFF);
        return std::string(buf);
    };
    auto utf16 = [&](unsigned cp) {
        if (cp < 0x10000) return hex4(cp);
        cp -= 0x10000;
        return hex4(0xD800 + (cp >> 10)) + hex4(0xDC00 + (cp & 0x3FF));
    };
    std::string cmap =
        "/CIDInit /ProcSet findresource begin\n12 dict begin\nbegincmap\n/CIDSystemInfo << /Registry (Adobe) /Ordering (UCS) /Supplement 0 >> def\n"
        "/CMapName /Adobe-Identity-UCS def\n/CMapType 2 def\n1 begincodespacerange\n<0000> <FFFF>\nendcodespacerange\n";
    std::vector<std::pair<int, unsigned>> pairs(glyphs.begin(), glyphs.end());
    for (std::size_t i = 0; i < pairs.size(); i += 100) {
        const std::size_t n = std::min<std::size_t>(100, pairs.size() - i);
        cmap += std::to_string(n) + " beginbfchar\n";
        for (std::size_t k = i; k < i + n; ++k) {
            cmap += "<" + hex4(static_cast<unsigned>(pairs[k].first)) + "> <" + utf16(pairs[k].second) + ">\n";
        }
        cmap += "endbfchar\n";
    }
    cmap += "endcmap\nCMapName currentdict /CMap defineresource pop\nend\nend\n";
    const int toUnicode = addObject(stream("", cmap));
    return "<< /Type /Font /Subtype /Type0 /BaseFont /" + name + " /Encoding /Identity-H /DescendantFonts [" + std::to_string(cid) +
           " 0 R] /ToUnicode " + std::to_string(toUnicode) + " 0 R >>";
}

}  // namespace

int addUnicodeFont(const std::function<int(std::string)>& addObject, const TrueType& font, const std::set<unsigned>& codepoints,
                   const std::string& nameSuffix) {
    return addObject(unicodeFontDict(addObject, font, codepoints, nameSuffix));
}

int addUnicodeFont(Writer& w, const TrueType& font, const std::set<unsigned>& codepoints, const std::string& nameSuffix) {
    return addUnicodeFont([&w](std::string body) { return w.add(std::move(body)); }, font, codepoints, nameSuffix);
}

std::string glyphText(const TrueType& font, const std::string& utf8) {
    std::string out = "<";
    char buf[8];
    for (unsigned cp : utf8Decode(utf8)) {
        if (cp == '\n' || cp == '\r') continue;
        std::snprintf(buf, sizeof buf, "%04X", static_cast<unsigned>(font.glyphIndex(cp == '\t' ? ' ' : cp)) & 0xFFFF);
        out += buf;
    }
    return out + ">";
}

bool needsUnicode(unsigned cp) { return cp >= 0x100 && winAnsi(cp) < 0 && unicodeFontIndex(cp) >= 0; }

std::string infoText(const std::string& utf8) {
    bool fits = true;
    for (unsigned cp : utf8Decode(utf8)) fits = fits && (cp == '?' || winAnsi(cp) >= 0);
    if (fits) return text(utf8);
    std::string out = "<FEFF";
    char buf[16];
    for (unsigned cp : utf8Decode(utf8)) {
        if (cp >= 0x10000) {
            cp -= 0x10000;
            std::snprintf(buf, sizeof buf, "%04X%04X", 0xD800 + (cp >> 10), 0xDC00 + (cp & 0x3FF));
        } else {
            std::snprintf(buf, sizeof buf, "%04X", cp);
        }
        out += buf;
    }
    return out + ">";
}

std::string UnicodeFallback::show(const std::string& utf8, const std::string& fontResource, double size) {
    std::string out, plain, wide;
    int wideFont = -1;
    auto flushPlain = [&] {
        if (plain.empty()) return;
        out += (out.empty() ? "" : " ") + text(plain) + " Tj";
        plain.clear();
    };
    auto flushWide = [&] {
        if (wide.empty()) return;
        Used& u = used_[wideFont];
        if (!u.id) u.id = w_.reserve();
        out += (out.empty() ? "" : " ") + std::string("/") + resourceName(wideFont) + " " + num(size) + " Tf " +
               glyphText(*unicodeFontAt(wideFont), wide) + " Tj /" + fontResource + " " + num(size) + " Tf";
        wide.clear();
    };
    for (unsigned cp : utf8Decode(utf8)) {
        const int f = cp >= 0x100 && winAnsi(cp) < 0 ? unicodeFontIndex(cp) : -1;
        if (f >= 0) {
            flushPlain();
            if (f != wideFont) flushWide();
            wideFont = f;
            used_[f].chars.insert(cp);
            wide += utf8Encode(cp);
        } else {
            flushWide();
            plain += utf8Encode(cp);
        }
    }
    flushPlain();
    flushWide();
    return out.empty() ? "() Tj" : out;
}

std::string UnicodeFallback::resourceName(int font) { return font == 0 ? "FU" : "FU" + std::to_string(font); }

int UnicodeFallback::id() const {
    auto it = used_.find(0);
    return it == used_.end() ? 0 : it->second.id;
}

std::string UnicodeFallback::resource() const {
    std::string r;
    for (const auto& [font, u] : used_) r += " /" + resourceName(font) + " " + std::to_string(u.id) + " 0 R";
    return r;
}

std::vector<std::pair<std::string, int>> UnicodeFallback::resources() const {
    std::vector<std::pair<std::string, int>> r;
    for (const auto& [font, u] : used_) r.emplace_back(resourceName(font), u.id);
    return r;
}

void UnicodeFallback::finish() {
    for (auto& [font, u] : used_) {
        if (u.finished) continue;
        w_.set(u.id, unicodeFontDict([this](std::string body) { return w_.add(std::move(body)); }, *unicodeFontAt(font), u.chars,
                                     font == 0 ? std::string() : "F" + std::to_string(font)));
        u.finished = true;
    }
}

}  // namespace wetwaresuite::pdfdoc
