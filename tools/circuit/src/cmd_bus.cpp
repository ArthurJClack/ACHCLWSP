// Buses: circuit.bus draws one (D[0..7], {SDA,SCL}), circuit.bus_tap joins pins to its members with
// a labelled stub and an entry line each, as KiCad does. The labels make the connections.
#include "cmd_common.hpp"

namespace wetwaresuite::circuit::cmd {

namespace {

// nearest point on a polyline
Pt nearestOn(const std::vector<Pt>& pts, Pt q) {
    Pt best = pts.front();
    double bestD = 1e18;
    for (std::size_t i = 1; i < pts.size(); ++i) {
        const Pt a = pts[i - 1], b = pts[i];
        const double dx = b.x - a.x, dy = b.y - a.y, len = dx * dx + dy * dy;
        const double t = len > 0 ? std::clamp(((q.x - a.x) * dx + (q.y - a.y) * dy) / len, 0.0, 1.0) : 0;
        const Pt p{a.x + t * dx, a.y + t * dy};
        const double d = (p.x - q.x) * (p.x - q.x) + (p.y - q.y) * (p.y - q.y);
        if (d < bestD) bestD = d, best = p;
    }
    return {std::round(best.x), std::round(best.y)};
}

std::vector<Pt> parsePoints(const std::string& text, std::string& error) {
    std::vector<Pt> pts;
    std::stringstream ss(text);
    std::string item;
    while (ss >> item) {
        const auto comma = item.find(',');
        try {
            if (comma == std::string::npos) throw std::invalid_argument("");
            pts.push_back({std::round(std::stod(item.substr(0, comma))), std::round(std::stod(item.substr(comma + 1)))});
        } catch (...) {
            error = "points should look like \"10,5 30,5 30,20\" (x,y pairs in grid units)";
            return {};
        }
    }
    return pts;
}

Bus* findBus(Circuit& c, const std::string& name) {
    for (auto& b : c.buses) {
        if (b.name == name || lower(b.name) == lower(name)) return &b;
        const auto open = b.name.find_first_of("[{");
        if (open != std::string::npos && lower(b.name.substr(0, open)) == lower(name)) return &b;  // "D" finds D[0..7]
    }
    return nullptr;
}

}  // namespace

void registerBusCommands(Session& session) {
    auto& reg = session.registry();
    reg.add({"circuit.bus", "Draw Bus",
             "Draw a bus: a thick line grouping related signals, named like D[0..7] (D0..D7), A[15..0] or {SDA,SCL,INT}. "
             "points= is the line (\"x,y x,y ...\", grid units), or from= and to= for a straight one. It joins nothing by itself: "
             "circuit.bus_tap puts each member's label on a pin. remove=true deletes the bus (its labels stay).",
             {docParam(),
              {"name", ParamType::String, "bus name, e.g. D[0..7] or {SDA,SCL}", true},
              {"points", ParamType::String, "the line: x,y x,y ... (grid units)", false},
              {"from", ParamType::String, "or: start x,y", false},
              {"to", ParamType::String, "and end x,y", false},
              {"remove", ParamType::Bool, "delete this bus", false, {}, false},
              previewParam()},
             true},
            circuitCommand(
                session,
                [](CircuitDoc& doc, const Params& p) {
                    Circuit& c = doc.circuit;
                    const std::string name = stringParam(p, "name");
                    if (boolParam(p, "remove", false)) {
                        Bus* b = findBus(c, name);
                        if (!b) return Result::failure("no bus called " + name);
                        const std::string was = b->name;
                        std::erase_if(c.buses, [&](const Bus& x) { return &x == b; });
                        return edited("removed bus " + was, p);
                    }
                    Bus bus;
                    bus.name = name;
                    bus.members = busMembers(name);
                    if (bus.members.empty()) return Result::failure("can't read the bus name '" + name + "': use D[0..7] or {SDA,SCL}");
                    if (findBus(c, name)) return Result::failure("there is already a bus called " + name);
                    std::string error;
                    if (has(p, "points")) bus.pts = parsePoints(stringParam(p, "points"), error);
                    else if (has(p, "from") && has(p, "to")) bus.pts = parsePoints(stringParam(p, "from") + " " + stringParam(p, "to"), error);
                    else error = "give points= or from= and to=";
                    if (!error.empty()) return Result::failure(error);
                    if (bus.pts.size() < 2) return Result::failure("a bus needs at least two points");
                    c.buses.push_back(bus);
                    std::string members;
                    for (const auto& m : bus.members) members += (members.empty() ? "" : ", ") + m;
                    Result r = edited("bus " + name + ": " + std::to_string(bus.members.size()) + " members (" + members + ")", p);
                    r.with("members", static_cast<std::int64_t>(bus.members.size()));
                    return r;
                },
                View::Schematic));

    reg.add({"circuit.bus_tap", "Tap Bus",
             "Join pins to a bus's members, in order: bus=D pins=U1.D0,U1.D1,... (or pins=U1.2..9 for a run of pin numbers). Each "
             "pin gets a stub with the member's net label and an entry line to the bus, so pins tapped from the same member "
             "anywhere on the sheet are one net. members= picks which members (default: from the first, in order).",
             {docParam(),
              {"bus", ParamType::String, "the bus, by name (D or D[0..7])", true},
              {"pins", ParamType::String, "pins in member order, comma-separated; U1.2..9 is pins 2 to 9", true},
              {"members", ParamType::String, "which members, e.g. D4,D5 (default: the first ones)", false},
              previewParam()},
             true},
            circuitCommand(
                session,
                [](CircuitDoc& doc, const Params& p) {
                    Circuit& c = doc.circuit;
                    Bus* bus = findBus(c, stringParam(p, "bus"));
                    if (!bus) return Result::failure("no bus called " + stringParam(p, "bus") + " (circuit.bus draws one)");
                    std::vector<std::string> pins;
                    for (const auto& item : splitList(stringParam(p, "pins"))) {
                        const auto dot = item.find('.'), dots = item.find("..");
                        if (dots != std::string::npos && dot != std::string::npos && dot < dots) {  // U1.2..9
                            const std::string ref = item.substr(0, item.find('.'));
                            try {
                                const int a = std::stoi(item.substr(ref.size() + 1, dots - ref.size() - 1)), b = std::stoi(item.substr(dots + 2));
                                for (int i = a;; i += a <= b ? 1 : -1) {
                                    pins.push_back(ref + "." + std::to_string(i));
                                    if (i == b) break;
                                }
                            } catch (...) {
                                return Result::failure("can't read the pin range " + item);
                            }
                        } else {
                            pins.push_back(item);
                        }
                    }
                    std::vector<std::string> members = has(p, "members") ? splitList(stringParam(p, "members")) : bus->members;
                    if (pins.size() > members.size()) return Result::failure(std::to_string(pins.size()) + " pins but " + bus->name + " has " + std::to_string(members.size()) + " members");
                    std::string done;
                    for (std::size_t i = 0; i < pins.size(); ++i) {
                        std::string error;
                        const PinRef r = resolvePin(c, pins[i], error);
                        if (!r.part) return Result::failure(error);
                        labelPin(c, *r.part, *r.pin, members[i]);
                        const Pt at = c.labels.back().pos;
                        bus->entries.push_back({at, nearestOn(bus->pts, at)});
                        done += (done.empty() ? "" : ", ") + pins[i] + "=" + members[i];
                    }
                    Result r = edited("tapped " + bus->name + ": " + done, p);
                    r.with("taps", static_cast<std::int64_t>(pins.size()));
                    return r;
                },
                View::Schematic));
}

}  // namespace wetwaresuite::circuit::cmd
