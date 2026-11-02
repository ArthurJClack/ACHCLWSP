// cad.sketch_* commands: drawing sketch geometry, constraining it, and seeing
// its state. Every edit solves the sketch again (inside the part's rebuild),
// writes the solved positions back, and reports how defined it is.
#include <algorithm>
#include <cmath>

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

}  // namespace wetwaresuite::cad
