// Drawing: the schematic as a pdf::Page (PDF, PNG and SVG from one display list),
// the board as PNG or SVG, and a small stroke font for silkscreen text.
#include <functional>
#include <cctype>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <map>
#include <set>
#include <sstream>

#include "wetwaresuite/circuit/circuit.hpp"
#include "wetwaresuite/raster/paint.hpp"
#include "geom.hpp"

namespace wetwaresuite::circuit {

using raster::Color;

namespace {

constexpr double kPi = 3.14159265358979323846;

// ---- stroke font -----------------------------------------------------------------------
// Glyphs on a 4 x 6 grid, y down from the cap line; strokes are runs of "xy" digit pairs.
const char* glyph(char ch) {
    switch (ch) {
        case '0': return "0040460600 0640";
        case '1': return "102026 1636";
        case '2': return "004043030646";
        case '3': return "00404606 0343";
        case '4': return "000343 4046";
        case '5': return "400003434606";
        case '6': return "400006464303";
        case '7': return "004026";
        case '8': return "0040460600 0343";
        case '9': return "430300404606";
        case 'A': return "060110304146 0343";
        case 'B': return "06003041423344453606 0333";
        case 'C': return "40000646";
        case 'D': return "00304145360600";
        case 'E': return "40000646 0333";
        case 'F': return "400006 0333";
        case 'G': return "400006464323";
        case 'H': return "0006 4046 0343";
        case 'I': return "0040 2026 0646";
        case 'J': return "4045361605";
        case 'K': return "0006 400346";
        case 'L': return "000646";
        case 'M': return "0600234046";
        case 'N': return "06004640";
        case 'O': return "0040460600";
        case 'P': return "0600404303";
        case 'Q': return "0040460600 2546";
        case 'R': return "060040430346";
        case 'S': return "413010010213334445361605";
        case 'T': return "0040 2026";
        case 'U': return "00064640";
        case 'V': return "002640";
        case 'W': return "0006234640";
        case 'X': return "0046 4006";
        case 'Y': return "002340 2326";
        case 'Z': return "00400646";
        case '-': return "0343";
        case '+': return "0343 2125";
        case '.': return "2526";
        case '/': return "0640";
        case '_': return "0646";
        case ':': return "2122 2425";
        case '(': return "30121436";
        case ')': return "10323416";
        case '#': return "1016 3036 0242 0444";
        case '=': return "0242 0444";
        case '*': return "1135 3115 0343";
        case '!': return "2024 2526";
        default: return "";
    }
}

Color rgb(unsigned v, std::uint8_t a = 255) {
    return Color{static_cast<std::uint8_t>(v >> 16), static_cast<std::uint8_t>((v >> 8) & 0xff), static_cast<std::uint8_t>(v & 0xff), a};
}

std::string hex6(Color c) {
    char b[8];
    std::snprintf(b, sizeof b, "#%02x%02x%02x", c.r, c.g, c.b);
    return b;
}

std::string num(double v) {
    char b[32];
    std::snprintf(b, sizeof b, "%.3f", v);
    std::string s = b;
    while (s.size() > 1 && s.back() == '0') s.pop_back();
    if (s.back() == '.') s.pop_back();
    if (s == "-0") s = "0";
    return s;
}

std::string xmlEscape(const std::string& s) {
    std::string o;
    for (char c : s) {
        if (c == '&') o += "&amp;";
        else if (c == '<') o += "&lt;";
        else if (c == '>') o += "&gt;";
        else if (c == '"') o += "&quot;";
        else o += c;
    }
    return o;
}

// ---- schematic -------------------------------------------------------------------------

constexpr double kUnit = 7.2;  // points per grid unit (0.1 in)

const Color kWire = rgb(0x008400);
const Color kBus = rgb(0x0000c8);
const Color kBody = rgb(0x840000);
const Color kFillBody = rgb(0xffffc2);
const Color kText = rgb(0x006464);
const Color kLabel = rgb(0x202020);
const Color kNc = rgb(0x0000c8);
const Color kHighlight = rgb(0xff7a00);

struct SheetPainter {
    pdf::Page& page;
    double ox, oy;  // grid origin offset (in grid units) so the content starts at the margin

    double X(double x) const { return (x - ox) * kUnit; }
    double Y(double y) const { return (y - oy) * kUnit; }
    void line(Pt a, Pt b, Color c, double w) { page.line(X(a.x), Y(a.y), X(b.x), Y(b.y), c, w * kUnit); }
    void circle(Pt c, double r, Color col, double w, bool fill) {
        const int n = std::clamp(static_cast<int>(r * 40), 12, 48);
        if (fill) {
            for (double rr = r; rr > 0.01; rr -= std::max(0.04, w * 0.8)) {
                for (int i = 0; i < n; ++i) {
                    const double a0 = 2 * kPi * i / n, a1 = 2 * kPi * (i + 1) / n;
                    line({c.x + rr * std::cos(a0), c.y + rr * std::sin(a0)}, {c.x + rr * std::cos(a1), c.y + rr * std::sin(a1)}, col, w);
                }
            }
            line(c, c, col, std::max(w, 0.02));
            return;
        }
        for (int i = 0; i < n; ++i) {
            const double a0 = 2 * kPi * i / n, a1 = 2 * kPi * (i + 1) / n;
            line({c.x + r * std::cos(a0), c.y + r * std::sin(a0)}, {c.x + r * std::cos(a1), c.y + r * std::sin(a1)}, col, w);
        }
    }
    // align: -1 left (x is the start), 0 centre, 1 right (x is the end); y is the baseline
    void text(double x, double y, const std::string& s, double size, Color c, int align = 0, bool bold = false) {
        if (s.empty()) return;
        pdf::Font f;
        f.bold = bold;
        const double pts = size * kUnit;
        const double w = pdf::textWidth(f, s, pts);
        double px = X(x);
        if (align == 0) px -= w / 2;
        else if (align > 0) px -= w;
        page.text(px, Y(y), s, f, pts, c);
    }
};

void drawSymbol(SheetPainter& sp, const Part& part, const Symbol& sym, const SheetStyle& style, const Netlist& nl) {
    auto T = [&](Pt p) { return toSheet(part, p); };
    const double w = 0.11;
    for (const auto& sh : sym.shapes) {
        switch (sh.kind) {
            case Shape::Poly: {
                for (std::size_t i = 1; i < sh.pts.size(); ++i) sp.line(T(sh.pts[i - 1]), T(sh.pts[i]), kBody, w);
                if (sh.closed && sh.pts.size() > 2) sp.line(T(sh.pts.back()), T(sh.pts.front()), kBody, w);
                if (sh.fill && sh.pts.size() >= 3) {
                    // fan of lines from the first point: enough to read as filled at schematic sizes
                    const Pt a = T(sh.pts[0]);
                    for (std::size_t i = 1; i + 1 < sh.pts.size(); ++i) {
                        const Pt b = T(sh.pts[i]), c = T(sh.pts[i + 1]);
                        for (int k = 0; k <= 6; ++k) {
                            const double t = k / 6.0;
                            sp.line(a, {b.x + (c.x - b.x) * t, b.y + (c.y - b.y) * t}, kBody, w);
                        }
                    }
                }
                break;
            }
            case Shape::Circle: sp.circle(T(sh.pts[0]), sh.r, kBody, w, sh.fill); break;
            case Shape::Rect: {
                const Pt a = T(sh.pts[0]), b = T(sh.pts[1]);
                const double x0 = std::min(a.x, b.x), x1 = std::max(a.x, b.x), y0 = std::min(a.y, b.y), y1 = std::max(a.y, b.y);
                sp.page.rect(sp.X(x0), sp.Y(y0), (x1 - x0) * kUnit, (y1 - y0) * kUnit, kFillBody, kBody, w * kUnit * 1.2);
                break;
            }
            case Shape::Text: {
                const Pt a = T(sh.pts[0]);
                sp.text(a.x, a.y, sh.text, sh.size, kBody, 0, true);
                break;
            }
        }
    }
    for (const auto& pin : sym.pins) {
        const Pt a = T(pin.pos), b = T(pin.end);
        const std::string net = nl.netOf(part.ref, pin.number);
        const bool hi = !style.highlightNet.empty() && net == style.highlightNet;
        sp.line(a, b, hi ? kHighlight : kBody, hi ? 0.2 : w);
        const double dx = b.x - a.x, dy = b.y - a.y;
        const bool horizontal = std::abs(dx) >= std::abs(dy);
        if (sym.showPinNumbers && style.pinNumbers && std::hypot(dx, dy) > 0.5) {
            const Pt m{(a.x + b.x) / 2, (a.y + b.y) / 2};
            if (horizontal) sp.text(m.x, m.y - 0.15, pin.number, 0.5, kBody, 0);
            else sp.text(m.x - 0.15, m.y + 0.2, pin.number, 0.5, kBody, 1);
        }
        if (sym.showPinNames) {
            std::string name = pin.name;
            if (name == pin.number) continue;
            if (horizontal) {
                if (dx > 0) sp.text(b.x + 0.3, b.y + 0.22, name, 0.55, kText, -1);
                else sp.text(b.x - 0.3, b.y + 0.22, name, 0.55, kText, 1);
            } else {
                if (dy > 0) sp.text(b.x, b.y + 0.75, name, 0.55, kText, 0);
                else sp.text(b.x, b.y - 0.3, name, 0.55, kText, 0);
            }
        }
    }
    // reference and value: horizontal text, pushed sideways when the part is turned
    auto placeLabel = [&](Pt local, const std::string& s, double size, Color c) {
        if (s.empty()) return;
        const Pt p = T(local);
        const double dx = p.x - part.pos.x, dy = p.y - part.pos.y;
        const int r = ((part.rot % 360) + 360) % 360;
        int align = 0;
        if ((r == 90 || r == 270) && std::abs(dx) > std::abs(dy) + 0.01) align = dx > 0 ? -1 : 1;
        sp.text(p.x, p.y + size * 0.35, s, size, c, align);
    };
    if (sym.power) {
        placeLabel(sym.valuePos, part.value, 0.65, kBody);
        return;
    }
    // units of one part: U1A, U1B ...
    const std::string unit = part.unit > 0 && part.unit <= 26 ? std::string(1, static_cast<char>('A' + part.unit - 1)) : "";
    placeLabel(sym.refPos, part.ref + unit, 0.7, kText);
    placeLabel(sym.valuePos, part.value, 0.65, kText);
}

std::pair<long long, long long> pkey(Pt p) { return {std::llround(p.x * 1000), std::llround(p.y * 1000)}; }

}  // namespace

std::vector<std::vector<Pt>> strokeText(const std::string& text, double x, double y, double h) {
    std::vector<std::vector<Pt>> out;
    const double u = h / 6;
    double pen = x;
    for (char raw : text) {
        char ch = static_cast<char>(std::toupper(static_cast<unsigned char>(raw)));
        const std::string g = glyph(ch);
        std::vector<Pt> cur;
        for (std::size_t i = 0; i <= g.size(); ++i) {
            if (i == g.size() || g[i] == ' ') {
                if (cur.size() >= 2) out.push_back(cur);
                cur.clear();
                continue;
            }
            if (i + 1 >= g.size()) break;
            const double gx = g[i] - '0', gy = g[i + 1] - '0';
            cur.push_back({pen + gx * u, y - h + gy * u});
            ++i;
        }
        pen += 6 * u;
    }
    return out;
}

double strokeTextWidth(const std::string& text, double h) { return text.empty() ? 0 : (static_cast<double>(text.size()) * 6 - 2) * h / 6; }

namespace {

void pageBounds(const Circuit& c, bool roomy, double& x0, double& y0, double& x1, double& y1) {
    sheetBounds(c, x0, y0, x1, y1);
    if (!roomy) return;
    x0 = std::min(x0, 0.0), y0 = std::min(y0, 0.0);
    x1 = std::max(x1, 60.0), y1 = std::max(y1, 40.0);
}

}  // namespace

pdf::Page schematicPage(const Circuit& c, const SheetStyle& style) {
    const Netlist nl = buildNetlist(c);
    double x0, y0, x1, y1;
    pageBounds(c, style.roomy, x0, y0, x1, y1);
    const double margin = 2;
    const double titleH = style.titleBlock ? 3 : 0;
    pdf::Page page;
    page.width = (x1 - x0 + 2 * margin) * kUnit;
    page.height = (y1 - y0 + 2 * margin + titleH) * kUnit;
    page.width = std::max(page.width, 30 * kUnit);
    SheetPainter sp{page, x0 - margin, y0 - margin};
    page.rect(0, 0, page.width, page.height, rgb(0xfffff8), std::nullopt);
    if (style.grid) {
        for (double gx = std::floor(sp.ox); gx <= sp.ox + page.width / kUnit; gx += 1) {
            const bool major = static_cast<long long>(std::llround(gx)) % 10 == 0;
            page.line(sp.X(gx), 0, sp.X(gx), page.height, major ? rgb(0xd8d8e8) : rgb(0xeeeef4), 0.4);
        }
        for (double gy = std::floor(sp.oy); gy <= sp.oy + page.height / kUnit; gy += 1) {
            const bool major = static_cast<long long>(std::llround(gy)) % 10 == 0;
            page.line(0, sp.Y(gy), page.width, sp.Y(gy), major ? rgb(0xd8d8e8) : rgb(0xeeeef4), 0.4);
        }
    }
    // which wires belong to the highlighted net
    std::set<std::pair<long long, long long>> hiPoints;
    if (!style.highlightNet.empty()) {
        for (const auto& p : c.parts) {
            auto sym = symbolOf(p);
            if (!sym) continue;
            for (const auto& pin : sym->pins) {
                if (nl.netOf(p.ref, pin.number) == style.highlightNet) hiPoints.insert(pkey(pinPos(p, pin)));
            }
        }
        for (const auto& l : c.labels) {
            if (l.name == style.highlightNet) hiPoints.insert(pkey(l.pos));
        }
        // grow along wires
        for (bool changed = true; changed;) {
            changed = false;
            for (const auto& w : c.wires) {
                bool touch = false;
                for (const auto& p : w.pts) touch = touch || hiPoints.count(pkey(p));
                if (!touch) continue;
                for (const auto& p : w.pts) changed = hiPoints.insert(pkey(p)).second || changed;
            }
        }
    }
    // buses: thick blue lines, their entries, the bus name
    for (const auto& b : c.buses) {
        for (std::size_t i = 1; i < b.pts.size(); ++i) sp.line(b.pts[i - 1], b.pts[i], kBus, 0.36);
        for (const auto& [a, e] : b.entries) sp.line(a, e, kBus, 0.14);
        if (b.pts.size() >= 2) {
            const Pt a = b.pts[0], n = b.pts[1];
            const bool horiz = std::abs(n.x - a.x) >= std::abs(n.y - a.y);
            sp.text(a.x + (horiz ? 0.2 : 0.4), a.y - (horiz ? 0.4 : 0.2), b.name, 0.8, kBus, -1, true);
        }
    }
    // wires and junctions
    std::map<std::pair<long long, long long>, int> ends;
    for (const auto& w : c.wires) {
        bool hi = false;
        for (const auto& p : w.pts) hi = hi || hiPoints.count(pkey(p));
        for (std::size_t i = 1; i < w.pts.size(); ++i) sp.line(w.pts[i - 1], w.pts[i], hi ? kHighlight : kWire, hi ? 0.2 : 0.12);
        if (!w.pts.empty()) {
            ++ends[pkey(w.pts.front())];
            ++ends[pkey(w.pts.back())];
            for (std::size_t i = 1; i + 1 < w.pts.size(); ++i) ends[pkey(w.pts[i])] += 2;
        }
    }
    for (const auto& p : c.parts) {
        auto sym = symbolOf(p);
        if (!sym) continue;
        for (const auto& pin : sym->pins) {
            auto k = pkey(pinPos(p, pin));
            if (ends.count(k)) ++ends[k];
        }
    }
    for (const auto& w : c.wires) {
        for (std::size_t i = 1; i < w.pts.size(); ++i) {
            for (const auto& [k, n] : ends) {
                const Pt q{static_cast<double>(k.first) / 1000.0, static_cast<double>(k.second) / 1000.0};
                if (geom::pointSeg(q, w.pts[i - 1], w.pts[i]) < 1e-6 && !(std::abs(q.x - w.pts[i - 1].x) < 1e-6 && std::abs(q.y - w.pts[i - 1].y) < 1e-6) &&
                    !(std::abs(q.x - w.pts[i].x) < 1e-6 && std::abs(q.y - w.pts[i].y) < 1e-6)) {
                    ends[k] += 2;
                }
            }
        }
    }
    for (const auto& [k, n] : ends) {
        if (n >= 3) sp.circle({static_cast<double>(k.first) / 1000.0, static_cast<double>(k.second) / 1000.0}, 0.18, hiPoints.count(k) ? kHighlight : kWire, 0.08, true);
    }
    for (const auto& p : c.parts) {
        auto sym = symbolOf(p);
        if (sym) drawSymbol(sp, p, *sym, style, nl);
    }
    for (const auto& l : c.labels) {
        // put the text on the side away from the wire
        int align = -1;
        double dy = -0.25;
        for (const auto& w : c.wires) {
            if (w.pts.size() < 2) continue;
            Pt other{1e9, 1e9};
            if (std::abs(w.pts.front().x - l.pos.x) < 1e-6 && std::abs(w.pts.front().y - l.pos.y) < 1e-6) other = w.pts[1];
            else if (std::abs(w.pts.back().x - l.pos.x) < 1e-6 && std::abs(w.pts.back().y - l.pos.y) < 1e-6) other = w.pts[w.pts.size() - 2];
            if (other.x > 1e8) continue;
            if (std::abs(other.y - l.pos.y) < 1e-6) {
                align = other.x > l.pos.x ? 1 : -1;
            } else {
                align = 0;
                dy = other.y > l.pos.y ? -0.3 : 0.85;
            }
            break;
        }
        const bool hi = l.name == style.highlightNet;
        const double x = align == -1 ? l.pos.x + 0.15 : align == 1 ? l.pos.x - 0.15 : l.pos.x;
        sp.text(x, l.pos.y + dy, l.name, 0.65, hi ? kHighlight : kLabel, align, true);
        sp.circle(l.pos, 0.08, hi ? kHighlight : kWire, 0.06, true);
    }
    for (const auto& n : c.noConnects) {
        sp.line({n.x - 0.3, n.y - 0.3}, {n.x + 0.3, n.y + 0.3}, kNc, 0.1);
        sp.line({n.x - 0.3, n.y + 0.3}, {n.x + 0.3, n.y - 0.3}, kNc, 0.1);
    }
    if (style.titleBlock) {
        const double ty = page.height - titleH * kUnit + 0.6 * kUnit;
        page.line(0, ty - 0.4 * kUnit, page.width, ty - 0.4 * kUnit, rgb(0x9090a0), 0.5);
        pdf::Font bold;
        bold.bold = true;
        page.text(1 * kUnit, ty + 1.0 * kUnit, c.title.empty() ? "untitled circuit" : c.title, bold, 1.0 * kUnit, rgb(0x303040));
        int parts = 0;
        for (const auto& p : c.parts) {
            auto sym = symbolOf(p);
            if (sym && !sym->power) ++parts;
        }
        const std::string info = std::to_string(parts) + " parts, " + std::to_string(nl.nets.size()) + " nets  -  wetwaresuite circuit";
        pdf::Font f;
        page.text(page.width - pdf::textWidth(f, info, 0.6 * kUnit) - 1 * kUnit, ty + 1.0 * kUnit, info, f, 0.6 * kUnit, rgb(0x707080));
    }
    return page;
}

std::string pageSvg(const pdf::Page& page) {
    std::ostringstream o;
    o << "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"" << num(page.width) << "pt\" height=\"" << num(page.height) << "pt\" viewBox=\"0 0 "
      << num(page.width) << " " << num(page.height) << "\">\n";
    for (const auto& op : page.ops) {
        if (auto l = std::get_if<pdf::LineOp>(&op)) {
            o << "<line x1=\"" << num(l->x1) << "\" y1=\"" << num(l->y1) << "\" x2=\"" << num(l->x2) << "\" y2=\"" << num(l->y2) << "\" stroke=\""
              << hex6(l->color) << "\" stroke-width=\"" << num(l->width) << "\" stroke-linecap=\"round\"/>\n";
        } else if (auto r = std::get_if<pdf::RectOp>(&op)) {
            o << "<rect x=\"" << num(r->x) << "\" y=\"" << num(r->y) << "\" width=\"" << num(r->w) << "\" height=\"" << num(r->h) << "\" fill=\""
              << (r->fill ? hex6(*r->fill) : "none") << "\"";
            if (r->stroke) o << " stroke=\"" << hex6(*r->stroke) << "\" stroke-width=\"" << num(r->lineWidth) << "\"";
            o << "/>\n";
        } else if (auto t = std::get_if<pdf::TextOp>(&op)) {
            const char* family = t->font.family == pdf::Family::Mono ? "Courier, monospace" : t->font.family == pdf::Family::Serif ? "Times, serif" : "Helvetica, Arial, sans-serif";
            o << "<text x=\"" << num(t->x) << "\" y=\"" << num(t->y) << "\" font-family=\"" << family << "\" font-size=\"" << num(t->size) << "\" fill=\""
              << hex6(t->color) << "\"" << (t->font.bold ? " font-weight=\"bold\"" : "") << ">" << xmlEscape(t->text) << "</text>\n";
        }
    }
    o << "</svg>\n";
    return o.str();
}

// ---- PCB -------------------------------------------------------------------------------

namespace {

class Canvas {
public:
    virtual ~Canvas() = default;
    virtual void beginLayer(double opacity) = 0;
    virtual void endLayer() = 0;
    virtual void box(double x0, double y0, double x1, double y1, Color c) = 0;
    virtual void disc(Pt p, double r, Color c) = 0;
    virtual void capsule(Pt a, Pt b, double r, Color c) = 0;
    virtual void ring(Pt p, double r, double width, Color c) = 0;
    virtual void polygon(const std::vector<Pt>& pts, Color c) = 0;
    virtual void outline(const std::vector<Pt>& pts, double width, Color c) = 0;
    virtual void label(Pt p, const std::string& text, double h, Color c) = 0;
};

class RasterCanvas : public Canvas {
public:
    RasterCanvas(int w, int h, double scale, double ox, double oy, bool mirror)
        : img_(w, h, rgb(0x16181c)), s_(scale), ox_(ox), oy_(oy), mirror_(mirror), w_(w) {}
    raster::Image& image() { return img_; }

    void beginLayer(double opacity) override {
        layer_ = raster::Image(img_.width(), img_.height(), Color{0, 0, 0, 0});
        opacity_ = opacity;
        inLayer_ = true;
    }
    void endLayer() override {
        raster::blendOnto(img_, layer_, 0, 0, raster::Blend::Normal, opacity_);
        inLayer_ = false;
    }
    void box(double x0, double y0, double x1, double y1, Color c) override {
        double a = X(x0), b = X(x1);
        if (a > b) std::swap(a, b);
        cover(a, Y(y0), b, Y(y1), c, [&](double px, double py) {
            const double cx = std::min(px - a, b - px), cy = std::min(py - Y(y0), Y(y1) - py);
            return std::clamp(std::min(cx, cy) + 0.5, 0.0, 1.0);
        });
    }
    void disc(Pt p, double r, Color c) override { capsule(p, p, r, c); }
    void capsule(Pt a, Pt b, double r, Color c) override {
        const Pt pa{X(a.x), Y(a.y)}, pb{X(b.x), Y(b.y)};
        const double rp = std::max(0.5, r * s_);
        cover(std::min(pa.x, pb.x) - rp, std::min(pa.y, pb.y) - rp, std::max(pa.x, pb.x) + rp, std::max(pa.y, pb.y) + rp, c,
              [&](double px, double py) { return std::clamp(rp - geom::pointSeg({px, py}, pa, pb) + 0.5, 0.0, 1.0); });
    }
    void ring(Pt p, double r, double width, Color c) override {
        const Pt pc{X(p.x), Y(p.y)};
        const double rp = r * s_, hw = std::max(0.5, width * s_ / 2);
        cover(pc.x - rp - hw, pc.y - rp - hw, pc.x + rp + hw, pc.y + rp + hw, c,
              [&](double px, double py) { return std::clamp(hw - std::abs(std::hypot(px - pc.x, py - pc.y) - rp) + 0.5, 0.0, 1.0); });
    }
    void polygon(const std::vector<Pt>& pts, Color c) override {
        std::vector<raster::Point> q;
        for (const auto& p : pts) q.push_back({X(p.x), Y(p.y)});
        raster::fillPolygon(target(), q, c);
    }
    void outline(const std::vector<Pt>& pts, double width, Color c) override {
        for (std::size_t i = 0; i < pts.size(); ++i) capsule(pts[i], pts[(i + 1) % pts.size()], width / 2, c);
    }
    void label(Pt p, const std::string& text, double h, Color c) override {
        const double w = pdf::textWidthPx(text, pdf::Font{}, h * s_);
        pdf::drawText(target(), X(p.x) - w / 2, Y(p.y) + h * s_ * 0.35, text, pdf::Font{}, h * s_, c);
    }

private:
    raster::Image& target() { return inLayer_ ? layer_ : img_; }
    double X(double x) const {
        const double v = (x - ox_) * s_;
        return mirror_ ? w_ - v : v;
    }
    double Y(double y) const { return (y - oy_) * s_; }
    template <typename Fn>
    void cover(double x0, double y0, double x1, double y1, Color c, Fn coverage) {
        raster::Image& t = target();
        const int xa = std::max(0, static_cast<int>(std::floor(x0))), xb = std::min(t.width() - 1, static_cast<int>(std::ceil(x1)));
        const int ya = std::max(0, static_cast<int>(std::floor(y0))), yb = std::min(t.height() - 1, static_cast<int>(std::ceil(y1)));
        for (int y = ya; y <= yb; ++y) {
            for (int x = xa; x <= xb; ++x) {
                const double cov = coverage(x + 0.5, y + 0.5);
                if (cov <= 0) continue;
                std::uint8_t* px = t.pixel(x, y);
                const double a = cov * c.a / 255.0;
                if (inLayer_) {
                    // within a layer, overlapping copper doesn't darken: keep the strongest coverage
                    const std::uint8_t na = static_cast<std::uint8_t>(std::lround(a * 255));
                    if (na >= px[3]) {
                        px[0] = c.r, px[1] = c.g, px[2] = c.b, px[3] = na;
                    }
                } else {
                    px[0] = static_cast<std::uint8_t>(std::lround(px[0] * (1 - a) + c.r * a));
                    px[1] = static_cast<std::uint8_t>(std::lround(px[1] * (1 - a) + c.g * a));
                    px[2] = static_cast<std::uint8_t>(std::lround(px[2] * (1 - a) + c.b * a));
                    px[3] = 255;
                }
            }
        }
    }

    raster::Image img_, layer_;
    double s_, ox_, oy_;
    bool mirror_;
    int w_;
    double opacity_ = 1;
    bool inLayer_ = false;
};

class SvgCanvas : public Canvas {
public:
    SvgCanvas(double w, double h, double ox, double oy, bool mirror) {
        o_ << "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"" << num(w) << "mm\" height=\"" << num(h) << "mm\" viewBox=\"0 0 " << num(w) << " "
           << num(h) << "\">\n<rect width=\"100%\" height=\"100%\" fill=\"#16181c\"/>\n";
        o_ << "<g transform=\"" << (mirror ? "translate(" + num(w) + ",0) scale(-1,1) " : std::string()) << "translate(" << num(-ox) << ","
           << num(-oy) << ")\">\n";
    }
    std::string str() {
        o_ << "</g>\n</svg>\n";
        return o_.str();
    }
    void beginLayer(double opacity) override { o_ << "<g opacity=\"" << num(opacity) << "\">\n"; }
    void endLayer() override { o_ << "</g>\n"; }
    void box(double x0, double y0, double x1, double y1, Color c) override {
        o_ << "<rect x=\"" << num(x0) << "\" y=\"" << num(y0) << "\" width=\"" << num(x1 - x0) << "\" height=\"" << num(y1 - y0) << "\"" << fill(c) << "/>\n";
    }
    void disc(Pt p, double r, Color c) override { o_ << "<circle cx=\"" << num(p.x) << "\" cy=\"" << num(p.y) << "\" r=\"" << num(r) << "\"" << fill(c) << "/>\n"; }
    void capsule(Pt a, Pt b, double r, Color c) override {
        o_ << "<line x1=\"" << num(a.x) << "\" y1=\"" << num(a.y) << "\" x2=\"" << num(b.x) << "\" y2=\"" << num(b.y) << "\" stroke=\"" << hex6(c)
           << "\" stroke-width=\"" << num(2 * r) << "\" stroke-linecap=\"round\"" << (c.a < 255 ? " stroke-opacity=\"" + num(c.a / 255.0) + "\"" : "") << "/>\n";
    }
    void ring(Pt p, double r, double width, Color c) override {
        o_ << "<circle cx=\"" << num(p.x) << "\" cy=\"" << num(p.y) << "\" r=\"" << num(r) << "\" fill=\"none\" stroke=\"" << hex6(c) << "\" stroke-width=\""
           << num(width) << "\"/>\n";
    }
    void polygon(const std::vector<Pt>& pts, Color c) override {
        o_ << "<polygon points=\"";
        for (const auto& p : pts) o_ << num(p.x) << "," << num(p.y) << " ";
        o_ << "\"" << fill(c) << "/>\n";
    }
    void outline(const std::vector<Pt>& pts, double width, Color c) override {
        o_ << "<polygon points=\"";
        for (const auto& p : pts) o_ << num(p.x) << "," << num(p.y) << " ";
        o_ << "\" fill=\"none\" stroke=\"" << hex6(c) << "\" stroke-width=\"" << num(width) << "\"/>\n";
    }
    void label(Pt p, const std::string& text, double h, Color c) override {
        o_ << "<text x=\"" << num(p.x) << "\" y=\"" << num(p.y + h * 0.35) << "\" font-family=\"Helvetica, Arial, sans-serif\" font-size=\"" << num(h)
           << "\" text-anchor=\"middle\"" << fill(c) << ">" << xmlEscape(text) << "</text>\n";
    }
    void zone(const ZoneFill& f, Color c) {
        // one path of merged runs
        o_ << "<path" << fill(c) << " d=\"";
        for (int y = 0; y < f.ny; ++y) {
            int x = 0;
            while (x < f.nx) {
                if (!f.at(x, y)) {
                    ++x;
                    continue;
                }
                const int xa = x;
                while (x < f.nx && f.at(x, y)) ++x;
                o_ << "M" << num(f.x0 + xa * f.cell) << " " << num(f.y0 + y * f.cell) << "h" << num((x - xa) * f.cell) << "v" << num(f.cell) << "h"
                   << num(-(x - xa) * f.cell) << "z";
            }
        }
        o_ << "\"/>\n";
    }

private:
    static std::string fill(Color c) {
        std::string s = " fill=\"" + hex6(c) + "\"";
        if (c.a < 255) s += " fill-opacity=\"" + num(c.a / 255.0) + "\"";
        return s;
    }
    std::ostringstream o_;
};

void drawPad(Canvas& cv, const WorldPad& p, Color c) {
    if (p.angle != 0 && p.shape == PadShape::Rect) cv.polygon(padOutline(p), c);
    else if (p.shape == PadShape::Rect) cv.box(p.pos.x - p.w / 2, p.pos.y - p.h / 2, p.pos.x + p.w / 2, p.pos.y + p.h / 2, c);
    else {
        const auto s = geom::padShape(p);
        cv.capsule(s.a, s.b, s.r, c);
    }
}

void drawBoard(Canvas& cv, const Circuit& c, const Netlist& nl, const PcbStyle& style, const std::function<void(const ZoneFill&, Color)>& zoneFn) {
    const auto pads = worldPads(c, nl);
    const auto fills = style.zones ? fillZones(c, nl) : std::vector<ZoneFill>{};
    if (c.board.outline.size() >= 3) cv.polygon(c.board.outline, rgb(0x0f3d24));
    const Color layerCol[4] = {rgb(0xd03c3c), rgb(0x3f74d8), rgb(0xc8b040), rgb(0x40b8a0)};
    const Color zoneCol[4] = {rgb(0xc03838, 90), rgb(0x3a6cd0, 90), rgb(0xb8a038, 80), rgb(0x38a890, 80)};
    const Color padCol = rgb(0xd4aa3c);
    // the far side first: B, In2, In1, F (or the other way round from below)
    std::vector<int> order = {1};
    if (c.board.layers == 4) order.push_back(3), order.push_back(2);
    order.push_back(0);
    if (style.mirror) std::reverse(order.begin(), order.end());
    for (int layer : order) {
        if ((layer == 0 && !style.top) || (layer == 1 && !style.bottom) || (layer >= 2 && !style.inner)) continue;
        const int bit = layerBit(layer);
        const bool near = layer == (style.mirror ? 1 : 0);
        cv.beginLayer(near ? 0.9 : layer >= 2 ? 0.6 : 0.75);
        for (const auto& f : fills) {
            if (f.layer == layer) zoneFn(f, zoneCol[std::clamp(layer, 0, 3)]);
        }
        for (const auto& t : c.board.traces) {
            if (t.layer != layer) continue;
            const bool hi = !style.highlightNet.empty() && t.net == style.highlightNet;
            cv.capsule(t.a, t.b, t.width / 2, hi ? rgb(0xffe040) : layerCol[std::clamp(layer, 0, 3)]);
        }
        for (const auto& p : pads) {
            if (!(p.layers & bit) || !p.plated) continue;
            if (layer >= 2 && p.drill <= 0) continue;
            const bool hi = !style.highlightNet.empty() && p.net == style.highlightNet;
            drawPad(cv, p, hi ? rgb(0xffe040) : (p.drill > 0 ? padCol : (layer == 0 ? rgb(0xe0a050) : rgb(0x8ab0e8))));

        }
        cv.endLayer();
    }
    for (const auto& v : c.board.vias) {
        const bool hi = !style.highlightNet.empty() && v.net == style.highlightNet;
        cv.disc(v.pos, v.diameter / 2, hi ? rgb(0xffe040) : rgb(0xc8c8c8));
    }
    for (const auto& p : pads) {
        if (p.drill > 0) cv.disc(p.pos, p.drill / 2, rgb(0x101010));
    }
    for (const auto& v : c.board.vias) cv.disc(v.pos, v.drill / 2, rgb(0x101010));
    if (style.silk) {
        for (const auto& part : c.parts) {
            if (!part.pcb.placed) continue;
            const bool far = part.pcb.bottom != style.mirror;
            if ((part.pcb.bottom && !style.bottom) || (!part.pcb.bottom && !style.top)) continue;
            auto fp = footprintOf(part);
            if (!fp) continue;
            const Color sc = far ? rgb(0x9a9ac8) : rgb(0xf2f2f2);
            for (const auto& [a, b] : fp->silk) cv.capsule(toBoard(part.pcb, a), toBoard(part.pcb, b), 0.075, sc);
            const double h = 1.0, w = strokeTextWidth(part.ref, h);
            for (const auto& stroke : strokeText(part.ref, -w / 2, fp->y0 - 0.3, h)) {
                for (std::size_t i = 1; i < stroke.size(); ++i) cv.capsule(toBoard(part.pcb, stroke[i - 1]), toBoard(part.pcb, stroke[i]), 0.075, sc);
            }
        }
    }
    if (c.board.outline.size() >= 3) cv.outline(c.board.outline, 0.15, rgb(0xe6d800));
    if (style.labels) {
        for (const auto& p : pads) {
            if (!p.net.empty()) cv.label(p.pos, p.net, std::clamp(std::min(p.w, p.h) * 0.45, 0.35, 0.8), rgb(0xffffff));
        }
    }
    if (style.ratsnest) {
        for (const auto& a : ratsnest(c, nl)) cv.capsule(a.a, a.b, 0.05, rgb(0xf0f0b0, 220));
    }
}

}  // namespace

namespace {

// the area a board picture covers (board plus anything placed off it, with a margin) and its scale
void pcbArea(const Circuit& c, double& pxPerMm, double& ox, double& oy, double& W, double& H) {
    double x0, y0, x1, y1;
    boardBounds(c.board, x0, y0, x1, y1);
    for (const auto& p : c.parts) {
        if (!p.pcb.placed) continue;
        x0 = std::min(x0, p.pcb.pos.x - 2), y0 = std::min(y0, p.pcb.pos.y - 2);
        x1 = std::max(x1, p.pcb.pos.x + 2), y1 = std::max(y1, p.pcb.pos.y + 2);
    }
    if (x1 - x0 < 1) x0 = 0, y0 = 0, x1 = 50, y1 = 30;
    const double m = 2;
    W = (x1 - x0 + 2 * m), H = (y1 - y0 + 2 * m);
    if (W * pxPerMm > 6000 || H * pxPerMm > 6000) pxPerMm = 6000 / std::max(W, H);
    ox = x0 - m, oy = y0 - m;
}

}  // namespace

raster::Image renderPcb(const Circuit& c, const Netlist& nl, double pxPerMm, const PcbStyle& style) {
    double s = pxPerMm, ox, oy, W, H;
    pcbArea(c, s, ox, oy, W, H);
    RasterCanvas cv(std::max(1, static_cast<int>(std::ceil(W * s))), std::max(1, static_cast<int>(std::ceil(H * s))), s, ox, oy, style.mirror);
    drawBoard(cv, c, nl, style, [&](const ZoneFill& f, Color col) {
        for (int y = 0; y < f.ny; ++y) {
            int x = 0;
            while (x < f.nx) {
                if (!f.at(x, y)) {
                    ++x;
                    continue;
                }
                const int xa = x;
                while (x < f.nx && f.at(x, y)) ++x;
                const double e = 0.5 / s;  // overlap by half a pixel so rows don't leave seams
                cv.box(f.x0 + xa * f.cell - e, f.y0 + y * f.cell - e, f.x0 + x * f.cell + e, f.y0 + (y + 1) * f.cell + e, col);
            }
        }
    });
    return std::move(cv.image());
}

std::string pcbSvg(const Circuit& c, const Netlist& nl, const PcbStyle& style) {
    double x0, y0, x1, y1;
    boardBounds(c.board, x0, y0, x1, y1);
    if (x1 - x0 < 1) x0 = 0, y0 = 0, x1 = 50, y1 = 30;
    const double m = 2;
    SvgCanvas cv(x1 - x0 + 2 * m, y1 - y0 + 2 * m, x0 - m, y0 - m, style.mirror);
    drawBoard(cv, c, nl, style, [&](const ZoneFill& f, Color col) { cv.zone(f, col); });
    return cv.str();
}

namespace {

double schematicPreviewScale(const pdf::Page& page) { return std::clamp(1400.0 / std::max(page.width, page.height), 0.5, 2.5); }

double pcbPreviewScale(const Circuit& c) {
    const double w = std::max(10.0, boardWidth(c.board)), h = std::max(10.0, boardHeight(c.board));
    return std::clamp(1200.0 / std::max(w, h), 3.0, 40.0);
}

}  // namespace

ViewFrame viewFrame(const Circuit& c, const std::string& view) {
    ViewFrame f;
    f.view = view;
    if (view == "pcb") {
        double s = pcbPreviewScale(c), W, H;
        pcbArea(c, s, f.origin.x, f.origin.y, W, H);
        f.scale = s;
    } else if (view == "3d") {
        f.scale = 0;
    } else {
        f.view = "schematic";
        SheetStyle roomy;
        roomy.roomy = true;
        const pdf::Page page = schematicPage(c, roomy);
        double x0, y0, x1, y1;
        pageBounds(c, true, x0, y0, x1, y1);
        f.origin = {x0 - 2, y0 - 2};
        f.scale = schematicPreviewScale(page) * kUnit;
    }
    return f;
}

raster::Image viewImage(const Circuit& c, const std::string& view) {
    if (view == "pcb") return renderPcb(c, buildNetlist(c), pcbPreviewScale(c));
    if (view == "3d") return renderBoard3d(c, buildNetlist(c), 1200, 900);
    SheetStyle roomy;
    roomy.roomy = true;
    roomy.grid = true;  // the grid parts and wires snap to, as in KiCad
    const pdf::Page page = schematicPage(c, roomy);
    return pdf::renderPage(page, schematicPreviewScale(page));
}

}  // namespace wetwaresuite::circuit
