// circuit.* schematic commands: placing parts, wiring, labels, power symbols, checks and exports.
#include <algorithm>
#include <set>

#include "cmd_common.hpp"

namespace wetwaresuite::circuit::cmd {

namespace {

bool samePt(Pt a, Pt b) { return std::abs(a.x - b.x) < 1e-6 && std::abs(a.y - b.y) < 1e-6; }

bool onInterior(Pt p, Pt a, Pt b) {
    if (samePt(p, a) || samePt(p, b)) return false;
    const double cross = (b.x - a.x) * (p.y - a.y) - (b.y - a.y) * (p.x - a.x);
    if (std::abs(cross) > 1e-6) return false;
    const double dot = (p.x - a.x) * (b.x - a.x) + (p.y - a.y) * (b.y - a.y);
    return dot > 0 && dot < (b.x - a.x) * (b.x - a.x) + (b.y - a.y) * (b.y - a.y);
}

std::map<std::string, std::string> parseFields(const std::string& text) {
    std::map<std::string, std::string> out;
    const char sep = text.find(';') != std::string::npos ? ';' : ',';
    for (const auto& item : splitList(text, sep)) {
        const auto eq = item.find('=');
        if (eq == std::string::npos || eq == 0) continue;
        std::string k = item.substr(0, eq), v = item.substr(eq + 1);
        while (!k.empty() && k.back() == ' ') k.pop_back();
        while (!v.empty() && v.front() == ' ') v.erase(0, 1);
        out[k] = v;
    }
    return out;
}

// Pins that a new wire runs across without ending on them: they look joined but aren't.
std::string crossingWarning(const Circuit& c, const Wire& w) {
    std::string s;
    for (const auto& part : c.parts) {
        auto sym = symbolOf(part);
        if (!sym) continue;
        for (const auto& pin : sym->pins) {
            const Pt q = pinPos(part, pin);
            for (std::size_t i = 1; i < w.pts.size(); ++i) {
                if (onInterior(q, w.pts[i - 1], w.pts[i])) {
                    s += (s.empty() ? "" : ", ") + part.ref + "." + pin.number;
                    break;
                }
            }
        }
    }
    return s.empty() ? s : "\nwarning: the wire runs over " + s + " and joins them (T-junction); use circuit.net to join by name instead";
}

std::string pinList(const Circuit& c, const Part& p) {
    auto sym = symbolOf(p);
    if (!sym) return {};
    std::string s;
    for (const auto& pin : sym->pins) s += (s.empty() ? "" : " ") + pin.number + (pin.name != pin.number ? "=" + pin.name : "") + "@" + pt(pinPos(p, pin));
    (void)c;
    return s;
}

// Unit vector pointing out of a pin, away from its part's body.
Pt outward(const PinRef& r) {
    const Pt inner = toSheet(*r.part, r.pin->end);
    double dx = r.pos.x - inner.x, dy = r.pos.y - inner.y;
    const double len = std::hypot(dx, dy);
    if (len < 1e-9) return {0, 0};
    return {std::round(dx / len), std::round(dy / len)};
}

bool segHitsBox(Pt a, Pt b, double x0, double y0, double x1, double y1) {
    // axis-aligned segments only (schematic wires); strictly inside the box
    const double sx0 = std::min(a.x, b.x), sx1 = std::max(a.x, b.x), sy0 = std::min(a.y, b.y), sy1 = std::max(a.y, b.y);
    return sx1 > x0 + 1e-6 && sx0 < x1 - 1e-6 && sy1 > y0 + 1e-6 && sy0 < y1 - 1e-6;
}

// A tidy wire between two pins: tries an L either way and routes that first step out of each
// pin, and keeps the one crossing the fewest part bodies, then the shortest. A route that would
// touch other wires, labels or pins (and so join nets) is never chosen: `ok` is false then.
Wire routeWire(const Circuit& c, const PinRef& a, const PinRef& b, bool& ok) {
    const Pt da = outward(a), db = outward(b);
    std::vector<std::vector<Pt>> cands;
    auto L = [](Pt p, Pt q, bool vfirst) {
        std::vector<Pt> v{p};
        if (std::abs(p.x - q.x) > 1e-9 && std::abs(p.y - q.y) > 1e-9) v.push_back(vfirst ? Pt{p.x, q.y} : Pt{q.x, p.y});
        v.push_back(q);
        return v;
    };
    for (bool vf : {false, true}) {
        cands.push_back(L(a.pos, b.pos, vf));
        for (int ka = 1; ka <= 3; ++ka) {
            for (int kb = 0; kb <= 3; ++kb) {
                const Pt a1{a.pos.x + da.x * ka, a.pos.y + da.y * ka}, b1{b.pos.x + db.x * kb, b.pos.y + db.y * kb};
                std::vector<Pt> v{a.pos};
                for (Pt q : L(a1, b1, vf)) v.push_back(q);
                v.push_back(b.pos);
                cands.push_back(v);
                // and the mirror image: step out of b only
                std::vector<Pt> w{a.pos};
                for (Pt q : L(a.pos, b1, vf)) w.push_back(q);
                w.push_back(b.pos);
                cands.push_back(w);
            }
        }
    }
    // everything a new wire must not touch
    std::vector<Pt> touch;
    for (const auto& w : c.wires) {
        for (const auto& q : w.pts) touch.push_back(q);
    }
    for (const auto& l : c.labels) touch.push_back(l.pos);
    for (const auto& n : c.noConnects) touch.push_back(n);
    for (const auto& part : c.parts) {
        if (auto sym = symbolOf(part)) {
            for (const auto& pin : sym->pins) touch.push_back(pinPos(part, pin));
        }
    }
    double best = 1e18;
    std::vector<Pt> pick = cands.front();
    for (auto v : cands) {
        // drop repeated and collinear points
        std::vector<Pt> s;
        for (Pt q : v) {
            if (!s.empty() && samePt(s.back(), q)) continue;
            if (s.size() >= 2) {
                const Pt u = s[s.size() - 2], w = s.back();
                const double cross = (w.x - u.x) * (q.y - w.y) - (w.y - u.y) * (q.x - w.x);
                if (std::abs(cross) < 1e-9) s.pop_back();
            }
            s.push_back(q);
        }
        if (s.size() < 2) continue;
        // a route doubling back on itself is useless
        bool back = false;
        for (std::size_t i = 2; i < s.size(); ++i) {
            const double dot = (s[i - 1].x - s[i - 2].x) * (s[i].x - s[i - 1].x) + (s[i - 1].y - s[i - 2].y) * (s[i].y - s[i - 1].y);
            back = back || dot < 0;
        }
        if (back) continue;
        double cost = 0;
        for (std::size_t i = 1; i < s.size(); ++i) cost += std::abs(s[i].x - s[i - 1].x) + std::abs(s[i].y - s[i - 1].y);
        cost += static_cast<double>(s.size()) * 0.5;
        for (const auto& q : touch) {
            if (samePt(q, a.pos) || samePt(q, b.pos)) continue;
            for (std::size_t i = 0; i < s.size(); ++i) {
                if (samePt(q, s[i]) || (i > 0 && onInterior(q, s[i - 1], s[i]))) cost += 1e6;
            }
        }
        for (std::size_t i = 1; i + 1 < s.size(); ++i) {
            if (!sheetPointFree(c, s[i])) cost += 1e6;
        }
        for (const auto& part : c.parts) {
            auto sym = symbolOf(part);
            if (!sym || sym->power) continue;
            const Pt p0 = toSheet(part, {sym->bodyMinX, sym->bodyMinY}), p1 = toSheet(part, {sym->bodyMaxX, sym->bodyMaxY});
            const double x0 = std::min(p0.x, p1.x), x1 = std::max(p0.x, p1.x), y0 = std::min(p0.y, p1.y), y1 = std::max(p0.y, p1.y);
            for (std::size_t i = 1; i < s.size(); ++i) {
                if (segHitsBox(s[i - 1], s[i], x0, y0, x1, y1)) cost += 100;
            }
        }
        if (cost < best) best = cost, pick = s;
    }
    ok = best < 1e6;
    return Wire{pick};
}

PinRef needPin(const Circuit& c, const std::string& text) {
    std::string error;
    PinRef r = resolvePin(c, text, error);
    if (!r.part) throw std::runtime_error(error);
    return r;
}

}  // namespace

void registerSchematicCommands(Session& session) {
    auto& reg = session.registry();
    reg.add({"circuit.place", "Place Part",
             "Place a library part on the schematic. ref is automatic (R1, R2, U1 ...) unless given; x, y are grid units of 0.1 in "
             "(default: right of what is already there). rot turns it counter-clockwise in steps of 90. Returns the ref and "
             "every pin's number, name and position. Examples: symbol=resistor value=330; symbol=ic pins=8 "
             "pin_names=VCC,TRIG,...; symbol=header pins=4; symbol=arduino_nano; symbol=gnd.",
             {docParam(),
              {"symbol", ParamType::String, "library symbol (circuit.symbols), e.g. resistor, led, npn, opamp, ic, header, esp32_devkit, gnd, +5v", true},
              {"ref", ParamType::String, "reference designator (default: next free, e.g. R3)", false},
              {"value", ParamType::String, "value, e.g. 10k, 100n, red, LM358 (default: the symbol's)", false},
              {"x", ParamType::Number, "x in grid units (0.1 in)", false},
              {"y", ParamType::Number, "y in grid units, down is positive", false},
              {"rot", ParamType::Int, "rotation in degrees counter-clockwise: 0, 90, 180, 270", false, {}, std::int64_t{0}},
              {"mirror", ParamType::Bool, "mirror left-right", false, {}, false},
              {"footprint", ParamType::String, "PCB footprint (circuit.footprints); empty keeps the default", false},
              {"pins", ParamType::Int, "pin count for ic, header (1 row) and header2 (2 rows, total pins)", false},
              {"pin_names", ParamType::String, "ic: pin names in pin order, comma-separated", false},
              {"fields", ParamType::String, "extra BOM fields, e.g. mpn=RC0805FR-0710KL; note=1%", false},
              {"unit", ParamType::Int, "multi-unit parts (opamp_dual, nand_quad, ...): place only this unit (1 = A); with ref= it joins that part", false},
              previewParam()},
             true},
            circuitCommand(
                session,
                [](CircuitDoc& doc, const Params& p) {
                    Circuit& c = doc.circuit;
                    std::string error;
                    if (const int units = symbolUnits(stringParam(p, "symbol")); units > 0) {
                        // one package, several units: every unit side by side (or the one asked for), one ref and one footprint
                        const std::string name = canonicalSymbol(stringParam(p, "symbol"));
                        const int only = static_cast<int>(intParam(p, "unit", 0));
                        if (has(p, "unit") && (only < 1 || only > units)) return Result::failure(name + " has units 1 to " + std::to_string(units));
                        std::string ref = has(p, "ref") ? stringParam(p, "ref") : nextRef(c, "U");
                        const Part* existing = c.part(ref);
                        if (existing && (existing->symbol != name || only == 0)) return Result::failure(ref + " already exists");
                        if (existing) {
                            for (const auto& q : c.parts)
                                if (q.ref == existing->ref && q.unit == only) return Result::failure(ref + " already has unit " + std::string(1, static_cast<char>('A' + only - 1)));
                            ref = existing->ref;
                        }
                        double x0, y0, x1, y1;
                        sheetBounds(c, x0, y0, x1, y1);
                        if (c.parts.empty() && c.wires.empty()) x1 = 0, y0 = 0;
                        Pt at{std::round(has(p, "x") ? numberParam(p, "x") : x1 + 7), std::round(has(p, "y") ? numberParam(p, "y") : y0 + 3)};
                        std::string pins;
                        for (int u = only ? only : 1; u <= (only ? only : units); ++u) {
                            Part part;
                            part.symbol = name;
                            part.unit = u;
                            auto sym = findSymbolUnit(name, u);
                            part.ref = ref;
                            part.value = has(p, "value") ? stringParam(p, "value") : sym->value;
                            // the footprint lives on unit A only (as KiCad does)
                            if (u == 1 || (existing == nullptr && only > 1)) part.footprint = has(p, "footprint") ? stringParam(p, "footprint") : sym->footprint;
                            if (existing) part.value = existing->value;
                            part.rot = snapRot(intParam(p, "rot", 0));
                            part.mirror = boolParam(p, "mirror", false);
                            if (has(p, "fields")) part.fields = parseFields(stringParam(p, "fields"));
                            part.pos = at;
                            at.x += 8;
                            c.parts.push_back(part);
                            pins += "\n  unit " + std::string(1, static_cast<char>('A' + u - 1)) + ": " + pinList(c, c.parts.back());
                        }
                        Result r = edited("placed " + ref + " (" + name + ", " + (only ? "unit " + std::string(1, static_cast<char>('A' + only - 1)) : std::to_string(units) + " units") + ")" + pins, p);
                        r.value = ref;
                        r.with("ref", ref).with("units", static_cast<std::int64_t>(only ? 1 : units));
                        return r;
                    }
                    Part part;
                    part.symbol = stringParam(p, "symbol");
                    part.pins = static_cast<int>(intParam(p, "pins", 0));
                    if (has(p, "pin_names")) part.pinNames = splitList(stringParam(p, "pin_names"));
                    auto sym = findSymbol(part.symbol, part.pins, part.pinNames, &error);
                    if (!sym) {
                        // a symbol imported from KiCad that another part already uses
                        for (const auto& other : c.parts) {
                            if (other.customSymbol && (other.customSymbol->name == part.symbol || other.customSymbol->title == part.symbol)) {
                                sym = part.customSymbol = other.customSymbol;
                                part.symbol = other.symbol;
                                part.unit = other.unit;
                                if (part.footprint.empty() && other.customFootprint) part.customFootprint = other.customFootprint;
                                break;
                            }
                        }
                    }
                    if (!sym) return Result::failure(error);
                    if (sym->name == "ic" && part.pins == 0) part.pins = static_cast<int>(sym->pins.size());
                    if ((sym->name == "header" || sym->name == "header2") && part.pins == 0) part.pins = static_cast<int>(sym->pins.size());
                    part.value = has(p, "value") ? stringParam(p, "value") : sym->value;
                    part.footprint = has(p, "footprint") && !stringParam(p, "footprint").empty() ? stringParam(p, "footprint") : sym->footprint;
                    if (part.customFootprint && !has(p, "footprint")) part.footprint = part.customFootprint->name;
                    else part.customFootprint = importedFootprint(c, part.footprint);
                    if (part.customSymbol && !part.customFootprint && !findFootprint(part.footprint)) part.footprint.clear();
                    if (!part.footprint.empty() && !part.customFootprint && !findFootprint(part.footprint)) return Result::failure("no footprint called '" + part.footprint + "' (circuit.footprints lists them)");
                    if (has(p, "ref")) {
                        part.ref = stringParam(p, "ref");
                        if (c.part(part.ref)) return Result::failure(part.ref + " already exists");
                    } else {
                        part.ref = nextRef(c, sym->prefix);
                    }
                    part.rot = snapRot(intParam(p, "rot", 0));
                    part.mirror = boolParam(p, "mirror", false);
                    if (has(p, "fields")) part.fields = parseFields(stringParam(p, "fields"));
                    if (has(p, "x") && has(p, "y")) {
                        part.pos = {std::round(numberParam(p, "x")), std::round(numberParam(p, "y"))};
                    } else {
                        double x0, y0, x1, y1;
                        sheetBounds(c, x0, y0, x1, y1);
                        if (c.parts.empty() && c.wires.empty()) x1 = 0, y0 = 0;
                        part.pos = {std::round(x1 + 4 - sym->minX), std::round(has(p, "y") ? numberParam(p, "y") : y0 - sym->minY)};
                        if (has(p, "x")) part.pos.x = std::round(numberParam(p, "x"));
                    }
                    c.parts.push_back(part);
                    Result r = edited("placed " + part.ref + " (" + sym->name + (part.value.empty() ? "" : " " + part.value) + ") at " + pt(part.pos) +
                                          "\npins: " + pinList(c, part),
                                      p);
                    r.value = part.ref;
                    r.with("ref", part.ref).with("x", part.pos.x).with("y", part.pos.y).with("footprint", part.footprint);
                    return r;
                },
                View::Schematic));

    reg.add({"circuit.edit", "Edit Part", "Change a part's value, footprint, reference, pin count/names or BOM fields (fields: k=v; k= removes k).",
             {docParam(),
              {"ref", ParamType::String, "the part, e.g. R1", true},
              {"value", ParamType::String, "new value", false},
              {"footprint", ParamType::String, "new footprint", false},
              {"new_ref", ParamType::String, "rename the part", false},
              {"pins", ParamType::Int, "new pin count (ic, header)", false},
              {"pin_names", ParamType::String, "ic: new pin names", false},
              {"fields", ParamType::String, "fields to set, e.g. mpn=NE555P; dnp=yes (dnp leaves it off the BOM)", false},
              previewParam()},
             true},
            circuitCommand(
                session,
                [](CircuitDoc& doc, const Params& p) {
                    Circuit& c = doc.circuit;
                    Part* part = c.part(stringParam(p, "ref"));
                    if (!part) return Result::failure("no part " + stringParam(p, "ref"));
                    std::vector<std::string> changed;
                    if (has(p, "value")) part->value = stringParam(p, "value"), changed.push_back("value");
                    if (has(p, "footprint")) {
                        const std::string fp = stringParam(p, "footprint");
                        auto imported = importedFootprint(c, fp);
                        if (!fp.empty() && !imported && !findFootprint(fp)) return Result::failure("no footprint called '" + fp + "'");
                        part->footprint = fp;
                        part->customFootprint = imported;
                        changed.push_back("footprint");
                    }
                    if (has(p, "pins")) part->pins = static_cast<int>(intParam(p, "pins")), changed.push_back("pins");
                    if (has(p, "pin_names")) part->pinNames = splitList(stringParam(p, "pin_names")), changed.push_back("pin names");
                    if (has(p, "fields")) {
                        for (const auto& [k, v] : parseFields(stringParam(p, "fields"))) {
                            if (v.empty()) part->fields.erase(k);
                            else part->fields[k] = v;
                        }
                        changed.push_back("fields");
                    }
                    if (has(p, "new_ref")) {
                        const std::string nr = stringParam(p, "new_ref");
                        if (const Part* other = c.part(nr); other && other != part) return Result::failure(nr + " already exists");
                        part->ref = nr;
                        changed.push_back("ref");
                    }
                    if (!symbolOf(*part)) return Result::failure("that makes the symbol invalid");
                    if (changed.empty()) return Result::failure("nothing to change: give value, footprint, new_ref, pins, pin_names or fields");
                    std::string list;
                    for (const auto& s : changed) list += (list.empty() ? "" : ", ") + s;
                    return edited("changed " + part->ref + ": " + list, p).with("ref", part->ref);
                },
                View::Schematic));

    reg.add({"circuit.move", "Move Part",
             "Move, rotate or mirror a part on the schematic; wire ends on its pins follow (with a bend added where needed).",
             {docParam(),
              {"ref", ParamType::String, "the part", true},
              {"x", ParamType::Number, "new x (grid units)", false},
              {"y", ParamType::Number, "new y", false},
              {"dx", ParamType::Number, "move by this much instead", false},
              {"dy", ParamType::Number, "move by this much instead", false},
              {"rot", ParamType::Int, "new rotation (0, 90, 180, 270)", false},
              {"mirror", ParamType::Bool, "mirror left-right", false},
              previewParam()},
             true},
            circuitCommand(
                session,
                [](CircuitDoc& doc, const Params& p) {
                    Circuit& c = doc.circuit;
                    Part* part = c.part(stringParam(p, "ref"));
                    if (!part) return Result::failure("no part " + stringParam(p, "ref"));
                    Pt pos = part->pos;
                    if (has(p, "x")) pos.x = std::round(numberParam(p, "x"));
                    if (has(p, "y")) pos.y = std::round(numberParam(p, "y"));
                    pos.x += std::round(numberParam(p, "dx", 0));
                    pos.y += std::round(numberParam(p, "dy", 0));
                    const int rot = has(p, "rot") ? snapRot(intParam(p, "rot")) : part->rot;
                    const bool mirror = has(p, "mirror") ? boolParam(p, "mirror") : part->mirror;
                    movePart(c, *part, pos, rot, mirror);
                    return edited("moved " + part->ref + " to " + pt(pos) + " rot " + std::to_string(rot) + "\npins: " + pinList(c, *part), p);
                },
                View::Schematic));

    reg.add({"circuit.remove", "Remove Part", "Delete a part, with the wires and labels that only served its pins (and its board footprint).",
             {docParam(), {"ref", ParamType::String, "the part", true}, previewParam()}, true},
            circuitCommand(
                session,
                [](CircuitDoc& doc, const Params& p) {
                    const std::string ref = stringParam(p, "ref");
                    if (!doc.circuit.part(ref)) return Result::failure("no part " + ref);
                    removePart(doc.circuit, ref);
                    return edited("removed " + ref, p);
                },
                View::Schematic));

    reg.add({"circuit.connect", "Connect Pins",
             "Draw wires between pins: from=R1.2 to=D1.2, or pins=R1.2,D1.2,U1.3 to chain several. Wires are routed with right-angle "
             "bends around part bodies (vertical_first=true/false forces a single bend). Pins are REF.NUMBER or REF.NAME (U1.VCC, A1.D13). For pins far apart, "
             "circuit.net (labels) keeps the drawing tidy.",
             {docParam(),
              {"from", ParamType::String, "first pin, e.g. R1.2", false},
              {"to", ParamType::String, "second pin", false},
              {"pins", ParamType::String, "comma-separated chain of pins instead of from/to", false},
              {"vertical_first", ParamType::Bool, "force one bend: vertical first (true) or horizontal first (false)", false},
              previewParam()},
             true},
            circuitCommand(
                session,
                [](CircuitDoc& doc, const Params& p) {
                    Circuit& c = doc.circuit;
                    std::vector<std::string> chain;
                    if (has(p, "pins")) chain = splitList(stringParam(p, "pins"));
                    else if (has(p, "from") && has(p, "to")) chain = {stringParam(p, "from"), stringParam(p, "to")};
                    if (chain.size() < 2) return Result::failure("give from= and to=, or pins= with at least two pins");
                    std::string warn;
                    for (std::size_t i = 1; i < chain.size(); ++i) {
                        const PinRef a = needPin(c, chain[i - 1]), b = needPin(c, chain[i]);
                        if (has(p, "vertical_first")) addWire(c, a.pos, b.pos, boolParam(p, "vertical_first", false));
                        else {
                            bool ok = false;
                            Wire w = routeWire(c, a, b, ok);
                            if (ok) {
                                c.wires.push_back(std::move(w));
                            } else {
                                // no clean path: join them by name instead
                                const Netlist cur = buildNetlist(c);
                                std::string name = cur.netOf(a.part->ref, a.pin->number);
                                const Net* n = cur.net(name);
                                if (!n || !n->named) name = cur.netOf(b.part->ref, b.pin->number), n = cur.net(name);
                                if (!n || !n->named) name = "N_" + a.part->ref + "_" + a.pin->number;
                                labelPin(c, *a.part, *a.pin, name);
                                labelPin(c, *b.part, *b.pin, name);
                                warn += "\nnote: no clear wire path from " + chain[i - 1] + " to " + chain[i] + "; joined them with net labels " + name;
                            }
                        }
                        warn += crossingWarning(c, c.wires.back());
                    }
                    const Netlist nl = buildNetlist(c);
                    const PinRef a = needPin(c, chain[0]);
                    const std::string net = nl.netOf(a.part->ref, a.pin->number);
                    return edited("connected " + std::to_string(chain.size()) + " pins on net " + net + warn, p).with("net", net);
                },
                View::Schematic));

    reg.add({"circuit.wire", "Draw Wire", "Draw a wire through points given in grid units: points=x1,y1,x2,y2,... (ends on pins or other wires connect).",
             {docParam(), {"points", ParamType::NumberList, "x1,y1,x2,y2,... in grid units", true}, previewParam()}, true},
            circuitCommand(
                session,
                [](CircuitDoc& doc, const Params& p) {
                    const NumberList v = listParam(p, "points");
                    if (v.size() < 4 || v.size() % 2) return Result::failure("points needs pairs: x1,y1,x2,y2,...");
                    Wire w;
                    for (std::size_t i = 0; i + 1 < v.size(); i += 2) w.pts.push_back({std::round(v[i] * 10) / 10, std::round(v[i + 1] * 10) / 10});
                    doc.circuit.wires.push_back(w);
                    return edited("wire with " + std::to_string(w.pts.size()) + " points" + crossingWarning(doc.circuit, w), p);
                },
                View::Schematic));

    reg.add({"circuit.net", "Join Pins by Name",
             "Join pins to a named net with net labels: each pin gets a short stub and a label, and every label with the same name "
             "is one net. The easiest way to wire a schematic: circuit.net name=SDA pins=U1.5,R3.1,J1.3.",
             {docParam(),
              {"name", ParamType::String, "net name, e.g. SDA, LED_A, VBAT", true},
              {"pins", ParamType::String, "comma-separated pins, e.g. R1.2,U1.VCC", true},
              previewParam()},
             true},
            circuitCommand(
                session,
                [](CircuitDoc& doc, const Params& p) {
                    Circuit& c = doc.circuit;
                    const std::string name = stringParam(p, "name");
                    if (name.empty() || name.find_first_of(" \t\n") != std::string::npos) return Result::failure("net names can't be empty or contain spaces");
                    const auto pins = splitList(stringParam(p, "pins"));
                    if (pins.empty()) return Result::failure("pins is empty");
                    for (const auto& s : pins) {
                        const PinRef r = needPin(c, s);
                        labelPin(c, *r.part, *r.pin, name);
                    }
                    const Netlist nl = buildNetlist(c);
                    const Net* n = nl.net(name);
                    return edited("net " + name + " now has " + std::to_string(n ? n->nodes.size() : 0) + " pins", p)
                        .with("net", name)
                        .with("pins", static_cast<std::int64_t>(n ? n->nodes.size() : 0));
                },
                View::Schematic));

    reg.add({"circuit.label", "Net Label", "Put a net label at a point (x, y in grid units) or on a pin; labels with the same name are joined.",
             {docParam(),
              {"name", ParamType::String, "net name", true},
              {"x", ParamType::Number, "x", false},
              {"y", ParamType::Number, "y", false},
              {"pin", ParamType::String, "or: a pin to label (adds a stub)", false},
              previewParam()},
             true},
            circuitCommand(
                session,
                [](CircuitDoc& doc, const Params& p) {
                    Circuit& c = doc.circuit;
                    const std::string name = stringParam(p, "name");
                    if (name.empty()) return Result::failure("name is empty");
                    if (has(p, "pin")) {
                        const PinRef r = needPin(c, stringParam(p, "pin"));
                        labelPin(c, *r.part, *r.pin, name);
                    } else {
                        if (!has(p, "x") || !has(p, "y")) return Result::failure("give x and y, or pin");
                        c.labels.push_back({name, {numberParam(p, "x"), numberParam(p, "y")}});
                    }
                    return edited("label " + name, p);
                },
                View::Schematic));
}

// power symbols, no-connect marks, wire removal and annotation
void registerSchematicMarkCommands(Session& session) {
    auto& reg = session.registry();
    reg.add({"circuit.power", "Power Symbol",
             "Attach a power or ground symbol to pins: symbol=gnd pins=R2.2,C1.2 puts a ground symbol under each pin and wires it. "
             "Power symbols with the same value are one global net (GND, VCC, +5V, +3V3, +12V, or any value=...).",
             {docParam(),
              {"symbol", ParamType::String, "gnd, vcc, +5v, +3v3, +12v, or power with value=", false, {}, std::string("gnd")},
              {"pins", ParamType::String, "comma-separated pins to attach it to", false},
              {"value", ParamType::String, "net name for symbol=power, e.g. +9V or VBAT", false},
              {"x", ParamType::Number, "or: place it at x", false},
              {"y", ParamType::Number, "and y", false},
              previewParam()},
             true},
            circuitCommand(
                session,
                [](CircuitDoc& doc, const Params& p) {
                    Circuit& c = doc.circuit;
                    const std::string symName = stringParam(p, "symbol", "gnd");
                    std::string error;
                    auto sym = findSymbol(symName, 0, {}, &error);
                    if (!sym) return Result::failure(error);
                    if (!sym->power) return Result::failure(symName + " isn't a power symbol (use gnd, vcc, +5v, +3v3, +12v or power)");
                    const std::string value = has(p, "value") ? stringParam(p, "value") : sym->value;
                    const bool ground = sym->name == "gnd";
                    auto make = [&](Pt at) {
                        Part part;
                        part.symbol = symName;
                        part.value = value;
                        part.ref = nextRef(c, "#PWR");
                        part.pos = at;
                        c.parts.push_back(part);
                    };
                    int n = 0;
                    if (has(p, "pins")) {
                        for (const auto& s : splitList(stringParam(p, "pins"))) {
                            const PinRef r = needPin(c, s);
                            const Pt d = outward(r);
                            // where the symbol goes: straight out of a vertical pin (turned to face away),
                            // or out of a horizontal pin and then one step down (ground) or up (power)
                            std::vector<Pt> path;
                            int rot = 0;
                            bool found = false;
                            // directions to leave the pin: straight out, then sideways
                            const Pt dirs[3] = {d, {-d.y, d.x}, {d.y, -d.x}};
                            for (int k = 1; k <= 8 && !found; ++k) {
                                for (const Pt dir : dirs) {
                                    if (dir.x == 0 && dir.y == 0 && k > 1) continue;
                                    path.clear();
                                    if (dir.x == 0 && dir.y != 0) {
                                        for (int j = 1; j <= k; ++j) path.push_back({r.pos.x, r.pos.y + dir.y * j});
                                        rot = (ground ? dir.y > 0 : dir.y < 0) ? 0 : 180;
                                    } else {
                                        for (int j = 1; j <= k && dir.x != 0; ++j) path.push_back({r.pos.x + dir.x * j, r.pos.y});
                                        const Pt last = path.empty() ? r.pos : path.back();
                                        path.push_back({last.x, last.y + (ground ? 1 : -1)});
                                        rot = 0;
                                    }
                                    bool free = true;
                                    for (Pt q : path) free = free && sheetPointFree(c, q);
                                    if (free) {
                                        found = true;
                                        break;
                                    }
                                }
                            }
                            if (!found) path = {r.pos};  // crowded: the symbol sits on the pin
                            const Pt at = path.back();
                            make(at);
                            c.parts.back().rot = rot;
                            Wire w{{r.pos}};
                            for (Pt q : path) {
                                if (!samePt(q, r.pos)) w.pts.push_back(q);
                            }
                            if (w.pts.size() < 2) {
                                ++n;
                                continue;
                            }
                            // keep only the corners
                            std::vector<Pt> simple{w.pts.front()};
                            for (std::size_t i = 1; i + 1 < w.pts.size(); ++i) {
                                const Pt u = simple.back(), v = w.pts[i], x = w.pts[i + 1];
                                if (std::abs((v.x - u.x) * (x.y - v.y) - (v.y - u.y) * (x.x - v.x)) > 1e-9) simple.push_back(v);
                            }
                            simple.push_back(w.pts.back());
                            c.wires.push_back({simple});
                            ++n;
                        }
                    } else if (has(p, "x") && has(p, "y")) {
                        make({std::round(numberParam(p, "x")), std::round(numberParam(p, "y"))});
                        ++n;
                    } else {
                        return Result::failure("give pins=... or x and y");
                    }
                    return edited("added " + std::to_string(n) + " " + value + " symbol" + (n == 1 ? "" : "s"), p).with("net", value);
                },
                View::Schematic));

    reg.add({"circuit.no_connect", "No Connect", "Mark pins as deliberately unconnected so ERC doesn't flag them.",
             {docParam(), {"pins", ParamType::String, "comma-separated pins", true}, previewParam()}, true},
            circuitCommand(
                session,
                [](CircuitDoc& doc, const Params& p) {
                    int n = 0;
                    for (const auto& s : splitList(stringParam(p, "pins"))) {
                        doc.circuit.noConnects.push_back(needPin(doc.circuit, s).pos);
                        ++n;
                    }
                    return edited("marked " + std::to_string(n) + " pins not connected", p);
                },
                View::Schematic));

    reg.add({"circuit.unwire", "Remove Wires",
             "Remove wires and labels: those touching a pin (pin=R1.2), a point (x, y), every label of a net (net=SDA), or all of them (all=true).",
             {docParam(),
              {"pin", ParamType::String, "a pin", false},
              {"x", ParamType::Number, "a point", false},
              {"y", ParamType::Number, "a point", false},
              {"net", ParamType::String, "remove this net's labels and their stubs", false},
              {"all", ParamType::Bool, "remove every wire, label and no-connect mark", false, {}, false},
              previewParam()},
             true},
            circuitCommand(
                session,
                [](CircuitDoc& doc, const Params& p) {
                    Circuit& c = doc.circuit;
                    const std::size_t w0 = c.wires.size(), l0 = c.labels.size();
                    if (boolParam(p, "all", false)) {
                        c.wires.clear();
                        c.labels.clear();
                        c.noConnects.clear();
                    } else if (has(p, "net")) {
                        const std::string name = stringParam(p, "net");
                        std::vector<Pt> at;
                        for (const auto& l : c.labels) {
                            if (l.name == name) at.push_back(l.pos);
                        }
                        std::erase_if(c.labels, [&](const Label& l) { return l.name == name; });
                        std::erase_if(c.wires, [&](const Wire& w) {
                            return w.pts.size() == 2 && std::any_of(at.begin(), at.end(), [&](Pt q) { return samePt(q, w.pts.front()) || samePt(q, w.pts.back()); });
                        });
                    } else {
                        Pt q;
                        if (has(p, "pin")) q = needPin(c, stringParam(p, "pin")).pos;
                        else if (has(p, "x") && has(p, "y")) q = {numberParam(p, "x"), numberParam(p, "y")};
                        else return Result::failure("give pin, x and y, net, or all=true");
                        std::erase_if(c.wires, [&](const Wire& w) {
                            for (std::size_t i = 0; i < w.pts.size(); ++i) {
                                if (samePt(w.pts[i], q)) return true;
                                if (i > 0 && onInterior(q, w.pts[i - 1], w.pts[i])) return true;
                            }
                            return false;
                        });
                        std::erase_if(c.labels, [&](const Label& l) { return samePt(l.pos, q); });
                        std::erase_if(c.noConnects, [&](Pt n) { return samePt(n, q); });
                    }
                    return edited("removed " + std::to_string(w0 - c.wires.size()) + " wires and " + std::to_string(l0 - c.labels.size()) + " labels", p);
                },
                View::Schematic));

    reg.add({"circuit.annotate", "Annotate", "Give every part a unique reference: fixes R? and duplicates; renumber=true renumbers all, top-left to bottom-right.",
             {docParam(), {"renumber", ParamType::Bool, "renumber every part by position", false, {}, false}, previewParam()}, true},
            circuitCommand(
                session,
                [](CircuitDoc& doc, const Params& p) {
                    Circuit& c = doc.circuit;
                    std::vector<Part*> order;
                    for (auto& part : c.parts) order.push_back(&part);
                    std::stable_sort(order.begin(), order.end(), [](const Part* a, const Part* b) {
                        return a->pos.y != b->pos.y ? a->pos.y < b->pos.y : a->pos.x < b->pos.x;
                    });
                    const bool all = boolParam(p, "renumber", false);
                    std::set<std::string> used;
                    int changed = 0;
                    auto prefixOf = [](const Part& part) {
                        auto sym = symbolOf(part);
                        if (sym) return sym->prefix;
                        std::string s;
                        for (char ch : part.ref) {
                            if (std::isdigit(static_cast<unsigned char>(ch)) || ch == '?') break;
                            s += ch;
                        }
                        return s.empty() ? std::string("X") : s;
                    };
                    if (!all) {
                        for (Part* part : order) {
                            const bool bad = part->ref.empty() || part->ref.back() == '?' || used.count(part->ref);
                            if (!bad) used.insert(part->ref);
                        }
                    }
                    std::map<std::string, int> counter;
                    std::set<std::string> seen;
                    for (Part* part : order) {
                        const bool bad = all || part->ref.empty() || part->ref.back() == '?' || seen.count(part->ref);
                        if (!bad) {
                            seen.insert(part->ref);
                            continue;
                        }
                        const std::string prefix = prefixOf(*part);
                        std::string ref;
                        do {
                            ref = prefix + std::to_string(++counter[prefix]);
                        } while (used.count(ref) || seen.count(ref));
                        if (ref != part->ref) ++changed;
                        part->ref = ref;
                        seen.insert(ref);
                    }
                    return edited("annotated: " + std::to_string(changed) + " references changed", p).with("changed", static_cast<std::int64_t>(changed));
                },
                View::Schematic));
}

void registerSchematicQueryCommands(Session& session) {
    auto& reg = session.registry();
    reg.add({"circuit.parts", "List Parts", "Every part: reference, symbol, value, footprint, schematic position and board placement.",
             {docParam()}, false, true},
            circuitCommand(
                session,
                [](CircuitDoc& doc, const Params& p) {
                    std::string s;
                    std::int64_t n = 0;
                    for (const auto& part : doc.circuit.parts) {
                        s += "\n" + part.ref + ": " + part.symbol + " " + part.value + (part.footprint.empty() ? "" : " [" + part.footprint + "]") + " at " +
                             pt(part.pos) + (part.rot ? " rot " + std::to_string(part.rot) : "");
                        if (part.pcb.placed) s += "; board " + pt(part.pcb.pos) + " mm" + (part.pcb.rot ? " rot " + std::to_string(part.pcb.rot) : "") + (part.pcb.bottom ? " bottom" : "");
                        for (const auto& [k, v] : part.fields) s += "; " + k + "=" + v;
                        ++n;
                    }
                    return edited(std::to_string(n) + " parts:" + s, p).with("count", n);
                },
                View::Schematic, false));

    reg.add({"circuit.pins", "List Pins", "A part's pins: number, name, type, schematic position and the net each is on.",
             {docParam(), {"ref", ParamType::String, "the part", true}}, false, true},
            circuitCommand(
                session,
                [](CircuitDoc& doc, const Params& p) {
                    const Part* part = doc.circuit.part(stringParam(p, "ref"));
                    if (!part) return Result::failure("no part " + stringParam(p, "ref"));
                    auto sym = symbolOf(*part);
                    if (!sym) return Result::failure("unknown symbol");
                    const Netlist nl = buildNetlist(doc.circuit);
                    std::string s = part->ref + " (" + sym->name + ", " + std::to_string(sym->pins.size()) + " pins):";
                    for (const auto& pin : sym->pins) {
                        const std::string net = nl.netOf(part->ref, pin.number);
                        s += "\n  " + pin.number + (pin.name != pin.number ? " " + pin.name : "") + " [" + toString(pin.type) + "] at " + pt(pinPos(*part, pin)) +
                             " -> " + (net.empty() ? "unconnected" : net);
                    }
                    return edited(s, p);
                },
                View::Schematic, false));

    reg.add({"circuit.nets", "List Nets", "The netlist: every net with the pins on it (or one net with net=).",
             {docParam(), {"net", ParamType::String, "only this net", false}}, false, true},
            circuitCommand(
                session,
                [](CircuitDoc& doc, const Params& p) {
                    const Netlist nl = buildNetlist(doc.circuit);
                    std::string s;
                    std::int64_t n = 0;
                    for (const auto& net : nl.nets) {
                        if (has(p, "net") && lower(net.name) != lower(stringParam(p, "net"))) continue;
                        s += "\n" + net.name + ":";
                        for (const auto& node : net.nodes) s += " " + node.ref + "." + node.pin;
                        ++n;
                    }
                    if (has(p, "net") && n == 0) return Result::failure("no net called " + stringParam(p, "net"));
                    if (!nl.isolatedPins.empty()) s += "\nunconnected pins: " + std::to_string(nl.isolatedPins.size());
                    return edited(std::to_string(n) + " nets:" + s, p).with("nets", n);
                },
                View::Schematic, false));

    reg.add({"circuit.erc", "Electrical Rules Check",
             "Check the schematic: unconnected pins, shorted power nets (two power names on one net), duplicate or missing "
             "references, labels reaching only one pin, dangling wires, outputs fighting, power pins with no source, footprint pad "
             "mismatches. Returns errors and warnings.",
             {docParam(), previewParam()}, false, true},
            circuitCommand(
                session,
                [](CircuitDoc& doc, const Params& p) {
                    const Netlist nl = buildNetlist(doc.circuit);
                    int errors = 0, warnings = 0;
                    const std::string text = issuesText(erc(doc.circuit, nl), errors, warnings);
                    Result r = edited("ERC: " + std::to_string(errors) + " errors, " + std::to_string(warnings) + " warnings" + text, p);
                    r.with("errors", static_cast<std::int64_t>(errors)).with("warnings", static_cast<std::int64_t>(warnings));
                    return r;
                },
                View::Schematic, false));

    reg.add({"circuit.bom", "Bill of Materials",
             "The bill of materials as CSV (Item, Qty, References, Value, Footprint, Description, plus any fields such as mpn). "
             "Grouped by value and footprint unless grouped=false. With path= it is written to a file.",
             {docParam(), {"path", ParamType::String, "write the CSV here", false}, {"grouped", ParamType::Bool, "one row per value+footprint", false, {}, true}},
             false},
            circuitCommand(
                session,
                [](CircuitDoc& doc, const Params& p) {
                    const std::string csv = bomCsv(doc.circuit, boolParam(p, "grouped", true));
                    const auto rows = static_cast<std::int64_t>(std::count(csv.begin(), csv.end(), '\n') - 1);
                    if (has(p, "path")) {
                        std::string error;
                        if (!writeFile(stringParam(p, "path"), csv, error)) return Result::failure(error);
                        return edited("wrote " + stringParam(p, "path") + " (" + std::to_string(rows) + " rows)", p).with("rows", rows);
                    }
                    return edited(csv, p).with("rows", rows);
                },
                View::Schematic, false));

    reg.add({"circuit.export_netlist", "Export Netlist",
             "Write the netlist: .net = KiCad s-expression netlist (opens in KiCad's PCB editor and in circuit.open), .json, or .txt (one net per line).",
             {docParam(), {"path", ParamType::String, "output file", true},
              {"format", ParamType::String, "override the extension", false, {"auto", "kicad", "json", "text"}, std::string("auto")}},
             false},
            circuitCommand(
                session,
                [](CircuitDoc& doc, const Params& p) {
                    const fs::path path = stringParam(p, "path");
                    std::string format = stringParam(p, "format", "auto");
                    if (format == "auto") {
                        const std::string ext = lower(path.extension().string());
                        format = ext == ".net" ? "kicad" : ext == ".json" ? "json" : "text";
                    }
                    const Netlist nl = buildNetlist(doc.circuit);
                    const std::string data = format == "kicad" ? kicadNetlist(doc.circuit, nl) : format == "json" ? netlistJson(doc.circuit, nl) : netlistText(doc.circuit, nl);
                    std::string error;
                    if (!writeFile(path, data, error)) return Result::failure(error);
                    return edited("wrote " + path.string() + " (" + format + ", " + std::to_string(nl.nets.size()) + " nets)", p).with("nets", static_cast<std::int64_t>(nl.nets.size()));
                },
                View::Schematic, false));
}

}  // namespace wetwaresuite::circuit::cmd
