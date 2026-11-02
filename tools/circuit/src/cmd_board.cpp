// circuit.* board tools beyond the basics: plane fan-out and KiCad board import.
#include "cmd_common.hpp"

namespace wetwaresuite::circuit::cmd {

void registerBoardToolCommands(Session& session) {
    auto& reg = session.registry();
    reg.add({"circuit.fanout", "Fan Out to Planes",
             "Give every SMD pad of the named nets (default: every net with a pour) a short trace and a via, so it reaches the "
             "plane poured on another layer: the usual way to connect ground and power on a 4-layer board (pour GND on In1, "
             "+3V3 on In2, then fanout). Pads a pour already reaches, through-hole pads and pads with a via are left alone.",
             {docParam(), {"nets", ParamType::String, "nets to fan out, comma-separated (default: nets with a pour)", false}, previewParam()},
             true},
            circuitCommand(
                session,
                [](CircuitDoc& doc, const Params& p) {
                    if (doc.circuit.board.outline.size() < 3) return Result::failure("no board yet: circuit.board or circuit.pcb_place first");
                    const Netlist nl = buildNetlist(doc.circuit);
                    std::vector<std::string> nets;
                    if (has(p, "nets")) {
                        for (const auto& n : splitList(stringParam(p, "nets"))) {
                            const Net* net = nl.net(n);
                            if (!net) return Result::failure("no net called " + n + " (circuit.nets lists them)");
                            nets.push_back(net->name);
                        }
                    }
                    if (nets.empty() && doc.circuit.board.zones.empty()) return Result::failure("no pours yet: give nets=, or pour a plane first (circuit.pour)");
                    const FanoutReport r = fanout(doc.circuit, nl, nets);
                    std::string s = "added " + std::to_string(r.vias) + " vias";
                    if (r.already) s += "; " + std::to_string(r.already) + " pads already had one";
                    for (const auto& f : r.failed) s += "\nno room for a via next to " + f;
                    return edited(s, p).with("vias", static_cast<std::int64_t>(r.vias)).with("failed", static_cast<std::int64_t>(r.failed.size()));
                },
                View::Pcb));

    reg.add({"circuit.import_board", "Import KiCad Board",
             "Put a KiCad board (.kicad_pcb) onto this circuit: footprints (KiCad's own, placed where KiCad has them) go on the parts "
             "with the same references, and the outline, tracks, vias and pours replace this board's. KiCad's net names are matched "
             "to the schematic's through the pads. Undo restores the old board.",
             {docParam(), {"path", ParamType::String, "the .kicad_pcb file", true}, previewParam()}, true},
            circuitCommand(
                session,
                [](CircuitDoc& doc, const Params& p) {
                    const fs::path path = stringParam(p, "path");
                    std::vector<std::string> warnings;
                    mergeKicadPcb(doc.circuit, readFile(path), &warnings);
                    int placed = 0;
                    for (const auto& part : doc.circuit.parts) placed += part.pcb.placed;
                    const Board& b = doc.circuit.board;
                    std::string s = "imported " + path.string() + ": " + std::to_string(placed) + " footprints, " + std::to_string(b.traces.size()) +
                                    " track segments, " + std::to_string(b.vias.size()) + " vias, " + std::to_string(b.zones.size()) + " pours, " +
                                    std::to_string(b.layers) + " copper layers";
                    for (const auto& w : warnings) s += "\nwarning: " + w;
                    return edited(s, p).with("placed", static_cast<std::int64_t>(placed));
                },
                View::Pcb));
    reg.add({"circuit.length", "Net Lengths",
             "Copper length of nets (their traces), in mm: for matching a bus or a differential pair before circuit.tune.",
             {docParam(), {"nets", ParamType::String, "nets, comma-separated (default: every routed net)", false}}, false},
            circuitCommand(
                session,
                [](CircuitDoc& doc, const Params& p) {
                    const Netlist nl = buildNetlist(doc.circuit);
                    std::vector<std::string> nets;
                    if (has(p, "nets")) {
                        for (const auto& n : splitList(stringParam(p, "nets"))) {
                            const Net* net = nl.net(n);
                            if (!net) return Result::failure("no net called " + n + " (circuit.nets lists them)");
                            nets.push_back(net->name);
                        }
                    } else {
                        for (const auto& t : doc.circuit.board.traces)
                            if (!t.net.empty() && std::find(nets.begin(), nets.end(), t.net) == nets.end()) nets.push_back(t.net);
                        std::sort(nets.begin(), nets.end());
                    }
                    std::string s;
                    Result r = Result::success("");
                    for (const auto& n : nets) {
                        const double l = netLength(doc.circuit, n);
                        s += (s.empty() ? "" : "\n") + n + ": " + num(l) + " mm";
                        r.with("length(" + n + ")", l);
                    }
                    r.message = s.empty() ? "no routed nets yet" : s;
                    return r;
                },
                View::Pcb, false));

    reg.add({"circuit.tune", "Tune Length",
             "Stretch a net with meanders (trombone bumps) until it is a given length, or as long as another net (match=), "
             "the way high-speed buses and differential pairs are matched. Bumps go on its longest straight traces, on "
             "whichever side has room; meanders only add length.",
             {docParam(),
              {"net", ParamType::String, "the net to stretch", true},
              {"length", ParamType::Number, "target length, mm", false},
              {"match", ParamType::String, "make it as long as this net instead", false},
              {"amplitude", ParamType::Number, "how far a bump sticks out, mm", false, {}, 1.0},
              {"spacing", ParamType::Number, "gap between bumps, mm", false, {}, 0.5},
              previewParam()},
             true},
            circuitCommand(
                session,
                [](CircuitDoc& doc, const Params& p) {
                    const Netlist nl = buildNetlist(doc.circuit);
                    const Net* net = nl.net(stringParam(p, "net"));
                    if (!net) return Result::failure("no net called " + stringParam(p, "net") + " (circuit.nets lists them)");
                    double target = 0;
                    if (has(p, "match")) {
                        const Net* other = nl.net(stringParam(p, "match"));
                        if (!other) return Result::failure("no net called " + stringParam(p, "match"));
                        target = netLength(doc.circuit, other->name);
                    } else if (has(p, "length")) {
                        target = numberParam(p, "length");
                    } else {
                        return Result::failure("give length= (mm) or match= (another net)");
                    }
                    const double amp = numberParam(p, "amplitude", 1.0), gap = numberParam(p, "spacing", 0.5);
                    if (amp <= 0 || gap <= 0) return Result::failure("amplitude and spacing must be above 0");
                    const TuneReport t = tuneLength(doc.circuit, nl, net->name, target, amp, gap);
                    std::string s = net->name + ": " + num(t.before) + " mm -> " + num(t.after) + " mm (target " + num(target) + ")";
                    if (!t.note.empty()) s += "; " + t.note;
                    return edited(s, p).with("before", t.before).with("after", t.after).with("segments", static_cast<std::int64_t>(t.segments));
                },
                View::Pcb));
}

}  // namespace wetwaresuite::circuit::cmd
