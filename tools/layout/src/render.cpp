// Pictures (loading, fitting, resolution) and the raster page renderer used
// for previews and PNG export. Everything is drawn as antialiased polygons in
// frame-local millimetres, then mapped to pixels, so rotation is free.
#include <algorithm>
#include <cmath>
#include <map>
#include <mutex>
#include <system_error>

#include "wetwaresuite/raster/codec.hpp"
#include "wetwaresuite/raster/ops.hpp"
#include "wetwaresuite/vector/vector.hpp"
#include "internal.hpp"

namespace wetwaresuite::layout {

using pdfdoc::Pt;
using Poly = std::vector<Pt>;
using Polys = std::vector<Poly>;

// ---- pictures ------------------------------------------------------------------

bool isVectorPath(const std::string& path) {
    return lower(std::filesystem::path(path).extension().string()) == ".svg";
}

namespace {

template <typename T>
std::shared_ptr<const T> cached(const std::string& path, std::string* error, const std::function<std::shared_ptr<const T>()>& load) {
    struct Entry {
        std::filesystem::file_time_type time;
        std::uintmax_t size = 0;
        std::shared_ptr<const T> value;
    };
    static std::mutex mutex;
    static std::map<std::string, Entry> cache;
    std::error_code ec;
    const auto time = std::filesystem::last_write_time(path, ec);
    const auto size = ec ? 0 : std::filesystem::file_size(path, ec);
    if (ec) {
        if (error) *error = "missing file " + path;
        return nullptr;
    }
    {
        std::lock_guard<std::mutex> lock(mutex);
        auto it = cache.find(path);
        if (it != cache.end() && it->second.time == time && it->second.size == size) return it->second.value;
    }
    std::shared_ptr<const T> value;
    try {
        value = load();
    } catch (const std::exception& e) {
        if (error) *error = e.what();
        return nullptr;
    }
    std::lock_guard<std::mutex> lock(mutex);
    if (cache.size() > 48) cache.clear();
    cache[path] = {time, size, value};
    return value;
}

}  // namespace

std::shared_ptr<const raster::Image> loadImage(const std::string& path, std::string* error) {
    return cached<raster::Image>(path, error, [&] { return std::make_shared<const raster::Image>(raster::load(path)); });
}

std::shared_ptr<const vector::VectorDoc> loadVector(const std::string& path, std::string* error) {
    return cached<vector::VectorDoc>(path, error, [&] { return std::shared_ptr<const vector::VectorDoc>(vector::openSvg(path)); });
}

PictureInfo pictureInfo(const std::string& path) {
    PictureInfo info;
    if (path.empty()) {
        info.error = "no picture placed";
        return info;
    }
    if (isVectorPath(path)) {
        auto v = loadVector(path, &info.error);
        if (!v) return info;
        info.vector = true;
        info.ok = true;
        info.naturalW = std::max(1, v->width) * 25.4 / 96;
        info.naturalH = std::max(1, v->height) * 25.4 / 96;
        return info;
    }
    auto img = loadImage(path, &info.error);
    if (!img) return info;
    info.ok = true;
    info.pxWidth = img->width();
    info.pxHeight = img->height();
    info.naturalW = img->width() / 300.0 * 25.4;
    info.naturalH = img->height() / 300.0 * 25.4;
    return info;
}

Box pictureBox(const Frame& f, const PictureInfo& info) {
    const double nw = std::max(1e-6, info.naturalW), nh = std::max(1e-6, info.naturalH);
    switch (f.fit) {
        case Fit::Stretch: return {0, 0, f.w, f.h};
        case Fit::Manual: return {f.imageX, f.imageY, nw * f.imageScale / 100, nh * f.imageScale / 100};
        case Fit::Fit:
        case Fit::Fill: {
            const double s = f.fit == Fit::Fit ? std::min(f.w / nw, f.h / nh) : std::max(f.w / nw, f.h / nh);
            return {(f.w - nw * s) / 2, (f.h - nh * s) / 2, nw * s, nh * s};
        }
    }
    return {0, 0, f.w, f.h};
}

double effectiveDpi(const Frame& f) {
    if (f.type != FrameType::Image) return 0;
    const PictureInfo info = pictureInfo(f.path);
    if (!info.ok || info.vector) return 0;
    const Box b = pictureBox(f, info);
    if (b.w <= 0 || b.h <= 0) return 0;
    return std::min(info.pxWidth / (b.w / 25.4), info.pxHeight / (b.h / 25.4));
}

// ---- polygons --------------------------------------------------------------------

namespace {

double signedArea(const Poly& p) {
    double a = 0;
    for (std::size_t i = 0; i < p.size(); ++i) {
        const Pt& u = p[i];
        const Pt& v = p[(i + 1) % p.size()];
        a += u.x * v.y - v.x * u.y;
    }
    return a / 2;
}

void orientPositive(Poly& p) {
    if (signedArea(p) < 0) std::reverse(p.begin(), p.end());
}

Poly ellipsePoly(double cx, double cy, double rx, double ry, int n = 0) {
    if (n <= 0) n = std::clamp(static_cast<int>(std::max(rx, ry) * 4), 24, 360);
    Poly p;
    for (int i = 0; i < n; ++i) {
        const double t = 2 * kPi * i / n;
        p.push_back({cx + rx * std::cos(t), cy + ry * std::sin(t)});
    }
    return p;
}

Poly rectPoly(double x, double y, double w, double h, double r) {
    r = std::clamp(r, 0.0, std::min(std::abs(w), std::abs(h)) / 2);
    if (r <= 0) return {{x, y}, {x + w, y}, {x + w, y + h}, {x, y + h}};
    Poly p;
    const int n = 10;
    auto corner = [&](double cx, double cy, double a0) {
        for (int i = 0; i <= n; ++i) {
            const double t = (a0 + 90.0 * i / n) * kPi / 180;
            p.push_back({cx + r * std::cos(t), cy + r * std::sin(t)});
        }
    };
    corner(x + w - r, y + r, -90);
    corner(x + w - r, y + h - r, 0);
    corner(x + r, y + h - r, 90);
    corner(x + r, y + r, 180);
    return p;
}

// The area a stroke of `width` covers along a polyline: one quad per segment plus round joins,
// all wound the same way so a nonzero fill merges them.
Polys strokePolys(const Poly& pts, bool closed, double width) {
    Polys out;
    if (pts.size() < 2 || width <= 0) return out;
    const double r = width / 2;
    const std::size_t n = pts.size();
    const std::size_t segs = closed ? n : n - 1;
    for (std::size_t i = 0; i < segs; ++i) {
        const Pt a = pts[i], b = pts[(i + 1) % n];
        const double dx = b.x - a.x, dy = b.y - a.y, len = std::hypot(dx, dy);
        if (len < 1e-9) continue;
        const double nx = -dy / len * r, ny = dx / len * r;
        Poly q{{a.x + nx, a.y + ny}, {b.x + nx, b.y + ny}, {b.x - nx, b.y - ny}, {a.x - nx, a.y - ny}};
        orientPositive(q);
        out.push_back(std::move(q));
    }
    const int joins = std::clamp(static_cast<int>(r * 6), 8, 32);
    for (std::size_t i = closed ? 0 : 1; i < (closed ? n : n - 1); ++i) {
        Poly c = ellipsePoly(pts[i].x, pts[i].y, r, r, joins);
        orientPositive(c);
        out.push_back(std::move(c));
    }
    return out;
}

// Sutherland-Hodgman against the rectangle [0, w] x [0, h].
Poly clipRect(const Poly& in, double w, double h) {
    Poly poly = in;
    for (int edge = 0; edge < 4 && !poly.empty(); ++edge) {
        auto inside = [&](const Pt& p) {
            switch (edge) {
                case 0: return p.x >= 0;
                case 1: return p.x <= w;
                case 2: return p.y >= 0;
                default: return p.y <= h;
            }
        };
        auto cross = [&](const Pt& a, const Pt& b) {
            double t = 0;
            switch (edge) {
                case 0: t = (0 - a.x) / (b.x - a.x); break;
                case 1: t = (w - a.x) / (b.x - a.x); break;
                case 2: t = (0 - a.y) / (b.y - a.y); break;
                default: t = (h - a.y) / (b.y - a.y); break;
            }
            return Pt{a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t};
        };
        Poly out;
        for (std::size_t i = 0; i < poly.size(); ++i) {
            const Pt& cur = poly[i];
            const Pt& prev = poly[(i + poly.size() - 1) % poly.size()];
            const bool ci = inside(cur), pi = inside(prev);
            if (ci) {
                if (!pi) out.push_back(cross(prev, cur));
                out.push_back(cur);
            } else if (pi) {
                out.push_back(cross(prev, cur));
            }
        }
        poly = std::move(out);
    }
    return poly;
}

// Antialiased fill of several contours (pixel coordinates), nonzero or even-odd.
void fillPolys(raster::Image& img, const Polys& polys, Color color, double opacity, bool evenOdd = false) {
    if (img.empty() || color.a == 0 || opacity <= 0) return;
    double minX = 1e18, minY = 1e18, maxX = -1e18, maxY = -1e18;
    struct Edge {
        double x0, y0, x1, y1;
        int dir;
    };
    std::vector<Edge> edges;
    for (const auto& p : polys) {
        if (p.size() < 3) continue;
        for (std::size_t i = 0; i < p.size(); ++i) {
            const Pt& a = p[i];
            const Pt& b = p[(i + 1) % p.size()];
            minX = std::min(minX, a.x);
            maxX = std::max(maxX, a.x);
            minY = std::min(minY, a.y);
            maxY = std::max(maxY, a.y);
            if (a.y == b.y) continue;
            if (a.y < b.y) edges.push_back({a.x, a.y, b.x, b.y, 1});
            else edges.push_back({b.x, b.y, a.x, a.y, -1});
        }
    }
    if (edges.empty()) return;
    const int bx0 = std::max(0, static_cast<int>(std::floor(minX)));
    const int by0 = std::max(0, static_cast<int>(std::floor(minY)));
    const int bx1 = std::min(img.width(), static_cast<int>(std::ceil(maxX)) + 1);
    const int by1 = std::min(img.height(), static_cast<int>(std::ceil(maxY)) + 1);
    if (bx1 <= bx0 || by1 <= by0) return;
    std::sort(edges.begin(), edges.end(), [](const Edge& a, const Edge& b) { return a.y0 < b.y0; });
    const int bw = bx1 - bx0;
    std::vector<float> row(static_cast<std::size_t>(bw) + 1);
    std::vector<std::pair<double, int>> xs;
    std::vector<const Edge*> active;
    std::size_t nextEdge = 0;
    constexpr int kSub = 4;
    const double alpha = color.a / 255.0 * std::clamp(opacity, 0.0, 1.0);
    for (int y = by0; y < by1; ++y) {
        std::fill(row.begin(), row.end(), 0.0f);
        bool any = false;
        for (int s = 0; s < kSub; ++s) {
            const double sy = y + (s + 0.5) / kSub;
            while (nextEdge < edges.size() && edges[nextEdge].y0 <= sy) active.push_back(&edges[nextEdge++]);
            active.erase(std::remove_if(active.begin(), active.end(), [&](const Edge* e) { return e->y1 <= sy; }), active.end());
            xs.clear();
            for (const Edge* e : active) {
                if (e->y0 > sy) continue;
                xs.push_back({e->x0 + (sy - e->y0) / (e->y1 - e->y0) * (e->x1 - e->x0), e->dir});
            }
            if (xs.size() < 2) continue;
            std::sort(xs.begin(), xs.end());
            int wind = 0;
            for (std::size_t i = 0; i + 1 < xs.size(); ++i) {
                wind = evenOdd ? (wind ^ 1) : wind + xs[i].second;
                if (wind == 0) continue;
                double l = std::max<double>(xs[i].first, bx0), r = std::min<double>(xs[i + 1].first, bx1);
                if (r <= l) continue;
                any = true;
                const int il = static_cast<int>(std::floor(l)), ir = static_cast<int>(std::floor(r));
                if (il == ir) {
                    row[static_cast<std::size_t>(il - bx0)] += static_cast<float>((r - l) / kSub);
                    continue;
                }
                row[static_cast<std::size_t>(il - bx0)] += static_cast<float>((il + 1 - l) / kSub);
                for (int x = il + 1; x < ir; ++x) row[static_cast<std::size_t>(x - bx0)] += 1.0f / kSub;
                if (ir < bx1) row[static_cast<std::size_t>(ir - bx0)] += static_cast<float>((r - ir) / kSub);
            }
        }
        if (!any) continue;
        for (int x = bx0; x < bx1; ++x) {
            const double cov = std::min(1.0f, row[static_cast<std::size_t>(x - bx0)]);
            if (cov <= 0) continue;
            const double a = cov * alpha;
            std::uint8_t* px = img.pixel(x, y);
            px[0] = static_cast<std::uint8_t>(std::lround(px[0] + (color.r - px[0]) * a));
            px[1] = static_cast<std::uint8_t>(std::lround(px[1] + (color.g - px[1]) * a));
            px[2] = static_cast<std::uint8_t>(std::lround(px[2] + (color.b - px[2]) * a));
            px[3] = static_cast<std::uint8_t>(std::min(255.0, px[3] + (255 - px[3]) * a));
        }
    }
}

Polys transformed(const Polys& polys, const Affine& m) {
    Polys out;
    out.reserve(polys.size());
    for (const auto& p : polys) {
        Poly q;
        q.reserve(p.size());
        for (const auto& pt : p) q.push_back(m.apply(pt.x, pt.y));
        out.push_back(std::move(q));
    }
    return out;
}

Color paintColor(const Layout& doc, const std::string& spec, bool* none = nullptr) {
    auto p = resolvePaint(doc, spec);
    if (none) *none = !p || p->none;
    return p && !p->none ? p->rgb : Color{0, 0, 0, 0};
}

// ---- drawing frames ------------------------------------------------------------------

struct Painter {
    const LayoutDoc& doc;
    raster::Image& img;
    Affine toPx;  // page mm -> px
    double pxPerMm = 1;
    bool guides = false;

    void fill(const Polys& local, const Affine& m, const std::string& spec, double opacity, bool evenOdd = false) {
        bool none = false;
        Color c = paintColor(doc, spec, &none);
        if (none) return;
        fillPolys(img, transformed(local, toPx * m), c, opacity, evenOdd);
    }

    void text(const Frame& f, const Affine& m, const FrameText& ft) {
        const Affine px = toPx * m;
        for (const auto& line : ft.lines) {
            for (const auto& run : line.runs) {
                bool none = false;
                const Color c = paintColor(doc, run.color, &none);
                if (none) continue;
                auto tt = run.face->outlines();
                const double emPx = run.size * pxPerMm;
                Polys glyphs;
                double x = run.x;
                const double unit = run.size / 1000.0;
                for (unsigned cp : pdfdoc::utf8Decode(run.text)) {
                    const double adv = run.face->advance(cp) * unit + run.tracking;
                    if (cp != ' ') {
                        if (tt && emPx >= 2.5) {
                            const auto& shape = pdfdoc::needsUnicode(cp) ? *pdfdoc::unicodeFontFor(cp) : *tt;
                            for (const auto& contour : shape.outline(cp)) {
                                Poly q;
                                q.reserve(contour.size());
                                for (const auto& g : contour) q.push_back({x + g.x * unit, line.baseline - g.y * unit});
                                glyphs.push_back(std::move(q));
                            }
                        } else {  // greeked: a grey bar per glyph
                            glyphs.push_back(rectPoly(x, line.baseline - run.size * 0.5, adv * 0.9, run.size * 0.5, 0));
                        }
                    }
                    x += adv + (cp == ' ' ? run.wordSpace : 0);
                }
                Color gc = c;
                if (!tt || emPx < 2.5) gc = {static_cast<std::uint8_t>((c.r + 2 * 170) / 3), static_cast<std::uint8_t>((c.g + 2 * 170) / 3),
                                             static_cast<std::uint8_t>((c.b + 2 * 170) / 3), c.a};
                fillPolys(img, transformed(glyphs, px), gc, f.opacity);
                if (run.underline) {
                    Polys u{rectPoly(run.x, line.baseline + run.size * 0.1, run.width, std::max(run.size * 0.05, 0.1), 0)};
                    fillPolys(img, transformed(u, px), c, f.opacity);
                }
            }
        }
    }

    void picture(const Frame& f, const Affine& m) {
        const PictureInfo info = pictureInfo(f.path);
        if (!info.ok) {
            if (guides) {  // placeholder cross
                Polys x = strokePolys({{0, 0}, {f.w, f.h}}, false, 0.3);
                for (auto& p : strokePolys({{f.w, 0}, {0, f.h}}, false, 0.3)) x.push_back(p);
                fillPolys(img, transformed(x, toPx * m), {200, 60, 60, 255}, 1);
            }
            return;
        }
        const Box b = pictureBox(f, info);
        if (info.vector) {
            vectorArt(f, m, b);
            return;
        }
        auto src = loadImage(f.path);
        if (!src || src->empty()) return;
        const Affine px = toPx * m, inv = px.inverse();
        // shrink big pictures to about the drawn size first so sampling doesn't alias
        std::shared_ptr<const raster::Image> image = src;
        const double drawnW = b.w * pxPerMm, drawnH = b.h * pxPerMm;
        if (drawnW > 0 && drawnH > 0 && (src->width() > drawnW * 2 || src->height() > drawnH * 2)) {
            const int w = std::max(1, static_cast<int>(std::lround(std::min<double>(src->width(), drawnW * 1.5))));
            const int h = std::max(1, static_cast<int>(std::lround(std::min<double>(src->height(), drawnH * 1.5))));
            image = std::make_shared<const raster::Image>(raster::resize(*src, w, h));
        }
        double x0 = 1e18, y0 = 1e18, x1 = -1e18, y1 = -1e18;
        for (auto [u, v] : {std::pair{0.0, 0.0}, {f.w, 0.0}, {0.0, f.h}, {f.w, f.h}}) {
            const auto p = px.apply(u, v);
            x0 = std::min(x0, p.x);
            y0 = std::min(y0, p.y);
            x1 = std::max(x1, p.x);
            y1 = std::max(y1, p.y);
        }
        const int ix0 = std::max(0, static_cast<int>(std::floor(x0))), iy0 = std::max(0, static_cast<int>(std::floor(y0)));
        const int ix1 = std::min(img.width(), static_cast<int>(std::ceil(x1))), iy1 = std::min(img.height(), static_cast<int>(std::ceil(y1)));
        const int iw = image->width(), ih = image->height();
        for (int y = iy0; y < iy1; ++y) {
            for (int x = ix0; x < ix1; ++x) {
                const auto l = inv.apply(x + 0.5, y + 0.5);
                if (l.x < 0 || l.y < 0 || l.x > f.w || l.y > f.h) continue;
                const double sx = (l.x - b.x) / b.w * iw - 0.5, sy = (l.y - b.y) / b.h * ih - 0.5;
                if (sx < -0.5 || sy < -0.5 || sx > iw - 0.5 || sy > ih - 0.5) continue;
                const int xa = std::clamp(static_cast<int>(std::floor(sx)), 0, iw - 1), ya = std::clamp(static_cast<int>(std::floor(sy)), 0, ih - 1);
                const int xb = std::min(xa + 1, iw - 1), yb = std::min(ya + 1, ih - 1);
                const double fx = std::clamp(sx - xa, 0.0, 1.0), fy = std::clamp(sy - ya, 0.0, 1.0);
                double c[4];
                for (int k = 0; k < 4; ++k) {
                    const double top = image->pixel(xa, ya)[k] * (1 - fx) + image->pixel(xb, ya)[k] * fx;
                    const double bot = image->pixel(xa, yb)[k] * (1 - fx) + image->pixel(xb, yb)[k] * fx;
                    c[k] = top * (1 - fy) + bot * fy;
                }
                const double a = c[3] / 255.0 * f.opacity;
                std::uint8_t* d = img.pixel(x, y);
                for (int k = 0; k < 3; ++k) d[k] = static_cast<std::uint8_t>(std::lround(d[k] + (c[k] - d[k]) * a));
            }
        }
    }

    void vectorArt(const Frame& f, const Affine& m, const Box& b) {
        auto v = loadVector(f.path);
        if (!v) return;
        const double sx = b.w / std::max(1, v->width), sy = b.h / std::max(1, v->height);
        const bool clip = f.fit == Fit::Fill || f.fit == Fit::Manual;
        const double detail = std::max(sx, sy) * pxPerMm;
        for (const auto& shape : v->shapes) {
            if (!shape.visible || shape.opacity <= 0) continue;
            const vector::Outline o = vector::outline(shape, std::max(0.25, detail));
            Polys fills, strokes;
            for (const auto& c : o) {
                Poly p;
                for (const auto& q : c.points) p.push_back({b.x + q.x * sx, b.y + q.y * sy});
                if (shape.stroke && shape.stroke->a > 0 && shape.strokeWidth > 0) {
                    for (auto& s : strokePolys(p, c.closed, shape.strokeWidth * std::sqrt(sx * sy))) strokes.push_back(s);
                }
                fills.push_back(std::move(p));
            }
            if (clip) {
                for (auto& p : fills) p = clipRect(p, f.w, f.h);
                for (auto& p : strokes) p = clipRect(p, f.w, f.h);
            }
            const double op = f.opacity * shape.opacity;
            if (shape.fill && shape.kind != vector::ShapeKind::Line) fillPolys(img, transformed(fills, toPx * m), *shape.fill, op, true);
            if (!strokes.empty()) fillPolys(img, transformed(strokes, toPx * m), *shape.stroke, op);
        }
    }

    void frame(const Frame& f, const FrameText* text) {
        const Affine m = frameToPage(f);
        const double sw = f.strokeWidth * kPtToMm;
        switch (f.type) {
            case FrameType::Line: {
                fill(strokePolys({{0, 0}, {f.w, f.h}}, false, sw), m, f.stroke.empty() ? "black" : f.stroke, f.opacity);
                return;
            }
            case FrameType::Ellipse: {
                const Poly e = ellipsePoly(f.w / 2, f.h / 2, std::abs(f.w) / 2, std::abs(f.h) / 2);
                fill({e}, m, f.fill, f.opacity);
                if (!f.stroke.empty()) fill(strokePolys(e, true, sw), m, f.stroke, f.opacity);
                return;
            }
            default: break;
        }
        const Poly box = rectPoly(0, 0, f.w, f.h, f.type == FrameType::Rect ? f.radius : 0);
        fill({box}, m, f.fill, f.opacity);
        if (f.type == FrameType::Image || f.type == FrameType::Vector) picture(f, m);
        if (f.type == FrameType::Text && text) this->text(f, m, *text);
        if (!f.stroke.empty()) fill(strokePolys(box, true, sw), m, f.stroke, f.opacity);
    }

    void outline(const Frame& f, Color c) {
        const Poly box = rectPoly(0, 0, f.w, f.h, 0);
        fillPolys(img, transformed(strokePolys(box, true, 1.0 / pxPerMm), toPx * frameToPage(f)), c, 1);
    }
};

}  // namespace

raster::Image renderPage(const LayoutDoc& doc, int pageIndex, const RenderOptions& options) {
    if (pageIndex < 0 || pageIndex >= static_cast<int>(doc.pages.size())) throw std::runtime_error("no such page");
    const Page& page = doc.pages[static_cast<std::size_t>(pageIndex)];
    const double s = std::clamp(options.dpi, 1.0, 2400.0) / 25.4;
    const double off = options.bleed ? doc.bleed : 0;
    const int w = std::max(1, static_cast<int>(std::lround((page.width + 2 * off) * s)));
    const int h = std::max(1, static_cast<int>(std::lround((page.height + 2 * off) * s)));
    if (static_cast<double>(w) * h > 400e6) throw std::runtime_error("that page would be over 400 megapixels; lower the dpi");
    raster::Image img(w, h, {255, 255, 255, 255});
    Painter painter{doc, img, Affine{s, 0, 0, s, off * s, off * s}, s, options.guides};

    if (const Master* master = doc.findMaster(page.master)) {
        for (const auto& mf : master->frames) {
            const Frame f = placedMasterFrame(doc, mf, pageIndex);
            if (f.type == FrameType::Text) {
                auto it = doc.stories.find(f.story);
                if (it == doc.stories.end()) continue;
                const Story story = fieldsFilled(doc, it->second, pageIndex);
                StoryLayout sl = layoutStory(doc, story, {&f}, pageIndex);
                painter.frame(f, &sl.frames[f.id]);
            } else {
                painter.frame(f, nullptr);
            }
        }
    }
    const auto layouts = layoutAll(doc);
    for (const auto& f : page.frames) {
        const FrameText* text = nullptr;
        if (f.type == FrameType::Text) {
            auto it = layouts.find(f.story);
            if (it != layouts.end()) {
                auto ft = it->second.frames.find(f.id);
                if (ft != it->second.frames.end()) text = &ft->second;
            }
        }
        painter.frame(f, text);
    }

    if (options.guides) {
        auto rectLine = [&](double x, double y, double rw, double rh, Color c) {
            Poly box = rectPoly(x, y, rw, rh, 0);
            fillPolys(img, transformed(strokePolys(box, true, 1.0 / s), painter.toPx), c, 1);
        };
        const Box mb = marginBox(doc, pageIndex);
        rectLine(mb.x, mb.y, mb.w, mb.h, {220, 80, 220, 255});
        if (doc.columns > 1) {
            const double cw = (mb.w - doc.gutter * (doc.columns - 1)) / doc.columns;
            for (int c = 1; c < doc.columns; ++c) {
                const double x = mb.x + c * cw + (c - 1) * doc.gutter;
                rectLine(x, mb.y, doc.gutter, mb.h, {160, 120, 230, 255});
            }
        }
        if (options.bleed) rectLine(-doc.bleed, -doc.bleed, page.width + 2 * doc.bleed, page.height + 2 * doc.bleed, {230, 60, 60, 255});
        rectLine(0, 0, page.width, page.height, {120, 120, 120, 255});
        for (const auto& f : page.frames) {
            if (f.type == FrameType::Text || f.type == FrameType::Image || f.type == FrameType::Vector) painter.outline(f, {90, 150, 230, 255});
        }
        // overset marker: a red square at the bottom right of a story's last frame
        for (const auto& [id, sl] : layouts) {
            if (sl.overset == 0) continue;
            auto chain = storyChain(doc, id);
            if (chain.empty()) continue;
            const Frame* last = chain.back();
            bool here = false;
            for (const auto& f : page.frames) here = here || &f == last;
            if (!here) continue;
            const double k = 3;
            Polys mark{rectPoly(last->w - k, last->h - k, k, k, 0)};
            fillPolys(img, transformed(mark, painter.toPx * frameToPage(*last)), {230, 30, 30, 255}, 1);
        }
    }
    return img;
}

}  // namespace wetwaresuite::layout
