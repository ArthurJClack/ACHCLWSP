// cad.sketch_* commands: drawing sketch geometry, constraining it, and seeing
// its state. Every edit solves the sketch again (inside the part's rebuild),
// writes the solved positions back, and reports how defined it is.
#include <algorithm>
#include <cmath>
#include <map>
#include <optional>

#include "wetwaresuite/blueprint/blueprint.hpp"
#include "wetwaresuite/raster/codec.hpp"
#include "wetwaresuite/mesh/ops.hpp"
#include "cmdutil.hpp"
#include "sketch_solve.hpp"

namespace wetwaresuite::cad {

using mesh::Vec2;
using namespace cmd;

namespace {

constexpr double kPi = 3.14159265358979323846;

ParamSpec sketchParam() { return {"sketch", ParamType::String, "name of a sketch feature, e.g. sketch1 (cad.sketch makes one)", true}; }

std::vector<ParamSpec> with(std::vector<ParamSpec> s) {
    s.insert(s.begin(), sketchParam());
    s.insert(s.begin(), docParam());
    s.push_back({"preview", ParamType::Bool, "attach a PNG of the sketch to the result", false, {}, false});
    return s;
}

double evalParam(const CadDoc& doc, const Params& p, const std::string& key, double fallback) {
    if (!has(p, key)) return fallback;
    return doc.eval(text(p, key));
}

double evalRequired(const CadDoc& doc, const Params& p, const std::string& key) {
    if (!has(p, key)) throw std::invalid_argument(key + " is required");
    return doc.eval(text(p, key));
}

}  // namespace

// The solved sketch drawn as a blueprint drawing (construction dashed, dimensions labelled).
std::shared_ptr<blueprint::Drawing> sketchDrawing(const CadDoc& doc, const Feature& f) {
    auto d = std::make_shared<blueprint::Drawing>();
    d->scaleNum = 1;
    d->scaleDen = 1;
    d->title.show = false;
    auto& geo = d->ensureLayer("geometry");
    geo.weight = 0.5;
    auto& con = d->ensureLayer("construction");
    con.linetype = blueprint::LineType::Dashed;
    con.color = {40, 110, 220, 255};
    auto& free = d->ensureLayer("free");
    free.color = {40, 110, 220, 255};
    auto& dims = d->ensureLayer("dimensions");
    dims.color = {200, 60, 40, 255};
    const FeatureExtra* x = doc.featureExtra(f.name);
    if (!x || !x->sketch) return d;
    const SolvedSketch& s = *x->sketch;
    auto bp = [](Vec2 v) { return blueprint::Vec2{v.x, v.y}; };
    for (const auto& e : f.sketch.entities) {
        blueprint::Entity be;
        be.layer = e.construction ? "construction" : "geometry";
        auto pts = detail::entityOutline(e, s, 64);
        if (e.kind == "line") {
            be.type = blueprint::EntityType::Line;
            be.pts = {bp(pts[0]), bp(pts[1])};
        } else {
            be.type = blueprint::EntityType::Polyline;
            for (const auto& q : pts) be.pts.push_back(bp(q));
        }
        d->add(be);
    }
    double extent = 1;
    for (const auto& [id, p] : s.points) extent = std::max({extent, std::fabs(p.x), std::fabs(p.y)});
    for (const auto& [id, p] : s.points) {
        const bool loose = std::find(s.underDefined.begin(), s.underDefined.end(), id) != s.underDefined.end();
        blueprint::Entity c;
        c.type = blueprint::EntityType::Circle;
        c.layer = loose ? "free" : "geometry";
        c.c = bp(p);
        c.r = extent * 0.008;
        d->add(c);
    }
    blueprint::Entity o;
    o.type = blueprint::EntityType::Point;
    o.c = {0, 0};
    d->add(o);
    for (const auto& k : f.sketch.constraints) {
        auto it = s.measured.find(k.id);
        if (it == s.measured.end() || k.refs.empty()) continue;
        Vec2 at{0, 0};
        int n = 0;
        for (const auto& r : k.refs) {
            for (const auto& e : f.sketch.entities) {
                if (e.id == r.substr(0, r.find('.'))) {
                    for (const auto& pid : e.points) {
                        at += s.points.at(pid);
                        ++n;
                    }
                }
            }
            if (s.points.count(r)) {
                at += s.points.at(r);
                ++n;
            }
        }
        if (n) at = at / n;
        blueprint::Entity t;
        t.type = blueprint::EntityType::Text;
        t.layer = "dimensions";
        t.c = bp(at);
        t.height = extent * 0.035;
        t.text = k.id + " " + k.type + " " + num(it->second);
        d->add(t);
    }
    return d;
}

namespace {

void attachSketch(Result& r, const CadDoc& doc, const Feature& f, const Params& p) {
    if (!boolParam(p, "preview", false)) return;
    auto d = sketchDrawing(doc, f);
    r.attach({"image/png", raster::encodePng(blueprint::render(*d, blueprint::extentsView(*d), 512)), "sketch.png"});
}

using SketchFn = std::function<Result(CadDoc&, Feature&, Sketch&, const Params&)>;

void writeBack(const CadDoc& doc, Feature& f) {
    const FeatureExtra* x = doc.featureExtra(f.name);
    if (!x || !x->sketch) return;
    for (auto& p : f.sketch.points) {
        auto it = x->sketch->points.find(p.id);
        if (it != x->sketch->points.end()) {
            p.x = it->second.x;
            p.y = it->second.y;
        }
    }
    for (auto& e : f.sketch.entities) {
        auto it = x->sketch->radii.find(e.id);
        if (it != x->sketch->radii.end()) e.radius = it->second;
    }
}

Result& sketchState(Result& r, const CadDoc& doc, const Feature& f) {
    const FeatureExtra* x = doc.featureExtra(f.name);
    if (!x || !x->sketch) return r;
    const SolvedSketch& s = *x->sketch;
    r.message += " (" + f.name + ": " + s.status() + ")";
    std::string loose;
    for (const auto& id : s.underDefined) loose += (loose.empty() ? "" : ",") + id;
    r.with("sketch", f.name)
        .with("status", s.status())
        .with("dof", static_cast<std::int64_t>(s.dof))
        .with("fully_defined", s.dof == 0)
        .with("under_defined", loose)
        .with("regions", static_cast<std::int64_t>(s.regions.size()));
    return r;
}

ActionFactory sketchCommand(Session& session, SketchFn fn) {
    return cadCommand(session, [fn](CadDoc& doc, const Params& p) {
        const std::string name = stringParam(p, "sketch");
        Feature* f = doc.findFeature(name);
        if (!f) return Result::failure("no feature called '" + name + "'");
        if (f->type != "sketch") return Result::failure(name + " is a " + f->type + ", not a sketch");
        Result r;
        try {
            r = fn(doc, *f, f->sketch, p);
        } catch (const std::invalid_argument& e) {
            return Result::failure(e.what());
        }
        if (!r.ok) return r;
        doc.regenerate();
        f = doc.findFeature(name);
        writeBack(doc, *f);
        r.with("doc", stringParam(p, "doc"));
        sketchState(r, doc, *f);
        attachSketch(r, doc, *f, p);
        return r;
    });
}

detail::Evaluator evaluator(const CadDoc& doc) {
    return [&doc](const std::string& e) { return doc.eval(e); };
}

std::string addConstraint(const CadDoc& doc, Sketch& sk, const std::string& type, std::vector<std::string> refs, const std::string& value = {}) {
    SketchConstraint c;
    c.type = type;
    c.refs = std::move(refs);
    c.value = value;
    detail::prepareConstraint(sk, c, evaluator(doc));
    c.id = sk.newId("k");
    sk.constraints.push_back(c);
    return c.id;
}

// A point at (x, y): an existing one there, or a new one (pinned to the origin when it's there).
std::string pointAt(const CadDoc& doc, Sketch& sk, double x, double y) {
    const double tol = 1e-7 * (1 + std::fabs(x) + std::fabs(y));
    for (const auto& p : sk.points) {
        if (std::fabs(p.x - x) <= tol && std::fabs(p.y - y) <= tol) return p.id;
    }
    SketchPoint p{sk.newId("p"), x, y};
    sk.points.push_back(p);
    if (std::fabs(x) <= tol && std::fabs(y) <= tol) addConstraint(doc, sk, "coincident", {p.id, "origin"});
    return p.id;
}

// "p3", "l1.end", ... -> the underlying point id.
std::string pointId(const Sketch& sk, const std::string& ref) {
    if (sk.point(ref)) return ref;
    auto dot = ref.find('.');
    const SketchEntity* e = dot == std::string::npos ? nullptr : sk.entity(ref.substr(0, dot));
    if (e) {
        const std::string part = ref.substr(dot + 1);
        if (part == "center" && (e->kind == "circle" || e->kind == "arc")) return e->points[0];
        if ((part == "start" || part == "p1") && e->kind == "arc") return e->points[1];
        if (part == "end" && e->kind == "arc") return e->points[2];
        if (part == "start" || part == "p1") return e->points.front();
        if (part == "end" || part == "p2") return e->points.back();
    }
    throw std::invalid_argument("'" + ref + "' isn't a point of the sketch (p1, l1.start, c1.center, ...)");
}

void autoAlign(const CadDoc& doc, Sketch& sk, const SketchEntity& line) {
    const SketchPoint* a = sk.point(line.points[0]);
    const SketchPoint* b = sk.point(line.points[1]);
    const double tol = 1e-9 * (1 + std::fabs(a->x) + std::fabs(b->x) + std::fabs(a->y) + std::fabs(b->y));
    if (std::fabs(a->y - b->y) <= tol) addConstraint(doc, sk, "horizontal", {line.id});
    else if (std::fabs(a->x - b->x) <= tol) addConstraint(doc, sk, "vertical", {line.id});
}

std::string addLine(const CadDoc& doc, Sketch& sk, const std::string& a, const std::string& b, bool construction, bool align) {
    if (a == b) throw std::invalid_argument("a line needs two different end points");
    SketchEntity e{sk.newId("l"), "line", {a, b}, 0, construction, false};
    sk.entities.push_back(e);
    if (align) autoAlign(doc, sk, e);
    return e.id;
}

}  // namespace

void registerSketchCommands(Session& session) {
    auto& reg = session.registry();

    reg.add({"cad.sketch_point", "Sketch Point", "Add a point to a sketch (for constraints, hole centres, ...). Returns its id.",
             with({str("x", "x in sketch coordinates (number or expression)", true), str("y", "y", true)}), true},
            sketchCommand(session, [](CadDoc& doc, Feature&, Sketch& sk, const Params& p) {
                const std::string id = pointAt(doc, sk, evalRequired(doc, p, "x"), evalRequired(doc, p, "y"));
                return Result::success("point " + id, id).with("point", id);
            }));

    reg.add({"cad.sketch_line", "Sketch Line",
             "Add a line from x1,y1 to x2,y2 (or from / to existing points: p3, l1.end, ...). Ends that land on an "
             "existing point join it; a line drawn exactly level or upright gets horizontal / vertical.",
             with({str("x1", "start x"), str("y1", "start y"), str("x2", "end x"), str("y2", "end y"),
                   str("from", "start at this existing point instead"), str("to", "end at this existing point instead"),
                   boolean("construction", "construction geometry (not part of the profile)", false)}),
             true},
            sketchCommand(session, [](CadDoc& doc, Feature&, Sketch& sk, const Params& p) {
                const std::string a = has(p, "from") ? pointId(sk, stringParam(p, "from"))
                                                     : pointAt(doc, sk, evalRequired(doc, p, "x1"), evalRequired(doc, p, "y1"));
                const std::string b = has(p, "to") ? pointId(sk, stringParam(p, "to"))
                                                   : pointAt(doc, sk, evalRequired(doc, p, "x2"), evalRequired(doc, p, "y2"));
                const std::string id = addLine(doc, sk, a, b, boolParam(p, "construction", false), true);
                return Result::success("line " + id + " (" + a + " -> " + b + ")", id).with("entity", id);
            }));

    reg.add({"cad.sketch_polyline", "Sketch Polyline",
             "Add connected lines through points x,y,x,y,... (expressions allowed); closed joins the last to the first, "
             "which makes a profile in one go. Level and upright segments get horizontal / vertical.",
             with({str("points", "x,y,x,y,... at least 2 points", true), boolean("closed", "join the last point to the first", true),
                   boolean("construction", "construction geometry", false)}),
             true},
            sketchCommand(session, [](CadDoc& doc, Feature&, Sketch& sk, const Params& p) {
                const auto items = splitList(stringParam(p, "points"));
                if (items.size() % 2 || items.size() < 4) return Result::failure("points must be x,y pairs, at least 2 points");
                std::vector<std::string> ids;
                for (std::size_t i = 0; i < items.size(); i += 2) ids.push_back(pointAt(doc, sk, doc.eval(items[i]), doc.eval(items[i + 1])));
                const bool closed = boolParam(p, "closed", true) && ids.size() > 2;
                std::string lines;
                for (std::size_t i = 0; i + 1 < ids.size() + (closed ? 1 : 0); ++i) {
                    lines += (lines.empty() ? "" : ",") + addLine(doc, sk, ids[i], ids[(i + 1) % ids.size()], boolParam(p, "construction", false), true);
                }
                return Result::success("lines " + lines, lines).with("entities", lines);
            }));

    reg.add({"cad.sketch_rect", "Sketch Rectangle",
             "Add a rectangle: four lines joined at the corners, kept horizontal and vertical. x,y is its lower-left "
             "corner, or its centre with center=true (a construction diagonal holds the centre). dims=true also adds "
             "width and height dimensions using the expressions you gave (e.g. width=w).",
             with({str("x", "corner (or centre) x", false, "0"), str("y", "corner (or centre) y", false, "0"), str("width", "width along sketch x", true),
                   str("height", "height along sketch y", true), boolean("center", "x,y is the centre", false),
                   boolean("dims", "add width and height dimensions", false), boolean("construction", "construction geometry", false)}),
             true},
            sketchCommand(session, [](CadDoc& doc, Feature&, Sketch& sk, const Params& p) {
                const double w = evalRequired(doc, p, "width"), h = evalRequired(doc, p, "height");
                if (!(w > 0) || !(h > 0)) return Result::failure("width and height must be positive");
                double x = evalParam(doc, p, "x", 0), y = evalParam(doc, p, "y", 0);
                const bool center = boolParam(p, "center", false), cons = boolParam(p, "construction", false);
                const double x0 = center ? x - w / 2 : x, y0 = center ? y - h / 2 : y;
                const std::string a = pointAt(doc, sk, x0, y0), b = pointAt(doc, sk, x0 + w, y0), c = pointAt(doc, sk, x0 + w, y0 + h),
                                  d = pointAt(doc, sk, x0, y0 + h);
                const std::string bottom = addLine(doc, sk, a, b, cons, false), right = addLine(doc, sk, b, c, cons, false),
                                  top = addLine(doc, sk, c, d, cons, false), left = addLine(doc, sk, d, a, cons, false);
                addConstraint(doc, sk, "horizontal", {bottom, top});
                addConstraint(doc, sk, "vertical", {right, left});
                if (center) {
                    const std::string diag = addLine(doc, sk, a, c, true, false);
                    const std::string mid = pointAt(doc, sk, x, y);
                    addConstraint(doc, sk, "midpoint", {mid, diag});
                }
                if (boolParam(p, "dims", false)) {
                    addConstraint(doc, sk, "length", {bottom}, text(p, "width"));
                    addConstraint(doc, sk, "length", {right}, text(p, "height"));
                }
                const std::string lines = bottom + "," + right + "," + top + "," + left;
                return Result::success("rectangle " + lines, lines).with("entities", lines);
            }));

    reg.add({"cad.sketch_circle", "Sketch Circle",
             "Add a circle centred on cx,cy. dims=true adds its diameter (or radius) dimension with your expression.",
             with({str("cx", "centre x", false, "0"), str("cy", "centre y", false, "0"), str("diameter", "diameter (or give radius)"),
                   str("radius", "radius"), boolean("dims", "add the diameter / radius dimension", false),
                   boolean("construction", "construction geometry", false)}),
             true},
            sketchCommand(session, [](CadDoc& doc, Feature&, Sketch& sk, const Params& p) {
                double r = has(p, "radius") ? evalRequired(doc, p, "radius") : has(p, "diameter") ? evalRequired(doc, p, "diameter") / 2 : -1;
                if (!(r > 0)) return Result::failure("give a positive diameter or radius");
                const std::string c = pointAt(doc, sk, evalParam(doc, p, "cx", 0), evalParam(doc, p, "cy", 0));
                SketchEntity e{sk.newId("c"), "circle", {c}, r, boolParam(p, "construction", false), false};
                sk.entities.push_back(e);
                if (boolParam(p, "dims", false)) {
                    if (has(p, "radius")) addConstraint(doc, sk, "radius", {e.id}, text(p, "radius"));
                    else addConstraint(doc, sk, "diameter", {e.id}, text(p, "diameter"));
                }
                return Result::success("circle " + e.id, e.id).with("entity", e.id);
            }));

    reg.add({"cad.sketch_arc", "Sketch Arc",
             "Add an arc. mode=center: centre cx,cy, radius, from start_angle to end_angle (degrees, counter-clockwise). "
             "mode=three_point: from x1,y1 through x2,y2 to x3,y3. Ends that land on existing points join them; tangent "
             "adds a tangent constraint to that line or arc.",
             with({{"mode", ParamType::String, "how the arc is given", false, {"center", "three_point"}, std::string("center")},
                   str("cx", "centre x"), str("cy", "centre y"), str("radius", "radius"), str("start_angle", "degrees", false, "0"),
                   str("end_angle", "degrees", false, "90"), str("x1", "start x"), str("y1", "start y"), str("x2", "through x"),
                   str("y2", "through y"), str("x3", "end x"), str("y3", "end y"),
                   str("tangent", "make it tangent to this line or arc"), boolean("construction", "construction geometry", false)}),
             true},
            sketchCommand(session, [](CadDoc& doc, Feature&, Sketch& sk, const Params& p) {
                Vec2 c, s, e;
                if (stringParam(p, "mode", "center") == "center") {
                    c = {evalParam(doc, p, "cx", 0), evalParam(doc, p, "cy", 0)};
                    const double r = evalRequired(doc, p, "radius");
                    if (!(r > 0)) return Result::failure("radius must be positive");
                    const double a0 = evalParam(doc, p, "start_angle", 0) * kPi / 180, a1 = evalParam(doc, p, "end_angle", 90) * kPi / 180;
                    s = c + Vec2{std::cos(a0), std::sin(a0)} * r;
                    e = c + Vec2{std::cos(a1), std::sin(a1)} * r;
                } else {
                    const Vec2 a{evalRequired(doc, p, "x1"), evalRequired(doc, p, "y1")}, m{evalRequired(doc, p, "x2"), evalRequired(doc, p, "y2")},
                        b{evalRequired(doc, p, "x3"), evalRequired(doc, p, "y3")};
                    const double d = 2 * (a.x * (m.y - b.y) + m.x * (b.y - a.y) + b.x * (a.y - m.y));
                    if (std::fabs(d) < 1e-12) return Result::failure("the three points are in a line");
                    const double a2 = mesh::dot(a, a), m2 = mesh::dot(m, m), b2 = mesh::dot(b, b);
                    c = {(a2 * (m.y - b.y) + m2 * (b.y - a.y) + b2 * (a.y - m.y)) / d, (a2 * (b.x - m.x) + m2 * (a.x - b.x) + b2 * (m.x - a.x)) / d};
                    // a -> m -> b turning left runs counter-clockwise, so the arc goes from a to b
                    const bool ccw = mesh::cross(m - a, b - m) > 0;
                    s = ccw ? a : b;
                    e = ccw ? b : a;
                }
                const std::string pc = pointAt(doc, sk, c.x, c.y), ps = pointAt(doc, sk, s.x, s.y), pe = pointAt(doc, sk, e.x, e.y);
                if (ps == pe) return Result::failure("the arc's ends are the same point; use cad.sketch_circle");
                SketchEntity arc{sk.newId("a"), "arc", {pc, ps, pe}, 0, boolParam(p, "construction", false), false};
                sk.entities.push_back(arc);
                if (has(p, "tangent")) addConstraint(doc, sk, "tangent", {stringParam(p, "tangent"), arc.id});
                return Result::success("arc " + arc.id, arc.id).with("entity", arc.id);
            }));

    reg.add({"cad.sketch_polygon", "Sketch Polygon",
             "Add a regular polygon with its corners on a construction circle (centre cx,cy, radius to the corners), first "
             "corner at angle degrees. Its sides stay equal. dims=true adds the circle's diameter dimension.",
             with({str("cx", "centre x", false, "0"), str("cy", "centre y", false, "0"), str("sides", "number of sides", true),
                   str("radius", "centre to corner", true), str("angle", "angle of the first corner, degrees", false, "0"),
                   boolean("dims", "add a diameter dimension on the circle", false)}),
             true},
            sketchCommand(session, [](CadDoc& doc, Feature&, Sketch& sk, const Params& p) {
                const long long n = std::llround(evalRequired(doc, p, "sides"));
                const double r = evalRequired(doc, p, "radius"), a0 = evalParam(doc, p, "angle", 0) * kPi / 180;
                if (n < 3 || n > 64) return Result::failure("sides must be between 3 and 64");
                if (!(r > 0)) return Result::failure("radius must be positive");
                const Vec2 c{evalParam(doc, p, "cx", 0), evalParam(doc, p, "cy", 0)};
                const std::string pc = pointAt(doc, sk, c.x, c.y);
                SketchEntity circle{sk.newId("c"), "circle", {pc}, r, true, false};
                sk.entities.push_back(circle);
                std::vector<std::string> corners, lines;
                for (long long i = 0; i < n; ++i) {
                    const double a = a0 + 2 * kPi * static_cast<double>(i) / static_cast<double>(n);
                    corners.push_back(pointAt(doc, sk, c.x + r * std::cos(a), c.y + r * std::sin(a)));
                }
                for (std::size_t i = 0; i < corners.size(); ++i) {
                    lines.push_back(addLine(doc, sk, corners[i], corners[(i + 1) % corners.size()], false, false));
                    addConstraint(doc, sk, "coincident", {corners[i], circle.id});
                }
                addConstraint(doc, sk, "equal", lines);
                if (boolParam(p, "dims", false)) addConstraint(doc, sk, "diameter", {circle.id}, "2*(" + text(p, "radius") + ")");
                std::string list;
                for (const auto& l : lines) list += (list.empty() ? "" : ",") + l;
                return Result::success("polygon " + list + " on " + circle.id, list).with("entities", list).with("circle", circle.id);
            }));

    reg.add({"cad.sketch_slot", "Sketch Slot",
             "Add a straight slot: two half circles joined by tangent lines. cx,cy is its middle, length the distance "
             "between the two arc centres, width the slot width, angle its direction. dims=true adds length and width.",
             with({str("cx", "middle x", false, "0"), str("cy", "middle y", false, "0"), str("length", "centre to centre", true),
                   str("width", "slot width", true), str("angle", "direction, degrees", false, "0"),
                   boolean("dims", "add the length and width dimensions", false)}),
             true},
            sketchCommand(session, [](CadDoc& doc, Feature&, Sketch& sk, const Params& p) {
                const double len = evalRequired(doc, p, "length"), w = evalRequired(doc, p, "width");
                if (!(len > 0) || !(w > 0)) return Result::failure("length and width must be positive");
                const double ang = evalParam(doc, p, "angle", 0) * kPi / 180;
                const Vec2 c{evalParam(doc, p, "cx", 0), evalParam(doc, p, "cy", 0)}, dir{std::cos(ang), std::sin(ang)}, n{-dir.y, dir.x};
                const Vec2 c1 = c - dir * (len / 2), c2 = c + dir * (len / 2);
                const std::string pc1 = pointAt(doc, sk, c1.x, c1.y), pc2 = pointAt(doc, sk, c2.x, c2.y);
                auto at = [&](Vec2 q) { return pointAt(doc, sk, q.x, q.y); };
                const std::string r0 = at(c2 - n * (w / 2)), r1 = at(c2 + n * (w / 2)), l0 = at(c1 + n * (w / 2)), l1 = at(c1 - n * (w / 2));
                SketchEntity right{sk.newId("a"), "arc", {pc2, r0, r1}, 0, false, false};
                sk.entities.push_back(right);
                SketchEntity left{sk.newId("a"), "arc", {pc1, l0, l1}, 0, false, false};
                sk.entities.push_back(left);
                const std::string top = addLine(doc, sk, r1, l0, false, false), bottom = addLine(doc, sk, l1, r0, false, false);
                for (const auto& l : {top, bottom}) {
                    addConstraint(doc, sk, "tangent", {l, right.id});
                    addConstraint(doc, sk, "tangent", {l, left.id});
                }
                addConstraint(doc, sk, "equal", {right.id, left.id});
                if (boolParam(p, "dims", false)) {
                    addConstraint(doc, sk, "distance", {pc1, pc2}, text(p, "length"));
                    addConstraint(doc, sk, "diameter", {right.id}, text(p, "width"));
                }
                const std::string list = right.id + "," + top + "," + left.id + "," + bottom;
                return Result::success("slot " + list, list).with("entities", list);
            }));

    reg.add({"cad.sketch_spline", "Sketch Spline",
             "Add a smooth spline passing through points x,y,x,y,... (each through point can be constrained like any "
             "other point). closed makes a smooth loop.",
             with({str("points", "x,y,x,y,... at least 2 points", true), boolean("closed", "a closed loop", false),
                   boolean("construction", "construction geometry", false)}),
             true},
            sketchCommand(session, [](CadDoc& doc, Feature&, Sketch& sk, const Params& p) {
                const auto items = splitList(stringParam(p, "points"));
                if (items.size() % 2 || items.size() < 4) return Result::failure("points must be x,y pairs, at least 2 points");
                SketchEntity e{sk.newId("s"), "spline", {}, 0, boolParam(p, "construction", false), boolParam(p, "closed", false)};
                for (std::size_t i = 0; i < items.size(); i += 2) e.points.push_back(pointAt(doc, sk, doc.eval(items[i]), doc.eval(items[i + 1])));
                if (e.closed && e.points.size() < 3) return Result::failure("a closed spline needs at least 3 points");
                sk.entities.push_back(e);
                return Result::success("spline " + e.id, e.id).with("entity", e.id);
            }));

    reg.add({"cad.sketch_fillet", "Sketch Fillet",
             "Round the corner where two lines meet at a point with an arc of radius: the lines are trimmed back, the arc "
             "is tangent to both and gets a radius dimension.",
             with({str("point", "the corner point (e.g. p3 or l1.end)", true), str("radius", "fillet radius", true)}), true},
            sketchCommand(session, [](CadDoc& doc, Feature&, Sketch& sk, const Params& p) {
                const std::string pid = pointId(sk, stringParam(p, "point"));
                const double r = evalRequired(doc, p, "radius");
                if (!(r > 0)) return Result::failure("radius must be positive");
                std::vector<SketchEntity*> lines;
                for (auto& e : sk.entities) {
                    if (e.kind == "line" && (e.points[0] == pid || e.points[1] == pid)) lines.push_back(&e);
                    else if (std::find(e.points.begin(), e.points.end(), pid) != e.points.end()) return Result::failure(pid + " is also used by " + e.id + "; fillet a corner of exactly two lines");
                }
                if (lines.size() != 2) return Result::failure(pid + " must be the corner of exactly two lines (it joins " + std::to_string(lines.size()) + ")");
                const SketchPoint* P = sk.point(pid);
                const Vec2 corner{P->x, P->y};
                auto away = [&](SketchEntity* l) {
                    const SketchPoint* o = sk.point(l->points[0] == pid ? l->points[1] : l->points[0]);
                    return Vec2{o->x, o->y} - corner;
                };
                const Vec2 a1 = away(lines[0]), a2 = away(lines[1]);
                const double l1 = mesh::length(a1), l2 = mesh::length(a2);
                const Vec2 u1 = a1 / l1, u2 = a2 / l2;
                const double phi = std::acos(std::clamp(mesh::dot(u1, u2), -1.0, 1.0));
                if (phi < 1e-6 || phi > kPi - 1e-6) return Result::failure("the lines are in line; there's no corner to round");
                const double t = r / std::tan(phi / 2);
                if (t >= l1 || t >= l2) return Result::failure("the radius is too big for these lines");
                Vec2 bis = u1 + u2;
                bis = bis / mesh::length(bis);
                const Vec2 cen = corner + bis * (r / std::sin(phi / 2)), t1 = corner + u1 * t, t2 = corner + u2 * t;
                SketchPoint q1{sk.newId("p"), t1.x, t1.y}, q2{sk.newId("p"), t2.x, t2.y}, qc{sk.newId("p"), cen.x, cen.y};
                sk.points.insert(sk.points.end(), {q1, q2, qc});
                for (int k = 0; k < 2; ++k) {
                    auto& pts = lines[static_cast<std::size_t>(k)]->points;
                    for (auto& id : pts) {
                        if (id == pid) id = k == 0 ? q1.id : q2.id;
                    }
                }
                const bool ccw = mesh::cross(t1 - cen, t2 - cen) > 0;
                SketchEntity arc{sk.newId("a"), "arc", {qc.id, ccw ? q1.id : q2.id, ccw ? q2.id : q1.id}, 0, false, false};
                const std::string id1 = lines[0]->id, id2 = lines[1]->id;
                sk.entities.push_back(arc);
                // constraints on the old corner point go with it
                std::string dropped;
                sk.constraints.erase(std::remove_if(sk.constraints.begin(), sk.constraints.end(),
                                                    [&](const SketchConstraint& c) {
                                                        bool uses = std::find(c.refs.begin(), c.refs.end(), pid) != c.refs.end();
                                                        if (uses) dropped += (dropped.empty() ? "" : ",") + c.id;
                                                        return uses;
                                                    }),
                                     sk.constraints.end());
                sk.points.erase(std::remove_if(sk.points.begin(), sk.points.end(), [&](const SketchPoint& q) { return q.id == pid; }), sk.points.end());
                addConstraint(doc, sk, "tangent", {id1, arc.id});
                addConstraint(doc, sk, "tangent", {id2, arc.id});
                addConstraint(doc, sk, "radius", {arc.id}, text(p, "radius"));
                Result res = Result::success("fillet " + arc.id + " between " + id1 + " and " + id2 + (dropped.empty() ? "" : " (dropped " + dropped + ")"), arc.id);
                return res.with("entity", arc.id);
            }));

    reg.add({"cad.sketch_move", "Move Sketch Point",
             "Drag a point to x,y: it goes there if its constraints let it, and the rest of the sketch follows (what's "
             "fixed or dimensioned stays). A point that can't move stays where it is.",
             with({str("point", "point to move (p3, l1.end, c1.center, ...)", true), str("x", "new x", true), str("y", "new y", true)}),
             true},
            sketchCommand(session, [](CadDoc& doc, Feature& f, Sketch& sk, const Params& p) {
                const std::string pid = pointId(sk, stringParam(p, "point"));
                const double x = evalRequired(doc, p, "x"), y = evalRequired(doc, p, "y");
                Sketch trial = sk;
                trial.point(pid)->x = x;
                trial.point(pid)->y = y;
                SketchConstraint pin{"#drag", "fixed", {pid}, {}, 0};
                char buf[96];
                std::snprintf(buf, sizeof buf, "%.17g,%.17g", x, y);
                pin.value = buf;
                trial.constraints.push_back(pin);
                const FeatureExtra* fx = doc.featureExtra(f.name);
                bool moved = true;
                try {
                    auto solved = detail::solveSketch(trial, fx && fx->sketch ? fx->sketch->frame : Frame{}, evaluator(doc), 16);
                    for (auto& q : sk.points) {
                        q.x = solved.points.at(q.id).x;
                        q.y = solved.points.at(q.id).y;
                    }
                    for (auto& e : sk.entities) {
                        if (solved.radii.count(e.id)) e.radius = solved.radii.at(e.id);
                    }
                } catch (const std::exception&) {
                    moved = false;
                }
                return Result::success(moved ? "moved " + pid : pid + " can't go there: its constraints hold it", pid).with("moved", moved);
            }));
}

void registerSketchEditCommands(Session& session);
void registerSketchTrimCommands(Session& session);

void registerSketchConstraintCommands(Session& session) {
    auto& reg = session.registry();
    reg.add({"cad.sketch_constrain", "Add Sketch Relation",
             "Add a geometric constraint between sketch items (refs, comma-separated: points p1, l1.start, c1.center, "
             "origin; entities l1, c1, a1; the axes xaxis, yaxis). coincident (points, or a point on a line/circle), "
             "horizontal / vertical (lines or two points), parallel, perpendicular, collinear (lines), tangent (line and "
             "arc/circle, or two arcs/circles), equal (lines, or circles/arcs), fixed (pins a point or entity where it "
             "is), midpoint (point, line), concentric (circles/arcs), symmetric (two points about a line).",
             with({{"type", ParamType::String, "which constraint", true, geometricConstraints(), {}},
                   str("refs", "the items it ties together, comma-separated, e.g. l1,l2 or p3,origin", true)}),
             true},
            sketchCommand(session, [](CadDoc& doc, Feature&, Sketch& sk, const Params& p) {
                const std::string type = stringParam(p, "type");
                const std::string id = addConstraint(doc, sk, type, splitList(stringParam(p, "refs")));
                return Result::success(type + " " + id, id).with("constraint", id);
            }));

    reg.add({"cad.sketch_dimension", "Add Sketch Dimension",
             "Add a driving dimension; value is a number or an expression over the part's parameters, so the sketch "
             "follows them. distance (two points or circle centres, a point and a line, or two parallel lines), "
             "hdistance / vdistance (along sketch x / y: two points, or one line), length (a line), radius / diameter "
             "(circle or arc), angle (two lines, degrees; or one line from the x axis).",
             with({{"type", ParamType::String, "which dimension", true, dimensionalConstraints(), {}},
                   str("refs", "what it measures, comma-separated, e.g. l1 or p1,p2", true), str("value", "the value or expression", true)}),
             true},
            sketchCommand(session, [](CadDoc& doc, Feature&, Sketch& sk, const Params& p) {
                const std::string type = stringParam(p, "type");
                const std::string id = addConstraint(doc, sk, type, splitList(stringParam(p, "refs")), text(p, "value"));
                return Result::success(type + " " + id + " = " + text(p, "value"), id).with("constraint", id);
            }));

    reg.add({"cad.sketch_set", "Edit Sketch Dimension",
             "Change a dimension's value (or a fixed constraint's x,y); the sketch and everything built on it follow.",
             with({str("constraint", "constraint id, e.g. k4 (cad.sketch_info lists them)", true), str("value", "new value or expression", true)}),
             true},
            sketchCommand(session, [](CadDoc& doc, Feature&, Sketch& sk, const Params& p) {
                const std::string id = stringParam(p, "constraint");
                SketchConstraint* c = sk.constraint(id);
                if (!c) return Result::failure("the sketch has no constraint '" + id + "'");
                c->value = text(p, "value");
                SketchConstraint check = *c;
                detail::prepareConstraint(sk, check, evaluator(doc));
                c->aux = check.aux;
                return Result::success(id + " = " + c->value, id).with("constraint", id);
            }));
    registerSketchEditCommands(session);
    registerSketchTrimCommands(session);
}

void registerSketchEditCommands(Session& session) {
    auto& reg = session.registry();
    reg.add({"cad.sketch_delete", "Delete Sketch Item",
             "Delete a constraint, an entity (with the constraints on it and points nothing else uses) or a lone point.",
             with({str("item", "constraint, entity or point id", true)}), true},
            sketchCommand(session, [](CadDoc&, Feature&, Sketch& sk, const Params& p) {
                const std::string id = stringParam(p, "item");
                auto refersTo = [&](const SketchConstraint& c, const std::string& what) {
                    for (const auto& r : c.refs) {
                        if (r == what || r.rfind(what + ".", 0) == 0) return true;
                    }
                    return false;
                };
                std::vector<std::string> gone;
                auto dropConstraintsOn = [&](const std::string& what) {
                    sk.constraints.erase(std::remove_if(sk.constraints.begin(), sk.constraints.end(),
                                                        [&](const SketchConstraint& c) {
                                                            if (!refersTo(c, what)) return false;
                                                            gone.push_back(c.id);
                                                            return true;
                                                        }),
                                         sk.constraints.end());
                };
                if (sk.constraint(id)) {
                    sk.constraints.erase(std::remove_if(sk.constraints.begin(), sk.constraints.end(), [&](const SketchConstraint& c) { return c.id == id; }),
                                         sk.constraints.end());
                    return Result::success("deleted " + id, id);
                }
                auto used = [&](const std::string& pid) {
                    for (const auto& e : sk.entities) {
                        if (std::find(e.points.begin(), e.points.end(), pid) != e.points.end()) return true;
                    }
                    return false;
                };
                if (SketchEntity* e = sk.entity(id)) {
                    const auto pts = e->points;
                    sk.entities.erase(std::remove_if(sk.entities.begin(), sk.entities.end(), [&](const SketchEntity& x) { return x.id == id; }),
                                      sk.entities.end());
                    dropConstraintsOn(id);
                    for (const auto& pid : pts) {
                        if (used(pid)) continue;
                        dropConstraintsOn(pid);
                        sk.points.erase(std::remove_if(sk.points.begin(), sk.points.end(), [&](const SketchPoint& q) { return q.id == pid; }), sk.points.end());
                    }
                } else if (sk.point(id)) {
                    if (used(id)) return Result::failure(id + " belongs to an entity; delete the entity instead");
                    dropConstraintsOn(id);
                    sk.points.erase(std::remove_if(sk.points.begin(), sk.points.end(), [&](const SketchPoint& q) { return q.id == id; }), sk.points.end());
                } else {
                    return Result::failure("the sketch has no item '" + id + "'");
                }
                std::string list;
                for (const auto& g : gone) list += (list.empty() ? "" : ",") + g;
                return Result::success("deleted " + id + (list.empty() ? "" : " and " + list), id).with("also_deleted", list);
            }));

    reg.add({"cad.sketch_construction", "Construction Geometry",
             "Turn an entity into construction geometry (kept for constraints and revolve axes, left out of profiles) or back.",
             with({str("entity", "entity id", true), boolean("on", "construction (true) or normal (false)", true)}), true},
            sketchCommand(session, [](CadDoc&, Feature&, Sketch& sk, const Params& p) {
                SketchEntity* e = sk.entity(stringParam(p, "entity"));
                if (!e) return Result::failure("the sketch has no entity '" + stringParam(p, "entity") + "'");
                e->construction = boolParam(p, "on", true);
                return Result::success(e->id + (e->construction ? " is construction geometry" : " is normal geometry"), e->id);
            }));

    reg.add({"cad.sketch_info", "Sketch Info",
             "Show a sketch: its plane, how defined it is (degrees of freedom, points that can still move), every point "
             "with its solved position, every entity, every constraint (dimensions with their value now), and the closed "
             "profiles it makes (regions with their area and holes) or why it doesn't.",
             with({}), false, true},
            cadCommand(session, [](CadDoc& doc, const Params& p) {
                const std::string name = stringParam(p, "sketch");
                const Feature* f = doc.findFeature(name);
                if (!f || f->type != "sketch") return Result::failure("no sketch called '" + name + "'");
                const FeatureExtra* x = doc.featureExtra(name);
                if (!x || !x->sketch) return Result::failure(name + " isn't built (suppressed?)");
                const SolvedSketch& s = *x->sketch;
                const Frame& fr = s.frame;
                auto v3 = [](const mesh::Vec3& v) { return num(v.x) + "," + num(v.y) + "," + num(v.z); };
                std::string t = name + ": " + s.status() + "; plane origin " + v3(fr.origin) + ", x " + v3(fr.u) + ", y " + v3(fr.v) + ", normal " + v3(fr.n);
                t += "\npoints:";
                for (const auto& [id, q] : s.points) {
                    const bool loose = std::find(s.underDefined.begin(), s.underDefined.end(), id) != s.underDefined.end();
                    t += "\n  " + id + " (" + num(q.x) + ", " + num(q.y) + ")" + (loose ? " free" : "");
                }
                t += "\nentities:";
                for (const auto& e : f->sketch.entities) {
                    t += "\n  " + e.id + " " + e.kind + (e.construction ? " (construction)" : "");
                    for (const auto& pid : e.points) t += " " + pid;
                    if (s.radii.count(e.id)) t += " r=" + num(s.radii.at(e.id));
                }
                t += "\nconstraints:";
                for (const auto& c : f->sketch.constraints) {
                    t += "\n  " + c.id + " " + c.type;
                    for (const auto& r : c.refs) t += " " + r;
                    if (!c.value.empty()) t += " = " + c.value;
                    if (s.measured.count(c.id)) t += " (now " + num(s.measured.at(c.id)) + ")";
                }
                double area = 0;
                t += "\nprofile: " + std::to_string(s.regions.size()) + (s.regions.size() == 1 ? " region" : " regions");
                for (std::size_t i = 0; i < s.regions.size(); ++i) {
                    double a = std::fabs(mesh::loopArea(s.regions[i].outer));
                    for (const auto& h : s.regions[i].holes) a -= std::fabs(mesh::loopArea(h));
                    area += a;
                    t += "\n  " + std::to_string(i + 1) + ": area " + num(a) + ", " + std::to_string(s.regions[i].holes.size()) + " holes";
                }
                if (!s.chains.empty()) t += "\n  " + std::to_string(s.chains.size()) + " open chains";
                if (!s.profileProblem.empty()) t += "\n  problem: " + s.profileProblem;
                std::string loose;
                for (const auto& id : s.underDefined) loose += (loose.empty() ? "" : ",") + id;
                Result r = Result::success(t);
                r.with("doc", stringParam(p, "doc"))
                    .with("sketch", name)
                    .with("status", s.status())
                    .with("dof", static_cast<std::int64_t>(s.dof))
                    .with("fully_defined", s.dof == 0)
                    .with("under_defined", loose)
                    .with("points", static_cast<std::int64_t>(s.points.size()))
                    .with("entities", static_cast<std::int64_t>(f->sketch.entities.size()))
                    .with("constraints", static_cast<std::int64_t>(f->sketch.constraints.size()))
                    .with("regions", static_cast<std::int64_t>(s.regions.size()))
                    .with("loops", static_cast<std::int64_t>(s.loops.size()))
                    .with("chains", static_cast<std::int64_t>(s.chains.size()))
                    .with("area", area)
                    .with("problem", s.profileProblem);
                for (const auto& [id, q] : s.points) r.with(id, NumberList{q.x, q.y});
                for (const auto& [id, v] : s.measured) r.with(id, v);
                attachSketch(r, doc, *f, p);
                return r;
            }, false));

    reg.add({"cad.sketch_geometry", "Sketch Geometry",
             "A sketch's solved geometry as numbers, for drawing it in a window: lines = x1,y1,x2,y2,... segments (arcs and "
             "circles as short segments), construction = the same for construction geometry, points = x,y of every point, "
             "free = points that can still move, profiles = each closed region as a count then x,y pairs, origin/u/v/n = its plane in the part, used = a later feature is built from it; "
             "point_ids/point_xy/point_free, entities (id:kind) with entity_outlines (count then x,y), and dims (id:type) "
             "with dim_values (one per line), dim_at (where to write each), dim_lines and dim_measured, for picking. Without sketch= it "
             "uses the last sketch; sketches lists them all.",
             {docParam(), {"sketch", ParamType::String, "name of a sketch feature (default: the last one)", false}}, false, true},
            cadCommand(session, [](CadDoc& doc, const Params& p) {
                std::string sketches, name = stringParam(p, "sketch");
                for (const auto& f : doc.features) {
                    if (f.type != "sketch") continue;
                    sketches += (sketches.empty() ? "" : ",") + f.name;
                    if (!has(p, "sketch")) name = f.name;
                }
                const Feature* f = doc.findFeature(name);
                if (!f || f->type != "sketch") return Result::failure(name.empty() ? "the part has no sketches yet" : "no sketch called '" + name + "'");
                const FeatureExtra* x = doc.featureExtra(name);
                if (!x || !x->sketch) return Result::failure(name + " isn't built (suppressed?)");
                const SolvedSketch& s = *x->sketch;
                NumberList lines, construction, points, free, profiles;
                for (const auto& e : f->sketch.entities) {
                    const auto pts = detail::entityOutline(e, s, 64);
                    NumberList& out = e.construction ? construction : lines;
                    for (std::size_t i = 0; i + 1 < pts.size(); ++i) out.insert(out.end(), {pts[i].x, pts[i].y, pts[i + 1].x, pts[i + 1].y});
                }
                for (const auto& [id, q] : s.points) {
                    const bool loose = std::find(s.underDefined.begin(), s.underDefined.end(), id) != s.underDefined.end();
                    (loose ? free : points).insert((loose ? free : points).end(), {q.x, q.y});
                }
                for (const auto& region : s.regions) {
                    profiles.push_back(static_cast<double>(region.outer.size()));
                    for (const auto& q : region.outer) profiles.insert(profiles.end(), {q.x, q.y});
                }
                // whether a later feature is built from it (fields naming it, alone or in a list)
                auto mentions = [&name](const std::string& value) {
                    for (const auto& item : splitList(value))
                        if (item == name) return true;
                    return false;
                };
                bool used = false;
                for (std::size_t i = static_cast<std::size_t>(doc.featureIndex(name)) + 1; i < doc.features.size() && !used; ++i)
                    for (const auto& [field, value] : doc.features[i].values) used = used || mentions(value);
                // for picking and dragging in a window: every point by id, every entity's outline, and each
                // dimension with where its value is written and what it measures
                std::string pointIds, entityIds, dimIds, dimValues;
                NumberList pointXy, pointFree, entityOutlines, dimAt, dimMeasured, dimLines;
                for (const auto& [id, q] : s.points) {
                    pointIds += (pointIds.empty() ? "" : ",") + id;
                    pointXy.insert(pointXy.end(), {q.x, q.y});
                    pointFree.push_back(std::find(s.underDefined.begin(), s.underDefined.end(), id) != s.underDefined.end() ? 1 : 0);
                }
                for (const auto& e : f->sketch.entities) {
                    entityIds += (entityIds.empty() ? "" : ",") + e.id + ":" + e.kind;
                    const auto pts = detail::entityOutline(e, s, 64);
                    entityOutlines.push_back(static_cast<double>(pts.size()));
                    for (const auto& q : pts) entityOutlines.insert(entityOutlines.end(), {q.x, q.y});
                }
                auto at2 = [&](const std::string& ref, mesh::Vec2& out) {
                    if (ref == "origin") return out = {0, 0}, true;
                    try {
                        const std::string id = pointId(f->sketch, ref);
                        auto it = s.points.find(id);
                        if (it == s.points.end()) return false;
                        out = it->second;
                        return true;
                    } catch (const std::invalid_argument&) {
                        return false;
                    }
                };
                for (const auto& c : f->sketch.constraints) {
                    if (std::find(dimensionalConstraints().begin(), dimensionalConstraints().end(), c.type) == dimensionalConstraints().end()) continue;
                    mesh::Vec2 a{}, b{};
                    std::vector<mesh::Vec2> ends;
                    for (const auto& ref : c.refs) {
                        mesh::Vec2 q;
                        if (at2(ref, q)) {
                            ends.push_back(q);
                            continue;
                        }
                        const SketchEntity* e = f->sketch.entity(ref);
                        if (!e) continue;
                        if (e->kind == "line" && e->points.size() == 2 && s.points.count(e->points[0]) && s.points.count(e->points[1])) {
                            ends.push_back(s.points.at(e->points[0]));
                            ends.push_back(s.points.at(e->points[1]));
                        } else if ((e->kind == "circle" || e->kind == "arc") && s.points.count(e->points[0])) {
                            const mesh::Vec2 ctr = s.points.at(e->points[0]);
                            const double r = s.radii.count(e->id) ? s.radii.at(e->id) : e->radius;
                            ends.push_back(ctr);
                            ends.push_back({ctr.x + r * 0.7071, ctr.y + r * 0.7071});
                        }
                    }
                    if (ends.empty()) continue;
                    a = ends.front();
                    b = ends.size() > 1 ? ends[1] : ends.front();
                    if (c.type == "angle") b = a;  // written at the first line's start
                    dimIds += (dimIds.empty() ? "" : ",") + c.id + ":" + c.type;
                    dimValues += (dimValues.empty() ? "" : "\n") + c.value;
                    dimAt.insert(dimAt.end(), {(a.x + b.x) / 2, (a.y + b.y) / 2});
                    dimLines.insert(dimLines.end(), {a.x, a.y, b.x, b.y});
                    dimMeasured.push_back(s.measured.count(c.id) ? s.measured.at(c.id) : 0);
                }
                Result r = Result::success(name + ": " + s.status(), name);
                r.with("doc", stringParam(p, "doc"))
                    .with("sketch", name)
                    .with("point_ids", pointIds)
                    .with("point_xy", pointXy)
                    .with("point_free", pointFree)
                    .with("entities", entityIds)
                    .with("entity_outlines", entityOutlines)
                    .with("dims", dimIds)
                    .with("dim_values", dimValues)
                    .with("dim_at", dimAt)
                    .with("dim_lines", dimLines)
                    .with("dim_measured", dimMeasured)
                    .with("used", used)
                    .with("sketches", sketches)
                    .with("status", s.status())
                    .with("dof", static_cast<std::int64_t>(s.dof))
                    .with("regions", static_cast<std::int64_t>(s.regions.size()))
                    .with("lines", lines)
                    .with("construction", construction)
                    .with("points", points)
                    .with("free", free)
                    .with("profiles", profiles)
                    .with("units", doc.units)
                    // where it sits in the part: sketch (x, y) is origin + x*u + y*v, n points the way it extrudes
                    .with("origin", NumberList{s.frame.origin.x, s.frame.origin.y, s.frame.origin.z})
                    .with("u", NumberList{s.frame.u.x, s.frame.u.y, s.frame.u.z})
                    .with("v", NumberList{s.frame.v.x, s.frame.v.y, s.frame.v.z})
                    .with("n", NumberList{s.frame.n.x, s.frame.n.y, s.frame.n.z});
                return r;
            }, false));
}


// ---- trim, extend, offset, mirror ------------------------------------------------------------

namespace {

// An entity's shape, read from its solved points.
struct Curve {
    std::string id, kind;  // line, circle, arc
    Vec2 a, b;             // line ends
    Vec2 c;                // circle / arc centre
    double r = 0, a0 = 0, sweep = 2 * kPi;  // arcs: start angle and ccw sweep
};

double wrap2pi(double a) {
    a = std::fmod(a, 2 * kPi);
    return a < 0 ? a + 2 * kPi : a;
}

std::optional<Curve> curveOf(const Sketch& sk, const SketchEntity& e) {
    auto P = [&](std::size_t i) {
        const SketchPoint* q = sk.point(e.points[i]);
        return q ? Vec2{q->x, q->y} : Vec2{};
    };
    Curve c;
    c.id = e.id;
    c.kind = e.kind;
    if (e.kind == "line" && e.points.size() == 2) {
        c.a = P(0), c.b = P(1);
    } else if (e.kind == "circle" && !e.points.empty()) {
        c.c = P(0), c.r = e.radius;
    } else if (e.kind == "arc" && e.points.size() == 3) {
        c.c = P(0);
        const Vec2 s = P(1), t = P(2);
        c.r = mesh::length(s - c.c);
        c.a0 = std::atan2(s.y - c.c.y, s.x - c.c.x);
        c.sweep = wrap2pi(std::atan2(t.y - c.c.y, t.x - c.c.x) - c.a0);
        if (c.sweep < 1e-12) c.sweep = 2 * kPi;
    } else {
        return std::nullopt;
    }
    return c;
}

// Where a point sits along a curve: 0..1 on lines, radians from the start on arcs and circles.
double paramOf(const Curve& c, Vec2 p) {
    if (c.kind == "line") {
        const Vec2 d = c.b - c.a;
        return mesh::dot(p - c.a, d) / std::max(1e-300, mesh::dot(d, d));
    }
    return wrap2pi(std::atan2(p.y - c.c.y, p.x - c.c.x) - c.a0);
}
Vec2 pointOf(const Curve& c, double t) {
    if (c.kind == "line") return c.a + (c.b - c.a) * t;
    return c.c + Vec2{std::cos(c.a0 + t), std::sin(c.a0 + t)} * c.r;
}
// Whether a parameter is on the drawn part of the curve.
bool onCurve(const Curve& c, double t, double tol = 1e-9) {
    if (c.kind == "line") return t >= -tol && t <= 1 + tol;
    if (c.kind == "circle") return true;
    return t <= c.sweep + tol || t >= 2 * kPi - tol;
}

// Crossings of two curves as infinite lines / full circles.
std::vector<Vec2> crossings(const Curve& p, const Curve& q) {
    std::vector<Vec2> out;
    auto lineCircle = [&](Vec2 a, Vec2 b, Vec2 c, double r) {
        const Vec2 d = b - a, f = a - c;
        const double A = mesh::dot(d, d), B = 2 * mesh::dot(f, d), C = mesh::dot(f, f) - r * r;
        const double disc = B * B - 4 * A * C;
        if (A < 1e-300 || disc < -1e-12 * std::max(1.0, B * B)) return;
        const double s = std::sqrt(std::max(0.0, disc));
        out.push_back(a + d * ((-B - s) / (2 * A)));
        if (s > 1e-12) out.push_back(a + d * ((-B + s) / (2 * A)));
    };
    if (p.kind == "line" && q.kind == "line") {
        const Vec2 d1 = p.b - p.a, d2 = q.b - q.a;
        const double den = mesh::cross(d1, d2);
        if (std::fabs(den) < 1e-12 * mesh::length(d1) * mesh::length(d2)) return out;
        const double t = mesh::cross(q.a - p.a, d2) / den;
        out.push_back(p.a + d1 * t);
    } else if (p.kind == "line") {
        lineCircle(p.a, p.b, q.c, q.r);
    } else if (q.kind == "line") {
        lineCircle(q.a, q.b, p.c, p.r);
    } else {
        const Vec2 d = q.c - p.c;
        const double dist = mesh::length(d);
        if (dist < 1e-12 || dist > p.r + q.r + 1e-9 || dist < std::fabs(p.r - q.r) - 1e-9) return out;
        const double a = (p.r * p.r - q.r * q.r + dist * dist) / (2 * dist);
        const double h = std::sqrt(std::max(0.0, p.r * p.r - a * a));
        const Vec2 m = p.c + d * (a / dist), n{-d.y / dist, d.x / dist};
        out.push_back(m + n * h);
        if (h > 1e-12) out.push_back(m - n * h);
    }
    return out;
}

double distTo(const Curve& c, Vec2 p) {
    if (c.kind == "line") {
        const double t = std::clamp(paramOf(c, p), 0.0, 1.0);
        return mesh::length(p - pointOf(c, t));
    }
    const double radial = std::fabs(mesh::length(p - c.c) - c.r);
    if (c.kind == "circle" || onCurve(c, paramOf(c, p))) return radial;
    return std::min(mesh::length(p - pointOf(c, 0)), mesh::length(p - pointOf(c, c.sweep)));
}

// The entity nearest to x,y (or the one named in entity=).
SketchEntity* pickEntity(Sketch& sk, const Params& p, Vec2 at, bool needPoint) {
    if (has(p, "entity")) {
        SketchEntity* e = sk.entity(stringParam(p, "entity"));
        if (!e) throw std::invalid_argument("the sketch has no entity '" + stringParam(p, "entity") + "'");
        return e;
    }
    if (!needPoint) throw std::invalid_argument("give entity= or x,y near it");
    SketchEntity* best = nullptr;
    double bestD = 1e300;
    for (auto& e : sk.entities) {
        auto c = curveOf(sk, e);
        if (!c) continue;
        const double d = distTo(*c, at);
        if (d < bestD) bestD = d, best = &e;
    }
    if (!best) throw std::invalid_argument("there's nothing to pick in the sketch");
    return best;
}

bool pointUsed(const Sketch& sk, const std::string& pid, const std::string& except = {}) {
    for (const auto& e : sk.entities) {
        if (e.id == except) continue;
        if (std::find(e.points.begin(), e.points.end(), pid) != e.points.end()) return true;
    }
    return false;
}

bool refers(const SketchConstraint& c, const std::string& what) {
    for (const auto& r : c.refs) {
        if (r == what || r.rfind(what + ".", 0) == 0) return true;
    }
    return false;
}

// Drops constraints on `what` of the given types (all when empty); returns their ids.
std::string dropOn(Sketch& sk, const std::string& what, const std::vector<std::string>& types = {}) {
    std::string gone;
    sk.constraints.erase(std::remove_if(sk.constraints.begin(), sk.constraints.end(),
                                        [&](const SketchConstraint& c) {
                                            if (!refers(c, what)) return false;
                                            if (!types.empty() && std::find(types.begin(), types.end(), c.type) == types.end()) return false;
                                            gone += (gone.empty() ? "" : ",") + c.id;
                                            return true;
                                        }),
                         sk.constraints.end());
    return gone;
}

void dropPointIfLoose(Sketch& sk, const std::string& pid) {
    if (pointUsed(sk, pid)) return;
    dropOn(sk, pid);
    sk.points.erase(std::remove_if(sk.points.begin(), sk.points.end(), [&](const SketchPoint& q) { return q.id == pid; }), sk.points.end());
}

void removeEntity(Sketch& sk, const std::string& id) {
    const SketchEntity* e = sk.entity(id);
    if (!e) return;
    const auto pts = e->points;
    sk.entities.erase(std::remove_if(sk.entities.begin(), sk.entities.end(), [&](const SketchEntity& x) { return x.id == id; }), sk.entities.end());
    dropOn(sk, id);
    for (const auto& pid : pts) dropPointIfLoose(sk, pid);
}

// A point at a crossing, held on the curve it was cut against.
std::string cutPoint(const CadDoc& doc, Sketch& sk, Vec2 at, const std::string& against) {
    const std::size_t before = sk.points.size();
    const std::string id = pointAt(doc, sk, at.x, at.y);
    if (sk.points.size() > before && !against.empty()) {
        const SketchEntity* o = sk.entity(against);
        if (o && std::find(o->points.begin(), o->points.end(), id) == o->points.end()) addConstraint(doc, sk, "coincident", {id, against});
    }
    return id;
}

// "point on curve" relations that became "point is the curve's end" say the same thing twice.
void dropEndOnCurve(Sketch& sk) {
    sk.constraints.erase(std::remove_if(sk.constraints.begin(), sk.constraints.end(),
                                        [&](const SketchConstraint& c) {
                                            if (c.type != "coincident" || c.refs.size() != 2) return false;
                                            for (int k = 0; k < 2; ++k) {
                                                const SketchEntity* e = sk.entity(c.refs[static_cast<std::size_t>(k)]);
                                                const std::string& pid = c.refs[static_cast<std::size_t>(1 - k)];
                                                if (!e || !sk.point(pid)) continue;
                                                const auto first = e->kind == "arc" ? e->points.begin() + 1 : e->points.begin();
                                                if (e->kind != "circle" && std::find(first, e->points.end(), pid) != e->points.end()) return true;
                                            }
                                            return false;
                                        }),
                         sk.constraints.end());
}

Vec2 reflect(Vec2 p, Vec2 a, Vec2 b) {
    const Vec2 d = b - a;
    const double t = mesh::dot(p - a, d) / std::max(1e-300, mesh::dot(d, d));
    const Vec2 foot = a + d * t;
    return foot * 2 - p;
}

}  // namespace

void registerSketchTrimCommands(Session& session) {
    auto& reg = session.registry();

    reg.add({"cad.sketch_trim", "Trim Sketch",
             "Cut away the piece of a line, arc or circle between the nearest places where other sketch geometry crosses "
             "it, on the side you click (x,y), like Power Trim in SolidWorks. A piece with no crossings is deleted; a "
             "circle cut twice becomes an arc. Cut ends stay on the geometry that cut them.",
             with({str("x", "a point on the piece to remove", true), str("y", "y of that point", true),
                   str("entity", "the entity to trim (default: the one nearest to x,y)")}),
             true},
            sketchCommand(session, [](CadDoc& doc, Feature&, Sketch& sk, const Params& p) {
                const Vec2 at{evalRequired(doc, p, "x"), evalRequired(doc, p, "y")};
                SketchEntity* e = pickEntity(sk, p, at, true);
                const auto curve = curveOf(sk, *e);
                if (!curve) return Result::failure(e->id + " is a " + e->kind + "; trim lines, arcs and circles");
                const Curve c = *curve;
                const std::string id = e->id;
                const double tc = paramOf(c, at);
                // every crossing with the rest of the sketch, as (parameter, the entity it crosses)
                std::vector<std::pair<double, std::string>> cuts;
                const double span = c.kind == "line" ? 1.0 : c.kind == "arc" ? c.sweep : 2 * kPi;
                const double eps = 1e-9;
                for (const auto& o : sk.entities) {
                    if (o.id == id) continue;
                    auto oc = curveOf(sk, o);
                    if (!oc) continue;
                    for (const Vec2& x : crossings(c, *oc)) {
                        if (!onCurve(*oc, paramOf(*oc, x))) continue;
                        double t = paramOf(c, x);
                        if (c.kind == "arc" && t > c.sweep + 1e-9) continue;
                        if (c.kind == "line" && (t < -1e-9 || t > 1 + 1e-9)) continue;
                        if (c.kind != "circle" && (t <= eps || t >= span - eps)) continue;  // its own ends
                        cuts.push_back({t, o.id});
                    }
                }
                std::sort(cuts.begin(), cuts.end());
                if (c.kind == "circle") {
                    if (cuts.size() < 2) {
                        removeEntity(sk, id);
                        return Result::success("deleted " + id + " (fewer than two crossings)", id).with("deleted", true);
                    }
                    // the gap holding the click, going round: from `lo` (before it) to `hi` (after it)
                    std::size_t hiI = 0;
                    while (hiI < cuts.size() && cuts[hiI].first <= tc) ++hiI;
                    const auto hi = cuts[hiI % cuts.size()], lo = cuts[(hiI + cuts.size() - 1) % cuts.size()];
                    const std::string ps = cutPoint(doc, sk, pointOf(c, hi.first), hi.second);
                    const std::string pe = cutPoint(doc, sk, pointOf(c, lo.first), lo.second);
                    e = sk.entity(id);
                    e->kind = "arc";
                    e->points = {e->points[0], ps, pe};
                    dropEndOnCurve(sk);
                    return Result::success("trimmed " + id + " to an arc", id).with("entity", id);
                }
                const std::pair<double, std::string>* lo = nullptr;
                const std::pair<double, std::string>* hi = nullptr;
                for (const auto& k : cuts) {
                    if (k.first < tc) lo = &k;
                    else if (!hi) hi = &k;
                }
                if (!lo && !hi) {
                    removeEntity(sk, id);
                    return Result::success("deleted " + id + " (nothing crosses it)", id).with("deleted", true);
                }
                const std::string dropped = dropOn(sk, id, {"length", "distance", "radius", "diameter", "midpoint"});
                const std::size_t first = c.kind == "line" ? 0 : 1, last = c.kind == "line" ? 1 : 2;
                std::string made;
                if (lo && hi) {
                    // keep both ends: this entity ends at lo, a new one starts at hi
                    const std::string pLo = cutPoint(doc, sk, pointOf(c, lo->first), lo->second);
                    const std::string pHi = cutPoint(doc, sk, pointOf(c, hi->first), hi->second);
                    e = sk.entity(id);
                    const std::string oldEnd = e->points[last];
                    SketchEntity piece = *e;
                    piece.id = sk.newId(c.kind == "line" ? "l" : "a");
                    piece.points[first] = pHi;
                    piece.points[last] = oldEnd;
                    e->points[last] = pLo;
                    sk.entities.push_back(piece);
                    made = piece.id;
                    // relations that still hold for both halves
                    std::vector<SketchConstraint> copies;
                    for (const auto& k : sk.constraints) {
                        if (k.refs.size() == 1 && k.refs[0] == id && (k.type == "horizontal" || k.type == "vertical")) copies.push_back(k);
                    }
                    for (auto k : copies) addConstraint(doc, sk, k.type, {made});
                    if (c.kind == "arc") addConstraint(doc, sk, "concentric", {id, made}), addConstraint(doc, sk, "equal", {id, made});
                    else addConstraint(doc, sk, "collinear", {id, made});
                } else if (hi) {
                    const std::string pHi = cutPoint(doc, sk, pointOf(c, hi->first), hi->second);
                    e = sk.entity(id);
                    const std::string old = e->points[first];
                    e->points[first] = pHi;
                    dropPointIfLoose(sk, old);
                } else {
                    const std::string pLo = cutPoint(doc, sk, pointOf(c, lo->first), lo->second);
                    e = sk.entity(id);
                    const std::string old = e->points[last];
                    e->points[last] = pLo;
                    dropPointIfLoose(sk, old);
                }
                dropEndOnCurve(sk);
                Result r = Result::success("trimmed " + id + (made.empty() ? "" : ", split off " + made) + (dropped.empty() ? "" : " (dropped " + dropped + ")"), id);
                return r.with("entity", id).with("new_entity", made).with("dropped", dropped);
            }));

    reg.add({"cad.sketch_extend", "Extend Sketch Line",
             "Lengthen a line from one end until it meets the next sketch line, arc or circle in its way. The end nearest "
             "x,y is extended (or end=start/end); a joined end can't be extended.",
             with({str("entity", "the line (default: the one nearest to x,y)"), str("x", "near the end to extend"), str("y", "y of that point"),
                   {"end", ParamType::String, "which end", false, {"nearest", "start", "end"}, std::string("nearest")}}),
             true},
            sketchCommand(session, [](CadDoc& doc, Feature&, Sketch& sk, const Params& p) {
                const bool click = has(p, "x") && has(p, "y");
                const Vec2 at = click ? Vec2{evalRequired(doc, p, "x"), evalRequired(doc, p, "y")} : Vec2{};
                SketchEntity* e = pickEntity(sk, p, at, click);
                if (e->kind != "line") return Result::failure(e->id + " is a " + e->kind + "; extend works on lines");
                const Curve c = *curveOf(sk, *e);
                std::string which = stringParam(p, "end", "nearest");
                if (which == "nearest") which = click && mesh::length(at - c.a) < mesh::length(at - c.b) ? "start" : "end";
                const std::size_t idx = which == "start" ? 0 : 1;
                const std::string pid = e->points[idx];
                if (pointUsed(sk, pid, e->id)) return Result::failure("that end of " + e->id + " is joined to other geometry");
                double best = 1e300;
                Vec2 hit;
                std::string against;
                for (const auto& o : sk.entities) {
                    if (o.id == e->id) continue;
                    auto oc = curveOf(sk, o);
                    if (!oc) continue;
                    for (const Vec2& x : crossings(c, *oc)) {
                        if (!onCurve(*oc, paramOf(*oc, x), 1e-7)) continue;
                        const double t = paramOf(c, x);
                        const double beyond = idx == 1 ? t - 1 : -t;  // how far past the end, in line lengths
                        if (beyond <= 1e-9 || beyond >= best) continue;
                        best = beyond, hit = x, against = o.id;
                    }
                }
                if (against.empty()) return Result::failure("nothing in the sketch is in the way of " + e->id + "'s " + which);
                const std::string dropped = dropOn(sk, e->id, {"length"});
                SketchPoint* q = sk.point(pid);
                q->x = hit.x;
                q->y = hit.y;
                const std::string id = e->id;
                addConstraint(doc, sk, "coincident", {pid, against});
                return Result::success("extended " + id + " to " + against + (dropped.empty() ? "" : " (dropped " + dropped + ")"), id).with("entity", id).with("to", against);
            }));

    reg.add({"cad.sketch_offset", "Offset Sketch",
             "Copy lines, arcs and circles at a distance, like Offset Entities: joined lines stay joined at the corners, "
             "arcs and circles become concentric copies. On a closed loop a positive distance goes outward; otherwise "
             "it goes to the left of the chain (negative flips it).",
             with({str("entities", "entity ids, comma-separated (a chain or loop)", true), str("distance", "how far (expression)", true),
                   boolean("construction", "make the copies construction geometry", false)}),
             true},
            sketchCommand(session, [](CadDoc& doc, Feature&, Sketch& sk, const Params& p) {
                const double dist = evalRequired(doc, p, "distance");
                if (std::fabs(dist) < 1e-12) return Result::failure("distance can't be 0");
                std::vector<std::string> ids = splitList(stringParam(p, "entities"));
                std::vector<SketchEntity> sel;
                for (const auto& id : ids) {
                    const SketchEntity* e = sk.entity(id);
                    if (!e) return Result::failure("the sketch has no entity '" + id + "'");
                    if (e->kind != "line" && e->kind != "arc" && e->kind != "circle") return Result::failure(id + " is a " + e->kind + "; offset lines, arcs and circles");
                    sel.push_back(*e);
                }
                if (sel.empty()) return Result::failure("give entities to offset");
                // order the open curves into a chain: (index, forwards)
                auto ends = [](const SketchEntity& e) {
                    return e.kind == "line" ? std::pair{e.points[0], e.points[1]} : std::pair{e.points[1], e.points[2]};
                };
                std::vector<std::pair<std::size_t, bool>> chain;
                std::vector<bool> used(sel.size(), false);
                std::vector<std::size_t> circles;
                std::size_t open = 0;
                for (std::size_t i = 0; i < sel.size(); ++i) {
                    if (sel[i].kind == "circle") {
                        circles.push_back(i);
                        used[i] = true;
                    } else {
                        ++open;
                    }
                }
                auto degree = [&](const std::string& pid) {
                    int n = 0;
                    for (std::size_t i = 0; i < sel.size(); ++i) {
                        if (sel[i].kind == "circle") continue;
                        const auto [a, b] = ends(sel[i]);
                        n += (a == pid) + (b == pid);
                    }
                    return n;
                };
                if (open > 0) {
                    // start at a free end when there is one
                    std::size_t start = sel.size();
                    bool fwd = true;
                    for (std::size_t i = 0; i < sel.size() && start == sel.size(); ++i) {
                        if (sel[i].kind == "circle") continue;
                        const auto [a, b] = ends(sel[i]);
                        if (degree(a) == 1) start = i, fwd = true;
                        else if (degree(b) == 1) start = i, fwd = false;
                    }
                    if (start == sel.size()) {
                        for (std::size_t i = 0; i < sel.size(); ++i) {
                            if (sel[i].kind != "circle") {
                                start = i;
                                break;
                            }
                        }
                    }
                    chain.push_back({start, fwd});
                    used[start] = true;
                    for (;;) {
                        const auto [i, f] = chain.back();
                        const auto [a, b] = ends(sel[i]);
                        const std::string tip = f ? b : a;
                        bool found = false;
                        for (std::size_t j = 0; j < sel.size() && !found; ++j) {
                            if (used[j]) continue;
                            const auto [c0, c1] = ends(sel[j]);
                            if (c0 == tip) chain.push_back({j, true}), used[j] = found = true;
                            else if (c1 == tip) chain.push_back({j, false}), used[j] = found = true;
                        }
                        if (!found) break;
                    }
                    if (chain.size() != open) return Result::failure("the lines and arcs must make one connected chain or loop");
                }
                auto P = [&](const std::string& id) {
                    const SketchPoint* q = sk.point(id);
                    return Vec2{q->x, q->y};
                };
                bool closed = false;
                double side = 1;  // +1: offset to the right of the chain's direction
                if (!chain.empty()) {
                    const auto [ia, fa] = chain.front();
                    const auto [ib, fb] = chain.back();
                    const auto ea = ends(sel[ia]), eb = ends(sel[ib]);
                    closed = chain.size() > 1 && (fa ? ea.first : ea.second) == (fb ? eb.second : eb.first);
                    if (closed) {
                        // signed area of the loop (arcs as their chords is enough to tell the turn)
                        double area = 0;
                        for (const auto& [i, f] : chain) {
                            const auto [a, b] = ends(sel[i]);
                            const Vec2 s = P(f ? a : b), t = P(f ? b : a);
                            area += mesh::cross(s, t);
                        }
                        side = area > 0 ? 1 : -1;  // counter-clockwise: the right is outside
                    } else {
                        side = -1;  // open chains: positive goes left
                    }
                }
                const double D = dist * side;  // to the right of the chain direction
                const bool cons = boolParam(p, "construction", false);
                // offset copies of each piece, as (start, end) positions along the chain
                struct Piece {
                    Vec2 s, e, c;
                    double r = 0;
                    bool line = true, fwd = true;
                };
                std::vector<Piece> pieces;
                for (const auto& [i, f] : chain) {
                    const SketchEntity& en = sel[i];
                    Piece pc;
                    pc.fwd = f;
                    if (en.kind == "line") {
                        const Vec2 a = P(f ? en.points[0] : en.points[1]), b = P(f ? en.points[1] : en.points[0]);
                        const Vec2 d = (b - a) / std::max(1e-300, mesh::length(b - a));
                        const Vec2 right{d.y, -d.x};
                        pc.s = a + right * D;
                        pc.e = b + right * D;
                    } else {
                        pc.line = false;
                        pc.c = P(en.points[0]);
                        const double r0 = mesh::length(P(en.points[1]) - pc.c);
                        pc.r = r0 + (f ? D : -D);  // walking an arc forwards (ccw), its right is away from the centre
                        if (pc.r <= 1e-9) return Result::failure("the offset is bigger than the radius of " + en.id);
                        const Vec2 s = P(f ? en.points[1] : en.points[2]), t = P(f ? en.points[2] : en.points[1]);
                        pc.s = pc.c + (s - pc.c) * (pc.r / r0);
                        pc.e = pc.c + (t - pc.c) * (pc.r / r0);
                    }
                    pieces.push_back(pc);
                }
                // corners between two lines meet where their offsets cross
                auto meet = [&](std::size_t k, std::size_t m) {
                    Piece &A = pieces[k], &B = pieces[m];
                    if (!A.line || !B.line) return;
                    const Vec2 d1 = A.e - A.s, d2 = B.e - B.s;
                    const double den = mesh::cross(d1, d2);
                    if (std::fabs(den) < 1e-12 * mesh::length(d1) * mesh::length(d2)) return;
                    const Vec2 x = A.s + d1 * (mesh::cross(B.s - A.s, d2) / den);
                    A.e = x;
                    B.s = x;
                };
                for (std::size_t k = 0; k + 1 < pieces.size(); ++k) meet(k, k + 1);
                if (closed && pieces.size() > 1) meet(pieces.size() - 1, 0);
                std::string made;
                for (std::size_t k = 0; k < pieces.size(); ++k) {
                    const Piece& pc = pieces[k];
                    const SketchEntity& src = sel[chain[k].first];
                    const std::string a = pointAt(doc, sk, pc.s.x, pc.s.y), b = pointAt(doc, sk, pc.e.x, pc.e.y);
                    std::string id;
                    if (pc.line) {
                        id = addLine(doc, sk, pc.fwd ? a : b, pc.fwd ? b : a, cons, false);
                        addConstraint(doc, sk, "parallel", {src.id, id});
                    } else {
                        const std::string c = pointAt(doc, sk, pc.c.x, pc.c.y);
                        SketchEntity arc{sk.newId("a"), "arc", {c, pc.fwd ? a : b, pc.fwd ? b : a}, 0, cons, false};
                        sk.entities.push_back(arc);
                        id = arc.id;
                    }
                    made += (made.empty() ? "" : ",") + id;
                }
                for (std::size_t i : circles) {
                    const SketchEntity& src = sel[i];
                    const double r = src.radius + dist;
                    if (r <= 1e-9) return Result::failure("the offset is bigger than the radius of " + src.id);
                    SketchEntity circle{sk.newId("c"), "circle", {src.points[0]}, r, cons, false};
                    sk.entities.push_back(circle);
                    made += (made.empty() ? "" : ",") + circle.id;
                }
                return Result::success("offset " + std::to_string(sel.size()) + " item" + (sel.size() == 1 ? "" : "s") + " by " + text(p, "distance") + ": " + made, made)
                    .with("entities", made);
            }));

    reg.add({"cad.sketch_mirror", "Mirror Sketch",
             "Mirror lines, arcs, circles, splines and points about a line (often a construction centreline): the copies "
             "are tied to the originals with symmetric relations, so editing one side moves the other.",
             with({str("entities", "entity or point ids, comma-separated", true), str("about", "the line to mirror about", true)}), true},
            sketchCommand(session, [](CadDoc& doc, Feature&, Sketch& sk, const Params& p) {
                const SketchEntity* axis = sk.entity(stringParam(p, "about"));
                if (!axis || axis->kind != "line") return Result::failure("about= must be a line of the sketch");
                const std::string axisId = axis->id;
                const Vec2 a{sk.point(axis->points[0])->x, sk.point(axis->points[0])->y}, b{sk.point(axis->points[1])->x, sk.point(axis->points[1])->y};
                std::map<std::string, std::string> twin;  // point -> its mirror
                auto mirrorPoint = [&](const std::string& pid) {
                    if (auto it = twin.find(pid); it != twin.end()) return it->second;
                    const SketchPoint* q = sk.point(pid);
                    const Vec2 m = reflect({q->x, q->y}, a, b);
                    const std::string id = pointAt(doc, sk, m.x, m.y);
                    twin[pid] = id;
                    if (id != pid) addConstraint(doc, sk, "symmetric", {pid, id, axisId});
                    return id;
                };
                std::string made;
                for (const auto& id : splitList(stringParam(p, "entities"))) {
                    if (id == axisId) continue;
                    if (sk.point(id)) {
                        made += (made.empty() ? "" : ",") + mirrorPoint(id);
                        continue;
                    }
                    const SketchEntity* e = sk.entity(id);
                    if (!e) return Result::failure("the sketch has no entity or point '" + id + "'");
                    SketchEntity copy = *e;
                    copy.id = sk.newId(e->kind == "line" ? "l" : e->kind == "circle" ? "c" : e->kind == "arc" ? "a" : "s");
                    for (auto& pid : copy.points) pid = mirrorPoint(pid);
                    if (copy.kind == "arc") std::swap(copy.points[1], copy.points[2]);  // a mirror turns counter-clockwise into clockwise
                    if (copy.kind == "line" && copy.points[0] == copy.points[1]) continue;
                    sk.entities.push_back(copy);
                    if (copy.kind == "circle" || copy.kind == "arc") addConstraint(doc, sk, "equal", {id, copy.id});
                    made += (made.empty() ? "" : ",") + copy.id;
                }
                if (made.empty()) return Result::failure("nothing to mirror");
                return Result::success("mirrored about " + axisId + ": " + made, made).with("entities", made);
            }));
}

}  // namespace wetwaresuite::cad
