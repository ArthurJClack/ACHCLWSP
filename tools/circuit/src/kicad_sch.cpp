// KiCad 6/7/8 schematics (.kicad_sch): reading (with KiCad's symbol drawings, hierarchical sheets
// flattened onto one sheet) and writing (one flat sheet, every symbol embedded).
#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <stdexcept>

#include "kicad_util.hpp"

namespace wetwaresuite::circuit {

using namespace kicad;

namespace {

constexpr double kGrid = 2.54;  // mm per schematic grid unit

std::string upperOf(std::string s) {
    for (auto& ch : s) ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
    return s;
}

bool near(Pt a, Pt b) { return std::abs(a.x - b.x) < 1e-3 && std::abs(a.y - b.y) < 1e-3; }

// KiCad library space (mm, y up) -> symbol space (grid units, y down)
Pt fromLib(Pt p) { return {p.x / kGrid, -p.y / kGrid}; }

PinType pinTypeOf(const std::string& t) {
    if (t == "input") return PinType::Input;
    if (t == "output") return PinType::Output;
    if (t == "bidirectional" || t == "tri_state") return PinType::Bidirectional;
    if (t == "power_in") return PinType::PowerIn;
    if (t == "power_out") return PinType::PowerOut;
    if (t == "open_collector" || t == "open_emitter") return PinType::OpenCollector;
    if (t == "no_connect") return PinType::NotConnected;
    return PinType::Passive;
}

const char* kicadPinType(PinType t) {
    switch (t) {
        case PinType::Input: return "input";
        case PinType::Output: return "output";
        case PinType::Bidirectional: return "bidirectional";
        case PinType::PowerIn: return "power_in";
        case PinType::PowerOut: return "power_out";
        case PinType::OpenCollector: return "open_collector";
        case PinType::NotConnected: return "no_connect";
        default: return "passive";
    }
}

bool onSegment(Pt p, Pt a, Pt b) {
    const double cross = (b.x - a.x) * (p.y - a.y) - (b.y - a.y) * (p.x - a.x);
    if (std::abs(cross) > 1e-3 * std::max(1.0, std::hypot(b.x - a.x, b.y - a.y))) return false;
    return p.x >= std::min(a.x, b.x) - 1e-3 && p.x <= std::max(a.x, b.x) + 1e-3 && p.y >= std::min(a.y, b.y) - 1e-3 &&
           p.y <= std::max(a.y, b.y) + 1e-3;
}

// ---- reading ----------------------------------------------------------------------------------

struct LibInfo {
    std::shared_ptr<const Symbol> sym;
    std::vector<std::string> hiddenPower;  // pin names of hidden power-in pins (implicit global nets)
    int units = 1;
    int pins = 0;  // for a symbol mapped onto a sized built-in one (headers)
    std::string footprint;  // the built-in one's default footprint, for parts KiCad gave none
};

class SchReader {
public:
    explicit SchReader(std::vector<std::string>* warnings) { warn.out = warnings; }

    Circuit read(const std::string& text, const std::filesystem::path& file) {
        const Sx root = parseSx(text);
        if (!root.is("kicad_sch")) throw std::runtime_error("not a KiCad schematic (expected (kicad_sch ...))");
        const std::string uuid = root.value("uuid");
        // KiCad 6 keeps the references of every sheet in the root file
        if (const Sx* si = root.child("symbol_instances")) {
            for (const auto& p : si->list) {
                if (p.is("path")) kicad6Refs[p.str(1)] = {p.value("reference"), std::atoi(p.value("unit").c_str())};
            }
        }
        Circuit c = readSheet(root, file, "/" + uuid, "", "", 0);
        if (const Sx* tb = root.child("title_block")) c.title = tb->value("title");
        if (c.title.empty() && !file.empty()) c.title = file.stem().string();
        fixRefs(c);
        return c;
    }

private:
    Warn warn;
    std::map<std::string, std::pair<std::string, int>> kicad6Refs;
    std::map<std::string, LibInfo> cache;
    int unknownFootprints = 0;

    LibInfo* libSymbol(const std::map<std::string, const Sx*>& libs, const std::string& fileKey, const std::string& key, int unit) {
        const std::string ck = fileKey + "|" + key + "|" + std::to_string(unit);
        if (auto it = cache.find(ck); it != cache.end()) return &it->second;
        auto found = libs.find(key);
        if (found == libs.end()) return nullptr;
        const Sx* node = found->second;
        const Sx* draw = node;
        if (const std::string ext = node->value("extends"); !ext.empty()) {
            const auto colon = key.find(':');
            auto p = libs.find(colon == std::string::npos ? ext : key.substr(0, colon + 1) + ext);
            if (p == libs.end()) p = libs.find(ext);
            if (p != libs.end()) draw = p->second;
            else warn(key + " extends " + ext + ", which the file doesn't hold");
        }
        auto s = std::make_shared<Symbol>();
        LibInfo info;
        s->name = key;
        s->title = key;
        s->category = "imported";
        s->prefix = "U";
        s->showPinNumbers = true;
        s->showPinNames = true;
        bool refSet = false, valueSet = false;
        for (const Sx* src : {node, draw}) {
            for (const auto& prop : src->list) {
                if (!prop.is("property")) continue;
                const std::string name = prop.str(1), value = prop.str(2);
                const Pt at = fromLib(xy(prop.child("at")));
                if (name == "Reference" && !refSet) {
                    std::string pre = value;
                    while (!pre.empty() && (pre.back() == '?' || std::isdigit(static_cast<unsigned char>(pre.back())))) pre.pop_back();
                    if (!pre.empty()) s->prefix = pre;
                    s->refPos = at;
                    refSet = true;
                } else if (name == "Value" && !valueSet) {
                    s->value = value;
                    s->valuePos = at;
                    valueSet = true;
                } else if (name == "Footprint" && s->footprint.empty()) {
                    s->footprint = value;
                }
            }
        }
        s->power = node->child("power") || draw->child("power");
        if (const Sx* pn = draw->child("pin_numbers")) s->showPinNumbers = !pn->flag("hide");
        if (const Sx* pn = node->child("pin_numbers")) s->showPinNumbers = !pn->flag("hide");
        if (const Sx* pn = draw->child("pin_names")) s->showPinNames = !pn->flag("hide");
        if (const Sx* pn = node->child("pin_names")) s->showPinNames = !pn->flag("hide");
        if (node->value("in_bom") == "no") s->inBom = false;
        auto fillOf = [](const Sx& item) {
            const Sx* f = item.child("fill");
            const std::string t = f ? f->value("type") : "";
            return t == "outline" || t == "color";
        };
        auto addItems = [&](const Sx& sub) {
            for (const auto& it : sub.list) {
                if (it.is("polyline") || it.is("bezier")) {
                    Shape sh;
                    if (const Sx* pts = it.child("pts")) {
                        for (const auto& p : pts->list) {
                            if (p.is("xy")) sh.pts.push_back(fromLib(xy(&p)));
                        }
                    }
                    if (sh.pts.size() < 2) continue;
                    sh.fill = fillOf(it);
                    sh.closed = sh.fill || near(sh.pts.front(), sh.pts.back());
                    s->shapes.push_back(std::move(sh));
                } else if (it.is("rectangle")) {
                    Shape sh;
                    sh.kind = Shape::Rect;
                    sh.pts = {fromLib(xy(it.child("start"))), fromLib(xy(it.child("end")))};
                    sh.fill = fillOf(it);
                    s->shapes.push_back(std::move(sh));
                } else if (it.is("circle")) {
                    Shape sh;
                    sh.kind = Shape::Circle;
                    sh.pts = {fromLib(xy(it.child("center")))};
                    if (const Sx* r = it.child("radius")) sh.r = r->num(1) / kGrid;
                    sh.fill = fillOf(it);
                    s->shapes.push_back(std::move(sh));
                } else if (it.is("arc")) {
                    const Sx *a = it.child("start"), *m = it.child("mid"), *b = it.child("end");
                    if (!a || !m || !b) continue;
                    Shape sh;
                    sh.pts = arcPoints(fromLib(xy(a)), fromLib(xy(m)), fromLib(xy(b)), 10);
                    s->shapes.push_back(std::move(sh));
                } else if (it.is("text")) {
                    const Sx* eff = it.child("effects");
                    if (eff && eff->flag("hide")) continue;
                    Shape sh;
                    sh.kind = Shape::Text;
                    sh.text = it.str(1);
                    const Pt at = fromLib(xy(it.child("at")));
                    double size = 1.27;
                    if (eff) {
                        if (const Sx* font = eff->child("font")) size = xy(font->child("size")).x;
                    }
                    sh.size = std::max(0.3, size / kGrid);
                    sh.pts = {{at.x, at.y + sh.size / 2}};
                    s->shapes.push_back(std::move(sh));
                } else if (it.is("pin")) {
                    PinDef pin;
                    pin.type = pinTypeOf(it.str(1));
                    const Sx* at = it.child("at");
                    const Pt pos = xy(at);
                    const double ang = at ? at->num(3) * kPi / 180 : 0;
                    const Sx* len = it.child("length");
                    const double l = len ? len->num(1) : 2.54;
                    pin.pos = fromLib(pos);
                    pin.end = fromLib({pos.x + l * std::cos(ang), pos.y + l * std::sin(ang)});
                    pin.number = it.value("number");
                    pin.name = it.value("name");
                    if (pin.name == "~") pin.name.clear();
                    const bool hidden = it.flag("hide");
                    if (hidden) {
                        pin.end = pin.pos;  // KiCad doesn't draw it
                        if (pin.type == PinType::PowerIn && !pin.name.empty()) info.hiddenPower.push_back(pin.name);
                    }
                    s->pins.push_back(std::move(pin));
                }
            }
        };
        addItems(*draw);
        for (const auto& sub : draw->list) {
            if (!sub.is("symbol")) continue;
            const std::string name = sub.str(1);
            int u = 0, st = 0;
            const auto p2 = name.rfind('_');
            const auto p1 = p2 == std::string::npos || p2 == 0 ? std::string::npos : name.rfind('_', p2 - 1);
            if (p1 != std::string::npos) {
                u = std::atoi(name.substr(p1 + 1, p2 - p1 - 1).c_str());
                st = std::atoi(name.substr(p2 + 1).c_str());
            }
            info.units = std::max(info.units, u);
            if ((u == 0 || u == unit) && (st == 0 || st == 1)) addItems(sub);
        }
        bool anyName = false;
        for (const auto& p : s->pins) anyName = anyName || (!p.name.empty() && p.name != p.number);
        s->showPinNames = s->showPinNames && anyName && !s->power;
        if (s->power) {
            s->showPinNumbers = false;
            info.hiddenPower.clear();
        }
        // a part we know (R, C, LED, Conn_01x04 ...) keeps KiCad's drawing but takes the built-in
        // name, so simulation and the 3D view recognise it; only when the pins agree
        if (!s->power) {
            const auto colon = key.find(':');
            int pins = 0;
            const std::string b = mapKicadPart(colon == std::string::npos ? key : key.substr(colon + 1), pins);
            auto bs = b.empty() || b == "power" || b == "gnd" ? nullptr : findSymbol(b, pins);
            if (bs && bs->pins.size() == s->pins.size()) {
                bool ok = true;
                for (const auto& bp : bs->pins) {
                    const PinDef* cp = nullptr;
                    for (const auto& q : s->pins) {
                        if (q.number == bp.number) cp = &q;
                    }
                    if (!cp) {
                        ok = false;
                        break;
                    }
                    const std::string bn = upperOf(bp.name.substr(0, bp.name.find('/'))), cn = upperOf(cp->name);
                    if (!(bn == cn || bn == bp.number || cn.empty() || cn == cp->number)) ok = false;
                }
                if (ok) {
                    s->name = bs->name;
                    info.pins = pins;
                    info.footprint = bs->footprint;
                }
            }
        }
        finishSymbol(*s);
        // KiCad often stacks reference and value as vertical text along a part; drawn level here
        // they would overlap, so they go beside the body instead
        if (!s->power && std::hypot(s->refPos.x - s->valuePos.x, s->refPos.y - s->valuePos.y) < 1.2) {
            const double mid = (s->bodyMinY + s->bodyMaxY) / 2;
            s->refPos = {s->bodyMaxX + 1.4, mid - 0.45};
            s->valuePos = {s->bodyMaxX + 1.4, mid + 0.45};
        }
        info.sym = s;
        return &(cache[ck] = std::move(info));
    }

    // pins of a part that land on a wire, label or no-connect mark
    static int pinHits(const Circuit& sheet, const Part& p, const Symbol& sym) {
        int hits = 0;
        for (const auto& pin : sym.pins) {
            if (near(pin.pos, pin.end)) continue;  // hidden
            const Pt at = toSheet(p, pin.pos);
            bool hit = false;
            for (const auto& w : sheet.wires) {
                for (std::size_t i = 1; i < w.pts.size() && !hit; ++i) hit = onSegment(at, w.pts[i - 1], w.pts[i]);
                if (hit) break;
            }
            for (const auto& l : sheet.labels) hit = hit || near(l.pos, at);
            for (const auto& n : sheet.noConnects) hit = hit || near(n, at);
            hits += hit;
        }
        return hits;
    }

    Circuit readSheet(const Sx& root, const std::filesystem::path& file, const std::string& path, const std::string& relPath,
                      const std::string& prefix, int depth) {
        Circuit c;
        std::map<std::string, const Sx*> libs;
        if (const Sx* ls = root.child("lib_symbols")) {
            for (const auto& s : ls->list) {
                if (s.is("symbol")) libs[s.str(1)] = &s;
            }
        }
        const std::string fileKey = file.string() + "#" + std::to_string(depth);
        for (const auto& n : root.list) {
            if (n.is("wire")) {
                Wire w;
                if (const Sx* pts = n.child("pts")) {
                    for (const auto& p : pts->list) {
                        if (p.is("xy")) w.pts.push_back({p.num(1) / kGrid, p.num(2) / kGrid});
                    }
                }
                if (w.pts.size() >= 2) c.wires.push_back(std::move(w));
            } else if (n.is("bus")) {
                // drawn as a bus; KiCad names it with a label on it, so it's named from the nearest bus label later
                Bus b;
                if (const Sx* pts = n.child("pts")) {
                    for (const auto& p : pts->list) {
                        if (p.is("xy")) b.pts.push_back({p.num(1) / kGrid, p.num(2) / kGrid});
                    }
                }
                if (b.pts.size() >= 2) c.buses.push_back(std::move(b));
            } else if (n.is("bus_entry")) {
                const Sx* at = n.child("at");
                const Sx* size = n.child("size");
                if (at && size && !c.buses.empty()) {
                    const Pt a{at->num(1) / kGrid, at->num(2) / kGrid};
                    c.buses.back().entries.push_back({a, {a.x + size->num(1) / kGrid, a.y + size->num(2) / kGrid}});
                }
            } else if (n.is("label") || n.is("global_label") || n.is("hierarchical_label")) {
                Label l;
                l.pos = {xy(n.child("at")).x / kGrid, xy(n.child("at")).y / kGrid};
                if (n.is("global_label")) {
                    l.name = n.str(1);
                    l.kind = "global";
                } else {
                    l.name = prefix + n.str(1);
                }
                c.labels.push_back(std::move(l));
            } else if (n.is("no_connect")) {
                const Pt p = xy(n.child("at"));
                c.noConnects.push_back({p.x / kGrid, p.y / kGrid});
            }
        }
        for (const auto& n : root.list) {
            if (!n.is("symbol")) continue;
            std::string key = n.value("lib_name");
            if (key.empty()) key = n.value("lib_id");
            const int unit = std::max(1, std::atoi(n.value("unit").c_str()));
            LibInfo* info = libSymbol(libs, fileKey, key, unit);
            if (!info) {
                warn("symbol " + key + " isn't in the file's lib_symbols; it was skipped");
                continue;
            }
            const Symbol& sym = *info->sym;
            Part p;
            p.customSymbol = info->sym;
            p.symbol = sym.name;
            p.pins = info->pins;
            const Sx* at = n.child("at");
            p.pos = {xy(at).x / kGrid, xy(at).y / kGrid};
            double angle = at ? at->num(3) : 0;
            const std::string mirror = n.value("mirror");
            if (mirror == "y") p.mirror = true;
            if (mirror == "x") p.mirror = true, angle += 180;
            if (std::abs(std::remainder(angle, 90.0)) > 0.5) warn(key + ": a symbol at " + num(angle) + " degrees was turned to a right angle");
            p.rot = snapAngle(angle);
            std::string ref;
            p.footprint = sym.footprint;
            for (const auto& prop : n.list) {
                if (!prop.is("property")) continue;
                const std::string name = prop.str(1), value = prop.str(2);
                if (name == "Reference") ref = value;
                else if (name == "Value") p.value = value;
                else if (name == "Footprint") p.footprint = value;
                else if (name == "Datasheet") {
                    if (!value.empty() && value != "~") p.fields["datasheet"] = value;
                } else if (name == "Description" || name.rfind("ki_", 0) == 0 || name.rfind("Sim.", 0) == 0) {
                } else if (!value.empty() && value != "~") {
                    p.fields[name] = value;
                }
            }
            int instUnit = unit;
            bool foundRef = false;
            if (const Sx* inst = n.child("instances")) {
                for (const auto& proj : inst->list) {
                    if (!proj.is("project")) continue;
                    for (const auto& ip : proj.list) {
                        if (ip.is("path") && ip.str(1) == path && !foundRef) {
                            ref = ip.value("reference");
                            if (const std::string u = ip.value("unit"); !u.empty()) instUnit = std::atoi(u.c_str());
                            foundRef = true;
                        }
                    }
                }
            }
            if (!foundRef) {
                if (auto it = kicad6Refs.find(relPath + "/" + n.value("uuid")); it != kicad6Refs.end()) {
                    ref = it->second.first;
                    if (it->second.second > 0) instUnit = it->second.second;
                }
            }
            p.ref = ref;
            p.unit = info->units > 1 ? instUnit : 0;
            if (n.value("dnp") == "yes") p.fields["dnp"] = "yes";
            if (p.footprint.empty() || p.footprint == "~") p.footprint = info->footprint;
            if (sym.power) {
                if (p.value.empty() && !sym.pins.empty()) p.value = sym.pins[0].name;
                p.footprint.clear();
            } else if (!p.footprint.empty()) {
                if (auto fp = findFootprint(p.footprint)) p.footprint = fp->name;
                else ++unknownFootprints;
            }
            // check the orientation against the wiring: try the others when pins miss it
            const int planned = pinHits(c, p, sym);
            int visible = 0;
            for (const auto& pin : sym.pins) visible += !near(pin.pos, pin.end);
            if (planned < visible) {
                Part best = p;
                int bestHits = planned;
                for (int m = 0; m < 2; ++m) {
                    for (int r = 0; r < 360; r += 90) {
                        Part t = p;
                        t.mirror = m == 1;
                        t.rot = r;
                        const int h = pinHits(c, t, sym);
                        if (h > bestHits) best = t, bestHits = h;
                    }
                }
                p = best;
            }
            if (!sym.power) {
                for (const auto& pin : sym.pins) {
                    if (std::find(info->hiddenPower.begin(), info->hiddenPower.end(), pin.name) != info->hiddenPower.end())
                        c.labels.push_back({pin.name, toSheet(p, pin.pos), "global"});
                }
            }
            c.parts.push_back(std::move(p));
        }
        // hierarchical sheets: the sheet's pins become labels here; the sub-sheet's hierarchical labels
        // carry the same names, and its contents go to the right of this sheet
        std::vector<Circuit> children;
        for (const auto& n : root.list) {
            if (!n.is("sheet")) continue;
            std::string name, sub;
            for (const auto& prop : n.list) {
                if (!prop.is("property")) continue;
                if (prop.str(1) == "Sheetname" || prop.str(1) == "Sheet name") name = prop.str(2);
                if (prop.str(1) == "Sheetfile" || prop.str(1) == "Sheet file") sub = prop.str(2);
            }
            const std::string childPrefix = prefix + name + "/";
            for (const auto& pin : n.list) {
                if (!pin.is("pin")) continue;
                const Pt at = xy(pin.child("at"));
                c.labels.push_back({childPrefix + pin.str(1), {at.x / kGrid, at.y / kGrid}, ""});
            }
            if (depth >= 8) {
                warn("sheets nest too deeply; " + name + " was skipped");
                continue;
            }
            const std::filesystem::path subPath = file.empty() ? std::filesystem::path(sub) : file.parent_path() / sub;
            std::ifstream in(subPath, std::ios::binary);
            if (sub.empty() || !in) {
                warn("sheet " + name + ": can't read " + subPath.string() + "; its contents are missing");
                continue;
            }
            std::stringstream ss;
            ss << in.rdbuf();
            try {
                const Sx childRoot = parseSx(ss.str());
                const std::string uuid = n.value("uuid");
                children.push_back(readSheet(childRoot, subPath, path + "/" + uuid, relPath + "/" + uuid, childPrefix, depth + 1));
                warn("hierarchical sheets were flattened onto one sheet (local labels are prefixed with the sheet name)");
            } catch (const std::exception& e) {
                warn("sheet " + name + ": " + e.what());
            }
        }
        for (auto& child : children) {
            if (child.parts.empty() && child.wires.empty()) continue;
            double x0, y0, x1, y1, cx0, cy0, cx1, cy1;
            sheetBounds(c, x0, y0, x1, y1);
            sheetBounds(child, cx0, cy0, cx1, cy1);
            if (x0 > x1) x0 = y0 = x1 = y1 = 0;
            const Pt d{std::round(x1 + 8 - cx0), std::round(y0 - cy0)};
            auto move = [&](Pt& p) { p = {p.x + d.x, p.y + d.y}; };
            for (auto& p : child.parts) move(p.pos);
            for (auto& w : child.wires) {
                for (auto& p : w.pts) move(p);
            }
            for (auto& l : child.labels) move(l.pos);
            for (auto& p : child.noConnects) move(p);
            for (auto& b : child.buses) {
                for (auto& p : b.pts) move(p);
                for (auto& [a, e] : b.entries) move(a), move(e);
                c.buses.push_back(std::move(b));
            }
            for (auto& p : child.parts) c.parts.push_back(std::move(p));
            for (auto& w : child.wires) c.wires.push_back(std::move(w));
            for (auto& l : child.labels) c.labels.push_back(std::move(l));
            for (auto& p : child.noConnects) c.noConnects.push_back(p);
        }
        nameBuses(c);
        return c;
    }

    // a bus is named by a label sitting on it (D[0..7]); that label names the bus, not a net
    static void nameBuses(Circuit& c) {
        for (auto& b : c.buses) {
            if (!b.name.empty()) continue;
            for (auto it = c.labels.begin(); it != c.labels.end(); ++it) {
                if (it->name.find_first_of("[{") == std::string::npos || busMembers(it->name).size() < 2) continue;
                bool on = false;
                for (std::size_t i = 1; i < b.pts.size() && !on; ++i) {
                    const Pt a = b.pts[i - 1], e = b.pts[i], q = it->pos;
                    const double cross = (e.x - a.x) * (q.y - a.y) - (e.y - a.y) * (q.x - a.x);
                    on = std::abs(cross) < 1e-6 && q.x >= std::min(a.x, e.x) - 1e-6 && q.x <= std::max(a.x, e.x) + 1e-6 &&
                         q.y >= std::min(a.y, e.y) - 1e-6 && q.y <= std::max(a.y, e.y) + 1e-6;
                }
                if (!on) continue;
                b.name = it->name;
                b.members = busMembers(b.name);
                c.labels.erase(it);
                break;
            }
            if (b.name.empty()) b.name = "BUS" + std::to_string(&b - c.buses.data() + 1);
        }
    }

    // unannotated (R?) and repeated references get fresh ones; multi-unit parts keep one footprint
    void fixRefs(Circuit& c) {
        std::set<std::pair<std::string, int>> seen;
        for (auto& p : c.parts) {
            const bool power = p.customSymbol && p.customSymbol->power;
            std::string pre = p.ref;
            while (!pre.empty() && (pre.back() == '?' || std::isdigit(static_cast<unsigned char>(pre.back())))) pre.pop_back();
            if (pre.empty()) pre = p.customSymbol ? p.customSymbol->prefix : "U";
            const bool unannotated = p.ref.empty() || p.ref.back() == '?';
            if (unannotated || (!power && !seen.insert({upperOf(p.ref), p.unit}).second)) {
                const std::string old = p.ref;
                p.ref = nextRef(c, pre);
                seen.insert({upperOf(p.ref), p.unit});
                if (!power) warn(old.empty() ? "a part had no reference; it became " + p.ref : old + " became " + p.ref + " (unannotated or repeated)");
            }
        }
        std::map<std::string, int> firstUnit;
        for (const auto& p : c.parts) {
            if (p.unit > 0) {
                auto [it, fresh] = firstUnit.emplace(p.ref, p.unit);
                if (!fresh) it->second = std::min(it->second, p.unit);
            }
        }
        for (auto& p : c.parts) {
            if (p.unit > 0 && p.unit != firstUnit[p.ref]) p.footprint.clear();
        }
        if (unknownFootprints)
            warn(std::to_string(unknownFootprints) +
                 " footprints aren't built in; import the board (.kicad_pcb) to bring KiCad's own footprints");
    }
};

// ---- writing ----------------------------------------------------------------------------------

std::string effects(double size = 1.27, bool hide = false, const char* justify = nullptr) {
    std::string s = "(effects (font (size " + num(size) + " " + num(size) + "))";
    if (justify) s += std::string(" (justify ") + justify + ")";
    if (hide) s += " hide";
    return s + ")";
}

std::string libXY(Pt p) { return num(p.x * kGrid) + " " + num(-p.y * kGrid); }

std::string libGraphics(const Symbol& s, const std::string& indent) {
    std::string o;
    const std::string stroke = "(stroke (width 0.254) (type default))";
    auto fill = [](bool f) { return std::string("(fill (type ") + (f ? "outline" : "none") + "))"; };
    for (const auto& sh : s.shapes) {
        if (sh.kind == Shape::Poly && sh.pts.size() >= 2) {
            o += indent + "(polyline (pts";
            for (const auto& p : sh.pts) o += " (xy " + libXY(p) + ")";
            if (sh.closed && !near(sh.pts.front(), sh.pts.back())) o += " (xy " + libXY(sh.pts.front()) + ")";
            o += ") " + stroke + " " + fill(sh.fill) + ")\n";
        } else if (sh.kind == Shape::Rect && sh.pts.size() >= 2) {
            o += indent + "(rectangle (start " + libXY(sh.pts[0]) + ") (end " + libXY(sh.pts[1]) + ") " + stroke + " " + fill(sh.fill) + ")\n";
        } else if (sh.kind == Shape::Circle && !sh.pts.empty()) {
            o += indent + "(circle (center " + libXY(sh.pts[0]) + ") (radius " + num(sh.r * kGrid) + ") " + stroke + " " + fill(sh.fill) + ")\n";
        } else if (sh.kind == Shape::Text && !sh.pts.empty()) {
            o += indent + "(text " + quote(sh.text) + " (at " + libXY({sh.pts[0].x, sh.pts[0].y - sh.size / 2}) + " 0) " + effects(sh.size * kGrid) + ")\n";
        }
    }
    return o;
}

std::string libPins(const Symbol& s, const std::string& indent, const std::string& powerName) {
    std::string o;
    for (const auto& p : s.pins) {
        const double dx = p.end.x - p.pos.x, dy = p.end.y - p.pos.y;
        const double len = std::hypot(dx, dy) * kGrid;
        int angle = len < 1e-6 ? (s.power ? 270 : 0) : snapAngle(std::atan2(-dy, dx) * 180 / kPi);
        const bool hide = s.power;
        const std::string name = !powerName.empty() ? powerName : p.name.empty() ? "~" : p.name;
        o += indent + "(pin " + (s.power ? "power_in" : kicadPinType(p.type)) + " line (at " + libXY(p.pos) + " " + std::to_string(angle) +
             ") (length " + num(len) + ")" + (hide ? " hide" : "") + "\n" + indent + "  (name " + quote(name) + " " + effects() + ")\n" +
             indent + "  (number " + quote(p.number) + " " + effects() + "))\n";
    }
    return o;
}

}  // namespace

Circuit fromKicadSchematic(const std::string& text, const std::string& path, std::vector<std::string>* warnings) {
    SchReader reader(warnings);
    return reader.read(text, path);
}

std::string toKicadSchematic(const Circuit& c) {
    const std::string rootUuid = uuidOf("sheet:" + c.title);
    double x0, y0, x1, y1;
    sheetBounds(c, x0, y0, x1, y1);
    if (x0 > x1) x0 = y0 = x1 = y1 = 0;
    // grid-aligned offset putting everything inside the page's border
    const Pt off{std::round(10 - x0), std::round(10 - y0)};
    const double wmm = (x1 - x0 + 20) * kGrid, hmm = (y1 - y0 + 20) * kGrid;
    const char* paper = "A4";
    if (wmm > 1189 || hmm > 841) paper = "A0";
    else if (wmm > 841 || hmm > 594) paper = "A0";
    else if (wmm > 594 || hmm > 420) paper = "A1";
    else if (wmm > 420 || hmm > 297) paper = "A2";
    else if (wmm > 297 || hmm > 210) paper = "A3";
    auto at = [&](Pt p) { return num((p.x + off.x) * kGrid) + " " + num((p.y + off.y) * kGrid); };

    // one library symbol per drawing (per distinct net name for power symbols)
    struct Lib {
        std::string id;
        std::map<int, const Symbol*> units;
        std::string powerName;
    };
    std::vector<Lib> libs;
    std::map<std::string, std::size_t> byKey;
    std::set<std::string> ids;
    auto libFor = [&](const Part& p, const Symbol& s) -> std::string {
        std::string key, base;
        if (s.power) {
            key = "power|" + p.value;
            base = "wetwaresuite:" + (p.value.empty() ? s.value : p.value);
        } else if (s.category == "imported" && !s.title.empty()) {
            key = "imported|" + s.title;
            base = s.title;
        } else if (p.customSymbol) {
            char b[32];
            std::snprintf(b, sizeof b, "%p", static_cast<const void*>(&s));
            key = b;
            base = "wetwaresuite:" + s.name;
        } else {
            key = "builtin|" + s.name + "|" + std::to_string(p.pins);
            for (const auto& name : p.pinNames) key += "|" + name;
            base = "wetwaresuite:" + s.name;
        }
        auto it = byKey.find(key);
        if (it == byKey.end()) {
            std::string id = base;
            for (int n = 2; ids.count(id); ++n) id = base + "_" + std::to_string(n);
            ids.insert(id);
            it = byKey.emplace(key, libs.size()).first;
            libs.push_back({id, {}, s.power ? (p.value.empty() ? s.value : p.value) : ""});
        }
        libs[it->second].units.emplace(std::max(1, p.unit), &s);
        return libs[it->second].id;
    };
    std::vector<std::string> partLib;
    for (const auto& p : c.parts) {
        auto s = symbolOf(p);
        partLib.push_back(s ? libFor(p, *s) : "");
    }

    std::ostringstream o;
    o << "(kicad_sch (version 20230121) (generator wetwaresuite)\n\n  (uuid " << rootUuid << ")\n\n  (paper \"" << paper << "\")\n\n";
    o << "  (title_block\n    (title " << quote(c.title) << ")\n  )\n\n  (lib_symbols\n";
    for (const auto& lib : libs) {
        const Symbol& first = *lib.units.begin()->second;
        std::string local = lib.id.substr(lib.id.find(':') == std::string::npos ? 0 : lib.id.find(':') + 1);
        o << "    (symbol " << quote(lib.id) << (first.power ? " (power)" : "") << (first.showPinNumbers ? "" : " (pin_numbers hide)")
          << " (pin_names (offset 0.508)" << (first.showPinNames ? "" : " hide") << ") (in_bom " << (first.inBom && !first.power ? "yes" : "no")
          << ") (on_board " << (first.power ? "no" : "yes") << ")\n";
        o << "      (property \"Reference\" " << quote(first.power ? "#PWR" : first.prefix) << " (at " << libXY(first.refPos) << " 0) "
          << effects(1.27, first.power) << ")\n";
        o << "      (property \"Value\" " << quote(first.power ? lib.powerName : first.value) << " (at " << libXY(first.valuePos) << " 0) "
          << effects() << ")\n";
        o << "      (property \"Footprint\" \"\" (at 0 0 0) " << effects(1.27, true) << ")\n";
        o << "      (property \"Datasheet\" \"~\" (at 0 0 0) " << effects(1.27, true) << ")\n";
        for (const auto& [unit, sym] : lib.units) {
            o << "      (symbol " << quote(local + "_" + std::to_string(unit) + "_1") << "\n";
            o << libGraphics(*sym, "        ") << libPins(*sym, "        ", lib.powerName);
            o << "      )\n";
        }
        o << "    )\n";
    }
    o << "  )\n\n";

    // wires, split wherever something touches them part-way so KiCad sees every connection
    std::vector<Pt> points;
    for (const auto& p : c.parts) {
        if (auto s = symbolOf(p)) {
            for (const auto& pin : s->pins) points.push_back(pinPos(p, pin));
        }
    }
    for (const auto& w : c.wires) points.insert(points.end(), w.pts.begin(), w.pts.end());
    for (const auto& l : c.labels) points.push_back(l.pos);
    std::vector<std::pair<Pt, Pt>> segs;
    for (const auto& w : c.wires) {
        for (std::size_t i = 1; i < w.pts.size(); ++i) {
            const Pt a = w.pts[i - 1], b = w.pts[i];
            std::vector<std::pair<double, Pt>> cuts;
            const double len2 = (b.x - a.x) * (b.x - a.x) + (b.y - a.y) * (b.y - a.y);
            if (len2 < 1e-9) continue;
            for (const auto& p : points) {
                if (near(p, a) || near(p, b) || !onSegment(p, a, b)) continue;
                cuts.push_back({((p.x - a.x) * (b.x - a.x) + (p.y - a.y) * (b.y - a.y)) / len2, p});
            }
            std::sort(cuts.begin(), cuts.end(), [](const auto& l, const auto& r) { return l.first < r.first; });
            Pt prev = a;
            for (const auto& [t, p] : cuts) {
                if (near(p, prev)) continue;
                segs.push_back({prev, p});
                prev = p;
            }
            if (!near(prev, b)) segs.push_back({prev, b});
        }
    }
    int n = 0;
    auto uuid = [&] { return uuidOf(rootUuid + "/" + std::to_string(n++)); };
    for (const auto& [a, b] : segs) {
        o << "  (wire (pts (xy " << at(a) << ") (xy " << at(b) << "))\n    (stroke (width 0) (type default))\n    (uuid " << uuid() << ")\n  )\n";
    }
    for (const auto& bus : c.buses) {
        for (std::size_t i = 1; i < bus.pts.size(); ++i)
            o << "  (bus (pts (xy " << at(bus.pts[i - 1]) << ") (xy " << at(bus.pts[i]) << "))\n    (stroke (width 0) (type default))\n    (uuid " << uuid() << ")\n  )\n";
        for (const auto& [a, e] : bus.entries)
            o << "  (polyline (pts (xy " << at(a) << ") (xy " << at(e) << "))\n    (stroke (width 0) (type default))\n    (uuid " << uuid() << ")\n  )\n";
        if (!bus.pts.empty())
            o << "  (label \"" << bus.name << "\" (at " << at(bus.pts.front()) << " 0)\n    (effects (font (size 1.27 1.27)) (justify left bottom))\n    (uuid " << uuid() << ")\n  )\n";
    }
    // junctions where three or more wire ends and pins meet
    std::vector<std::pair<Pt, int>> ends;
    auto count = [&](Pt p) {
        for (auto& [q, k] : ends) {
            if (near(p, q)) return void(++k);
        }
        ends.push_back({p, 1});
    };
    for (const auto& [a, b] : segs) count(a), count(b);
    for (const auto& p : c.parts) {
        if (auto s = symbolOf(p)) {
            for (const auto& pin : s->pins) {
                const Pt q = pinPos(p, pin);
                for (auto& [e, k] : ends) {
                    if (near(e, q)) ++k;
                }
            }
        }
    }
    for (const auto& [p, k] : ends) {
        if (k >= 3) o << "  (junction (at " << at(p) << ") (diameter 0) (color 0 0 0 0)\n    (uuid " << uuid() << ")\n  )\n";
    }
    for (const auto& p : c.noConnects) o << "  (no_connect (at " << at(p) << ") (uuid " << uuid() << "))\n";
    for (const auto& l : c.labels) {
        if (l.kind == "global") {
            o << "  (global_label " << quote(l.name) << " (shape bidirectional) (at " << at(l.pos) << " 0) (fields_autoplaced)\n    "
              << effects(1.27, false, "left") << "\n    (uuid " << uuid() << ")\n  )\n";
        } else {
            o << "  (label " << quote(l.name) << " (at " << at(l.pos) << " 0) (fields_autoplaced)\n    " << effects(1.27, false, "left bottom")
              << "\n    (uuid " << uuid() << ")\n  )\n";
        }
    }
    const std::string project = c.title.empty() ? "circuit" : c.title;
    for (std::size_t i = 0; i < c.parts.size(); ++i) {
        const Part& p = c.parts[i];
        auto s = symbolOf(p);
        if (!s) continue;
        const int unit = std::max(1, p.unit);
        const std::string symUuid = uuidOf("sym/" + p.ref + "/" + std::to_string(unit));
        const bool dnp = p.fields.count("dnp") > 0;
        o << "  (symbol (lib_id " << quote(partLib[i]) << ") (at " << at(p.pos) << " " << snapAngle(p.rot) << ")"
          << (p.mirror ? " (mirror y)" : "") << " (unit " << unit << ")\n    (in_bom " << (s->inBom && !s->power ? "yes" : "no")
          << ") (on_board " << (s->power ? "no" : "yes") << ") (dnp " << (dnp ? "yes" : "no") << ")\n    (uuid " << symUuid << ")\n";
        o << "    (property \"Reference\" " << quote(p.ref) << " (at " << at(toSheet(p, s->refPos)) << " 0) " << effects(1.27, s->power) << ")\n";
        o << "    (property \"Value\" " << quote(p.value.empty() ? s->value : p.value) << " (at " << at(toSheet(p, s->valuePos)) << " 0) "
          << effects() << ")\n";
        std::string fp = p.footprint;
        if (!fp.empty() && fp.find(':') == std::string::npos) fp = "wetwaresuite:" + fp;
        o << "    (property \"Footprint\" " << quote(fp) << " (at " << at(p.pos) << " 0) " << effects(1.27, true) << ")\n";
        auto ds = p.fields.find("datasheet");
        o << "    (property \"Datasheet\" " << quote(ds == p.fields.end() ? "~" : ds->second) << " (at " << at(p.pos) << " 0) " << effects(1.27, true)
          << ")\n";
        for (const auto& [k, v] : p.fields) {
            if (k == "datasheet" || k == "dnp") continue;
            o << "    (property " << quote(k) << " " << quote(v) << " (at " << at(p.pos) << " 0) " << effects(1.27, true) << ")\n";
        }
        for (const auto& pin : s->pins) o << "    (pin " << quote(pin.number) << " (uuid " << uuidOf(symUuid + "/" + pin.number) << "))\n";
        o << "    (instances\n      (project " << quote(project) << "\n        (path \"/" << rootUuid << "\" (reference " << quote(p.ref)
          << ") (unit " << unit << "))\n      )\n    )\n  )\n";
    }
    o << "\n  (sheet_instances\n    (path \"/\" (page \"1\"))\n  )\n)\n";
    return o.str();
}

}  // namespace wetwaresuite::circuit
