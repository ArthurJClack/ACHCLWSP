#include "wetwaresuite/pdf/fonts.hpp"

#include "internal.hpp"

namespace wetwaresuite::pdf {

namespace {

// Unicode code points of WinAnsi 0x80..0x9F (0 = unassigned).
constexpr unsigned kHigh[32] = {0x20AC, 0,      0x201A, 0x0192, 0x201E, 0x2026, 0x2020, 0x2021, 0x02C6, 0x2030, 0x0160,
                                0x2039, 0x0152, 0,      0x017D, 0,      0,      0x2018, 0x2019, 0x201C, 0x201D, 0x2022,
                                0x2013, 0x2014, 0x02DC, 0x2122, 0x0161, 0x203A, 0x0153, 0,      0x017E, 0x0178};

}  // namespace

int fontIndex(Font f) {
    int base = f.family == Family::Sans ? 0 : f.family == Family::Serif ? 4 : 8;
    return base + (f.bold ? 1 : 0) + (f.italic ? 2 : 0);
}

const char* baseFontName(Font f) {
    static const char* names[12] = {"Helvetica",   "Helvetica-Bold", "Helvetica-Oblique", "Helvetica-BoldOblique",
                                    "Times-Roman", "Times-Bold",     "Times-Italic",      "Times-BoldItalic",
                                    "Courier",     "Courier-Bold",   "Courier-Oblique",   "Courier-BoldOblique"};
    return names[fontIndex(f)];
}

std::vector<unsigned> utf8Decode(const std::string& text) {
    std::vector<unsigned> out;
    out.reserve(text.size());
    auto byte = [&](std::size_t j) { return static_cast<unsigned>(static_cast<unsigned char>(text[j])); };
    auto cont = [&](std::size_t j) { return j < text.size() && (byte(j) & 0xC0u) == 0x80u; };
    for (std::size_t i = 0; i < text.size(); ++i) {
        const unsigned ch = byte(i);
        if (ch < 0x80) {
            out.push_back(ch);
        } else if ((ch & 0xE0u) == 0xC0u && cont(i + 1)) {
            out.push_back(((ch & 0x1Fu) << 6) | (byte(i + 1) & 0x3Fu));
            i += 1;
        } else if ((ch & 0xF0u) == 0xE0u && cont(i + 1) && cont(i + 2)) {
            out.push_back(((ch & 0x0Fu) << 12) | ((byte(i + 1) & 0x3Fu) << 6) | (byte(i + 2) & 0x3Fu));
            i += 2;
        } else if ((ch & 0xF8u) == 0xF0u && cont(i + 1) && cont(i + 2) && cont(i + 3)) {
            out.push_back(((ch & 0x07u) << 18) | ((byte(i + 1) & 0x3Fu) << 12) | ((byte(i + 2) & 0x3Fu) << 6) | (byte(i + 3) & 0x3Fu));
            i += 3;
        } else {
            out.push_back(ch);
        }
    }
    return out;
}

std::string utf8Encode(unsigned cp) {
    std::string s;
    auto put = [&](unsigned v) { s += static_cast<char>(static_cast<unsigned char>(v)); };
    if (cp < 0x80) {
        put(cp);
    } else if (cp < 0x800) {
        put(0xC0u | (cp >> 6));
        put(0x80u | (cp & 0x3Fu));
    } else if (cp < 0x10000) {
        put(0xE0u | (cp >> 12));
        put(0x80u | ((cp >> 6) & 0x3Fu));
        put(0x80u | (cp & 0x3Fu));
    } else {
        put(0xF0u | (cp >> 18));
        put(0x80u | ((cp >> 12) & 0x3Fu));
        put(0x80u | ((cp >> 6) & 0x3Fu));
        put(0x80u | (cp & 0x3Fu));
    }
    return s;
}

unsigned char toWinAnsi(unsigned cp) {
    if (cp == '\t') return ' ';
    if ((cp >= 32 && cp < 0x80) || (cp >= 0xA0 && cp <= 0xFF)) return static_cast<unsigned char>(cp);
    for (unsigned i = 0; i < 32; ++i) {
        if (kHigh[i] && kHigh[i] == cp) return static_cast<unsigned char>(0x80 + i);
    }
    switch (cp) {  // close relatives that would otherwise be lost
        case 0x2010: case 0x2011: case 0x2012: case 0x2212: return '-';
        case 0x2015: return 0x97;
        case 0x00A0: case 0x2002: case 0x2003: case 0x2009: case 0x200A: case 0x202F: return ' ';
        case 0x2032: return '\'';
        case 0x2033: return '"';
        case 0x25CF: case 0x25E6: case 0x2043: case 0x25AA: case 0x25A0: return 0x95;
        default: return '?';
    }
}

unsigned fromWinAnsi(unsigned char b) {
    if (b >= 0x80 && b < 0xA0) return kHigh[b - 0x80] ? kHigh[b - 0x80] : b;
    return b;
}

std::string winAnsi(const std::string& utf8) {
    std::string out;
    for (unsigned cp : utf8Decode(utf8)) {
        if (cp == '\n' || cp == '\r') continue;
        out += static_cast<char>(toWinAnsi(cp));
    }
    return out;
}

double charWidth(Font font, unsigned char b) {
    if (font.family == Family::Mono) return 600;
    if (b < 32) return 0;
    return detail::stdWidth(fontIndex(font), b);
}

double textWidth(Font font, const std::string& utf8, double size) {
    double em = 0;
    for (unsigned cp : utf8Decode(utf8)) {
        if (cp == '\n' || cp == '\r') continue;
        em += detail::needsUnicode(cp) ? detail::unicodeFont(cp).advance(cp) : charWidth(font, toWinAnsi(cp));
    }
    return em / 1000.0 * size;
}

namespace detail {

const pdfdoc::TrueType& unicodeFont(unsigned cp) {
    auto f = pdfdoc::unicodeFontFor(cp);
    return f ? *f : *pdfdoc::builtinSans();
}

bool needsUnicode(unsigned cp) {
    if (cp < 0x100 || toWinAnsi(cp) != '?') return false;
    return pdfdoc::unicodeFontIndex(cp) >= 0;
}

std::vector<Run> splitRuns(const std::string& utf8) {
    std::vector<Run> runs;
    for (unsigned cp : utf8Decode(utf8)) {
        if (cp == '\n' || cp == '\r') continue;
        const bool u = needsUnicode(cp);
        const int f = u ? pdfdoc::unicodeFontIndex(cp) : -1;
        if (runs.empty() || runs.back().unicode != u || runs.back().font != f) runs.push_back({u, f, {}});
        runs.back().text += utf8Encode(cp);
    }
    return runs;
}

}  // namespace detail

}  // namespace wetwaresuite::pdf
