// Assembly documents: parts, components, mates, the mate solve and .wwasm files.
#include "wetwaresuite/cad/assembly.hpp"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <limits>
#include <sstream>

#include <nlohmann/json.hpp>

#include "wetwaresuite/mesh/io.hpp"
#include "blend.hpp"
#include "build.hpp"
#include "solver.hpp"

namespace wetwaresuite::cad {

using mesh::Mat4;
using mesh::Mesh;
using mesh::Vec3;
using json = nlohmann::json;

const std::vector<std::string>& mateTypes() {
    static const std::vector<std::string> t = {"coincident", "concentric", "distance", "angle", "parallel", "perpendicular", "fastened", "gear", "rack"};
    return t;
}

std::string AssemblyDoc::summary() const {
    std::string s = std::to_string(components.size()) + " components, " + std::to_string(parts.size()) + " parts, " +
                    std::to_string(mates.size()) + " mates, " + (dof == 0 ? std::string("fully defined") : std::to_string(dof) + " DOF");
    if (!path.empty()) s += ", " + path;
    return s;
}

const AssemblyPart* AssemblyDoc::findPart(const std::string& name) const {
    for (const auto& p : parts) {
        if (p.name == name) return &p;
    }
    return nullptr;
}
Component* AssemblyDoc::findComponent(const std::string& name) {
    for (auto& c : components) {
        if (c.name == name) return &c;
    }
    return nullptr;
}
const Component* AssemblyDoc::findComponent(const std::string& name) const { return const_cast<AssemblyDoc*>(this)->findComponent(name); }
Mate* AssemblyDoc::findMate(const std::string& name) {
    for (auto& m : mates) {
        if (m.name == name) return &m;
    }
    return nullptr;
}
std::string AssemblyDoc::newName(const std::string& base) {
    for (;;) {
        std::string n = base + std::to_string(++counters[base]);
        if (!findComponent(n) && !findMate(n) && !findPart(n)) return n;
    }
}

double AssemblyDoc::eval(const std::string& expr) const {
    return evaluate(expr, [this](const std::string& name) -> std::optional<double> {
        for (const auto& p : params) {
            if (p.name == name) return evaluate(p.expr);
        }
        return std::nullopt;
    });
}

namespace {

constexpr double kPi = 3.14159265358979323846;

struct Geo {
    enum Kind { Plane, Axis, Point } kind = Point;
    Vec3 p, d{0, 0, 1};
};

const char* kindName(Geo::Kind k) { return k == Geo::Plane ? "plane" : k == Geo::Axis ? "axis" : "point"; }

const CadDoc& scratchDoc() {
    static const auto doc = newCad();
    return *doc;
}

Geo localGeo(const AssemblyPart& part, const std::string& sel) {
    Geo g;
    if (sel == "origin") return g;
    if (sel == "xy" || sel == "xz" || sel == "yz") {
        g.kind = Geo::Plane;
        g.d = sel == "xy" ? Vec3{0, 0, 1} : sel == "xz" ? Vec3{0, 1, 0} : Vec3{1, 0, 0};
        return g;
    }
    if (sel == "x" || sel == "y" || sel == "z") {
        g.kind = Geo::Axis;
        g.d = sel == "x" ? Vec3{1, 0, 0} : sel == "y" ? Vec3{0, 1, 0} : Vec3{0, 0, 1};
        return g;
    }
    if (!sel.empty() && sel[0] == '@') {
        const auto items = splitList(sel.substr(1));
        if (items.size() != 3) throw std::invalid_argument("a point is @x,y,z");
        g.p = {evaluate(items[0]), evaluate(items[1]), evaluate(items[2])};
        return g;
    }
    std::string feature = sel;
    int index = 1;
    if (auto hash = sel.find('#'); hash != std::string::npos) {
        feature = sel.substr(0, hash);
        index = std::atoi(sel.c_str() + hash + 1);
    }
    if (part.doc && part.doc->findFeature(feature)) {
        const FeatureExtra* x = part.doc->featureExtra(feature);
        if (x && !x->axes.empty()) {
            if (index < 1 || index > static_cast<int>(x->axes.size())) {
                throw std::invalid_argument(feature + " has " + std::to_string(x->axes.size()) + " axes");
            }
            g.kind = Geo::Axis;
            g.p = x->axes[static_cast<std::size_t>(index - 1)].point;
            g.d = mesh::normalize(x->axes[static_cast<std::size_t>(index - 1)].dir);
            return g;
        }
        if (x && x->plane) {
            g.kind = Geo::Plane;
            g.p = x->plane->origin;
            g.d = mesh::normalize(x->plane->n);
            return g;
        }
        throw std::invalid_argument("feature " + feature + " of " + part.name + " has no plane or axis to mate to");
    }
    if (!part.mesh) throw std::invalid_argument("part " + part.name + " has no geometry");
    Frame f;
    std::string error;
    if (!detail::facePlane(*part.mesh, sel, scratchDoc(), f, error)) {
        throw std::invalid_argument("'" + sel + "': " + error + " (or name a datum, hole, cylinder, xy/xz/yz, x/y/z, origin or @x,y,z)");
    }
    g.kind = Geo::Plane;
    g.p = f.origin;
    g.d = f.n;
    return g;
}

Mat4 rotationVector(const Vec3& rv) {
    const double a = mesh::length(rv);
    if (a < 1e-15) return {};
    return Mat4::rotateAxis(rv / a, a * 180 / kPi);
}

struct Placed {
    int var = -1;  // first of its 6 variables, -1 when fixed
    Mat4 base;
    Vec3 pivot;
    Mat4 at(const std::vector<double>& x) const {
        if (var < 0) return base;
        const auto o = static_cast<std::size_t>(var);
        const Vec3 t{x[o], x[o + 1], x[o + 2]}, rv{x[o + 3], x[o + 4], x[o + 5]};
        return Mat4::translate(pivot + t) * rotationVector(rv) * Mat4::translate(pivot * -1) * base;
    }
    std::vector<int> vars() const {
        if (var < 0) return {};
        return {var, var + 1, var + 2, var + 3, var + 4, var + 5};
    }
};

// How far (radians) pose `now` has turned about `axis` since pose `then`: a vector square to the axis,
// turned by the change in rotation, measured around the axis.
double turnedAbout(const Mat4& now, const Mat4& then, const Vec3& axis) {
    const Mat4 rel = now * then.inverse();
    Vec3 v = std::fabs(axis.x) < 0.9 ? Vec3{1, 0, 0} : Vec3{0, 1, 0};
    v = mesh::normalize(v - axis * mesh::dot(v, axis));
    Vec3 w = rel.direction(v);
    w = w - axis * mesh::dot(w, axis);
    return std::atan2(mesh::dot(mesh::cross(v, w), axis), mesh::dot(v, w));
}

std::vector<int> both(const Placed& a, const Placed& b) {
    auto v = a.vars();
    auto w = b.vars();
    v.insert(v.end(), w.begin(), w.end());
    return v;
}

}  // namespace

mesh::Mesh AssemblyDoc::componentMesh(const Component& c, bool exploded) const {
    const AssemblyPart* part = findPart(c.part);
    if (!part || !part->mesh) return {};
    Mesh m = *part->mesh;
    mesh::transform(m, c.pose);
    if (exploded && explode != 0 && components.size() > 1) {
        mesh::Bounds all;
        std::vector<mesh::Bounds> boxes;
        for (const auto& o : components) {
            const AssemblyPart* op = findPart(o.part);
            mesh::Bounds b;
            if (op && op->mesh) {
                Mesh t = *op->mesh;
                mesh::transform(t, o.pose);
                b = mesh::bounds(t);
            }
            boxes.push_back(b);
            all.merge(b);
        }
        const mesh::Bounds mine = mesh::bounds(m);
        Vec3 offset;
        if (explodeAxis == "radial") {
            Vec3 d = mine.center() - all.center();
            if (mesh::length(d) < 1e-9 * (1 + all.diagonal())) d = {0, 0, 1};
            offset = mesh::normalize(d) * (explode * mine.diagonal());
        } else {
            Vec3 axis;
            detail::directionByName(explodeAxis, axis);
            // components spread out along the axis in the order they sit on it
            std::size_t rank = 0;
            const double mineAt = mesh::dot(mine.center(), axis);
            double step = 0;
            for (const auto& b : boxes) {
                if (b.empty()) continue;
                step = std::max(step, std::fabs(mesh::dot(b.size(), axis)));
                if (mesh::dot(b.center(), axis) < mineAt - 1e-9) ++rank;
            }
            offset = axis * (explode * step * static_cast<double>(rank));
        }
        mesh::transform(m, Mat4::translate(offset));
    }
    return m;
}

void AssemblyDoc::rebuildMesh() {
    static const raster::Color palette[] = {{122, 158, 204, 255}, {214, 150, 92, 255}, {120, 180, 120, 255}, {190, 120, 180, 255},
                                            {200, 190, 100, 255}, {110, 180, 190, 255}, {200, 110, 110, 255}, {150, 150, 150, 255}};
    mesh = {};
    previewOptions.faceColors.clear();
    std::vector<raster::Color> colors;
    std::size_t i = 0;
    for (const auto& c : components) {
        Mesh m = componentMesh(c, true);
        colors.insert(colors.end(), m.triangleCount(), palette[i++ % 8]);
        mesh::append(mesh, m);
    }
    previewOptions.faceColors = colors;
}

void AssemblyDoc::solve() {
    std::vector<Placed> placed(components.size());
    std::map<std::string, std::size_t> index;
    int nvars = 0;
    mesh::Bounds all;
    for (std::size_t i = 0; i < components.size(); ++i) {
        const Component& c = components[i];
        index[c.name] = i;
        placed[i].base = c.pose;
        placed[i].pivot = c.pose.point({0, 0, 0});
        if (const AssemblyPart* part = findPart(c.part); part && part->mesh) {
            Mesh m = *part->mesh;
            mesh::transform(m, c.pose);
            all.merge(mesh::bounds(m));
            placed[i].pivot = mesh::bounds(m).center();
        }
        if (!c.fixed && c.name != hold) {
            placed[i].var = nvars;
            nvars += 6;
        }
    }
    hold.clear();
    // forced: the value a limit mate is held at (its bound); NaN = the mate's own value
    auto buildMate = [&](std::size_t mi, double forced, std::vector<detail::Equation>& eqs) {
        const Mate& mate = mates[mi];
        auto split = [&](const std::string& ref, std::size_t& ci, Geo& g) {
            const auto slash = ref.find('/');
            if (slash == std::string::npos) throw std::invalid_argument("mate " + mate.name + ": '" + ref + "' must be component/reference, e.g. bracket1/top");
            const std::string comp = ref.substr(0, slash), sel = ref.substr(slash + 1);
            auto it = index.find(comp);
            if (it == index.end()) throw std::invalid_argument("mate " + mate.name + ": no component called '" + comp + "'");
            ci = it->second;
            const AssemblyPart* part = findPart(components[ci].part);
            if (!part) throw std::invalid_argument("mate " + mate.name + ": component " + comp + " has no part");
            try {
                g = localGeo(*part, sel);
            } catch (const std::invalid_argument& e) {
                throw std::invalid_argument("mate " + mate.name + ": " + comp + ": " + e.what());
            }
        };
        std::size_t ia = 0, ib = 0;
        Geo ga, gb;
        split(mate.a, ia, ga);
        split(mate.b, ib, gb);
        if (ia == ib) throw std::invalid_argument("mate " + mate.name + ": both ends are on " + components[ia].name);
        const Placed pa = placed[ia], pb = placed[ib];
        const int owner = static_cast<int>(mi);
        const std::vector<int> vars = both(pa, pb);
        // the references in the assembly for the current variables
        auto world = [](const Placed& pl, const Geo& g, const std::vector<double>& x, Vec3& p, Vec3& d) {
            const Mat4 m = pl.at(x);
            p = m.point(g.p);
            d = mesh::normalize(m.direction(g.d));
        };
        auto add = [&](std::function<double(const Vec3&, const Vec3&, const Vec3&, const Vec3&)> f) {
            eqs.push_back({vars, [pa, pb, ga, gb, world, f](const std::vector<double>& x) {
                               Vec3 p1, d1, p2, d2;
                               world(pa, ga, x, p1, d1);
                               world(pb, gb, x, p2, d2);
                               return f(p1, d1, p2, d2);
                           },
                           owner});
        };
        auto add3 = [&](std::function<Vec3(const Vec3&, const Vec3&, const Vec3&, const Vec3&)> f) {
            for (int k = 0; k < 3; ++k) {
                add([f, k](const Vec3& p1, const Vec3& d1, const Vec3& p2, const Vec3& d2) { return f(p1, d1, p2, d2)[k]; });
            }
        };
        const std::string& t = mate.type;
        const double s = mate.flip ? -1 : 1;
        double value = 0;
        if (!std::isnan(forced)) {
            value = forced;
        } else if (t == "distance" || t == "angle") {
            if (mate.value.empty()) throw std::invalid_argument("mate " + mate.name + ": " + t + " needs a value (or min/max limits)");
            value = eval(mate.value);
        } else if (t == "gear" || t == "rack") {
            value = mate.value.empty() ? 1.0 : eval(mate.value);
        }
        const auto ka = ga.kind, kb = gb.kind;
        auto expects = [&](const std::string& what) {
            throw std::invalid_argument("mate " + mate.name + ": " + t + " takes " + what + " (got a " + kindName(ka) + " and a " + kindName(kb) + ")");
        };
        auto planeOrAxis = [](Geo::Kind k) { return k != Geo::Point; };
        if (t == "coincident" || t == "concentric" || (t == "distance" && ka == Geo::Plane && kb == Geo::Plane)) {
            const double gap = t == "distance" ? value : 0;
            if (t == "concentric" && (ka != Geo::Axis || kb != Geo::Axis)) expects("two axes (holes, cylinders, datum axes)");
            if (ka == Geo::Plane && kb == Geo::Plane) {
                add3([s](const Vec3&, const Vec3& d1, const Vec3&, const Vec3& d2) { return d1 + d2 * s; });
                add([gap](const Vec3& p1, const Vec3& d1, const Vec3& p2, const Vec3&) { return mesh::dot(p2 - p1, d1) - gap; });
            } else if (ka == Geo::Axis && kb == Geo::Axis) {
                add3([](const Vec3&, const Vec3& d1, const Vec3&, const Vec3& d2) { return mesh::cross(d1, d2); });
                add3([](const Vec3& p1, const Vec3& d1, const Vec3& p2, const Vec3&) { return mesh::cross(p2 - p1, d1); });
            } else if (ka == Geo::Point && kb == Geo::Point) {
                add3([](const Vec3& p1, const Vec3&, const Vec3& p2, const Vec3&) { return p1 - p2; });
            } else if ((ka == Geo::Point) != (kb == Geo::Point)) {
                const bool pointFirst = ka == Geo::Point;
                const Geo::Kind other = pointFirst ? kb : ka;
                if (other == Geo::Plane) {
                    add([pointFirst](const Vec3& p1, const Vec3& d1, const Vec3& p2, const Vec3& d2) {
                        return pointFirst ? mesh::dot(p1 - p2, d2) : mesh::dot(p2 - p1, d1);
                    });
                } else {
                    add3([pointFirst](const Vec3& p1, const Vec3& d1, const Vec3& p2, const Vec3& d2) {
                        return pointFirst ? mesh::cross(p1 - p2, d2) : mesh::cross(p2 - p1, d1);
                    });
                }
            } else {  // an axis in a plane
                const bool axisFirst = ka == Geo::Axis;
                add([axisFirst](const Vec3&, const Vec3& d1, const Vec3&, const Vec3& d2) { return mesh::dot(d1, d2); });
                add([axisFirst](const Vec3& p1, const Vec3& d1, const Vec3& p2, const Vec3& d2) {
                    return axisFirst ? mesh::dot(p1 - p2, d2) : mesh::dot(p2 - p1, d1);
                });
            }
        } else if (t == "distance") {
            if (ka == Geo::Point && kb == Geo::Point) {
                add([value](const Vec3& p1, const Vec3&, const Vec3& p2, const Vec3&) { return mesh::length(p2 - p1) - value; });
            } else if (ka == Geo::Axis && kb == Geo::Axis) {
                add3([](const Vec3&, const Vec3& d1, const Vec3&, const Vec3& d2) { return mesh::cross(d1, d2); });
                add([value](const Vec3& p1, const Vec3& d1, const Vec3& p2, const Vec3&) { return mesh::length(mesh::cross(p2 - p1, d1)) - value; });
            } else if ((ka == Geo::Point && kb == Geo::Plane) || (ka == Geo::Plane && kb == Geo::Point)) {
                const bool pointFirst = ka == Geo::Point;
                add([pointFirst, value](const Vec3& p1, const Vec3& d1, const Vec3& p2, const Vec3& d2) {
                    return (pointFirst ? mesh::dot(p1 - p2, d2) : mesh::dot(p2 - p1, d1)) - value;
                });
            } else {
                expects("two planes, two points, two axes, or a point and a plane");
            }
        } else if (t == "angle" || t == "parallel" || t == "perpendicular") {
            if (!planeOrAxis(ka) || !planeOrAxis(kb)) expects("planes or axes");
            if (t == "parallel") {
                add3([](const Vec3&, const Vec3& d1, const Vec3&, const Vec3& d2) { return mesh::cross(d1, d2); });
            } else {
                const double c = t == "perpendicular" ? 0 : std::cos(value * kPi / 180);
                add([c, s](const Vec3&, const Vec3& d1, const Vec3&, const Vec3& d2) { return s * mesh::dot(d1, d2) - c; });
            }
        } else if (t == "fastened") {
            if (mate.lock.size() != 16) throw std::invalid_argument("mate " + mate.name + ": fastened has no stored position");
            const std::vector<double> lock = mate.lock;
            for (int k = 0; k < 12; ++k) {
                eqs.push_back({vars, [pa, pb, lock, k](const std::vector<double>& x) {
                                   const Mat4 rel = pa.at(x).inverse() * pb.at(x);
                                   return rel.m[k] - lock[static_cast<std::size_t>(k)];
                               },
                               owner});
            }
        } else if (t == "gear" || t == "rack") {
            if (ka != Geo::Axis) expects(t == "gear" ? "two axes (each gear's hole or cylinder)" : "an axis (the pinion's) and an axis or plane along the rack");
            if (t == "gear" && kb != Geo::Axis) expects("two axes (each gear's hole or cylinder)");
            if (t == "rack" && kb == Geo::Point) expects("an axis (the pinion's) and an axis or plane along the rack");
            const std::vector<double> lock = mate.lock.size() == 32 ? mate.lock : std::vector<double>{};
            Mat4 la = pa.base, lb = pb.base;
            if (!lock.empty()) {
                std::copy(lock.begin(), lock.begin() + 16, la.m);
                std::copy(lock.begin() + 16, lock.end(), lb.m);
            }
            const bool gear = t == "gear";
            eqs.push_back({vars, [pa, pb, ga, gb, la, lb, value, s, gear](const std::vector<double>& x) {
                               const Mat4 ma = pa.at(x), mb = pb.at(x);
                               const double turnA = turnedAbout(ma, la, mesh::normalize(ma.direction(ga.d)));
                               if (gear) return value * turnA + s * turnedAbout(mb, lb, mesh::normalize(mb.direction(gb.d)));
                               const Vec3 slid = mb.point(gb.p) - lb.point(gb.p);
                               return value * turnA - s * mesh::dot(slid, mesh::normalize(mb.direction(gb.d)));
                           },
                           owner});
        } else {
            throw std::invalid_argument("mate " + mate.name + ": unknown type '" + t + "'");
        }
    };
    // a limit mate's measured value: build it at a reference value and read its last residual back
    auto measure = [&](std::size_t mi, const std::vector<double>& x) {
        std::vector<detail::Equation> probe;
        const bool angle = mates[mi].type == "angle";
        buildMate(mi, angle ? 90.0 : 0.0, probe);
        const double r = probe.back().f(x);
        return angle ? std::acos(std::clamp(r, -1.0, 1.0)) * 180 / kPi : r;
    };
    const double nan = std::numeric_limits<double>::quiet_NaN();
    std::vector<detail::Equation> eqs;
    std::vector<std::size_t> limited;
    for (std::size_t mi = 0; mi < mates.size(); ++mi) {
        const Mate& m = mates[mi];
        const bool limits = (m.type == "distance" || m.type == "angle") && (!m.min.empty() || !m.max.empty());
        if (limits) limited.push_back(mi);
        if (!limits || !m.value.empty()) buildMate(mi, nan, eqs);
    }
    std::vector<double> x(static_cast<std::size_t>(nvars), 0.0);
    const double scale = std::max(1.0, all.empty() ? 1.0 : all.diagonal());
    auto rep = detail::solve(x, {}, eqs, 1e-9 * scale, 300);
    // limits: anything past a bound is held at that bound and solved again (a few rounds, as one may push another)
    limitsHit.clear();
    for (int round = 0; round < 4 && rep.converged && !limited.empty(); ++round) {
        bool added = false;
        for (std::size_t& mi : limited) {
            if (mi == static_cast<std::size_t>(-1)) continue;
            const Mate& m = mates[mi];
            const double v = measure(mi, x);
            const double tol = m.type == "angle" ? 1e-6 : 1e-7 * scale;
            double bound = nan;
            if (!m.min.empty() && v < eval(m.min) - tol) bound = eval(m.min);
            if (!m.max.empty() && v > eval(m.max) + tol) bound = eval(m.max);
            if (std::isnan(bound)) continue;
            buildMate(mi, bound, eqs);
            limitsHit.push_back(m.name);
            mi = static_cast<std::size_t>(-1);
            added = true;
        }
        if (!added) break;
        rep = detail::solve(x, {}, eqs, 1e-9 * scale, 300);
    }
    if (!rep.converged) {
        std::string names;
        for (int o : rep.failing) {
            if (o >= 0) names += (names.empty() ? "" : ", ") + mates[static_cast<std::size_t>(o)].name + " (" + mates[static_cast<std::size_t>(o)].type + ")";
        }
        throw std::runtime_error("the mates can't all hold: " + (names.empty() ? std::string("they conflict") : names) +
                                 " (over defined; delete or change one, or flip a plane mate)");
    }
    dof = rep.dof;
    redundant = rep.redundant;
    freeComponents.clear();
    for (std::size_t i = 0; i < components.size(); ++i) {
        components[i].pose = placed[i].at(x);
        if (placed[i].var >= 0) {
            bool free = false;
            for (int v : placed[i].vars()) free = free || rep.movable[static_cast<std::size_t>(v)];
            if (free) freeComponents.push_back(components[i].name);
        }
    }
    // gears and racks count turns from here next time
    for (auto& m : mates) {
        if (m.type != "gear" && m.type != "rack") continue;
        const Component* ca = findComponent(m.a.substr(0, m.a.find('/')));
        const Component* cb = findComponent(m.b.substr(0, m.b.find('/')));
        if (!ca || !cb) continue;
        m.lock.assign(ca->pose.m, ca->pose.m + 16);
        m.lock.insert(m.lock.end(), cb->pose.m, cb->pose.m + 16);
    }
    rebuildMesh();
}

// ---- parts and files ------------------------------------------------------------

AssemblyPart partFromDoc(const CadDoc& doc, const std::string& name, const std::string& sourceDoc) {
    AssemblyPart p;
    p.name = name;
    p.sourceDoc = sourceDoc;
    p.source = doc.path;
    p.json = toJson(doc);
    auto built = fromJson(p.json);
    p.mesh = std::make_shared<const Mesh>(built->mesh);
    p.doc = built;
    return p;
}

AssemblyPart partFromFile(const std::filesystem::path& path, const std::string& name) {
    auto doc = openCad(path);
    AssemblyPart p = partFromDoc(*doc, name, "");
    p.source = path.string();
    return p;
}

std::string assemblyToJson(const AssemblyDoc& doc) {
    json j;
    j["format"] = "wwasm";
    j["version"] = 1;
    j["units"] = doc.units;
    json params = json::array();
    for (const auto& p : doc.params) params.push_back({{"name", p.name}, {"value", p.expr}});
    j["params"] = params;
    json parts = json::array();
    for (const auto& p : doc.parts) {
        json jp = {{"name", p.name}, {"source", p.source}, {"part", json::parse(p.json)}};
        parts.push_back(jp);
    }
    j["parts"] = parts;
    json comps = json::array();
    for (const auto& c : doc.components) {
        comps.push_back({{"name", c.name}, {"part", c.part}, {"pose", std::vector<double>(c.pose.m, c.pose.m + 16)}, {"fixed", c.fixed}});
    }
    j["components"] = comps;
    json mates = json::array();
    for (const auto& m : doc.mates) {
        json jm = {{"name", m.name}, {"type", m.type}, {"a", m.a}, {"b", m.b}};
        if (!m.value.empty()) jm["value"] = m.value;
        if (!m.min.empty()) jm["min"] = m.min;
        if (!m.max.empty()) jm["max"] = m.max;
        if (m.flip) jm["flip"] = true;
        if (!m.lock.empty()) jm["lock"] = m.lock;
        mates.push_back(jm);
    }
    j["mates"] = mates;
    j["explode"] = doc.explode;
    j["explode_axis"] = doc.explodeAxis;
    j["counters"] = doc.counters;
    return j.dump(2) + "\n";
}

std::shared_ptr<AssemblyDoc> assemblyFromJson(const std::string& text) {
    json j = json::parse(text, nullptr, false);
    if (j.is_discarded() || !j.is_object() || j.value("format", "") != "wwasm") throw std::runtime_error("not a .wwasm assembly file");
    auto doc = std::make_shared<AssemblyDoc>();
    try {
        doc->units = j.value("units", "mm");
        for (const auto& p : j.value("params", json::array())) doc->params.push_back({p.at("name").get<std::string>(), p.at("value").get<std::string>()});
        for (const auto& jp : j.value("parts", json::array())) {
            AssemblyPart p;
            p.name = jp.at("name").get<std::string>();
            p.source = jp.value("source", "");
            p.json = jp.at("part").dump();
            auto built = fromJson(p.json);
            p.mesh = std::make_shared<const Mesh>(built->mesh);
            p.doc = built;
            doc->parts.push_back(std::move(p));
        }
        for (const auto& jc : j.value("components", json::array())) {
            Component c;
            c.name = jc.at("name").get<std::string>();
            c.part = jc.at("part").get<std::string>();
            const auto pose = jc.at("pose").get<std::vector<double>>();
            if (pose.size() != 16) throw std::runtime_error("component " + c.name + ": bad pose");
            std::copy(pose.begin(), pose.end(), c.pose.m);
            c.fixed = jc.value("fixed", false);
            if (!doc->findPart(c.part)) throw std::runtime_error("component " + c.name + " uses a missing part " + c.part);
            doc->components.push_back(std::move(c));
        }
        for (const auto& jm : j.value("mates", json::array())) {
            Mate m;
            m.name = jm.at("name").get<std::string>();
            m.type = jm.at("type").get<std::string>();
            m.a = jm.at("a").get<std::string>();
            m.b = jm.at("b").get<std::string>();
            m.value = jm.value("value", "");
            m.min = jm.value("min", "");
            m.max = jm.value("max", "");
            m.flip = jm.value("flip", false);
            m.lock = jm.value("lock", std::vector<double>{});
            doc->mates.push_back(std::move(m));
        }
        doc->explode = j.value("explode", 0.0);
        doc->explodeAxis = j.value("explode_axis", "radial");
        doc->counters = j.value("counters", std::map<std::string, int>{});
    } catch (const json::exception& e) {
        throw std::runtime_error(std::string("bad .wwasm file: ") + e.what());
    }
    doc->solve();
    return doc;
}

// ---- undo -------------------------------------------------------------------------

Result AssemblyEdit::apply() {
    before_ = {doc_->params, doc_->parts, doc_->components, doc_->mates, doc_->counters, doc_->explode, doc_->explodeAxis};
    Result r;
    try {
        r = fn_(*doc_);
        if (r.ok) doc_->solve();
    } catch (const std::exception& e) {
        r = Result::failure(e.what());
    }
    if (!r.ok) revert();
    return r;
}

void AssemblyEdit::revert() {
    doc_->params = before_.params;
    doc_->parts = before_.parts;
    doc_->components = before_.components;
    doc_->mates = before_.mates;
    doc_->counters = before_.counters;
    doc_->explode = before_.explode;
    doc_->explodeAxis = before_.explodeAxis;
    try {
        doc_->solve();
    } catch (const std::exception&) {
    }
}

}  // namespace wetwaresuite::cad
