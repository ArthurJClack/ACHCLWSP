// Fabrication output: Gerber RS-274X with X2 file attributes, and an Excellon drill file.
// Board y runs down; Gerber y runs up, so every y is negated (the picture is unchanged).
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <map>
#include <sstream>

#include "wetwaresuite/circuit/circuit.hpp"
#include "geom.hpp"

namespace wetwaresuite::circuit {

namespace {

std::string coord(double v) {
    char b[32];
    std::snprintf(b, sizeof b, "%lld", static_cast<long long>(std::llround(v * 1e6)));
    return b;
}

std::string dec(double v) {
    char b[32];
    std::snprintf(b, sizeof b, "%.6f", v);
    return b;
}

class Gerber {
public:
    Gerber(const std::string& function, const std::string& polarityNote = "Positive") {
        head_ << "%TF.GenerationSoftware,wetwaresuite,circuit,1.0*%\n";
        head_ << "%TF.FileFunction," << function << "*%\n";
        head_ << "%TF.FilePolarity," << polarityNote << "*%\n";
        head_ << "%FSLAX46Y46*%\n%MOMM*%\n%LPD*%\nG01*\n";
    }
    int aperture(const std::string& def) {
        auto it = apertures_.find(def);
        if (it != apertures_.end()) return it->second;
        const int code = 10 + static_cast<int>(apertures_.size());
        apertures_[def] = code;
        order_.push_back(def);
        return code;
    }
    void use(int code) {
        if (code != current_) {
            body_ << "D" << code << "*\n";
            current_ = code;
        }
    }
    void line(Pt a, Pt b, double width) {
        use(aperture("C," + dec(width)));
        body_ << "X" << coord(a.x) << "Y" << coord(-a.y) << "D02*\n";
        body_ << "X" << coord(b.x) << "Y" << coord(-b.y) << "D01*\n";
    }
    void flash(Pt p, const std::string& def) {
        use(aperture(def));
        body_ << "X" << coord(p.x) << "Y" << coord(-p.y) << "D03*\n";
    }
    void pad(const WorldPad& p, double grow) {
        if (p.angle != 0 && p.shape == PadShape::Rect) {  // a turned pad: its outline as a region
            region(padOutline(p, grow));
            return;
        }
        if (p.angle != 0 && p.shape == PadShape::Oval && std::abs(p.rw - p.rh) > 1e-9) {
            region(padOutline(p, grow));
            return;
        }
        const double w = p.w + 2 * grow, h = p.h + 2 * grow;
        if (p.shape == PadShape::Circle) flash(p.pos, "C," + dec(std::min(w, h)));
        else if (p.shape == PadShape::Oval) flash(p.pos, std::abs(w - h) < 1e-9 ? "C," + dec(w) : "O," + dec(w) + "X" + dec(h));
        else flash(p.pos, "R," + dec(w) + "X" + dec(h));
    }
    void region(const std::vector<Pt>& pts) {
        if (pts.size() < 3) return;
        body_ << "G36*\n";
        body_ << "X" << coord(pts[0].x) << "Y" << coord(-pts[0].y) << "D02*\n";
        for (std::size_t i = 1; i < pts.size(); ++i) body_ << "X" << coord(pts[i].x) << "Y" << coord(-pts[i].y) << "D01*\n";
        body_ << "X" << coord(pts[0].x) << "Y" << coord(-pts[0].y) << "D01*\n";
        body_ << "G37*\n";
    }
    void rect(double x0, double y0, double x1, double y1) {
        body_ << "G36*\n";
        body_ << "X" << coord(x0) << "Y" << coord(-y0) << "D02*\n";
        body_ << "X" << coord(x1) << "Y" << coord(-y0) << "D01*\n";
        body_ << "X" << coord(x1) << "Y" << coord(-y1) << "D01*\n";
        body_ << "X" << coord(x0) << "Y" << coord(-y1) << "D01*\n";
        body_ << "X" << coord(x0) << "Y" << coord(-y0) << "D01*\n";
        body_ << "G37*\n";
    }
    std::string str() const {
        std::ostringstream o;
        o << head_.str();
        for (const auto& def : order_) o << "%ADD" << apertures_.at(def) << def << "*%\n";
        o << body_.str() << "M02*\n";
        return o.str();
    }

private:
    std::ostringstream head_, body_;
    std::map<std::string, int> apertures_;
    std::vector<std::string> order_;
    int current_ = -1;
};

void zoneRegions(Gerber& g, const ZoneFill& f) {
    // horizontal runs merged into rectangles while they repeat on following rows
    std::map<std::pair<int, int>, int> open;  // (xa, xb) -> start row
    auto emit = [&](int xa, int xb, int ya, int yb) {
        g.rect(f.x0 + xa * f.cell, f.y0 + ya * f.cell, f.x0 + (xb + 1) * f.cell, f.y0 + (yb + 1) * f.cell);
    };
    for (int y = 0; y <= f.ny; ++y) {
        std::map<std::pair<int, int>, int> next;
        if (y < f.ny) {
            int x = 0;
            while (x < f.nx) {
                if (!f.at(x, y)) {
                    ++x;
                    continue;
                }
                const int xa = x;
                while (x < f.nx && f.at(x, y)) ++x;
                const std::pair<int, int> k{xa, x - 1};
                auto it = open.find(k);
                next[k] = it != open.end() ? it->second : y;
                if (it != open.end()) open.erase(it);
            }
        }
        for (const auto& [k, start] : open) emit(k.first, k.second, start, y - 1);
        open = std::move(next);
    }
}

}  // namespace

std::vector<OutFile> fabFiles(const Circuit& c, const Netlist& nl, const std::string& base) {
    std::vector<OutFile> out;
    const auto pads = worldPads(c, nl);
    const auto fills = fillZones(c, nl);
    const double mask = 0.05;
    const int nLayers = c.board.layers == 4 ? 4 : 2;
    // copper, top to bottom: F, In1, In2, B
    std::vector<int> stack = {0};
    if (nLayers == 4) stack.push_back(2), stack.push_back(3);
    stack.push_back(1);
    for (std::size_t k = 0; k < stack.size(); ++k) {
        const int layer = stack[k];
        const int bit = layerBit(layer);
        const std::string pos = layer == 0 ? "Top" : layer == 1 ? "Bot" : "Inr";
        Gerber cu("Copper,L" + std::to_string(k + 1) + "," + pos);
        for (const auto& f : fills) {
            if (f.layer == layer) zoneRegions(cu, f);
        }
        for (const auto& t : c.board.traces) {
            if (t.layer == layer) cu.line(t.a, t.b, t.width);
        }
        for (const auto& p : pads) {
            if ((p.layers & bit) && p.plated) cu.pad(p, 0);
        }
        for (const auto& v : c.board.vias) cu.flash(v.pos, "C," + dec(v.diameter));
        std::string name = layer == 0 ? "-F_Cu.gtl" : layer == 1 ? "-B_Cu.gbl" : layer == 2 ? "-In1_Cu.g2" : "-In2_Cu.g3";
        out.push_back({base + name, cu.str()});
    }
    for (int layer = 0; layer < 2; ++layer) {
        const int bit = layerBit(layer);
        const std::string side = layer == 0 ? "Top" : "Bot";
        // solder mask openings
        Gerber sm(std::string("Soldermask,") + side, "Negative");
        for (const auto& p : pads) {
            if (p.layers & bit) sm.pad(p, mask);
        }
        out.push_back({base + (layer == 0 ? "-F_Mask.gts" : "-B_Mask.gbs"), sm.str()});
        // paste (SMD pads only)
        Gerber sp(std::string("Paste,") + side);
        for (const auto& p : pads) {
            if (p.drill <= 0 && (p.layers & bit)) sp.pad(p, 0);
        }
        out.push_back({base + (layer == 0 ? "-F_Paste.gtp" : "-B_Paste.gbp"), sp.str()});
        // silkscreen
        Gerber sk(std::string("Legend,") + side);
        for (const auto& part : c.parts) {
            if (!part.pcb.placed || part.pcb.bottom != (layer == 1)) continue;
            auto fp = footprintOf(part);
            if (!fp) continue;
            for (const auto& [a, b] : fp->silk) sk.line(toBoard(part.pcb, a), toBoard(part.pcb, b), 0.15);
            const double h = 1.0;
            const double w = strokeTextWidth(part.ref, h);
            for (const auto& stroke : strokeText(part.ref, -w / 2, fp->y0 - 0.3, h)) {
                for (std::size_t i = 1; i < stroke.size(); ++i) sk.line(toBoard(part.pcb, stroke[i - 1]), toBoard(part.pcb, stroke[i]), 0.15);
            }
        }
        out.push_back({base + (layer == 0 ? "-F_Silkscreen.gto" : "-B_Silkscreen.gbo"), sk.str()});
    }
    Gerber edge("Profile,NP");
    const auto& poly = c.board.outline;
    for (std::size_t i = 0; i < poly.size(); ++i) edge.line(poly[i], poly[(i + 1) % poly.size()], 0.1);
    out.push_back({base + "-Edge_Cuts.gm1", edge.str()});

    // Excellon drill files: plated holes (pads and vias), and non-plated holes when there are any
    auto drillFile = [&](bool plated) {
        std::map<long long, std::vector<Pt>> byTool;  // drill in microns -> holes
        for (const auto& p : pads) {
            if (p.drill > 0 && p.plated == plated) byTool[std::llround(p.drill * 1000)].push_back(p.pos);
        }
        if (plated) {
            for (const auto& v : c.board.vias) byTool[std::llround(v.drill * 1000)].push_back(v.pos);
        }
        std::ostringstream d;
        d << "M48\n; DRILL file {wetwaresuite circuit} " << (plated ? "plated" : "non-plated") << " through holes\n"
          << "; FORMAT={-:-/ absolute / metric / decimal}\n; #@! TF.FileFunction," << (plated ? "Plated,1," : "NonPlated,1,")
          << nLayers << (plated ? ",PTH" : ",NPTH") << "\nFMAT,2\nMETRIC\n";
        int tool = 1;
        std::map<long long, int> toolOf;
        for (const auto& [size, holes] : byTool) {
            toolOf[size] = tool;
            char b[64];
            std::snprintf(b, sizeof b, "T%dC%.3f\n", tool++, static_cast<double>(size) / 1000.0);
            d << b;
        }
        d << "%\nG90\nG05\n";
        for (const auto& [size, holes] : byTool) {
            d << "T" << toolOf[size] << "\n";
            for (const auto& h : holes) {
                char b[64];
                std::snprintf(b, sizeof b, "X%.3fY%.3f\n", h.x, -h.y);
                d << b;
            }
        }
        d << "M30\n";
        return std::make_pair(d.str(), byTool.size());
    };
    out.push_back({base + ".drl", drillFile(true).first});
    auto npth = drillFile(false);
    if (npth.second > 0) out.push_back({base + "-NPTH.drl", npth.first});
    return out;
}

std::string placementCsv(const Circuit& c, bool bottomLeftOrigin) {
    double x0 = 0, y0 = 0, x1 = 0, y1 = 0;
    boardBounds(c.board, x0, y0, x1, y1);
    std::ostringstream o;
    o << "Designator,Val,Package,Mid X,Mid Y,Rotation,Layer\n";
    std::vector<const Part*> parts;
    for (const auto& p : c.parts) {
        if (!p.pcb.placed || !footprintOf(p)) continue;
        auto sym = symbolOf(p);
        if ((sym && !sym->inBom) || p.fields.count("dnp")) continue;
        parts.push_back(&p);
    }
    std::sort(parts.begin(), parts.end(), [](const Part* a, const Part* b) {
        auto split = [](const std::string& r) {
            std::size_t i = 0;
            while (i < r.size() && !std::isdigit(static_cast<unsigned char>(r[i]))) ++i;
            return std::make_pair(r.substr(0, i), i < r.size() ? std::atol(r.c_str() + i) : 0L);
        };
        return split(a->ref) < split(b->ref);
    });
    auto field = [](const std::string& v) {
        if (v.find_first_of(",\"") == std::string::npos) return v;
        std::string q = "\"";
        for (char ch : v) q += ch == '"' ? std::string("\"\"") : std::string(1, ch);
        return q + "\"";
    };
    for (const Part* p : parts) {
        const double x = bottomLeftOrigin ? p->pcb.pos.x - x0 : p->pcb.pos.x;
        const double y = bottomLeftOrigin ? y1 - p->pcb.pos.y : -p->pcb.pos.y;
        char b[160];
        std::snprintf(b, sizeof b, ",%.4fmm,%.4fmm,%d,%s\n", x, y, ((p->pcb.rot % 360) + 360) % 360, p->pcb.bottom ? "Bottom" : "Top");
        o << field(p->ref) << "," << field(p->value) << "," << field(footprintOf(*p)->name) << b;
    }
    return o.str();
}

}  // namespace wetwaresuite::circuit
