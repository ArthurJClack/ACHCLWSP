// cad.drawing: engineering drawings from a part or assembly. Standard views
// (front, top, right, ... and iso) are projected from the solid: sharp edges
// and silhouettes, split into visible and hidden runs by ray casting, laid out
// in third- or first-angle projection on a sheet with overall dimensions,
// hole diameters, centre marks, thread callouts, a parts list for assemblies
// and a title block. The result is a blueprint drawing (edit it further with
// blueprint.*) and exports to PDF, SVG, DXF or PNG.
#include <algorithm>
#include <chrono>
#include <cmath>
#include <ctime>
#include <map>

#include "wetwaresuite/blueprint/blueprint.hpp"
#include "wetwaresuite/cad/assembly.hpp"
#include "wetwaresuite/mesh/csg.hpp"
#include "wetwaresuite/mesh/primitives.hpp"
#include "wetwaresuite/mesh/topology.hpp"
#include "cmdutil.hpp"

namespace wetwaresuite::cad {

using mesh::Mesh;
using mesh::Vec3;
using P2 = blueprint::Vec2;

namespace {

struct ViewDef {
    std::string name;
    Vec3 r, u, d;  // screen right, screen up, looking direction
};

bool viewByName(const std::string& n, ViewDef& v) {
    v.name = n;
    if (n == "front") v = {n, {1, 0, 0}, {0, 0, 1}, {0, 1, 0}};
    else if (n == "back") v = {n, {-1, 0, 0}, {0, 0, 1}, {0, -1, 0}};
    else if (n == "top") v = {n, {1, 0, 0}, {0, 1, 0}, {0, 0, -1}};
    else if (n == "bottom") v = {n, {1, 0, 0}, {0, -1, 0}, {0, 0, 1}};
    else if (n == "right") v = {n, {0, 1, 0}, {0, 0, 1}, {-1, 0, 0}};
    else if (n == "left") v = {n, {0, -1, 0}, {0, 0, 1}, {1, 0, 0}};
    else if (n == "iso") v = {n, mesh::normalize(Vec3{1, 1, 0}), mesh::normalize(Vec3{-1, 1, 2}), mesh::normalize(Vec3{-1, 1, -1})};
    else return false;
    return true;
}

struct Seg {
    P2 a, b;
    bool hidden = false;
};

// Visible and hidden outline of the mesh seen along v.d, in view coordinates.
std::vector<Seg> projectEdges(const Mesh& in, const ViewDef& v, bool withHidden) {
    Mesh m;
    m.positions = in.positions;
    m.indices = in.indices;
    const double diag = std::max(mesh::bounds(m).diagonal(), 1e-9);
    mesh::weld(m, diag * 1e-9);
    const std::size_t nt = m.triangleCount();
    std::vector<Vec3> fn(nt);
    for (std::size_t t = 0; t < nt; ++t) fn[t] = mesh::normalize(m.faceNormal(t));
    std::map<std::pair<std::uint32_t, std::uint32_t>, std::vector<std::size_t>> edges;
    for (std::size_t t = 0; t < nt; ++t) {
        if (mesh::length(fn[t]) == 0) continue;
        for (int k = 0; k < 3; ++k) {
            std::uint32_t a = m.indices[t * 3 + static_cast<std::size_t>(k)], b = m.indices[t * 3 + static_cast<std::size_t>((k + 1) % 3)];
            if (a > b) std::swap(a, b);
            edges[{a, b}].push_back(t);
        }
    }
    const double creaseCos = std::cos(20.0 * 3.14159265358979323846 / 180);
    auto to2 = [&](const Vec3& p) { return P2{mesh::dot(p, v.r), mesh::dot(p, v.u)}; };
    // a coarse grid over the projected triangles, for the visibility rays
    blueprint::Box box;
    for (const auto& p : m.positions) box.add(to2(p));
    const int G = 48;
    const double gw = std::max(box.width(), 1e-9) / G, gh = std::max(box.height(), 1e-9) / G;
    std::vector<std::vector<std::uint32_t>> grid(static_cast<std::size_t>(G * G));
    if (withHidden) {
        for (std::size_t t = 0; t < nt; ++t) {
            blueprint::Box tb;
            for (int k = 0; k < 3; ++k) tb.add(to2(m.corner(t, k)));
            const int x0 = std::clamp(static_cast<int>((tb.x0 - box.x0) / gw), 0, G - 1), x1 = std::clamp(static_cast<int>((tb.x1 - box.x0) / gw), 0, G - 1);
            const int y0 = std::clamp(static_cast<int>((tb.y0 - box.y0) / gh), 0, G - 1), y1 = std::clamp(static_cast<int>((tb.y1 - box.y0) / gh), 0, G - 1);
            for (int y = y0; y <= y1; ++y) {
                for (int x = x0; x <= x1; ++x) grid[static_cast<std::size_t>(y * G + x)].push_back(static_cast<std::uint32_t>(t));
            }
        }
    }
    const Vec3 toViewer = v.d * -1;
    const double eps = diag * 1e-7;
    auto hiddenAt = [&](const Vec3& p) {
        const P2 q = to2(p);
        const int x = std::clamp(static_cast<int>((q.x - box.x0) / gw), 0, G - 1), y = std::clamp(static_cast<int>((q.y - box.y0) / gh), 0, G - 1);
        const Vec3 o = p + toViewer * eps;
        for (std::uint32_t t : grid[static_cast<std::size_t>(y * G + x)]) {
            const double hit = mesh::rayTriangle(o, toViewer, m.corner(t, 0), m.corner(t, 1), m.corner(t, 2));
            if (hit > eps) return true;
        }
        return false;
    };
    std::vector<Seg> out;
    for (const auto& [key, tris] : edges) {
        bool draw = tris.size() != 2;
        if (!draw) {
            const Vec3 n1 = fn[tris[0]], n2 = fn[tris[1]];
            const double s1 = mesh::dot(n1, toViewer), s2 = mesh::dot(n2, toViewer);
            draw = mesh::dot(n1, n2) < creaseCos || (s1 > 1e-9) != (s2 > 1e-9);
        }
        if (!draw) continue;
        const Vec3 a = m.positions[key.first], b = m.positions[key.second];
        const P2 a2 = to2(a), b2 = to2(b);
        if (blueprint::length(b2 - a2) < diag * 1e-9) continue;  // seen end-on
        if (!withHidden) {
            out.push_back({a2, b2, false});
            continue;
        }
        const int n = std::clamp(static_cast<int>(mesh::length(b - a) / (diag / 150)), 2, 64);
        std::vector<bool> state(static_cast<std::size_t>(n + 1));
        for (int i = 0; i <= n; ++i) {
            const double t = (i == 0 ? 0.002 : i == n ? 0.998 : static_cast<double>(i) / n);
            state[static_cast<std::size_t>(i)] = hiddenAt(mesh::lerp(a, b, t));
        }
        double start = 0;
        for (int i = 1; i <= n; ++i) {
            if (state[static_cast<std::size_t>(i)] == state[static_cast<std::size_t>(i - 1)] && i < n) continue;
            double end = 1;
            if (state[static_cast<std::size_t>(i)] != state[static_cast<std::size_t>(i - 1)]) {
                double lo = static_cast<double>(i - 1) / n, hi = static_cast<double>(i) / n;
                for (int k = 0; k < 8; ++k) {
                    const double mid = (lo + hi) / 2;
                    (hiddenAt(mesh::lerp(a, b, mid)) == state[static_cast<std::size_t>(i - 1)] ? lo : hi) = mid;
                }
                end = (lo + hi) / 2;
            }
            out.push_back({to2(mesh::lerp(a, b, start)), to2(mesh::lerp(a, b, end)), state[static_cast<std::size_t>(i - 1)]});
            start = end;
            if (i == n && end < 1) out.push_back({to2(mesh::lerp(a, b, end)), b2, state[static_cast<std::size_t>(n)]});
        }
    }
    // merge overlapping collinear pieces; a hidden piece under a visible one goes
    struct Item {
        double angle, offset, t0, t1;
        P2 dir;
        bool hidden;
    };
    std::vector<Item> items;
    const double tol = diag * 1e-6;
    for (const auto& s : out) {
        P2 d = s.b - s.a;
        const double l = blueprint::length(d);
        if (l < tol) continue;
        d = d / l;
        if (d.x < -1e-12 || (std::fabs(d.x) <= 1e-12 && d.y < 0)) d = d * -1;
        double ang = std::atan2(d.y, d.x);
        if (ang < 0) ang += 3.14159265358979323846;
        const double off = blueprint::cross(d, s.a);
        double t0 = blueprint::dot(s.a, d), t1 = blueprint::dot(s.b, d);
        if (t0 > t1) std::swap(t0, t1);
        items.push_back({ang, off, t0, t1, d, s.hidden});
    }
    std::sort(items.begin(), items.end(), [](const Item& x, const Item& y) { return x.angle != y.angle ? x.angle < y.angle : x.offset < y.offset; });
    std::vector<Seg> merged;
    for (std::size_t i = 0; i < items.size();) {
        std::size_t j = i + 1;
        while (j < items.size() && std::fabs(items[j].angle - items[i].angle) < 1e-7 && std::fabs(items[j].offset - items[j - 1].offset) < tol) ++j;
        const P2 d = items[i].dir;
        const P2 nrm{-d.y, d.x};
        const double off = items[i].offset;
        auto unite = [](std::vector<std::pair<double, double>> v) {
            std::sort(v.begin(), v.end());
            std::vector<std::pair<double, double>> o;
            for (const auto& iv : v) {
                if (!o.empty() && iv.first <= o.back().second + 1e-12) o.back().second = std::max(o.back().second, iv.second);
                else o.push_back(iv);
            }
            return o;
        };
        std::vector<std::pair<double, double>> vis, hid;
        for (std::size_t k = i; k < j; ++k) (items[k].hidden ? hid : vis).push_back({items[k].t0, items[k].t1});
        vis = unite(vis);
        hid = unite(hid);
        auto emit = [&](double t0, double t1, bool hidden) {
            if (t1 - t0 > tol) merged.push_back({d * t0 + nrm * off, d * t1 + nrm * off, hidden});
        };
        for (const auto& iv : vis) emit(iv.first, iv.second, false);
        for (auto iv : hid) {
            for (const auto& w : vis) {
                if (w.second <= iv.first || w.first >= iv.second) continue;
                if (w.first > iv.first) emit(iv.first, w.first, true);
                iv.first = std::max(iv.first, w.second);
                if (iv.first >= iv.second) break;
            }
            if (iv.first < iv.second) emit(iv.first, iv.second, true);
        }
        i = j;
    }
    return merged;
}

blueprint::Unit unitOf(const std::string& u) {
    if (u == "cm") return blueprint::Unit::Cm;
    if (u == "m") return blueprint::Unit::M;
    if (u == "in") return blueprint::Unit::In;
    return blueprint::Unit::Mm;
}

struct Placed {
    ViewDef view;
    std::vector<Seg> segs;
    blueprint::Box box;  // view coordinates
    P2 at;               // where the box's lower-left corner goes on the drawing
    std::vector<Seg> hatch;  // section views: the cut faces' hatching
    std::string label;       // instead of the view's name
};

// The loops where a plane cuts the mesh (n.p = offset), in 3D.
std::vector<std::vector<Vec3>> planeLoops(const Mesh& m, const Vec3& n, double offset) {
    std::vector<std::pair<Vec3, Vec3>> segs;
    for (std::size_t t = 0; t < m.triangleCount(); ++t) {
        Vec3 pts[3] = {m.corner(t, 0), m.corner(t, 1), m.corner(t, 2)};
        double d[3];
        for (int k = 0; k < 3; ++k) d[k] = mesh::dot(pts[k], n) - offset;
        std::vector<Vec3> cut;
        for (int k = 0; k < 3; ++k) {
            const int j = (k + 1) % 3;
            if ((d[k] > 0) != (d[j] > 0)) cut.push_back(mesh::lerp(pts[k], pts[j], d[k] / (d[k] - d[j])));
        }
        if (cut.size() == 2) segs.push_back({cut[0], cut[1]});
    }
    std::vector<std::vector<Vec3>> loops;
    const double tol = 1e-7 * (mesh::bounds(m).diagonal() + 1);
    std::vector<char> used(segs.size(), 0);
    for (std::size_t i = 0; i < segs.size(); ++i) {
        if (used[i]) continue;
        used[i] = 1;
        std::vector<Vec3> loop{segs[i].first, segs[i].second};
        for (bool grew = true; grew;) {
            grew = false;
            for (std::size_t j = 0; j < segs.size(); ++j) {
                if (used[j]) continue;
                if (mesh::length(segs[j].first - loop.back()) < tol) loop.push_back(segs[j].second);
                else if (mesh::length(segs[j].second - loop.back()) < tol) loop.push_back(segs[j].first);
                else continue;
                used[j] = 1;
                grew = true;
            }
        }
        if (loop.size() >= 4) loops.push_back(std::move(loop));
    }
    return loops;
}

// 45 degree hatching inside the loops (even-odd, so holes stay clear), `spacing` apart.
std::vector<Seg> hatchLoops(const std::vector<std::vector<P2>>& loops, double spacing) {
    std::vector<Seg> out;
    if (loops.empty() || !(spacing > 0)) return out;
    // in coordinates turned 45 degrees: u along the hatch lines, w across them
    auto toUW = [](const P2& p) { return P2{(p.x + p.y) * 0.70710678, (p.y - p.x) * 0.70710678}; };
    auto fromUW = [](const P2& q) { return P2{(q.x - q.y) * 0.70710678, (q.x + q.y) * 0.70710678}; };
    double w0 = 1e300, w1 = -1e300;
    for (const auto& l : loops)
        for (const auto& p : l) {
            const double w = toUW(p).y;
            w0 = std::min(w0, w), w1 = std::max(w1, w);
        }
    if ((w1 - w0) / spacing > 4000) spacing = (w1 - w0) / 4000;
    for (double w = std::ceil(w0 / spacing) * spacing; w <= w1; w += spacing) {
        std::vector<double> xs;
        for (const auto& l : loops)
            for (std::size_t i = 0; i < l.size(); ++i) {
                const P2 a = toUW(l[i]), b = toUW(l[(i + 1) % l.size()]);
                if ((a.y > w) != (b.y > w)) xs.push_back(a.x + (b.x - a.x) * (w - a.y) / (b.y - a.y));
            }
        std::sort(xs.begin(), xs.end());
        for (std::size_t i = 0; i + 1 < xs.size(); i += 2) out.push_back({fromUW({xs[i], w}), fromUW({xs[i + 1], w}), false});
    }
    return out;
}

// The part of segment ab inside the circle (c, r), if any.
bool clipToCircle(P2& a, P2& b, const P2& c, double r) {
    const P2 d = b - a, f = a - c;
    const double A = blueprint::dot(d, d), B = 2 * blueprint::dot(f, d), C = blueprint::dot(f, f) - r * r;
    if (A < 1e-30) return C <= 0;
    const double disc = B * B - 4 * A * C;
    if (disc < 0) return false;
    const double sq = std::sqrt(disc);
    const double t0 = std::max(0.0, (-B - sq) / (2 * A)), t1 = std::min(1.0, (-B + sq) / (2 * A));
    if (t0 >= t1) return false;
    const P2 a0 = a;
    a = a0 + d * t0;
    b = a0 + d * t1;
    return true;
}

std::string today() {
    const std::time_t t = std::time(nullptr);
    char buf[16];
    std::strftime(buf, sizeof buf, "%Y-%m-%d", std::localtime(&t));
    return buf;
}

}  // namespace

void registerDrawingCommands(Session& session) {
    session.registry().add(
        {"cad.drawing", "Make Drawing",
         "Make an engineering drawing of a part or assembly: standard views projected from the model (views=front,top,"
         "right,iso; also left, back, bottom) with hidden lines dashed, laid out in third-angle (US) or first-angle "
         "(ISO) projection, overall dimensions, hole and circle diameters with centre marks, thread callouts, a parts "
         "list for assemblies and a title block, on a sheet at a standard scale. It opens as a blueprint drawing (edit "
         "it with blueprint.*) and path= exports .pdf, .svg, .dxf or .png. section= adds a hatched section view (with its "
         "cutting line on another view) and detail= an enlarged detail circle.",
         {{"doc", ParamType::String, "id of an open part (cad1) or assembly (asm1)", true},
          cmd::str("path", "export to this file: .pdf, .svg, .dxf or .png (optional)"),
          cmd::str("views", "comma-separated views", false, "front,top,right,iso"),
          {"projection", ParamType::String, "view layout", false, {"third", "first"}, std::string("third")},
          cmd::str("scale", "drawing scale like 1:2, or auto to fit the sheet", false, "auto"),
          cmd::str("paper", "sheet size: A4, A3, A2, A1, A0, letter, tabloid", false, "A3"), cmd::str("title", "title block title"),
          cmd::str("author", "drawn by"), cmd::boolean("hidden", "draw hidden lines (dashed)", true),
          cmd::boolean("dims", "add dimensions, centre marks and callouts", true),
          cmd::str("section", "add a section view cut by a plane: x=20, y=0 or z=5 (model coordinates), hatched where it cuts"),
          cmd::str("detail", "add an enlarged detail of a view: view:a,b,radius, e.g. front:20,5,8 (a, b along the view's right and up)"),
          {"detail_scale", ParamType::Number, "how much the detail is enlarged", false, {}, 2.0}},
         false},
        [&session](const Params& p) -> std::unique_ptr<Action> {
            return std::make_unique<FunctionAction>([&session, p] {
                try {
                    const std::string id = stringParam(p, "doc");
                    auto doc = session.workspace().getAs<mesh::MeshDoc>(id);
                    if (!doc) return Result::failure("no open part or assembly called '" + id + "'");
                    if (doc->mesh.empty()) return Result::failure(id + " is empty; nothing to draw");
                    auto cad = std::dynamic_pointer_cast<CadDoc>(doc);
                    auto assembly = std::dynamic_pointer_cast<AssemblyDoc>(doc);
                    const std::string units = cad ? cad->units : assembly ? assembly->units : "mm";
                    auto d = std::make_shared<blueprint::Drawing>();
                    d->units = unitOf(units);
                    d->dims.precision = units == "mm" ? 1 : 2;
                    d->paper = stringParam(p, "paper", "A3");
                    if (!blueprint::findPaper(d->paper)) return Result::failure("unknown paper size '" + d->paper + "'");
                    d->landscape = true;
                    auto& vis = d->ensureLayer("VISIBLE");
                    vis.weight = 0.5;
                    auto& hid = d->ensureLayer("HIDDEN");
                    hid.weight = 0.25;
                    hid.linetype = blueprint::LineType::Hidden;
                    hid.color = {90, 90, 90, 255};
                    auto& cen = d->ensureLayer("CENTER");
                    cen.weight = 0.18;
                    cen.linetype = blueprint::LineType::Center;
                    cen.color = {60, 60, 160, 255};
                    auto& dim = d->ensureLayer("DIMS");
                    dim.weight = 0.18;
                    const bool hidden = boolParam(p, "hidden", true), dims = boolParam(p, "dims", true);
                    std::vector<Placed> views;
                    for (const auto& name : splitList(stringParam(p, "views", "front,top,right,iso"))) {
                        ViewDef v;
                        if (!viewByName(name, v)) return Result::failure("unknown view '" + name + "' (front, back, top, bottom, left, right, iso)");
                        Placed pv{v, projectEdges(doc->mesh, v, hidden && name != "iso"), {}, {}};
                        if (name == "iso") {
                            // iso shows what's visible only
                            pv.segs = projectEdges(doc->mesh, v, true);
                            pv.segs.erase(std::remove_if(pv.segs.begin(), pv.segs.end(), [](const Seg& s) { return s.hidden; }), pv.segs.end());
                        }
                        for (const auto& s : pv.segs) {
                            pv.box.add(s.a);
                            pv.box.add(s.b);
                        }
                        views.push_back(std::move(pv));
                    }
                    // a section: the part behind the plane, seen square on, with the cut faces hatched
                    std::string sectionAxis;
                    double sectionAt = 0;
                    if (cmd::has(p, "section")) {
                        const std::string sec = stringParam(p, "section");
                        const auto eq = sec.find('=');
                        if (eq != 1 || (sec[0] != 'x' && sec[0] != 'y' && sec[0] != 'z'))
                            return Result::failure("section is a plane like x=20, y=0 or z=5");
                        sectionAxis = sec.substr(0, 1);
                        sectionAt = cad ? cad->eval(sec.substr(2)) : std::stod(sec.substr(2));
                        ViewDef v;
                        viewByName(sectionAxis == "y" ? "front" : sectionAxis == "x" ? "right" : "top", v);
                        const Vec3 n = sectionAxis == "x" ? Vec3{1, 0, 0} : sectionAxis == "y" ? Vec3{0, 1, 0} : Vec3{0, 0, 1};
                        const mesh::Bounds bb = mesh::bounds(doc->mesh);
                        const double pad = bb.diagonal() + 1;
                        // take away what's between the viewer and the plane
                        Vec3 lo = bb.min - Vec3{pad, pad, pad}, hi = bb.max + Vec3{pad, pad, pad};
                        const bool viewerLow = mesh::dot(v.d, n) > 0;  // looking along +n: the viewer is on the low side
                        if (viewerLow) hi = hi - n * mesh::dot(hi, n) + n * sectionAt;
                        else lo = lo - n * mesh::dot(lo, n) + n * sectionAt;
                        Mesh cutter = mesh::box(hi - lo, false);
                        for (auto& q : cutter.positions) q = q + lo;
                        const Mesh half = mesh::booleanDifference(doc->mesh, cutter);
                        if (half.empty()) return Result::failure("the section plane " + sec + " misses the part");
                        Placed pv{v, projectEdges(half, v, true), {}, {}, {}, "SECTION A-A"};
                        pv.segs.erase(std::remove_if(pv.segs.begin(), pv.segs.end(), [](const Seg& q) { return q.hidden; }), pv.segs.end());
                        std::vector<std::vector<P2>> loops2;
                        for (const auto& loop : planeLoops(doc->mesh, n, sectionAt)) {
                            std::vector<P2> l2;
                            for (const auto& q : loop) l2.push_back({mesh::dot(q, v.r), mesh::dot(q, v.u)});
                            loops2.push_back(std::move(l2));
                        }
                        pv.hatch = hatchLoops(loops2, bb.diagonal() / 60);
                        for (const auto& q : pv.segs) pv.box.add(q.a), pv.box.add(q.b);
                        pv.view.name = "section";
                        views.push_back(std::move(pv));
                    }
                    // a detail: a circle of another view, enlarged
                    std::string detailOf;
                    P2 detailC{0, 0};
                    double detailR = 0;
                    if (cmd::has(p, "detail")) {
                        const std::string det = stringParam(p, "detail");
                        const auto colon = det.find(':');
                        detailOf = colon == std::string::npos ? "front" : det.substr(0, colon);
                        const auto nums = splitList(colon == std::string::npos ? det : det.substr(colon + 1));
                        if (nums.size() != 3) return Result::failure("detail is view:a,b,radius, e.g. front:20,5,8");
                        auto value = [&](const std::string& t) { return cad ? cad->eval(t) : std::stod(t); };
                        detailC = {value(nums[0]), value(nums[1])};
                        detailR = value(nums[2]);
                        if (!(detailR > 0)) return Result::failure("the detail's radius must be positive");
                        const double k = numberParam(p, "detail_scale", 2);
                        if (!(k > 0)) return Result::failure("detail_scale must be positive");
                        ViewDef v;
                        if (!viewByName(detailOf, v) || detailOf == "iso") return Result::failure("detail: unknown view '" + detailOf + "' (front, top, right, ...)");
                        Placed pv{v, {}, {}, {}, {}, ""};
                        char label[64];
                        std::snprintf(label, sizeof label, "DETAIL B (%g:1)", k);
                        pv.label = label;
                        for (auto q : projectEdges(doc->mesh, v, hidden)) {
                            if (!clipToCircle(q.a, q.b, detailC, detailR)) continue;
                            pv.segs.push_back({detailC + (q.a - detailC) * k, detailC + (q.b - detailC) * k, q.hidden});
                        }
                        for (int i = 0; i < 72; ++i) {  // the detail's own circle
                            const double a0 = i * 2 * 3.14159265358979323846 / 72, a1 = (i + 1) * 2 * 3.14159265358979323846 / 72;
                            pv.segs.push_back({detailC + P2{std::cos(a0), std::sin(a0)} * (detailR * k), detailC + P2{std::cos(a1), std::sin(a1)} * (detailR * k), false});
                        }
                        for (const auto& q : pv.segs) pv.box.add(q.a), pv.box.add(q.b);
                        pv.view.name = "detail";
                        views.push_back(std::move(pv));
                    }
                    if (views.empty()) return Result::failure("no views asked for");
                    // layout: front in the middle, top above (third angle) or below (first), side views beside it
                    double big = 0;
                    for (const auto& v : views) big = std::max({big, v.box.width(), v.box.height()});
                    const double gap = big * 0.45;
                    const bool third = stringParam(p, "projection", "third") == "third";
                    const Placed* front = nullptr;
                    for (const auto& v : views) {
                        if (v.view.name == "front") front = &v;
                    }
                    const double fw = front ? front->box.width() : 0, fh = front ? front->box.height() : 0;
                    double others = 0;
                    for (auto& v : views) {
                        const std::string& n = v.view.name;
                        const double w = v.box.width(), h = v.box.height();
                        P2 c{0, 0};  // where the view's centre goes, relative to the front view's centre
                        const double vy = (fh + h) / 2 + gap, vx = (fw + w) / 2 + gap;
                        if (n == "front") c = {0, 0};
                        else if (n == "top") c = {0, third ? vy : -vy};
                        else if (n == "bottom") c = {0, third ? -vy : vy};
                        else if (n == "right") c = {third ? vx : -vx, 0};
                        else if (n == "left") c = {third ? -vx : vx, 0};
                        else if (n == "back") c = {(third ? 1 : -1) * (vx + fw + gap), 0};
                        else {
                            c = {vx + others, vy};
                            others += w + gap;
                        }
                        v.at = P2{c.x - w / 2, c.y - h / 2};
                    }
                    blueprint::Box layout;
                    for (const auto& v : views) {
                        layout.add(v.at);
                        layout.add(v.at + P2{v.box.width(), v.box.height()});
                    }
                    layout.add(P2{layout.x0 - gap * 0.6, layout.y0 - gap * 0.6});
                    layout.add(P2{layout.x1 + gap * 0.3, layout.y1 + gap * 0.3});
                    // scale: the biggest standard one that fits above the title block
                    const double pw = d->paperWidth() - 20, ph = d->paperHeight() - 20 - 45;
                    const double unitMm = blueprint::unitToMm(d->units);
                    const std::string scaleText = stringParam(p, "scale", "auto");
                    if (scaleText == "auto") {
                        const double fit = std::min(pw / (layout.width() * unitMm), ph / (layout.height() * unitMm));
                        const std::pair<double, double> scales[] = {{10, 1}, {5, 1}, {2, 1}, {1, 1}, {1, 2}, {1, 5}, {1, 10}, {1, 20}, {1, 50}, {1, 100}, {1, 200}, {1, 500}, {1, 1000}};
                        d->scaleNum = 1;
                        d->scaleDen = 1000;
                        for (const auto& [a, b] : scales) {
                            if (a / b <= fit) {
                                d->scaleNum = a;
                                d->scaleDen = b;
                                break;
                            }
                        }
                    } else if (!blueprint::parseScale(scaleText, d->scaleNum, d->scaleDen)) {
                        return Result::failure("scale must look like 1:2 or 2:1, or be auto");
                    }
                    const P2 mid{(layout.x0 + layout.x1) / 2, (layout.y0 + layout.y1) / 2};
                    d->origin = mid - P2{d->paperWidth() / 2, 10 + 45 + ph / 2} / d->k();
                    int lines = 0, hiddenLines = 0, dimCount = 0;
                    bool sectionDrawn = false;
                    auto& sect = d->ensureLayer("SECTION");
                    sect.weight = 0.13;
                    sect.color = {70, 70, 70, 255};
                    auto add = [&](blueprint::Entity e) {
                        d->add(std::move(e));
                    };
                    for (const auto& v : views) {
                        const P2 shift = v.at - P2{v.box.x0, v.box.y0};
                        for (const auto& s : v.segs) {
                            blueprint::Entity e;
                            e.type = blueprint::EntityType::Line;
                            e.layer = s.hidden ? "HIDDEN" : "VISIBLE";
                            e.pts = {s.a + shift, s.b + shift};
                            add(e);
                            (s.hidden ? hiddenLines : lines) += 1;
                        }
                        for (const auto& hs : v.hatch) {
                            blueprint::Entity e;
                            e.type = blueprint::EntityType::Line;
                            e.layer = "SECTION";
                            e.pts = {hs.a + shift, hs.b + shift};
                            add(e);
                        }
                        // where the section cuts, on the first view that sees the plane edge on; the detail's circle on its view
                        if (!sectionAxis.empty() && v.view.name != "section" && v.view.name != "detail" && v.view.name != "iso" && !sectionDrawn) {
                            const Vec3 n = sectionAxis == "x" ? Vec3{1, 0, 0} : sectionAxis == "y" ? Vec3{0, 1, 0} : Vec3{0, 0, 1};
                            if (std::fabs(mesh::dot(n, v.view.d)) < 1e-9) {
                                sectionDrawn = true;
                                const double alongR = mesh::dot(n, v.view.r), alongU = mesh::dot(n, v.view.u);
                                const double ext = gap * 0.25;
                                P2 a, b;
                                if (std::fabs(alongR) > 0.5) {
                                    const double x = sectionAt * alongR;
                                    a = P2{x, v.box.y0 - ext}, b = P2{x, v.box.y1 + ext};
                                } else {
                                    const double y = sectionAt * alongU;
                                    a = P2{v.box.x0 - ext, y}, b = P2{v.box.x1 + ext, y};
                                }
                                blueprint::Entity cut;
                                cut.type = blueprint::EntityType::Line;
                                cut.layer = "CENTER";
                                cut.pts = {a + shift, b + shift};
                                add(cut);
                                for (const P2& end : {a, b}) {
                                    blueprint::Entity t;
                                    t.type = blueprint::EntityType::Text;
                                    t.layer = "DIMS";
                                    t.text = "A";
                                    t.height = 3.5 / d->k();
                                    t.halign = blueprint::HAlign::Center;
                                    t.c = end + shift + P2{0, 1.5 / d->k()};
                                    add(t);
                                }
                            }
                        }
                        if (!detailOf.empty() && v.view.name == detailOf) {
                            blueprint::Entity ring;
                            ring.type = blueprint::EntityType::Circle;
                            ring.layer = "DIMS";
                            ring.c = detailC + shift;
                            ring.r = detailR;
                            add(ring);
                            blueprint::Entity t;
                            t.type = blueprint::EntityType::Text;
                            t.layer = "DIMS";
                            t.text = "B";
                            t.height = 3.5 / d->k();
                            t.c = detailC + shift + P2{detailR * 0.75, detailR * 0.75};
                            add(t);
                        }
                        blueprint::Entity label;
                        label.type = blueprint::EntityType::Text;
                        label.layer = "DIMS";
                        label.text = !v.label.empty() ? v.label : v.view.name == "iso" ? "ISOMETRIC" : v.view.name == "top" ? "TOP" : v.view.name == "front" ? "FRONT" : v.view.name == "right" ? "RIGHT" : v.view.name == "left" ? "LEFT" : v.view.name == "back" ? "BACK" : "BOTTOM";
                        label.height = 3.5 / d->k();
                        label.halign = blueprint::HAlign::Center;
                        label.c = P2{v.at.x + v.box.width() / 2, v.at.y - gap * 0.55};
                        add(label);
                        if (!dims || v.view.name == "iso" || v.view.name == "section" || v.view.name == "detail") continue;
                        const double off = gap * 0.3;
                        blueprint::Entity w;
                        w.type = blueprint::EntityType::Dimension;
                        w.layer = "DIMS";
                        w.dim = blueprint::DimKind::Horizontal;
                        w.pts = {v.at, v.at + P2{v.box.width(), 0}, v.at + P2{0, -off}};
                        if (v.box.width() > 1e-9) {
                            add(w);
                            ++dimCount;
                        }
                        blueprint::Entity h = w;
                        h.dim = blueprint::DimKind::Vertical;
                        h.pts = {v.at, v.at + P2{0, v.box.height()}, v.at + P2{-off, 0}};
                        if (v.box.height() > 1e-9) {
                            add(h);
                            ++dimCount;
                        }
                        // circles seen face on: holes, bosses, sketch circles, threads
                        if (!cad) continue;
                        std::map<long long, std::vector<P2>> byRadius;
                        std::vector<std::pair<P2, std::string>> callouts;
                        for (const auto& f : cad->features) {
                            const FeatureExtra* x = cad->featureExtra(f.name);
                            if (!x || f.suppressed) continue;
                            for (const auto& a : x->axes) {
                                if (a.radius <= 0 || std::fabs(std::fabs(mesh::dot(mesh::normalize(a.dir), v.view.d)) - 1) > 1e-6) continue;
                                const P2 c2 = P2{mesh::dot(a.point, v.view.r), mesh::dot(a.point, v.view.u)} + shift;
                                auto& list = byRadius[std::llround(a.radius * 1e6)];
                                bool dup = false;
                                for (const auto& q : list) dup = dup || blueprint::length(q - c2) < 1e-6 * (1 + big);
                                if (!dup) list.push_back(c2);
                                if (f.type == "thread") {
                                    char buf[64];
                                    std::snprintf(buf, sizeof buf, "M%gx%g", cad->eval(f.values.at("diameter")), cad->eval(f.values.at("pitch")));
                                    callouts.push_back({c2, buf});
                                }
                            }
                        }
                        for (const auto& [rk, centres] : byRadius) {
                            const double r = static_cast<double>(rk) / 1e6;
                            for (const auto& c2 : centres) {
                                const double m = r * 1.25;
                                for (int k = 0; k < 2; ++k) {
                                    blueprint::Entity cl;
                                    cl.type = blueprint::EntityType::Line;
                                    cl.layer = "CENTER";
                                    cl.pts = k == 0 ? std::vector<P2>{c2 - P2{m, 0}, c2 + P2{m, 0}} : std::vector<P2>{c2 - P2{0, m}, c2 + P2{0, m}};
                                    add(cl);
                                }
                            }
                            blueprint::Entity dd;
                            dd.type = blueprint::EntityType::Dimension;
                            dd.layer = "DIMS";
                            dd.dim = blueprint::DimKind::Diameter;
                            dd.c = centres.front();
                            dd.r = r;
                            dd.rotation = 45;
                            if (centres.size() > 1) dd.text = std::to_string(centres.size()) + "x <>";
                            add(dd);
                            ++dimCount;
                        }
                        for (const auto& [c2, t] : callouts) {
                            blueprint::Entity ct;
                            ct.type = blueprint::EntityType::Text;
                            ct.layer = "DIMS";
                            ct.text = t;
                            ct.height = 2.5 / d->k();
                            ct.c = c2 + P2{0, -gap * 0.15};
                            ct.halign = blueprint::HAlign::Center;
                            add(ct);
                        }
                    }
                    if (assembly && dims) {
                        // parts list, top left of the sheet
                        const double rowH = 6 / d->k();
                        P2 at = d->fromPaper({15, d->paperHeight() - 15});
                        int item = 0;
                        auto row = [&](const std::string& t) {
                            blueprint::Entity e;
                            e.type = blueprint::EntityType::Text;
                            e.layer = "DIMS";
                            e.text = t;
                            e.height = 2.5 / d->k();
                            e.c = at;
                            add(e);
                            at.y -= rowH;
                        };
                        row("ITEM  PART  QTY");
                        for (const auto& part : assembly->parts) {
                            int qty = 0;
                            for (const auto& c : assembly->components) qty += c.part == part.name ? 1 : 0;
                            if (qty) row(std::to_string(++item) + "  " + part.name + "  " + std::to_string(qty));
                        }
                    }
                    d->title.title = cmd::has(p, "title") ? stringParam(p, "title") : (doc->path.empty() ? id : std::filesystem::path(doc->path).stem().string());
                    d->title.author = stringParam(p, "author");
                    d->title.date = today();
                    std::string bp = session.workspace().add(d, "bp");
                    Result r = Result::success("drawing " + bp + ": " + std::to_string(views.size()) + " views at " + d->scaleText() + ", " +
                                                   std::to_string(lines) + " visible and " + std::to_string(hiddenLines) + " hidden lines, " +
                                                   std::to_string(dimCount) + " dimensions",
                                               bp);
                    r.with("drawing", bp).with("scale", d->scaleText()).with("visible", static_cast<std::int64_t>(lines))
                        .with("hidden", static_cast<std::int64_t>(hiddenLines)).with("dimensions", static_cast<std::int64_t>(dimCount));
                    if (cmd::has(p, "path")) {
                        const std::string path = stringParam(p, "path");
                        blueprint::exportFile(*d, path);
                        r.with("path", path);
                        r.message += ", saved " + path;
                    }
                    return r;
                } catch (const std::exception& e) {
                    return Result::failure(e.what());
                }
            });
        });
}

}  // namespace wetwaresuite::cad
