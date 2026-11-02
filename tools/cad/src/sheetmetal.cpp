// Sheet metal, the SolidWorks / Onshape way at its simplest: a flat base plate of one thickness
// (sheet_base), flanges bent up or down off its straight edges with an inside bend radius
// (sheet_flange), and the flat pattern (cad.sheet_flat) with the bends unrolled by the K-factor,
// bend lines and bend notes, ready for a laser cutter as DXF.
#include <algorithm>
#include <cmath>
#include <filesystem>

#include "wetwaresuite/blueprint/blueprint.hpp"
#include "wetwaresuite/mesh/primitives.hpp"
#include "build.hpp"
#include "cmdutil.hpp"
#include "sheetmetal.hpp"

namespace wetwaresuite::cad {

using mesh::Mesh;
using mesh::Vec2;
using mesh::Vec3;

namespace detail {

namespace {

constexpr double kPi = 3.14159265358979323846;

double cross2(const Vec2& a, const Vec2& b) { return a.x * b.y - a.y * b.x; }

// The base's outline, counter-clockwise, from its points (or width x depth from the origin).
std::vector<Vec2> baseOutline(const Ctx& c) {
    std::vector<Vec2> pts;
    if (c.has("points") && !c.text("points").empty()) {
        pts = c.pts("points", 3);
    } else {
        const double w = c.number("width"), d = c.number("depth");
        if (!(w > 0 && d > 0)) c.fail("width and depth must be positive (or give points=)");
        pts = {{0, 0}, {w, 0}, {w, d}, {0, d}};
    }
    double area = 0;
    for (std::size_t i = 0; i < pts.size(); ++i) area += cross2(pts[i], pts[(i + 1) % pts.size()]);
    if (std::fabs(area) < 1e-12) c.fail("the base outline has no area");
    if (area < 0) std::reverse(pts.begin(), pts.end());
    return pts;
}

// Which edge of the outline a flange goes on: front/back/left/right (the edge facing that way the
// most) or near=x,y (the edge closest to the point).
std::size_t pickEdge(const Ctx& c, const std::vector<Vec2>& outline) {
    const std::string edge = c.text("edge");
    const std::size_t n = outline.size();
    std::size_t best = 0;
    double score = -1e300;
    Vec2 want{0, 0};
    bool byPoint = false;
    Vec2 at{0, 0};
    if (edge == "front") want = {0, -1};
    else if (edge == "back") want = {0, 1};
    else if (edge == "left") want = {-1, 0};
    else if (edge == "right") want = {1, 0};
    else {
        const auto items = splitList(edge);
        if (items.size() != 2) c.fail("edge is front, back, left, right, or a point x,y near it");
        at = {c.doc.eval(items[0]), c.doc.eval(items[1])};
        byPoint = true;
    }
    for (std::size_t i = 0; i < n; ++i) {
        const Vec2 a = outline[i], b = outline[(i + 1) % n];
        const Vec2 d = b - a;
        const double len = std::hypot(d.x, d.y);
        if (len < 1e-12) continue;
        const Vec2 out{d.y / len, -d.x / len};  // counter-clockwise: outside is to the right
        double s;
        if (byPoint) {
            const double t = std::clamp(((at.x - a.x) * d.x + (at.y - a.y) * d.y) / (len * len), 0.0, 1.0);
            s = -std::hypot(a.x + d.x * t - at.x, a.y + d.y * t - at.y);
        } else {
            s = out.x * want.x + out.y * want.y + 1e-9 * len;  // facing that way; the longest wins a tie
        }
        if (s > score) score = s, best = i;
    }
    return best;
}

// A bent strip as a closed mesh: pairs of (inner, outer) points in the section plane, swept along
// the edge from `a` for `length`.
Mesh sweepStrip(const std::vector<std::pair<Vec3, Vec3>>& section, const Vec3& along, double length) {
    Mesh m;
    const std::size_t n = section.size();
    std::vector<std::uint32_t> i0(n), o0(n), i1(n), o1(n);
    for (std::size_t k = 0; k < n; ++k) {
        i0[k] = m.addVertex(section[k].first);
        o0[k] = m.addVertex(section[k].second);
        i1[k] = m.addVertex(section[k].first + along * length);
        o1[k] = m.addVertex(section[k].second + along * length);
    }
    auto quad = [&](std::uint32_t a, std::uint32_t b, std::uint32_t c2, std::uint32_t d) {
        m.addTriangle(a, b, c2);
        m.addTriangle(a, c2, d);
    };
    for (std::size_t k = 0; k + 1 < n; ++k) {
        quad(i0[k], i0[k + 1], i1[k + 1], i1[k]);  // inner surface
        quad(o0[k], o1[k], o1[k + 1], o0[k + 1]);  // outer surface
        quad(i0[k], o0[k], o0[k + 1], i0[k + 1]);  // the side at the edge's start
        quad(i1[k], i1[k + 1], o1[k + 1], o1[k]);  // the side at its end
    }
    quad(i0[0], i1[0], o1[0], o0[0]);                  // the start of the strip
    quad(i0[n - 1], o0[n - 1], o1[n - 1], i1[n - 1]);  // its free end
    // the winding depends on which way the bend turns: make it face out
    if (mesh::volume(m) < 0)
        for (std::size_t t = 0; t < m.triangleCount(); ++t) std::swap(m.indices[t * 3 + 1], m.indices[t * 3 + 2]);
    return m;
}

}  // namespace

bool isSheetMetal(const std::string& type) { return type == "sheet_base" || type == "sheet_flange"; }

SheetInfo sheetInfo(const CadDoc& doc, const std::string& featureName) {
    SheetInfo info;
    const Feature* f = doc.findFeature(featureName);
    std::vector<const Feature*> chain;
    for (int guard = 0; f && guard < 1000; ++guard) {
        chain.push_back(f);
        if (f->type == "sheet_base") break;
        if (f->type != "sheet_flange") return {};
        auto it = f->values.find("target");
        f = it == f->values.end() ? nullptr : doc.findFeature(it->second);
    }
    if (chain.empty() || chain.back()->type != "sheet_base") return {};
    const Feature& base = *chain.back();
    const FeatureType* bt = featureType("sheet_base");
    Ctx c{doc, base, *bt};
    info.ok = true;
    info.base = base.name;
    info.outline = baseOutline(c);
    info.thickness = c.number("thickness");
    info.radius = c.has("radius") && !c.text("radius").empty() ? c.number("radius") : info.thickness;
    info.k = c.number("k_factor");
    const FeatureType* ft = featureType("sheet_flange");
    for (auto it = chain.rbegin() + 1; it != chain.rend(); ++it) {
        if ((*it)->suppressed) continue;
        Ctx fc{doc, **it, *ft};
        SheetInfo::Flange fl;
        fl.name = (*it)->name;
        fl.edge = pickEdge(fc, info.outline);
        fl.length = fc.number("length");
        fl.angle = fc.number("angle");
        fl.up = fc.text("direction") != "down";
        fl.radius = fc.has("radius") && !fc.text("radius").empty() ? fc.number("radius") : info.radius;
        info.flanges.push_back(fl);
    }
    return info;
}

Built buildSheetMetal(const Ctx& c, const Done& done) {
    if (c.f.type == "sheet_base") {
        const auto outline = baseOutline(c);
        const double t = c.number("thickness");
        if (!(t > 0)) c.fail("thickness must be positive");
        const double k = c.number("k_factor");
        if (!(k >= 0 && k <= 1)) c.fail("k_factor is between 0 and 1 (0.33 to 0.5 for most sheet)");
        mesh::Polygon2 shape;
        shape.outer = outline;
        return {share(mesh::extrude(shape, t)), {}, {}, {}};
    }
    // a flange: off one edge of the base, through the bend, then straight on
    const auto* target = input(done, c, "target");
    if (!target || !target->mesh) c.fail("the target has no body");
    const SheetInfo info = sheetInfo(c.doc, c.text("target"));
    if (!info.ok) c.fail("a flange goes on a sheet_base (or on a flange of one)");
    const std::size_t e = pickEdge(c, info.outline);
    const Vec2 a2 = info.outline[e], b2 = info.outline[(e + 1) % info.outline.size()];
    const Vec2 d2 = b2 - a2;
    const double len = std::hypot(d2.x, d2.y);
    const double angle = c.number("angle"), L = c.number("length");
    const double R = c.has("radius") && !c.text("radius").empty() ? c.number("radius") : info.radius;
    const double t = info.thickness;
    if (!(angle > 0 && angle <= 180)) c.fail("angle is between 0 and 180 degrees");
    if (!(L >= 0)) c.fail("length can't be negative");
    if (!(R >= 0)) c.fail("radius can't be negative");
    const bool up = c.text("direction") != "down";
    const Vec3 along{d2.x / len, d2.y / len, 0}, out{d2.y / len, -d2.x / len, 0}, z{0, 0, up ? 1.0 : -1.0};
    const Vec3 a{a2.x, a2.y, 0};
    // bending up, the top face is the inside of the bend: the axis is R above the top, at the edge
    const Vec3 axis = a + Vec3{0, 0, up ? t + R : -R};
    const double theta = angle * kPi / 180;
    const int steps = std::max(2, static_cast<int>(std::ceil(angle / 7.5)));
    std::vector<std::pair<Vec3, Vec3>> section;
    // start a hair inside the plate, so the union overlaps instead of touching
    const double tuck = std::min(t, 1e-3 * (len + 1));
    section.push_back({axis - z * R - out * tuck, axis - z * (R + t) - out * tuck});
    for (int i = 0; i <= steps; ++i) {
        const double phi = theta * i / steps;
        const Vec3 radial = out * std::sin(phi) - z * std::cos(phi);
        section.push_back({axis + radial * R, axis + radial * (R + t)});
    }
    if (L > 0) {
        const Vec3 tangent = out * std::cos(theta) + z * std::sin(theta);
        section.push_back({section.back().first + tangent * L, section.back().second + tangent * L});
    }
    const Mesh strip = sweepStrip(section, along, len);
    return {share(csg(*target->mesh, strip, mesh::BooleanOp::Union)), {}, {}, {}};
}

}  // namespace detail

void registerSheetMetalCommands(Session& session) {
    session.registry().add(
        {"cad.sheet_flat", "Flat Pattern",
         "Unfold a sheet metal part (a sheet_base and its flanges) into its flat pattern: each bend is unrolled by the "
         "bend allowance angle x (radius + k_factor x thickness), with dashed bend lines and notes (UP 90 R2). Opens as a "
         "blueprint drawing; path= writes .dxf for a laser or plasma cutter (or .svg, .pdf, .png).",
         {cmd::docParam(), cmd::str("feature", "the last flange (or the base) of the sheet part (default: the last sheet feature)"),
          cmd::str("path", "export to this file: .dxf, .svg, .pdf or .png (optional)")},
         false},
        [&session](const Params& p) -> std::unique_ptr<Action> {
            return std::make_unique<FunctionAction>([&session, p] {
                try {
                    const std::string id = stringParam(p, "doc");
                    auto doc = session.workspace().getAs<CadDoc>(id);
                    if (!doc) return Result::failure("no open cad document called '" + id + "'");
                    std::string name = stringParam(p, "feature");
                    if (name.empty())
                        for (const auto& f : doc->features)
                            if (detail::isSheetMetal(f.type) && !f.suppressed) name = f.name;
                    if (name.empty()) return Result::failure("the part has no sheet metal (cad.sheet_base starts one)");
                    const detail::SheetInfo info = detail::sheetInfo(*doc, name);
                    if (!info.ok) return Result::failure("'" + name + "' isn't a sheet_base or a flange of one");
                    // walk the outline; an edge with a flange grows a tab as long as the unrolled bend plus the flange
                    const auto& o = info.outline;
                    const std::size_t n = o.size();
                    std::vector<double> reach(n, 0);
                    std::vector<std::string> notes(n);
                    std::vector<double> allowance(n, 0);
                    for (const auto& fl : info.flanges) {
                        const double ba = fl.angle * 3.14159265358979323846 / 180 * (fl.radius + info.k * info.thickness);
                        reach[fl.edge] = ba + fl.length;
                        allowance[fl.edge] = ba;
                        char buf[96];
                        std::snprintf(buf, sizeof buf, "%s %g\xC2\xB0 R%g", fl.up ? "UP" : "DOWN", fl.angle, fl.radius);
                        notes[fl.edge] = buf;
                    }
                    auto d = std::make_shared<blueprint::Drawing>();
                    d->units = doc->units == "in" ? blueprint::Unit::In : doc->units == "cm" ? blueprint::Unit::Cm : doc->units == "m" ? blueprint::Unit::M : blueprint::Unit::Mm;
                    auto& cutLayer = d->ensureLayer("CUT");
                    cutLayer.weight = 0.35;
                    auto& bendLayer = d->ensureLayer("BEND");
                    bendLayer.weight = 0.18;
                    bendLayer.linetype = blueprint::LineType::Hidden;
                    bendLayer.color = {200, 60, 60, 255};
                    d->ensureLayer("NOTES");
                    std::vector<blueprint::Vec2> outline;
                    double perimeter = 0;
                    int bends = 0;
                    for (std::size_t i = 0; i < n; ++i) {
                        const Vec2 a = o[i], b = o[(i + 1) % n];
                        const Vec2 dd = b - a;
                        const double len = std::hypot(dd.x, dd.y);
                        const Vec2 out{dd.y / len, -dd.x / len};
                        outline.push_back({a.x, a.y});
                        if (reach[i] > 0) {
                            const Vec2 a1 = a + out * reach[i], b1 = b + out * reach[i];
                            outline.push_back({a1.x, a1.y});
                            outline.push_back({b1.x, b1.y});
                            // bend lines: where the bend starts and ends
                            for (double at : {0.0, allowance[i]}) {
                                blueprint::Entity bl;
                                bl.type = blueprint::EntityType::Line;
                                bl.layer = "BEND";
                                const Vec2 p0 = a + out * at, p1 = b + out * at;
                                bl.pts = {{p0.x, p0.y}, {p1.x, p1.y}};
                                d->add(bl);
                            }
                            blueprint::Entity note;
                            note.type = blueprint::EntityType::Text;
                            note.layer = "NOTES";
                            note.text = notes[i];
                            note.height = std::max(info.thickness * 2, len / 25);
                            note.halign = blueprint::HAlign::Center;
                            const Vec2 m = (a + b) * 0.5 + out * (allowance[i] / 2);
                            note.c = {m.x, m.y};
                            note.rotation = std::atan2(dd.y, dd.x) * 180 / 3.14159265358979323846;
                            d->add(note);
                            ++bends;
                        }
                    }
                    for (std::size_t i = 0; i < outline.size(); ++i) {
                        const auto& a = outline[i];
                        const auto& b = outline[(i + 1) % outline.size()];
                        perimeter += std::hypot(b.x - a.x, b.y - a.y);
                    }
                    blueprint::Entity cut;
                    cut.type = blueprint::EntityType::Polyline;
                    cut.layer = "CUT";
                    cut.pts = outline;
                    cut.closed = true;
                    d->add(cut);
                    blueprint::Box box;
                    for (const auto& q : outline) box.add(q);
                    d->title.title = "Flat pattern " + (doc->path.empty() ? id : std::filesystem::path(doc->path).stem().string());
                    char size[160];
                    std::snprintf(size, sizeof size, "%g x %g x %g", box.width(), box.height(), info.thickness);
                    d->paper = "A3";
                    d->landscape = true;
                    const double fit = std::min((d->paperWidth() - 20) / (box.width() + 1e-9), (d->paperHeight() - 75) / (box.height() + 1e-9)) /
                                       blueprint::unitToMm(d->units);
                    d->scaleNum = 1;
                    d->scaleDen = 1;
                    for (double den : {1.0, 2.0, 5.0, 10.0, 20.0, 50.0, 100.0})
                        if (1 / den <= fit) {
                            d->scaleDen = den;
                            break;
                        }
                    d->origin = blueprint::Vec2{(box.x0 + box.x1) / 2, (box.y0 + box.y1) / 2} -
                                blueprint::Vec2{d->paperWidth() / 2, 10 + 45 + (d->paperHeight() - 75) / 2} / d->k();
                    const std::string bp = session.workspace().add(d, "bp");
                    Result r = Result::success("flat pattern " + bp + ": " + size + " " + doc->units + ", " + std::to_string(bends) + " bends, cut length " +
                                                   std::to_string(static_cast<long long>(std::llround(perimeter))) + " " + doc->units,
                                               bp);
                    r.with("drawing", bp).with("width", box.width()).with("height", box.height()).with("bends", static_cast<std::int64_t>(bends))
                        .with("perimeter", perimeter);
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
