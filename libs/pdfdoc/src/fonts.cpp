#include "wetwaresuite/pdfdoc/fonts.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <functional>
#include <map>
#include <set>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <system_error>

#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wsign-compare"
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
#pragma GCC diagnostic ignored "-Wunused-parameter"
#endif
#if defined(_MSC_VER)
#pragma warning(push, 0)
#endif
#define STBTT_STATIC
#define STB_TRUETYPE_IMPLEMENTATION
#include "stb_truetype.h"
#if defined(_MSC_VER)
#pragma warning(pop)
#endif
#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic pop
#endif

namespace wetwaresuite::pdfdoc {

extern const unsigned char kBuiltinSans[];
extern const unsigned int kBuiltinSansSize;

namespace {

#include "std_metrics.inc"

// Unicode of WinAnsi codes 128..159 (0 = unused).
constexpr unsigned kHigh[32] = {0x20AC, 0, 0x201A, 0x0192, 0x201E, 0x2026, 0x2020, 0x2021, 0x02C6, 0x2030, 0x0160,
                                0x2039, 0x0152, 0, 0x017D, 0, 0, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022,
                                0x2013, 0x2014, 0x02DC, 0x2122, 0x0161, 0x203A, 0x0153, 0, 0x017E, 0x0178};

std::string lower(std::string s) {
    for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

}  // namespace

// ---- encoding ---------------------------------------------------------------

std::vector<unsigned> utf8Decode(const std::string& text) {
    std::vector<unsigned> out;
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
        put(0xC0 | (cp >> 6));
        put(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
        put(0xE0 | (cp >> 12));
        put(0x80 | ((cp >> 6) & 0x3F));
        put(0x80 | (cp & 0x3F));
    } else {
        put(0xF0 | (cp >> 18));
        put(0x80 | ((cp >> 12) & 0x3F));
        put(0x80 | ((cp >> 6) & 0x3F));
        put(0x80 | (cp & 0x3F));
    }
    return s;
}

int winAnsi(unsigned cp) {
    if (cp >= 32 && cp < 127) return static_cast<int>(cp);
    if (cp == 0xA0) return 32;  // no-break space prints as a space
    if (cp >= 0xA1 && cp <= 0xFF) return static_cast<int>(cp);
    for (int i = 0; i < 32; ++i) {
        if (kHigh[i] && kHigh[i] == cp) return 128 + i;
    }
    return -1;
}

unsigned unicodeOf(int code) {
    if (code >= 128 && code < 160) return kHigh[code - 128];
    return code >= 32 && code <= 255 && code != 127 ? static_cast<unsigned>(code) : 0;
}

std::string toWinAnsi(const std::string& utf8) {
    std::string out;
    for (unsigned cp : utf8Decode(utf8)) {
        int c = winAnsi(cp);
        out += static_cast<char>(c < 0 ? '?' : c);
    }
    return out;
}

// ---- standard fonts -----------------------------------------------------------

std::optional<std::string> standardFamily(const std::string& name) {
    std::string n = lower(name);
    n.erase(std::remove_if(n.begin(), n.end(), [](char c) { return c == ' ' || c == '-' || c == '_'; }), n.end());
    if (n == "helvetica" || n == "sans" || n == "sansserif" || n == "arial" || n == "liberationsans" || n == "arimo") {
        return "helvetica";
    }
    if (n == "times" || n == "timesroman" || n == "serif" || n == "timesnewroman" || n == "roman" || n == "liberationserif" ||
        n == "tinos") {
        return "times";
    }
    if (n == "courier" || n == "mono" || n == "monospace" || n == "couriernew" || n == "liberationmono" || n == "cousine") {
        return "courier";
    }
    return std::nullopt;
}

Standard standardFont(const std::string& family, bool bold, bool italic) {
    const int style = (bold ? 1 : 0) + (italic ? 2 : 0);
    const int base = family == "times" ? 4 : family == "courier" ? 8 : 0;
    return static_cast<Standard>(base + style);  // regular, bold, italic/oblique, bold italic
}

const char* baseFontName(Standard f) {
    static const char* names[] = {"Helvetica", "Helvetica-Bold", "Helvetica-Oblique", "Helvetica-BoldOblique",
                                  "Times-Roman", "Times-Bold", "Times-Italic", "Times-BoldItalic",
                                  "Courier", "Courier-Bold", "Courier-Oblique", "Courier-BoldOblique"};
    return names[static_cast<int>(f)];
}

bool isBold(Standard f) { return (static_cast<int>(f) % 4) % 2 == 1; }
bool isItalic(Standard f) { return (static_cast<int>(f) % 4) >= 2; }

double standardWidth(Standard f, int code) {
    if (code < 32 || code > 255) return 0;
    const int i = code - 32;
    switch (f) {
        case Standard::Helvetica:
        case Standard::HelveticaOblique: return kHelvetica[i];
        case Standard::HelveticaBold:
        case Standard::HelveticaBoldOblique: return kHelveticaBold[i];
        case Standard::TimesRoman: return kTimesRoman[i];
        case Standard::TimesBold: return kTimesBold[i];
        case Standard::TimesItalic: return kTimesItalic[i];
        case Standard::TimesBoldItalic: return kTimesBoldItalic[i];
        default: return kHelvetica[i] == 0 ? 0 : 600;
    }
}

VMetrics standardMetrics(Standard f) {
    const int i = static_cast<int>(f);
    if (i < 4) return {718, -207, 718};
    if (i < 8) return {683, -217, f == Standard::TimesBold ? 676.0 : 662.0};
    return {629, -157, 562};
}

// ---- TrueType ---------------------------------------------------------------

struct TrueType::Impl {
    std::filesystem::path path;
    std::string bytes;
    stbtt_fontinfo info{};
    double scale = 1;  // font units -> 1/1000 em
    std::uint32_t table(const char* tag) const {
        auto* d = const_cast<unsigned char*>(reinterpret_cast<const unsigned char*>(bytes.data()));
        return stbtt__find_table(d, static_cast<stbtt_uint32>(info.fontstart), tag);
    }
    mutable std::map<unsigned, Contours> outlines;
};

TrueType::TrueType() : impl_(std::make_unique<Impl>()) {}
TrueType::~TrueType() = default;

std::shared_ptr<TrueType> TrueType::load(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) throw std::runtime_error("can't open font " + path.string());
    std::ostringstream ss;
    ss << in.rdbuf();
    auto font = fromBytes(ss.str(), path.string());
    font->impl_->path = path;
    return font;
}

std::shared_ptr<TrueType> TrueType::fromBytes(std::string bytes, const std::string& name) {
    std::shared_ptr<TrueType> font(new TrueType());
    auto& im = *font->impl_;
    im.path = name;
    im.bytes = std::move(bytes);
    const auto* data = reinterpret_cast<const unsigned char*>(im.bytes.data());
    const int offset = im.bytes.size() > 12 ? stbtt_GetFontOffsetForIndex(data, 0) : -1;
    if (offset < 0 || !stbtt_InitFont(&im.info, data, offset)) throw std::runtime_error(name + " is not a TrueType font");
    im.scale = 1000.0 * stbtt_ScaleForMappingEmToPixels(&im.info, 1.0f);
    return font;
}

const std::filesystem::path& TrueType::path() const { return impl_->path; }
const std::string& TrueType::bytes() const { return impl_->bytes; }

std::string TrueType::postscriptName() const {
    std::string name;
    int len = 0;
    if (const char* s = stbtt_GetFontNameString(&impl_->info, &len, STBTT_PLATFORM_ID_MICROSOFT, STBTT_MS_EID_UNICODE_BMP,
                                                STBTT_MS_LANG_ENGLISH, 6)) {
        for (int i = 1; i < len; i += 2) name += s[i];  // UTF-16BE, keep the low bytes
    } else if (const char* m = stbtt_GetFontNameString(&impl_->info, &len, STBTT_PLATFORM_ID_MAC, 0, 0, 6)) {
        name.assign(m, static_cast<std::size_t>(len));
    }
    std::string safe;
    for (char c : name) {
        if (std::isalnum(static_cast<unsigned char>(c)) || c == '-' || c == '_') safe += c;
    }
    return safe.empty() ? "EmbeddedFont" : safe;
}

bool TrueType::embeddable() const { return impl_->table("glyf") != 0; }

bool TrueType::hasGlyph(unsigned cp) const { return stbtt_FindGlyphIndex(&impl_->info, static_cast<int>(cp)) != 0; }

double TrueType::advance(unsigned cp) const {
    int adv = 0, lsb = 0;
    stbtt_GetCodepointHMetrics(&impl_->info, static_cast<int>(cp), &adv, &lsb);
    return adv * impl_->scale;
}

VMetrics TrueType::metrics() const {
    VMetrics m;
    int a = 0, d = 0, gap = 0;
    if (!stbtt_GetFontVMetricsOS2(&impl_->info, &a, &d, &gap)) stbtt_GetFontVMetrics(&impl_->info, &a, &d, &gap);
    m.ascent = a * impl_->scale;
    m.descent = d * impl_->scale;
    int x0 = 0, y0 = 0, x1 = 0, y1 = 0;
    if (stbtt_GetCodepointBox(&impl_->info, 'H', &x0, &y0, &x1, &y1)) m.capHeight = y1 * impl_->scale;
    else m.capHeight = m.ascent * 0.7;
    return m;
}

double TrueType::italicAngle() const {
    const std::uint32_t post = impl_->table("post");
    if (!post || post + 8 > impl_->bytes.size()) return 0;
    const auto* p = reinterpret_cast<const unsigned char*>(impl_->bytes.data()) + post + 4;
    const auto fixed = static_cast<std::int32_t>((std::uint32_t{p[0]} << 24) | (std::uint32_t{p[1]} << 16) | (std::uint32_t{p[2]} << 8) | p[3]);
    return fixed / 65536.0;
}

bool TrueType::fixedPitch() const {
    const std::uint32_t post = impl_->table("post");
    if (!post || post + 16 > impl_->bytes.size()) return false;
    const auto* p = reinterpret_cast<const unsigned char*>(impl_->bytes.data()) + post + 12;
    return (p[0] | p[1] | p[2] | p[3]) != 0;
}

void TrueType::bbox(double out[4]) const {
    int x0 = 0, y0 = 0, x1 = 0, y1 = 0;
    stbtt_GetFontBoundingBox(&impl_->info, &x0, &y0, &x1, &y1);
    out[0] = x0 * impl_->scale;
    out[1] = y0 * impl_->scale;
    out[2] = x1 * impl_->scale;
    out[3] = y1 * impl_->scale;
}

const Contours& TrueType::outline(unsigned cp) const {
    auto it = impl_->outlines.find(cp);
    if (it != impl_->outlines.end()) return it->second;
    Contours& out = impl_->outlines[cp];
    stbtt_vertex* v = nullptr;
    const int n = stbtt_GetCodepointShape(&impl_->info, static_cast<int>(cp), &v);
    const double s = impl_->scale;
    Pt cur;
    for (int i = 0; i < n; ++i) {
        const Pt p{v[i].x * s, v[i].y * s};
        if (v[i].type == STBTT_vmove) {
            out.push_back({p});
        } else if (!out.empty()) {
            auto& c = out.back();
            if (v[i].type == STBTT_vline) {
                c.push_back(p);
            } else {
                const Pt c1{v[i].cx * s, v[i].cy * s};
                const Pt c2{v[i].cx1 * s, v[i].cy1 * s};
                const bool cubic = v[i].type == STBTT_vcubic;
                const double len = std::hypot(c1.x - cur.x, c1.y - cur.y) + std::hypot(p.x - c1.x, p.y - c1.y);
                const int steps = std::clamp(static_cast<int>(len / 20), 3, 24);
                for (int k = 1; k <= steps; ++k) {
                    const double t = static_cast<double>(k) / steps, u = 1 - t;
                    if (cubic) {
                        c.push_back({u * u * u * cur.x + 3 * u * u * t * c1.x + 3 * u * t * t * c2.x + t * t * t * p.x,
                                     u * u * u * cur.y + 3 * u * u * t * c1.y + 3 * u * t * t * c2.y + t * t * t * p.y});
                    } else {
                        c.push_back({u * u * cur.x + 2 * u * t * c1.x + t * t * p.x, u * u * cur.y + 2 * u * t * c1.y + t * t * p.y});
                    }
                }
            }
        }
        cur = p;
    }
    if (v) stbtt_FreeShape(&impl_->info, v);
    out.erase(std::remove_if(out.begin(), out.end(), [](const auto& c) { return c.size() < 3; }), out.end());
    return out;
}

std::string TrueType::svgPath(unsigned cp, double x, double y, double size) const {
    stbtt_vertex* v = nullptr;
    const int n = stbtt_GetCodepointShape(&impl_->info, static_cast<int>(cp), &v);
    const double k = impl_->scale * size / 1000.0;
    std::string d;
    char buf[160];
    auto X = [&](double fx) { return x + fx * k; };
    auto Y = [&](double fy) { return y - fy * k; };
    bool open = false;
    for (int i = 0; i < n; ++i) {
        switch (v[i].type) {
            case STBTT_vmove:
                if (open) d += "Z";
                std::snprintf(buf, sizeof buf, "M%.2f %.2f", X(v[i].x), Y(v[i].y));
                open = true;
                break;
            case STBTT_vline: std::snprintf(buf, sizeof buf, "L%.2f %.2f", X(v[i].x), Y(v[i].y)); break;
            case STBTT_vcurve:
                std::snprintf(buf, sizeof buf, "Q%.2f %.2f %.2f %.2f", X(v[i].cx), Y(v[i].cy), X(v[i].x), Y(v[i].y));
                break;
            default:
                std::snprintf(buf, sizeof buf, "C%.2f %.2f %.2f %.2f %.2f %.2f", X(v[i].cx), Y(v[i].cy), X(v[i].cx1), Y(v[i].cy1),
                              X(v[i].x), Y(v[i].y));
                break;
        }
        d += buf;
    }
    if (open) d += "Z";
    if (v) stbtt_FreeShape(&impl_->info, v);
    return d;
}

double TrueType::kern(unsigned left, unsigned right) const {
    return stbtt_GetCodepointKernAdvance(&impl_->info, static_cast<int>(left), static_cast<int>(right)) * impl_->scale;
}

int TrueType::glyphIndex(unsigned cp) const { return stbtt_FindGlyphIndex(&impl_->info, static_cast<int>(cp)); }

int TrueType::glyphCount() const { return impl_->info.numGlyphs; }

double TrueType::glyphAdvance(int glyph) const {
    int adv = 0, lsb = 0;
    stbtt_GetGlyphHMetrics(&impl_->info, glyph, &adv, &lsb);
    return adv * impl_->scale;
}

namespace {

std::uint32_t be16(const std::string& b, std::size_t at) {
    return at + 2 <= b.size() ? (std::uint32_t{static_cast<unsigned char>(b[at])} << 8) | static_cast<unsigned char>(b[at + 1]) : 0;
}
std::uint32_t be32(const std::string& b, std::size_t at) { return (be16(b, at) << 16) | be16(b, at + 2); }
void put16(std::string& b, std::uint32_t v) {
    b += static_cast<char>((v >> 8) & 0xFF);
    b += static_cast<char>(v & 0xFF);
}
void put32(std::string& b, std::uint32_t v) {
    put16(b, v >> 16);
    put16(b, v & 0xFFFF);
}
std::uint32_t checksum(const std::string& t) {
    std::uint32_t sum = 0;
    for (std::size_t i = 0; i < t.size(); i += 4) {
        std::uint32_t v = 0;
        for (std::size_t k = 0; k < 4; ++k) v = (v << 8) | (i + k < t.size() ? static_cast<unsigned char>(t[i + k]) : 0u);
        sum += v;
    }
    return sum;
}

}  // namespace

std::string TrueType::subset(const std::set<int>& wanted) const { return subsetImpl(wanted, nullptr); }

std::string TrueType::subsetCompact(const std::set<int>& wanted, std::vector<int>& oldOf) const {
    oldOf.clear();
    return subsetImpl(wanted, &oldOf);
}

std::string TrueType::subsetImpl(const std::set<int>& wanted, std::vector<int>* compact) const {
    const std::string& b = impl_->bytes;
    const std::size_t start = static_cast<std::size_t>(impl_->info.fontstart);
    const int numGlyphs = impl_->info.numGlyphs;
    const std::size_t glyf = impl_->table("glyf"), loca = impl_->table("loca"), head = impl_->table("head");
    const std::size_t hhea = impl_->table("hhea"), hmtx = impl_->table("hmtx");
    if (!glyf || !loca || !head || numGlyphs <= 0 || (compact && (!hhea || !hmtx))) return b;
    const bool longLoca = be16(b, head + 50) != 0;
    auto offset = [&](int g) -> std::size_t {
        return longLoca ? be32(b, loca + static_cast<std::size_t>(g) * 4) : be16(b, loca + static_cast<std::size_t>(g) * 2) * 2u;
    };
    // walks a composite glyph's components: fn(offset of the component's glyph index)
    auto components = [&](std::size_t a, std::size_t e, const std::function<void(std::size_t)>& fn) {
        std::size_t p = a + 10;
        for (;;) {
            if (p + 4 > e) break;
            const std::uint32_t flags = be16(b, p);
            fn(p + 2);
            p += 4 + ((flags & 1) ? 4 : 2);
            if (flags & 8) p += 2;
            else if (flags & 0x40) p += 4;
            else if (flags & 0x80) p += 8;
            if (!(flags & 0x20)) break;
        }
    };

    // glyph 0, the wanted glyphs and every component of a composite glyph
    std::set<int> keep{0};
    std::vector<int> todo(wanted.begin(), wanted.end());
    while (!todo.empty()) {
        const int g = todo.back();
        todo.pop_back();
        if (g < 0 || g >= numGlyphs || !keep.insert(g).second) continue;
        const std::size_t a = glyf + offset(g), e = glyf + offset(g + 1);
        if (e <= a + 10 || e > b.size()) continue;
        if (static_cast<std::int16_t>(be16(b, a)) >= 0) continue;  // simple glyph
        components(a, e, [&](std::size_t at) { todo.push_back(static_cast<int>(be16(b, at))); });
    }
    // compact: the kept glyphs renumbered 0..n-1 (composites point at the new numbers)
    std::map<int, int> newOf;
    if (compact) {
        for (int g : keep) {
            newOf[g] = static_cast<int>(compact->size());
            compact->push_back(g);
        }
    }
    // a composite's component may come before it in numbering: the loop above already
    // added them all before any is copied, so one pass is enough
    std::string newGlyf, newLoca, newHmtx;
    const std::uint32_t oldMetrics = hhea ? be16(b, hhea + 34) : 0;
    auto copyGlyph = [&](int g) {
        put32(newLoca, static_cast<std::uint32_t>(newGlyf.size()));
        const std::size_t a = glyf + offset(g), e = glyf + offset(g + 1);
        if (e > a && e <= b.size()) {
            const std::size_t at = newGlyf.size();
            newGlyf.append(b, a, e - a);
            if (compact && e > a + 10 && static_cast<std::int16_t>(be16(b, a)) < 0) {
                components(a, e, [&](std::size_t pos) {
                    const int n = newOf.count(static_cast<int>(be16(b, pos))) ? newOf[static_cast<int>(be16(b, pos))] : 0;
                    newGlyf[at + (pos - a)] = static_cast<char>((n >> 8) & 0xFF);
                    newGlyf[at + (pos - a) + 1] = static_cast<char>(n & 0xFF);
                });
            }
        }
        while (newGlyf.size() % 4) newGlyf += '\0';
        if (compact && oldMetrics) {
            const std::uint32_t m = std::min<std::uint32_t>(static_cast<std::uint32_t>(g), oldMetrics - 1);
            put16(newHmtx, be16(b, hmtx + m * 4));  // advance
            const std::size_t lsbAt = static_cast<std::uint32_t>(g) < oldMetrics ? hmtx + m * 4 + 2 : hmtx + oldMetrics * 4 + (static_cast<std::size_t>(g) - oldMetrics) * 2;
            put16(newHmtx, be16(b, lsbAt));
        }
    };
    if (compact) {
        for (int g : *compact) copyGlyph(g);
    } else {
        for (int g = 0; g < numGlyphs; ++g) {
            if (keep.count(g)) {
                copyGlyph(g);
            } else {
                put32(newLoca, static_cast<std::uint32_t>(newGlyf.size()));
            }
        }
    }
    put32(newLoca, static_cast<std::uint32_t>(newGlyf.size()));
    const std::uint32_t count = compact ? static_cast<std::uint32_t>(compact->size()) : static_cast<std::uint32_t>(numGlyphs);

    // copy the tables a PDF viewer needs, with the new glyf/loca and long loca offsets
    // (compact subsets get an empty cmap: PDF picks glyphs by number, and the numbers changed)
    const char* tags[] = {"OS/2", "cmap", "cvt ", "fpgm", "glyf", "head", "hhea", "hmtx", "loca", "maxp", "prep"};
    std::vector<std::pair<std::string, std::string>> tables;
    for (const char* tag : tags) {
        std::string data;
        if (std::strcmp(tag, "glyf") == 0) {
            data = newGlyf;
        } else if (std::strcmp(tag, "loca") == 0) {
            data = newLoca;
        } else if (compact && std::strcmp(tag, "hmtx") == 0) {
            data = newHmtx;
        } else if (compact && std::strcmp(tag, "cmap") == 0) {
            // an empty Unicode cmap: the glyph numbers changed, and readers want the table there
            for (std::uint32_t v : {0u, 1u, 3u, 1u}) put16(data, v);
            put32(data, 12);
            for (std::uint32_t v : {4u, 24u, 0u, 2u, 2u, 0u, 0u, 0xFFFFu, 0u, 0xFFFFu, 1u, 0u}) put16(data, v);
        } else {
            const std::size_t numTables = be16(b, start + 4);
            for (std::size_t i = 0; i < numTables; ++i) {
                const std::size_t rec = start + 12 + i * 16;
                if (b.compare(rec, 4, tag) != 0) continue;
                const std::size_t off = be32(b, rec + 8), len = be32(b, rec + 12);
                if (off + len <= b.size()) data = b.substr(off, len);
            }
            if (data.empty()) continue;
            if (std::strcmp(tag, "head") == 0 && data.size() >= 54) {
                data[8] = data[9] = data[10] = data[11] = 0;  // checkSumAdjustment, filled in below
                data[50] = 0;
                data[51] = 1;  // long loca
            }
            if (compact && std::strcmp(tag, "hhea") == 0 && data.size() >= 36) {
                data[34] = static_cast<char>((count >> 8) & 0xFF);  // numberOfHMetrics: one per glyph now
                data[35] = static_cast<char>(count & 0xFF);
            }
            if (compact && std::strcmp(tag, "maxp") == 0 && data.size() >= 6) {
                data[4] = static_cast<char>((count >> 8) & 0xFF);
                data[5] = static_cast<char>(count & 0xFF);
            }
        }
        tables.emplace_back(tag, std::move(data));
    }
    const std::size_t n = tables.size();
    int log2 = 0;
    while ((std::size_t{2} << log2) <= n) ++log2;
    const std::uint32_t range = (1u << log2) * 16;
    std::string out;
    put32(out, 0x00010000);
    put16(out, static_cast<std::uint32_t>(n));
    put16(out, range);
    put16(out, static_cast<std::uint32_t>(log2));
    put16(out, static_cast<std::uint32_t>(n * 16 - range));
    std::size_t at = 12 + n * 16;
    std::size_t headAt = 0;
    std::string body;
    for (auto& [tag, data] : tables) {
        out += tag;
        put32(out, checksum(data));
        put32(out, static_cast<std::uint32_t>(at + body.size()));
        put32(out, static_cast<std::uint32_t>(data.size()));
        if (tag == "head") headAt = at + body.size();
        body += data;
        while (body.size() % 4) body += '\0';
    }
    out += body;
    if (headAt) {
        const std::uint32_t adjust = 0xB1B0AFBAu - checksum(out);
        for (int k = 0; k < 4; ++k) out[headAt + 8 + static_cast<std::size_t>(k)] = static_cast<char>((adjust >> (24 - 8 * k)) & 0xFF);
    }
    return out;
}

std::shared_ptr<TrueType> builtinSans() {
    static std::shared_ptr<TrueType> font = [] {
        return TrueType::fromBytes(std::string(reinterpret_cast<const char*>(kBuiltinSans), kBuiltinSansSize), "Roboto Medium (built in)");
    }();
    return font;
}

// ---- system fonts -------------------------------------------------------------

namespace {

// An environment variable, or empty (MSVC wants _dupenv_s over getenv).
std::string env(const char* name) {
#ifdef _MSC_VER
    char* value = nullptr;
    std::size_t len = 0;
    std::string out;
    if (_dupenv_s(&value, &len, name) == 0 && value) out = value;
    std::free(value);
    return out;
#else
    const char* v = std::getenv(name);
    return v ? v : "";
#endif
}

std::vector<std::filesystem::path> fontDirs() {
    std::vector<std::filesystem::path> dirs;
    if (const auto d = env("WETWARESUITE_FONT_DIR"); !d.empty()) dirs.emplace_back(d);
#ifdef _WIN32
    if (const auto w = env("WINDIR"); !w.empty()) dirs.push_back(std::filesystem::path(w) / "Fonts");
    else dirs.emplace_back("C:\\Windows\\Fonts");
    if (const auto l = env("LOCALAPPDATA"); !l.empty()) dirs.push_back(std::filesystem::path(l) / "Microsoft" / "Windows" / "Fonts");
#else
    const std::string homeDir = env("HOME");
    const char* home = homeDir.empty() ? nullptr : homeDir.c_str();
#ifdef __APPLE__
    dirs.emplace_back("/System/Library/Fonts/Supplemental");
    dirs.emplace_back("/System/Library/Fonts");
    dirs.emplace_back("/Library/Fonts");
    if (home) dirs.push_back(std::filesystem::path(home) / "Library" / "Fonts");
#else
    dirs.emplace_back("/usr/share/fonts");
    dirs.emplace_back("/usr/local/share/fonts");
    if (home) {
        dirs.push_back(std::filesystem::path(home) / ".fonts");
        dirs.push_back(std::filesystem::path(home) / ".local" / "share" / "fonts");
    }
#endif
#endif
    return dirs;
}

// Lowercase file name -> path of every font file in the font folders.
const std::map<std::string, std::filesystem::path>& fontFiles() {
    static const std::map<std::string, std::filesystem::path> files = [] {
        std::map<std::string, std::filesystem::path> out;
        for (const auto& dir : fontDirs()) {
            std::error_code ec;
            if (!std::filesystem::is_directory(dir, ec)) continue;
            std::filesystem::recursive_directory_iterator it(dir, std::filesystem::directory_options::skip_permission_denied, ec), end;
            for (; !ec && it != end; it.increment(ec)) {
                if (it.depth() > 4) {
                    it.disable_recursion_pending();
                    continue;
                }
                std::error_code fe;
                if (!it->is_regular_file(fe)) continue;
                const std::string name = lower(it->path().filename().string());
                if (name.size() > 4 && name.compare(name.size() - 4, 4, ".ttf") == 0) out.emplace(name, it->path());
            }
        }
        return out;
    }();
    return files;
}

// The same with .ttc collections too (fallback fonts are often collections).
const std::map<std::string, std::filesystem::path>& allFontFiles() {
    static const std::map<std::string, std::filesystem::path> files = [] {
        std::map<std::string, std::filesystem::path> out = fontFiles();
        for (const auto& dir : fontDirs()) {
            std::error_code ec;
            if (!std::filesystem::is_directory(dir, ec)) continue;
            std::filesystem::recursive_directory_iterator it(dir, std::filesystem::directory_options::skip_permission_denied, ec), end;
            for (; !ec && it != end; it.increment(ec)) {
                if (it.depth() > 4) {
                    it.disable_recursion_pending();
                    continue;
                }
                std::error_code fe;
                if (!it->is_regular_file(fe)) continue;
                const std::string name = lower(it->path().filename().string());
                if (name.size() > 4 && name.compare(name.size() - 4, 4, ".ttc") == 0) out.emplace(name, it->path());
            }
        }
        return out;
    }();
    return files;
}

std::vector<std::string> candidates(const std::string& family, int style) {
    // style: 0 regular, 1 bold, 2 italic, 3 bold italic
    static const char* sfx[4] = {"regular", "bold", "italic", "bolditalic"};
    std::vector<std::string> c;
    const std::string s = sfx[style];
    if (family == "helvetica") {
        c = {"liberationsans-" + s + ".ttf", "arimo-" + s + ".ttf"};
        static const char* win[4] = {"arial.ttf", "arialbd.ttf", "ariali.ttf", "arialbi.ttf"};
        static const char* mac[4] = {"arial.ttf", "arial bold.ttf", "arial italic.ttf", "arial bold italic.ttf"};
        static const char* fr[4] = {"freesans.ttf", "freesansbold.ttf", "freesansoblique.ttf", "freesansboldoblique.ttf"};
        static const char* dj[4] = {"dejavusans.ttf", "dejavusans-bold.ttf", "dejavusans-oblique.ttf", "dejavusans-boldoblique.ttf"};
        c.insert(c.end(), {win[style], mac[style], fr[style], dj[style]});
    } else if (family == "times") {
        c = {"liberationserif-" + s + ".ttf", "tinos-" + s + ".ttf"};
        static const char* win[4] = {"times.ttf", "timesbd.ttf", "timesi.ttf", "timesbi.ttf"};
        static const char* mac[4] = {"times new roman.ttf", "times new roman bold.ttf", "times new roman italic.ttf",
                                     "times new roman bold italic.ttf"};
        static const char* fr[4] = {"freeserif.ttf", "freeserifbold.ttf", "freeserifitalic.ttf", "freeserifbolditalic.ttf"};
        static const char* dj[4] = {"dejavuserif.ttf", "dejavuserif-bold.ttf", "dejavuserif-italic.ttf", "dejavuserif-bolditalic.ttf"};
        c.insert(c.end(), {win[style], mac[style], fr[style], dj[style]});
    } else {
        c = {"liberationmono-" + s + ".ttf", "cousine-" + s + ".ttf"};
        static const char* win[4] = {"cour.ttf", "courbd.ttf", "couri.ttf", "courbi.ttf"};
        static const char* mac[4] = {"courier new.ttf", "courier new bold.ttf", "courier new italic.ttf", "courier new bold italic.ttf"};
        static const char* fr[4] = {"freemono.ttf", "freemonobold.ttf", "freemonooblique.ttf", "freemonoboldoblique.ttf"};
        static const char* dj[4] = {"dejavusansmono.ttf", "dejavusansmono-bold.ttf", "dejavusansmono-oblique.ttf",
                                    "dejavusansmono-boldoblique.ttf"};
        c.insert(c.end(), {win[style], mac[style], fr[style], dj[style]});
    }
    return c;
}

// Fonts with wide coverage, best first. Names are lowercase file names.
const std::vector<std::string>& fallbackNames() {
    static const std::vector<std::string> names = {
        // Chinese, Japanese, Korean
        "msyh.ttc", "msyh.ttf", "msgothic.ttc", "malgun.ttf", "simsun.ttc", "meiryo.ttc", "yugothr.ttc", "msjh.ttc",
        "pingfang.ttc", "arial unicode.ttf", "arialuni.ttf", "applegothic.ttf", "applesdgothicneo.ttc",
        "notosanssc-regular.ttf", "notosansjp-regular.ttf", "notosanskr-regular.ttf", "notosanstc-regular.ttf",
        "notosanscjksc-regular.ttf", "wqy-zenhei.ttc", "wqy-microhei.ttc", "droidsansfallbackfull.ttf", "droidsansfallback.ttf",
        "ukai.ttc", "uming.ttc", "nanumgothic.ttf", "takaopgothic.ttf", "ipagp.ttf", "ipag.ttf",
        // other scripts
        "arial.ttf", "tahoma.ttf", "nirmala.ttf", "nirmalaui.ttf", "leelawui.ttf", "ebrima.ttf", "gadugi.ttf", "segoeui.ttf",
        "notosans-regular.ttf", "notosansarabic-regular.ttf", "notosanshebrew-regular.ttf", "notosansdevanagari-regular.ttf",
        "notosansthai-regular.ttf", "notosansbengali-regular.ttf", "notosanstamil-regular.ttf", "notosansarmenian-regular.ttf",
        "notosansgeorgian-regular.ttf", "notosansethiopic-regular.ttf", "dejavusans.ttf", "freesans.ttf", "freeserif.ttf",
        // symbols and emoji (outlines: printed in one colour)
        "seguiemj.ttf", "seguisym.ttf", "notoemoji-regular.ttf", "notoemoji.ttf", "symbola.ttf", "notosanssymbols2-regular.ttf",
        "notosanssymbols-regular.ttf", "notosansmath-regular.ttf", "apple symbols.ttf", "unifont.ttf",
    };
    return names;
}

struct Fallbacks {
    std::mutex mutex;
    std::vector<std::filesystem::path> todo;              // not loaded yet, in order
    std::vector<std::shared_ptr<TrueType>> fonts;         // [0] = built in
    std::map<unsigned, int> found;                        // codepoint -> index or -1
    bool started = false;

    void start() {
        if (started) return;
        started = true;
        fonts.push_back(builtinSans());
        std::set<std::string> seen;
        auto addPath = [&](const std::filesystem::path& p) {
            const std::string key = lower(p.generic_string());
            if (seen.insert(key).second) todo.push_back(p);
        };
        // the user's own picks first: a list split on ';' (or ':' outside Windows)
        {
            const std::string list = env("WETWARESUITE_FALLBACK_FONT");
#ifdef _WIN32
            const char sep = ';';
#else
            const char sep = list.find(';') != std::string::npos ? ';' : ':';
#endif
            std::stringstream ss(list);
            for (std::string item; std::getline(ss, item, sep);) {
                if (!item.empty()) addPath(item);
            }
        }
        if (const auto d = env("WETWARESUITE_FONT_DIR"); !d.empty()) {
            std::error_code ec;
            for (const auto& e : std::filesystem::directory_iterator(d, ec)) {
                const std::string n = lower(e.path().filename().string());
                if (n.size() > 4 && (n.compare(n.size() - 4, 4, ".ttf") == 0 || n.compare(n.size() - 4, 4, ".ttc") == 0)) addPath(e.path());
            }
        }
        const auto& files = allFontFiles();
        for (const auto& name : fallbackNames()) {
            auto f = files.find(name);
            if (f != files.end()) addPath(f->second);
        }
    }

    int indexFor(unsigned cp) {
        std::lock_guard<std::mutex> lock(mutex);
        start();
        if (auto it = found.find(cp); it != found.end()) return it->second;
        int at = -1;
        for (std::size_t i = 0; i < fonts.size() && at < 0; ++i) {
            if (fonts[i]->glyphIndex(cp) != 0) at = static_cast<int>(i);
        }
        // load more fonts only when the ones we have can't draw it
        while (at < 0 && !todo.empty()) {
            const auto path = todo.front();
            todo.erase(todo.begin());
            try {
                auto f = TrueType::load(path);
                if (!f->embeddable()) continue;
                fonts.push_back(f);
                if (f->glyphIndex(cp) != 0) at = static_cast<int>(fonts.size()) - 1;
            } catch (const std::exception&) {
            }
        }
        found[cp] = at;
        return at;
    }

    std::shared_ptr<TrueType> at(int i) {
        std::lock_guard<std::mutex> lock(mutex);
        start();
        return i >= 0 && i < static_cast<int>(fonts.size()) ? fonts[static_cast<std::size_t>(i)] : nullptr;
    }
};

Fallbacks& fallbacks() {
    static Fallbacks f;
    return f;
}

}  // namespace

int unicodeFontIndex(unsigned cp) { return fallbacks().indexFor(cp); }
std::shared_ptr<TrueType> unicodeFontAt(int index) { return fallbacks().at(index); }
std::shared_ptr<TrueType> unicodeFontFor(unsigned cp) { return unicodeFontAt(unicodeFontIndex(cp)); }

std::shared_ptr<TrueType> systemFont(const std::string& family, bool bold, bool italic) {
    static std::mutex mutex;
    static std::map<std::string, std::shared_ptr<TrueType>> cache;
    std::lock_guard<std::mutex> lock(mutex);
    const int style = (bold ? 1 : 0) + (italic ? 2 : 0);
    const std::string key = family + std::to_string(style);
    auto it = cache.find(key);
    if (it != cache.end()) return it->second;
    std::shared_ptr<TrueType> found;
    const auto& files = fontFiles();
    for (int s : {style, 0}) {
        for (const auto& name : candidates(family, s)) {
            auto f = files.find(name);
            if (f == files.end()) continue;
            try {
                found = TrueType::load(f->second);
                break;
            } catch (const std::exception&) {
            }
        }
        if (found) break;
    }
    cache[key] = found;
    return found;
}

}  // namespace wetwaresuite::pdfdoc
