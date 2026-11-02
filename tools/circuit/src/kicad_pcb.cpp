// KiCad 6/7/8 boards (.kicad_pcb): reading footprints (as custom footprints), tracks, vias, zones and
// the Edge.Cuts outline, and writing them back out.
#include <algorithm>
#include <array>
#include <cctype>
#include <map>
#include <set>
#include <sstream>
#include <stdexcept>

#include "kicad_util.hpp"

namespace wetwaresuite::circuit {

using namespace kicad;

namespace {

bool isInt(const std::string& s) {
    return !s.empty() && std::all_of(s.begin(), s.end(), [](char c) { return std::isdigit(static_cast<unsigned char>(c)); });
}

int copperLayer(const std::string& name) {
    if (name == "F.Cu") return 0;
    if (name == "B.Cu") return 1;
    if (name == "In1.Cu") return 2;
    if (name == "In2.Cu") return 3;
    return -1;
}

// natural order: 2 before 10, A2 before A10
bool naturalLess(const std::string& a, const std::string& b) {
    std::size_t i = 0, j = 0;
    while (i < a.size() && j < b.size()) {
        if (std::isdigit(static_cast<unsigned char>(a[i])) && std::isdigit(static_cast<unsigned char>(b[j]))) {
            std::size_t i2 = i, j2 = j;
            while (i2 < a.size() && std::isdigit(static_cast<unsigned char>(a[i2]))) ++i2;
            while (j2 < b.size() && std::isdigit(static_cast<unsigned char>(b[j2]))) ++j2;
            const long long x = std::stoll(a.substr(i, std::min<std::size_t>(i2 - i, 15))), y = std::stoll(b.substr(j, std::min<std::size_t>(j2 - j, 15)));
            if (x != y) return x < y;
            i = i2, j = j2;
        } else {
            if (a[i] != b[j]) return a[i] < b[j];
            ++i, ++j;
        }
    }
    return a.size() - i < b.size() - j;
}

struct PadIn {
    std::string number, net, function;
};

struct FpIn {
    std::string ref, value;
    std::shared_ptr<const Footprint> fp;
    PcbPlace place;
    std::vector<PadIn> pads;
    bool dnp = false, noBom = false;
};

struct PcbIn {
    std::vector<FpIn> fps;
    Board board;
};

class PcbReader {
public:
    explicit PcbReader(std::vector<std::string>* warnings) { warn.out = warnings; }
    Warn warn;

    PcbIn read(const std::string& text) {
        const Sx root = parseSx(text);
        if (!root.is("kicad_pcb")) throw std::runtime_error("not a KiCad board (expected (kicad_pcb ...))");
        PcbIn in;
        int copper = 0;
        if (const Sx* layers = root.child("layers")) {
            for (const auto& l : layers->list) {
                const std::string name = l.str(1);
                if (name.size() > 3 && name.compare(name.size() - 3, 3, ".Cu") == 0) ++copper;
            }
        }
        if (copper > 4) warn("the board has " + std::to_string(copper) + " copper layers; only F.Cu, In1.Cu, In2.Cu and B.Cu were kept");
        in.board.layers = copper >= 4 ? 4 : 2;
        for (const auto& n : root.list) {
            if (n.is("net") && isInt(n.str(1))) nets[n.str(1)] = clean(n.str(2));
        }
        std::vector<std::pair<Pt, Pt>> edges;
        for (const auto& n : root.list) {
            if (n.is("footprint") || n.is("module")) {
                readFootprint(n, in);
            } else if (n.is("segment") || n.is("arc")) {
                const int layer = copperLayer(n.value("layer"));
                if (layer < 0 || layer >= in.board.layers) continue;
                const double w = n.child("width") ? n.child("width")->num(1) : 0.25;
                const std::string net = netOf(n);
                std::vector<Pt> pts{xy(n.child("start")), xy(n.child("end"))};
                if (n.is("arc") && n.child("mid")) pts = arcPoints(pts[0], xy(n.child("mid")), pts[1], 8);
                for (std::size_t i = 1; i < pts.size(); ++i) in.board.traces.push_back({pts[i - 1], pts[i], w, layer, net});
            } else if (n.is("via")) {
                if (n.flag("blind") || n.flag("micro")) warn("blind and micro vias became through vias");
                const Sx* size = n.child("size");
                const Sx* drill = n.child("drill");
                in.board.vias.push_back({xy(n.child("at")), drill ? drill->num(1) : 0.4, size ? size->num(1) : 0.8, netOf(n)});
            } else if (n.is("zone")) {
                readZone(n, in.board);
            } else if (n.is("gr_line") || n.is("gr_arc") || n.is("gr_rect") || n.is("gr_circle") || n.is("gr_poly")) {
                if (n.value("layer") != "Edge.Cuts") continue;
                const Pt a = xy(n.child("start")), b = xy(n.child("end"));
                std::vector<Pt> pts;
                if (n.is("gr_line")) pts = {a, b};
                else if (n.is("gr_arc") && n.child("mid")) pts = arcPoints(a, xy(n.child("mid")), b, 12);
                else if (n.is("gr_rect")) pts = {a, {b.x, a.y}, b, {a.x, b.y}, a};
                else if (n.is("gr_circle")) pts = circlePoints(xy(n.child("center")), std::hypot(b.x - xy(n.child("center")).x, b.y - xy(n.child("center")).y));
                else if (n.is("gr_poly")) {
                    if (const Sx* p = n.child("pts")) {
                        for (const auto& q : p->list) {
                            if (q.is("xy")) pts.push_back(xy(&q));
                        }
                    }
                    if (!pts.empty()) pts.push_back(pts.front());
                }
                for (std::size_t i = 1; i < pts.size(); ++i) edges.push_back({pts[i - 1], pts[i]});
            }
        }
        in.board.outline = chain(edges);
        {
            double x0 = 1e18, y0 = 1e18, x1 = -1e18, y1 = -1e18;
            for (const auto& p : in.board.outline) x0 = std::min(x0, p.x), y0 = std::min(y0, p.y), x1 = std::max(x1, p.x), y1 = std::max(y1, p.y);
            for (const auto& z : zoneBoxes) {
                if (std::abs(z[0] - x0) > 1 || std::abs(z[1] - y0) > 1 || std::abs(z[2] - x1) > 1 || std::abs(z[3] - y1) > 1)
                    warn("pours fill the whole board here: smaller KiCad zone outlines grew to the board's");
            }
        }
        // move everything so the board's corner sits at the origin
        double x0 = 1e18, y0 = 1e18;
        for (const auto& p : in.board.outline) x0 = std::min(x0, p.x), y0 = std::min(y0, p.y);
        if (in.board.outline.empty()) {
            for (const auto& f : in.fps) x0 = std::min(x0, f.place.pos.x - 5), y0 = std::min(y0, f.place.pos.y - 5);
        }
        if (x0 > 1e17) x0 = y0 = 0;
        auto move = [&](Pt& p) { p = {p.x - x0, p.y - y0}; };
        for (auto& p : in.board.outline) move(p);
        for (auto& t : in.board.traces) move(t.a), move(t.b);
        for (auto& v : in.board.vias) move(v.pos);
        for (auto& f : in.fps) move(f.place.pos);
        return in;
    }

    std::shared_ptr<const Footprint> readModule(const std::string& text) {
        const Sx root = parseSx(text);
        if (!root.is("footprint") && !root.is("module")) throw std::runtime_error("not a KiCad footprint (expected (footprint ...))");
        PcbIn in;
        readFootprint(root, in);
        return in.fps.front().fp;
    }

private:
    std::map<std::string, std::string> nets;
    std::vector<std::array<double, 4>> zoneBoxes;  // pour outlines' bounding boxes
    std::map<std::string, std::vector<std::shared_ptr<const Footprint>>> footprints;  // by name, deduplicated

    static std::string clean(std::string name) {
        while (!name.empty() && name[0] == '/') name.erase(0, 1);
        return name;
    }

    std::string netOf(const Sx& n) const {
        const Sx* net = n.child("net");
        if (!net) return "";
        const std::string a = net->str(1);
        if (net->list.size() > 2) return clean(net->str(2));
        if (isInt(a) && !net->list[1].quoted) {
            auto it = nets.find(a);
            return it == nets.end() ? "" : it->second;
        }
        return clean(a);
    }

    void readZone(const Sx& n, Board& board) {
        if (n.child("keepout")) return;
        std::string net = clean(n.value("net_name"));
        if (net.empty()) net = netOf(n);
        if (net.empty()) {
            warn("a pour with no net was skipped");
            return;
        }
        std::vector<int> layers;
        if (int l = copperLayer(n.value("layer")); l >= 0) layers.push_back(l);
        if (const Sx* ls = n.child("layers")) {
            for (std::size_t i = 1; i < ls->list.size(); ++i) {
                const std::string name = ls->str(i);
                if (name == "*.Cu" || name == "F&B.Cu") {
                    layers = {0, 1};
                    break;
                }
                if (int l = copperLayer(name); l >= 0) layers.push_back(l);
            }
        }
        for (int l : layers) {
            if (l >= board.layers) continue;
            Zone z;
            z.net = net;
            z.layer = l;
            if (const Sx* cp = n.child("connect_pads")) {
                if (const Sx* cl = cp->child("clearance")) z.clearance = cl->num(1);
                if (cp->str(1) == "yes") z.thermal = false;
                if (cp->str(1) == "no") warn("a pour that doesn't connect pads now joins them with thermal spokes");
            }
            if (const Sx* mt = n.child("min_thickness")) z.minWidth = mt->num(1);
            if (const Sx* fill = n.child("fill")) {
                if (const Sx* tb = fill->child("thermal_bridge_width")) z.spoke = tb->num(1);
            }
            board.zones.push_back(z);
        }
        if (const Sx* poly = n.child("polygon")) {
            if (const Sx* pts = poly->child("pts")) {
                double x0 = 1e18, y0 = 1e18, x1 = -1e18, y1 = -1e18;
                for (const auto& q : pts->list) {
                    if (!q.is("xy")) continue;
                    const Pt p = xy(&q);
                    x0 = std::min(x0, p.x), y0 = std::min(y0, p.y), x1 = std::max(x1, p.x), y1 = std::max(y1, p.y);
                }
                zoneBoxes.push_back({x0, y0, x1, y1});
            }
        }
    }

    void readFootprint(const Sx& n, PcbIn& in) {
        FpIn f;
        std::string name = n.str(1);
        if (auto colon = name.find(':'); colon != std::string::npos) name = name.substr(colon + 1);
        const bool bottom = n.value("layer") == "B.Cu";
        const Sx* at = n.child("at");
        const double angle = at ? at->num(3) : 0;
        f.place.placed = true;
        f.place.pos = xy(at);
        f.place.bottom = bottom;
        if (std::abs(std::remainder(angle, 90.0)) > 0.5) warn(name + ": footprints at odd angles were turned to the nearest right angle");
        const int rot = static_cast<int>(((std::lround(angle) % 360) + 360) % 360);  // any whole degree
        // KiCad stores a bottom footprint flipped top-to-bottom; here bottom parts mirror left-to-right,
        // which is the same flip turned half a turn
        f.place.rot = bottom ? (rot + 180) % 360 : rot;
        auto local = [&](Pt p) { return bottom ? Pt{p.x, -p.y} : p; };
        for (const auto& t : n.list) {
            if (t.is("fp_text") && t.str(1) == "reference") f.ref = t.str(2);
            if (t.is("fp_text") && t.str(1) == "value") f.value = t.str(2);
            if (t.is("property") && t.str(1) == "Reference") f.ref = t.str(2);
            if (t.is("property") && t.str(1) == "Value") f.value = t.str(2);
        }
        if (const Sx* attr = n.child("attr")) {
            f.dnp = attr->flag("dnp");
            f.noBom = attr->flag("exclude_from_bom") || attr->flag("board_only");
        }
        Footprint fp;
        fp.name = name;
        fp.description = n.value("descr");
        double cx0 = 1e18, cy0 = 1e18, cx1 = -1e18, cy1 = -1e18;
        for (const auto& t : n.list) {
            if (t.is("pad")) {
                PadDef pad;
                pad.number = t.str(1);
                const std::string type = t.str(2), shape = t.str(3);
                const Sx* pat = t.child("at");
                pad.pos = local(xy(pat));
                const Pt size = xy(t.child("size"));
                pad.w = size.x, pad.h = size.y;
                const double padAngle = pat ? pat->num(3) : 0;
                if (std::abs(std::remainder(padAngle - angle, 90.0)) > 0.5) warn(name + ": pads at odd angles were turned to the nearest right angle");
                if (snapAngle(padAngle - angle) % 180 == 90) std::swap(pad.w, pad.h);
                pad.shape = shape == "circle" ? PadShape::Circle : shape == "oval" ? PadShape::Oval : PadShape::Rect;
                if (type == "thru_hole" || type == "np_thru_hole") {
                    if (const Sx* d = t.child("drill")) {
                        for (std::size_t i = 1; i < d->list.size(); ++i) {
                            if (!d->list[i].isList && d->list[i].atom != "oval") {
                                pad.drill = d->num(i);
                                break;
                            }
                        }
                    }
                    if (pad.drill <= 0) pad.drill = std::min(pad.w, pad.h) * 0.6;
                    pad.plated = type == "thru_hole";
                }
                if (pad.number.empty() && pad.plated && pad.drill <= 0) continue;  // paste-only or mask-only shapes
                fp.pads.push_back(pad);
                if (!pad.number.empty()) f.pads.push_back({pad.number, netOf(t), t.value("pinfunction")});
            } else if (t.is("fp_line") || t.is("fp_rect") || t.is("fp_circle") || t.is("fp_arc") || t.is("fp_poly")) {
                const std::string layer = t.value("layer");
                const bool silk = layer == "F.SilkS" || layer == "B.SilkS" || layer == "F.Silkscreen" || layer == "B.Silkscreen";
                const bool court = layer == "F.CrtYd" || layer == "B.CrtYd" || layer == "F.Courtyard" || layer == "B.Courtyard";
                if (!silk && !court) continue;
                const Pt a = xy(t.child("start")), b = xy(t.child("end"));
                std::vector<Pt> pts;
                if (t.is("fp_line")) pts = {a, b};
                else if (t.is("fp_rect")) pts = {a, {b.x, a.y}, b, {a.x, b.y}, a};
                else if (t.is("fp_arc") && t.child("mid")) pts = arcPoints(a, xy(t.child("mid")), b, 8);
                else if (t.is("fp_circle")) {
                    const Pt c = xy(t.child("center"));
                    pts = circlePoints(c, std::hypot(b.x - c.x, b.y - c.y), 16);
                } else if (t.is("fp_poly")) {
                    if (const Sx* p = t.child("pts")) {
                        for (const auto& q : p->list) {
                            if (q.is("xy")) pts.push_back(xy(&q));
                        }
                    }
                    if (!pts.empty()) pts.push_back(pts.front());
                }
                for (auto& p : pts) p = local(p);
                if (court) {
                    for (const auto& p : pts) cx0 = std::min(cx0, p.x), cy0 = std::min(cy0, p.y), cx1 = std::max(cx1, p.x), cy1 = std::max(cy1, p.y);
                } else {
                    for (std::size_t i = 1; i < pts.size(); ++i) fp.silk.push_back({pts[i - 1], pts[i]});
                }
            }
        }
        finishFootprint(fp);
        if (cx0 <= cx1) fp.x0 = cx0, fp.y0 = cy0, fp.x1 = cx1, fp.y1 = cy1;
        // share one footprint between parts that use the same one
        auto& same = footprints[name];
        for (const auto& known : same) {
            if (known->pads.size() != fp.pads.size()) continue;
            bool eq = true;
            for (std::size_t i = 0; i < fp.pads.size() && eq; ++i) {
                const auto &a = known->pads[i], &b = fp.pads[i];
                eq = a.number == b.number && std::abs(a.pos.x - b.pos.x) < 1e-3 && std::abs(a.pos.y - b.pos.y) < 1e-3 && std::abs(a.w - b.w) < 1e-3 &&
                     std::abs(a.h - b.h) < 1e-3 && std::abs(a.drill - b.drill) < 1e-3;
            }
            if (eq) f.fp = known;
        }
        if (!f.fp) {
            f.fp = std::make_shared<const Footprint>(std::move(fp));
            same.push_back(f.fp);
        }
        in.fps.push_back(std::move(f));
    }

    // Edge.Cuts pieces joined end to end into the board outline (the biggest closed loop)
    std::vector<Pt> chain(std::vector<std::pair<Pt, Pt>> edges) {
        if (edges.empty()) return {};
        auto close = [](Pt a, Pt b) { return std::hypot(a.x - b.x, a.y - b.y) < 0.01; };
        std::vector<std::vector<Pt>> loops;
        double x0 = 1e18, y0 = 1e18, x1 = -1e18, y1 = -1e18;
        for (const auto& [a, b] : edges) {
            for (Pt p : {a, b}) x0 = std::min(x0, p.x), y0 = std::min(y0, p.y), x1 = std::max(x1, p.x), y1 = std::max(y1, p.y);
        }
        bool broken = false;
        while (!edges.empty()) {
            std::vector<Pt> loop{edges.front().first, edges.front().second};
            edges.erase(edges.begin());
            for (bool grew = true; grew && !close(loop.front(), loop.back());) {
                grew = false;
                for (std::size_t i = 0; i < edges.size(); ++i) {
                    if (close(edges[i].first, loop.back())) loop.push_back(edges[i].second);
                    else if (close(edges[i].second, loop.back())) loop.push_back(edges[i].first);
                    else continue;
                    edges.erase(edges.begin() + static_cast<long>(i));
                    grew = true;
                    break;
                }
            }
            if (!close(loop.front(), loop.back())) broken = true;
            else loop.pop_back();
            loops.push_back(std::move(loop));
        }
        auto area = [](const std::vector<Pt>& l) {
            double a = 0;
            for (std::size_t i = 0; i < l.size(); ++i) {
                const Pt p = l[i], q = l[(i + 1) % l.size()];
                a += p.x * q.y - q.x * p.y;
            }
            return std::abs(a) / 2;
        };
        if (broken) {
            warn("the Edge.Cuts outline isn't closed; the board became its bounding box");
            return {{x0, y0}, {x1, y0}, {x1, y1}, {x0, y1}};
        }
        std::sort(loops.begin(), loops.end(), [&](const auto& a, const auto& b) { return area(a) > area(b); });
        if (loops.size() > 1) warn("cut-outs inside the board outline were left out");
        return loops.front();
    }
};

std::string prefixOf(const std::string& ref) {
    std::string p;
    for (char ch : ref) {
        if (std::isdigit(static_cast<unsigned char>(ch))) break;
        p += ch;
    }
    return p.empty() ? "U" : p;
}

}  // namespace

Circuit fromKicadPcb(const std::string& text, std::vector<std::string>* warnings) {
    PcbReader reader(warnings);
    PcbIn in = reader.read(text);
    Circuit c;
    c.board = in.board;
    // parts with boxed symbols, laid out on a grid, each pin labelled with its pad's net
    std::map<std::string, std::shared_ptr<const Symbol>> symbols;
    double x = 0, y = 0, rowH = 0;
    std::set<std::string> refs;
    for (auto& f : in.fps) {
        if (f.ref.empty() || refs.count(f.ref)) {
            const std::string old = f.ref;
            f.ref = nextRef(c, prefixOf(f.ref));
            if (!old.empty()) reader.warn(old + " appears twice; the second became " + f.ref);
        }
        refs.insert(f.ref);
        std::vector<std::pair<std::string, std::string>> pins;
        std::set<std::string> seen;
        for (const auto& p : f.pads) {
            if (seen.insert(p.number).second) pins.push_back({p.number, p.function == "~" ? "" : p.function});
        }
        std::sort(pins.begin(), pins.end(), [](const auto& a, const auto& b) { return naturalLess(a.first, b.first); });
        std::string key = f.fp->name;
        for (const auto& [num, fn] : pins) key += "|" + num + ":" + fn;
        auto& sym = symbols[key];
        if (!sym) {
            Symbol s = makeBoxSymbol(f.fp->name, prefixOf(f.ref), f.value, pins);
            s.category = "imported";
            s.title = "";
            s.footprint = f.fp->name;
            s.showPinNumbers = true;
            if (pins.empty()) s.inBom = false;
            sym = std::make_shared<const Symbol>(std::move(s));
        }
        Part p;
        p.ref = f.ref;
        p.symbol = sym->name;
        p.customSymbol = sym;
        p.value = f.value;
        p.footprint = f.fp->name;
        p.customFootprint = f.fp;
        p.pcb = f.place;
        if (f.dnp || f.noBom) p.fields["dnp"] = "yes";
        const double w = std::ceil(sym->maxX - sym->minX) + 8, h = std::ceil(sym->maxY - sym->minY) + 4;
        if (x > 0 && x + w > 90) {
            x = 0;
            y += rowH;
            rowH = 0;
        }
        p.pos = {std::round(x - sym->minX + 4), std::round(y - sym->minY + 2)};
        x += w;
        rowH = std::max(rowH, h);
        c.parts.push_back(std::move(p));
    }
    for (std::size_t i = 0; i < in.fps.size(); ++i) {
        const Part& part = c.parts[i];
        const auto& sym = *part.customSymbol;
        for (const auto& pin : sym.pins) {
            std::string net;
            for (const auto& pad : in.fps[i].pads) {
                if (pad.number == pin.number && !pad.net.empty()) net = pad.net;
            }
            if (net.empty() || net.rfind("unconnected-", 0) == 0) c.noConnects.push_back(pinPos(part, pin));
            else labelPin(c, part, pin, net);
        }
    }
    return c;
}

std::shared_ptr<const Footprint> fromKicadMod(const std::string& text, std::vector<std::string>* warnings) {
    PcbReader reader(warnings);
    return reader.readModule(text);
}

std::string toKicadMod(const Footprint& fp) {
    auto xyS = [](Pt p) { return num(p.x) + " " + num(p.y); };
    bool smd = true;
    for (const auto& pad : fp.pads) smd = smd && pad.drill <= 0;
    std::ostringstream o;
    o << "(footprint " << quote(fp.name) << " (version 20221018) (generator wetwaresuite)\n  (layer \"F.Cu\")\n";
    if (!fp.description.empty()) o << "  (descr " << quote(fp.description) << ")\n";
    o << "  (attr " << (smd ? "smd" : "through_hole") << ")\n";
    o << "  (fp_text reference \"REF**\" (at 0 " << num(fp.y0 - 0.8) << ") (layer \"F.SilkS\")\n    (effects (font (size 1 1) (thickness 0.15)))\n  )\n";
    o << "  (fp_text value " << quote(fp.name) << " (at 0 " << num(fp.y1 + 0.8) << ") (layer \"F.Fab\")\n    (effects (font (size 1 1) (thickness 0.15)))\n  )\n";
    for (const auto& [a, b] : fp.silk)
        o << "  (fp_line (start " << xyS(a) << ") (end " << xyS(b) << ") (stroke (width 0.12) (type solid)) (layer \"F.SilkS\"))\n";
    o << "  (fp_rect (start " << xyS({fp.x0, fp.y0}) << ") (end " << xyS({fp.x1, fp.y1}) << ") (stroke (width 0.05) (type solid)) (fill none) (layer \"F.CrtYd\"))\n";
    for (const auto& pad : fp.pads) {
        const char* shape = pad.shape == PadShape::Circle ? "circle" : pad.shape == PadShape::Oval ? "oval" : "rect";
        const bool tht = pad.drill > 0;
        o << "  (pad " << quote(pad.plated ? pad.number : "") << " " << (tht ? (pad.plated ? "thru_hole" : "np_thru_hole") : "smd") << " " << shape
          << " (at " << xyS(pad.pos) << ") (size " << num(pad.w) << " " << num(pad.h) << ")";
        if (tht) o << " (drill " << num(pad.drill) << ") (layers \"*.Cu\" \"*.Mask\")";
        else o << " (layers \"F.Cu\" \"F.Paste\" \"F.Mask\")";
        o << ")\n";
    }
    o << ")\n";
    return o.str();
}

void mergeKicadPcb(Circuit& c, const std::string& text, std::vector<std::string>* warnings) {
    PcbReader reader(warnings);
    PcbIn in = reader.read(text);
    const Netlist nl = buildNetlist(c);
    // KiCad's net names -> ours, through the pads they reach
    std::map<std::string, std::string> netMap;
    int placed = 0;
    std::set<std::string> matched;
    for (const auto& f : in.fps) {
        Part* part = nullptr;
        for (auto& p : c.parts) {
            if (p.ref == f.ref && (p.unit <= 1 || !p.footprint.empty()) && !(p.customSymbol && p.customSymbol->power)) {
                part = &p;
                break;
            }
        }
        if (!part) {
            if (!f.ref.empty()) reader.warn(f.ref + " is on the board but not in the schematic; it was left out");
            continue;
        }
        matched.insert(f.ref);
        part->pcb = f.place;
        part->footprint = f.fp->name;
        part->customFootprint = f.fp;
        ++placed;
        for (const auto& pad : f.pads) {
            if (pad.net.empty()) continue;
            const std::string ours = nl.netOf(f.ref, pad.number);
            if (ours.empty()) continue;
            auto [it, fresh] = netMap.emplace(pad.net, ours);
            if (!fresh && it->second != ours) reader.warn("board net " + pad.net + " joins schematic nets " + it->second + " and " + ours);
        }
    }
    auto mapNet = [&](const std::string& n) {
        auto it = netMap.find(n);
        return it == netMap.end() ? n : it->second;
    };
    for (auto& t : in.board.traces) t.net = mapNet(t.net);
    for (auto& v : in.board.vias) v.net = mapNet(v.net);
    for (auto& z : in.board.zones) z.net = mapNet(z.net);
    const Rules rules = c.board.rules;
    c.board = in.board;
    c.board.rules = rules;
    int missing = 0;
    for (const auto& p : c.parts) {
        auto sym = symbolOf(p);
        if (sym && !sym->power && !p.footprint.empty() && !matched.count(p.ref)) ++missing;
    }
    if (missing) reader.warn(std::to_string(missing) + " parts of the schematic aren't on the board yet (circuit.place_footprints places them)");
    (void)placed;
}

std::string toKicadPcb(const Circuit& c) {
    const Netlist nl = buildNetlist(c);
    std::map<std::string, int> netIds;
    std::vector<std::string> netNames{""};
    auto netId = [&](const std::string& n) {
        if (n.empty()) return 0;
        auto [it, fresh] = netIds.emplace(n, static_cast<int>(netNames.size()));
        if (fresh) netNames.push_back(n);
        return it->second;
    };
    // a pin alone on an unnamed net is unconnected: no net, as KiCad writes them
    auto padNet = [&](const std::string& ref, const std::string& pad) {
        const std::string n = nl.netOf(ref, pad);
        const Net* net = nl.net(n);
        return net && net->nodes.size() < 2 && !net->named ? std::string() : n;
    };
    for (const auto& p : c.parts) {
        if (!p.pcb.placed) continue;
        if (auto fp = footprintOf(p)) {
            for (const auto& pad : fp->pads) netId(padNet(p.ref, pad.number));
        }
    }
    for (const auto& t : c.board.traces) netId(t.net);
    for (const auto& v : c.board.vias) netId(v.net);
    for (const auto& z : c.board.zones) netId(z.net);
    const bool four = c.board.layers == 4;
    auto xyS = [](Pt p) { return num(p.x) + " " + num(p.y); };
    auto netS = [&](const std::string& n) {
        const int id = netId(n);
        return "(net " + std::to_string(id) + " " + quote(netNames[static_cast<std::size_t>(id)]) + ")";
    };

    std::ostringstream o;
    o << "(kicad_pcb (version 20221018) (generator wetwaresuite)\n\n  (general\n    (thickness 1.6)\n  )\n\n  (paper \"A4\")\n";
    if (!c.title.empty()) o << "  (title_block\n    (title " << quote(c.title) << ")\n  )\n";
    o << "\n  (layers\n    (0 \"F.Cu\" signal)\n";
    if (four) o << "    (1 \"In1.Cu\" power)\n    (2 \"In2.Cu\" power)\n";
    o << "    (31 \"B.Cu\" signal)\n    (32 \"B.Adhes\" user \"B.Adhesive\")\n    (33 \"F.Adhes\" user \"F.Adhesive\")\n"
         "    (34 \"B.Paste\" user)\n    (35 \"F.Paste\" user)\n    (36 \"B.SilkS\" user \"B.Silkscreen\")\n    (37 \"F.SilkS\" user \"F.Silkscreen\")\n"
         "    (38 \"B.Mask\" user)\n    (39 \"F.Mask\" user)\n    (40 \"Dwgs.User\" user \"User.Drawings\")\n    (44 \"Edge.Cuts\" user)\n"
         "    (45 \"Margin\" user)\n    (46 \"B.CrtYd\" user \"B.Courtyard\")\n    (47 \"F.CrtYd\" user \"F.Courtyard\")\n    (48 \"B.Fab\" user)\n"
         "    (49 \"F.Fab\" user)\n  )\n\n";
    o << "  (setup\n    (pad_to_mask_clearance 0)\n    (pcbplotparams (outputdirectory \"fab/\"))\n  )\n\n";
    for (std::size_t i = 0; i < netNames.size(); ++i) o << "  (net " << i << " " << quote(netNames[i]) << ")\n";
    o << "\n";
    for (const auto& p : c.parts) {
        if (!p.pcb.placed) continue;
        auto fp = footprintOf(p);
        if (!fp) continue;
        const bool bottom = p.pcb.bottom;
        const int rot = bottom ? (p.pcb.rot + 180) % 360 : ((p.pcb.rot % 360) + 360) % 360;
        auto local = [&](Pt q) { return bottom ? Pt{q.x, -q.y} : q; };
        const char* side = bottom ? "B." : "F.";
        std::string lib = p.footprint.find(':') == std::string::npos ? "wetwaresuite:" + fp->name : p.footprint;
        o << "  (footprint " << quote(lib) << " (layer \"" << side << "Cu\")\n    (tstamp " << uuidOf("fp/" + p.ref) << ")\n    (at "
          << xyS(p.pcb.pos) << (rot ? " " + std::to_string(rot) : "") << ")\n";
        if (!fp->description.empty()) o << "    (descr " << quote(fp->description) << ")\n";
        o << "    (path \"/" << uuidOf("sym/" + p.ref + "/" + std::to_string(std::max(1, p.unit))) << "\")\n";
        bool smd = true;
        for (const auto& pad : fp->pads) smd = smd && pad.drill <= 0;
        auto sym = symbolOf(p);
        o << "    (attr " << (smd ? "smd" : "through_hole") << ((sym && !sym->inBom) || p.fields.count("dnp") ? " exclude_from_bom" : "") << ")\n";
        const double ty = local({0, fp->y0 - 0.8}).y, vy = local({0, fp->y1 + 0.8}).y;
        o << "    (fp_text reference " << quote(p.ref) << " (at 0 " << num(ty) << (rot ? " " + std::to_string(rot) : "") << ") (layer \"" << side
          << "SilkS\")\n      (effects (font (size 1 1) (thickness 0.15))" << (bottom ? " (justify mirror)" : "") << ")\n    )\n";
        o << "    (fp_text value " << quote(p.value) << " (at 0 " << num(vy) << (rot ? " " + std::to_string(rot) : "") << ") (layer \"" << side
          << "Fab\")\n      (effects (font (size 1 1) (thickness 0.15))" << (bottom ? " (justify mirror)" : "") << ")\n    )\n";
        for (const auto& [a, b] : fp->silk) {
            o << "    (fp_line (start " << xyS(local(a)) << ") (end " << xyS(local(b)) << ")\n      (stroke (width 0.12) (type solid)) (layer \""
              << side << "SilkS\"))\n";
        }
        o << "    (fp_rect (start " << xyS(local({fp->x0, fp->y0})) << ") (end " << xyS(local({fp->x1, fp->y1}))
          << ")\n      (stroke (width 0.05) (type solid)) (fill none) (layer \"" << side << "CrtYd\"))\n";
        for (const auto& pad : fp->pads) {
            const char* shape = pad.shape == PadShape::Circle ? "circle" : pad.shape == PadShape::Oval ? "oval" : "rect";
            const bool tht = pad.drill > 0;
            const char* type = tht ? (pad.plated ? "thru_hole" : "np_thru_hole") : "smd";
            o << "    (pad " << quote(pad.plated ? pad.number : "") << " " << type << " " << shape << " (at " << xyS(local(pad.pos))
              << (rot ? " " + std::to_string(rot) : "") << ") (size " << num(pad.w) << " " << num(pad.h) << ")";
            if (tht) o << " (drill " << num(pad.drill) << ")";
            if (tht) o << " (layers \"*.Cu\" \"*.Mask\")";
            else o << " (layers \"" << side << "Cu\" \"" << side << "Paste\" \"" << side << "Mask\")";
            if (pad.plated) {
                const std::string net = padNet(p.ref, pad.number);
                if (!net.empty()) o << " " << netS(net);
                if (sym) {
                    for (const auto& pin : sym->pins) {
                        if (pin.number == pad.number && !pin.name.empty() && pin.name != pin.number) o << " (pinfunction " << quote(pin.name) << ")";
                    }
                }
            }
            o << ")\n";
        }
        o << "  )\n\n";
    }
    const auto& outline = c.board.outline;
    for (std::size_t i = 0; i < outline.size() && outline.size() >= 3; ++i) {
        o << "  (gr_line (start " << xyS(outline[i]) << ") (end " << xyS(outline[(i + 1) % outline.size()])
          << ") (stroke (width 0.1) (type default)) (layer \"Edge.Cuts\"))\n";
    }
    for (const auto& t : c.board.traces) {
        o << "  (segment (start " << xyS(t.a) << ") (end " << xyS(t.b) << ") (width " << num(t.width) << ") (layer \"" << layerName(t.layer) << "\") (net "
          << netId(t.net) << "))\n";
    }
    for (const auto& v : c.board.vias) {
        o << "  (via (at " << xyS(v.pos) << ") (size " << num(v.diameter) << ") (drill " << num(v.drill) << ") (layers \"F.Cu\" \"B.Cu\") (net "
          << netId(v.net) << "))\n";
    }
    for (const auto& z : c.board.zones) {
        if (outline.size() < 3) break;
        o << "  (zone " << netS(z.net) << " (net_name " << quote(z.net) << ") (layer \"" << layerName(z.layer) << "\") (hatch edge 0.5)\n";
        o << "    (connect_pads" << (z.thermal ? "" : " yes") << " (clearance " << num(z.clearance) << "))\n";
        o << "    (min_thickness " << num(z.minWidth) << ") (filled_areas_thickness no)\n";
        o << "    (fill yes (thermal_gap " << num(z.clearance) << ") (thermal_bridge_width " << num(z.spoke) << "))\n";
        o << "    (polygon\n      (pts";
        for (const auto& p : outline) o << " (xy " << xyS(p) << ")";
        o << ")\n    )\n  )\n";
    }
    o << ")\n";
    return o.str();
}

}  // namespace wetwaresuite::circuit
