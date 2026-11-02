// cad.assembly_* and cad.mate* commands.
#include <algorithm>
#include <cctype>
#include <cmath>
#include <fstream>
#include <map>
#include <sstream>

#include "wetwaresuite/cad/assembly.hpp"
#include "wetwaresuite/mesh/csg.hpp"
#include "wetwaresuite/mesh/io.hpp"
#include "wetwaresuite/mesh/render.hpp"
#include "wetwaresuite/raster/codec.hpp"
#include "build.hpp"
#include "cmdutil.hpp"

namespace wetwaresuite::cad {

using mesh::Mat4;
using mesh::Mesh;
using mesh::Vec3;
using cmd::boolean;
using cmd::has;
using cmd::num;
using cmd::str;

namespace {

ParamSpec asmParam() { return {"doc", ParamType::String, "id of an open assembly, e.g. asm1", true}; }

using AsmFn = std::function<Result(AssemblyDoc&, const Params&)>;

ActionFactory asmCommand(Session& session, AsmFn fn, bool undoable = true) {
    return [&session, fn, undoable](const Params& p) -> std::unique_ptr<Action> {
        const std::string id = stringParam(p, "doc");
        auto doc = session.workspace().getAs<AssemblyDoc>(id);
        if (!doc) return std::make_unique<FunctionAction>([id] { return Result::failure("no open assembly called '" + id + "' (cad.assembly_new makes one)"); });
        if (!undoable) {
            return std::make_unique<FunctionAction>([doc, fn, p] {
                try {
                    return fn(*doc, p);
                } catch (const std::exception& e) {
                    return Result::failure(e.what());
                }
            });
        }
        return std::make_unique<AssemblyEdit>(doc, [fn, p](AssemblyDoc& d) { return fn(d, p); });
    };
}

// After an edit: how defined the assembly is, and a picture when asked for.
AsmFn reported(AsmFn fn) {
    return [fn](AssemblyDoc& doc, const Params& p) {
        Result r = fn(doc, p);
        if (!r.ok) return r;
        doc.solve();
        std::string loose;
        for (const auto& c : doc.freeComponents) loose += (loose.empty() ? "" : ",") + c;
        r.message += doc.dof == 0 ? " (fully defined)" : " (" + std::to_string(doc.dof) + (doc.dof == 1 ? " degree" : " degrees") + " of freedom left: " + loose + ")";
        std::string hit;
        for (const auto& m : doc.limitsHit) hit += (hit.empty() ? "" : ",") + m;
        if (!hit.empty()) r.message += ", at the limit of " + hit;
        r.with("doc", stringParam(p, "doc")).with("dof", static_cast<std::int64_t>(doc.dof)).with("free", loose).with("limits_hit", hit);
        return mesh::maybePreview(r, doc, p);
    };
}

std::vector<ParamSpec> withDoc(std::vector<ParamSpec> s, bool preview = true) {
    s.insert(s.begin(), asmParam());
    if (preview) {
        s.push_back(mesh::previewParam());
        s.push_back(mesh::viewParam());
    }
    return s;
}

Mat4 poseFrom(const AssemblyDoc& doc, const Params& p, const Mat4& base, bool absolute) {
    auto v = [&](const std::string& k) { return has(p, k) ? doc.eval(cmd::text(p, k)) : 0.0; };
    const Mat4 m = Mat4::translate({v("x"), v("y"), v("z")}) * Mat4::rotate({v("rx"), v("ry"), v("rz")});
    return absolute ? m : m * base;
}

std::vector<ParamSpec> placementParams() {
    return {str("x", "move along X"), str("y", "Y"), str("z", "Z"), str("rx", "rotate about X, degrees"), str("ry", "about Y"),
            str("rz", "about Z")};
}

std::string stem(const std::string& path) { return std::filesystem::path(path).stem().string(); }

std::string cleanName(std::string s) {
    for (auto& c : s) {
        if (!std::isalnum(static_cast<unsigned char>(c)) && c != '_' && c != '-') c = '_';
    }
    return s.empty() ? std::string("part") : s;
}

double meshVolume(const Mesh& m) { return std::fabs(mesh::volume(m)); }

}  // namespace

void registerAssemblyEditCommands(Session& session);

void registerAssemblyCommands(Session& session) {
    auto& reg = session.registry();

    reg.add({"cad.assembly_new", "New Assembly",
             "Create an empty assembly and return its id (asm1). Insert parts with cad.assembly_insert, then hold them "
             "together with cad.mate.",
             {{"units", ParamType::String, "length unit", false, unitNames(), std::string("mm")}}, false, true},
            [&session](const Params& p) -> std::unique_ptr<Action> {
                return std::make_unique<FunctionAction>([&session, p] {
                    auto doc = std::make_shared<AssemblyDoc>();
                    doc->units = stringParam(p, "units", "mm");
                    const std::string id = session.workspace().add(doc, "asm");
                    return Result::success("created assembly " + id, id).with("doc", id);
                });
            });

    reg.add({"cad.assembly_open", "Open Assembly", "Open a .wwasm assembly (its parts are stored inside it).",
             {{"path", ParamType::String, ".wwasm file", true}}, false, true},
            [&session](const Params& p) -> std::unique_ptr<Action> {
                return std::make_unique<FunctionAction>([&session, p] {
                    try {
                        const std::string path = stringParam(p, "path");
                        std::ifstream in(path, std::ios::binary);
                        if (!in) return Result::failure("can't open " + path);
                        std::stringstream ss;
                        ss << in.rdbuf();
                        auto doc = assemblyFromJson(ss.str());
                        doc->path = path;
                        const std::string id = session.workspace().add(doc, "asm");
                        return Result::success("opened " + id + " (" + doc->summary() + ")", id).with("doc", id);
                    } catch (const std::exception& e) {
                        return Result::failure(e.what());
                    }
                });
            });

    reg.add({"cad.assembly_insert", "Insert Component",
             "Insert a part as a component: an open cad document (part=cad1, its tree is copied; cad.assembly_update "
             "pulls later changes) or a file (.wwcad, STEP or a mesh). Inserting the same part again adds another "
             "instance. The first component is fixed in place; the others move to satisfy mates. x y z rx ry rz place it.",
             withDoc([] {
                 std::vector<ParamSpec> s{str("part", "an open cad document id (cad1) or a file path", true),
                                          str("name", "component name (default: the part name + a number)"),
                                          boolean("fixed", "fix it in place (default: only the first component is)", false)};
                 for (auto& q : placementParams()) s.push_back(q);
                 return s;
             }()),
             true},
            [&session](const Params& p) -> std::unique_ptr<Action> {
                return asmCommand(session, reported([&session](AssemblyDoc& doc, const Params& q) {
                    const std::string ref = stringParam(q, "part");
                    std::string partName;
                    auto cad = session.workspace().getAs<CadDoc>(ref);
                    for (const auto& existing : doc.parts) {
                        if ((cad && existing.sourceDoc == ref) || (!cad && existing.source == ref)) partName = existing.name;
                    }
                    if (partName.empty()) {
                        std::string base = cleanName(cad ? (cad->path.empty() ? ref : stem(cad->path)) : stem(ref));
                        partName = base;
                        for (int k = 2; doc.findPart(partName); ++k) partName = base + "_" + std::to_string(k);
                        AssemblyPart part = cad ? partFromDoc(*cad, partName, ref) : partFromFile(ref, partName);
                        if (!part.mesh || part.mesh->empty()) return Result::failure("part " + ref + " has no solid");
                        doc.parts.push_back(std::move(part));
                    }
                    Component c;
                    c.name = cmd::has(q, "name") ? stringParam(q, "name") : doc.newName(partName);
                    if (doc.findComponent(c.name)) return Result::failure("there is already a component called '" + c.name + "'");
                    c.part = partName;
                    c.fixed = boolParam(q, "fixed", false) || doc.components.empty();
                    c.pose = poseFrom(doc, q, {}, true);
                    doc.components.push_back(c);
                    return Result::success("inserted " + c.name + " (" + partName + (c.fixed ? ", fixed" : "") + ")", c.name).with("component", c.name);
                }))(p);
            });

    reg.add({"cad.mate", "Add Mate",
             "Hold two components together. a and b are component/reference: a face (bracket1/top, or bracket1/10,5,0 for "
             "the face nearest that point), a part plane (xy, xz, yz), axis (x, y, z), origin, a point (@x,y,z) or a "
             "feature of the part (datum plane or axis, sketch plane, hole or cylinder axis; hole1#2 = the second hole of "
             "a pattern). coincident: faces meet (facing each other; flip for the same way), axes line up, points meet. "
             "concentric: two axes line up. distance: planes face each other value apart (also points, axes). angle "
             "between planes or axes; parallel; perpendicular; fastened locks b to a where it is now. distance and angle "
             "take min/max limits instead of a value (a slot's travel, a hinge's stop): the mate only holds at a limit. gear: "
             "two axes turn together, b turning value turns per turn of a (teeth of a / teeth of b), opposite ways unless "
             "flip; rack: a's turning slides b along its axis or plane, value per radian (the pinion's pitch radius). Gears "
             "and racks only link the turning: mate each axis to its shaft too.",
             withDoc({{"type", ParamType::String, "kind of mate", true, mateTypes(), {}}, str("a", "first reference, e.g. base1/top", true),
                      str("b", "second reference, e.g. peg1/bottom", true), str("value", "distance or angle (degrees), gear ratio or rack radius: number or expression"),
                      str("min", "distance / angle: lower limit (instead of a value)"), str("max", "distance / angle: upper limit"),
                      boolean("flip", "planes face the same way (or the other angle branch)", false), str("name", "mate name (default mate1, ...)")}),
             true},
            asmCommand(session, reported([](AssemblyDoc& doc, const Params& p) {
                Mate m;
                m.type = stringParam(p, "type");
                m.a = stringParam(p, "a");
                m.b = stringParam(p, "b");
                m.value = cmd::text(p, "value");
                m.min = cmd::text(p, "min");
                m.max = cmd::text(p, "max");
                m.flip = boolParam(p, "flip", false);
                if ((m.type == "distance" || m.type == "angle") && m.value.empty() && m.min.empty() && m.max.empty())
                    return Result::failure(m.type + " needs a value, or min/max limits");
                if (!m.min.empty() && !m.max.empty() && doc.eval(m.min) > doc.eval(m.max)) return Result::failure("min is bigger than max");
                m.name = has(p, "name") ? stringParam(p, "name") : doc.newName("mate");
                if (doc.findMate(m.name)) return Result::failure("there is already a mate called '" + m.name + "'");
                if (m.type == "fastened") {
                    const Component* ca = doc.findComponent(m.a.substr(0, m.a.find('/')));
                    const Component* cb = doc.findComponent(m.b.substr(0, m.b.find('/')));
                    if (!ca || !cb) return Result::failure("fastened needs two components (a=comp1/origin b=comp2/origin)");
                    const Mat4 rel = ca->pose.inverse() * cb->pose;
                    m.lock.assign(rel.m, rel.m + 16);
                }
                doc.mates.push_back(m);
                return Result::success(m.type + " mate " + m.name + " (" + m.a + ", " + m.b + ")", m.name).with("mate", m.name);
            })));

    reg.add({"cad.mate_set", "Edit Mate", "Change a mate's value (distance, angle, gear ratio), its min/max limits, or flip it. An empty value clears it.",
             withDoc({str("mate", "mate name", true), str("value", "new value or expression"), str("min", "lower limit"), str("max", "upper limit"),
                      {"flip", ParamType::Bool, "flip it", false, {}, {}}}), true},
            asmCommand(session, reported([](AssemblyDoc& doc, const Params& p) {
                Mate* m = doc.findMate(stringParam(p, "mate"));
                if (!m) return Result::failure("no mate called '" + stringParam(p, "mate") + "'");
                if (has(p, "value")) m->value = cmd::text(p, "value");
                if (has(p, "min")) m->min = cmd::text(p, "min");
                if (has(p, "max")) m->max = cmd::text(p, "max");
                if (has(p, "flip")) m->flip = boolParam(p, "flip", false);
                return Result::success("changed " + m->name, m->name);
            })));

    reg.add({"cad.mate_delete", "Delete Mate", "Remove a mate; its components are free to move again.", withDoc({str("mate", "mate name", true)}), true},
            asmCommand(session, reported([](AssemblyDoc& doc, const Params& p) {
                const std::string name = stringParam(p, "mate");
                if (!doc.findMate(name)) return Result::failure("no mate called '" + name + "'");
                doc.mates.erase(std::remove_if(doc.mates.begin(), doc.mates.end(), [&](const Mate& m) { return m.name == name; }), doc.mates.end());
                return Result::success("deleted " + name, name);
            })));

    reg.add({"cad.assembly_move", "Move Component",
             "Place a component (x y z rx ry rz; absolute, or relative=true to move it from where it is). Mates still "
             "hold: it ends up as close as they allow. drag=true holds it where you put it and moves the rest to suit, like "
             "dragging it in a CAD app (with relative=true it turns about its own origin): turn a gear and the other gears turn, swing a link and the linkage follows, up to its "
             "limits. fixed=true/false pins or frees it.",
             withDoc([] {
                 std::vector<ParamSpec> s{str("component", "component name", true), boolean("relative", "move from where it is", false),
                                          boolean("drag", "hold it where it's put and move the rest (falls back to as close as the mates allow)", false),
                                          {"fixed", ParamType::Bool, "pin it in place (or free it)", false, {}, {}}};
                 for (auto& q : placementParams()) s.push_back(q);
                 return s;
             }()),
             true},
            asmCommand(session, reported([](AssemblyDoc& doc, const Params& p) {
                Component* c = doc.findComponent(stringParam(p, "component"));
                if (!c) return Result::failure("no component called '" + stringParam(p, "component") + "'");
                bool any = false;
                for (const auto* k : {"x", "y", "z", "rx", "ry", "rz"}) any = any || has(p, k);
                const bool drag = boolParam(p, "drag", false);
                if (any && drag && boolParam(p, "relative", false)) {
                    // dragging turns it about its own origin (a gear or wheel is modelled round it), as a mouse drag would
                    auto v = [&](const std::string& k) { return has(p, k) ? doc.eval(cmd::text(p, k)) : 0.0; };
                    const mesh::Vec3 centre = c->pose.point({0, 0, 0});
                    c->pose = Mat4::translate(mesh::Vec3{v("x"), v("y"), v("z")} + centre) * Mat4::rotate({v("rx"), v("ry"), v("rz")}) *
                              Mat4::translate(centre * -1) * c->pose;
                } else if (any) {
                    c->pose = poseFrom(doc, p, c->pose, !boolParam(p, "relative", false));
                }
                if (has(p, "fixed")) c->fixed = boolParam(p, "fixed", false);
                if (any && drag) {
                    // try holding it; when the mates can't take it there, fall back to the closest fit
                    const auto poses = [&] {
                        std::vector<Mat4> v;
                        for (const auto& o : doc.components) v.push_back(o.pose);
                        return v;
                    }();
                    doc.hold = c->name;
                    try {
                        doc.solve();
                        return Result::success("dragged " + c->name, c->name);
                    } catch (const std::exception&) {
                        for (std::size_t i = 0; i < doc.components.size(); ++i) doc.components[i].pose = poses[i];
                    }
                }
                return Result::success("moved " + c->name, c->name);
            })));

    registerAssemblyEditCommands(session);
}

void registerAssemblyEditCommands(Session& session) {
    auto& reg = session.registry();

    reg.add({"cad.assembly_remove", "Remove Component", "Remove a component and the mates on it.", withDoc({str("component", "component name", true)}), true},
            asmCommand(session, reported([](AssemblyDoc& doc, const Params& p) {
                const std::string name = stringParam(p, "component");
                if (!doc.findComponent(name)) return Result::failure("no component called '" + name + "'");
                doc.components.erase(std::remove_if(doc.components.begin(), doc.components.end(), [&](const Component& c) { return c.name == name; }),
                                     doc.components.end());
                doc.mates.erase(std::remove_if(doc.mates.begin(), doc.mates.end(),
                                               [&](const Mate& m) { return m.a.rfind(name + "/", 0) == 0 || m.b.rfind(name + "/", 0) == 0; }),
                                doc.mates.end());
                if (!doc.components.empty() && std::none_of(doc.components.begin(), doc.components.end(), [](const Component& c) { return c.fixed; })) {
                    doc.components.front().fixed = true;
                }
                return Result::success("removed " + name, name);
            })));

    reg.add({"cad.assembly_param", "Assembly Parameter", "Set a named parameter that mate values can use (gap = 2, then value=gap).",
             withDoc({str("name", "parameter name", true), str("value", "number or expression", true)}), true},
            asmCommand(session, reported([](AssemblyDoc& doc, const Params& p) {
                const std::string name = stringParam(p, "name");
                if (!validParamName(name)) return Result::failure("'" + name + "' isn't a valid parameter name");
                bool found = false;
                for (auto& par : doc.params) {
                    if (par.name == name) {
                        par.expr = cmd::text(p, "value");
                        found = true;
                    }
                }
                if (!found) doc.params.push_back({name, cmd::text(p, "value")});
                return Result::success(name + " = " + cmd::text(p, "value"), doc.eval(name));
            })));

    reg.add({"cad.assembly_update", "Update Parts",
             "Pull the latest version of every part: from the open cad document it was inserted from, or its file. "
             "Mates are solved again on the new geometry.",
             withDoc({}), true},
            [&session](const Params& p) -> std::unique_ptr<Action> {
                return asmCommand(session, reported([&session](AssemblyDoc& doc, const Params&) {
                    int updated = 0;
                    for (auto& part : doc.parts) {
                        if (auto cad = part.sourceDoc.empty() ? nullptr : session.workspace().getAs<CadDoc>(part.sourceDoc)) {
                            part = partFromDoc(*cad, part.name, part.sourceDoc);
                            ++updated;
                        } else if (!part.source.empty() && std::filesystem::exists(part.source)) {
                            part = partFromFile(part.source, part.name);
                            ++updated;
                        }
                    }
                    return Result::success("updated " + std::to_string(updated) + " parts", static_cast<std::int64_t>(updated));
                }))(p);
            });

    reg.add({"cad.assembly_explode", "Explode Assembly",
             "Show the assembly pulled apart (for pictures, exports and drawings; mates are unchanged): factor 0 puts it "
             "back. axis=radial moves each component away from the centre by factor x its size; +z, -x, ... spread "
             "them along that direction in the order they sit on it.",
             withDoc({str("factor", "how far apart, 0 = assembled", true, "1"),
                      {"axis", ParamType::String, "direction", false, {"radial", "+z", "-z", "+x", "-x", "+y", "-y"}, std::string("radial")}}),
             true},
            asmCommand(session, reported([](AssemblyDoc& doc, const Params& p) {
                doc.explode = doc.eval(cmd::text(p, "factor"));
                if (doc.explode < 0) return Result::failure("factor can't be negative");
                doc.explodeAxis = stringParam(p, "axis", "radial");
                return Result::success(doc.explode == 0 ? "assembled view" : "exploded view (" + doc.explodeAxis + ", x" + num(doc.explode) + ")");
            })));

    reg.add({"cad.assembly_info", "Assembly Info",
             "List the parts, components (part, position, fixed or free) and mates, and how defined the assembly is.",
             {asmParam()}, false, true},
            asmCommand(session, [](AssemblyDoc& doc, const Params& p) {
                std::string t = stringParam(p, "doc") + ": " + doc.summary() + "\nparts:";
                for (const auto& part : doc.parts) t += "\n  " + part.name + (part.source.empty() ? "" : " (" + part.source + ")") + ", volume " + num(meshVolume(*part.mesh));
                t += "\ncomponents:";
                std::string names;
                for (const auto& c : doc.components) {
                    const Vec3 o = c.pose.point({0, 0, 0});
                    const bool free = std::find(doc.freeComponents.begin(), doc.freeComponents.end(), c.name) != doc.freeComponents.end();
                    t += "\n  " + c.name + " (" + c.part + ") at " + num(o.x) + "," + num(o.y) + "," + num(o.z) + (c.fixed ? " fixed" : free ? " free" : " placed");
                    names += (names.empty() ? "" : ",") + c.name;
                }
                t += "\nmates:";
                for (const auto& m : doc.mates) t += "\n  " + m.name + " " + m.type + " " + m.a + " " + m.b + (m.value.empty() ? "" : " = " + m.value) + (m.flip ? " flipped" : "");
                Result r = Result::success(t);
                r.with("doc", stringParam(p, "doc")).with("components", names).with("mates", static_cast<std::int64_t>(doc.mates.size()))
                    .with("dof", static_cast<std::int64_t>(doc.dof));
                for (const auto& c : doc.components) {
                    const Vec3 o = c.pose.point({0, 0, 0});
                    r.with(c.name, NumberList{o.x, o.y, o.z});
                }
                return r;
            }, false));

    reg.add({"cad.assembly_interference", "Check Interference",
             "Find components that overlap each other (solid volume shared, not just touching faces) and how much.",
             {asmParam()}, false, true},
            asmCommand(session, [](AssemblyDoc& doc, const Params& p) {
                std::vector<Mesh> meshes;
                for (const auto& c : doc.components) meshes.push_back(doc.componentMesh(c, false));
                std::string t, pairs;
                double total = 0;
                int count = 0;
                for (std::size_t i = 0; i < meshes.size(); ++i) {
                    for (std::size_t j = i + 1; j < meshes.size(); ++j) {
                        const auto a = mesh::bounds(meshes[i]), b = mesh::bounds(meshes[j]);
                        const double eps = 1e-9 * (a.diagonal() + b.diagonal());
                        if (a.max.x <= b.min.x + eps || b.max.x <= a.min.x + eps || a.max.y <= b.min.y + eps || b.max.y <= a.min.y + eps ||
                            a.max.z <= b.min.z + eps || b.max.z <= a.min.z + eps) {
                            continue;
                        }
                        const double v = meshVolume(detail::csg(meshes[i], meshes[j], mesh::BooleanOp::Intersection));
                        if (v <= 1e-6 * std::min(meshVolume(meshes[i]), meshVolume(meshes[j]))) continue;
                        ++count;
                        total += v;
                        t += "\n  " + doc.components[i].name + " and " + doc.components[j].name + ": " + num(v) + " " + doc.units + "^3";
                        pairs += (pairs.empty() ? "" : ";") + doc.components[i].name + "," + doc.components[j].name;
                    }
                }
                Result r = Result::success(count == 0 ? "no interference" : std::to_string(count) + " interfering pairs" + t, static_cast<std::int64_t>(count));
                return r.with("doc", stringParam(p, "doc")).with("count", static_cast<std::int64_t>(count)).with("volume", total).with("pairs", pairs);
            }, false));

    reg.add({"cad.assembly_bom", "Bill of Materials",
             "List the parts with their quantities, volume and mass (density in g/cm^3, default 1.25); path= also "
             "writes it as CSV.",
             {asmParam(), str("density", "g/cm^3", false, "1.25"), str("path", "write a .csv here")}, false},
            asmCommand(session, [](AssemblyDoc& doc, const Params& p) {
                const double density = doc.eval(stringParam(p, "density", "1.25")), cm = unitsToCm(doc.units);
                std::string t = "item,part,quantity,volume_" + doc.units + "3,mass_g,source\n";
                std::string msg;
                int item = 0, total = 0;
                double massTotal = 0;
                for (const auto& part : doc.parts) {
                    int qty = 0;
                    for (const auto& c : doc.components) qty += c.part == part.name ? 1 : 0;
                    if (qty == 0) continue;
                    const double v = meshVolume(*part.mesh), mass = v * cm * cm * cm * density;
                    ++item;
                    total += qty;
                    massTotal += mass * qty;
                    t += std::to_string(item) + "," + part.name + "," + std::to_string(qty) + "," + num(v) + "," + num(mass) + "," + part.source + "\n";
                    msg += "\n  " + std::to_string(item) + ". " + part.name + " x" + std::to_string(qty) + ", " + num(mass) + " g each";
                }
                if (has(p, "path")) {
                    std::ofstream out(stringParam(p, "path"), std::ios::binary);
                    if (!out) return Result::failure("can't write " + stringParam(p, "path"));
                    out << t;
                }
                Result r = Result::success(std::to_string(item) + " parts, " + std::to_string(total) + " components, " + num(massTotal) + " g" + msg, t);
                return r.with("doc", stringParam(p, "doc")).with("items", static_cast<std::int64_t>(item)).with("quantity", static_cast<std::int64_t>(total))
                    .with("mass_g", massTotal).with("csv", t);
            }, false));

    reg.add({"cad.assembly_save", "Save Assembly",
             "Save an assembly: .wwasm keeps parts, components and mates (reopen with cad.assembly_open); step, stl, "
             "obj, 3mf, glb, ... export the assembled (or exploded) model; png/jpg render it.",
             {asmParam(), str("path", "output file; the extension picks the format", true), {"size", ParamType::Int, "image size", false, {}, std::int64_t{1024}},
              mesh::viewParam()},
             false},
            asmCommand(session, [](AssemblyDoc& doc, const Params& p) {
                const std::string path = stringParam(p, "path");
                std::string ext = std::filesystem::path(path).extension().string();
                for (auto& c : ext) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                if (ext == ".wwasm") {
                    std::ofstream out(path, std::ios::binary);
                    if (!out) return Result::failure("can't write " + path);
                    out << assemblyToJson(doc);
                    doc.path = path;
                    return Result::success("saved " + path, path);
                }
                if (doc.mesh.empty()) return Result::failure("the assembly is empty");
                if (ext == ".png" || ext == ".jpg" || ext == ".jpeg" || ext == ".bmp") {
                    const int size = static_cast<int>(std::clamp<std::int64_t>(intParam(p, "size", 1024), 16, 8192));
                    mesh::RenderOptions o = doc.previewOptions;
                    if (auto v = mesh::parseView(stringParam(p, "view", "iso"))) o.view = *v;
                    raster::save(mesh::render(doc.mesh, size, size, o), path);
                    return Result::success("saved " + path, path);
                }
                if (!mesh::canWrite(path)) return Result::failure("unknown format '" + ext + "'");
                mesh::save(path, doc.mesh);
                return Result::success("saved " + path, path);
            }, false));

    reg.add({"cad.assembly_preview", "Preview Assembly", "Render the assembly (each component in its own colour) to a PNG.",
             {asmParam(), mesh::viewParam(), {"size", ParamType::Int, "image size", false, {}, std::int64_t{512}}}, false, true},
            asmCommand(session, [](AssemblyDoc& doc, const Params& p) {
                if (doc.mesh.empty()) return Result::failure("the assembly is empty");
                const int size = static_cast<int>(std::clamp<std::int64_t>(intParam(p, "size", 512), 16, 4096));
                mesh::RenderOptions o = doc.previewOptions;
                if (auto v = mesh::parseView(stringParam(p, "view", "iso"))) o.view = *v;
                Result r = Result::success("preview");
                r.attach({"image/png", raster::encodePng(mesh::render(doc.mesh, size, size, o)), "assembly.png"});
                return r;
            }, false));
}

}  // namespace wetwaresuite::cad
