// pdf.save: the open file rewritten with its edits. Annotation-like edits
// (highlights, notes, ink, shapes) become real PDF annotations with their own
// appearance, so other viewers show them and can edit or delete them; text,
// pictures, white-out, redaction and signatures are drawn into the page.
#include <algorithm>
#include <chrono>
#include <cmath>
#include <ctime>
#include <set>
#include <sstream>

#include "internal.hpp"
#include "wetwaresuite/pdf/edit.hpp"
#include "wetwaresuite/pdfdoc/fonts.hpp"
#include "wetwaresuite/pdfdoc/writer.hpp"

namespace wetwaresuite::pdf::tool {

namespace {

using pdfdoc::num;

std::string rgb(Color c) { return num(c.r / 255.0) + " " + num(c.g / 255.0) + " " + num(c.b / 255.0); }

std::string pdfDate() {
    const std::time_t t = std::time(nullptr);
    std::tm tmv{};
#ifdef _WIN32
    gmtime_s(&tmv, &t);
#else
    gmtime_r(&t, &tmv);
#endif
    char buf[32];
    std::strftime(buf, sizeof buf, "D:%Y%m%d%H%M%SZ", &tmv);
    return buf;
}

pdfdoc::Standard standardOf(const Font& f) {
    const char* fam = f.family == Family::Serif ? "times" : f.family == Family::Mono ? "courier" : "helvetica";
    return pdfdoc::standardFont(fam, f.bold, f.italic);
}

// JPEG component count from its frame header (0 when it can't tell)
int jpegComponents(const std::string& b) {
    for (std::size_t i = 2; i + 9 < b.size();) {
        if (static_cast<unsigned char>(b[i]) != 0xFF) return 0;
        const unsigned char m = static_cast<unsigned char>(b[i + 1]);
        const std::size_t len = (static_cast<unsigned char>(b[i + 2]) << 8) | static_cast<unsigned char>(b[i + 3]);
        if (m >= 0xC0 && m <= 0xC2) return static_cast<unsigned char>(b[i + 9]);
        i += 2 + len;
    }
    return 0;
}

struct Writer {
    Rebuilder& rb;
    std::map<int, int> fonts;  // standard font index -> object
    int unicodeFont = 0;
    std::map<std::string, int> gstates;  // key -> object

    int font(const Font& f) {
        const int idx = static_cast<int>(standardOf(f));
        auto it = fonts.find(idx);
        if (it != fonts.end()) return it->second;
        const int id = rb.add(std::string("<< /Type /Font /Subtype /Type1 /BaseFont /") + pdfdoc::baseFontName(standardOf(f)) +
                              " /Encoding /WinAnsiEncoding >>");
        fonts[idx] = id;
        return id;
    }

    int gstate(double alpha, bool multiply) {
        const std::string key = num(alpha) + (multiply ? "m" : "");
        auto it = gstates.find(key);
        if (it != gstates.end()) return it->second;
        const int id = rb.add("<< /Type /ExtGState /ca " + num(alpha) + " /CA " + num(alpha) + (multiply ? " /BM /Multiply" : "") + " >>");
        gstates[key] = id;
        return id;
    }

    int image(const ImageData& img) {
        const raster::Image& px = img.pixels;
        const int comps = img.jpeg.empty() ? 0 : jpegComponents(img.jpeg);
        if (comps == 1 || comps == 3) {
            return rb.addStream("/Type /XObject /Subtype /Image /Width " + std::to_string(px.width()) + " /Height " + std::to_string(px.height()) +
                                    " /ColorSpace /" + (comps == 1 ? "DeviceGray" : "DeviceRGB") + " /BitsPerComponent 8 /Filter /DCTDecode",
                                img.jpeg, false);
        }
        std::string rgbData, alpha;
        bool hasAlpha = false;
        rgbData.reserve(static_cast<std::size_t>(px.width()) * px.height() * 3);
        for (int y = 0; y < px.height(); ++y) {
            for (int x = 0; x < px.width(); ++x) {
                const std::uint8_t* p = px.pixel(x, y);
                rgbData.append(reinterpret_cast<const char*>(p), 3);
                alpha += static_cast<char>(p[3]);
                hasAlpha = hasAlpha || p[3] != 255;
            }
        }
        const std::string size = " /Width " + std::to_string(px.width()) + " /Height " + std::to_string(px.height()) + " /BitsPerComponent 8";
        std::string smask;
        if (hasAlpha) smask = " /SMask " + std::to_string(rb.addStream("/Type /XObject /Subtype /Image /ColorSpace /DeviceGray" + size, alpha)) + " 0 R";
        return rb.addStream("/Type /XObject /Subtype /Image /ColorSpace /DeviceRGB" + size + smask, rgbData);
    }
};

struct Ops {
    std::string s;
    std::vector<std::pair<std::string, int>> fonts, xobjects, gstates;
    void font(const std::string& name, int id) {
        for (const auto& f : fonts) {
            if (f.first == name) return;
        }
        fonts.emplace_back(name, id);
    }
    void gs(const std::string& name, int id) {
        for (const auto& g : gstates) {
            if (g.first == name) return;
        }
        gstates.emplace_back(name, id);
    }
};

// Text in the y-down page space: each line flipped back upright.
void textOps(Writer& w, Ops& o, const std::string& line, const Font& font, double size, double x, double baseline) {
    if (line.empty()) return;
    const int fid = w.font(font);
    const std::string fname = "WWF" + std::to_string(static_cast<int>(standardOf(font)));
    o.font(fname, fid);
    o.s += "BT 1 0 0 -1 " + num(x) + " " + num(baseline) + " Tm ";
    // runs: WinAnsi text in the standard font, the rest in the built-in Unicode font
    std::string run;
    bool uni = false;
    auto flush = [&] {
        if (run.empty()) return;
        if (uni) {
            o.font("WWFU", w.unicodeFont);
            o.s += "/WWFU " + num(size) + " Tf " + pdfdoc::glyphText(*pdfdoc::builtinSans(), run) + " Tj ";
        } else {
            o.s += "/" + fname + " " + num(size) + " Tf " + pdfdoc::text(run) + " Tj ";
        }
        run.clear();
    };
    for (unsigned cp : utf8Decode(line)) {
        const bool u = w.unicodeFont && pdfdoc::needsUnicode(cp);
        if (u != uni) flush();
        uni = u;
        run += utf8Encode(cp);
    }
    flush();
    o.s += "ET\n";
}

std::string rectPath(double x, double y, double w, double h) { return num(x) + " " + num(y) + " " + num(w) + " " + num(h) + " re "; }

std::string ellipsePath(double x, double y, double w, double h) {
    const double k = 0.5523, rx = w / 2, ry = h / 2, cx = x + rx, cy = y + ry;
    auto p = [](double a, double b) { return num(a) + " " + num(b) + " "; };
    return p(cx + rx, cy) + "m " + p(cx + rx, cy + ry * k) + p(cx + rx * k, cy + ry) + p(cx, cy + ry) + "c " + p(cx - rx * k, cy + ry) +
           p(cx - rx, cy + ry * k) + p(cx - rx, cy) + "c " + p(cx - rx, cy - ry * k) + p(cx - rx * k, cy - ry) + p(cx, cy - ry) + "c " +
           p(cx + rx * k, cy - ry) + p(cx + rx, cy - ry * k) + p(cx + rx, cy) + "c h ";
}

// The drawing of one mark, in base page points (y down).
void markOps(Writer& w, Ops& o, const Mark& m) {
    o.s += "q\n";
    if (m.opacity < 0.999 || m.kind == "highlight") {
        const bool hl = m.kind == "highlight";
        const double a = hl ? 0.4 * m.opacity : m.opacity;
        const int id = w.gstate(a, hl);
        const std::string name = "WWG" + std::to_string(id);
        o.gs(name, id);
        o.s += "/" + name + " gs\n";
    }
    if (m.kind == "text") {
        o.s += rgb(m.color) + " rg\n";
        const auto lines = markLines(m);
        for (std::size_t i = 0; i < lines.size(); ++i) textOps(w, o, lines[i], m.font, m.size, m.x, m.y + m.size * 0.8 + static_cast<double>(i) * lineHeight(m));
    } else if (m.kind == "rect" || m.kind == "whiteout" || m.kind == "redact" || m.kind == "ellipse") {
        const std::string path = m.kind == "ellipse" ? ellipsePath(m.x, m.y, m.w, m.h) : rectPath(m.x, m.y, m.w, m.h);
        const bool stroke = (m.kind == "rect" || m.kind == "ellipse") && m.width > 0;
        if (m.fill) o.s += rgb(*m.fill) + " rg ";
        if (stroke) o.s += rgb(m.color) + " RG " + num(m.width) + " w ";
        o.s += path + (m.fill && stroke ? "B" : m.fill ? "f" : stroke ? "S" : "n") + "\n";
    } else if (m.kind == "line" || m.kind == "arrow") {
        if (m.points.size() >= 4) {
            const double x1 = m.points[0], y1 = m.points[1], x2 = m.points[2], y2 = m.points[3];
            o.s += rgb(m.color) + " RG " + rgb(m.color) + " rg " + num(m.width) + " w 1 J " + num(x1) + " " + num(y1) + " m " + num(x2) + " " + num(y2) + " l S\n";
            const double len = std::hypot(x2 - x1, y2 - y1);
            if (m.kind == "arrow" && len > 0) {
                const double ux = (x2 - x1) / len, uy = (y2 - y1) / len, s = std::max(6.0, m.width * 4);
                o.s += num(x2) + " " + num(y2) + " m " + num(x2 - ux * s - uy * s * 0.5) + " " + num(y2 - uy * s + ux * s * 0.5) + " l " +
                       num(x2 - ux * s + uy * s * 0.5) + " " + num(y2 - uy * s - ux * s * 0.5) + " l f\n";
            }
        }
    } else if (m.kind == "ink" || (m.kind == "signature" && !m.strokes.empty())) {
        o.s += rgb(m.color) + " RG " + num(m.width) + " w 1 J 1 j\n";
        for (const auto& st : m.strokes) {
            for (std::size_t k = 0; k + 1 < st.size(); k += 2) o.s += num(st[k]) + " " + num(st[k + 1]) + (k == 0 ? " m " : " l ");
            if (st.size() == 2) o.s += num(st[0] + 0.01) + " " + num(st[1]) + " l ";
            o.s += "S\n";
        }
    } else if (m.kind == "highlight") {
        o.s += rgb(m.color) + " rg ";
        for (const auto& b : m.boxes) o.s += rectPath(b[0], b[1], b[2], b[3]);
        o.s += "f\n";
    } else if (m.kind == "underline" || m.kind == "strikeout") {
        o.s += rgb(m.color) + " RG ";
        for (const auto& b : m.boxes) {
            const double ly = b[1] + b[3] * (m.kind == "underline" ? 0.92 : 0.55);
            o.s += num(std::max(0.75, b[3] * 0.07)) + " w " + num(b[0]) + " " + num(ly) + " m " + num(b[0] + b[2]) + " " + num(ly) + " l S ";
        }
        o.s += "\n";
    } else if (m.kind == "image" || m.kind == "signature") {
        if (m.image && !m.image->pixels.empty()) {
            const int id = w.image(*m.image);
            const std::string name = "WWI" + std::to_string(id);
            o.xobjects.emplace_back(name, id);
            o.s += num(m.w) + " 0 0 " + num(-m.h) + " " + num(m.x) + " " + num(m.y + m.h) + " cm /" + name + " Do\n";
        } else if (!m.text.empty()) {
            o.s += rgb(m.color) + " rg\n";
            textOps(w, o, m.text, Font{Family::Serif, false, true}, m.size, m.x, m.y + m.h * 0.75);
        }
    } else if (m.kind == "ocr") {
        // the words read off a scan, as invisible text over the picture: it can be searched, selected and copied
        const Font font{Family::Sans, false, false};
        const int fid = w.font(font);
        const std::string fname = "WWF" + std::to_string(static_cast<int>(standardOf(font)));
        o.font(fname, fid);
        o.s += "BT 3 Tr\n";
        std::size_t start = 0;
        for (std::size_t i = 0; i < m.boxes.size(); ++i) {
            const std::size_t nl = m.text.find('\n', start);
            const std::string word = m.text.substr(start, nl == std::string::npos ? std::string::npos : nl - start);
            start = nl == std::string::npos ? m.text.size() : nl + 1;
            if (word.empty() || 2 * i + 1 >= m.points.size()) continue;
            const auto& b = m.boxes[i];
            const double size = std::max(1.0, m.points[2 * i + 1]), natural = textWidth(font, word, size);
            const double scale = natural > 0 ? std::clamp(100.0 * b[2] / natural, 10.0, 1000.0) : 100.0;
            o.s += "1 0 0 -1 " + num(b[0]) + " " + num(m.points[2 * i]) + " Tm /" + fname + " " + num(size) + " Tf " + num(scale) + " Tz " + pdfdoc::text(word) + " Tj\n";
        }
        o.s += "ET\n";
    } else if (m.kind == "note") {
        const double x = m.x, y = m.y, z = 18;
        o.s += rgb(m.color) + " rg 0.35 0.27 0 RG 0.75 w " + num(x) + " " + num(y) + " m " + num(x + z) + " " + num(y) + " l " + num(x + z) + " " +
               num(y + z * 0.75) + " l " + num(x + z * 0.45) + " " + num(y + z * 0.75) + " l " + num(x + z * 0.2) + " " + num(y + z) + " l " +
               num(x + z * 0.25) + " " + num(y + z * 0.75) + " l " + num(x) + " " + num(y + z * 0.75) + " l h B\n";
    }
    o.s += "Q\n";
}

// The mark's extent in base page points: x0, y0, x1, y1.
std::array<double, 4> markBounds(const Mark& m) {
    double x0 = 1e18, y0 = 1e18, x1 = -1e18, y1 = -1e18;
    auto add = [&](double x, double y) {
        x0 = std::min(x0, x);
        y0 = std::min(y0, y);
        x1 = std::max(x1, x);
        y1 = std::max(y1, y);
    };
    if (m.kind == "highlight" || m.kind == "underline" || m.kind == "strikeout") {
        for (const auto& b : m.boxes) {
            add(b[0], b[1]);
            add(b[0] + b[2], b[1] + b[3]);
        }
    } else if (m.kind == "ink") {
        for (const auto& st : m.strokes) {
            for (std::size_t k = 0; k + 1 < st.size(); k += 2) add(st[k], st[k + 1]);
        }
    } else if (m.kind == "line" || m.kind == "arrow") {
        for (std::size_t k = 0; k + 1 < m.points.size(); k += 2) add(m.points[k], m.points[k + 1]);
    } else if (m.kind == "note") {
        add(m.x, m.y);
        add(m.x + 18, m.y + 18);
    } else {
        add(m.x, m.y);
        add(m.x + m.w, m.y + m.h);
    }
    const double pad = (m.kind == "arrow" ? std::max(6.0, m.width * 4) : 0) + m.width + 1;
    return {x0 - pad, y0 - pad, x1 + pad, y1 + pad};
}

std::string userBox(const Matrix& baseToUser, double x0, double y0, double x1, double y1, double out[4]) {
    double xs[4], ys[4];
    baseToUser.apply(x0, y0, xs[0], ys[0]);
    baseToUser.apply(x1, y0, xs[1], ys[1]);
    baseToUser.apply(x1, y1, xs[2], ys[2]);
    baseToUser.apply(x0, y1, xs[3], ys[3]);
    out[0] = *std::min_element(xs, xs + 4);
    out[1] = *std::min_element(ys, ys + 4);
    out[2] = *std::max_element(xs, xs + 4);
    out[3] = *std::max_element(ys, ys + 4);
    return "[" + num(out[0]) + " " + num(out[1]) + " " + num(out[2]) + " " + num(out[3]) + "]";
}

std::string matrixText(const Matrix& m) {
    return num(m.a) + " " + num(m.b) + " " + num(m.c) + " " + num(m.d) + " " + num(m.e) + " " + num(m.f);
}

std::string resourcesOf(const Ops& o) {
    std::string r = "<<";
    auto group = [&](const char* key, const std::vector<std::pair<std::string, int>>& v) {
        if (v.empty()) return;
        r += std::string(" /") + key + " <<";
        for (const auto& [n, id] : v) r += " /" + n + " " + std::to_string(id) + " 0 R";
        r += " >>";
    };
    group("Font", o.fonts);
    group("XObject", o.xobjects);
    group("ExtGState", o.gstates);
    return r + " >>";
}

// One PDF annotation for a mark, with an appearance drawn by markOps.
int annotationFor(Writer& w, const Mark& m, const Matrix& baseToUser) {
    const auto b = markBounds(m);
    double ub[4];
    const std::string rect = userBox(baseToUser, b[0], b[1], b[2], b[3], ub);
    std::string dict = "<< /Type /Annot /F 4 /M " + pdfdoc::literal(pdfDate()) + " /NM " + pdfdoc::literal("wetwaresuite-" + std::to_string(m.id)) +
                       " /C [" + rgb(m.color) + "]";
    if (!m.author.empty()) dict += " /T " + pdfdoc::infoText(m.author);
    if (m.kind == "note") {
        double nb[4];
        userBox(baseToUser, m.x, m.y, m.x + 18, m.y + 18, nb);
        return w.rb.add(dict + " /Subtype /Text /Name /Comment /Open false /Rect [" + num(nb[0]) + " " + num(nb[1]) + " " + num(nb[2]) + " " +
                        num(nb[3]) + "] /Contents " + pdfdoc::infoText(m.text) + " >>");
    }
    Ops o;
    markOps(w, o, m);
    const int ap = w.rb.addStream("/Type /XObject /Subtype /Form /BBox " + rect + " /Resources " + resourcesOf(o),
                                  "q " + matrixText(baseToUser) + " cm\n" + o.s + "Q\n");
    dict += " /Rect " + rect + " /AP << /N " + std::to_string(ap) + " 0 R >>";
    if (!m.text.empty()) dict += " /Contents " + pdfdoc::infoText(m.text);
    if (m.opacity < 0.999) dict += " /CA " + num(m.opacity);
    const std::string border = " /BS << /W " + num(m.width) + " >>";
    auto pt = [&](double x, double y) {
        double ux, uy;
        baseToUser.apply(x, y, ux, uy);
        return num(ux) + " " + num(uy);
    };
    if (m.kind == "highlight" || m.kind == "underline" || m.kind == "strikeout") {
        dict += std::string(" /Subtype /") + (m.kind == "highlight" ? "Highlight" : m.kind == "underline" ? "Underline" : "StrikeOut") + " /QuadPoints [";
        for (const auto& q : m.boxes) {
            dict += pt(q[0], q[1]) + " " + pt(q[0] + q[2], q[1]) + " " + pt(q[0], q[1] + q[3]) + " " + pt(q[0] + q[2], q[1] + q[3]) + " ";
        }
        dict += "]";
    } else if (m.kind == "ink") {
        dict += " /Subtype /Ink" + border + " /InkList [";
        for (const auto& st : m.strokes) {
            dict += "[";
            for (std::size_t k = 0; k + 1 < st.size(); k += 2) dict += pt(st[k], st[k + 1]) + " ";
            dict += "]";
        }
        dict += "]";
    } else if (m.kind == "rect" || m.kind == "ellipse") {
        dict += std::string(" /Subtype /") + (m.kind == "rect" ? "Square" : "Circle") + border;
        if (m.fill) dict += " /IC [" + rgb(*m.fill) + "]";
    } else if (m.kind == "line" || m.kind == "arrow") {
        dict += " /Subtype /Line" + border + " /L [" + pt(m.points[0], m.points[1]) + " " + pt(m.points[2], m.points[3]) + "]";
        if (m.kind == "arrow") dict += " /LE [/None /ClosedArrow]";
    }
    return w.rb.add(dict + " >>");
}

// UTF-16 text string bytes (or plain bytes for ASCII), for /V.
Object textObject(const std::string& utf8) {
    Object s;
    s.kind = Object::String;
    bool ascii = true;
    for (char c : utf8) ascii = ascii && static_cast<unsigned char>(c) < 128;
    if (ascii) {
        s.str = utf8;
        return s;
    }
    s.str = "\xFE\xFF";
    for (unsigned cp : utf8Decode(utf8)) {
        auto unit = [&](unsigned u) {
            s.str += static_cast<char>((u >> 8) & 0xFF);
            s.str += static_cast<char>(u & 0xFF);
        };
        if (cp >= 0x10000) {
            cp -= 0x10000;
            unit(0xD800 + (cp >> 10));
            unit(0xDC00 + (cp & 0x3FF));
        } else {
            unit(cp);
        }
    }
    return s;
}

// A simple appearance for a text or choice field showing `value`.
int fieldAppearance(Writer& w, const PdfFile& f, const Object& widget, const FieldInfo& fi, const std::string& value) {
    const double* r = nullptr;
    double rect[4] = {0, 0, 0, 0};
    if (const Object* ro = widget.get("Rect")) {
        const Object& ra = f.resolve(*ro);
        for (std::size_t k = 0; k < 4 && k < ra.items.size(); ++k) rect[k] = f.resolve(ra.items[k]).number;
        r = rect;
    }
    const double wdt = r ? std::abs(rect[2] - rect[0]) : 100, hgt = r ? std::abs(rect[3] - rect[1]) : 20;
    double size = 0;
    std::string da;
    for (const Object* node = &widget; node && da.empty();) {
        if (const Object* d = node->get("DA")) da = f.resolve(*d).str;
        const Object* p = node->get("Parent");
        node = p ? &f.resolve(*p) : nullptr;
    }
    if (da.empty()) {
        if (const Object* af = f.catalog().get("AcroForm")) {
            if (const Object* d = f.resolve(*af).get("DA")) da = f.resolve(*d).str;
        }
    }
    const auto tf = da.find(" Tf");
    if (tf != std::string::npos) {
        auto start = da.rfind(' ', tf - 1);
        try {
            size = std::stod(da.substr(start == std::string::npos ? 0 : start + 1, tf - (start == std::string::npos ? 0 : start + 1)));
        } catch (const std::exception&) {
        }
    }
    const Font helv;
    std::vector<std::string> lines;
    std::stringstream ss(value);
    std::string line;
    while (std::getline(ss, line)) lines.push_back(line);
    if (lines.empty()) lines.emplace_back();
    if (!fi.multiline && lines.size() > 1) {
        std::string joined;
        for (const auto& l : lines) joined += (joined.empty() ? "" : " ") + l;
        lines = {joined};
    }
    if (size <= 0) {
        size = fi.multiline ? 10 : std::min(12.0, hgt * 0.7);
        if (!fi.multiline) {
            const double tw = textWidth(helv, lines[0], size);
            if (tw > wdt - 4 && tw > 0) size = std::max(4.0, size * (wdt - 4) / tw);
        }
    }
    const int fid = w.font(helv);
    std::string c = "/Tx BMC q 1 1 " + num(wdt - 2) + " " + num(hgt - 2) + " re W n BT /Helv " + num(size) + " Tf 0 g ";
    double y = fi.multiline ? hgt - 2 - size : (hgt - size * 0.72) / 2;
    c += "2 " + num(y) + " Td ";
    for (std::size_t i = 0; i < lines.size(); ++i) {
        if (i) c += "0 " + num(-size * 1.15) + " Td ";
        c += pdfdoc::text(lines[i]) + " Tj ";
    }
    c += "ET Q EMC";
    return w.rb.addStream("/Type /XObject /Subtype /Form /BBox [0 0 " + num(wdt) + " " + num(hgt) + "] /Resources << /Font << /Helv " +
                              std::to_string(fid) + " 0 R >> >>",
                          c);
}

void writeFields(Writer& w, const DocData& d) {
    if (d.values.empty() || d.files.empty() || !d.files[0]) return;
    const PdfFile& f = *d.files[0];
    std::map<int, Object> edits;
    auto edit = [&](int num) -> Object& {
        auto it = edits.find(num);
        if (it != edits.end()) return it->second;
        return edits[num] = f.object(num);
    };
    for (const auto& fi : readFields(f)) {
        auto v = d.values.find(fi.name);
        if (v == d.values.end() || !fi.fieldObject) continue;
        const bool button = fi.type == "checkbox" || fi.type == "radio";
        if (button) edit(fi.fieldObject).set("V", Object::makeName(v->second.empty() ? "Off" : v->second));
        else edit(fi.fieldObject).set("V", textObject(v->second));
        for (const auto& wd : fi.widgets) {
            if (!wd.object) continue;
            Object& widget = edit(wd.object);
            if (button) {
                std::string state = "Off";
                if (const Object* apd = widget.get("AP")) {
                    if (const Object* n = f.resolve(*apd).get("N")) {
                        if (f.resolve(*n).get(v->second)) state = v->second;
                    }
                }
                widget.set("AS", Object::makeName(state));
            } else {
                const int ap = fieldAppearance(w, f, widget, fi, v->second);
                Object apDict;
                apDict.kind = Object::Dict;
                apDict.set("N", Rebuilder::outputRef(ap));
                widget.set("AP", apDict);
            }
        }
    }
    for (auto& [n, o] : edits) w.rb.replace(f, n, std::move(o));
}

}  // namespace

std::string saveBytes(const DocData& d, bool flatten, const std::vector<int>* only) {
    const PdfFile* main = d.files.empty() ? nullptr : d.files[0].get();
    Rebuilder rb(main);
    Writer w{rb};
    // the built-in Unicode font, once, with every character the edits need
    std::set<unsigned> uni;
    for (const auto& p : d.pages) {
        for (const auto& m : p.marks) {
            if (m.kind != "text" && m.kind != "signature") continue;
            for (unsigned cp : utf8Decode(m.text)) {
                if (pdfdoc::needsUnicode(cp)) uni.insert(cp);
            }
        }
    }
    if (!uni.empty()) w.unicodeFont = pdfdoc::addUnicodeFont([&rb](std::string body) { return rb.add(std::move(body)); }, *pdfdoc::builtinSans(), uni, "WW");

    std::vector<int> order;
    if (only) order = *only;
    else
        for (std::size_t i = 0; i < d.pages.size(); ++i) order.push_back(static_cast<int>(i));
    std::vector<Rebuilder::Page> pages;
    for (int i : order) {
        const PageEntry& e = d.pages.at(static_cast<std::size_t>(i));
        Rebuilder::Page p;
        if (e.file >= 0) {
            p.file = d.files[static_cast<std::size_t>(e.file)].get();
            p.index = e.index;
        } else {
            p.width = e.width;
            p.height = e.height;
        }
        p.rotate = e.rotate;
        if (e.content) {
            p.replaceContent = true;
            p.content = *e.content;
        }
        p.dropAnnots = e.dropAnnots;
        const PageGeom g = geometry(d, i);
        const Matrix baseToUser = g.userToBase.inverse();
        Ops o;
        for (const auto& m : e.marks) {
            if (m.kind == "note" || (m.isAnnotation() && !flatten)) {
                p.annots.push_back(annotationFor(w, m, baseToUser));
            } else {
                markOps(w, o, m);
            }
        }
        if (!o.s.empty()) {
            p.overlay = "q " + matrixText(baseToUser) + " cm\n" + o.s + "Q\n";
            p.fonts = o.fonts;
            p.xobjects = o.xobjects;
            p.extGStates = o.gstates;
        }
        pages.push_back(std::move(p));
    }
    writeFields(w, d);
    rb.setPages(std::move(pages));
    std::map<std::string, std::string> info = d.info;
    info["Producer"] = "wetwaresuite";
    info["ModDate"] = pdfDate();
    return rb.finish(info);
}

}  // namespace wetwaresuite::pdf::tool
