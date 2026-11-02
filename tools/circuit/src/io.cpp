// .circuit.json files, KiCad netlist import, the document class and snapshot undo.
#include <map>
#include <cstdio>
#include <algorithm>
#include <cctype>
#include <cmath>
#include <set>
#include <sstream>
#include <stdexcept>

#include <nlohmann/json.hpp>

#include "wetwaresuite/circuit/circuit.hpp"
#include "wetwaresuite/raster/codec.hpp"
#include "sexpr.hpp"

namespace wetwaresuite::circuit {

using json = nlohmann::json;

namespace {

json pt(Pt p) { return json::array({p.x, p.y}); }

Pt readPt(const json& j) {
    if (!j.is_array() || j.size() < 2) throw std::runtime_error("a point should be [x, y]");
    return {j[0].get<double>(), j[1].get<double>()};
}

std::string lower(std::string s) {
    for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

}  // namespace

std::string mapKicadPart(const std::string& partIn, int& pins) {
    const std::string p = lower(partIn);
    auto starts = [&](const char* s) { return p.rfind(s, 0) == 0; };
    auto count = [&](const char* prefix) {
        std::string digits;
        for (std::size_t i = std::string(prefix).size(); i < p.size() && std::isdigit(static_cast<unsigned char>(p[i])); ++i) digits += p[i];
        return digits.empty() ? 0 : std::stoi(digits);
    };
    if (p == "r" || p == "r_small" || p == "r_us" || p == "r_small_us") return "resistor";
    if (starts("r_pot") || starts("potentiometer")) return "potentiometer";
    if (p == "c" || p == "c_small") return "capacitor";
    if (starts("c_polarized") || starts("cp") || p == "c_pol") return "capacitor_polarized";
    if (p == "l" || p == "l_small" || starts("l_core")) return "inductor";
    if (starts("led")) return "led";
    if (p == "d" || starts("d_") || starts("1n4")) return "diode";
    if (starts("q_npn")) return "npn";
    if (starts("q_pnp")) return "pnp";
    if (starts("q_nmos")) return "nmos";
    if (starts("q_pmos")) return "pmos";
    if (starts("crystal")) return "crystal";
    if (starts("sw_push")) return "switch";
    if (starts("sw_spdt")) return "switch_spdt";
    if (starts("conn_01x")) {
        pins = count("conn_01x");
        return "header";
    }
    if (starts("conn_02x")) {
        pins = 2 * count("conn_02x");
        return "header2";
    }
    if (starts("battery")) return "battery";
    if (starts("arduino_uno")) return "arduino_uno";
    if (starts("arduino_nano")) return "arduino_nano";
    return canonicalSymbol(p);
}

namespace {

// ---- custom symbols and footprints (from KiCad files) ----------------------------------

json symbolJson(const Symbol& s) {
    json j;
    j["name"] = s.name;
    j["title"] = s.title;
    j["prefix"] = s.prefix;
    j["value"] = s.value;
    j["footprint"] = s.footprint;
    if (s.power) j["power"] = true;
    if (s.joinSameName) j["join_same_name"] = true;
    if (!s.inBom) j["in_bom"] = false;
    j["pin_numbers"] = s.showPinNumbers;
    j["pin_names"] = s.showPinNames;
    j["ref_at"] = pt(s.refPos);
    j["value_at"] = pt(s.valuePos);
    json pins = json::array();
    for (const auto& p : s.pins) pins.push_back({{"number", p.number}, {"name", p.name}, {"at", pt(p.pos)}, {"end", pt(p.end)}, {"type", toString(p.type)}});
    j["pins"] = std::move(pins);
    json shapes = json::array();
    static const char* kinds[] = {"poly", "circle", "rect", "text"};
    for (const auto& sh : s.shapes) {
        json q;
        q["kind"] = kinds[sh.kind];
        json pts = json::array();
        for (const auto& p : sh.pts) pts.push_back(pt(p));
        q["pts"] = std::move(pts);
        if (sh.r) q["r"] = sh.r;
        if (sh.closed) q["closed"] = true;
        if (sh.fill) q["fill"] = true;
        if (!sh.text.empty()) {
            q["text"] = sh.text;
            q["size"] = sh.size;
        }
        shapes.push_back(std::move(q));
    }
    j["shapes"] = std::move(shapes);
    return j;
}

PinType readPinType(const std::string& t) {
    for (PinType k : {PinType::Passive, PinType::Input, PinType::Output, PinType::Bidirectional, PinType::PowerIn, PinType::PowerOut,
                      PinType::OpenCollector, PinType::NotConnected}) {
        if (t == toString(k)) return k;
    }
    return PinType::Passive;
}

std::shared_ptr<const Symbol> readSymbol(const json& j) {
    auto s = std::make_shared<Symbol>();
    s->name = j.value("name", "");
    s->title = j.value("title", "");
    s->category = "imported";
    s->prefix = j.value("prefix", "U");
    s->value = j.value("value", "");
    s->footprint = j.value("footprint", "");
    s->power = j.value("power", false);
    s->joinSameName = j.value("join_same_name", false);
    s->inBom = j.value("in_bom", true);
    s->showPinNumbers = j.value("pin_numbers", false);
    s->showPinNames = j.value("pin_names", false);
    if (j.contains("ref_at")) s->refPos = readPt(j["ref_at"]);
    if (j.contains("value_at")) s->valuePos = readPt(j["value_at"]);
    for (const auto& p : j.value("pins", json::array())) {
        s->pins.push_back({p.value("number", ""), p.value("name", ""), readPt(p["at"]), readPt(p["end"]), readPinType(p.value("type", "passive"))});
    }
    for (const auto& q : j.value("shapes", json::array())) {
        Shape sh;
        const std::string k = q.value("kind", "poly");
        sh.kind = k == "circle" ? Shape::Circle : k == "rect" ? Shape::Rect : k == "text" ? Shape::Text : Shape::Poly;
        for (const auto& p : q.value("pts", json::array())) sh.pts.push_back(readPt(p));
        sh.r = q.value("r", 0.0);
        sh.closed = q.value("closed", false);
        sh.fill = q.value("fill", false);
        sh.text = q.value("text", "");
        sh.size = q.value("size", 0.7);
        s->shapes.push_back(std::move(sh));
    }
    finishSymbol(*s);
    return s;
}

json footprintJson(const Footprint& f) {
    json j;
    j["name"] = f.name;
    j["description"] = f.description;
    json pads = json::array();
    static const char* shapes[] = {"rect", "circle", "oval"};
    for (const auto& p : f.pads) {
        json q{{"number", p.number}, {"at", pt(p.pos)}, {"size", json::array({p.w, p.h})}, {"shape", shapes[static_cast<int>(p.shape)]}};
        if (p.drill > 0) q["drill"] = p.drill;
        if (!p.plated) q["plated"] = false;
        pads.push_back(std::move(q));
    }
    j["pads"] = std::move(pads);
    json silk = json::array();
    for (const auto& [a, b] : f.silk) silk.push_back(json::array({a.x, a.y, b.x, b.y}));
    j["silk"] = std::move(silk);
    j["courtyard"] = json::array({f.x0, f.y0, f.x1, f.y1});
    return j;
}

std::shared_ptr<const Footprint> readFootprint(const json& j) {
    auto f = std::make_shared<Footprint>();
    f->name = j.value("name", "");
    f->description = j.value("description", "");
    for (const auto& q : j.value("pads", json::array())) {
        PadDef p;
        p.number = q.value("number", "");
        p.pos = readPt(q["at"]);
        const Pt size = readPt(q["size"]);
        p.w = size.x, p.h = size.y;
        const std::string sh = q.value("shape", "rect");
        p.shape = sh == "circle" ? PadShape::Circle : sh == "oval" ? PadShape::Oval : PadShape::Rect;
        p.drill = q.value("drill", 0.0);
        p.plated = q.value("plated", true);
        f->pads.push_back(p);
    }
    for (const auto& l : j.value("silk", json::array())) {
        if (l.is_array() && l.size() == 4) f->silk.push_back({{l[0].get<double>(), l[1].get<double>()}, {l[2].get<double>(), l[3].get<double>()}});
    }
    finishFootprint(*f);
    if (j.contains("courtyard") && j["courtyard"].size() == 4) {
        const auto& c = j["courtyard"];
        f->x0 = c[0].get<double>(), f->y0 = c[1].get<double>(), f->x1 = c[2].get<double>(), f->y1 = c[3].get<double>();
    }
    return f;
}

}  // namespace

std::string toJson(const Circuit& c) {
    json j;
    j["format"] = "wetwaresuite-circuit";
    j["version"] = 1;
    j["title"] = c.title;
    json parts = json::array();
    // custom symbols and footprints are stored once each, keyed by name (made unique)
    std::map<const void*, std::string> customKeys;
    json symbols = json::object(), footprints = json::object();
    auto keyFor = [&](const void* ptr, const std::string& name, json& table, const json& body) {
        if (auto it = customKeys.find(ptr); it != customKeys.end()) return it->second;
        std::string key = name.empty() ? "custom" : name;
        for (int n = 2; table.contains(key); ++n) key = (name.empty() ? "custom" : name) + "~" + std::to_string(n);
        table[key] = body;
        customKeys[ptr] = key;
        return key;
    };
    for (const auto& p : c.parts) {
        json q;
        if (p.customSymbol) q["custom_symbol"] = keyFor(p.customSymbol.get(), p.customSymbol->name, symbols, symbolJson(*p.customSymbol));
        if (p.customFootprint)
            q["custom_footprint"] = keyFor(p.customFootprint.get(), p.customFootprint->name, footprints, footprintJson(*p.customFootprint));
        if (p.unit) q["unit"] = p.unit;
        q["ref"] = p.ref;
        q["symbol"] = p.symbol;
        q["value"] = p.value;
        q["footprint"] = p.footprint;
        if (p.pins) q["pins"] = p.pins;
        if (!p.pinNames.empty()) q["pin_names"] = p.pinNames;
        q["at"] = pt(p.pos);
        q["rot"] = p.rot;
        if (p.mirror) q["mirror"] = true;
        if (!p.fields.empty()) q["fields"] = p.fields;
        if (p.pcb.placed) {
            q["pcb"] = {{"at", pt(p.pcb.pos)}, {"rot", p.pcb.rot}, {"side", p.pcb.bottom ? "bottom" : "top"}};
        }
        parts.push_back(std::move(q));
    }
    j["parts"] = std::move(parts);
    if (!symbols.empty()) j["symbols"] = std::move(symbols);
    if (!footprints.empty()) j["footprints"] = std::move(footprints);
    json wires = json::array();
    for (const auto& w : c.wires) {
        json pts = json::array();
        for (const auto& p : w.pts) pts.push_back(pt(p));
        wires.push_back(std::move(pts));
    }
    j["wires"] = std::move(wires);
    if (!c.buses.empty()) {
        json buses = json::array();
        for (const auto& b : c.buses) {
            json pts = json::array(), entries = json::array();
            for (const auto& p : b.pts) pts.push_back(pt(p));
            for (const auto& [a, e] : b.entries) entries.push_back(json::array({pt(a), pt(e)}));
            buses.push_back({{"name", b.name}, {"pts", pts}, {"entries", entries}});
        }
        j["buses"] = std::move(buses);
    }
    json labels = json::array();
    for (const auto& l : c.labels) {
        json q{{"name", l.name}, {"at", pt(l.pos)}};
        if (!l.kind.empty()) q["kind"] = l.kind;
        labels.push_back(std::move(q));
    }
    j["labels"] = std::move(labels);
    json nc = json::array();
    for (const auto& p : c.noConnects) nc.push_back(pt(p));
    j["no_connects"] = std::move(nc);
    json b;
    json outline = json::array();
    for (const auto& p : c.board.outline) outline.push_back(pt(p));
    b["layers"] = c.board.layers;
    b["outline"] = std::move(outline);
    json traces = json::array();
    for (const auto& t : c.board.traces) {
        traces.push_back({{"a", pt(t.a)}, {"b", pt(t.b)}, {"width", t.width}, {"layer", layerShort(t.layer)}, {"net", t.net}});
    }
    b["traces"] = std::move(traces);
    json vias = json::array();
    for (const auto& v : c.board.vias) vias.push_back({{"at", pt(v.pos)}, {"drill", v.drill}, {"diameter", v.diameter}, {"net", v.net}});
    b["vias"] = std::move(vias);
    json zones = json::array();
    for (const auto& z : c.board.zones) {
        zones.push_back({{"net", z.net}, {"layer", layerShort(z.layer)}, {"clearance", z.clearance}, {"min_width", z.minWidth}, {"thermal", z.thermal}, {"spoke", z.spoke}});
    }
    b["zones"] = std::move(zones);
    const Rules& r = c.board.rules;
    b["rules"] = {{"trace", r.trace},         {"power_trace", r.powerTrace}, {"clearance", r.clearance}, {"min_trace", r.minTrace},
                  {"via_drill", r.viaDrill},  {"via_diameter", r.viaDiameter}, {"min_drill", r.minDrill}, {"min_annular", r.minAnnular},
                  {"edge_clearance", r.edgeClearance}, {"grid", r.grid}};
    j["board"] = std::move(b);
    return j.dump(1) + "\n";
}

Circuit fromJson(const std::string& text) {
    json j;
    try {
        j = json::parse(text);
    } catch (const std::exception& e) {
        throw std::runtime_error(std::string("not valid JSON: ") + e.what());
    }
    if (!j.is_object() || !j.contains("parts")) throw std::runtime_error("not a circuit file (no \"parts\")");
    Circuit c;
    try {
        c.title = j.value("title", "");
        std::map<std::string, std::shared_ptr<const Symbol>> symbols;
        std::map<std::string, std::shared_ptr<const Footprint>> footprints;
        if (j.contains("symbols")) {
            for (const auto& [key, body] : j["symbols"].items()) symbols[key] = readSymbol(body);
        }
        if (j.contains("footprints")) {
            for (const auto& [key, body] : j["footprints"].items()) footprints[key] = readFootprint(body);
        }
        for (const auto& q : j["parts"]) {
            Part p;
            if (q.contains("custom_symbol")) {
                auto it = symbols.find(q["custom_symbol"].get<std::string>());
                if (it == symbols.end()) throw std::runtime_error("a part names a symbol the file doesn't hold");
                p.customSymbol = it->second;
            }
            if (q.contains("custom_footprint")) {
                auto it = footprints.find(q["custom_footprint"].get<std::string>());
                if (it == footprints.end()) throw std::runtime_error("a part names a footprint the file doesn't hold");
                p.customFootprint = it->second;
            }
            p.unit = q.value("unit", 0);
            p.ref = q.value("ref", "");
            p.symbol = q.value("symbol", "");
            p.value = q.value("value", "");
            p.footprint = q.value("footprint", "");
            p.pins = q.value("pins", 0);
            if (q.contains("pin_names")) p.pinNames = q["pin_names"].get<std::vector<std::string>>();
            if (q.contains("at")) p.pos = readPt(q["at"]);
            p.rot = q.value("rot", 0);
            p.mirror = q.value("mirror", false);
            if (q.contains("fields")) p.fields = q["fields"].get<std::map<std::string, std::string>>();
            if (q.contains("pcb")) {
                const auto& b = q["pcb"];
                p.pcb.placed = true;
                p.pcb.pos = readPt(b["at"]);
                p.pcb.rot = b.value("rot", 0);
                p.pcb.bottom = b.value("side", "top") == "bottom";
            }
            c.parts.push_back(std::move(p));
        }
        if (j.contains("wires")) {
            for (const auto& w : j["wires"]) {
                Wire wire;
                for (const auto& p : w) wire.pts.push_back(readPt(p));
                if (wire.pts.size() >= 2) c.wires.push_back(std::move(wire));
            }
        }
        if (j.contains("buses")) {
            for (const auto& q : j["buses"]) {
                Bus b;
                b.name = q.value("name", "");
                b.members = busMembers(b.name);
                for (const auto& p : q["pts"]) b.pts.push_back(readPt(p));
                if (q.contains("entries"))
                    for (const auto& e : q["entries"]) b.entries.push_back({readPt(e[0]), readPt(e[1])});
                if (b.pts.size() >= 2) c.buses.push_back(std::move(b));
            }
        }
        if (j.contains("labels")) {
            for (const auto& l : j["labels"]) c.labels.push_back({l.value("name", ""), readPt(l["at"]), l.value("kind", "")});
        }
        if (j.contains("no_connects")) {
            for (const auto& p : j["no_connects"]) c.noConnects.push_back(readPt(p));
        }
        if (j.contains("board")) {
            const auto& b = j["board"];
            c.board.layers = b.value("layers", 2) == 4 ? 4 : 2;
            if (b.contains("outline")) {
                for (const auto& p : b["outline"]) c.board.outline.push_back(readPt(p));
            }
            if (b.contains("traces")) {
                for (const auto& t : b["traces"]) {
                    c.board.traces.push_back({readPt(t["a"]), readPt(t["b"]), t.value("width", 0.25), std::max(0, parseLayer(t.value("layer", "F"))), t.value("net", "")});
                }
            }
            if (b.contains("vias")) {
                for (const auto& v : b["vias"]) c.board.vias.push_back({readPt(v["at"]), v.value("drill", 0.4), v.value("diameter", 0.8), v.value("net", "")});
            }
            if (b.contains("zones")) {
                for (const auto& z : b["zones"]) {
                    Zone zone{z.value("net", ""), std::max(0, parseLayer(z.value("layer", "B"))), z.value("clearance", 0.3), z.value("min_width", 0.25)};
                    zone.thermal = z.value("thermal", true);
                    zone.spoke = z.value("spoke", 0.5);
                    c.board.zones.push_back(zone);
                }
            }
            if (b.contains("rules")) {
                const auto& r = b["rules"];
                Rules& o = c.board.rules;
                o.trace = r.value("trace", o.trace);
                o.powerTrace = r.value("power_trace", o.powerTrace);
                o.clearance = r.value("clearance", o.clearance);
                o.minTrace = r.value("min_trace", o.minTrace);
                o.viaDrill = r.value("via_drill", o.viaDrill);
                o.viaDiameter = r.value("via_diameter", o.viaDiameter);
                o.minDrill = r.value("min_drill", o.minDrill);
                o.minAnnular = r.value("min_annular", o.minAnnular);
                o.edgeClearance = r.value("edge_clearance", o.edgeClearance);
                o.grid = r.value("grid", o.grid);
            }
        }
    } catch (const json::exception& e) {
        throw std::runtime_error(std::string("bad circuit file: ") + e.what());
    }
    return c;
}

Circuit fromKicadNetlist(const std::string& text, std::vector<std::string>* warnings) {
    Sx root = parseSx(text);
    if (!root.isList || root.list.empty() || root.list[0].atom != "export") throw std::runtime_error("not a KiCad netlist (expected (export ...))");
    Circuit c;
    if (const Sx* design = root.child("design")) c.title = design->value("source");
    auto warn = [&](const std::string& w) {
        if (warnings) warnings->push_back(w);
    };
    struct Node {
        std::string ref, pin, function;
    };
    struct NetIn {
        std::string name;
        std::vector<Node> nodes;
    };
    std::vector<NetIn> nets;
    if (const Sx* ns = root.child("nets")) {
        for (const auto& n : ns->list) {
            if (!n.isList || n.list.empty() || n.list[0].atom != "net") continue;
            NetIn net;
            net.name = n.value("name");
            while (!net.name.empty() && net.name[0] == '/') net.name.erase(0, 1);
            for (const auto& node : n.list) {
                if (!node.isList || node.list.empty() || node.list[0].atom != "node") continue;
                net.nodes.push_back({node.value("ref"), node.value("pin"), node.value("pinfunction")});
            }
            nets.push_back(std::move(net));
        }
    }
    // pins each part uses, for checking the mapping and sizing generic ICs
    std::map<std::string, std::map<std::string, std::string>> usedPins;  // ref -> pin -> function
    for (const auto& n : nets) {
        for (const auto& node : n.nodes) usedPins[node.ref][node.pin] = node.function;
    }
    double x = 0, y = 0, rowH = 0;
    const double rowWidth = 70;
    if (const Sx* comps = root.child("components")) {
        for (const auto& comp : comps->list) {
            if (!comp.isList || comp.list.empty() || comp.list[0].atom != "comp") continue;
            Part p;
            p.ref = comp.value("ref");
            p.value = comp.value("value");
            std::string libPart;
            if (const Sx* ls = comp.child("libsource")) libPart = ls->value("part");
            int pins = 0;
            std::string sym = mapKicadPart(libPart, pins);
            if (sym.empty() || sym == "power" || sym == "gnd") sym = "";
            const auto& used = usedPins[p.ref];
            std::shared_ptr<const Symbol> s;
            if (!sym.empty()) {
                s = findSymbol(sym, pins);
                if (s) {
                    for (const auto& [pin, fn] : used) {
                        bool found = false;
                        for (const auto& pd : s->pins) found = found || pd.number == pin;
                        if (!found) {
                            s = nullptr;
                            break;
                        }
                    }
                }
            }
            if (!s) {
                int maxPin = 0;
                for (const auto& [pin, fn] : used) {
                    if (!pin.empty() && std::all_of(pin.begin(), pin.end(), [](char ch) { return std::isdigit(static_cast<unsigned char>(ch)); }))
                        maxPin = std::max(maxPin, std::stoi(pin));
                    else warn(p.ref + ": pin '" + pin + "' isn't a number; it was skipped");
                }
                maxPin = std::max(maxPin, 2);
                std::vector<std::string> names(static_cast<std::size_t>(maxPin));
                for (const auto& [pin, fn] : used) {
                    if (!pin.empty() && std::all_of(pin.begin(), pin.end(), [](char ch) { return std::isdigit(static_cast<unsigned char>(ch)); })) {
                        const int k = std::stoi(pin);
                        if (k >= 1 && k <= maxPin && !fn.empty() && fn != "~") names[static_cast<std::size_t>(k - 1)] = fn;
                    }
                }
                sym = "ic";
                pins = maxPin;
                p.pinNames = names;
                if (!libPart.empty()) warn(p.ref + ": '" + libPart + "' has no library symbol; using a generic " + std::to_string(maxPin + maxPin % 2) + "-pin IC");
                s = findSymbol(sym, pins, p.pinNames);
            }
            p.symbol = sym;
            p.pins = pins;
            const std::string fp = comp.value("footprint");
            if (!fp.empty() && findFootprint(fp)) {
                p.footprint = fp;
                if (auto colon = fp.find(':'); colon != std::string::npos) p.footprint = fp.substr(colon + 1);
                // keep the short library name when we know it
                p.footprint = findFootprint(fp)->name;
            } else {
                p.footprint = s->footprint;
                if (!fp.empty()) warn(p.ref + ": footprint '" + fp + "' isn't built in; using " + (s->footprint.empty() ? "none" : s->footprint));
            }
            if (p.value.empty()) p.value = s->value;
            // grid layout
            const double w = std::ceil(s->maxX - s->minX) + 6, h = std::ceil(s->maxY - s->minY) + 4;
            if (x > 0 && x + w > rowWidth) {
                x = 0;
                y += rowH;
                rowH = 0;
            }
            p.pos = {std::round(x - s->minX + 3), std::round(y - s->minY + 2)};
            x += w;
            rowH = std::max(rowH, h);
            c.parts.push_back(std::move(p));
        }
    }
    std::set<std::string> refs;
    for (const auto& p : c.parts) refs.insert(p.ref);
    for (const auto& n : nets) {
        // single-pin nets only matter when they carry a real name (a power rail, a test point)
        if (n.nodes.empty() || (n.nodes.size() < 2 && (n.name.empty() || n.name.rfind("Net-(", 0) == 0 || n.name.rfind("unconnected", 0) == 0))) continue;
        for (const auto& node : n.nodes) {
            const Part* part = c.part(node.ref);
            if (!part) {
                warn("net " + n.name + " names an unknown part " + node.ref);
                continue;
            }
            auto s = symbolOf(*part);
            const PinDef* pd = nullptr;
            for (const auto& q : s->pins) {
                if (q.number == node.pin) pd = &q;
            }
            if (pd) labelPin(c, *part, *pd, n.name);
        }
    }
    return c;
}

// ---- document -------------------------------------------------------------------------

std::string CircuitDoc::summary() const {
    int parts = 0;
    for (const auto& p : circuit.parts) {
        auto sym = symbolOf(p);
        if (sym && !sym->power) ++parts;
    }
    const Netlist nl = buildNetlist(circuit);
    std::string s = std::to_string(parts) + " parts, " + std::to_string(nl.nets.size()) + " nets, " + std::to_string(circuit.wires.size()) + " wires";
    if (circuit.board.outline.size() >= 3) {
        char b[96];
        std::snprintf(b, sizeof b, "; board %.1f x %.1f mm, %zu traces, %zu vias", boardWidth(circuit.board), boardHeight(circuit.board),
                      circuit.board.traces.size(), circuit.board.vias.size());
        s += b;
    }
    if (!path.empty()) s += " (" + path + ")";
    return s;
}

std::unique_ptr<Attachment> CircuitDoc::preview() const {
    return std::make_unique<Attachment>(Attachment{"image/png", raster::encodePng(viewImage(circuit, view)), view + ".png"});
}

Result CircuitEdit::apply() {
    before_ = doc_->circuit;
    Result r;
    try {
        r = fn_(*doc_);
    } catch (const std::exception& e) {
        r = Result::failure(e.what());
    }
    if (!r.ok) revert();
    return r;
}

void CircuitEdit::revert() { doc_->circuit = before_; }

}  // namespace wetwaresuite::circuit
