// circuit.footprint_* : the footprint editor. Make a footprint from datasheet numbers, move, resize,
// add and remove its pads, and keep it in your own library (a folder of KiCad .kicad_mod files).
#include "cmd_common.hpp"

namespace wetwaresuite::circuit::cmd {

namespace {

PadShape shapeOf(const std::string& s) {
    const std::string l = lower(s);
    if (l == "circle" || l == "round") return PadShape::Circle;
    if (l == "oval" || l == "oblong") return PadShape::Oval;
    if (l == "rect" || l == "square" || l.empty()) return PadShape::Rect;
    throw std::runtime_error("pad shape is rect, circle or oval");
}

// A picture of a footprint on its own: a tiny board with just that part on it.
Attachment footprintPng(const Footprint& fp) {
    Circuit c;
    std::vector<std::pair<std::string, std::string>> pins;
    for (const auto& pad : fp.pads)
        if (!pad.number.empty()) pins.push_back({pad.number, ""});
    Part part;
    part.ref = "U1";
    part.customSymbol = std::make_shared<const Symbol>(makeBoxSymbol(fp.name.empty() ? "footprint" : fp.name, "U", fp.name, pins));
    part.symbol = part.customSymbol->name;
    part.customFootprint = std::make_shared<const Footprint>(fp);
    part.footprint = fp.name;
    part.pcb.placed = true;
    const double m = 2;
    const double w = fp.x1 - fp.x0 + 2 * m, h = fp.y1 - fp.y0 + 2 * m;
    part.pcb.pos = {m - fp.x0, m - fp.y0};
    c.parts.push_back(std::move(part));
    c.board.outline = {{0, 0}, {w, 0}, {w, h}, {0, h}};
    return pcbPng(c, {}, 700);
}

std::string padText(const PadDef& p) {
    std::string s = (p.number.empty() ? std::string("(hole)") : p.number) + " at " + pt(p.pos) + ", " + num(p.w) + " x " + num(p.h) + " " +
                    (p.shape == PadShape::Circle ? "circle" : p.shape == PadShape::Oval ? "oval" : "rect");
    if (p.drill > 0) s += ", drill " + num(p.drill) + (p.plated ? "" : " (not plated)");
    return s;
}

Result footprintResult(std::string message, const Footprint& fp, const Params& p, bool editedDoc) {
    Result r = editedDoc ? edited(message, p) : Result::success(std::move(message));
    r.with("name", fp.name).with("pads", static_cast<std::int64_t>(fp.pads.size()))
        .with("width", fp.x1 - fp.x0).with("height", fp.y1 - fp.y0);
    if (boolParam(p, "preview", false)) {
        r.attachments.clear();
        r.attach(footprintPng(fp));
    }
    return r;
}

}  // namespace

void registerFootprintEditCommands(Session& session) {
    auto& reg = session.registry();
    reg.add({"circuit.footprint_make", "Make Footprint",
             "Make a footprint from the numbers on a datasheet's land pattern page and keep it in your footprint library "
             "(a folder of KiCad .kicad_mod files), so circuit.place footprint=NAME and KiCad can both use it. Kinds: smd2 "
             "(two-pad chip: pitch = pad centre to pad centre), row (single row: headers, connectors), dual (SOIC/DIP/SOP: span = "
             "row centre to row centre), quad (QFP/QFN: span = across, exposed = centre pad), grid (BGA: balls A1, A2, ..., "
             "columns per row) or blank (add pads with circuit.footprint_pad). pad_w is the pad's width along the row, pad_h its "
             "length pointing out from the body. Give ref= to put it on a part of this circuit straight away.",
             {{"doc", ParamType::String, "circuit to put it on (with ref=)", false},
              {"name", ParamType::String, "footprint name, e.g. SOP-16_W7.5", true},
              {"kind", ParamType::String, "smd2, row, dual, quad, grid or blank", true, {"smd2", "row", "dual", "quad", "grid", "blank"}},
              {"pins", ParamType::Int, "pad count", false, {}, static_cast<std::int64_t>(2)},
              {"pitch", ParamType::Number, "pad to pad, mm", false, {}, 1.27},
              {"span", ParamType::Number, "dual: row to row; quad: across the pads, mm", false, {}, 5.4},
              {"pad_w", ParamType::Number, "pad width along the row, mm (grid: ball size)", false, {}, 0.6},
              {"pad_h", ParamType::Number, "pad length out from the body, mm", false, {}, 1.5},
              {"drill", ParamType::Number, "hole size for through-hole pads, mm (0: SMD)", false, {}, 0.0},
              {"shape", ParamType::String, "pad shape: rect, circle or oval (pad 1 of through-hole parts stays square)", false, {"rect", "circle", "oval"}, std::string("rect")},
              {"columns", ParamType::Int, "grid: balls per row (default: square)", false},
              {"exposed", ParamType::Number, "quad: exposed centre pad size, mm", false, {}, 0.0},
              {"ref", ParamType::String, "put it on this part (needs doc=)", false},
              {"save", ParamType::Bool, "keep it in your library", false, {}, true},
              previewParam()},
             true},
            [&session](const Params& p) -> std::unique_ptr<Action> {
                FootprintSpec sp;
                sp.name = stringParam(p, "name");
                sp.kind = stringParam(p, "kind");
                sp.pins = static_cast<int>(intParam(p, "pins", 2));
                sp.pitch = numberParam(p, "pitch", 1.27);
                sp.span = numberParam(p, "span", 5.4);
                sp.padW = numberParam(p, "pad_w", 0.6);
                sp.padH = numberParam(p, "pad_h", 1.5);
                sp.drill = numberParam(p, "drill", 0);
                sp.columns = static_cast<int>(intParam(p, "columns", 0));
                sp.exposed = numberParam(p, "exposed", 0);
                auto fail = [](std::string m) -> std::unique_ptr<Action> { return std::make_unique<FunctionAction>([m] { return Result::failure(m); }); };
                if (sp.name.empty()) return fail("give the footprint a name");
                if (sp.pins < 1 || sp.pins > 2000) return fail("pins must be 1 to 2000");
                if (sp.pitch <= 0 || sp.padW <= 0 || sp.padH <= 0 || sp.drill < 0) return fail("pitch and pad sizes must be above 0");
                if (sp.drill > 0 && sp.drill >= std::min(sp.padW, sp.padH)) return fail("the drill is as big as the pad: make the pad bigger or the drill smaller");
                Footprint fp;
                try {
                    sp.shape = shapeOf(stringParam(p, "shape", "rect"));
                    fp = makeFootprint(sp);
                } catch (const std::exception& e) {
                    return fail(e.what());
                }
                if (findFootprint(sp.name)) return fail("'" + sp.name + "' is a built-in footprint name; pick another so the two don't get mixed up");
                const bool save = boolParam(p, "save", true);
                if (has(p, "ref")) {
                    if (!has(p, "doc")) return fail("ref= needs doc= (the circuit the part is in)");
                    auto shared = std::make_shared<const Footprint>(fp);
                    Params q = p;
                    q.erase("preview");
                    return circuitCommand(
                        session,
                        [shared, save, p](CircuitDoc& doc, const Params& qq) {
                            Part* part = doc.circuit.part(stringParam(qq, "ref"));
                            if (!part) return Result::failure("no part " + stringParam(qq, "ref"));
                            std::string where;
                            if (save) where = "; saved to " + saveUserFootprint(*shared).string();
                            part->footprint = shared->name;
                            part->customFootprint = shared;
                            return footprintResult("made " + shared->name + " (" + shared->description + ") and put it on " + part->ref + where, *shared, p, true);
                        },
                        View::Pcb)(q);
                }
                return std::make_unique<FunctionAction>([fp, save, p] {
                    std::string where;
                    try {
                        if (save) where = "; saved to " + saveUserFootprint(fp).string();
                    } catch (const std::exception& e) {
                        return Result::failure(e.what());
                    }
                    return footprintResult("made " + fp.name + " (" + fp.description + ")" + where, fp, p, false);
                });
            });

    reg.add({"circuit.footprint_pad", "Edit Footprint Pad",
             "Edit the footprint on one part: move or resize a pad, change its shape, hole or number, add a pad (a number the "
             "footprint doesn't have yet) or remove one. The part gets its own copy of the footprint, so other parts and the "
             "library keep theirs; circuit.footprint_save puts the edited one in your library. Without changes it lists the pads.",
             {docParam(),
              {"ref", ParamType::String, "the part, e.g. U1", true},
              {"number", ParamType::String, "the pad (a new number adds a pad)", false},
              {"x", ParamType::Number, "pad centre x, mm (footprint space: origin at the part's centre, y down)", false},
              {"y", ParamType::Number, "pad centre y, mm", false},
              {"dx", ParamType::Number, "move by, mm", false},
              {"dy", ParamType::Number, "move by, mm", false},
              {"w", ParamType::Number, "width, mm", false},
              {"h", ParamType::Number, "height, mm", false},
              {"shape", ParamType::String, "rect, circle or oval", false, {"rect", "circle", "oval"}},
              {"drill", ParamType::Number, "hole, mm (0: SMD)", false},
              {"plated", ParamType::Bool, "false: a bare mounting hole", false},
              {"new_number", ParamType::String, "renumber the pad", false},
              {"remove", ParamType::Bool, "delete the pad", false, {}, false},
              {"name", ParamType::String, "rename the edited footprint", false},
              previewParam()},
             true},
            circuitCommand(
                session,
                [](CircuitDoc& doc, const Params& p) {
                    Part* part = doc.circuit.part(stringParam(p, "ref"));
                    if (!part) return Result::failure("no part " + stringParam(p, "ref"));
                    auto cur = footprintOf(*part);
                    Footprint fp = cur ? *cur : Footprint{};
                    if (!cur) fp.name = part->ref + "_footprint", fp.description = "drawn by hand";
                    if (!has(p, "number")) {
                        if (has(p, "name")) {
                            fp.name = stringParam(p, "name");
                        } else {
                            std::string s = fp.name + ": " + std::to_string(fp.pads.size()) + " pads";
                            for (const auto& pad : fp.pads) s += "\n  " + padText(pad);
                            return footprintResult(s, fp, p, false);
                        }
                    } else {
                        const std::string number = stringParam(p, "number");
                        auto it = std::find_if(fp.pads.begin(), fp.pads.end(), [&](const PadDef& d) { return d.number == number; });
                        std::string what;
                        if (boolParam(p, "remove", false)) {
                            if (it == fp.pads.end()) return Result::failure(fp.name + " has no pad " + number);
                            fp.pads.erase(it);
                            what = "removed pad " + number;
                        } else {
                            if (it == fp.pads.end()) {
                                PadDef d;
                                d.number = number;
                                if (!fp.pads.empty()) d.w = fp.pads.back().w, d.h = fp.pads.back().h, d.shape = fp.pads.back().shape, d.drill = fp.pads.back().drill;
                                fp.pads.push_back(d);
                                it = fp.pads.end() - 1;
                                what = "added pad " + number;
                            } else {
                                what = "pad " + number;
                            }
                            PadDef& d = *it;
                            if (has(p, "x")) d.pos.x = numberParam(p, "x");
                            if (has(p, "y")) d.pos.y = numberParam(p, "y");
                            d.pos.x += numberParam(p, "dx", 0);
                            d.pos.y += numberParam(p, "dy", 0);
                            if (has(p, "w")) d.w = numberParam(p, "w");
                            if (has(p, "h")) d.h = numberParam(p, "h");
                            if (has(p, "drill")) d.drill = numberParam(p, "drill");
                            if (has(p, "plated")) d.plated = boolParam(p, "plated");
                            try {
                                if (has(p, "shape")) d.shape = shapeOf(stringParam(p, "shape"));
                            } catch (const std::exception& e) {
                                return Result::failure(e.what());
                            }
                            if (has(p, "new_number")) {
                                const std::string nn = stringParam(p, "new_number");
                                for (const auto& o : fp.pads)
                                    if (&o != &d && o.number == nn && !nn.empty()) return Result::failure(fp.name + " already has a pad " + nn);
                                d.number = nn;
                            }
                            if (d.w <= 0 || d.h <= 0) return Result::failure("pad sizes must be above 0");
                            if (d.drill < 0 || (d.drill > 0 && d.plated && d.drill >= std::min(d.w, d.h))) return Result::failure("the drill must be smaller than the pad");
                            what += ": " + padText(d);
                        }
                        if (has(p, "name")) fp.name = stringParam(p, "name");
                        finishFootprint(fp);
                        part->customFootprint = std::make_shared<const Footprint>(fp);
                        part->footprint = fp.name;
                        return footprintResult(part->ref + " (" + fp.name + "): " + what, fp, p, true);
                    }
                    finishFootprint(fp);
                    part->customFootprint = std::make_shared<const Footprint>(fp);
                    part->footprint = fp.name;
                    return footprintResult(part->ref + "'s footprint is now called " + fp.name, fp, p, true);
                },
                View::Pcb));

    reg.add({"circuit.footprint_save", "Save Footprint to Library",
             "Put a part's footprint (edited or not) into your footprint library as NAME.kicad_mod, for later circuits "
             "(circuit.place footprint=NAME) and KiCad. Also: list=true lists the library, path= imports a .kicad_mod "
             "file into it.",
             {{"doc", ParamType::String, "id of an open circuit", false},
              {"ref", ParamType::String, "the part whose footprint to save", false},
              {"name", ParamType::String, "save under this name (default: the footprint's)", false},
              {"path", ParamType::String, "a .kicad_mod file to copy into the library instead", false},
              {"list", ParamType::Bool, "list the library", false, {}, false},
              previewParam()},
             false},
            [&session](const Params& p) -> std::unique_ptr<Action> {
                return std::make_unique<FunctionAction>([&session, p] {
                    try {
                        if (boolParam(p, "list", false) || (!has(p, "ref") && !has(p, "path"))) {
                            const auto names = userFootprints();
                            std::string s = std::to_string(names.size()) + " footprints in " + userFootprintDir().string();
                            for (const auto& n : names) s += "\n  " + n;
                            return Result::success(s).with("count", static_cast<std::int64_t>(names.size()));
                        }
                        std::shared_ptr<const Footprint> fp;
                        if (has(p, "path")) {
                            std::vector<std::string> warnings;
                            fp = fromKicadMod(readFile(stringParam(p, "path")), &warnings);
                        } else {
                            if (!has(p, "doc")) return Result::failure("ref= needs doc=");
                            auto d = session.workspace().getAs<CircuitDoc>(stringParam(p, "doc"));
                            if (!d) return Result::failure("no open circuit called '" + stringParam(p, "doc") + "'");
                            const Part* part = d->circuit.part(stringParam(p, "ref"));
                            if (!part) return Result::failure("no part " + stringParam(p, "ref"));
                            fp = footprintOf(*part);
                            if (!fp) return Result::failure(part->ref + " has no footprint");
                        }
                        Footprint copy = *fp;
                        if (has(p, "name")) copy.name = stringParam(p, "name");
                        if (findFootprint(copy.name)) return Result::failure("'" + copy.name + "' is a built-in footprint name; save it under another (name=)");
                        const fs::path file = saveUserFootprint(copy);
                        return footprintResult("saved " + copy.name + " (" + std::to_string(copy.pads.size()) + " pads) to " + file.string(), copy, p, false)
                            .with("path", file.string());
                    } catch (const std::exception& e) {
                        return Result::failure(e.what());
                    }
                });
            });
}

}  // namespace wetwaresuite::circuit::cmd
