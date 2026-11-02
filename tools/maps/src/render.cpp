// Map rendering: the map is laid out once as a display list in pixels (paths,
// text, images), then drawn by one of three backends: an antialiased raster
// (PNG), SVG text or a vector PDF. Also styling classes, the offline country
// basemap, map packs (.pmtiles from disk) and the optional OSM tile basemap (fetched with curl, cached).
#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <set>
#include <tuple>
#include <variant>

#include "wetwaresuite/core/process.hpp"
#include "wetwaresuite/pdf/page.hpp"
#include "wetwaresuite/pdfdoc/writer.hpp"
#include "wetwaresuite/raster/codec.hpp"
#include "internal.hpp"

namespace wetwaresuite::maps {

namespace fs = std::filesystem;
using namespace detail;

// ---- style classes ------------------------------------------------------------------------

namespace {

const std::vector<Color>& qualitative() {
    static const std::vector<Color> c = {{0x4e, 0x79, 0xa7, 255}, {0xf2, 0x8e, 0x2b, 255}, {0xe1, 0x57, 0x59, 255}, {0x76, 0xb7, 0xb2, 255},
                                         {0x59, 0xa1, 0x4f, 255}, {0xed, 0xc9, 0x48, 255}, {0xb0, 0x7a, 0xa1, 255}, {0xff, 0x9d, 0xa7, 255},
                                         {0x9c, 0x75, 0x5f, 255}, {0xba, 0xb0, 0xac, 255}, {0x86, 0xbc, 0xb6, 255}, {0xd4, 0xa6, 0xc8, 255}};
    return c;
}

Color darken(Color c, double k) {
    return {static_cast<std::uint8_t>(c.r * k), static_cast<std::uint8_t>(c.g * k), static_cast<std::uint8_t>(c.b * k), c.a};
}

Color withAlpha(Color c, double a) {
    c.a = static_cast<std::uint8_t>(std::lround(std::clamp(a, 0.0, 1.0) * c.a));
    return c;
}

std::string shortNum(double v) {
    const double a = std::abs(v);
    if (a >= 1e9) return fmt(v / 1e9, a >= 1e10 ? 0 : 1) + "B";
    if (a >= 1e6) return fmt(v / 1e6, a >= 1e7 ? 0 : 1) + "M";
    if (a >= 1e4) return fmt(v / 1e3, 0) + "k";
    if (a >= 100) return fmt(v, 0);
    if (a >= 1) return fmt(v, 2);
    return fmt(v, 4);
}

}  // namespace

// ---- Jenks natural breaks --------------------------------------------------------------

namespace {

// Sorted values grouped into runs: count, sum and sum of squares per group, prefix-summed,
// so the squared deviation of any run of groups is O(1).
struct JenksGroups {
    std::vector<double> w, s, ss;  // prefix sums, size groups + 1
    std::vector<double> last;      // largest value in each group
    double cost(std::size_t i, std::size_t j) const {  // groups i..j inclusive
        const double n = w[j + 1] - w[i];
        const double sum = s[j + 1] - s[i];
        const double c = (ss[j + 1] - ss[i]) - sum * sum / n;
        return c > 0 ? c : 0;
    }
};

// Fills cur[j] for j in [lo, hi] (the best cost of putting groups 0..j into m+1 classes), knowing
// the best split for those j lies in [optLo, optHi]: the split point never moves left as j grows
// (the squared-deviation cost obeys the quadrangle inequality), so divide and conquer finds every
// optimum in O(n log n) per class instead of O(n^2).
void jenksLayer(const JenksGroups& g, const std::vector<double>& prev, std::vector<double>& cur, std::vector<std::uint32_t>& from,
                std::size_t m, std::size_t lo, std::size_t hi, std::size_t optLo, std::size_t optHi) {
    // iterative over an explicit stack: deep recursion would be fine (log n), but keep it flat anyway
    struct Job {
        std::size_t lo, hi, optLo, optHi;
    };
    std::vector<Job> stack{{lo, hi, optLo, optHi}};
    while (!stack.empty()) {
        const Job job = stack.back();
        stack.pop_back();
        if (job.lo > job.hi) continue;
        const std::size_t mid = job.lo + (job.hi - job.lo) / 2;
        double best = std::numeric_limits<double>::infinity();
        std::size_t arg = std::max(job.optLo, m);
        // the last class is groups i..mid; groups 0..i-1 hold the other m classes
        for (std::size_t i = std::max(job.optLo, m); i <= std::min(mid, job.optHi); ++i) {
            const double c = prev[i - 1] + g.cost(i, mid);
            if (c < best) {
                best = c;
                arg = i;
            }
        }
        cur[mid] = best;
        from[mid] = static_cast<std::uint32_t>(arg);
        if (mid > job.lo) stack.push_back({job.lo, mid - 1, job.optLo, arg});
        stack.push_back({mid + 1, job.hi, arg, job.optHi});
    }
}

}  // namespace

std::vector<double> jenksBreaks(std::vector<double> values, int classes, std::size_t maxGroups) {
    values.erase(std::remove_if(values.begin(), values.end(), [](double v) { return !std::isfinite(v); }), values.end());
    if (values.empty()) return {};
    std::sort(values.begin(), values.end());
    // equal values always share a class, so work on distinct values with their counts; when there are
    // still too many, merge neighbours into maxGroups runs of equal size (breaks then fall between runs)
    std::vector<std::size_t> starts;  // index of the first value of each distinct run
    for (std::size_t i = 0; i < values.size(); ++i) {
        if (i == 0 || values[i] != values[i - 1]) starts.push_back(i);
    }
    const std::size_t distinct = starts.size();
    maxGroups = std::max<std::size_t>(maxGroups, 2);
    if (distinct > maxGroups) {
        // half the runs start at the widest gaps between neighbouring values (where natural breaks fall),
        // the other half are spread evenly by rank
        const std::size_t half = maxGroups / 2;
        std::vector<std::size_t> byGap(distinct - 1);  // byGap[i]: the gap before distinct value i + 1
        for (std::size_t i = 0; i + 1 < distinct; ++i) byGap[i] = i + 1;
        std::nth_element(byGap.begin(), byGap.begin() + static_cast<std::ptrdiff_t>(half), byGap.end(), [&](std::size_t a, std::size_t b) {
            const double ga = values[starts[a]] - values[starts[a] - 1], gb = values[starts[b]] - values[starts[b] - 1];
            return ga != gb ? ga > gb : a < b;
        });
        std::vector<std::size_t> pick(byGap.begin(), byGap.begin() + static_cast<std::ptrdiff_t>(half));
        for (std::size_t gi = 0; gi < maxGroups - half; ++gi) pick.push_back(gi * distinct / (maxGroups - half));
        std::sort(pick.begin(), pick.end());
        pick.erase(std::unique(pick.begin(), pick.end()), pick.end());
        std::vector<std::size_t> merged;
        for (std::size_t i : pick) merged.push_back(starts[i]);
        starts = std::move(merged);
    }
    const std::size_t n = starts.size();
    JenksGroups g;
    g.w.assign(n + 1, 0);
    g.s.assign(n + 1, 0);
    g.ss.assign(n + 1, 0);
    g.last.resize(n);
    const double shift = values[values.size() / 2];  // centring keeps the sums of squares accurate
    for (std::size_t gi = 0; gi < n; ++gi) {
        const std::size_t b = starts[gi], e = gi + 1 < n ? starts[gi + 1] : values.size();
        double s = 0, ss = 0;
        for (std::size_t i = b; i < e; ++i) {
            const double d = values[i] - shift;
            s += d;
            ss += d * d;
        }
        g.w[gi + 1] = g.w[gi] + static_cast<double>(e - b);
        g.s[gi + 1] = g.s[gi] + s;
        g.ss[gi + 1] = g.ss[gi] + ss;
        g.last[gi] = values[e - 1];
    }
    const std::size_t k = std::min<std::size_t>(static_cast<std::size_t>(std::max(1, classes)), n);
    std::vector<double> prev(n), cur(n);
    for (std::size_t j = 0; j < n; ++j) prev[j] = g.cost(0, j);
    std::vector<std::vector<std::uint32_t>> from(k);  // from[m][j]: first group of the last class
    for (std::size_t m = 1; m < k; ++m) {
        from[m].assign(n, 0);
        std::fill(cur.begin(), cur.end(), std::numeric_limits<double>::infinity());
        jenksLayer(g, prev, cur, from[m], m, m, n - 1, m, n - 1);
        std::swap(prev, cur);
    }
    std::vector<double> breaks(k + 1);
    breaks[0] = values.front();
    std::size_t j = n - 1;
    for (std::size_t m = k - 1; m >= 1; --m) {
        breaks[m + 1] = g.last[j];
        j = from[m][j] - 1;
    }
    breaks[1] = g.last[j];
    return breaks;
}

std::vector<StyleClass> styleClasses(const Layer& layer) {
    const Style& s = layer.style;
    std::vector<StyleClass> out;
    if (s.mode == "categorized" && !s.field.empty()) {
        std::vector<Prop> values;
        std::map<std::string, std::size_t> counts;
        for (const auto& f : layer.features) {
            auto it = f.props.find(s.field);
            const Prop v = it == f.props.end() ? Prop::null() : it->second;
            if (counts[v.text()]++ == 0) values.push_back(v);
        }
        std::sort(values.begin(), values.end(), [](const Prop& a, const Prop& b) {
            auto x = a.asNumber(), y = b.asNumber();
            if (x && y) return *x < *y;
            if (a.isNull() != b.isNull()) return b.isNull();
            return a.text() < b.text();
        });
        const std::size_t limit = 12;
        const auto& pal = s.palette.empty() ? qualitative() : s.palette;
        for (std::size_t i = 0; i < values.size() && i < limit; ++i) {
            StyleClass c;
            c.value = values[i];
            c.label = values[i].isNull() ? "(none)" : values[i].text();
            c.color = pal[i % pal.size()];
            out.push_back(c);
        }
        if (values.size() > limit) {
            StyleClass c;
            c.other = true;
            c.label = "other (" + std::to_string(values.size() - limit) + ")";
            c.color = {0xaa, 0xaa, 0xaa, 255};
            out.push_back(c);
        }
    } else if (s.mode == "graduated" && !s.field.empty()) {
        std::vector<double> v;
        for (const auto& f : layer.features) {
            auto it = f.props.find(s.field);
            if (it == f.props.end()) continue;
            if (auto n = it->second.asNumber(); n && std::isfinite(*n)) v.push_back(*n);
        }
        if (v.empty()) return out;
        std::sort(v.begin(), v.end());
        const int k = std::clamp(s.classes, 1, 12);
        std::vector<double> breaks{v.front()};
        if (s.method == "equal") {
            for (int i = 1; i <= k; ++i) breaks.push_back(v.front() + (v.back() - v.front()) * i / k);
        } else if (s.method == "natural" || s.method == "jenks") {
            breaks = jenksBreaks(v, k);  // v is sorted and finite, so this starts at v.front()
        } else {
            for (int i = 1; i <= k; ++i) breaks.push_back(v[std::min(v.size() - 1, static_cast<std::size_t>(std::ceil(static_cast<double>(i) * static_cast<double>(v.size()) / k)) - 1)]);
        }
        // drop duplicate breaks; the first class may hold just the minimum (e.g. many zeros), so its top may equal it
        std::vector<double> b{breaks.front()};
        for (std::size_t i = 1; i < breaks.size(); ++i) {
            if (breaks[i] > b.back() || (b.size() == 1 && breaks.size() > 2 && breaks[i] == b.back() && i + 1 < breaks.size() &&
                                         breaks[i + 1] > breaks[i])) {
                b.push_back(breaks[i]);
            }
        }
        if (b.size() == 1) b.push_back(b[0]);
        const std::size_t n = b.size() - 1;
        for (std::size_t i = 0; i < n; ++i) {
            StyleClass c;
            c.lo = b[i];
            c.hi = b[i + 1];
            c.label = shortNum(c.lo) + " - " + shortNum(c.hi);
            c.color = rampColor(s.ramp, n == 1 ? 1.0 : static_cast<double>(i) / static_cast<double>(n - 1));
            if (s.sizeByValue) c.size = s.pointSize * (0.6 + 1.6 * (n == 1 ? 1.0 : static_cast<double>(i) / static_cast<double>(n - 1)));
            out.push_back(c);
        }
    }
    return out;
}

namespace {

// The class a feature falls in, or -1 (no classes / no value).
int classOf(const Style& s, const std::vector<StyleClass>& classes, const Feature& f) {
    if (classes.empty()) return -1;
    auto it = f.props.find(s.field);
    const Prop v = it == f.props.end() ? Prop::null() : it->second;
    if (s.mode == "categorized") {
        for (std::size_t i = 0; i < classes.size(); ++i) {
            if (classes[i].other) return static_cast<int>(i);
            if (classes[i].value == v && classes[i].value.isNull() == v.isNull()) return static_cast<int>(i);
        }
        return -1;
    }
    auto n = v.asNumber();
    if (!n) return -1;
    for (std::size_t i = 0; i < classes.size(); ++i) {
        if (*n <= classes[i].hi || i + 1 == classes.size()) return static_cast<int>(i);
    }
    return -1;
}

// ---- display list -------------------------------------------------------------------------

struct P {
    double x = 0, y = 0;
};

struct DPath {
    std::vector<std::vector<P>> parts;
    bool closed = false;
    std::optional<Color> fill, stroke;
    double width = 1;
    std::string dash;
};

struct DText {
    double x = 0, y = 0;  // baseline start (after alignment)
    std::string text;
    double size = 11;
    Color color{0, 0, 0, 255};
    bool halo = false, bold = false;
};

struct DImage {
    double x = 0, y = 0, w = 0, h = 0;
    std::shared_ptr<const raster::Image> img;
};

using DOp = std::variant<DPath, DText, DImage>;

struct Scene {
    int W = 0, H = 0;
    Color background;
    std::vector<DOp> ops;
};

pdf::Font fontOf(bool bold) { return pdf::Font{pdf::Family::Sans, bold, false}; }
double textWidth(const std::string& s, double size, bool bold) { return pdf::textWidthPx(s, fontOf(bold), size); }

// ---- the view -----------------------------------------------------------------------------

struct View {
    Crs crs;
    double x0 = 0, y0 = 0, scale = 1;  // projected top-left, px per unit
    int W = 0, H = 0;
    BBox domain{-180, -90, 180, 90};  // lon/lat worth projecting (clips the far side of a TM, the poles of Mercator)
    P px(const Coord& ll) const {
        const Coord p = crs.forward(ll);
        return {(p.x - x0) * scale, (y0 - p.y) * scale};
    }
    Coord lonlat(double x, double y) const { return crs.inverse({x0 + x / scale, y0 - y / scale}); }
};

View makeView(const Map& map, const RenderOptions& o, std::vector<std::string>& warnings) {
    View v;
    v.crs = o.crs;
    // an explicit area, else the map's own view (set by maps.view or a bookmark), else everything
    const std::optional<BBox> chosen = o.bbox ? o.bbox : (map.view && map.view->valid() ? map.view : std::nullopt);
    BBox ll = chosen ? *chosen : map.bounds(true);
    if (!ll.valid()) ll = {-180, -60, 180, 85};
    if (v.crs.kind == Crs::WebMercator) {
        ll.minY = std::max(ll.minY, -85.0);
        ll.maxY = std::min(ll.maxY, 85.0);
        // a world view: Antarctica would fill the bottom half
        if (!chosen && ll.width() > 180 && ll.minY < -60) ll.minY = -60;
    }
    // a single point or a tiny extent: open it up to about 2 km
    if (ll.width() < 1e-4 && ll.height() < 1e-4) {
        ll.minX -= 0.01;
        ll.maxX += 0.01;
        ll.minY -= 0.01;
        ll.maxY += 0.01;
    }
    BBox pb;
    for (int i = 0; i <= 16; ++i) {
        for (int j = 0; j <= 16; ++j) {
            const Coord p = v.crs.forward({ll.minX + ll.width() * i / 16, ll.minY + ll.height() * j / 16});
            pb.add(p.x, p.y);
        }
    }
    if (!chosen) {
        const double mx = pb.width() * o.margin, my = pb.height() * o.margin;
        pb.minX -= mx;
        pb.maxX += mx;
        pb.minY -= my;
        pb.maxY += my;
        // a few places in a north-south line: no taller than wide, so they don't open as a thin strip
        if (o.height <= 0 && pb.height() > pb.width()) {
            const double grow = (pb.height() - pb.width()) / 2;
            pb.minX -= grow;
            pb.maxX += grow;
        }
    }
    const double pw = std::max(pb.width(), 1e-9), ph = std::max(pb.height(), 1e-9);
    v.W = std::clamp(o.width, 16, 8000);
    v.H = o.height > 0 ? std::clamp(o.height, 16, 8000) : std::clamp(static_cast<int>(std::lround(v.W * ph / pw)), 64, 8000);
    v.scale = std::min(v.W / pw, v.H / ph);
    const double cx = (pb.minX + pb.maxX) / 2, cy = (pb.minY + pb.maxY) / 2;
    v.x0 = cx - v.W / v.scale / 2;
    v.y0 = cy + v.H / v.scale / 2;
    if (v.crs.kind == Crs::TransverseMercator && ll.width() > 40) warnings.push_back("a transverse Mercator/UTM view this wide distorts a lot; webmercator or wgs84 suit large areas");
    if (v.crs.kind == Crs::WebMercator) v.domain = {-180, -85.06, 180, 85.06};
    if (v.crs.kind == Crs::TransverseMercator) v.domain = {std::max(-180.0, v.crs.lon0 - 75), -90, std::min(180.0, v.crs.lon0 + 75), 90};
    if (v.crs.kind == Crs::EqualEarth) v.domain = {-180, -90, 180, 90};
    // an azimuthal view: the hemisphere around its centre (the far side folds over)
    if (v.crs.kind == Crs::Laea) v.domain = {std::max(-180.0, v.crs.lon0 - 90), std::max(-90.0, v.crs.lat0 - 90), std::min(180.0, v.crs.lon0 + 90), std::min(90.0, v.crs.lat0 + 90)};
    // and not much beyond what the view shows
    BBox seen;
    for (int i = 0; i <= 10; ++i) {
        for (int j = 0; j <= 10; ++j) {
            if (i != 0 && i != 10 && j != 0 && j != 10) continue;
            const Coord c = v.lonlat(v.W * i / 10.0, v.H * j / 10.0);
            seen.add(c.x, c.y);
        }
    }
    if (seen.valid() && seen.width() < 300) {
        const double mx = seen.width() * 0.25 + 1, my = seen.height() * 0.25 + 1;
        v.domain.minX = std::max(v.domain.minX, seen.minX - mx);
        v.domain.maxX = std::min(v.domain.maxX, seen.maxX + mx);
        v.domain.minY = std::max(v.domain.minY, seen.minY - my);
        v.domain.maxY = std::min(v.domain.maxY, seen.maxY + my);
    }
    return v;
}

BBox viewLonLat(const View& v) {
    BBox b;
    for (int i = 0; i <= 10; ++i) {
        for (int j = 0; j <= 10; ++j) {
            if (i != 0 && i != 10 && j != 0 && j != 10 && !(i == 5 && j == 5)) continue;
            const Coord c = v.lonlat(v.W * i / 10.0, v.H * j / 10.0);
            b.add(c.x, c.y);
        }
    }
    b.minX = std::max(b.minX, -180.0);
    b.maxX = std::min(b.maxX, 180.0);
    b.minY = std::max(b.minY, -90.0);
    b.maxY = std::min(b.maxY, 90.0);
    return b;
}

double metresPerPixel(const View& v, double y) {
    const Coord a = v.lonlat(v.W / 2.0 - 50, y), b = v.lonlat(v.W / 2.0 + 50, y);
    return geodesicDistance(a.x, a.y, b.x, b.y) / 100;
}

// ---- geometry -> pixels -----------------------------------------------------------------

void addPaths(const View& v, const Geometry& g0, DPath& path, double margin) {
    const BBox gb = bboxOf(g0);
    if (!gb.intersects(v.domain)) return;
    Geometry clipped;
    const bool inside = gb.minX >= v.domain.minX && gb.maxX <= v.domain.maxX && gb.minY >= v.domain.minY && gb.maxY <= v.domain.maxY;
    if (!inside) clipped = clip(g0, v.domain);
    const Geometry& g = inside ? g0 : clipped;
    Geometry px;
    px.type = g.type;
    auto conv = [&](const std::vector<Coord>& cs) {
        std::vector<Coord> out;
        out.reserve(cs.size());
        for (const auto& c : cs) {
            const P p = v.px(c);
            out.push_back({p.x, p.y});
        }
        return out;
    };
    for (const auto& l : g.lines) px.lines.push_back(conv(l));
    for (const auto& poly : g.polygons) {
        Polygon pp;
        for (const auto& r : poly) pp.push_back(conv(r));
        px.polygons.push_back(std::move(pp));
    }
    const BBox box{-margin, -margin, v.W + margin, v.H + margin};
    if (!bboxOf(px).intersects(box)) return;
    px = clip(px, box);
    auto toP = [](const std::vector<Coord>& cs) {
        std::vector<P> out;
        out.reserve(cs.size());
        for (const auto& c : cs) out.push_back({c.x, c.y});
        return out;
    };
    for (const auto& l : px.lines) path.parts.push_back(toP(l));
    for (const auto& poly : px.polygons) {
        for (const auto& r : poly) path.parts.push_back(toP(r));
    }
}

void marker(Scene& sc, double x, double y, double size, const std::string& shape, Color fill, Color stroke, double width) {
    DPath p;
    p.closed = true;
    p.fill = fill;
    p.stroke = stroke;
    p.width = width;
    const double r = size / 2;
    std::vector<P> pts;
    if (shape == "square") {
        pts = {{x - r, y - r}, {x + r, y - r}, {x + r, y + r}, {x - r, y + r}};
    } else if (shape == "triangle") {
        pts = {{x, y - r * 1.15}, {x + r * 1.05, y + r * 0.75}, {x - r * 1.05, y + r * 0.75}};
    } else if (shape == "diamond") {
        pts = {{x, y - r * 1.2}, {x + r, y}, {x, y + r * 1.2}, {x - r, y}};
    } else {
        const int n = std::clamp(static_cast<int>(size * 1.5), 12, 48);
        for (int i = 0; i < n; ++i) pts.push_back({x + r * std::cos(2 * kPi * i / n), y + r * std::sin(2 * kPi * i / n)});
    }
    p.parts.push_back(std::move(pts));
    sc.ops.push_back(std::move(p));
}

// ---- rasters into the view ---------------------------------------------------------------

std::vector<float> hillshade(const Grid& g) {
    std::vector<float> shade(g.values.size(), 1.0f);
    // cell size in metres
    const Coord c0 = g.crs.inverse(g.geo.apply(g.width / 2.0, g.height / 2.0)), cx = g.crs.inverse(g.geo.apply(g.width / 2.0 + 1, g.height / 2.0)),
                cy = g.crs.inverse(g.geo.apply(g.width / 2.0, g.height / 2.0 + 1));
    const double dx = std::max(1e-6, geodesicDistance(c0.x, c0.y, cx.x, cx.y)), dy = std::max(1e-6, geodesicDistance(c0.x, c0.y, cy.x, cy.y));
    const double az = 315 * kDeg, alt = 45 * kDeg;
    auto at = [&](int x, int y) {
        x = std::clamp(x, 0, g.width - 1);
        y = std::clamp(y, 0, g.height - 1);
        const float v = g.values[static_cast<std::size_t>(y) * g.width + x];
        return std::isfinite(v) ? static_cast<double>(v) : 0.0;
    };
    for (int y = 0; y < g.height; ++y) {
        for (int x = 0; x < g.width; ++x) {
            const double gx = ((at(x + 1, y - 1) + 2 * at(x + 1, y) + at(x + 1, y + 1)) - (at(x - 1, y - 1) + 2 * at(x - 1, y) + at(x - 1, y + 1))) / (8 * dx);
            const double gy = ((at(x - 1, y + 1) + 2 * at(x, y + 1) + at(x + 1, y + 1)) - (at(x - 1, y - 1) + 2 * at(x, y - 1) + at(x + 1, y - 1))) / (8 * dy);
            const double slope = std::atan(std::hypot(gx, gy));
            const double aspect = std::atan2(gy, -gx);
            double s = std::cos(alt) * std::cos(slope) + std::sin(alt) * std::sin(slope) * std::cos(az - kPi / 2 - aspect);
            shade[static_cast<std::size_t>(y) * g.width + x] = static_cast<float>(std::clamp(0.35 + 0.75 * s, 0.0, 1.0));
        }
    }
    return shade;
}

bool sameCrs(const Crs& a, const Crs& b) {
    return a.kind == b.kind && (a.kind == Crs::Geographic || a.kind == Crs::WebMercator ||
                                (a.lon0 == b.lon0 && a.lat0 == b.lat0 && a.k0 == b.k0 && a.falseEasting == b.falseEasting && a.falseNorthing == b.falseNorthing));
}

std::shared_ptr<raster::Image> rasterize(const View& v, const Grid& g, const Style& st) {
    auto img = std::make_shared<raster::Image>(v.W, v.H);
    const bool values = g.image.empty() && g.hasValues();
    std::vector<float> shade;
    if (g.hasValues() && st.hillshade) shade = hillshade(g);
    const double span = g.maxValue - g.minValue;
    const bool direct = sameCrs(v.crs, g.crs);
    bool any = false;
    for (int y = 0; y < v.H; ++y) {
        for (int x = 0; x < v.W; ++x) {
            double gx = 0, gy = 0;
            const double px = x + 0.5, py = y + 0.5;
            if (direct) {
                gx = v.x0 + px / v.scale;
                gy = v.y0 - py / v.scale;
            } else {
                const Coord ll = v.lonlat(px, py);
                if (!std::isfinite(ll.x) || !std::isfinite(ll.y)) continue;
                const Coord p = g.crs.forward(ll);
                gx = p.x;
                gy = p.y;
            }
            double col = 0, row = 0;
            if (!g.geo.invert(gx, gy, col, row)) continue;
            const int c = static_cast<int>(std::floor(col)), r = static_cast<int>(std::floor(row));
            if (c < 0 || r < 0 || c >= g.width || r >= g.height) continue;
            const std::size_t i = static_cast<std::size_t>(r) * g.width + c;
            Color out;
            if (values) {
                const float val = g.values[i];
                if (!std::isfinite(val) || (!std::isnan(g.nodata) && val == static_cast<float>(g.nodata))) continue;
                out = rampColor(st.rasterRamp, span > 0 ? (val - g.minValue) / span : 0.5);
            } else {
                out = g.image.at(c, r);
            }
            if (!shade.empty()) {
                const float s = shade[i];
                out = {static_cast<std::uint8_t>(out.r * s), static_cast<std::uint8_t>(out.g * s), static_cast<std::uint8_t>(out.b * s), out.a};
            }
            out.a = static_cast<std::uint8_t>(out.a * std::clamp(st.opacity, 0.0, 1.0));
            if (out.a) any = true;
            img->set(x, y, out);
        }
    }
    if (!any) return nullptr;
    return img;
}

// ---- OSM tiles --------------------------------------------------------------------------

std::string envVar(const char* name) {
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

std::shared_ptr<Grid> osmBasemap(const View& v, const RenderOptions& o, RenderInfo& info) {
    const double world = 2 * kPi * kA;
    // Web Mercator units per screen pixel at the view centre
    const Coord centre = v.lonlat(v.W / 2.0, v.H / 2.0);
    const double mpp = metresPerPixel(v, v.H / 2.0);
    const double upp = mpp / std::max(0.05, std::cos(centre.y * kDeg));
    int z = std::clamp(static_cast<int>(std::lround(std::log2(world / (256 * upp)))), 0, 19);
    const BBox ll = viewLonLat(v);
    double tx0 = 0, ty0 = 0, tx1 = 0, ty1 = 0;
    for (;; --z) {
        lonLatToTile(ll.minX, std::min(ll.maxY, 85.05), z, tx0, ty0);
        lonLatToTile(ll.maxX, std::max(ll.minY, -85.05), z, tx1, ty1);
        const double count = (std::floor(tx1) - std::floor(tx0) + 1) * (std::floor(ty1) - std::floor(ty0) + 1);
        if (count <= o.maxTiles || z == 0) break;
    }
    const int n = 1 << z;
    const int x0 = std::clamp(static_cast<int>(std::floor(tx0)), 0, n - 1), x1 = std::clamp(static_cast<int>(std::floor(tx1)), 0, n - 1);
    const int y0 = std::clamp(static_cast<int>(std::floor(ty0)), 0, n - 1), y1 = std::clamp(static_cast<int>(std::floor(ty1)), 0, n - 1);
    info.zoom = z;
    const fs::path cache = o.tileCache.empty() ? defaultTileCache() : fs::path(o.tileCache);
    // a tile_url that isn't http(s) is a folder of tiles already on disk: read it, never download
    const bool localTiles = o.tileUrl.rfind("http://", 0) != 0 && o.tileUrl.rfind("https://", 0) != 0;
    const auto curl = o.offline || localTiles ? std::nullopt : findProgram("curl");
    if (!o.offline && !localTiles && !curl) info.warnings.push_back("curl isn't installed, so only cached OSM tiles can be used");
    // a folder per tile server, so switching tile_url never mixes caches
    std::string serverKey;
    for (char c : o.tileUrl.substr(0, o.tileUrl.find('{'))) serverKey += std::isalnum(static_cast<unsigned char>(c)) ? c : '_';
    const fs::path base = cache / serverKey;
    auto mosaic = std::make_shared<Grid>();
    mosaic->width = (x1 - x0 + 1) * 256;
    mosaic->height = (y1 - y0 + 1) * 256;
    mosaic->image = raster::Image(mosaic->width, mosaic->height);
    mosaic->crs = Crs::webMercator();
    const double tileUnits = world / n;
    mosaic->geo = {tileUnits / 256, 0, -world / 2 + x0 * tileUnits, 0, -tileUnits / 256, world / 2 - y0 * tileUnits};
    int missing = 0;
    bool failedDownload = false;
    for (int ty = y0; ty <= y1; ++ty) {
        for (int tx = x0; tx <= x1; ++tx) {
            fs::path file = base / std::to_string(z) / std::to_string(tx) / (std::to_string(ty) + ".png");
            if (localTiles) {
                std::string local = o.tileUrl;
                for (const auto& [key, val] : {std::pair<std::string, int>{"{z}", z}, {"{x}", tx}, {"{y}", ty}}) {
                    for (auto at = local.find(key); at != std::string::npos; at = local.find(key)) local.replace(at, key.size(), std::to_string(val));
                }
                file = fs::path(local);
            }
            std::error_code ec;
            bool have = fs::exists(file, ec);
            bool stale = false;
            if (have && !localTiles) {
                const auto age = fs::file_time_type::clock::now() - fs::last_write_time(file, ec);
                stale = !ec && age > std::chrono::hours(24 * 30);
            }
            if ((!have || stale) && curl && !failedDownload) {
                std::string url = o.tileUrl;
                auto rep = [&](const std::string& key, int val) {
                    for (auto p = url.find(key); p != std::string::npos; p = url.find(key)) url.replace(p, key.size(), std::to_string(val));
                };
                rep("{z}", z);
                rep("{x}", tx);
                rep("{y}", ty);
                fs::create_directories(file.parent_path(), ec);
                fs::path tmp = file;
                tmp += ".part";
                // one request at a time, identified, as the OSM tile usage policy asks
                const ProcessResult r = runProgram({curl->string(), "-sSfL", "--max-time", "20", "-A",
                                                    "wetwaresuite-maps/1.0 (+https://github.com/ArthurJClack/wetwaresuite)", "-o", tmp.string(), url});
                if (r.exitCode == 0 && fs::exists(tmp, ec)) {
                    fs::rename(tmp, file, ec);
                    if (ec) {
                        fs::remove(file, ec);
                        fs::rename(tmp, file, ec);
                    }
                    have = fs::exists(file, ec);
                    ++info.tilesFetched;
                } else {
                    fs::remove(tmp, ec);
                    failedDownload = true;  // one failure (offline, blocked, rate-limited): stop asking the server this render
                    if (info.warnings.empty() || info.warnings.back().rfind("tile download failed", 0) != 0)
                        info.warnings.push_back("tile download failed (" + trim(r.output).substr(0, 160) + "); offline areas stay blank");
                }
            }
            if (!have) {
                ++missing;
                continue;
            }
            try {
                const raster::Image t = raster::load(file);
                const int ox = (tx - x0) * 256, oy = (ty - y0) * 256;
                for (int y = 0; y < std::min(256, t.height()); ++y) {
                    for (int x = 0; x < std::min(256, t.width()); ++x) mosaic->image.set(ox + x, oy + y, t.at(x, y));
                }
                ++info.tilesUsed;
            } catch (const std::exception&) {
                ++missing;
            }
        }
    }
    if (missing) info.warnings.push_back(std::to_string(missing) + " OSM tiles weren't available" + std::string(o.offline ? " (offline=true uses cached tiles only)" : ""));
    return mosaic;
}

// ---- building the scene ------------------------------------------------------------------

double niceStep(double raw) {
    const double p = std::pow(10.0, std::floor(std::log10(raw)));
    for (double m : {1.0, 2.0, 2.5, 5.0, 10.0}) {
        if (m * p >= raw) return m * p;
    }
    return 10 * p;
}

// decimals enough to tell graticule lines `step` degrees apart
int stepDecimals(double step) {
    int d = 0;
    while (d < 6 && std::abs(step * std::pow(10.0, d) - std::round(step * std::pow(10.0, d))) > 1e-6) ++d;
    return d;
}

std::string degLabel(double v, bool lat, double step) {
    const double a = std::abs(v);
    std::string s = fmt(a, stepDecimals(step)) + "\xC2\xB0";
    if (v != 0) s += lat ? (v > 0 ? "N" : "S") : (v > 0 ? "E" : "W");
    return s;
}

void text(Scene& sc, double x, double y, const std::string& s, double size, Color c, int align, bool halo, bool bold = false) {
    DText t;
    const double w = textWidth(s, size, bold);
    t.x = align == 1 ? x - w / 2 : align == 2 ? x - w : x;
    t.y = y;
    t.text = s;
    t.size = size;
    t.color = c;
    t.halo = halo;
    t.bold = bold;
    sc.ops.push_back(t);
}

void box(Scene& sc, double x, double y, double w, double h, Color fill, std::optional<Color> stroke = std::nullopt) {
    DPath p;
    p.closed = true;
    p.fill = fill;
    p.stroke = stroke;
    p.width = 1;
    p.parts.push_back({{x, y}, {x + w, y}, {x + w, y + h}, {x, y + h}});
    sc.ops.push_back(std::move(p));
}

void line(Scene& sc, std::vector<P> pts, Color c, double w, const std::string& dash = {}) {
    DPath p;
    p.stroke = c;
    p.width = w;
    p.dash = dash;
    p.parts.push_back(std::move(pts));
    sc.ops.push_back(std::move(p));
}

struct Label {
    double x, y;
    std::string text;
    double size;
    Color color;
    bool halo;
    int align;
};

void drawLayer(Scene& sc, const View& v, const Layer& l, std::vector<Label>& labels) {
    const Style& st = l.style;
    const auto classes = styleClasses(l);
    const double op = std::clamp(st.opacity, 0.0, 1.0);
    const double margin = st.width * 2 + st.pointSize + 4;
    // polygons, then lines, then points so markers stay on top
    for (int pass = 0; pass < 3; ++pass) {
        for (const auto& f : l.features) {
            const GeomType want = pass == 0 ? GeomType::Polygon : pass == 1 ? GeomType::Line : GeomType::Point;
            if (f.geom.type != want) continue;
            const int ci = classOf(st, classes, f);
            if (!classes.empty() && ci < 0 && st.mode == "categorized") continue;
            const bool classed = ci >= 0;
            const Color base = classed ? classes[static_cast<std::size_t>(ci)].color : st.fill;
            if (want == GeomType::Point) {
                const double size = classed && classes[static_cast<std::size_t>(ci)].size > 0 ? classes[static_cast<std::size_t>(ci)].size : st.pointSize;
                const Color outline = (!classed && !(st.stroke == st.fill)) ? st.stroke : Color{255, 255, 255, 255};
                for (const auto& c : f.geom.points) {
                    const P p = v.px(c);
                    if (p.x < -size || p.y < -size || p.x > v.W + size || p.y > v.H + size) continue;
                    marker(sc, p.x, p.y, size, st.marker, withAlpha(base, op), withAlpha(outline, op), std::max(0.8, size / 7));
                }
            } else {
                DPath path;
                path.closed = want == GeomType::Polygon;
                path.width = st.width;
                path.dash = st.dash;
                if (want == GeomType::Polygon) {
                    path.fill = withAlpha(base, (classed ? std::max(st.fillOpacity, 0.0) : st.fillOpacity) * op);
                    path.stroke = withAlpha(classed ? darken(base, 0.7) : st.stroke, op);
                    if (st.width <= 0) path.stroke.reset();
                    if (path.fill->a == 0) path.fill.reset();
                } else {
                    path.stroke = withAlpha(classed ? base : st.stroke, op);
                }
                addPaths(v, f.geom, path, margin);
                if (!path.parts.empty()) sc.ops.push_back(std::move(path));
            }
        }
    }
    // a handful of named places reads like a map without asking: label them by name
    std::string field = st.labelField;
    if (field.empty() && l.features.size() <= 100 &&
        std::all_of(l.features.begin(), l.features.end(), [](const Feature& f) { return f.geom.type == GeomType::Point && f.props.count("name"); }))
        field = "name";
    if (field.empty()) return;
    for (const auto& f : l.features) {
        auto it = f.props.find(field);
        if (it == f.props.end() || it->second.isNull() || f.geom.empty()) continue;
        Label lb{0, 0, it->second.text(), st.labelSize, st.labelColor, st.halo, 1};
        if (f.geom.type == GeomType::Point) {
            const P p = v.px(f.geom.points[0]);
            lb.x = p.x + st.pointSize / 2 + 3;
            lb.y = p.y + st.labelSize * 0.35;
            lb.align = 0;
        } else if (f.geom.type == GeomType::Line) {
            const Line* longest = &f.geom.lines[0];
            for (const auto& ln : f.geom.lines) {
                if (ln.size() > longest->size()) longest = &ln;
            }
            const P p = v.px((*longest)[longest->size() / 2]);
            lb.x = p.x;
            lb.y = p.y - 4;
        } else {
            const P p = v.px(labelPoint(f.geom));
            lb.x = p.x;
            lb.y = p.y + st.labelSize * 0.35;
        }
        if (lb.x < 0 || lb.y < 0 || lb.x > v.W || lb.y > v.H) continue;
        labels.push_back(lb);
    }
}

void drawGraticule(Scene& sc, const View& v, const RenderOptions& o) {
    const BBox ll = viewLonLat(v);
    const double span = std::max(ll.width(), ll.height());
    const double step = o.graticuleStep > 0 ? o.graticuleStep : niceStep(span / 6);
    const Color c{0x60, 0x70, 0x80, 110};
    const Color lc{0x44, 0x4c, 0x55, 255};
    int count = 0;
    for (double lon = std::ceil(ll.minX / step) * step; lon <= ll.maxX + 1e-9 && count < 400; lon += step, ++count) {
        std::vector<P> pts;
        for (int k = 0; k <= 60; ++k) pts.push_back(v.px({lon, ll.minY + ll.height() * k / 60}));
        line(sc, pts, c, 0.6);
        // label where it meets the bottom edge
        for (std::size_t k = 1; k < pts.size(); ++k) {
            if ((pts[k - 1].y - v.H) * (pts[k].y - v.H) <= 0 && pts[k].x > 20 && pts[k].x < v.W - 20) {
                text(sc, pts[k].x, v.H - 4.0, degLabel(lon, false, step), 9, lc, 1, true);
                break;
            }
        }
    }
    count = 0;
    for (double lat = std::ceil(ll.minY / step) * step; lat <= ll.maxY + 1e-9 && count < 400; lat += step, ++count) {
        std::vector<P> pts;
        for (int k = 0; k <= 60; ++k) pts.push_back(v.px({ll.minX + ll.width() * k / 60, lat}));
        line(sc, pts, c, 0.6);
        for (std::size_t k = 1; k < pts.size(); ++k) {
            if (pts[k - 1].x * pts[k].x <= 0 && pts[k].y > 14 && pts[k].y < v.H - 14) {
                text(sc, 3, pts[k].y - 2, degLabel(lat, true, step), 9, lc, 0, true);
                break;
            }
        }
    }
}

void drawScaleBar(Scene& sc, const View& v, RenderInfo& info) {
    const double y = v.H - 22.0;
    const double mpp = metresPerPixel(v, v.H / 2.0);  // true at the map's centre
    info.metresPerPixel = metresPerPixel(v, v.H / 2.0);
    if (!(mpp > 0) || !std::isfinite(mpp)) return;
    const double target = mpp * v.W / 4;
    const double p = std::pow(10.0, std::floor(std::log10(target)));
    const double m = target / p;
    const double metres = (m >= 5 ? 5 : m >= 2 ? 2 : 1) * p;
    const double w = metres / mpp;
    const std::string label = metres >= 1000 ? fmt(metres / 1000, 3) + " km" : fmt(metres, 2) + " m";
    const double x = 12;
    box(sc, x - 6, y - 18, w + 12 + textWidth(label, 10, false) + 6, 30, {255, 255, 255, 200});
    const Color black{0x22, 0x22, 0x22, 255};
    for (int i = 0; i < 4; ++i) box(sc, x + w * i / 4, y, w / 4, 5, i % 2 ? Color{255, 255, 255, 255} : black, black);
    text(sc, x, y - 4, "0", 9, black, 1, false);
    text(sc, x + w + 4, y + 6, label, 10, black, 0, false);
}

void drawNorthArrow(Scene& sc, const View& v) {
    const double cx = v.W - 26.0, cy = 40.0;
    const Coord ll = v.lonlat(cx, cy);
    const P a = v.px({ll.x, std::min(89.0, ll.y + 0.01)});
    double ang = std::atan2(a.x - cx, cy - a.y);  // 0 = up
    if (!std::isfinite(ang)) ang = 0;
    auto rot = [&](double x, double y) { return P{cx + x * std::cos(ang) - y * std::sin(ang), cy + x * std::sin(ang) + y * std::cos(ang)}; };
    DPath left, right;
    left.closed = right.closed = true;
    left.fill = Color{0x22, 0x22, 0x22, 255};
    right.fill = Color{255, 255, 255, 255};
    left.stroke = right.stroke = Color{0x22, 0x22, 0x22, 255};
    left.width = right.width = 1;
    left.parts.push_back({rot(0, -16), rot(0, 6), rot(-8, 12)});
    right.parts.push_back({rot(0, -16), rot(8, 12), rot(0, 6)});
    sc.ops.push_back(std::move(right));
    sc.ops.push_back(std::move(left));
    const P t = rot(0, -20);
    text(sc, t.x, t.y, "N", 12, {0x22, 0x22, 0x22, 255}, 1, true, true);
}

void drawLegend(Scene& sc, const Map& map, const RenderOptions& o) {
    struct Row {
        std::string label;
        bool header = false;
        int kind = 0;  // 0 none, 1 polygon, 2 line, 3 point, 4 ramp
        Color color, stroke;
        double size = 8;
        std::string ramp;
        double lo = 0, hi = 0;
        std::string marker = "circle";
    };
    std::vector<Row> rows;
    for (auto it = map.layers.rbegin(); it != map.layers.rend(); ++it) {
        const Layer& l = *it;
        if (!l.visible) continue;
        if (l.grid) {
            if (!l.grid->hasValues() || !l.grid->image.empty()) continue;
            rows.push_back({l.name, true});
            Row r;
            r.kind = 4;
            r.ramp = l.style.rasterRamp;
            r.lo = l.grid->minValue;
            r.hi = l.grid->maxValue;
            rows.push_back(r);
            continue;
        }
        if (l.features.empty()) continue;
        int kind = 0;
        for (const auto& f : l.features) {
            const int k = f.geom.type == GeomType::Polygon ? 1 : f.geom.type == GeomType::Line ? 2 : f.geom.type == GeomType::Point ? 3 : 0;
            if (k && (!kind || k < kind)) kind = k;
        }
        if (!kind) continue;
        const auto classes = styleClasses(l);
        if (classes.empty()) {
            rows.push_back({l.name, false, kind, kind == 2 ? l.style.stroke : l.style.fill, l.style.stroke, l.style.pointSize, {}, 0, 0, l.style.marker});
        } else {
            rows.push_back({l.name + (l.style.field.empty() ? "" : " (" + l.style.field + ")"), true});
            for (const auto& c : classes) rows.push_back({c.label, false, kind, c.color, darken(c.color, 0.7), c.size > 0 ? c.size : l.style.pointSize, {}, 0, 0, l.style.marker});
        }
    }
    if (rows.empty()) return;
    double w = 0;
    for (const auto& r : rows) w = std::max(w, textWidth(r.label, 10, r.header) + (r.header ? 0 : 24));
    w = std::max(w, 120.0) + 16;
    const double rowH = 16;
    double h = 10;
    for (const auto& r : rows) h += r.kind == 4 ? 26 : rowH;
    const double x = 10, y = o.title.empty() ? 10 : 34;
    box(sc, x, y, w, h, {255, 255, 255, 215}, Color{0xbb, 0xbb, 0xbb, 255});
    double cy = y + 6;
    const Color ink{0x22, 0x22, 0x22, 255};
    for (const auto& r : rows) {
        if (r.kind == 4) {
            for (int i = 0; i < 40; ++i) box(sc, x + 8 + (w - 16) * i / 40.0, cy + 2, (w - 16) / 40.0 + 0.5, 8, rampColor(r.ramp, i / 39.0));
            text(sc, x + 8, cy + 21, shortNum(r.lo), 9, ink, 0, false);
            text(sc, x + w - 8, cy + 21, shortNum(r.hi), 9, ink, 2, false);
            cy += 26;
            continue;
        }
        const double mid = cy + rowH / 2;
        if (r.header) {
            text(sc, x + 8, mid + 4, r.label, 10, ink, 0, false, true);
        } else {
            const double sx = x + 8;
            if (r.kind == 1) {
                box(sc, sx, mid - 5, 14, 10, withAlpha(r.color, 0.8), r.stroke);
            } else if (r.kind == 2) {
                line(sc, {{sx, mid}, {sx + 14, mid}}, r.color, 2.5);
            } else if (r.kind == 3) {
                marker(sc, sx + 7, mid, std::min(12.0, r.size), r.marker, r.color, r.stroke, 1);
            }
            text(sc, sx + 22, mid + 4, r.label, 10, ink, 0, false);
        }
        cy += rowH;
    }
}

void placeLabels(Scene& sc, std::vector<Label>& labels) {
    struct R {
        double x0, y0, x1, y1;
    };
    std::vector<R> taken;
    for (const auto& l : labels) {
        const double w = textWidth(l.text, l.size, false);
        const double x = l.align == 1 ? l.x - w / 2 : l.x;
        const R r{x - 2, l.y - l.size, x + w + 2, l.y + l.size * 0.3};
        bool clash = false;
        for (const auto& t : taken) {
            if (r.x0 < t.x1 && t.x0 < r.x1 && r.y0 < t.y1 && t.y0 < r.y1) {
                clash = true;
                break;
            }
        }
        if (clash) continue;
        taken.push_back(r);
        text(sc, l.x, l.y, l.text, l.size, l.color, l.align, l.halo);
    }
}

// ---- map packs: PMTiles basemaps (vector or raster), read from disk -------------------------

// fractional slippy-map zoom of the view, for 256 px tiles
double viewZoom(const View& v) {
    const double world = 2 * kPi * kA;
    const Coord centre = v.lonlat(v.W / 2.0, v.H / 2.0);
    const double upp = metresPerPixel(v, v.H / 2.0) / std::max(0.05, std::cos(centre.y * kDeg));
    return std::log2(world / (256 * upp));
}

struct TileRange {
    int z = 0, x0 = 0, x1 = 0, y0 = 0, y1 = 0;
};

TileRange tileRange(const View& v, int z, int minZoom, int maxTiles) {
    const BBox ll = viewLonLat(v);
    TileRange t;
    for (;; --z) {
        double tx0 = 0, ty0 = 0, tx1 = 0, ty1 = 0;
        lonLatToTile(ll.minX, std::min(ll.maxY, 85.05), z, tx0, ty0);
        lonLatToTile(ll.maxX, std::max(ll.minY, -85.05), z, tx1, ty1);
        const int n = 1 << z;
        t = {z, std::clamp(static_cast<int>(std::floor(tx0)), 0, n - 1), std::clamp(static_cast<int>(std::floor(tx1)), 0, n - 1),
             std::clamp(static_cast<int>(std::floor(ty0)), 0, n - 1), std::clamp(static_cast<int>(std::floor(ty1)), 0, n - 1)};
        const double count = static_cast<double>(t.x1 - t.x0 + 1) * (t.y1 - t.y0 + 1);
        if (count <= maxTiles || z <= minZoom || z == 0) return t;
    }
}

// The tile, or the nearest ancestor the archive has (sparse extracts, overzoom).
std::optional<std::string> tileOrAncestor(const PmtilesReader& r, int& z, int& x, int& y) {
    for (; z >= r.header().minZoom; --z, x >>= 1, y >>= 1) {
        if (auto t = r.tile(z, static_cast<std::uint32_t>(x), static_cast<std::uint32_t>(y))) return t;
        if (z == 0) break;
    }
    return std::nullopt;
}

void vectorPack(Scene& sc, const View& v, const PmtilesReader& r, const TileRange& tr, double styleZoom, RenderInfo& info) {
    const double world = 2 * kPi * kA;
    const bool mercator = v.crs.kind == Crs::WebMercator;
    std::vector<detail::StyledPath> paths;
    std::vector<detail::StyledLabel> styled;
    std::set<std::tuple<int, int, int>> drawn;
    int missing = 0, broken = 0;
    for (int ty = tr.y0; ty <= tr.y1; ++ty) {
        for (int tx = tr.x0; tx <= tr.x1; ++tx) {
            int z = tr.z, x = tx, y = ty;
            std::optional<std::string> bytes;
            try {
                bytes = tileOrAncestor(r, z, x, y);
            } catch (const std::exception& e) {
                if (!broken++) info.warnings.push_back(std::string("a map tile couldn't be read: ") + e.what());
                continue;
            }
            if (!bytes) {
                ++missing;
                continue;
            }
            if (!drawn.insert({z, x, y}).second) continue;
            std::vector<MvtLayer> layers;
            try {
                layers = decodeMvt(*bytes);
            } catch (const std::exception& e) {
                if (!broken++) info.warnings.push_back(std::string("a map tile couldn't be read: ") + e.what());
                continue;
            }
            const double n = std::ldexp(1.0, z);
            const detail::TileToPixels toPx = [&, x, y, n](double lx, double ly) -> std::pair<double, double> {
                const double mx = (x + lx / 4096.0) / n, my = (y + ly / 4096.0) / n;
                if (mercator) return {((mx - 0.5) * world - v.x0) * v.scale, (v.y0 - (0.5 - my) * world) * v.scale};
                const double lon = mx * 360.0 - 180.0, lat = std::atan(std::sinh(kPi * (1 - 2 * my))) / kDeg;
                const P p = v.px({lon, lat});
                return {p.x, p.y};
            };
            detail::styleVectorTile(layers, styleZoom, toPx, v.W, v.H, paths, styled);
            ++info.tilesUsed;
        }
    }
    std::stable_sort(paths.begin(), paths.end(), [](const detail::StyledPath& a, const detail::StyledPath& b) { return a.order < b.order; });
    for (auto& sp : paths) {
        DPath p;
        p.closed = sp.closed;
        p.fill = sp.fill;
        p.stroke = sp.stroke;
        p.width = sp.width;
        p.dash = sp.dash;
        for (auto& part : sp.parts) {
            std::vector<P> pts;
            pts.reserve(part.size());
            for (const auto& [px, py] : part) pts.push_back({px, py});
            p.parts.push_back(std::move(pts));
        }
        sc.ops.push_back(std::move(p));
    }
    std::stable_sort(styled.begin(), styled.end(), [](const detail::StyledLabel& a, const detail::StyledLabel& b) { return a.rank > b.rank; });
    std::vector<Label> labels;
    std::set<std::string> named;  // a place repeats in the buffer of neighbouring tiles
    for (const auto& l : styled) {
        if (labels.size() >= 200) break;
        if (!named.insert(l.text).second) continue;
        labels.push_back({l.x, l.y, l.text, l.size, l.color, true, 1});
    }
    placeLabels(sc, labels);
    if (missing) info.warnings.push_back(std::to_string(missing) + " map tiles aren't in this map pack (the area is outside it); they stay blank");
}

void rasterPack(Scene& sc, const View& v, const PmtilesReader& r, const TileRange& tr, RenderInfo& info) {
    const double world = 2 * kPi * kA;
    const int n = 1 << tr.z;
    std::shared_ptr<Grid> mosaic;
    int T = 256, missing = 0;
    for (int ty = tr.y0; ty <= tr.y1; ++ty) {
        for (int tx = tr.x0; tx <= tr.x1; ++tx) {
            std::optional<std::string> bytes;
            try {
                bytes = r.tile(tr.z, static_cast<std::uint32_t>(tx), static_cast<std::uint32_t>(ty));
            } catch (const std::exception&) {
            }
            if (!bytes) {
                ++missing;
                continue;
            }
            raster::Image t;
            try {
                t = raster::decode(*bytes);
            } catch (const std::exception&) {
                ++missing;
                continue;
            }
            if (!mosaic) {
                T = std::clamp(t.width(), 16, 1024);
                mosaic = std::make_shared<Grid>();
                mosaic->width = (tr.x1 - tr.x0 + 1) * T;
                mosaic->height = (tr.y1 - tr.y0 + 1) * T;
                mosaic->image = raster::Image(mosaic->width, mosaic->height);
                mosaic->crs = Crs::webMercator();
                const double units = world / n;
                mosaic->geo = {units / T, 0, -world / 2 + tr.x0 * units, 0, -units / T, world / 2 - tr.y0 * units};
            }
            const int ox = (tx - tr.x0) * T, oy = (ty - tr.y0) * T;
            for (int y = 0; y < std::min(T, t.height()); ++y) {
                for (int x = 0; x < std::min(T, t.width()); ++x) mosaic->image.set(ox + x, oy + y, t.at(x, y));
            }
            ++info.tilesUsed;
        }
    }
    if (missing) info.warnings.push_back(std::to_string(missing) + " map tiles aren't in this map pack; they stay blank");
    if (!mosaic) return;
    Style st;
    st.opacity = 1;
    if (auto img = rasterize(v, *mosaic, st)) sc.ops.push_back(DImage{0, 0, static_cast<double>(v.W), static_cast<double>(v.H), img});
}

// Draws a .pmtiles basemap; false (with a warning) when it can't, so the caller falls back.
bool pmtilesBasemap(Scene& sc, const View& v, const RenderOptions& o, const fs::path& file, RenderInfo& info) {
    std::shared_ptr<const PmtilesReader> r;
    TilesInfo meta;
    try {
        r = openPmtiles(file);
        meta = describePmtiles(*r);
    } catch (const std::exception& e) {
        info.warnings.push_back("the map file " + file.string() + " can't be used (" + e.what() + "); showing the built-in country outlines");
        return false;
    }
    const PmtilesHeader& h = r->header();
    const bool vector = h.tileType == TileType::Mvt;
    if (!vector && h.tileType != TileType::Png && h.tileType != TileType::Jpeg) {
        info.warnings.push_back(std::string("the map file holds ") + toString(h.tileType) +
                                " tiles, which the maps app can't draw (it reads vector, PNG and JPEG tiles); showing the built-in country outlines");
        return false;
    }
    const double zf = viewZoom(v);
    // Protomaps vector tiles are drawn at 512 px: one zoom level fewer than 256 px raster tiles
    const int want = static_cast<int>(std::lround(zf)) - (vector ? 1 : 0);
    const TileRange tr = tileRange(v, std::clamp(want, h.minZoom, h.maxZoom), h.minZoom, o.maxTiles);
    info.zoom = tr.z;
    info.tileSource = file.string();
    info.attribution = meta.attribution;
    if (vector) {
        sc.background = detail::vectorWaterColor();
        vectorPack(sc, v, *r, tr, std::max(0.0, zf - 1), info);
    } else {
        sc.background = {0xf2, 0xef, 0xe9, 255};
        rasterPack(sc, v, *r, tr, info);
    }
    if (want > h.maxZoom) info.warnings.push_back("zoomed in past the map pack's detail (zoom " + std::to_string(h.maxZoom) + "); a bigger pack shows more");
    return true;
}

bool endsWith(const std::string& s, const std::string& tail) { return s.size() >= tail.size() && s.compare(s.size() - tail.size(), tail.size(), tail) == 0; }

void countriesBasemap(Scene& sc, const View& v, const RenderOptions& o) {
    DPath land;
    land.closed = true;
    land.fill = o.land;
    land.stroke = o.borders;
    land.width = 0.6;
    for (const auto& f : countries()) {
        DPath one = land;
        addPaths(v, f.geom, one, 4);
        if (!one.parts.empty()) sc.ops.push_back(std::move(one));
    }
}

// The built-in cities over the country outlines, biggest first, as many as fit without overlapping.
void placesOverlay(Scene& sc, const View& v) {
    struct Box {
        double x0, y0, x1, y1;
    };
    std::vector<Box> taken;
    int shown = 0;
    for (const auto& f : places()) {
        if (shown >= 40) break;
        if (f.geom.empty()) continue;
        const Coord ll = labelPoint(f.geom);
        if (!v.domain.contains(ll.x, ll.y)) continue;
        const P at = v.px(ll);
        if (at.x < 4 || at.y < 14 || at.x > v.W - 4 || at.y > v.H - 4) continue;
        const auto named = f.props.find("name");
        if (named == f.props.end() || named->second.str.empty()) continue;
        const std::string& name = named->second.str;
        const bool capital = f.props.count("kind") && f.props.at("kind").str == "capital";
        const double size = capital ? 11 : 10;
        const double w = textWidth(name, size, capital);
        Box b{at.x - 3, at.y - size - 2, at.x + 6 + w, at.y + 3};
        if (b.x1 > v.W - 2) b = {at.x - 6 - w, b.y0, at.x + 3, b.y1};  // near the right edge: name on the left
        if (std::any_of(taken.begin(), taken.end(), [&](const Box& o) { return b.x0 < o.x1 && o.x0 < b.x1 && b.y0 < o.y1 && o.y0 < b.y1; })) continue;
        taken.push_back(b);
        DPath dot;
        dot.closed = true;
        dot.fill = Color{0x33, 0x33, 0x33, 255};
        dot.stroke = Color{255, 255, 255, 255};
        dot.width = 1;
        std::vector<P> ring;
        for (int k = 0; k < 12; ++k) ring.push_back({at.x + 2.5 * std::cos(k * kPi / 6), at.y + 2.5 * std::sin(k * kPi / 6)});
        dot.parts.push_back(std::move(ring));
        sc.ops.push_back(std::move(dot));
        const bool left = b.x1 <= at.x + 3;
        text(sc, left ? at.x - 5 : at.x + 5, at.y + 3, name, size, {0x22, 0x22, 0x22, 255}, left ? 2 : 0, true, capital);
        ++shown;
    }
}

// Picks and draws the basemap; returns what was drawn: none, countries, osm or pack.
std::string drawBasemap(Scene& sc, const View& v, const RenderOptions& o, RenderInfo& info) {
    const std::string basemap = lower(trim(o.basemap));
    const std::string url = lower(o.tileUrl);
    std::optional<fs::path> pack;
    if (endsWith(basemap, ".pmtiles")) {
        pack = fs::path(trim(o.basemap));
    } else if ((basemap == "osm" || basemap == "tiles") && endsWith(url, ".pmtiles")) {
        pack = fs::path(o.tileUrl);
    } else if (endsWith(basemap, ".mbtiles") || ((basemap == "osm" || basemap == "tiles") && endsWith(url, ".mbtiles"))) {
        info.warnings.push_back("MBTiles files can't be read directly (they need SQLite, which the maps app leaves out); convert it once with "
                                "'pmtiles convert in.mbtiles out.pmtiles' and use the .pmtiles file. Showing the built-in country outlines");
        countriesBasemap(sc, v, o);
        return "countries";
    } else if (basemap == "auto" || basemap == "pack") {
        pack = bestMapPack();
        if (!pack) {
            if (basemap == "pack")
                info.warnings.push_back("no map pack is installed (an offline edition has them: hub install maps --from <the edition folder>); "
                                        "showing the built-in country outlines");
            countriesBasemap(sc, v, o);
            return "countries";
        }
    }
    if (pack) {
        if (pmtilesBasemap(sc, v, o, *pack, info)) return "pack";
        countriesBasemap(sc, v, o);
        return "countries";
    }
    if (basemap == "osm" || basemap == "tiles") {
        sc.background = {0xf2, 0xef, 0xe9, 255};
        auto g = osmBasemap(v, o, info);
        Style st;
        st.opacity = 1;
        if (auto img = rasterize(v, *g, st)) sc.ops.push_back(DImage{0, 0, static_cast<double>(v.W), static_cast<double>(v.H), img});
        info.attribution = "\xC2\xA9 OpenStreetMap contributors";
        return "osm";
    }
    if (basemap == "countries" || basemap == "land") {
        countriesBasemap(sc, v, o);
        return "countries";
    }
    if (basemap != "none" && !basemap.empty())
        info.warnings.push_back("unknown basemap '" + o.basemap + "' (use auto, countries, none, pack, osm or a .pmtiles file); drawing none");
    return "none";
}

Scene buildScene(const Map& map, const RenderOptions& o, RenderInfo& info) {
    View v = makeView(map, o, info.warnings);
    Scene sc;
    sc.W = v.W;
    sc.H = v.H;
    sc.background = o.background;
    info.width = v.W;
    info.height = v.H;
    info.bbox = viewLonLat(v);
    const bool countriesBase = drawBasemap(sc, v, o, info) == "countries";
    std::vector<Label> labels;
    for (const auto& l : map.layers) {
        if (!l.visible) continue;
        if (l.grid) {
            if (auto img = rasterize(v, *l.grid, l.style)) sc.ops.push_back(DImage{0, 0, static_cast<double>(v.W), static_cast<double>(v.H), img});
        } else {
            drawLayer(sc, v, l, labels);
        }
    }
    if (o.graticule) drawGraticule(sc, v, o);
    if (o.labels) placeLabels(sc, labels);
    if (o.labels && countriesBase) placesOverlay(sc, v);
    if (!o.title.empty()) text(sc, v.W / 2.0, 24, o.title, 18, {0x18, 0x18, 0x18, 255}, 1, true, true);
    if (o.legend) drawLegend(sc, map, o);
    if (o.scaleBar) drawScaleBar(sc, v, info);
    else info.metresPerPixel = metresPerPixel(v, v.H / 2.0);
    if (o.northArrow) drawNorthArrow(sc, v);
    if (!info.attribution.empty()) {
        const double w = textWidth(info.attribution, 9, false);
        box(sc, v.W - w - 8, v.H - 15.0, w + 8, 15, {255, 255, 255, 200});
        text(sc, v.W - 4.0, v.H - 4.0, info.attribution, 9, {0x33, 0x33, 0x33, 255}, 2, false);
    }
    return sc;
}

// ---- raster backend ---------------------------------------------------------------------

void blend(raster::Image& img, int x, int y, Color c, double cover) {
    if (cover <= 0 || !img.contains(x, y)) return;
    const double a = std::min(1.0, cover) * c.a / 255.0;
    std::uint8_t* p = img.pixel(x, y);
    const double da = p[3] / 255.0;
    const double oa = a + da * (1 - a);
    if (oa <= 0) return;
    for (int k = 0; k < 3; ++k) {
        const double sc = k == 0 ? c.r : k == 1 ? c.g : c.b;
        p[k] = static_cast<std::uint8_t>(std::lround((sc * a + p[k] * da * (1 - a)) / oa));
    }
    p[3] = static_cast<std::uint8_t>(std::lround(oa * 255));
}

void fillParts(raster::Image& img, const std::vector<std::vector<P>>& parts, Color c) {
    double minY = 1e18, maxY = -1e18, minX = 1e18, maxX = -1e18;
    struct E {
        double x0, y0, x1, y1;
        int dir;
    };
    std::vector<E> edges;
    for (const auto& r : parts) {
        for (std::size_t i = 0; i < r.size(); ++i) {
            const P& a = r[i];
            const P& b = r[(i + 1) % r.size()];
            minY = std::min(minY, a.y);
            maxY = std::max(maxY, a.y);
            minX = std::min(minX, a.x);
            maxX = std::max(maxX, a.x);
            if (a.y == b.y) continue;
            edges.push_back(a.y < b.y ? E{a.x, a.y, b.x, b.y, 1} : E{b.x, b.y, a.x, a.y, -1});
        }
    }
    if (edges.empty()) return;
    const int W = img.width();
    const int y0 = std::max(0, static_cast<int>(std::floor(minY))), y1 = std::min(img.height() - 1, static_cast<int>(std::ceil(maxY)));
    const int bx0 = std::max(0, static_cast<int>(std::floor(minX))), bx1 = std::min(W - 1, static_cast<int>(std::ceil(maxX)));
    if (bx0 > bx1) return;
    std::sort(edges.begin(), edges.end(), [](const E& a, const E& b) { return a.y0 < b.y0; });
    std::vector<float> cov(static_cast<std::size_t>(W) + 2, 0.0f);
    std::vector<std::pair<double, int>> xs;
    constexpr int kSub = 4;
    for (int y = y0; y <= y1; ++y) {
        std::fill(cov.begin() + bx0, cov.begin() + bx1 + 2, 0.0f);
        bool any = false;
        for (int s = 0; s < kSub; ++s) {
            const double sy = y + (s + 0.5) / kSub;
            xs.clear();
            for (std::size_t k = 0; k < edges.size(); ++k) {
                const E& e = edges[k];
                if (e.y0 > sy) break;
                if (e.y1 <= sy) continue;
                xs.push_back({e.x0 + (sy - e.y0) / (e.y1 - e.y0) * (e.x1 - e.x0), e.dir});
            }
            if (xs.size() < 2) continue;
            std::sort(xs.begin(), xs.end());
            int wind = 0;
            for (std::size_t k = 0; k + 1 < xs.size(); ++k) {
                wind += xs[k].second;
                if (wind == 0) continue;
                const double l = std::clamp(xs[k].first, static_cast<double>(bx0), static_cast<double>(bx1 + 1));
                const double r = std::clamp(xs[k + 1].first, static_cast<double>(bx0), static_cast<double>(bx1 + 1));
                if (r <= l) continue;
                any = true;
                const int il = static_cast<int>(l), ir = static_cast<int>(r);
                const float w = 1.0f / kSub;
                if (il == ir) {
                    cov[static_cast<std::size_t>(il)] += static_cast<float>((r - l) * w);
                } else {
                    cov[static_cast<std::size_t>(il)] += static_cast<float>((il + 1 - l) * w);
                    for (int x = il + 1; x < ir; ++x) cov[static_cast<std::size_t>(x)] += w;
                    if (ir <= bx1) cov[static_cast<std::size_t>(ir)] += static_cast<float>((r - ir) * w);
                }
            }
        }
        if (!any) continue;
        for (int x = bx0; x <= bx1; ++x) {
            if (cov[static_cast<std::size_t>(x)] > 0.002f) blend(img, x, y, c, cov[static_cast<std::size_t>(x)]);
        }
    }
}

std::vector<std::vector<P>> dashed(const std::vector<P>& pts, bool closed, const std::string& dash, double width) {
    if (dash.empty() || pts.size() < 2) {
        std::vector<P> p = pts;
        if (closed && !p.empty()) p.push_back(p.front());
        return {p};
    }
    const double on = dash == "dot" ? std::max(1.0, width) : 4 * std::max(1.0, width), off = dash == "dot" ? 2 * std::max(1.0, width) : 2.5 * std::max(1.0, width);
    std::vector<P> path = pts;
    if (closed) path.push_back(pts.front());
    std::vector<std::vector<P>> out;
    std::vector<P> cur{path[0]};
    bool drawing = true;
    double left = on;
    for (std::size_t i = 1; i < path.size(); ++i) {
        P a = path[i - 1];
        const P b = path[i];
        double seg = std::hypot(b.x - a.x, b.y - a.y);
        while (seg > left) {
            const double t = left / seg;
            const P m{a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t};
            if (drawing) {
                cur.push_back(m);
                out.push_back(cur);
                cur.clear();
            } else {
                cur = {m};
            }
            drawing = !drawing;
            seg -= left;
            a = m;
            left = drawing ? on : off;
        }
        left -= seg;
        if (drawing) cur.push_back(b);
    }
    if (drawing && cur.size() >= 2) out.push_back(cur);
    return out;
}

void strokeParts(raster::Image& img, const std::vector<std::vector<P>>& parts, bool closed, Color c, double width, const std::string& dash) {
    const double hw = std::max(0.35, width / 2);
    double minX = 1e18, minY = 1e18, maxX = -1e18, maxY = -1e18;
    for (const auto& r : parts) {
        for (const auto& p : r) {
            minX = std::min(minX, p.x);
            minY = std::min(minY, p.y);
            maxX = std::max(maxX, p.x);
            maxY = std::max(maxY, p.y);
        }
    }
    const int x0 = std::max(0, static_cast<int>(std::floor(minX - hw - 1))), y0 = std::max(0, static_cast<int>(std::floor(minY - hw - 1)));
    const int x1 = std::min(img.width() - 1, static_cast<int>(std::ceil(maxX + hw + 1))), y1 = std::min(img.height() - 1, static_cast<int>(std::ceil(maxY + hw + 1)));
    if (x0 > x1 || y0 > y1) return;
    const int bw = x1 - x0 + 1;
    std::vector<float> mask(static_cast<std::size_t>(bw) * (y1 - y0 + 1), 0.0f);
    // thin lines get their coverage scaled down instead of growing past their width
    const double alphaScale = width < 0.7 ? width / 0.7 : 1.0;
    for (const auto& r : parts) {
        for (const auto& chain : dashed(r, closed, dash, width)) {
            for (std::size_t i = 0; i + 1 < chain.size() || (chain.size() == 1 && i == 0); ++i) {
                const P a = chain[i];
                const P b = chain.size() == 1 ? chain[0] : chain[i + 1];
                const int sx0 = std::max(x0, static_cast<int>(std::floor(std::min(a.x, b.x) - hw - 1)));
                const int sx1 = std::min(x1, static_cast<int>(std::ceil(std::max(a.x, b.x) + hw + 1)));
                const int sy0 = std::max(y0, static_cast<int>(std::floor(std::min(a.y, b.y) - hw - 1)));
                const int sy1 = std::min(y1, static_cast<int>(std::ceil(std::max(a.y, b.y) + hw + 1)));
                const double dx = b.x - a.x, dy = b.y - a.y, len2 = dx * dx + dy * dy;
                for (int y = sy0; y <= sy1; ++y) {
                    for (int x = sx0; x <= sx1; ++x) {
                        const double px = x + 0.5, py = y + 0.5;
                        double t = len2 > 0 ? ((px - a.x) * dx + (py - a.y) * dy) / len2 : 0;
                        t = std::clamp(t, 0.0, 1.0);
                        const double ex = a.x + t * dx - px, ey = a.y + t * dy - py;
                        const double d = std::sqrt(ex * ex + ey * ey);
                        const double cov = std::clamp(hw + 0.5 - d, 0.0, 1.0);
                        float& m = mask[static_cast<std::size_t>(y - y0) * bw + (x - x0)];
                        if (cov > m) m = static_cast<float>(cov);
                    }
                }
            }
        }
    }
    for (int y = y0; y <= y1; ++y) {
        for (int x = x0; x <= x1; ++x) {
            const float m = mask[static_cast<std::size_t>(y - y0) * bw + (x - x0)];
            if (m > 0.002f) blend(img, x, y, c, m * alphaScale);
        }
    }
}

raster::Image drawRaster(const Scene& sc) {
    raster::Image img(sc.W, sc.H, sc.background);
    for (const auto& op : sc.ops) {
        if (auto* p = std::get_if<DPath>(&op)) {
            if (p->fill && p->closed) fillParts(img, p->parts, *p->fill);
            if (p->stroke && p->width > 0) strokeParts(img, p->parts, p->closed, *p->stroke, p->width, p->dash);
        } else if (auto* t = std::get_if<DText>(&op)) {
            if (t->halo) {
                const Color white{255, 255, 255, 220};
                for (int k = 0; k < 8; ++k) {
                    const double a = k * kPi / 4;
                    pdf::drawText(img, t->x + 1.3 * std::cos(a), t->y + 1.3 * std::sin(a), t->text, fontOf(t->bold), t->size, white);
                }
            }
            pdf::drawText(img, t->x, t->y, t->text, fontOf(t->bold), t->size, t->color);
        } else if (auto* im = std::get_if<DImage>(&op)) {
            raster::blendOnto(img, *im->img, static_cast<int>(im->x), static_cast<int>(im->y), raster::Blend::Normal, 1.0);
        }
    }
    return img;
}

// ---- SVG backend ------------------------------------------------------------------------

std::string svgColor(Color c) {
    char buf[12];
    std::snprintf(buf, sizeof buf, "#%02x%02x%02x", c.r, c.g, c.b);
    return buf;
}

std::string pathData(const std::vector<std::vector<P>>& parts, bool closed) {
    std::string d;
    for (const auto& r : parts) {
        if (r.empty()) continue;
        for (std::size_t i = 0; i < r.size(); ++i) d += (i ? "L" : "M") + fmt(r[i].x, 2) + " " + fmt(r[i].y, 2);
        if (closed) d += "Z";
    }
    return d;
}

std::string drawSvg(const Scene& sc) {
    std::string s = "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"" + std::to_string(sc.W) + "\" height=\"" +
                    std::to_string(sc.H) + "\" viewBox=\"0 0 " + std::to_string(sc.W) + " " + std::to_string(sc.H) + "\">\n";
    s += "<rect width=\"100%\" height=\"100%\" fill=\"" + svgColor(sc.background) + "\"/>\n";
    for (const auto& op : sc.ops) {
        if (auto* p = std::get_if<DPath>(&op)) {
            const std::string d = pathData(p->parts, p->closed);
            if (d.empty()) continue;
            s += "<path d=\"" + d + "\"";
            if (p->fill && p->closed) {
                s += " fill=\"" + svgColor(*p->fill) + "\"";
                if (p->fill->a < 255) s += " fill-opacity=\"" + fmt(p->fill->a / 255.0, 3) + "\"";
            } else {
                s += " fill=\"none\"";
            }
            if (p->stroke && p->width > 0) {
                s += " stroke=\"" + svgColor(*p->stroke) + "\" stroke-width=\"" + fmt(p->width, 2) + "\" stroke-linejoin=\"round\" stroke-linecap=\"round\"";
                if (p->stroke->a < 255) s += " stroke-opacity=\"" + fmt(p->stroke->a / 255.0, 3) + "\"";
                if (p->dash == "dash") s += " stroke-dasharray=\"" + fmt(4 * p->width, 2) + " " + fmt(2.5 * p->width, 2) + "\"";
                if (p->dash == "dot") s += " stroke-dasharray=\"" + fmt(p->width, 2) + " " + fmt(2 * p->width, 2) + "\"";
            }
            s += "/>\n";
        } else if (auto* t = std::get_if<DText>(&op)) {
            s += "<text x=\"" + fmt(t->x, 2) + "\" y=\"" + fmt(t->y, 2) + "\" font-family=\"Helvetica, Arial, sans-serif\" font-size=\"" + fmt(t->size, 2) +
                 "\" fill=\"" + svgColor(t->color) + "\"";
            if (t->bold) s += " font-weight=\"bold\"";
            if (t->halo) s += " paint-order=\"stroke\" stroke=\"#ffffff\" stroke-opacity=\"0.85\" stroke-width=\"2.6\" stroke-linejoin=\"round\"";
            s += ">" + xmlEscape(t->text) + "</text>\n";
        } else if (auto* im = std::get_if<DImage>(&op)) {
            s += "<image x=\"" + fmt(im->x, 2) + "\" y=\"" + fmt(im->y, 2) + "\" width=\"" + fmt(im->w, 2) + "\" height=\"" + fmt(im->h, 2) +
                 "\" href=\"data:image/png;base64," + base64Encode(raster::encodePng(*im->img)) + "\"/>\n";
        }
    }
    return s + "</svg>\n";
}

// ---- PDF backend --------------------------------------------------------------------------

std::string drawPdf(const Scene& sc, double dpi, const std::string& title) {
    pdfdoc::Writer w;
    const double k = 72.0 / std::max(1.0, dpi);
    const double pw = sc.W * k, ph = sc.H * k;
    const int font = pdfdoc::addStandardFont(w, pdfdoc::Standard::Helvetica);
    const int bold = pdfdoc::addStandardFont(w, pdfdoc::Standard::HelveticaBold);
    pdfdoc::UnicodeFallback uni(w);
    std::map<int, std::string> alphas;  // alpha*1000 -> name
    std::vector<std::pair<std::string, int>> images;
    auto gs = [&](double a) {
        const int key = static_cast<int>(std::lround(std::clamp(a, 0.0, 1.0) * 1000));
        auto it = alphas.find(key);
        if (it != alphas.end()) return it->second;
        const std::string name = "GA" + std::to_string(alphas.size());
        alphas[key] = name;
        return name;
    };
    auto rgb = [](Color c) { return pdfdoc::num(c.r / 255.0) + " " + pdfdoc::num(c.g / 255.0) + " " + pdfdoc::num(c.b / 255.0); };
    std::string cs;
    // px coordinates, y down
    cs += pdfdoc::num(k) + " 0 0 " + pdfdoc::num(-k) + " 0 " + pdfdoc::num(ph) + " cm\n";
    cs += rgb(sc.background) + " rg 0 0 " + std::to_string(sc.W) + " " + std::to_string(sc.H) + " re f\n";
    cs += "1 J 1 j\n";
    for (const auto& op : sc.ops) {
        if (auto* p = std::get_if<DPath>(&op)) {
            std::string d;
            for (const auto& r : p->parts) {
                if (r.empty()) continue;
                for (std::size_t i = 0; i < r.size(); ++i) d += pdfdoc::num(r[i].x) + " " + pdfdoc::num(r[i].y) + (i ? " l\n" : " m\n");
                if (p->closed) d += "h\n";
            }
            if (d.empty()) continue;
            if (p->fill && p->closed) cs += "q /" + gs(p->fill->a / 255.0) + " gs " + rgb(*p->fill) + " rg\n" + d + "f\nQ\n";
            if (p->stroke && p->width > 0) {
                cs += "q /" + gs(p->stroke->a / 255.0) + " gs " + rgb(*p->stroke) + " RG " + pdfdoc::num(p->width) + " w";
                if (p->dash == "dash") cs += " [" + pdfdoc::num(4 * p->width) + " " + pdfdoc::num(2.5 * p->width) + "] 0 d";
                if (p->dash == "dot") cs += " [" + pdfdoc::num(p->width) + " " + pdfdoc::num(2 * p->width) + "] 0 d";
                cs += "\n" + d + "S\nQ\n";
            }
        } else if (auto* t = std::get_if<DText>(&op)) {
            const std::string fr = t->bold ? "F2" : "F1";
            const std::string f = "/" + fr + " ";
            const std::string pos = "1 0 0 -1 " + pdfdoc::num(t->x) + " " + pdfdoc::num(t->y) + " Tm ";
            const std::string shown = uni.show(t->text, fr, t->size);
            if (t->halo) cs += "q /" + gs(0.85) + " gs 1 1 1 RG 2.6 w BT 1 Tr " + f + pdfdoc::num(t->size) + " Tf " + pos + shown + " ET Q\n";
            cs += "q /" + gs(t->color.a / 255.0) + " gs " + rgb(t->color) + " rg BT " + f + pdfdoc::num(t->size) + " Tf " + pos + shown + " ET Q\n";
        } else if (auto* im = std::get_if<DImage>(&op)) {
            const int id = pdfdoc::addImage(w, *im->img);
            const std::string name = "Im" + std::to_string(images.size());
            images.push_back({name, id});
            cs += "q " + pdfdoc::num(im->w) + " 0 0 " + pdfdoc::num(-im->h) + " " + pdfdoc::num(im->x) + " " + pdfdoc::num(im->y + im->h) + " cm /" + name + " Do Q\n";
        }
    }
    const int content = w.addStream("", cs);
    std::string res = "<< /Font << /F1 " + std::to_string(font) + " 0 R /F2 " + std::to_string(bold) + " 0 R" + uni.resource() + " >>";
    if (!alphas.empty()) {
        res += " /ExtGState <<";
        for (const auto& [a, name] : alphas) res += " /" + name + " << /Type /ExtGState /ca " + pdfdoc::num(a / 1000.0) + " /CA " + pdfdoc::num(a / 1000.0) + " >>";
        res += " >>";
    }
    if (!images.empty()) {
        res += " /XObject <<";
        for (const auto& [name, id] : images) res += " /" + name + " " + std::to_string(id) + " 0 R";
        res += " >>";
    }
    res += " >>";
    const int pages = w.reserve();
    const int page = w.add("<< /Type /Page /Parent " + std::to_string(pages) + " 0 R /MediaBox [0 0 " + pdfdoc::num(pw) + " " + pdfdoc::num(ph) +
                           "] /Contents " + std::to_string(content) + " 0 R /Resources " + res + " >>");
    w.set(pages, "<< /Type /Pages /Kids [" + std::to_string(page) + " 0 R] /Count 1 >>");
    const int catalog = w.add("<< /Type /Catalog /Pages " + std::to_string(pages) + " 0 R >>");
    uni.finish();
    const int info = w.add("<< /Producer (wetwaresuite maps) /Title " + pdfdoc::infoText(title.empty() ? "Map" : title) + " >>");
    return w.finish(catalog, info);
}

}  // namespace

// ---- tiles ----------------------------------------------------------------------------------

void lonLatToTile(double lon, double lat, int zoom, double& tx, double& ty) {
    const double n = std::ldexp(1.0, zoom);
    lat = std::clamp(lat, -85.05112878, 85.05112878);
    tx = (lon + 180.0) / 360.0 * n;
    ty = (1.0 - std::asinh(std::tan(lat * kDeg)) / kPi) / 2.0 * n;
}

void tileToLonLat(double tx, double ty, int zoom, double& lon, double& lat) {
    const double n = std::ldexp(1.0, zoom);
    lon = tx / n * 360.0 - 180.0;
    lat = std::atan(std::sinh(kPi * (1 - 2 * ty / n))) / kDeg;
}

fs::path defaultTileCache() {
#ifdef _WIN32
    std::string base = envVar("LOCALAPPDATA");
    if (base.empty()) base = envVar("APPDATA");
    fs::path root = base.empty() ? fs::temp_directory_path() : fs::path(base);
    return root / "wetwaresuite" / "cache" / "tiles";
#elif defined(__APPLE__)
    const std::string home = envVar("HOME");
    return (home.empty() ? fs::temp_directory_path() : fs::path(home) / "Library" / "Caches") / "wetwaresuite" / "tiles";
#else
    const std::string xdg = envVar("XDG_CACHE_HOME");
    if (!xdg.empty()) return fs::path(xdg) / "wetwaresuite" / "tiles";
    const std::string home = envVar("HOME");
    return (home.empty() ? fs::temp_directory_path() : fs::path(home) / ".cache") / "wetwaresuite" / "tiles";
#endif
}

std::string renderProfile(const std::vector<std::pair<double, double>>& prof, int width, int height, const std::string& format,
                          const std::string& title) {
    Scene sc;
    sc.W = std::clamp(width, 200, 6000);
    sc.H = std::clamp(height, 120, 4000);
    sc.background = {255, 255, 255, 255};
    const double left = 56, right = 16, top = title.empty() ? 16 : 36, bottom = 36;
    const double pw = sc.W - left - right, ph = sc.H - top - bottom;
    double maxD = 0, lo = 1e300, hi = -1e300;
    for (const auto& [d, e] : prof) {
        maxD = std::max(maxD, d);
        lo = std::min(lo, e);
        hi = std::max(hi, e);
    }
    if (prof.empty()) lo = hi = 0;
    if (hi - lo < 10) {
        const double mid = (hi + lo) / 2;
        lo = mid - 5;
        hi = mid + 5;
    }
    const double ystep = niceStep((hi - lo) / 5);
    lo = std::floor(lo / ystep) * ystep;
    hi = std::ceil(hi / ystep) * ystep;
    const bool km = maxD >= 2000;
    const double xstep = niceStep(std::max(1.0, maxD) / 6);
    auto X = [&](double d) { return left + (maxD > 0 ? d / maxD : 0) * pw; };
    auto Y = [&](double e) { return top + (1 - (e - lo) / (hi - lo)) * ph; };
    const Color grid{0xdd, 0xdd, 0xdd, 255}, ink{0x33, 0x33, 0x33, 255};
    for (double e = lo; e <= hi + 1e-9; e += ystep) {
        line(sc, {{left, Y(e)}, {left + pw, Y(e)}}, grid, 1);
        text(sc, left - 6, Y(e) + 4, fmt(e, 0) + " m", 10, ink, 2, false);
    }
    for (double d = 0; d <= maxD + 1e-9 && xstep > 0; d += xstep) {
        line(sc, {{X(d), top}, {X(d), top + ph}}, grid, 1);
        text(sc, X(d), top + ph + 16, km ? fmt(d / 1000, 1) + " km" : fmt(d, 0) + " m", 10, ink, 1, false);
    }
    if (prof.size() >= 2) {
        DPath area;
        area.closed = true;
        area.fill = Color{0x2b, 0x5c, 0xab, 70};
        std::vector<P> pts{{X(prof.front().first), Y(lo)}};
        std::vector<P> ln;
        for (const auto& [d, e] : prof) {
            pts.push_back({X(d), Y(e)});
            ln.push_back({X(d), Y(e)});
        }
        pts.push_back({X(prof.back().first), Y(lo)});
        area.parts.push_back(pts);
        sc.ops.push_back(std::move(area));
        line(sc, ln, {0x2b, 0x5c, 0xab, 255}, 2);
    }
    line(sc, {{left, top}, {left, top + ph}, {left + pw, top + ph}}, ink, 1);
    if (!title.empty()) text(sc, sc.W / 2.0, 22, title, 14, ink, 1, false, true);
    if (format == "svg") return drawSvg(sc);
    return raster::encodePng(drawRaster(sc));
}

raster::Image renderImage(const Map& map, const RenderOptions& o, RenderInfo* info) {
    RenderInfo local;
    RenderInfo& in = info ? *info : local;
    return drawRaster(buildScene(map, o, in));
}

std::string renderMap(const Map& map, const RenderOptions& o, const std::string& format, RenderInfo* info) {
    RenderInfo local;
    RenderInfo& in = info ? *info : local;
    const Scene sc = buildScene(map, o, in);
    if (format == "svg") return drawSvg(sc);
    if (format == "pdf") return drawPdf(sc, o.dpi, o.title.empty() ? map.title : o.title);
    if (format == "jpg" || format == "jpeg") return raster::encodeJpeg(drawRaster(sc), 90);
    return raster::encodePng(drawRaster(sc));
}

// ---- documents --------------------------------------------------------------------------

std::string MapDoc::summary() const {
    std::size_t features = 0, rasters = 0;
    for (const auto& l : map.layers) {
        features += l.features.size();
        rasters += l.grid ? 1 : 0;
    }
    std::string s = std::to_string(map.layers.size()) + " layer" + (map.layers.size() == 1 ? "" : "s") + ", " + std::to_string(features) + " feature" +
                    (features == 1 ? "" : "s");
    if (rasters) s += ", " + std::to_string(rasters) + " raster" + (rasters == 1 ? "" : "s");
    if (!map.title.empty()) s += ", \"" + map.title + "\"";
    if (!path.empty()) s += ", " + path;
    return s;
}

RenderOptions previewOptions(const Map& map) {
    RenderOptions o;
    o.width = 800;
    if (map.viewAspect > 0) {
        // the window's shape, about a thousand px on the long side
        const double a = std::clamp(map.viewAspect, 0.2, 5.0);
        o.width = a >= 1 ? 1000 : static_cast<int>(std::lround(1000 * a));
        o.height = a >= 1 ? static_cast<int>(std::lround(1000 / a)) : 1000;
    }
    o.title = map.title;
    return o;
}

Coord pixelToLonLat(const Map& map, const RenderOptions& options, double x, double y) {
    std::vector<std::string> warnings;
    const View v = makeView(map, options, warnings);
    return v.lonlat(x, y);
}

Coord lonLatToPixel(const Map& map, const RenderOptions& options, double lon, double lat) {
    std::vector<std::string> warnings;
    const View v = makeView(map, options, warnings);
    const P p = v.px({lon, lat});
    return {p.x, p.y};
}

BBox shownArea(const Map& map, const RenderOptions& options, int* width, int* height) {
    std::vector<std::string> warnings;
    const View v = makeView(map, options, warnings);
    if (width) *width = v.W;
    if (height) *height = v.H;
    return viewLonLat(v);
}

double renderMetresPerPixel(const Map& map, const RenderOptions& options) {
    std::vector<std::string> warnings;
    const View v = makeView(map, options, warnings);
    return metresPerPixel(v, v.H / 2.0);
}

bool MapDoc::setViewShape(int width, int height) {
    if (width < 16 || height < 16) return false;
    const double a = static_cast<double>(width) / height;
    if (map.viewAspect > 0 && std::abs(a - map.viewAspect) < 0.03 * a) return false;
    map.viewAspect = a;
    return true;
}

std::unique_ptr<Attachment> MapDoc::preview() const {
    const RenderOptions o = previewOptions(map);
    return std::make_unique<Attachment>(Attachment{"image/png", raster::encodePng(renderImage(map, o)), "preview.png"});
}

Result MapEdit::apply() {
    before_ = doc_->map;
    Result r;
    try {
        r = fn_(*doc_);
    } catch (const std::exception& e) {
        r = Result::failure(e.what());
    }
    if (!r.ok) revert();
    return r;
}

void MapEdit::revert() { doc_->map = before_; }

}  // namespace wetwaresuite::maps
