#include <cstdio>
#include <cstdlib>
#include <map>
#include <set>
#include <stdexcept>

#include "wetwaresuite/pdf/page.hpp"
#include "wetwaresuite/pdfdoc/writer.hpp"
#include "wetwaresuite/raster/codec.hpp"
#include "internal.hpp"

#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmissing-declarations"
#endif
#include "stb_image.h"
#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic pop
#endif

// Defined (and exported) by stb_image_write inside libs/raster.
extern "C" unsigned char* stbi_zlib_compress(unsigned char* data, int data_len, int* out_len, int quality);

namespace wetwaresuite::pdf {

using detail::num;

void Page::text(double x, double y, const std::string& s, Font font, double size, Color color) {
    TextOp t;
    t.x = x;
    t.y = y;
    t.text = s;
    t.font = font;
    t.size = size;
    t.color = color;
    ops.emplace_back(std::move(t));
}

void Page::rect(double x, double y, double w, double h, std::optional<Color> fill, std::optional<Color> stroke, double lineWidth) {
    ops.emplace_back(RectOp{x, y, w, h, fill, stroke, lineWidth});
}

void Page::line(double x1, double y1, double x2, double y2, Color color, double width) {
    ops.emplace_back(LineOp{x1, y1, x2, y2, color, width});
}

void Page::image(double x, double y, double w, double h, std::shared_ptr<const ImageData> img) {
    ops.emplace_back(ImageOp{x, y, w, h, std::move(img)});
}

void Page::path(std::vector<std::pair<double, double>> points, bool closed, std::optional<Color> fill, std::optional<Color> stroke,
                double lineWidth) {
    ops.emplace_back(PathOp{std::move(points), closed, fill, stroke, lineWidth});
}

std::string deflate(const std::string& data, int level) {
    int outLen = 0;
    unsigned char* out = stbi_zlib_compress(reinterpret_cast<unsigned char*>(const_cast<char*>(data.data())),
                                            static_cast<int>(data.size()), &outLen, level < 5 ? 5 : level + 2);
    if (!out) throw std::runtime_error("deflate failed");
    std::string s(reinterpret_cast<char*>(out), static_cast<std::size_t>(outLen));
    std::free(out);
    return s;
}

std::string inflate(const std::string& data) {
    if (data.empty()) return {};
    int outLen = 0;
    char* out = stbi_zlib_decode_malloc_guesssize_headerflag(data.data(), static_cast<int>(data.size()),
                                                            static_cast<int>(data.size() * 4 + 64), &outLen, 1);
    if (!out) {
        // some writers leave the zlib header off
        out = stbi_zlib_decode_noheader_malloc(data.data(), static_cast<int>(data.size()), &outLen);
    }
    if (!out) throw std::runtime_error("corrupt compressed data");
    std::string s(out, static_cast<std::size_t>(outLen));
    std::free(out);
    return s;
}

std::string deflateRaw(const std::string& data, int level) {
    std::string z = deflate(data, level);
    if (z.size() < 6) throw std::runtime_error("deflate failed");
    return z.substr(2, z.size() - 6);  // drop the 2-byte header and the Adler-32 trailer
}

std::string inflateRaw(const std::string& data) {
    if (data.empty()) return {};
    int outLen = 0;
    char* out = stbi_zlib_decode_noheader_malloc(data.data(), static_cast<int>(data.size()), &outLen);
    if (!out) throw std::runtime_error("corrupt compressed data");
    std::string s(out, static_cast<std::size_t>(outLen));
    std::free(out);
    return s;
}

std::shared_ptr<ImageData> loadImageData(const std::string& bytes) {
    auto img = std::make_shared<ImageData>();
    img->pixels = raster::decode(bytes);
    if (bytes.size() > 3 && static_cast<unsigned char>(bytes[0]) == 0xFF && static_cast<unsigned char>(bytes[1]) == 0xD8) {
        int w = 0, h = 0, comp = 0;
        if (stbi_info_from_memory(reinterpret_cast<const stbi_uc*>(bytes.data()), static_cast<int>(bytes.size()), &w, &h, &comp) &&
            (comp == 1 || comp == 3)) {
            img->jpeg = bytes;
        }
    }
    return img;
}

namespace {

std::string pdfString(const std::string& utf8) {
    std::string out = "(";
    for (char ch : winAnsi(utf8)) {
        unsigned char b = static_cast<unsigned char>(ch);
        if (b == '(' || b == ')' || b == '\\') {
            out += '\\';
            out += static_cast<char>(b);
        } else if (b < 32 || b > 126) {
            char buf[8];
            std::snprintf(buf, sizeof buf, "\\%03o", b);
            out += buf;
        } else {
            out += static_cast<char>(b);
        }
    }
    return out + ")";
}

// Document info text: WinAnsi when it fits, otherwise UTF-16 so any language survives.
std::string infoString(const std::string& utf8) {
    bool fits = true;
    for (unsigned cp : utf8Decode(utf8)) fits = fits && (cp == '?' || toWinAnsi(cp) != '?');
    if (fits) return pdfString(utf8);
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

std::string rgb(Color c) { return num(c.r / 255.0) + " " + num(c.g / 255.0) + " " + num(c.b / 255.0); }

struct Builder {
    std::vector<std::string> objects;  // object i+1
    int reserve() {
        objects.emplace_back();
        return static_cast<int>(objects.size());
    }
    int add(std::string body) {
        objects.push_back(std::move(body));
        return static_cast<int>(objects.size());
    }
    void set(int id, std::string body) { objects[static_cast<std::size_t>(id - 1)] = std::move(body); }
    static std::string stream(const std::string& dictExtra, const std::string& data) {
        return "<< " + dictExtra + " /Length " + std::to_string(data.size()) + " >>\nstream\n" + data + "\nendstream";
    }
};

std::string ref(int id) { return std::to_string(id) + " 0 R"; }

}  // namespace

std::string writePdf(const std::vector<Page>& pages, const DocInfo& info) {
    Builder b;
    const int catalog = b.reserve();
    const int pagesId = b.reserve();
    std::map<int, int> fonts;                     // font index -> object
    std::map<const ImageData*, int> images;       // image -> object
    std::vector<int> pageIds;

    // embedded Unicode fonts (built in, then system fallbacks) for every character the standard fonts lack
    std::map<int, std::set<unsigned>> unicodeChars;
    for (const auto& page : pages) {
        for (const auto& op : page.ops) {
            if (auto t = std::get_if<TextOp>(&op)) {
                for (unsigned cp : utf8Decode(t->text)) {
                    if (detail::needsUnicode(cp)) unicodeChars[pdfdoc::unicodeFontIndex(cp)].insert(cp);
                }
            }
        }
    }
    std::map<int, int> unicodeFontIds;
    for (const auto& [font, chars] : unicodeChars) {
        unicodeFontIds[font] = pdfdoc::addUnicodeFont([&b](std::string body) { return b.add(std::move(body)); }, *pdfdoc::unicodeFontAt(font), chars,
                                                      font ? "F" + std::to_string(font) : std::string());
    }

    for (const auto& page : pages) {
        const double H = page.height;
        std::string c;
        std::vector<std::string> annots;
        std::map<int, bool> usedFonts;
        std::map<int, bool> usedImages;
        std::set<int> usedUnicode;
        for (const auto& op : page.ops) {
            if (auto t = std::get_if<TextOp>(&op)) {
                if (t->text.empty() || t->color.a == 0) continue;
                const int fi = fontIndex(t->font);
                if (!fonts.count(fi)) {
                    fonts[fi] = b.add(std::string("<< /Type /Font /Subtype /Type1 /BaseFont /") + baseFontName(t->font) +
                                      " /Encoding /WinAnsiEncoding >>");
                }
                usedFonts[fi] = true;
                c += "BT " + rgb(t->color) + " rg " + num(t->x) + " " + num(H - t->y) + " Td";
                for (const auto& run : detail::splitRuns(t->text)) {
                    if (run.unicode) {
                        usedUnicode.insert(run.font);
                        c += " /FU" + (run.font ? std::to_string(run.font) : std::string()) + " " + num(t->size) + " Tf " +
                             pdfdoc::glyphText(*pdfdoc::unicodeFontAt(run.font), run.text) + " Tj";
                    } else {
                        c += " /F" + std::to_string(fi) + " " + num(t->size) + " Tf " + pdfString(run.text) + " Tj";
                    }
                }
                c += " ET\n";
                const double w = textWidth(t->font, t->text, t->size);
                if (t->underline) {
                    c += rgb(t->color) + " rg " + num(t->x) + " " + num(H - t->y - t->size * 0.12) + " " + num(w) + " " +
                         num(t->size * 0.06) + " re f\n";
                }
                if (!t->link.empty()) {
                    annots.push_back("<< /Type /Annot /Subtype /Link /Rect [" + num(t->x) + " " + num(H - t->y - t->size * 0.25) + " " +
                                     num(t->x + w) + " " + num(H - t->y + t->size * 0.85) + "] /Border [0 0 0] /A << /S /URI /URI " +
                                     pdfString(t->link) + " >> >>");
                }
            } else if (auto r = std::get_if<RectOp>(&op)) {
                const bool fill = r->fill && r->fill->a > 0, stroke = r->stroke && r->stroke->a > 0;
                if (!fill && !stroke) continue;
                if (fill) c += rgb(*r->fill) + " rg ";
                if (stroke) c += rgb(*r->stroke) + " RG " + num(r->lineWidth) + " w ";
                c += num(r->x) + " " + num(H - r->y - r->h) + " " + num(r->w) + " " + num(r->h) + " re " +
                     (fill && stroke ? "B" : fill ? "f" : "S") + "\n";
            } else if (auto l = std::get_if<LineOp>(&op)) {
                if (l->color.a == 0) continue;
                c += rgb(l->color) + " RG " + num(l->width) + " w " + num(l->x1) + " " + num(H - l->y1) + " m " + num(l->x2) + " " +
                     num(H - l->y2) + " l S\n";
            } else if (auto pa = std::get_if<PathOp>(&op)) {
                const bool fill = pa->closed && pa->fill && pa->fill->a > 0, stroke = pa->stroke && pa->stroke->a > 0;
                if ((!fill && !stroke) || pa->points.size() < 2) continue;
                if (fill) c += rgb(*pa->fill) + " rg ";
                if (stroke) c += rgb(*pa->stroke) + " RG " + num(pa->lineWidth) + " w 1 j ";
                for (std::size_t i = 0; i < pa->points.size(); ++i) {
                    c += num(pa->points[i].first) + " " + num(H - pa->points[i].second) + (i == 0 ? " m " : " l ");
                }
                c += std::string(pa->closed ? "h " : "") + (fill && stroke ? "B" : fill ? "f" : "S") + "\n";
            } else if (auto im = std::get_if<ImageOp>(&op)) {
                if (!im->image || im->image->pixels.empty()) continue;
                const ImageData* key = im->image.get();
                if (!images.count(key)) {
                    const auto& px = key->pixels;
                    const int W = px.width(), Ht = px.height();
                    std::string dims = "/Type /XObject /Subtype /Image /Width " + std::to_string(W) + " /Height " + std::to_string(Ht) +
                                       " /BitsPerComponent 8";
                    if (!key->jpeg.empty()) {
                        int w = 0, h = 0, comp = 3;
                        stbi_info_from_memory(reinterpret_cast<const stbi_uc*>(key->jpeg.data()), static_cast<int>(key->jpeg.size()), &w,
                                              &h, &comp);
                        images[key] = b.add(Builder::stream(dims + (comp == 1 ? " /ColorSpace /DeviceGray" : " /ColorSpace /DeviceRGB") +
                                                                " /Filter /DCTDecode",
                                                            key->jpeg));
                    } else {
                        std::string rgbBytes, alpha;
                        rgbBytes.reserve(static_cast<std::size_t>(W) * Ht * 3);
                        bool hasAlpha = false;
                        for (int y = 0; y < Ht; ++y) {
                            for (int x = 0; x < W; ++x) {
                                const std::uint8_t* p = px.pixel(x, y);
                                rgbBytes += static_cast<char>(p[0]);
                                rgbBytes += static_cast<char>(p[1]);
                                rgbBytes += static_cast<char>(p[2]);
                                alpha += static_cast<char>(p[3]);
                                hasAlpha = hasAlpha || p[3] != 255;
                            }
                        }
                        std::string smask;
                        if (hasAlpha) {
                            int m = b.add(Builder::stream("/Type /XObject /Subtype /Image /Width " + std::to_string(W) + " /Height " +
                                                              std::to_string(Ht) + " /BitsPerComponent 8 /ColorSpace /DeviceGray /Filter /FlateDecode",
                                                          deflate(alpha)));
                            smask = " /SMask " + ref(m);
                        }
                        images[key] = b.add(Builder::stream(dims + " /ColorSpace /DeviceRGB /Filter /FlateDecode" + smask, deflate(rgbBytes)));
                    }
                }
                usedImages[images[key]] = true;
                c += "q " + num(im->w) + " 0 0 " + num(im->h) + " " + num(im->x) + " " + num(H - im->y - im->h) + " cm /Im" +
                     std::to_string(images[key]) + " Do Q\n";
            }
        }
        const int content = b.add(Builder::stream("/Filter /FlateDecode", deflate(c)));
        std::string res = "<< /ProcSet [/PDF /Text /ImageC]";
        if (!usedFonts.empty()) {
            res += " /Font <<";
            for (const auto& [fi, used] : usedFonts) res += " /F" + std::to_string(fi) + " " + ref(fonts[fi]);
            for (int f : usedUnicode) res += " /FU" + (f ? std::to_string(f) : std::string()) + " " + ref(unicodeFontIds[f]);
            res += " >>";
        }
        if (!usedImages.empty()) {
            res += " /XObject <<";
            for (const auto& [id, used] : usedImages) res += " /Im" + std::to_string(id) + " " + ref(id);
            res += " >>";
        }
        res += " >>";
        std::string annotRefs;
        for (auto& a : annots) annotRefs += (annotRefs.empty() ? "" : " ") + ref(b.add(std::move(a)));
        pageIds.push_back(b.add("<< /Type /Page /Parent " + ref(pagesId) + " /MediaBox [0 0 " + num(page.width) + " " + num(page.height) +
                                "] /Resources " + res + " /Contents " + ref(content) +
                                (annotRefs.empty() ? "" : " /Annots [" + annotRefs + "]") + " >>"));
    }
    if (pageIds.empty()) {  // a PDF needs at least one page
        const int content = b.add(Builder::stream("", ""));
        pageIds.push_back(b.add("<< /Type /Page /Parent " + ref(pagesId) + " /MediaBox [0 0 595.28 841.89] /Resources << >> /Contents " +
                                ref(content) + " >>"));
    }
    std::string kids;
    for (int id : pageIds) kids += (kids.empty() ? "" : " ") + ref(id);
    b.set(pagesId, "<< /Type /Pages /Kids [" + kids + "] /Count " + std::to_string(pageIds.size()) + " >>");
    b.set(catalog, "<< /Type /Catalog /Pages " + ref(pagesId) + " >>");
    std::string infoDict = "<< /Producer (wetwaresuite) /Creator (wetwaresuite office)";
    if (!info.title.empty()) infoDict += " /Title " + infoString(info.title);
    if (!info.author.empty()) infoDict += " /Author " + infoString(info.author);
    if (!info.subject.empty()) infoDict += " /Subject " + infoString(info.subject);
    const int infoId = b.add(infoDict + " >>");

    std::string out = "%PDF-1.4\n%\xE2\xE3\xCF\xD3\n";
    std::vector<std::size_t> offsets;
    for (std::size_t i = 0; i < b.objects.size(); ++i) {
        offsets.push_back(out.size());
        out += std::to_string(i + 1) + " 0 obj\n" + b.objects[i] + "\nendobj\n";
    }
    const std::size_t xref = out.size();
    out += "xref\n0 " + std::to_string(b.objects.size() + 1) + "\n0000000000 65535 f \n";
    for (auto off : offsets) {
        char buf[32];
        std::snprintf(buf, sizeof buf, "%010zu 00000 n \n", off);
        out += buf;
    }
    out += "trailer\n<< /Size " + std::to_string(b.objects.size() + 1) + " /Root " + ref(catalog) + " /Info " + ref(infoId) +
           " >>\nstartxref\n" + std::to_string(xref) + "\n%%EOF\n";
    return out;
}

}  // namespace wetwaresuite::pdf
