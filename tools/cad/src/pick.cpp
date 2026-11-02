// Working on the part in its 3D view, as in Onshape or SolidWorks: cad.pick says what is under a
// point of the view (which body, the face, its outline), so a window can highlight faces, put
// arrows on them and start sketches on them; cad.push_pull pulls a flat face out (or pushes it in)
// by sketching its outline on it and extruding that, so the result stays an editable sketch and
// extrude in the tree.
#include <algorithm>
#include <cmath>
#include <map>
#include <tuple>

#include "blend.hpp"
#include "build.hpp"
#include "cmdutil.hpp"

namespace wetwaresuite::cad {

using namespace cmd;
using mesh::Mesh;
using mesh::Vec3;

namespace {

using Key = std::tuple<double, double, double>;
Key key(const Vec3& p) { return {p.x, p.y, p.z}; }

// The flat face a triangle belongs to: the triangles joined to it by edges that lie in its plane,
// and the face's outline loops (outer and holes, in the triangles' winding).
struct Face {
    Vec3 normal, centre;
    double area = 0;
    bool flat = true;  // false: a facet of a curved surface (its neighbours bend away gently)
    std::vector<std::vector<Vec3>> loops;
};

Face faceOf(const Mesh& m, std::size_t start) {
    Face out;
    const std::size_t nt = m.triangleCount();
    out.normal = mesh::normalize(m.faceNormal(start));
    const Vec3 p0 = m.corner(start, 0);
    const Vec3 lo = mesh::bounds(m).min, hi = mesh::bounds(m).max;
    const double tol = 1e-7 * (1 + mesh::length(hi - lo));
    // every edge -> the triangles walking it
    std::map<std::pair<Key, Key>, std::vector<std::size_t>> edges;
    for (std::size_t t = 0; t < nt; ++t)
        for (int k = 0; k < 3; ++k) {
            Key a = key(m.corner(t, k)), b = key(m.corner(t, (k + 1) % 3));
            if (b < a) std::swap(a, b);
            edges[{a, b}].push_back(t);
        }
    auto inPlane = [&](std::size_t t) {
        const Vec3 n = m.faceNormal(t);
        const double l = mesh::length(n);
        if (l == 0 || mesh::dot(n / l, out.normal) < 1 - 1e-6) return false;
        for (int k = 0; k < 3; ++k)
            if (std::fabs(mesh::dot(m.corner(t, k) - p0, out.normal)) > tol) return false;
        return true;
    };
    std::vector<char> seen(nt, 0);
    std::vector<std::size_t> todo{start}, region;
    seen[start] = 1;
    while (!todo.empty()) {
        const std::size_t t = todo.back();
        todo.pop_back();
        region.push_back(t);
        for (int k = 0; k < 3; ++k) {
            Key a = key(m.corner(t, k)), b = key(m.corner(t, (k + 1) % 3));
            if (b < a) std::swap(a, b);
            for (std::size_t u : edges[{a, b}]) {
                if (seen[u]) continue;
                if (inPlane(u)) {
                    seen[u] = 1;
                    todo.push_back(u);
                } else {
                    const Vec3 n = mesh::normalize(m.faceNormal(u));
                    if (mesh::dot(n, out.normal) > std::cos(25 * mesh::kPi / 180)) out.flat = false;  // bends away gently
                }
            }
        }
    }
    // the outline: directed edges no other region triangle walks back
    std::map<std::pair<Key, Key>, int> count;
    Vec3 sum;
    for (std::size_t t : region) {
        const double a = mesh::length(m.faceNormal(t)) / 2;
        out.area += a;
        sum = sum + (m.corner(t, 0) + m.corner(t, 1) + m.corner(t, 2)) * (a / 3);
        for (int k = 0; k < 3; ++k) {
            Key a1 = key(m.corner(t, k)), b1 = key(m.corner(t, (k + 1) % 3));
            if (b1 < a1) std::swap(a1, b1);
            ++count[{a1, b1}];
        }
    }
    out.centre = out.area > 0 ? sum / out.area : p0;
    std::multimap<Key, Key> next;
    for (std::size_t t : region)
        for (int k = 0; k < 3; ++k) {
            const Key a = key(m.corner(t, k)), b = key(m.corner(t, (k + 1) % 3));
            if (count[a < b ? std::make_pair(a, b) : std::make_pair(b, a)] == 1) next.emplace(a, b);
        }
    while (!next.empty()) {
        auto it = next.begin();
        const Key first = it->first;
        Key at = it->second;
        std::vector<Vec3> loop{{std::get<0>(first), std::get<1>(first), std::get<2>(first)}};
        next.erase(it);
        bool closed = false;
        for (std::size_t guard = 0; guard < 100000; ++guard) {
            if (at == first) {
                closed = true;
                break;
            }
            loop.push_back({std::get<0>(at), std::get<1>(at), std::get<2>(at)});
            auto n = next.find(at);
            if (n == next.end()) break;
            at = n->second;
            next.erase(n);
        }
        if (!closed || loop.size() < 3) continue;
        // drop points on straight runs
        std::vector<Vec3> clean;
        for (std::size_t i = 0; i < loop.size(); ++i) {
            const Vec3 &a = loop[(i + loop.size() - 1) % loop.size()], &b = loop[i], &c = loop[(i + 1) % loop.size()];
            const double cr = mesh::length(mesh::cross(b - a, c - b));
            if (cr > 1e-9 * (mesh::length(b - a) * mesh::length(c - b) + 1e-30)) clean.push_back(b);
        }
        if (clean.size() >= 3) out.loops.push_back(std::move(clean));
    }
    return out;
}

// Ray against a triangle: distance along dir, or -1.
double hitTriangle(const Vec3& o, const Vec3& d, const Vec3& a, const Vec3& b, const Vec3& c) {
    const Vec3 e1 = b - a, e2 = c - a, pv = mesh::cross(d, e2);
    const double det = mesh::dot(e1, pv);
    if (std::fabs(det) < 1e-18) return -1;
    const double inv = 1 / det;
    const Vec3 tv = o - a;
    const double u = mesh::dot(tv, pv) * inv;
    if (u < -1e-9 || u > 1 + 1e-9) return -1;
    const Vec3 qv = mesh::cross(tv, e1);
    const double v = mesh::dot(d, qv) * inv;
    if (v < -1e-9 || u + v > 1 + 1e-9) return -1;
    const double t = mesh::dot(e2, qv) * inv;
    return t >= 0 ? t : -1;
}

double pointTriangle(const Vec3& p, const Vec3& a, const Vec3& b, const Vec3& c) {
    // distance from p to the triangle: its plane when the foot is inside, else the nearest edge
    const Vec3 n = mesh::normalize(mesh::cross(b - a, c - a));
    const Vec3 foot = p - n * mesh::dot(p - a, n);
    auto inside = [&](const Vec3& x, const Vec3& y) { return mesh::dot(mesh::cross(y - x, foot - x), n) >= -1e-12; };
    if (inside(a, b) && inside(b, c) && inside(c, a)) return std::fabs(mesh::dot(p - a, n));
    auto seg = [&](const Vec3& x, const Vec3& y) {
        const Vec3 d = y - x;
        const double l2 = mesh::dot(d, d);
        const double t = l2 > 0 ? std::clamp(mesh::dot(p - x, d) / l2, 0.0, 1.0) : 0;
        return mesh::length(p - (x + d * t));
    };
    return std::min({seg(a, b), seg(b, c), seg(c, a)});
}

struct Hit {
    const Body* body = nullptr;
    std::size_t tri = 0;
    Vec3 point;
};

Hit nearestFace(const CadDoc& doc, const Vec3& p, const std::string& only) {
    Hit h;
    double best = 1e300;
    for (const auto& b : doc.bodies) {
        if (!only.empty() && b.feature != only) continue;
        for (std::size_t t = 0; t < b.mesh.triangleCount(); ++t) {
            if (mesh::length(b.mesh.faceNormal(t)) == 0) continue;
            const double d = pointTriangle(p, b.mesh.corner(t, 0), b.mesh.corner(t, 1), b.mesh.corner(t, 2));
            if (d < best) {
                best = d;
                h = {&b, t, p};
            }
        }
    }
    return h;
}

NumberList triple(const Vec3& v) { return {v.x, v.y, v.z}; }
std::string at(const Vec3& v) { return num(v.x) + "," + num(v.y) + "," + num(v.z); }

const char* faceName(const Vec3& n) {
    if (n.z > 1 - 1e-6) return "top";
    if (n.z < -1 + 1e-6) return "bottom";
    if (n.y < -1 + 1e-6) return "front";
    if (n.y > 1 - 1e-6) return "back";
    if (n.x < -1 + 1e-6) return "left";
    if (n.x > 1 - 1e-6) return "right";
    return "slanted";
}

ParamSpec numberSpec(const std::string& name, const std::string& help, double fallback) {
    return {name, ParamType::Number, help, false, {}, fallback};
}

// The ray of a point of the view (u, v and the camera, like app.view) or of ray=x,y,z,dx,dy,dz.
bool viewRay(CadDoc& doc, const Params& p, Vec3& o, Vec3& d, std::string& error, double* pxPerUnit = nullptr) {
    if (has(p, "ray")) {
        const auto items = splitList(stringParam(p, "ray"));
        if (items.size() != 6) {
            error = "ray is x,y,z,dx,dy,dz";
            return false;
        }
        double v[6];
        for (int i = 0; i < 6; ++i) v[i] = doc.eval(items[static_cast<std::size_t>(i)]);
        o = {v[0], v[1], v[2]};
        d = mesh::normalize(Vec3{v[3], v[4], v[5]});
    } else {
        OrbitView view;
        view.yaw = numberParam(p, "yaw", 45);
        view.pitch = numberParam(p, "pitch", 35.26);
        view.zoom = numberParam(p, "zoom", 1);
        view.panX = numberParam(p, "pan_x", 0);
        view.panY = numberParam(p, "pan_y", 0);
        const int w = static_cast<int>(intParam(p, "width", 800)), h = static_cast<int>(intParam(p, "height", 600));
        if (w < 1 || h < 1) {
            error = "width and height must be positive";
            return false;
        }
        const mesh::Projection c = mesh::toProjection(mesh::orbitCamera(doc.mesh, view, w, h, doc.viewOptions()));
        const double a = (numberParam(p, "u", 0.5) * w - w / 2.0) / c.scale + c.cx;
        const double b = (h / 2.0 - numberParam(p, "v", 0.5) * h) / c.scale + c.cy;
        const mesh::Bounds bb = mesh::bounds(doc.mesh);
        const double back = mesh::length(bb.max - bb.min) + std::fabs(mesh::dot(bb.min, c.forward)) +
                            std::fabs(mesh::dot(bb.max, c.forward)) + 1;
        d = c.forward;
        o = c.right * a + c.up * b - d * back;
        if (pxPerUnit) *pxPerUnit = c.scale;
    }
    return true;
}

std::vector<ParamSpec> viewParams() {
    return {numberSpec("u", "0..1 across the view", 0.5), numberSpec("v", "0..1 down the view", 0.5),
            numberSpec("yaw", "view yaw, degrees", 45), numberSpec("pitch", "view pitch, degrees", 35.26),
            numberSpec("zoom", "view zoom", 1), numberSpec("pan_x", "view pan, model radii", 0), numberSpec("pan_y", "view pan", 0),
            {"width", ParamType::Int, "view width, px", false, {}, std::int64_t{800}},
            {"height", ParamType::Int, "view height, px", false, {}, std::int64_t{600}},
            str("ray", "instead of the view: a ray start and direction x,y,z,dx,dy,dz")};
}

// Closest approach of a ray (o, unit d) and a segment ab: the distance between them, and how far
// along the ray (s) and the segment (t, 0..1) it is.
double raySegment(const Vec3& o, const Vec3& d, const Vec3& a, const Vec3& b, double& s, double& t) {
    const Vec3 u = b - a, w = o - a;
    const double uu = mesh::dot(u, u), ud = mesh::dot(u, d), uw = mesh::dot(u, w), dw = mesh::dot(d, w);
    const double den = uu - ud * ud;
    t = den > 1e-18 ? std::clamp((uw - ud * dw) / den, 0.0, 1.0) : 0.0;
    s = std::max(0.0, mesh::dot(a + u * t - o, d));
    return mesh::length(o + d * s - (a + u * t));
}

}  // namespace

void registerPickCommands(Session& session) {
    auto& reg = session.registry();

    reg.add({"cad.pick", "Pick Face",
             "Say what is at a point of the 3D view: the body, the face (its point, normal, whether it is flat, its "
             "outline loops as a count then x,y,z points each, its area and centre) and the body's bounds. Give the "
             "view as u,v (0..1 across and down) with yaw/pitch/zoom/pan_x/pan_y/width/height like app.view, or a "
             "ray=x,y,z,dx,dy,dz. Pass face= on to cad.sketch (on=body) or cad.push_pull.",
             [] {
                 auto ps = viewParams();
                 ps.insert(ps.begin(), docParam());
                 return ps;
             }(),
             false, true},
            cadCommand(session, [](CadDoc& doc, const Params& p) {
                Vec3 o, d;
                if (std::string error; !viewRay(doc, p, o, d, error)) return Result::failure(error);
                Hit hit;
                double best = 1e300;
                for (const auto& b : doc.bodies)
                    for (std::size_t t = 0; t < b.mesh.triangleCount(); ++t) {
                        const double s = hitTriangle(o, d, b.mesh.corner(t, 0), b.mesh.corner(t, 1), b.mesh.corner(t, 2));
                        if (s >= 0 && s < best && mesh::length(b.mesh.faceNormal(t)) > 0) {
                            best = s;
                            hit = {&b, t, o + d * s};
                        }
                    }
                if (!hit.body) {
                    Result r = Result::success("nothing there", false);
                    r.with("doc", stringParam(p, "doc")).with("hit", false);
                    return r;
                }
                const Face f = faceOf(hit.body->mesh, hit.tri);
                NumberList outline;
                for (const auto& loop : f.loops) {
                    outline.push_back(static_cast<double>(loop.size()));
                    for (const Vec3& q : loop) outline.insert(outline.end(), {q.x, q.y, q.z});
                }
                const mesh::Bounds bb = mesh::bounds(hit.body->mesh);
                const std::string side = faceName(f.normal);
                Result r = Result::success((side == "slanted" ? std::string("a") : "the " + side) + (f.flat ? " flat" : " curved") + " face of " +
                                               hit.body->feature + " at " + at(hit.point) + (f.flat ? ", area " + num(f.area) : std::string()),
                                           true);
                r.with("doc", stringParam(p, "doc"))
                    .with("hit", true)
                    .with("body", hit.body->feature)
                    .with("point", triple(hit.point))
                    .with("normal", triple(f.normal))
                    .with("flat", f.flat)
                    .with("face", at(hit.point))
                    .with("side", side)
                    .with("area", f.area)
                    .with("centre", triple(f.centre))
                    .with("outline", outline)
                    .with("bounds", NumberList{bb.min.x, bb.min.y, bb.min.z, bb.max.x, bb.max.y, bb.max.z});
                return r;
            }, false));

    reg.add({"cad.pick_edge", "Pick Edge",
             "Say which sharp edge is at a point of the 3D view (or nearest a ray), as clicking an edge does in SolidWorks "
             "or Onshape: its body, ends, midpoint and whether it is outer or inner, plus the chain of edges that carry on "
             "smoothly from it (all of a rounded corner's run, a whole circle). Pass near= (the chain's midpoints) and "
             "target= (the body) to cad.fillet or cad.chamfer. Hidden edges behind the part are not picked.",
             [] {
                 auto ps = viewParams();
                 ps.insert(ps.begin(), docParam());
                 ps.push_back(numberSpec("pixels", "how close to the edge counts, px (view)", 8));
                 ps.push_back(numberSpec("tolerance", "how close counts, model units (ray; default 1% of the part)", 0));
                 ps.push_back({"chain", ParamType::Bool, "also take the edges that carry on smoothly from it", false, {}, true});
                 return ps;
             }(),
             false, true},
            cadCommand(session, [](CadDoc& doc, const Params& p) {
                Vec3 o, d;
                double scale = 0;
                if (std::string error; !viewRay(doc, p, o, d, error, &scale)) return Result::failure(error);
                const mesh::Bounds all = mesh::bounds(doc.mesh);
                double tol = scale > 0 ? numberParam(p, "pixels", 8) / scale : numberParam(p, "tolerance", 0);
                if (!(tol > 0)) tol = 0.01 * (mesh::length(all.max - all.min) + 1e-9);
                // how far the ray goes before it enters the part: edges further than that are hidden
                double front = 1e300;
                for (const auto& b : doc.bodies)
                    for (std::size_t t = 0; t < b.mesh.triangleCount(); ++t) {
                        const double s = hitTriangle(o, d, b.mesh.corner(t, 0), b.mesh.corner(t, 1), b.mesh.corner(t, 2));
                        if (s >= 0 && s < front && mesh::length(b.mesh.faceNormal(t)) > 0) front = s;
                    }
                const Body* body = nullptr;
                std::vector<detail::SharpEdge> edges;
                std::size_t pick = 0;
                double best = 1e300;
                for (const auto& b : doc.bodies) {
                    auto es = detail::sharpEdges(b.mesh, 30);
                    for (std::size_t i = 0; i < es.size(); ++i) {
                        double s = 0, t = 0;
                        const double dist = raySegment(o, d, es[i].a, es[i].b, s, t);
                        if (dist > tol || s > front + 2 * tol) continue;
                        // nearer the ray wins; at a tie the one in front
                        const double score = dist + 1e-6 * s;
                        if (score < best) {
                            best = score;
                            body = &b;
                            edges = es;
                            pick = i;
                        }
                    }
                }
                if (!body) {
                    Result r = Result::success("no edge there", false);
                    r.with("doc", stringParam(p, "doc")).with("hit", false);
                    return r;
                }
                // the chain: edges sharing an end that carry on within 35 degrees, inner with inner, outer with outer
                std::vector<std::size_t> chain{pick};
                if (boolParam(p, "chain", true)) {
                    const double eps = 1e-6 * (mesh::length(all.max - all.min) + 1);
                    std::vector<char> in(edges.size(), 0);
                    in[pick] = 1;
                    const double cosLimit = std::cos(35 * mesh::kPi / 180);
                    for (std::size_t k = 0; k < chain.size() && chain.size() < 4096; ++k) {
                        const auto& e = edges[chain[k]];
                        for (std::size_t j = 0; j < edges.size(); ++j) {
                            if (in[j] || edges[j].convex != e.convex) continue;
                            const auto& f = edges[j];
                            const Vec3 ends[2] = {e.a, e.b};
                            for (int side = 0; side < 2 && !in[j]; ++side) {
                                const Vec3& at = ends[side];
                                const Vec3 out = side == 0 ? mesh::normalize(e.a - e.b) : mesh::normalize(e.b - e.a);  // carrying on past this end
                                Vec3 next;
                                if (mesh::length(f.a - at) < eps) next = mesh::normalize(f.b - f.a);
                                else if (mesh::length(f.b - at) < eps) next = mesh::normalize(f.a - f.b);
                                else continue;
                                if (mesh::dot(out, next) >= cosLimit) {
                                    in[j] = 1;
                                    chain.push_back(j);
                                }
                            }
                        }
                    }
                }
                const auto& e = edges[pick];
                NumberList near, segments;
                double length = 0;
                for (std::size_t i : chain) {
                    const Vec3 mid = (edges[i].a + edges[i].b) * 0.5;
                    near.insert(near.end(), {mid.x, mid.y, mid.z});
                    segments.insert(segments.end(), {edges[i].a.x, edges[i].a.y, edges[i].a.z, edges[i].b.x, edges[i].b.y, edges[i].b.z});
                    length += mesh::length(edges[i].b - edges[i].a);
                }
                std::string nearText;
                for (std::size_t i = 0; i < near.size(); ++i) nearText += (i ? "," : "") + num(near[i]);
                const Vec3 mid = (e.a + e.b) * 0.5;
                Result r = Result::success(std::string(e.convex ? "an outer" : "an inner") + " edge of " + body->feature + " at " + at(mid) +
                                               (chain.size() > 1 ? ", with " + std::to_string(chain.size() - 1) + " more carrying on from it" : std::string()) +
                                               ", length " + num(length),
                                           true);
                r.with("doc", stringParam(p, "doc"))
                    .with("hit", true)
                    .with("body", body->feature)
                    .with("a", triple(e.a))
                    .with("b", triple(e.b))
                    .with("midpoint", triple(mid))
                    .with("outer", e.convex)
                    .with("out", triple(mesh::normalize(e.n1 + e.n2)))  // between the two faces, away from the part
                    .with("angle", e.angle)
                    .with("edges", static_cast<std::int64_t>(chain.size()))
                    .with("length", length)
                    .with("near", nearText)
                    .with("segments", segments);
                return r;
            }, false));

    reg.add({"cad.push_pull", "Push/Pull Face",
             "Pull a flat face of a body out by distance (or push it in with a negative distance), like Press Pull in "
             "Fusion or Instant3D in SolidWorks: it sketches the face's outline on it and extrudes that sketch, joined to "
             "the body (or cut into it), so both stay editable in the tree. face is a point x,y,z on the face (cad.pick "
             "gives one). Returns the extrude's name.",
             {docParam(), str("face", "a point x,y,z on the face", true),
              {"distance", ParamType::Number, "how far: positive pulls out, negative pushes in", true},
              str("target", "the body (default: the one whose face is nearest the point)"),
              str("name", "name for the extrude (default sketch_extrude1, ...)")},
             true},
            cadCommand(session, [](CadDoc& doc, const Params& p) {
                const auto items = splitList(stringParam(p, "face"));
                if (items.size() != 3) return Result::failure("face is a point x,y,z on the face");
                const Vec3 at3{doc.eval(items[0]), doc.eval(items[1]), doc.eval(items[2])};
                const double dist = numberParam(p, "distance", 0);
                if (!(std::fabs(dist) > 0)) return Result::failure("distance can't be 0");
                const Hit hit = nearestFace(doc, at3, stringParam(p, "target"));
                if (!hit.body) return Result::failure(has(p, "target") ? "no body called '" + stringParam(p, "target") + "'" : "the part has no bodies yet");
                const Face f = faceOf(hit.body->mesh, hit.tri);
                if (!f.flat || f.loops.empty()) return Result::failure("that face isn't flat: push/pull works on flat faces");
                Frame frame;
                std::string error;
                if (!detail::facePlane(hit.body->mesh, at(at3), doc, frame, error)) return Result::failure(error);
                const std::string body = hit.body->feature;

                Feature sk;
                sk.type = "sketch";
                sk.name = doc.newFeatureName("sketch");
                sk.values["on"] = body;
                sk.values["face"] = at(at3);
                for (const auto& loop : f.loops) {
                    std::vector<std::string> ids;
                    for (const Vec3& q : loop) {
                        const mesh::Vec2 s = frame.project(q);
                        SketchPoint pt{sk.sketch.newId("p"), s.x, s.y};
                        sk.sketch.points.push_back(pt);
                        ids.push_back(pt.id);
                    }
                    for (std::size_t i = 0; i < ids.size(); ++i)
                        sk.sketch.entities.push_back({sk.sketch.newId("l"), "line", {ids[i], ids[(i + 1) % ids.size()]}, 0, false, false});
                }
                Feature ex;
                ex.type = "sketch_extrude";
                ex.name = has(p, "name") ? stringParam(p, "name") : doc.newFeatureName("sketch_extrude");
                if (doc.findFeature(ex.name)) return Result::failure("there is already a feature called '" + ex.name + "'");
                ex.values["sketch"] = sk.name;
                ex.values["depth"] = num(std::fabs(dist));
                ex.values["operation"] = dist > 0 ? "join" : "cut";
                ex.values["target"] = body;
                const std::string name = ex.name, sketchName = sk.name;
                doc.features.push_back(std::move(sk));
                doc.features.push_back(std::move(ex));
                Result r = Result::success((dist > 0 ? "pulled the face of " : "pushed the face of ") + body + " by " + num(std::fabs(dist)) +
                                               " (" + sketchName + " + " + name + ")",
                                           name);
                r.with("doc", stringParam(p, "doc")).with("feature", name).with("sketch", sketchName).with("body", body);
                return r;
            }));
}

}  // namespace wetwaresuite::cad
