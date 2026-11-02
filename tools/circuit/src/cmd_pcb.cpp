// circuit.* board commands (outline, placement, traces, routing, pours, rules), checks,
// fabrication output and the simulator.
#include <algorithm>
#include <set>
#include <sstream>

#include "wetwaresuite/raster/ora.hpp"
#include "cmd_common.hpp"

namespace wetwaresuite::circuit::cmd {

namespace {

int layerParam(const Params& p, const std::string& fallback = "F") {
    const int l = parseLayer(stringParam(p, "layer", fallback));
    return l < 0 ? 0 : l;
}

// Fails with a message when `layer=` names no layer or one the board doesn't have.
std::string layerError(const Board& b, const Params& p) {
    if (!has(p, "layer")) return {};
    const int l = parseLayer(stringParam(p, "layer"));
    if (l < 0) return "layer must be F, B, In1 or In2";
    if (l >= b.layers) return std::string(layerName(l)) + " needs a 4-layer board (circuit.board layers=4)";
    return {};
}

std::vector<ParamSpec> withPreview(std::vector<ParamSpec> v) {
    v.push_back(previewParam());
    return v;
}

// The net of the copper (pad, trace or via) under a point on a layer, "" if none.
std::string netAt(const Circuit& c, const Netlist& nl, Pt q, int layer) {
    for (const auto& pad : worldPads(c, nl)) {
        if (!(pad.layers & layerBit(layer)) || !pad.plated) continue;
        if (std::abs(q.x - pad.pos.x) <= pad.w / 2 + 1e-6 && std::abs(q.y - pad.pos.y) <= pad.h / 2 + 1e-6) return pad.net;
    }
    for (const auto& t : c.board.traces) {
        if (t.layer != layer) continue;
        const double dx = t.b.x - t.a.x, dy = t.b.y - t.a.y, l2 = dx * dx + dy * dy;
        const double u = l2 > 0 ? std::clamp(((q.x - t.a.x) * dx + (q.y - t.a.y) * dy) / l2, 0.0, 1.0) : 0;
        if (std::hypot(q.x - t.a.x - u * dx, q.y - t.a.y - u * dy) <= t.width / 2 + 1e-6) return t.net;
    }
    for (const auto& v : c.board.vias) {
        if (std::hypot(q.x - v.pos.x, q.y - v.pos.y) <= v.diameter / 2 + 1e-6) return v.net;
    }
    return {};
}

}  // namespace

void registerPcbCommands(Session& session) {
    auto& reg = session.registry();
    reg.add({"circuit.board", "Board Outline",
             "Set the board outline in mm: a width x height rectangle with its top-left corner at x, y, or any polygon with "
             "points=x1,y1,x2,y2,... layers=2 (F, B) or 4 (F, In1, In2, B; inner layers are usually ground and power planes).",
             withPreview({docParam(),
                          {"width", ParamType::Number, "rectangle width, mm", false},
                          {"height", ParamType::Number, "rectangle height, mm", false},
                          {"x", ParamType::Number, "left edge, mm", false, {}, 0.0},
                          {"y", ParamType::Number, "top edge, mm", false, {}, 0.0},
                          {"points", ParamType::NumberList, "polygon corners x1,y1,x2,y2,... in mm instead of a rectangle", false},
                          {"layers", ParamType::Int, "copper layers, 2 or 4 (kept when not given)", false, {}, {}}}),
             true},
            circuitCommand(
                session,
                [](CircuitDoc& doc, const Params& p) {
                    Board& b = doc.circuit.board;
                    if (has(p, "layers")) {
                        const auto n = intParam(p, "layers");
                        if (n != 2 && n != 4) return Result::failure("layers must be 2 or 4");
                        if (n == 2) {
                            for (const auto& t : b.traces) {
                                if (t.layer > 1) return Result::failure("there are traces on inner layers: circuit.unroute layer=In1 / In2 first");
                            }
                            std::erase_if(b.zones, [](const Zone& z) { return z.layer > 1; });
                        }
                        b.layers = static_cast<int>(n);
                        if (!has(p, "points") && !has(p, "width") && b.outline.size() >= 3) {
                            return edited(std::to_string(b.layers) + " copper layers; board " + num(boardWidth(b)) + " x " + num(boardHeight(b)) + " mm", p)
                                .with("layers", static_cast<std::int64_t>(b.layers));
                        }
                    }
                    if (has(p, "points")) {
                        const NumberList v = listParam(p, "points");
                        if (v.size() < 6 || v.size() % 2) return Result::failure("points needs at least three x,y pairs");
                        b.outline.clear();
                        for (std::size_t i = 0; i + 1 < v.size(); i += 2) b.outline.push_back({v[i], v[i + 1]});
                    } else {
                        if (!has(p, "width") || !has(p, "height")) return Result::failure("give width and height (mm), or points");
                        const double w = numberParam(p, "width"), h = numberParam(p, "height"), x = numberParam(p, "x", 0), y = numberParam(p, "y", 0);
                        if (w <= 0 || h <= 0 || w > 1000 || h > 1000) return Result::failure("width and height must be 0..1000 mm");
                        b.outline = {{x, y}, {x + w, y}, {x + w, y + h}, {x, y + h}};
                    }
                    return edited("board " + num(boardWidth(b)) + " x " + num(boardHeight(b)) + " mm, " + std::to_string(b.layers) + " copper layers", p)
                        .with("layers", static_cast<std::int64_t>(b.layers))
                        .with("width", boardWidth(b))
                        .with("height", boardHeight(b));
                },
                View::Pcb));

    reg.add({"circuit.pcb_place", "Place Footprints",
             "Put the footprints of schematic parts on the board, in rows, neighbours (by connectivity) together. Parts already "
             "placed stay unless all=true. With no outline yet, a board is made to fit. Then circuit.footprint_move to tidy, "
             "circuit.route, circuit.drc.",
             withPreview({docParam(), {"all", ParamType::Bool, "re-place parts already on the board too", false, {}, false},
                          {"spacing", ParamType::Number, "gap between footprints, mm", false, {}, 2.0}}),
             true},
            circuitCommand(
                session,
                [](CircuitDoc& doc, const Params& p) {
                    const Netlist nl = buildNetlist(doc.circuit);
                    const int n = placeFootprints(doc.circuit, nl, boolParam(p, "all", false), std::clamp(numberParam(p, "spacing", 2.0), 0.0, 50.0));
                    std::string s = "placed " + std::to_string(n) + " footprints; board " + num(boardWidth(doc.circuit.board)) + " x " +
                                    num(boardHeight(doc.circuit.board)) + " mm";
                    double x0, y0, x1, y1;
                    boardBounds(doc.circuit.board, x0, y0, x1, y1);
                    for (const auto& part : doc.circuit.parts) {
                        if (!part.pcb.placed) continue;
                        if (!insideBoard(doc.circuit.board, part.pcb.pos)) s += "\nwarning: " + part.ref + " is outside the board (make it bigger or move parts)";
                    }
                    return edited(s, p).with("placed", static_cast<std::int64_t>(n));
                },
                View::Pcb));

    reg.add({"circuit.footprint_move", "Move Footprint",
             "Move, rotate (multiples of 90, counter-clockwise) or flip a footprint on the board. x, y in mm are the footprint centre.",
             withPreview({docParam(),
                          {"ref", ParamType::String, "the part", true},
                          {"x", ParamType::Number, "centre x, mm", false},
                          {"y", ParamType::Number, "centre y, mm", false},
                          {"dx", ParamType::Number, "move by, mm", false},
                          {"dy", ParamType::Number, "move by, mm", false},
                          {"rot", ParamType::Int, "rotation in degrees counter-clockwise, any angle (e.g. 45)", false},
                          {"side", ParamType::String, "top or bottom", false, {"top", "bottom"}}}),
             true},
            circuitCommand(
                session,
                [](CircuitDoc& doc, const Params& p) {
                    Part* part = doc.circuit.part(stringParam(p, "ref"));
                    if (!part) return Result::failure("no part " + stringParam(p, "ref"));
                    if (!footprintOf(*part)) return Result::failure(part->ref + " has no footprint");
                    if (!part->pcb.placed && !(has(p, "x") && has(p, "y"))) return Result::failure(part->ref + " isn't on the board yet: give x and y, or run circuit.pcb_place");
                    part->pcb.placed = true;
                    if (has(p, "x")) part->pcb.pos.x = numberParam(p, "x");
                    if (has(p, "y")) part->pcb.pos.y = numberParam(p, "y");
                    part->pcb.pos.x += numberParam(p, "dx", 0);
                    part->pcb.pos.y += numberParam(p, "dy", 0);
                    if (has(p, "rot")) part->pcb.rot = static_cast<int>(((intParam(p, "rot") % 360) + 360) % 360);
                    if (has(p, "side")) part->pcb.bottom = stringParam(p, "side") == "bottom";
                    return edited(part->ref + " at " + pt(part->pcb.pos) + " mm, rot " + std::to_string(part->pcb.rot) + (part->pcb.bottom ? ", bottom" : ", top"), p);
                },
                View::Pcb));

    reg.add({"circuit.trace", "Draw Trace",
             "Draw copper through points=x1,y1,x2,y2,... (mm) on layer F, B (or In1, In2 on 4 layers). The net comes from the pad or trace under the first "
             "point unless net= is given; width defaults to the rules (power nets wider).",
             withPreview({docParam(),
                          {"points", ParamType::NumberList, "x1,y1,x2,y2,... in mm", true},
                          {"layer", ParamType::String, "F (top), B (bottom), In1 or In2", false, {"F", "B", "In1", "In2"}, std::string("F")},
                          {"width", ParamType::Number, "trace width, mm", false},
                          {"net", ParamType::String, "net name (default: from the copper under the first point)", false},
                          {"shove", ParamType::Bool, "push other nets' traces aside instead of overlapping them", false, {}, false}}),
             true},
            circuitCommand(
                session,
                [](CircuitDoc& doc, const Params& p) {
                    Circuit& c = doc.circuit;
                    const NumberList v = listParam(p, "points");
                    if (v.size() < 4 || v.size() % 2) return Result::failure("points needs pairs: x1,y1,x2,y2,...");
                    if (auto e = layerError(c.board, p); !e.empty()) return Result::failure(e);
                    const int layer = layerParam(p);
                    const Netlist nl = buildNetlist(c);
                    std::string net = stringParam(p, "net");
                    if (net.empty()) net = netAt(c, nl, {v[0], v[1]}, layer);
                    if (net.empty()) net = netAt(c, nl, {v[v.size() - 2], v[v.size() - 1]}, layer);
                    const double width = has(p, "width") ? numberParam(p, "width") : (isPowerName(net) ? c.board.rules.powerTrace : c.board.rules.trace);
                    if (width <= 0) return Result::failure("width must be positive");
                    std::vector<Trace> segs;
                    for (std::size_t i = 2; i + 1 < v.size(); i += 2) segs.push_back({{v[i - 2], v[i - 1]}, {v[i], v[i + 1]}, width, layer, net});
                    const int n = static_cast<int>(segs.size());
                    std::string extra;
                    ShoveReport shoved;
                    if (boolParam(p, "shove", false)) {
                        shoved = addTraceShoving(c, nl, segs);
                        if (shoved.moved) extra += "; pushed " + std::to_string(shoved.moved) + " trace" + (shoved.moved == 1 ? "" : "s") + " aside";
                        if (!shoved.blocked.empty()) {
                            extra += "; still too close:";
                            for (const auto& b : shoved.blocked) extra += " " + b;
                        }
                    } else {
                        for (const auto& s : segs) c.board.traces.push_back(s);
                    }
                    return edited(std::to_string(n) + " trace segments on " + layerName(layer) + (net.empty() ? " (no net: it touches no pad)" : " net " + net) + extra, p)
                        .with("net", net)
                        .with("shoved", static_cast<std::int64_t>(shoved.moved))
                        .with("blocked", static_cast<std::int64_t>(shoved.blocked.size()));
                },
                View::Pcb));

    reg.add({"circuit.via", "Add Via", "Add a via (a plated hole joining every copper layer) at x, y mm.",
             withPreview({docParam(),
                          {"x", ParamType::Number, "x, mm", true},
                          {"y", ParamType::Number, "y, mm", true},
                          {"net", ParamType::String, "net (default: from the copper there)", false},
                          {"drill", ParamType::Number, "hole, mm (default from rules)", false},
                          {"diameter", ParamType::Number, "pad diameter, mm (default from rules)", false}}),
             true},
            circuitCommand(
                session,
                [](CircuitDoc& doc, const Params& p) {
                    Circuit& c = doc.circuit;
                    const Pt q{numberParam(p, "x"), numberParam(p, "y")};
                    std::string net = stringParam(p, "net");
                    if (net.empty()) {
                        const Netlist nl = buildNetlist(c);
                        for (int l = 0; l < c.board.layers && net.empty(); ++l) net = netAt(c, nl, q, l);
                    }
                    c.board.vias.push_back({q, numberParam(p, "drill", c.board.rules.viaDrill), numberParam(p, "diameter", c.board.rules.viaDiameter), net});
                    return edited("via at " + pt(q) + (net.empty() ? "" : " on " + net), p).with("net", net);
                },
                View::Pcb));

    reg.add({"circuit.route", "Autoroute",
             "Route unrouted connections with a grid maze router on the copper layers (vias where it helps). Short nets go first; a "
             "second pass retries failed nets first. Good for small boards; check with circuit.drc. Route before pouring copper.",
             withPreview({docParam(),
                          {"nets", ParamType::String, "only these nets, comma-separated", false},
                          {"skip", ParamType::String, "leave these nets alone, comma-separated (e.g. GND for a pour)", false},
                          {"layers", ParamType::Int, "1 = top only, 2 = top and bottom, 4 = all four (4-layer boards)", false, {}, std::int64_t{2}},
                          {"via_cost", ParamType::Number, "how much a via costs, in grid steps (higher = fewer vias)", false, {}, 8.0},
                          {"time_limit", ParamType::Int, "seconds", false, {}, std::int64_t{60}}}),
             true},
            circuitCommand(
                session,
                [](CircuitDoc& doc, const Params& p) {
                    if (doc.circuit.board.outline.size() < 3) return Result::failure("no board yet: circuit.board or circuit.pcb_place first");
                    const Netlist nl = buildNetlist(doc.circuit);
                    RouteOptions o;
                    if (has(p, "nets")) o.nets = splitList(stringParam(p, "nets"));
                    if (has(p, "skip")) o.skip = splitList(stringParam(p, "skip"));
                    o.layers = static_cast<int>(std::clamp<std::int64_t>(intParam(p, "layers", 2), 1, doc.circuit.board.layers));
                    if (o.layers == 3) o.layers = 2;
                    o.viaCost = std::clamp(numberParam(p, "via_cost", 8), 0.0, 1000.0);
                    o.maxSeconds = static_cast<int>(std::clamp<std::int64_t>(intParam(p, "time_limit", 60), 1, 3600));
                    const RouteReport r = autoroute(doc.circuit, nl, o);
                    std::string s = "routed " + std::to_string(r.routed) + " of " + std::to_string(r.connections) + " connections (" + std::to_string(r.segments) +
                                    " segments, " + std::to_string(r.vias) + " vias)";
                    for (const auto& f : r.failedLinks) s += "\nfailed: " + f;
                    if (r.failed) s += "\ntip: move parts apart (circuit.footprint_move), enlarge the board, or set a smaller trace/clearance with circuit.rules";
                    Result res = edited(s, p);
                    res.with("routed", static_cast<std::int64_t>(r.routed)).with("failed", static_cast<std::int64_t>(r.failed)).with("vias", static_cast<std::int64_t>(r.vias));
                    return res;
                },
                View::Pcb));

    reg.add({"circuit.unroute", "Remove Traces", "Remove traces and vias: of one net (net=), of one layer (layer=), or all of them.",
             withPreview({docParam(), {"net", ParamType::String, "only this net", false}, {"layer", ParamType::String, "only this layer (F, B, In1, In2; vias stay)", false, {"F", "B", "In1", "In2"}}}),
             true},
            circuitCommand(
                session,
                [](CircuitDoc& doc, const Params& p) {
                    Board& b = doc.circuit.board;
                    const std::size_t t0 = b.traces.size(), v0 = b.vias.size();
                    const bool byNet = has(p, "net"), byLayer = has(p, "layer");
                    const std::string net = stringParam(p, "net");
                    const int layer = layerParam(p);
                    std::erase_if(b.traces, [&](const Trace& t) { return (!byNet || t.net == net) && (!byLayer || t.layer == layer); });
                    if (!byLayer) std::erase_if(b.vias, [&](const Via& v) { return !byNet || v.net == net; });
                    return edited("removed " + std::to_string(t0 - b.traces.size()) + " traces and " + std::to_string(v0 - b.vias.size()) + " vias", p);
                },
                View::Pcb));

    reg.add({"circuit.pour", "Copper Pour",
             "Fill a layer with copper on a net (usually GND), keeping clearance from other nets and the board edge; islands that "
             "touch no pad of the net are dropped. Recomputed whenever the board changes.",
             withPreview({docParam(),
                          {"net", ParamType::String, "net to pour", false, {}, std::string("GND")},
                          {"layer", ParamType::String, "F, B, both, or In1/In2 on a 4-layer board (a plane)", false, {"F", "B", "both", "In1", "In2"}, std::string("B")},
                          {"clearance", ParamType::Number, "gap to other copper, mm", false, {}, 0.3},
                          {"thermal", ParamType::Bool, "join pads through thermal-relief spokes (easier hand soldering); false = solid", false, {}, true},
                          {"spoke", ParamType::Number, "thermal spoke width, mm", false, {}, 0.5}}),
             true},
            circuitCommand(
                session,
                [](CircuitDoc& doc, const Params& p) {
                    Circuit& c = doc.circuit;
                    const std::string net = stringParam(p, "net", "GND");
                    const Netlist nl = buildNetlist(c);
                    if (!nl.net(net)) return Result::failure("no net called " + net + " (circuit.nets lists them)");
                    const std::string realName = nl.net(net)->name;
                    if (c.board.outline.size() < 3) return Result::failure("no board outline yet");
                    const std::string l = stringParam(p, "layer", "B");
                    std::vector<int> layers;
                    if (l == "both") layers = {0, 1};
                    else {
                        if (auto e = layerError(c.board, p); !e.empty()) return Result::failure(e);
                        layers = {layerParam(p, "B")};
                    }
                    for (int layer : layers) {
                        std::erase_if(c.board.zones, [&](const Zone& z) { return z.layer == layer; });
                        Zone z{realName, layer, std::max(0.1, numberParam(p, "clearance", 0.3)), 0.25};
                        z.thermal = boolParam(p, "thermal", true);
                        z.spoke = std::clamp(numberParam(p, "spoke", 0.5), 0.15, 5.0);
                        c.board.zones.push_back(z);
                    }
                    const auto fills = fillZones(c, nl);
                    double area = 0;
                    for (const auto& f : fills) {
                        if (std::find(layers.begin(), layers.end(), f.layer) == layers.end()) continue;  // only this pour
                        area += static_cast<double>(std::count(f.cells.begin(), f.cells.end(), 1)) * f.cell * f.cell;
                    }
                    return edited("poured " + realName + " on " + (l == "both" ? "both outer layers" : layerName(layers[0])) + ", " + num(area) + " mm2 of copper", p)
                        .with("area", area);
                },
                View::Pcb));

    reg.add({"circuit.unpour", "Remove Pour", "Remove copper pours (all, or one net's).",
             withPreview({docParam(), {"net", ParamType::String, "only this net's", false}}), true},
            circuitCommand(
                session,
                [](CircuitDoc& doc, const Params& p) {
                    const std::size_t n = doc.circuit.board.zones.size();
                    std::erase_if(doc.circuit.board.zones, [&](const Zone& z) { return !has(p, "net") || z.net == stringParam(p, "net"); });
                    return edited("removed " + std::to_string(n - doc.circuit.board.zones.size()) + " pours", p);
                },
                View::Pcb));

    reg.add({"circuit.rules", "Design Rules",
             "Show or set the design rules in mm: trace width, power-net trace width, clearance, minimum trace, via drill and "
             "diameter, minimum drill, edge clearance and the router grid. Defaults suit cheap 2-layer fabs (0.25 trace, 0.2 clearance).",
             withPreview({docParam(),
                          {"trace", ParamType::Number, "default trace width", false},
                          {"power_trace", ParamType::Number, "trace width for power nets (GND, VCC, +5V ...)", false},
                          {"clearance", ParamType::Number, "copper to copper", false},
                          {"min_trace", ParamType::Number, "DRC minimum width", false},
                          {"via_drill", ParamType::Number, "via hole", false},
                          {"via_diameter", ParamType::Number, "via pad", false},
                          {"min_drill", ParamType::Number, "DRC minimum hole", false},
                          {"edge_clearance", ParamType::Number, "copper to board edge", false},
                          {"grid", ParamType::Number, "autorouter grid (smaller = finer, slower)", false}}),
             true},
            circuitCommand(
                session,
                [](CircuitDoc& doc, const Params& p) {
                    Rules& r = doc.circuit.board.rules;
                    auto set = [&](const char* key, double& field, double lo, double hi) {
                        if (has(p, key)) field = std::clamp(numberParam(p, key), lo, hi);
                    };
                    set("trace", r.trace, 0.05, 10);
                    set("power_trace", r.powerTrace, 0.05, 10);
                    set("clearance", r.clearance, 0.05, 10);
                    set("min_trace", r.minTrace, 0.01, 10);
                    set("via_drill", r.viaDrill, 0.1, 5);
                    set("via_diameter", r.viaDiameter, 0.2, 6);
                    set("min_drill", r.minDrill, 0.05, 5);
                    set("edge_clearance", r.edgeClearance, 0, 10);
                    set("grid", r.grid, 0.05, 2.54);
                    const std::string s = "rules (mm): trace " + num(r.trace) + ", power_trace " + num(r.powerTrace) + ", clearance " + num(r.clearance) + ", min_trace " +
                                          num(r.minTrace) + ", via " + num(r.viaDrill) + "/" + num(r.viaDiameter) + ", min_drill " + num(r.minDrill) +
                                          ", edge_clearance " + num(r.edgeClearance) + ", grid " + num(r.grid);
                    return edited(s, p);
                },
                View::Pcb));
}

void registerPcbOutputCommands(Session& session) {
    auto& reg = session.registry();
    reg.add({"circuit.ratsnest", "Ratsnest", "List unrouted connections (airwires): per net, the shortest links still missing between its copper islands.",
             {docParam(), previewParam()}, false, true},
            circuitCommand(
                session,
                [](CircuitDoc& doc, const Params& p) {
                    const Netlist nl = buildNetlist(doc.circuit);
                    const auto air = ratsnest(doc.circuit, nl);
                    std::string s = std::to_string(air.size()) + " unrouted connections";
                    for (std::size_t i = 0; i < air.size() && i < 60; ++i) s += "\n" + air[i].net + ": " + air[i].from + " -> " + air[i].to;
                    return edited(s, p).with("unrouted", static_cast<std::int64_t>(air.size()));
                },
                View::Pcb, false));

    reg.add({"circuit.pads", "List Pads",
             "Board positions (mm) of pads with their size, layers and net: of one part (ref=), of one net (net=), or all. Use them "
             "for circuit.trace and circuit.via by hand.",
             {docParam(), {"ref", ParamType::String, "only this part", false}, {"net", ParamType::String, "only this net", false}}, false, true},
            circuitCommand(
                session,
                [](CircuitDoc& doc, const Params& p) {
                    const Netlist nl = buildNetlist(doc.circuit);
                    std::string s;
                    std::int64_t n = 0;
                    for (const auto& pad : worldPads(doc.circuit, nl)) {
                        if (has(p, "ref") && lower(pad.ref) != lower(stringParam(p, "ref"))) continue;
                        if (has(p, "net") && lower(pad.net) != lower(stringParam(p, "net"))) continue;
                        s += "\n" + pad.ref + "." + pad.number + " at " + pt(pad.pos) + " size " + num(pad.w) + "x" + num(pad.h) +
                             (pad.drill > 0 ? " drill " + num(pad.drill) + (pad.plated ? " (all layers)" : " (non-plated)") : pad.layers == 1 ? " (F)" : " (B)") + " net " + (pad.net.empty() ? "-" : pad.net);
                        ++n;
                    }
                    if (n == 0) return Result::failure("no pads match (is the part on the board? circuit.pcb_place)");
                    return edited(std::to_string(n) + " pads:" + s, p).with("pads", n);
                },
                View::Pcb, false));

    reg.add({"circuit.drc", "Design Rules Check",
             "Check the board: clearance and shorts between nets, trace width, via drill and annular ring, copper too near or past "
             "the edge, overlapping footprints, parts not placed and unrouted connections.",
             {docParam(), previewParam()}, false, true},
            circuitCommand(
                session,
                [](CircuitDoc& doc, const Params& p) {
                    const Netlist nl = buildNetlist(doc.circuit);
                    int errors = 0, warnings = 0;
                    const std::string text = issuesText(drc(doc.circuit, nl), errors, warnings);
                    Result r = edited("DRC: " + std::to_string(errors) + " errors, " + std::to_string(warnings) + " warnings" + text, p);
                    r.with("errors", static_cast<std::int64_t>(errors)).with("warnings", static_cast<std::int64_t>(warnings));
                    return r;
                },
                View::Pcb, false));

    reg.add({"circuit.gerber", "Export Gerber",
             "Write fabrication files: Gerber RS-274X for copper (2 or 4 layers), mask, paste and silkscreen plus the board edge "
             "(Edge_Cuts), Excellon drill files (plated, and non-plated when there are mounting holes), and for assembly a "
             "pick-and-place CSV (NAME-pos.csv) and a BOM (NAME-bom.csv). path ending in .zip writes one zip ready to upload to a "
             "fab; otherwise a folder.",
             {docParam(),
              {"path", ParamType::String, "output folder, or a .zip file", true},
              {"name", ParamType::String, "file name prefix (default: the title or 'board')", false},
              {"assembly", ParamType::Bool, "also write the pick-and-place and BOM CSVs", false, {}, true}},
             false},
            circuitCommand(
                session,
                [](CircuitDoc& doc, const Params& p) {
                    if (doc.circuit.board.outline.size() < 3) return Result::failure("no board outline (circuit.board)");
                    const Netlist nl = buildNetlist(doc.circuit);
                    std::string base = stringParam(p, "name");
                    if (base.empty()) base = doc.circuit.title.empty() ? "board" : doc.circuit.title;
                    for (auto& ch : base) {
                        if (!std::isalnum(static_cast<unsigned char>(ch)) && ch != '-' && ch != '_') ch = '_';
                    }
                    auto files = fabFiles(doc.circuit, nl, base);
                    if (boolParam(p, "assembly", true)) {
                        files.push_back({base + "-pos.csv", placementCsv(doc.circuit)});
                        files.push_back({base + "-bom.csv", bomCsv(doc.circuit, true)});
                    }
                    const fs::path path = stringParam(p, "path");
                    std::string error;
                    std::string list;
                    for (const auto& f : files) list += "\n  " + f.name + " (" + std::to_string(f.data.size()) + " bytes)";
                    if (lower(path.extension().string()) == ".zip") {
                        std::vector<raster::ZipEntry> entries;
                        for (const auto& f : files) entries.push_back({f.name, f.data});
                        if (!writeFile(path, raster::writeZip(entries), error)) return Result::failure(error);
                    } else {
                        for (const auto& f : files) {
                            if (!writeFile(path / f.name, f.data, error)) return Result::failure(error);
                        }
                    }
                    const auto air = ratsnest(doc.circuit, nl);
                    std::string warn = air.empty() ? "" : "\nwarning: " + std::to_string(air.size()) + " connections are not routed (circuit.drc)";
                    return edited("wrote " + std::to_string(files.size()) + " files to " + path.string() + list + warn, p)
                        .with("files", static_cast<std::int64_t>(files.size()))
                        .with("path", path.string());
                },
                View::Pcb, false));

    reg.add({"circuit.placement", "Pick and Place File",
             "Write the pick-and-place (component placement, CPL) CSV for assembly: Designator, Val, Package, Mid X, Mid Y, "
             "Rotation, Layer, in mm with y up from the board's bottom-left corner (the format JLCPCB and most assemblers read).",
             {docParam(), {"path", ParamType::String, "output .csv (omit to just print it)", false}}, false, true},
            circuitCommand(
                session,
                [](CircuitDoc& doc, const Params& p) {
                    const std::string csv = placementCsv(doc.circuit);
                    const auto rows = static_cast<std::int64_t>(std::count(csv.begin(), csv.end(), '\n') - 1);
                    if (!has(p, "path")) return edited(csv, p).with("parts", rows);
                    std::string error;
                    if (!writeFile(stringParam(p, "path"), csv, error)) return Result::failure(error);
                    return edited("wrote " + stringParam(p, "path") + " (" + std::to_string(rows) + " parts)", p).with("parts", rows);
                },
                View::Pcb, false));

    reg.add({"circuit.sim_op", "DC Operating Point",
             "Simulate the DC operating point (SPICE-lite): every net's voltage and each source's current. Handles resistors, "
             "capacitors (open), inductors (short), diodes/LEDs, NPN/PNP transistors (fields beta, br, is), N/P MOSFETs (fields vth, k, "
             "lambda), switches (field state=on), potentiometers (field wiper=0..1), "
             "op-amps (ideal), voltage/current sources and batteries; power symbols named like +5V or +3V3 become sources. Ground is GND.",
             {docParam(), {"ignore_unsupported", ParamType::Bool, "leave out parts the simulator can't model (ICs, modules)", false, {}, false}},
             false, true},
            circuitCommand(
                session,
                [](CircuitDoc& doc, const Params& p) {
                    const Netlist nl = buildNetlist(doc.circuit);
                    SimOptions o;
                    o.ignoreUnsupported = boolParam(p, "ignore_unsupported", false);
                    const SimResult r = simulate(doc.circuit, nl, o);
                    std::string s = "operating point (" + std::to_string(r.iterations) + " iterations):";
                    Result res = edited("", p);
                    for (std::size_t i = 0; i < r.nets.size(); ++i) {
                        s += "\n  V(" + r.nets[i] + ") = " + formatValue(r.voltages[i]) + "V";
                        res.with("V(" + r.nets[i] + ")", r.voltages[i]);
                    }
                    for (const auto& [name, i] : r.currents) {
                        s += "\n  I(" + name + ") = " + formatValue(i) + "A delivered";
                        res.with("I(" + name + ")", i);
                    }
                    for (const auto& w : r.warnings) s += "\nnote: " + w;
                    res.message = s;
                    return res;
                },
                View::Schematic, false));

    reg.add({"circuit.sim_ac", "AC Sweep",
             "Frequency response (a Bode plot): every part is linearised at the DC operating point and a voltage source "
             "sweeps from start to stop. The source with field ac=<amplitude> drives it (else the first vsource, at 1 V). "
             "Reports gain (dB) and phase per probed net, the -3 dB corner(s) and the peak; preview=true attaches the plot; "
             "path= writes CSV.",
             {docParam(),
              {"start", ParamType::String, "first frequency, e.g. 10 (Hz)", false, {}, std::string("10")},
              {"stop", ParamType::String, "last frequency, e.g. 1meg", false, {}, std::string("1meg")},
              {"per_decade", ParamType::Int, "points per decade", false, {}, std::int64_t{20}},
              {"probes", ParamType::String, "nets to record, comma-separated (default: up to 8)", false},
              {"path", ParamType::String, "write freq,dB(net),deg(net)... CSV here", false},
              {"ignore_unsupported", ParamType::Bool, "leave out parts the simulator can't model", false, {}, false},
              previewParam()},
             false},
            [&session](const Params& p) -> std::unique_ptr<Action> {
                auto doc = session.workspace().getAs<CircuitDoc>(stringParam(p, "doc"));
                return std::make_unique<FunctionAction>([doc, p] {
                    if (!doc) return Result::failure("no open circuit called '" + stringParam(p, "doc") + "'");
                    try {
                        const Netlist nl = buildNetlist(doc->circuit);
                        SimOptions o;
                        const auto f0 = parseValue(stringParam(p, "start", "10")), f1 = parseValue(stringParam(p, "stop", "1meg"));
                        if (!f0 || !f1 || *f0 <= 0 || *f1 <= *f0) return Result::failure("start and stop must be frequencies with stop above start, e.g. start=10 stop=100k");
                        o.acStart = *f0;
                        o.acStop = *f1;
                        o.acPerDecade = static_cast<int>(intParam(p, "per_decade", 20));
                        if (has(p, "probes")) o.probes = splitList(stringParam(p, "probes"));
                        o.ignoreUnsupported = boolParam(p, "ignore_unsupported", false);
                        const SimResult r = simulate(doc->circuit, nl, o);
                        std::string s = "ac sweep " + formatValue(o.acStart) + "Hz to " + formatValue(o.acStop) + "Hz, " + std::to_string(r.freqs.size()) + " points:";
                        Result res = Result::success("");
                        res.with("doc", stringParam(p, "doc"));
                        for (std::size_t i = 0; i < r.acProbes.size(); ++i) {
                            const auto& m = r.acMag[i];
                            const auto peak = std::max_element(m.begin(), m.end());
                            const std::size_t pk = static_cast<std::size_t>(peak - m.begin());
                            s += "\n  " + r.acProbes[i] + ": " + formatValue(m.front()) + " dB at " + formatValue(r.freqs.front()) + "Hz";
                            if (pk != 0 && pk + 1 != m.size()) s += ", peak " + formatValue(*peak) + " dB at " + formatValue(r.freqs[pk]) + "Hz";
                            s += ", " + formatValue(m.back()) + " dB at " + formatValue(r.freqs.back()) + "Hz";
                            // -3 dB corners from the peak, interpolated on the log scale
                            std::vector<double> corners;
                            for (std::size_t k = 1; k < m.size(); ++k) {
                                const double a = m[k - 1] - (*peak - 3), b = m[k] - (*peak - 3);
                                if ((a > 0) != (b > 0)) {
                                    const double t = a / (a - b);
                                    corners.push_back(std::pow(10.0, std::log10(r.freqs[k - 1]) + t * (std::log10(r.freqs[k]) - std::log10(r.freqs[k - 1]))));
                                }
                            }
                            if (!corners.empty()) {
                                s += ", -3 dB at";
                                for (double f : corners) s += " " + formatValue(f) + "Hz";
                                res.with("f3db(" + r.acProbes[i] + ")", corners.front());
                            }
                            res.with("gain_db(" + r.acProbes[i] + ")", *peak);
                        }
                        for (const auto& w : r.warnings) s += "\nnote: " + w;
                        if (has(p, "path")) {
                            std::ostringstream csv;
                            csv << "freq";
                            for (const auto& pr : r.acProbes) csv << ",dB(" << pr << "),deg(" << pr << ")";
                            csv << "\n";
                            for (std::size_t k = 0; k < r.freqs.size(); ++k) {
                                csv << r.freqs[k];
                                for (std::size_t i = 0; i < r.acProbes.size(); ++i) csv << "," << r.acMag[i][k] << "," << r.acPhase[i][k];
                                csv << "\n";
                            }
                            std::string error;
                            if (!writeFile(stringParam(p, "path"), csv.str(), error)) return Result::failure(error);
                            s += "\nwrote " + stringParam(p, "path");
                        }
                        res.message = s;
                        if (boolParam(p, "preview", false)) res.attach({"image/png", raster::encodePng(plotBode(r, 900, 520)), "bode.png"});
                        return res;
                    } catch (const std::exception& e) {
                        return Result::failure(e.what());
                    }
                });
            });

    reg.add({"circuit.sim_tran", "Transient Simulation",
             "Simulate over time with a fixed step (backward Euler): sources can be SIN(offset amplitude freq) or PULSE(v1 v2 delay "
             "rise fall width period). Reports min/max/final per probed net; preview=true attaches a plot; path= writes CSV.",
             {docParam(),
              {"tstop", ParamType::String, "end time, e.g. 10m (10 ms)", true},
              {"step", ParamType::String, "time step (default tstop/500)", false},
              {"probes", ParamType::String, "nets to record, comma-separated (default: up to 8)", false},
              {"path", ParamType::String, "write time,V(net)... CSV here", false},
              {"ignore_unsupported", ParamType::Bool, "leave out parts the simulator can't model", false, {}, false},
              previewParam()},
             false},
            [&session](const Params& p) -> std::unique_ptr<Action> {
                auto doc = session.workspace().getAs<CircuitDoc>(stringParam(p, "doc"));
                return std::make_unique<FunctionAction>([doc, p] {
                    if (!doc) return Result::failure("no open circuit called '" + stringParam(p, "doc") + "'");
                    try {
                        const Netlist nl = buildNetlist(doc->circuit);
                        SimOptions o;
                        auto tstop = parseValue(stringParam(p, "tstop"));
                        if (!tstop || *tstop <= 0) return Result::failure("tstop must be a time such as 5m or 0.01");
                        o.tstop = *tstop;
                        if (has(p, "step")) {
                            auto st = parseValue(stringParam(p, "step"));
                            if (!st || *st <= 0) return Result::failure("step must be a positive time");
                            o.step = *st;
                        }
                        if (has(p, "probes")) o.probes = splitList(stringParam(p, "probes"));
                        o.ignoreUnsupported = boolParam(p, "ignore_unsupported", false);
                        const SimResult r = simulate(doc->circuit, nl, o);
                        std::string s = "transient to " + formatValue(o.tstop) + "s, " + std::to_string(r.times.size()) + " points:";
                        Result res = Result::success("");
                        res.with("doc", stringParam(p, "doc"));
                        for (std::size_t i = 0; i < r.probes.size(); ++i) {
                            const auto& v = r.series[i];
                            const auto [mn, mx] = std::minmax_element(v.begin(), v.end());
                            s += "\n  V(" + r.probes[i] + "): min " + formatValue(*mn) + "V, max " + formatValue(*mx) + "V, final " + formatValue(v.back()) + "V";
                            res.with("V(" + r.probes[i] + ")", v.back());
                        }
                        for (const auto& w : r.warnings) s += "\nnote: " + w;
                        if (has(p, "path")) {
                            std::ostringstream csv;
                            csv << "time";
                            for (const auto& pr : r.probes) csv << ",V(" << pr << ")";
                            csv << "\n";
                            for (std::size_t k = 0; k < r.times.size(); ++k) {
                                csv << r.times[k];
                                for (const auto& sr : r.series) csv << "," << sr[k];
                                csv << "\n";
                            }
                            std::string error;
                            if (!writeFile(stringParam(p, "path"), csv.str(), error)) return Result::failure(error);
                            s += "\nwrote " + stringParam(p, "path");
                        }
                        res.message = s;
                        if (boolParam(p, "preview", false)) res.attach({"image/png", raster::encodePng(plotSeries(r, 900, 420)), "transient.png"});
                        return res;
                    } catch (const std::exception& e) {
                        return Result::failure(e.what());
                    }
                });
            });
}

}  // namespace wetwaresuite::circuit::cmd

namespace wetwaresuite::circuit {

void registerCommands(Session& session) {
    cmd::registerFileCommands(session);
    cmd::registerSchematicCommands(session);
    cmd::registerSchematicMarkCommands(session);
    cmd::registerSchematicQueryCommands(session);
    cmd::registerBusCommands(session);
    cmd::registerPcbCommands(session);
    cmd::registerPcbOutputCommands(session);
    cmd::registerBoardToolCommands(session);
    cmd::registerViewCommands(session);
    cmd::registerFootprintEditCommands(session);
}

}  // namespace wetwaresuite::circuit
