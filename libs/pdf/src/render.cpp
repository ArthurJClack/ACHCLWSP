#include <algorithm>
#include <cmath>

#include "wetwaresuite/pdf/page.hpp"
#include "wetwaresuite/raster/ops.hpp"
#include "wetwaresuite/raster/paint.hpp"
#include "internal.hpp"

namespace wetwaresuite::pdf {

namespace {

void blendPixel(raster::Image& img, int x, int y, Color c, double a) {
    if (a <= 0 || !img.contains(x, y)) return;
    if (a > 1) a = 1;
    std::uint8_t* p = img.pixel(x, y);
    const double da = p[3] / 255.0;
    const double oa = a + da * (1 - a);
    if (oa <= 0) return;
    auto mix = [&](std::uint8_t d, std::uint8_t s) {
        return static_cast<std::uint8_t>(std::lround((s * a + d * da * (1 - a)) / oa));
    };
    p[0] = mix(p[0], c.r);
    p[1] = mix(p[1], c.g);
    p[2] = mix(p[2], c.b);
    p[3] = static_cast<std::uint8_t>(std::lround(oa * 255));
}

double sample(const detail::Glyph& g, double sx, double sy) {
    // bilinear sample of the 0..15 coverage grid, zero outside
    const int x0 = static_cast<int>(std::floor(sx)), y0 = static_cast<int>(std::floor(sy));
    const double fx = sx - x0, fy = sy - y0;
    auto at = [&](int x, int y) -> double {
        if (x < 0 || y < 0 || x >= g.width || y >= g.height) return 0;
        return g.coverage[static_cast<std::size_t>(y) * static_cast<std::size_t>(g.width) + static_cast<std::size_t>(x)] / 15.0;
    };
    return (at(x0, y0) * (1 - fx) + at(x0 + 1, y0) * fx) * (1 - fy) + (at(x0, y0 + 1) * (1 - fx) + at(x0 + 1, y0 + 1) * fx) * fy;
}

// A glyph from the built-in Unicode font, filled from its outline. Returns the advance in px.
double drawUnicodeGlyph(raster::Image& img, double x, double y, unsigned cp, double sizePx, double shear, Color color) {
    const pdfdoc::TrueType& font = detail::unicodeFont(cp);
    const double k = sizePx / 1000.0;
    // all contours in one point list, each joined back to a common start so the joins cancel out (even-odd)
    std::vector<raster::Point> pts;
    const auto& contours = font.outline(cp);
    if (!contours.empty()) {
        auto map = [&](const pdfdoc::Pt& p) {
            const double py = y - p.y * k;
            return raster::Point{x + p.x * k + shear * (y - py), py};
        };
        const raster::Point origin = map(contours[0][0]);
        for (const auto& c : contours) {
            pts.push_back(origin);
            for (const auto& p : c) pts.push_back(map(p));
            pts.push_back(map(c[0]));
        }
        pts.push_back(origin);
        raster::fillPolygon(img, pts, color);
    }
    return font.advance(cp) * k;
}

}  // namespace

double textWidthPx(const std::string& text, Font font, double sizePx) { return textWidth(font, text, sizePx); }

void drawText(raster::Image& img, double x, double y, const std::string& text, Font font, double sizePx, Color color) {
    if (sizePx <= 0.5 || color.a == 0) return;
    const int rf = (font.family == Family::Sans ? 0 : font.family == Family::Serif ? 2 : 4) + (font.bold ? 1 : 0);
    const double k = sizePx / detail::glyphEm();
    const int ss = std::clamp(static_cast<int>(std::ceil(1.0 / k)), 1, 4);  // supersampling when shrinking
    const double shear = font.italic ? 0.2 : 0.0;
    const double alpha = color.a / 255.0;
    double pen = x;
    for (unsigned cp : utf8Decode(text)) {
        if (cp == '\n' || cp == '\r') continue;
        if (detail::needsUnicode(cp)) {
            pen += drawUnicodeGlyph(img, pen, y, cp, sizePx, shear, color);
            continue;
        }
        const unsigned char b = toWinAnsi(cp);
        const double adv = charWidth(font, b) / 1000.0 * sizePx;
        detail::Glyph g = detail::glyph(rf, b);
        if (g.coverage) {
            const double gx = pen + g.left * k, gy = y - g.top * k;
            const double gw = g.width * k, gh = g.height * k;
            const int px0 = static_cast<int>(std::floor(gx - shear * (gh - (y - gy)))) - 1;
            const int px1 = static_cast<int>(std::ceil(gx + gw + shear * (y - gy))) + 1;
            const int py0 = static_cast<int>(std::floor(gy)), py1 = static_cast<int>(std::ceil(gy + gh));
            for (int py = std::max(py0, 0); py < std::min(py1, img.height()); ++py) {
                for (int px = std::max(px0, 0); px < std::min(px1, img.width()); ++px) {
                    double cov = 0;
                    for (int sy = 0; sy < ss; ++sy) {
                        for (int sx = 0; sx < ss; ++sx) {
                            const double dx = px + (sx + 0.5) / ss, dy = py + (sy + 0.5) / ss;
                            const double ux = dx - shear * (y - dy);
                            cov += sample(g, (ux - gx) / k - 0.5, (dy - gy) / k - 0.5);
                        }
                    }
                    blendPixel(img, px, py, color, cov / (ss * ss) * alpha);
                }
            }
        }
        pen += adv;
    }
}

raster::Image renderPage(const Page& page, double s, Color background) {
    const int W = std::max(1, static_cast<int>(std::lround(page.width * s)));
    const int H = std::max(1, static_cast<int>(std::lround(page.height * s)));
    raster::Image img(W, H, background);
    for (const auto& op : page.ops) {
        if (auto t = std::get_if<TextOp>(&op)) {
            drawText(img, t->x * s, t->y * s, t->text, t->font, t->size * s, t->color);
            if (t->underline) {
                raster::fillRect(img, t->x * s, (t->y + t->size * 0.06) * s, textWidth(t->font, t->text, t->size) * s,
                                 std::max(1.0, t->size * 0.06 * s), t->color);
            }
        } else if (auto r = std::get_if<RectOp>(&op)) {
            if (r->fill && r->fill->a > 0) raster::fillRect(img, r->x * s, r->y * s, r->w * s, r->h * s, *r->fill);
            if (r->stroke && r->stroke->a > 0) {
                raster::strokeRect(img, r->x * s, r->y * s, r->w * s, r->h * s, *r->stroke, std::max(0.75, r->lineWidth * s));
            }
        } else if (auto l = std::get_if<LineOp>(&op)) {
            raster::drawLine(img, l->x1 * s, l->y1 * s, l->x2 * s, l->y2 * s, l->color, std::max(0.75, l->width * s));
        } else if (auto pa = std::get_if<PathOp>(&op)) {
            if (pa->points.size() < 2) continue;
            if (pa->closed && pa->fill && pa->fill->a > 0) {
                std::vector<raster::Point> pts;
                for (const auto& [x, y] : pa->points) pts.push_back({x * s, y * s});
                raster::fillPolygon(img, pts, *pa->fill);
            }
            if (pa->stroke && pa->stroke->a > 0) {
                const double w = std::max(0.75, pa->lineWidth * s);
                const std::size_t n = pa->points.size();
                for (std::size_t i = 0; i + (pa->closed ? 0 : 1) < n; ++i) {
                    const auto& a = pa->points[i];
                    const auto& b = pa->points[(i + 1) % n];
                    raster::drawLine(img, a.first * s, a.second * s, b.first * s, b.second * s, *pa->stroke, w);
                }
            }
        } else if (auto im = std::get_if<ImageOp>(&op)) {
            if (!im->image || im->image->pixels.empty()) continue;
            const int w = std::max(1, static_cast<int>(std::lround(im->w * s)));
            const int h = std::max(1, static_cast<int>(std::lround(im->h * s)));
            raster::Image scaled = raster::resize(im->image->pixels, w, h);
            raster::blendOnto(img, scaled, static_cast<int>(std::lround(im->x * s)), static_cast<int>(std::lround(im->y * s)),
                              raster::Blend::Normal, 1.0);
        }
    }
    return img;
}

}  // namespace wetwaresuite::pdf
