// Schematic geometry, connectivity (netlist), ERC, BOM and netlist exports.
#include <optional>
#include <map>
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <numeric>
#include <set>
#include <sstream>

#include "wetwaresuite/circuit/circuit.hpp"

namespace wetwaresuite::circuit {

namespace {

std::string upper(std::string s) {
    for (auto& c : s) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return s;
}

std::string lower(std::string s) {
    for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

struct Uf {
    std::vector<int> p;
    int add() {
        p.push_back(static_cast<int>(p.size()));
        return p.back();
    }
    int find(int x) {
        while (p[static_cast<std::size_t>(x)] != x) {
            p[static_cast<std::size_t>(x)] = p[static_cast<std::size_t>(p[static_cast<std::size_t>(x)])];
            x = p[static_cast<std::size_t>(x)];
        }
        return x;
    }
    void unite(int a, int b) {
        a = find(a), b = find(b);
        if (a != b) p[static_cast<std::size_t>(std::max(a, b))] = std::min(a, b);
    }
};

std::pair<long long, long long> key(Pt p) { return {std::llround(p.x * 1000), std::llround(p.y * 1000)}; }

bool same(Pt a, Pt b) { return std::abs(a.x - b.x) < 1e-6 && std::abs(a.y - b.y) < 1e-6; }

// p strictly inside segment ab (not at its ends)
bool onSegmentInterior(Pt p, Pt a, Pt b) {
    if (same(p, a) || same(p, b)) return false;
    const double cross = (b.x - a.x) * (p.y - a.y) - (b.y - a.y) * (p.x - a.x);
    const double len = std::hypot(b.x - a.x, b.y - a.y);
    if (len < 1e-9 || std::abs(cross) / len > 1e-6) return false;
    const double dot = (p.x - a.x) * (b.x - a.x) + (p.y - a.y) * (b.y - a.y);
    return dot > 0 && dot < len * len;
}

std::string csvField(const std::string& s) {
    if (s.find_first_of(",\"\n") == std::string::npos) return s;
    std::string out = "\"";
    for (char c : s) {
        if (c == '"') out += '"';
        out += c;
    }
    return out + "\"";
}

std::string jsonStr(const std::string& s) {
    std::string out = "\"";
    for (char c : s) {
        if (c == '"' || c == '\\') out += '\\', out += c;
        else if (c == '\n') out += "\\n";
        else if (static_cast<unsigned char>(c) < 0x20) out += ' ';
        else out += c;
    }
    return out + "\"";
}

std::string sexpStr(const std::string& s) {
    std::string out = "\"";
    for (char c : s) {
        if (c == '"' || c == '\\') out += '\\';
        out += c;
    }
    return out + "\"";
}

bool refLess(const std::string& a, const std::string& b) {
    // R2 before R10
    std::size_t i = 0, j = 0;
    while (i < a.size() && !std::isdigit(static_cast<unsigned char>(a[i]))) ++i;
    while (j < b.size() && !std::isdigit(static_cast<unsigned char>(b[j]))) ++j;
    const std::string pa = a.substr(0, i), pb = b.substr(0, j);
    if (pa != pb) return pa < pb;
    const long na = i < a.size() ? std::atol(a.c_str() + i) : 0, nb = j < b.size() ? std::atol(b.c_str() + j) : 0;
    if (na != nb) return na < nb;
    return a < b;
}

}  // namespace

Part* Circuit::part(const std::string& ref) {
    for (auto& p : parts) {
        if (p.ref == ref) return &p;
    }
    const std::string u = upper(ref);
    for (auto& p : parts) {
        if (upper(p.ref) == u) return &p;
    }
    return nullptr;
}

const Part* Circuit::part(const std::string& ref) const { return const_cast<Circuit*>(this)->part(ref); }

std::shared_ptr<const Symbol> symbolOf(const Part& part) {
    if (part.customSymbol) return part.customSymbol;
    if (part.unit > 0)
        if (auto u = findSymbolUnit(part.symbol, part.unit)) return u;
    return findSymbol(part.symbol, part.pins, part.pinNames);
}

std::shared_ptr<const Footprint> footprintOf(const Part& part) {
    if (part.customFootprint) return part.customFootprint;
    return findFootprint(part.footprint);
}

Pt toSheet(const Part& part, Pt p) {
    if (part.mirror) p.x = -p.x;
    const int r = ((part.rot % 360) + 360) % 360;
    Pt q = p;
    if (r == 90) q = {p.y, -p.x};
    else if (r == 180) q = {-p.x, -p.y};
    else if (r == 270) q = {-p.y, p.x};
    return {part.pos.x + q.x, part.pos.y + q.y};
}

Pt pinPos(const Part& part, const PinDef& pin) { return toSheet(part, pin.pos); }

PinRef resolvePin(const Circuit& c, const std::string& text, std::string& error) {
    PinRef out;
    const auto dot = text.rfind('.');
    if (dot == std::string::npos || dot == 0 || dot + 1 == text.size()) {
        error = "pin '" + text + "' should look like R1.1 or U1.VCC";
        return out;
    }
    const std::string ref = text.substr(0, dot), pin = text.substr(dot + 1);
    const Part* part = c.part(ref);
    if (!part) {
        error = "no part " + ref + " (circuit.parts lists them)";
        return out;
    }
    if (part->unit > 0) {  // a multi-unit part: the pin may be on another unit with the same ref
        const std::string ur = upper(part->ref);
        for (int pass = 0; pass < 2; ++pass) {
            for (const auto& q : c.parts) {
                if (q.unit <= 0 || upper(q.ref) != ur) continue;
                auto qs = symbolOf(q);
                if (!qs) continue;
                for (const auto& pd : qs->pins) {
                    if (pass == 0 ? pd.number == pin : upper(pd.name) == upper(pin)) {
                        out.part = &q;
                        out.pin = &pd;
                        out.pos = pinPos(q, pd);
                        return out;
                    }
                }
            }
        }
    }
    auto sym = symbolOf(*part);
    if (!sym) {
        error = "part " + ref + " has an unknown symbol " + part->symbol;
        return out;
    }
    const PinDef* found = nullptr;
    for (const auto& p : sym->pins) {
        if (p.number == pin) found = &p;
    }
    const std::string u = upper(pin);
    if (!found) {
        for (const auto& p : sym->pins) {
            if (upper(p.name) == u) {
                found = &p;
                break;
            }
        }
    }
    if (!found) {
        for (const auto& p : sym->pins) {
            std::stringstream ss(p.name);
            std::string alias;
            while (std::getline(ss, alias, '/')) {
                if (upper(alias) == u) found = &p;
            }
            if (found) break;
        }
    }
    if (!found) {
        std::string list;
        for (std::size_t i = 0; i < sym->pins.size() && i < 40; ++i) list += (i ? ", " : "") + sym->pins[i].number + (sym->pins[i].name != sym->pins[i].number ? "=" + sym->pins[i].name : "");
        error = ref + " has no pin '" + pin + "' (pins: " + list + ")";
        return out;
    }
    out.part = part;
    out.pin = found;
    out.pos = pinPos(*part, *found);
    return out;
}

std::string nextRef(const Circuit& c, const std::string& prefix) {
    std::set<std::string> used;
    for (const auto& p : c.parts) used.insert(upper(p.ref));
    for (int i = 1;; ++i) {
        const std::string r = prefix + std::to_string(i);
        if (!used.count(upper(r))) return r;
    }
}

std::optional<double> parseValue(const std::string& textIn) {
    std::string t;
    for (std::size_t i = 0; i < textIn.size(); ++i) {
        const unsigned char c = static_cast<unsigned char>(textIn[i]);
        if (c == 0xC2 && i + 1 < textIn.size() && static_cast<unsigned char>(textIn[i + 1]) == 0xB5) t += 'u', ++i;       // µ
        else if (c == 0xCE && i + 1 < textIn.size() && static_cast<unsigned char>(textIn[i + 1]) == 0xBC) t += 'u', ++i;  // μ
        else if (c == 0xCE && i + 1 < textIn.size() && static_cast<unsigned char>(textIn[i + 1]) == 0xA9) ++i;            // Ω
        else if (c != ' ') t += static_cast<char>(c);
    }
    if (t.empty()) return std::nullopt;
    std::size_t i = 0;
    std::string num;
    if (t[i] == '+' || t[i] == '-') num += t[i++];
    while (i < t.size() && (std::isdigit(static_cast<unsigned char>(t[i])) || t[i] == '.')) num += t[i++];
    if (num.empty() || num == "+" || num == "-") return std::nullopt;
    double mult = 1;
    std::string rest = t.substr(i);
    std::string lr = lower(rest);
    auto suffix = [&](char ch) -> std::optional<double> {
        switch (ch) {
            case 'f': return 1e-15;
            case 'p': return 1e-12;
            case 'n': return 1e-9;
            case 'u': return 1e-6;
            case 'm': return 1e-3;
            case 'k': return 1e3;
            case 'M': return 1e6;
            case 'G': return 1e9;
            case 'T': return 1e12;
            case 'R': case 'r': return 1.0;
            default: return std::nullopt;
        }
    };
    std::string frac;
    if (lr.rfind("meg", 0) == 0) {
        mult = 1e6;
        rest = rest.substr(3);
    } else if (!rest.empty()) {
        if (auto m = suffix(rest[0])) {
            // "1M" is mega for parts; SPICE's "m" stays milli
            mult = *m;
            rest = rest.substr(1);
            // 4k7 style
            while (!rest.empty() && std::isdigit(static_cast<unsigned char>(rest[0]))) frac += rest[0], rest = rest.substr(1);
        } else if (rest[0] == 'K') {
            mult = 1e3;
            rest = rest.substr(1);
            while (!rest.empty() && std::isdigit(static_cast<unsigned char>(rest[0]))) frac += rest[0], rest = rest.substr(1);
        }
    }
    // whatever is left must be a unit
    const std::string unit = lower(rest);
    static const std::set<std::string> units = {"", "v", "a", "f", "h", "ohm", "ohms", "hz", "s", "w"};
    if (!units.count(unit)) return std::nullopt;
    if (!frac.empty()) {
        if (num.find('.') != std::string::npos) return std::nullopt;
        num += "." + frac;
    }
    try {
        return std::stod(num) * mult;
    } catch (...) {
        return std::nullopt;
    }
}

std::string formatValue(double v) {
    if (v == 0) return "0";
    static const char* prefixes[] = {"f", "p", "n", "u", "m", "", "k", "M", "G"};
    const double a = std::abs(v);
    int e = static_cast<int>(std::floor(std::log10(a) / 3));
    e = std::clamp(e, -5, 3);
    const double scaled = v / std::pow(1000.0, e);
    char buf[32];
    std::snprintf(buf, sizeof buf, "%.3g", scaled);
    return std::string(buf) + prefixes[e + 5];
}

// ---- connectivity ---------------------------------------------------------------------

const Net* Netlist::net(const std::string& name) const {
    for (const auto& n : nets) {
        if (n.name == name) return &n;
    }
    const std::string u = upper(name);
    for (const auto& n : nets) {
        if (upper(n.name) == u) return &n;
    }
    return nullptr;
}

std::string Netlist::netOf(const std::string& ref, const std::string& pin) const {
    auto it = pinNet.find(ref + "." + pin);
    if (it == pinNet.end() || it->second < 0) return {};
    return nets[static_cast<std::size_t>(it->second)].name;
}

Netlist buildNetlist(const Circuit& c) {
    Netlist nl;
    Uf uf;
    std::map<std::pair<long long, long long>, int> points;
    std::vector<int> touches;  // incidences per point id
    std::vector<bool> tjoin;
    auto pointId = [&](Pt p) {
        auto k = key(p);
        auto it = points.find(k);
        if (it != points.end()) return it->second;
        const int id = uf.add();
        touches.push_back(0);
        tjoin.push_back(false);
        points[k] = id;
        return id;
    };
    struct PinAt {
        const Part* part;
        const PinDef* pin;
        int id;
        bool power;
        bool module;
    };
    std::vector<PinAt> pins;
    std::map<std::string, int> nameAnchor;  // label/power name -> point id
    std::map<int, std::vector<std::string>> namesAt;
    std::set<std::string> powerNames;
    for (const auto& part : c.parts) {
        auto sym = symbolOf(part);
        if (!sym) continue;
        std::map<std::string, int> sameName;
        for (const auto& pin : sym->pins) {
            const int id = pointId(pinPos(part, pin));
            ++touches[static_cast<std::size_t>(id)];
            pins.push_back({&part, &pin, id, sym->power, sym->module});
            if (sym->module || sym->joinSameName) {
                auto [it, fresh] = sameName.emplace(pin.name, id);
                if (!fresh) uf.unite(it->second, id);
            }
            if (sym->power) {
                const std::string name = part.value.empty() ? sym->value : part.value;
                powerNames.insert(name);
                namesAt[id].push_back(name);
                auto [it, fresh] = nameAnchor.emplace(name, id);
                if (!fresh) uf.unite(it->second, id);
            }
        }
    }
    struct Seg {
        Pt a, b;
        int ida;
    };
    std::vector<Seg> segs;
    std::vector<int> wireEnds;
    for (const auto& w : c.wires) {
        if (w.pts.empty()) continue;
        int prev = -1;
        for (std::size_t i = 0; i < w.pts.size(); ++i) {
            const int id = pointId(w.pts[i]);
            ++touches[static_cast<std::size_t>(id)];
            if (i > 0 && i + 1 < w.pts.size()) ++touches[static_cast<std::size_t>(id)];  // a bend is not an end
            if (prev >= 0) {
                uf.unite(prev, id);
                segs.push_back({w.pts[i - 1], w.pts[i], prev});
            }
            prev = id;
        }
        wireEnds.push_back(pointId(w.pts.front()));
        wireEnds.push_back(pointId(w.pts.back()));
    }
    for (const auto& l : c.labels) {
        const int id = pointId(l.pos);
        ++touches[static_cast<std::size_t>(id)];
        namesAt[id].push_back(l.name);
        auto [it, fresh] = nameAnchor.emplace(l.name, id);
        if (!fresh) uf.unite(it->second, id);
    }
    std::set<int> ncPoints;
    for (const auto& p : c.noConnects) ncPoints.insert(pointId(p));
    // T-junctions: any point lying inside a wire segment joins that wire
    for (const auto& [k, id] : points) {
        const Pt p{static_cast<double>(k.first) / 1000.0, static_cast<double>(k.second) / 1000.0};
        for (const auto& s : segs) {
            if (onSegmentInterior(p, s.a, s.b)) {
                uf.unite(id, s.ida);
                tjoin[static_cast<std::size_t>(id)] = true;
            }
        }
    }
    // groups
    std::map<int, int> groupNet;
    std::map<int, std::vector<std::string>> groupNames;
    std::map<int, int> groupWires;
    for (const auto& [id, names] : namesAt) {
        auto& g = groupNames[uf.find(id)];
        for (const auto& n : names) {
            if (std::find(g.begin(), g.end(), n) == g.end()) g.push_back(n);
        }
    }
    std::map<int, int> groupAllPins, groupLabels;
    for (const auto& pa : pins) {
        ++groupAllPins[uf.find(pa.id)];
    }
    for (const auto& l : c.labels) ++groupLabels[uf.find(pointId(l.pos))];
    for (const auto& s : segs) ++groupWires[uf.find(s.ida)];
    // net order: by first pin in part order
    for (const auto& pa : pins) {
        if (pa.power) continue;
        const int g = uf.find(pa.id);
        int idx;
        if (auto it = groupNet.find(g); it != groupNet.end()) {
            idx = it->second;
        } else {
            idx = static_cast<int>(nl.nets.size());
            groupNet[g] = idx;
            Net n;
            auto names = groupNames[g];
            // power names first, then labels in the order they were added
            std::stable_sort(names.begin(), names.end(), [&](const std::string& a, const std::string& b) {
                return powerNames.count(a) > powerNames.count(b);
            });
            n.names = names;
            for (const auto& nm : names) {
                if (powerNames.count(nm)) n.powerNames.push_back(nm);
            }
            n.named = !names.empty();
            n.wires = groupWires[g];
            nl.nets.push_back(std::move(n));
        }
        nl.nets[static_cast<std::size_t>(idx)].nodes.push_back({pa.part->ref, pa.pin->number, pa.pin->name, pa.pin->type});
    }
    for (const auto& pa : pins) {
        const int g = uf.find(pa.id);
        auto it = groupNet.find(g);
        nl.pinNet[pa.part->ref + "." + pa.pin->number] = it == groupNet.end() ? -1 : it->second;
        // isolated: the only pin in its group, no wire and no label reaching it
        const bool isolated = groupAllPins[g] == 1 && groupWires[g] == 0 && groupLabels[g] == 0;
        if (isolated && !ncPoints.count(pa.id)) nl.isolatedPins.push_back(pa.part->ref + "." + pa.pin->number);
    }
    // names for unnamed nets
    std::set<std::string> taken;
    for (auto& n : nl.nets) {
        if (n.named) {
            n.name = n.names.front();
            taken.insert(n.name);
        }
    }
    for (auto& n : nl.nets) {
        if (n.named) continue;
        auto first = std::min_element(n.nodes.begin(), n.nodes.end(), [](const NetNode& a, const NetNode& b) {
            return a.ref != b.ref ? refLess(a.ref, b.ref) : a.pin < b.pin;
        });
        std::string name = "Net-(" + first->ref + "-" + first->pin + ")";
        while (taken.count(name)) name += "_";
        taken.insert(name);
        n.name = name;
    }
    for (int id : wireEnds) {
        if (touches[static_cast<std::size_t>(id)] <= 1 && !tjoin[static_cast<std::size_t>(id)] && !ncPoints.count(id)) {
            for (const auto& [k, pid] : points) {
                if (pid == id) {
                    char buf[64];
                    std::snprintf(buf, sizeof buf, "%g,%g", static_cast<double>(k.first) / 1000.0, static_cast<double>(k.second) / 1000.0);
                    nl.danglingWireEnds.push_back(buf);
                    break;
                }
            }
        }
    }
    return nl;
}

std::vector<Issue> erc(const Circuit& c, const Netlist& nl) {
    std::vector<Issue> out;
    std::map<std::string, int> refs;
    for (const auto& p : c.parts) {
        auto sym = symbolOf(p);
        if (!sym) {
            out.push_back({"error", "unknown_symbol", p.ref + " uses an unknown symbol '" + p.symbol + "'"});
            continue;
        }
        if (sym->power) continue;
        ++refs[upper(p.ref) + (p.unit > 0 ? " (unit " + std::to_string(p.unit) + ")" : "")];
        if (p.ref.empty() || p.ref.back() == '?') out.push_back({"error", "unannotated", "part '" + p.ref + "' has no reference number"});
        if (!p.footprint.empty()) {
            auto fp = footprintOf(p);
            if (!fp) {
                out.push_back({"warning", "unknown_footprint", p.ref + ": footprint '" + p.footprint + "' isn't in the library (circuit.footprints)"});
            } else {
                for (const auto& pin : sym->pins) {
                    bool found = false;
                    for (const auto& pad : fp->pads) found = found || pad.number == pin.number;
                    if (!found && pin.type != PinType::NotConnected) {
                        out.push_back({"warning", "pad_missing", p.ref + ": pin " + pin.number + " has no pad in " + fp->name});
                    }
                }
            }
        }
    }
    for (const auto& [ref, n] : refs) {
        if (n > 1) out.push_back({"error", "duplicate_ref", ref + " is used by " + std::to_string(n) + " parts"});
    }
    std::map<std::string, int> moduleUnused;
    std::set<std::string> isolated(nl.isolatedPins.begin(), nl.isolatedPins.end());
    for (const auto& p : c.parts) {
        auto sym = symbolOf(p);
        if (!sym) continue;
        for (const auto& pin : sym->pins) {
            const std::string id = p.ref + "." + pin.number;
            if (!isolated.count(id)) continue;
            if (sym->power) {
                out.push_back({"warning", "unconnected_power", "power symbol " + p.value + " (" + p.ref + ") isn't connected to anything"});
            } else if (sym->module) {
                ++moduleUnused[p.ref];
            } else if (pin.type != PinType::NotConnected) {
                out.push_back({"error", "unconnected_pin", id + (pin.name != pin.number ? " (" + pin.name + ")" : "") +
                                                              " is not connected (wire it or mark it with circuit.no_connect)"});
            }
        }
    }
    for (const auto& [ref, n] : moduleUnused) {
        out.push_back({"info", "module_unused", ref + ": " + std::to_string(n) + " pins unused (normal for a dev board)"});
    }
    std::map<std::string, int> labelUse;
    for (const auto& l : c.labels) ++labelUse[l.name];
    for (const auto& n : nl.nets) {
        if (n.powerNames.size() > 1) {
            std::string list;
            for (const auto& s : n.powerNames) list += (list.empty() ? "" : ", ") + s;
            out.push_back({"error", "shorted_power", "power nets shorted together: " + list});
        } else if (n.names.size() > 1) {
            std::string list;
            for (const auto& s : n.names) list += (list.empty() ? "" : ", ") + s;
            out.push_back({"warning", "multiple_names", "one net has several names: " + list + " (it is called " + n.name + ")"});
        }
        if (n.nodes.size() == 1 && n.powerNames.empty()) {
            if (!n.named && n.wires > 0) {
                out.push_back({"warning", "single_pin_net", n.nodes[0].ref + "." + n.nodes[0].pin + " has a wire that leads nowhere"});
            } else if (n.named && labelUse[n.name] <= 1 && !isPowerName(n.name)) {
                out.push_back({"warning", "single_pin_net", "label " + n.name + " only reaches " + n.nodes[0].ref + "." + n.nodes[0].pin});
            }
        }
        int outputs = 0, powerIn = 0, others = 0;
        for (const auto& node : n.nodes) {
            if (node.type == PinType::Output || node.type == PinType::PowerOut) ++outputs;
            else if (node.type == PinType::PowerIn) ++powerIn;
            else if (node.type != PinType::Input) ++others;
        }
        if (outputs > 1) out.push_back({"warning", "output_conflict", "net " + n.name + " has " + std::to_string(outputs) + " outputs driving it"});
        if (powerIn > 0 && outputs == 0 && others == 0 && n.powerNames.empty() && n.nodes.size() > 1) {
            out.push_back({"warning", "power_not_driven", "power pins on net " + n.name + " have no source (add a power symbol or a regulator)"});
        }
    }
    for (const auto& d : nl.danglingWireEnds) out.push_back({"warning", "dangling_wire", "wire end at " + d + " touches nothing"});
    return out;
}

std::string bomCsv(const Circuit& c, bool grouped) {
    std::set<std::string> fieldKeys;
    std::vector<const Part*> parts;
    std::set<std::string> multiUnit;  // the units of one part count once
    for (const auto& p : c.parts) {
        if (p.unit > 0 && !multiUnit.insert(p.ref).second) continue;
        auto sym = symbolOf(p);
        if (sym && (sym->power || ((sym->name == "vsource" || sym->name == "isource") && p.footprint.empty()))) continue;
        if (p.fields.count("dnp") || (sym && !sym->inBom)) continue;
        parts.push_back(&p);
        for (const auto& [k, v] : p.fields) fieldKeys.insert(k);
    }
    std::sort(parts.begin(), parts.end(), [](const Part* a, const Part* b) { return refLess(a->ref, b->ref); });
    std::ostringstream o;
    o << "Item,Qty,References,Value,Footprint,Description";
    for (const auto& k : fieldKeys) o << "," << csvField(k);
    o << "\n";
    struct Row {
        std::vector<const Part*> parts;
    };
    std::vector<Row> rows;
    for (const Part* p : parts) {
        bool merged = false;
        if (grouped) {
            for (auto& r : rows) {
                const Part* q = r.parts.front();
                if (q->value == p->value && q->footprint == p->footprint && canonicalSymbol(q->symbol) == canonicalSymbol(p->symbol) &&
                    q->fields == p->fields) {
                    r.parts.push_back(p);
                    merged = true;
                    break;
                }
            }
        }
        if (!merged) rows.push_back({{p}});
    }
    int item = 1;
    for (const auto& r : rows) {
        std::string refs;
        for (const Part* p : r.parts) refs += (refs.empty() ? "" : " ") + p->ref;
        const Part* p = r.parts.front();
        auto sym = symbolOf(*p);
        o << item++ << "," << r.parts.size() << "," << csvField(refs) << "," << csvField(p->value) << "," << csvField(p->footprint) << ","
          << csvField(sym ? sym->title : p->symbol);
        for (const auto& k : fieldKeys) {
            auto it = p->fields.find(k);
            o << "," << csvField(it == p->fields.end() ? "" : it->second);
        }
        o << "\n";
    }
    return o.str();
}

std::string netlistText(const Circuit&, const Netlist& nl) {
    std::ostringstream o;
    for (const auto& n : nl.nets) {
        o << n.name << ":";
        for (const auto& node : n.nodes) o << " " << node.ref << "." << node.pin;
        o << "\n";
    }
    return o.str();
}

std::string kicadNetlist(const Circuit& c, const Netlist& nl) {
    std::ostringstream o;
    o << "(export (version \"E\")\n  (design\n    (source " << sexpStr(c.title.empty() ? "circuit" : c.title) << ")\n    (tool \"wetwaresuite circuit\"))\n";
    o << "  (components\n";
    std::vector<const Part*> parts;
    std::set<std::string> multiUnit;
    for (const auto& p : c.parts) {
        auto sym = symbolOf(p);
        if (sym && sym->power) continue;
        if (p.unit > 0 && !multiUnit.insert(p.ref).second) continue;
        parts.push_back(&p);
    }
    std::sort(parts.begin(), parts.end(), [](const Part* a, const Part* b) { return refLess(a->ref, b->ref); });
    for (const Part* p : parts) {
        auto sym = symbolOf(*p);
        o << "    (comp (ref " << sexpStr(p->ref) << ")\n      (value " << sexpStr(p->value) << ")\n";
        if (!p->footprint.empty()) o << "      (footprint " << sexpStr(p->footprint) << ")\n";
        o << "      (libsource (lib \"wetwaresuite\") (part " << sexpStr(canonicalSymbol(p->symbol)) << ") (description "
          << sexpStr(sym ? sym->title : "") << ")))\n";
    }
    o << "  )\n  (nets\n";
    int code = 1;
    for (const auto& n : nl.nets) {
        o << "    (net (code \"" << code++ << "\") (name " << sexpStr(n.name) << ")";
        for (const auto& node : n.nodes) {
            o << "\n      (node (ref " << sexpStr(node.ref) << ") (pin " << sexpStr(node.pin) << ")";
            if (node.pinName != node.pin) o << " (pinfunction " << sexpStr(node.pinName) << ")";
            o << ")";
        }
        o << ")\n";
    }
    o << "  )\n)\n";
    return o.str();
}

std::string netlistJson(const Circuit&, const Netlist& nl) {
    std::ostringstream o;
    o << "{\"nets\": [";
    for (std::size_t i = 0; i < nl.nets.size(); ++i) {
        const auto& n = nl.nets[i];
        o << (i ? ",\n  " : "\n  ") << "{\"name\": " << jsonStr(n.name) << ", \"pins\": [";
        for (std::size_t j = 0; j < n.nodes.size(); ++j) o << (j ? ", " : "") << jsonStr(n.nodes[j].ref + "." + n.nodes[j].pin);
        o << "]}";
    }
    o << "\n]}\n";
    return o.str();
}

// ---- editing helpers ------------------------------------------------------------------

void addWire(Circuit& c, Pt a, Pt b, bool verticalFirst) {
    Wire w;
    w.pts.push_back(a);
    if (std::abs(a.x - b.x) > 1e-9 && std::abs(a.y - b.y) > 1e-9) w.pts.push_back(verticalFirst ? Pt{a.x, b.y} : Pt{b.x, a.y});
    w.pts.push_back(b);
    c.wires.push_back(std::move(w));
}

bool sheetPointFree(const Circuit& c, Pt p) {
    for (const auto& w : c.wires) {
        for (std::size_t i = 0; i < w.pts.size(); ++i) {
            if (same(w.pts[i], p)) return false;
            if (i > 0 && onSegmentInterior(p, w.pts[i - 1], w.pts[i])) return false;
        }
    }
    for (const auto& l : c.labels) {
        if (same(l.pos, p)) return false;
    }
    for (const auto& n : c.noConnects) {
        if (same(n, p)) return false;
    }
    for (const auto& part : c.parts) {
        if (auto sym = symbolOf(part)) {
            for (const auto& pin : sym->pins) {
                if (same(pinPos(part, pin), p)) return false;
            }
        }
    }
    return true;
}

void labelPin(Circuit& c, const Part& part, const PinDef& pin, const std::string& name) {
    const Pt pos = pinPos(part, pin);
    const Pt inner = toSheet(part, pin.end);
    double dx = pos.x - inner.x, dy = pos.y - inner.y;
    const double len = std::hypot(dx, dy);
    if (len < 1e-9) {
        dx = 0, dy = -1;
        auto sym = symbolOf(part);
        // power symbols: away from the glyph
        if (sym && sym->power) dy = name == "GND" || sym->name == "gnd" ? -1 : 1;
    } else {
        dx /= len, dy /= len;
    }
    // step out until the stub and label land on free points (a label on another wire would join
    // nets): straight out first, then sideways; a label of the same name already there is reused
    const Pt d{std::round(dx), std::round(dy)};
    for (const auto& l : c.labels) {
        if (l.name == name && same(l.pos, Pt{pos.x + d.x, pos.y + d.y})) return;
    }
    const Pt dirs[4] = {d, {-d.y, d.x}, {d.y, -d.x}, {-d.x, -d.y}};
    for (int k = 1; k <= 4; ++k) {
        for (const Pt dir : dirs) {
            if (dir.x == 0 && dir.y == 0) continue;
            bool free = true;
            for (int j = 1; j <= k && free; ++j) free = sheetPointFree(c, {pos.x + dir.x * j, pos.y + dir.y * j});
            if (!free) continue;
            const Pt end{pos.x + dir.x * k, pos.y + dir.y * k};
            c.wires.push_back({{pos, end}});
            c.labels.push_back({name, end});
            return;
        }
    }
    // nowhere free: the label sits on the pin itself
    c.labels.push_back({name, pos});
}

void movePart(Circuit& c, Part& part, Pt pos, int rot, bool mirror) {
    auto sym = symbolOf(part);
    std::vector<Pt> before;
    if (sym) {
        for (const auto& pin : sym->pins) before.push_back(pinPos(part, pin));
    }
    part.pos = pos;
    part.rot = ((rot % 360) + 360) % 360;
    part.mirror = mirror;
    if (!sym) return;
    std::vector<Pt> after;
    for (const auto& pin : sym->pins) after.push_back(pinPos(part, pin));
    auto moved = [&](Pt p) -> std::optional<Pt> {
        for (std::size_t i = 0; i < before.size(); ++i) {
            if (same(before[i], p)) return after[i];
        }
        return std::nullopt;
    };
    for (auto& w : c.wires) {
        if (w.pts.size() < 2) continue;
        auto first = moved(w.pts.front());
        auto last = moved(w.pts.back());
        if (first && last && w.pts.size() == 2) {
            w.pts = {*first, *last};
            if (std::abs(first->x - last->x) > 1e-9 && std::abs(first->y - last->y) > 1e-9) w.pts.insert(w.pts.begin() + 1, Pt{last->x, first->y});
            continue;
        }
        if (first) {
            const Pt old = w.pts[0], next = w.pts[1];
            w.pts[0] = *first;
            if (std::abs(first->x - next.x) > 1e-9 && std::abs(first->y - next.y) > 1e-9) {
                const bool horizontal = std::abs(old.y - next.y) < 1e-9;
                w.pts.insert(w.pts.begin() + 1, horizontal ? Pt{first->x, next.y} : Pt{next.x, first->y});
            }
        }
        if (last) {
            const std::size_t n = w.pts.size();
            const Pt old = w.pts[n - 1], prev = w.pts[n - 2];
            w.pts[n - 1] = *last;
            if (std::abs(last->x - prev.x) > 1e-9 && std::abs(last->y - prev.y) > 1e-9) {
                const bool horizontal = std::abs(old.y - prev.y) < 1e-9;
                w.pts.insert(w.pts.end() - 1, horizontal ? Pt{last->x, prev.y} : Pt{prev.x, last->y});
            }
        }
    }
    for (auto& l : c.labels) {
        if (auto m = moved(l.pos)) l.pos = *m;
    }
    for (auto& n : c.noConnects) {
        if (auto m = moved(n)) n = *m;
    }
}

void removePart(Circuit& c, const std::string& ref) {
    Part* part = c.part(ref);
    if (!part) return;
    std::vector<Pt> pins;
    if (auto sym = symbolOf(*part)) {
        for (const auto& pin : sym->pins) pins.push_back(pinPos(*part, pin));
    }
    auto atPin = [&](Pt p) {
        return std::any_of(pins.begin(), pins.end(), [&](Pt q) { return same(p, q); });
    };
    std::vector<Pt> farEnds;
    std::vector<Wire> kept;
    for (auto& w : c.wires) {
        if (w.pts.size() >= 2 && (atPin(w.pts.front()) || atPin(w.pts.back()))) {
            farEnds.push_back(atPin(w.pts.front()) ? w.pts.back() : w.pts.front());
        } else {
            kept.push_back(std::move(w));
        }
    }
    c.wires = std::move(kept);
    auto touchedElsewhere = [&](Pt p) {
        for (const auto& w : c.wires) {
            for (std::size_t i = 0; i < w.pts.size(); ++i) {
                if (same(w.pts[i], p)) return true;
                if (i > 0 && onSegmentInterior(p, w.pts[i - 1], w.pts[i])) return true;
            }
        }
        for (const auto& other : c.parts) {
            if (&other == part) continue;
            if (auto sym = symbolOf(other)) {
                for (const auto& pin : sym->pins) {
                    if (same(pinPos(other, pin), p)) return true;
                }
            }
        }
        return false;
    };
    std::erase_if(c.labels, [&](const Label& l) {
        if (atPin(l.pos)) return true;
        return std::any_of(farEnds.begin(), farEnds.end(), [&](Pt e) { return same(e, l.pos); }) && !touchedElsewhere(l.pos);
    });
    std::erase_if(c.noConnects, [&](Pt p) { return atPin(p); });
    std::erase_if(c.parts, [&](const Part& p) { return &p == part; });
}

std::vector<std::string> busMembers(const std::string& raw) {
    std::string name = raw;
    while (!name.empty() && name.front() == ' ') name.erase(0, 1);
    while (!name.empty() && name.back() == ' ') name.pop_back();
    std::vector<std::string> out;
    const auto open = name.find('['), dots = name.find("..");
    if (open != std::string::npos && dots != std::string::npos && name.back() == ']' && dots > open) {
        const std::string base = name.substr(0, open);
        try {
            const int a = std::stoi(name.substr(open + 1, dots - open - 1)), b = std::stoi(name.substr(dots + 2, name.size() - dots - 3));
            if (std::abs(a - b) > 255) return {};
            for (int i = a;; i += a <= b ? 1 : -1) {
                out.push_back(base + std::to_string(i));
                if (i == b) break;
            }
        } catch (...) {
            return {};
        }
        return out;
    }
    std::string list = name;
    if (!list.empty() && list.front() == '{' && list.back() == '}') list = list.substr(1, list.size() - 2);
    std::stringstream ss(list);
    std::string part;
    while (std::getline(ss, part, ',')) {
        while (!part.empty() && part.front() == ' ') part.erase(0, 1);
        while (!part.empty() && part.back() == ' ') part.pop_back();
        if (!part.empty()) out.push_back(part);
    }
    return out;
}

void sheetBounds(const Circuit& c, double& x0, double& y0, double& x1, double& y1) {
    x0 = y0 = 1e9;
    x1 = y1 = -1e9;
    auto add = [&](Pt p) {
        x0 = std::min(x0, p.x);
        y0 = std::min(y0, p.y);
        x1 = std::max(x1, p.x);
        y1 = std::max(y1, p.y);
    };
    for (const auto& p : c.parts) {
        auto sym = symbolOf(p);
        if (!sym) continue;
        add(toSheet(p, {sym->minX, sym->minY}));
        add(toSheet(p, {sym->maxX, sym->maxY}));
        add(toSheet(p, {sym->minX, sym->maxY}));
        add(toSheet(p, {sym->maxX, sym->minY}));
        if (!sym->power) add(toSheet(p, {sym->refPos.x, sym->refPos.y - 0.7}));
        add(toSheet(p, sym->valuePos));
    }
    for (const auto& b : c.buses) {
        for (const auto& p : b.pts) add(p);
    }
    for (const auto& w : c.wires) {
        for (const auto& p : w.pts) add(p);
    }
    for (const auto& l : c.labels) {
        add(l.pos);
        add({l.pos.x + 0.6 * static_cast<double>(l.name.size()), l.pos.y - 1});
        add({l.pos.x - 0.6 * static_cast<double>(l.name.size()), l.pos.y});
    }
    for (const auto& p : c.noConnects) add(p);
    if (x0 > x1) x0 = y0 = 0, x1 = y1 = 10;
}

}  // namespace wetwaresuite::circuit
