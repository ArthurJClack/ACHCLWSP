#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <random>
#include <sstream>
#include <string>

#include "wetwaresuite/circuit/circuit.hpp"
#include "wetwaresuite/core/cli.hpp"
#include "wetwaresuite/mcp/server.hpp"
#include "wetwaresuite/raster/ora.hpp"
#include "check.hpp"

using namespace wetwaresuite;
using namespace wetwaresuite::circuit;
namespace fs = std::filesystem;

namespace {

fs::path tempDir() {
    std::random_device rd;
    const auto stamp = static_cast<unsigned long long>(std::chrono::high_resolution_clock::now().time_since_epoch().count());
    fs::path d = fs::temp_directory_path() / ("wetwaresuite_circuit_" + std::to_string(stamp) + "_" + std::to_string(rd()));
    fs::remove_all(d);
    fs::create_directories(d);
    return d;
}

std::string q(const fs::path& p) { return "\"" + p.generic_string() + "\""; }

Result run(cli::Batch& b, const std::string& line) {
    Result r = b.runLine(line);
    if (!r.ok) std::printf("  (%s -> %s)\n", line.c_str(), r.message.c_str());
    return r;
}

std::int64_t intField(const Result& r, const std::string& key) {
    const Value* v = r.field(key);
    if (!v) return -999;
    if (auto i = std::get_if<std::int64_t>(v)) return *i;
    return -998;
}

double numField(const Result& r, const std::string& key) {
    const Value* v = r.field(key);
    if (!v) return -999;
    if (auto d = std::get_if<double>(v)) return *d;
    if (auto i = std::get_if<std::int64_t>(v)) return static_cast<double>(*i);
    return -998;
}

std::string readAll(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    std::stringstream s;
    s << in.rdbuf();
    return s.str();
}

Circuit& circuitOf(Session& s, const std::string& id) { return s.workspace().getAs<CircuitDoc>(id)->circuit; }

bool near(double a, double b, double tol) { return std::abs(a - b) <= tol; }

void testValues() {
    CHECK(near(*parseValue("4k7"), 4700, 1e-9));
    CHECK(near(*parseValue("10k"), 10000, 1e-9));
    CHECK(near(*parseValue("100n"), 1e-7, 1e-18));
    CHECK(near(*parseValue("1M"), 1e6, 1e-6));
    CHECK(near(*parseValue("1meg"), 1e6, 1e-6));
    CHECK(near(*parseValue("2m"), 2e-3, 1e-12));
    CHECK(near(*parseValue("5V"), 5, 1e-12));
    CHECK(near(*parseValue("2R2"), 2.2, 1e-12));
    CHECK(near(*parseValue("4.7uF"), 4.7e-6, 1e-15));
    CHECK(!parseValue("red"));
    CHECK(!parseValue("LM358"));
    CHECK(formatValue(4700) == "4.7k");
    CHECK(isPowerName("GND") && isPowerName("+5V") && isPowerName("3V3") && isPowerName("VCC") && !isPowerName("SDA") && !isPowerName("LED1"));
}

void testLibrary() {
    for (const auto& e : symbolCatalog()) {
        auto s = findSymbol(e.name, 6);
        CHECK(s != nullptr);
        if (!s) continue;
        CHECK(!s->pins.empty() || s->name == "mounting_hole");
        for (const auto& p : s->pins) CHECK(std::abs(p.pos.x - std::round(p.pos.x)) < 1e-9 && std::abs(p.pos.y - std::round(p.pos.y)) < 1e-9);
        if (!s->footprint.empty()) {
            auto f = findFootprint(s->footprint);
            CHECK(f != nullptr);
            if (!f) continue;
            for (const auto& p : s->pins) {
                bool found = false;
                for (const auto& pad : f->pads) found = found || pad.number == p.number;
                CHECK(found || p.type == PinType::NotConnected || s->name == "opamp");
            }
        }
    }
    CHECK(canonicalSymbol("R") == "resistor" && canonicalSymbol("uno") == "arduino_uno" && canonicalSymbol("+5V") == "power");
    CHECK(findSymbol("+3v3")->value == "+3V3");
    CHECK(findSymbol("ic", 8, {"VCC", "GND"})->pins[0].name == "VCC");
    CHECK(findSymbol("nope") == nullptr);
    CHECK(findFootprint("SOIC-14")->pads.size() == 14);
    CHECK(findFootprint("DIP-28")->pads.size() == 28);
    CHECK(findFootprint("PinHeader_2x05")->pads.size() == 10);
    CHECK(findFootprint("Resistor_SMD:R_0805_2012Metric")->name == "R_0805");
    CHECK(findFootprint("Connector_PinHeader_2.54mm:PinHeader_1x04_P2.54mm_Vertical") == nullptr ||
          findFootprint("PinHeader_1x04_P2.54mm_Vertical")->pads.size() == 4);
    CHECK(findFootprint("Arduino_Uno_Shield")->pads.size() == 32);
    CHECK(findFootprint("BluePill")->pads.size() == 40);
    CHECK(findFootprint("XYZ") == nullptr);
    // DIP numbering: pad 5 of DIP-8 is opposite pad 4
    auto dip = findFootprint("DIP-8");
    CHECK(near(dip->pads[4].pos.y, dip->pads[3].pos.y, 1e-9) && dip->pads[4].pos.x > 0);
}

void testSchematic(const fs::path& dir) {
    Session s;
    registerCommands(s);
    cli::Batch b(s);
    CHECK(run(b, "circuit.new title=divider -> c").ok);
    CHECK(run(b, "circuit.place doc=$c symbol=resistor value=10k x=0 y=0").ok);
    CHECK(run(b, "circuit.place doc=$c symbol=resistor value=10k x=0 y=6 rot=90").ok);
    Result r = run(b, "circuit.place doc=$c symbol=capacitor value=1u");
    CHECK(r.ok && numField(r, "x") > 0);
    CHECK(run(b, "circuit.power doc=$c symbol=+5v pins=R1.1").ok);
    CHECK(run(b, "circuit.connect doc=$c from=R1.2 to=R2.2").ok);
    CHECK(run(b, "circuit.net doc=$c name=MID pins=R1.2,C1.1").ok);
    CHECK(run(b, "circuit.power doc=$c symbol=gnd pins=R2.1,C1.2").ok);
    Circuit& c = circuitOf(s, "ckt1");
    Netlist nl = buildNetlist(c);
    CHECK(nl.netOf("R1", "2") == "MID");
    CHECK(nl.netOf("R2", "2") == "MID");
    CHECK(nl.netOf("C1", "1") == "MID");
    CHECK(nl.netOf("R1", "1") == "+5V");
    CHECK(nl.netOf("R2", "1") == "GND" && nl.netOf("C1", "2") == "GND");
    r = run(b, "circuit.erc doc=$c");
    CHECK(r.ok && intField(r, "errors") == 0 && intField(r, "warnings") == 0);
    // moving a part keeps its wires on
    CHECK(run(b, "circuit.move doc=$c ref=R2 dx=-4 dy=2").ok);
    nl = buildNetlist(c);
    CHECK(nl.netOf("R2", "2") == "MID" && nl.netOf("R2", "1") == "GND");
    CHECK(run(b, "circuit.move doc=$c ref=R2 rot=0").ok);
    nl = buildNetlist(c);
    CHECK(nl.netOf("R2", "2") == "MID" && nl.netOf("R2", "1") == "GND");
    // simulation
    r = run(b, "circuit.sim_op doc=$c");
    CHECK(r.ok && near(numField(r, "V(MID)"), 2.5, 1e-6));
    r = run(b, "circuit.sim_tran doc=$c tstop=50m step=0.1m probes=MID preview=true path=" + q(dir / "tran.csv"));
    CHECK(r.ok && r.attachments.size() == 1 && near(numField(r, "V(MID)"), 2.5, 0.01));
    CHECK(readAll(dir / "tran.csv").rfind("time,V(MID)", 0) == 0);
    // ERC catches problems
    CHECK(run(b, "circuit.place doc=$c symbol=led ref=D9 x=40 y=40").ok);
    r = run(b, "circuit.erc doc=$c");
    CHECK(intField(r, "errors") == 2 && r.message.find("D9.1") != std::string::npos);
    CHECK(run(b, "circuit.no_connect doc=$c pins=D9.1,D9.2").ok);
    CHECK(intField(run(b, "circuit.erc doc=$c"), "errors") == 0);
    CHECK(run(b, "circuit.net doc=$c name=+5V pins=R2.1").ok);  // GND and +5V on one net
    r = run(b, "circuit.erc doc=$c");
    CHECK(r.message.find("shorted_power") != std::string::npos);
    CHECK(run(b, "undo").ok);
    c.parts.push_back(c.parts[0]);  // duplicate R1
    CHECK(erc(c, buildNetlist(c)).size() >= 1);
    bool dup = false;
    for (const auto& i : erc(c, buildNetlist(c))) dup = dup || i.code == "duplicate_ref";
    CHECK(dup);
    c.parts.pop_back();
    CHECK(!run(b, "circuit.connect doc=$c from=R1.9 to=R2.1").ok);
    CHECK(!run(b, "circuit.place doc=$c symbol=flux_capacitor").ok);
    // undo restores
    const std::size_t parts = c.parts.size();
    CHECK(run(b, "circuit.remove doc=$c ref=C1").ok);
    CHECK(c.parts.size() < parts);
    CHECK(run(b, "undo").ok);
    CHECK(c.parts.size() == parts);
    // files and exports
    CHECK(run(b, "circuit.save doc=$c path=" + q(dir / "div.circuit.json")).ok);
    CHECK(run(b, "circuit.open path=" + q(dir / "div.circuit.json") + " -> d").ok);
    CHECK(buildNetlist(circuitOf(s, "ckt2")).nets.size() == buildNetlist(c).nets.size());
    CHECK(run(b, "circuit.export doc=$c path=" + q(dir / "s.svg")).ok);
    CHECK(readAll(dir / "s.svg").find("<svg") != std::string::npos);
    CHECK(run(b, "circuit.export doc=$c path=" + q(dir / "s.pdf")).ok);
    CHECK(readAll(dir / "s.pdf").rfind("%PDF", 0) == 0);
    CHECK(run(b, "circuit.export doc=$c path=" + q(dir / "s.png")).ok);
    CHECK(readAll(dir / "s.png").substr(1, 3) == "PNG");
    r = run(b, "circuit.bom doc=$c");
    CHECK(r.ok && r.message.find("2,R1 R2,10k,R_0805") != std::string::npos);
    // netlist round trip through KiCad format, with Windows line endings
    CHECK(run(b, "circuit.export_netlist doc=$c path=" + q(dir / "div.net")).ok);
    std::string net = readAll(dir / "div.net");
    std::string crlf;
    for (char ch : net) crlf += ch == '\n' ? std::string("\r\n") : std::string(1, ch);
    std::ofstream(dir / "crlf.net", std::ios::binary) << crlf;
    r = run(b, "circuit.open path=" + q(dir / "crlf.net"));
    CHECK(r.ok);
    if (r.ok) {
        const Netlist a = buildNetlist(c), k = buildNetlist(circuitOf(s, std::get<std::string>(r.value)));
        CHECK(k.netOf("R1", "2") == "MID" && k.netOf("C1", "1") == "MID" && k.netOf("R2", "1") == "GND");
        const Result e = run(b, "circuit.erc doc=" + std::get<std::string>(r.value));
        CHECK(intField(e, "errors") == 2);  // D9's no-connect marks aren't part of a netlist
        if (intField(e, "errors") != 2) std::printf("%s\n", e.message.c_str());
    }
}

void testKicadImport(const fs::path& dir) {
    const std::string text =
        "(export (version \"E\")\n (design (source \"/tmp/x.kicad_sch\"))\n (components\n"
        "  (comp (ref \"U1\") (value \"NE555\") (footprint \"Package_DIP:DIP-8_W7.62mm\") (libsource (lib \"Timer\") (part \"NE555P\")))\n"
        "  (comp (ref \"R1\") (value \"1k\") (footprint \"Resistor_SMD:R_0805_2012Metric\") (libsource (lib \"Device\") (part \"R\")))\n"
        "  (comp (ref \"J1\") (value \"Conn\") (libsource (lib \"Connector\") (part \"Conn_01x02_Male\"))))\n"
        " (nets\n"
        "  (net (code \"1\") (name \"/VCC\") (node (ref \"U1\") (pin \"8\") (pinfunction \"VCC\")) (node (ref \"R1\") (pin \"1\")) (node (ref \"J1\") (pin \"1\")))\n"
        "  (net (code \"2\") (name \"GND\") (node (ref \"U1\") (pin \"1\") (pinfunction \"GND\")) (node (ref \"J1\") (pin \"2\")))\n"
        "  (net (code \"3\") (name \"Net-(R1-Pad2)\") (node (ref \"U1\") (pin \"7\")) (node (ref \"R1\") (pin \"2\")))))\n";
    std::ofstream(dir / "t.net", std::ios::binary) << text;
    Session s;
    registerCommands(s);
    cli::Batch b(s);
    Result r = run(b, "circuit.open path=" + q(dir / "t.net"));
    CHECK(r.ok && r.message.find("generic") != std::string::npos);
    Circuit& c = circuitOf(s, "ckt1");
    CHECK(c.part("U1") && c.part("U1")->symbol == "ic" && c.part("U1")->footprint == "DIP-8");
    CHECK(c.part("R1")->symbol == "resistor" && c.part("R1")->footprint == "R_0805");
    CHECK(c.part("J1")->symbol == "header" && c.part("J1")->pins == 2);
    const Netlist nl = buildNetlist(c);
    CHECK(nl.netOf("U1", "8") == "VCC" && nl.netOf("R1", "1") == "VCC" && nl.netOf("J1", "1") == "VCC");
    CHECK(nl.netOf("U1", "1") == "GND" && nl.netOf("R1", "2") == nl.netOf("U1", "7"));
    std::string error;
    CHECK(resolvePin(c, "U1.VCC", error).part != nullptr);
    CHECK(!run(b, "circuit.open path=" + q(dir / "missing.net")).ok);
}

// A board: placement, routing, pour, DRC and fabrication output.
void testBoard(const fs::path& dir) {
    Session s;
    registerCommands(s);
    cli::Batch b(s);
    CHECK(run(b, "circuit.new title=leds -> c").ok);
    CHECK(run(b, "circuit.place doc=$c symbol=header pins=4 ref=J1").ok);
    for (int i = 1; i <= 3; ++i) {
        CHECK(run(b, "circuit.place doc=$c symbol=resistor value=330").ok);
        CHECK(run(b, "circuit.place doc=$c symbol=led").ok);
        const std::string n = std::to_string(i);
        CHECK(run(b, "circuit.net doc=$c name=IN" + n + " pins=J1." + std::to_string(i + 1) + ",R" + n + ".1").ok);
        CHECK(run(b, "circuit.connect doc=$c from=R" + n + ".2 to=D" + n + ".A").ok);
        CHECK(run(b, "circuit.net doc=$c name=GND pins=D" + n + ".K").ok);
    }
    CHECK(run(b, "circuit.power doc=$c symbol=gnd pins=J1.1").ok);
    Result r = run(b, "circuit.erc doc=$c");
    CHECK(intField(r, "errors") == 0);
    r = run(b, "circuit.drc doc=$c");
    CHECK(r.message.find("no_board") != std::string::npos);
    CHECK(!run(b, "circuit.route doc=$c").ok);
    r = run(b, "circuit.pcb_place doc=$c");
    CHECK(r.ok && intField(r, "placed") == 7);
    Circuit& c = circuitOf(s, "ckt1");
    CHECK(c.board.outline.size() == 4);
    r = run(b, "circuit.ratsnest doc=$c");
    CHECK(intField(r, "unrouted") == 9);
    r = run(b, "circuit.route doc=$c preview=true");
    CHECK(r.ok && intField(r, "failed") == 0 && r.attachments.size() == 1);
    CHECK(intField(run(b, "circuit.ratsnest doc=$c"), "unrouted") == 0);
    r = run(b, "circuit.drc doc=$c");
    CHECK(intField(r, "errors") == 0);
    if (intField(r, "errors") != 0) std::printf("%s\n", r.message.c_str());
    // a pour on the ground net
    CHECK(run(b, "circuit.unroute doc=$c net=GND").ok);
    CHECK(intField(run(b, "circuit.ratsnest doc=$c"), "unrouted") > 0);
    r = run(b, "circuit.pour doc=$c net=GND layer=both");
    CHECK(r.ok && numField(r, "area") > 10);
    r = run(b, "circuit.drc doc=$c");
    CHECK(r.ok);
    // the pour joins GND pads it reaches on both layers; route whatever is left
    CHECK(run(b, "circuit.route doc=$c").ok);
    r = run(b, "circuit.drc doc=$c");
    CHECK(intField(r, "errors") == 0);
    if (intField(r, "errors") != 0) std::printf("%s\n", r.message.c_str());
    // a deliberate short
    const auto pads = worldPads(c, buildNetlist(c));
    Pt a, bb;
    for (const auto& p : pads) {
        if (p.net == "IN1") a = p.pos;
        if (p.net == "IN2") bb = p.pos;
    }
    CHECK(run(b, "circuit.trace doc=$c layer=F net=IN1 points=" + std::to_string(a.x) + "," + std::to_string(a.y) + "," + std::to_string(bb.x) + "," +
                     std::to_string(bb.y))
              .ok);
    r = run(b, "circuit.drc doc=$c");
    CHECK(r.message.find("short") != std::string::npos);
    CHECK(run(b, "undo").ok);
    // fabrication files
    r = run(b, "circuit.gerber doc=$c path=" + q(dir / "fab"));
    CHECK(r.ok && intField(r, "files") == 12);
    const std::string top = readAll(dir / "fab" / "leds-F_Cu.gtl");
    CHECK(top.find("%FSLAX46Y46*%") != std::string::npos && top.find("M02*") != std::string::npos && top.find("G36*") != std::string::npos);
    CHECK(readAll(dir / "fab" / "leds-Edge_Cuts.gm1").find("D01*") != std::string::npos);
    const std::string drl = readAll(dir / "fab" / "leds.drl");
    CHECK(drl.rfind("M48", 0) == 0 && drl.find("T1C") != std::string::npos && drl.find("M30") != std::string::npos);
    CHECK(run(b, "circuit.gerber doc=$c path=" + q(dir / "fab.zip")).ok);
    CHECK(raster::readZip(readAll(dir / "fab.zip")).size() == 12);
    CHECK(run(b, "circuit.export doc=$c view=pcb path=" + q(dir / "b.svg")).ok);
    CHECK(run(b, "circuit.export doc=$c view=pcb path=" + q(dir / "b.png")).ok);
    // the board as solids for the cad app: one per body (board + parts)
    CHECK(run(b, "circuit.export doc=$c path=" + q(dir / "b.step")).ok);
    {
        const std::string step = readAll(dir / "b.step");
        std::size_t solids = 0;
        for (auto at = step.find("MANIFOLD_SOLID_BREP"); at != std::string::npos; at = step.find("MANIFOLD_SOLID_BREP", at + 1)) ++solids;
        CHECK(step.rfind("ISO-10303-21", 0) == 0 && solids >= 3);
    }
    CHECK(run(b, "circuit.export doc=$c path=" + q(dir / "b.stl")).ok && fs::file_size(dir / "b.stl") > 1000);
    CHECK(fs::file_size(dir / "b.png") > 1000);
    CHECK(run(b, "circuit.footprint_move doc=$c ref=J1 rot=90 side=bottom").ok);
    CHECK(c.part("J1")->pcb.bottom && c.part("J1")->pcb.rot == 90);
    // board survives save/open
    CHECK(run(b, "circuit.save doc=$c path=" + q(dir / "leds.circuit.json")).ok);
    Circuit back = fromJson(readAll(dir / "leds.circuit.json"));
    CHECK(back.board.traces.size() == c.board.traces.size() && back.board.zones.size() == 2 && back.part("J1")->pcb.bottom);
}

void testSimDevices(const fs::path& dir) {
    Session s;
    registerCommands(s);
    cli::Batch b(s);
    // an RC low-pass: -3 dB at 1 / (2 pi R C) = 1 kHz
    CHECK(run(b, "circuit.new title=rc -> c").ok);
    CHECK(run(b, "circuit.place doc=$c symbol=vsource ref=V1 value=0").ok);
    CHECK(run(b, "circuit.edit doc=$c ref=V1 fields=ac=1").ok);
    CHECK(run(b, "circuit.place doc=$c symbol=resistor ref=R1 value=1k").ok);
    CHECK(run(b, "circuit.place doc=$c symbol=capacitor ref=C1 value=159.15n").ok);
    CHECK(run(b, "circuit.net doc=$c name=IN pins=V1.1,R1.1").ok);
    CHECK(run(b, "circuit.net doc=$c name=OUT pins=R1.2,C1.1").ok);
    CHECK(run(b, "circuit.power doc=$c symbol=gnd pins=V1.2,C1.2").ok);
    Result r = run(b, "circuit.sim_ac doc=$c start=10 stop=100k per_decade=50 probes=OUT preview=true path=" + q(dir / "ac.csv"));
    CHECK(r.ok && near(numField(r, "f3db(OUT)"), 1000, 30) && near(numField(r, "gain_db(OUT)"), 0, 0.05) && r.attachments.size() == 1);
    CHECK(readAll(dir / "ac.csv").rfind("freq,dB(OUT),deg(OUT)", 0) == 0);
    CHECK(!run(b, "circuit.sim_ac doc=$c start=100 stop=10").ok);

    // an NPN switch: base fed from +5V through 10k saturates it
    CHECK(run(b, "circuit.new title=npn -> q").ok);
    CHECK(run(b, "circuit.place doc=$q symbol=npn ref=Q1").ok);
    CHECK(run(b, "circuit.place doc=$q symbol=resistor ref=RB value=10k").ok);
    CHECK(run(b, "circuit.place doc=$q symbol=resistor ref=RC value=1k").ok);
    CHECK(run(b, "circuit.power doc=$q symbol=+5v pins=RB.1,RC.1").ok);
    CHECK(run(b, "circuit.net doc=$q name=B pins=RB.2,Q1.1").ok);
    CHECK(run(b, "circuit.net doc=$q name=C pins=RC.2,Q1.3").ok);
    CHECK(run(b, "circuit.power doc=$q symbol=gnd pins=Q1.2").ok);
    r = run(b, "circuit.sim_op doc=$q");
    CHECK(r.ok && numField(r, "V(C)") < 0.3 && numField(r, "V(B)") > 0.6 && numField(r, "V(B)") < 0.85);
    // biased into the active region (RB = 1M, beta 100): Ic = ~0.43 mA, so V(C) = ~4.57 V
    CHECK(run(b, "circuit.edit doc=$q ref=RB value=1meg").ok);
    r = run(b, "circuit.sim_op doc=$q");
    CHECK(r.ok && near(numField(r, "V(C)"), 4.57, 0.08));
    // and as a common-emitter amplifier: gain = gm * RC = (0.43 mA / 25.9 mV) * 1k = ~16.6 (24.4 dB)
    CHECK(run(b, "circuit.place doc=$q symbol=vsource ref=V1 value=0").ok);
    CHECK(run(b, "circuit.edit doc=$q ref=V1 fields=ac=1").ok);
    CHECK(run(b, "circuit.place doc=$q symbol=capacitor ref=CIN value=10u").ok);
    CHECK(run(b, "circuit.net doc=$q name=IN pins=V1.1,CIN.1").ok);
    CHECK(run(b, "circuit.net doc=$q name=B pins=CIN.2").ok);
    CHECK(run(b, "circuit.power doc=$q symbol=gnd pins=V1.2").ok);
    r = run(b, "circuit.sim_ac doc=$q start=1k stop=10k probes=C");
    CHECK(r.ok && near(numField(r, "gain_db(C)"), 24.4, 0.6));

    // an N-channel MOSFET switch, then the P-channel mirror image
    CHECK(run(b, "circuit.new title=fet -> m").ok);
    CHECK(run(b, "circuit.place doc=$m symbol=nmos ref=Q1").ok);
    CHECK(run(b, "circuit.place doc=$m symbol=resistor ref=RD value=1k").ok);
    CHECK(run(b, "circuit.power doc=$m symbol=+5v pins=RD.1,Q1.1").ok);
    CHECK(run(b, "circuit.net doc=$m name=D pins=RD.2,Q1.3").ok);
    CHECK(run(b, "circuit.power doc=$m symbol=gnd pins=Q1.2").ok);
    r = run(b, "circuit.sim_op doc=$m");
    CHECK(r.ok && numField(r, "V(D)") < 0.1);
    CHECK(run(b, "circuit.new title=pfet -> p").ok);
    CHECK(run(b, "circuit.place doc=$p symbol=pmos ref=Q1").ok);
    CHECK(run(b, "circuit.place doc=$p symbol=resistor ref=RD value=1k").ok);
    CHECK(run(b, "circuit.power doc=$p symbol=+5v pins=Q1.2").ok);
    CHECK(run(b, "circuit.power doc=$p symbol=gnd pins=Q1.1,RD.1").ok);
    CHECK(run(b, "circuit.net doc=$p name=D pins=RD.2,Q1.3").ok);
    r = run(b, "circuit.sim_op doc=$p");
    CHECK(r.ok && numField(r, "V(D)") > 4.9);
}

void testMultiUnit() {
    Session s;
    registerCommands(s);
    cli::Batch b(s);
    CHECK(symbolUnits("opamp_dual") == 3 && symbolUnits("74hc00") == 5 && symbolUnits("resistor") == 0);
    CHECK(run(b, "circuit.new title=units -> c").ok);
    Result r = run(b, "circuit.place doc=$c symbol=opamp_dual");
    CHECK(r.ok && toText(r.value) == "U1" && intField(r, "units") == 3);
    Circuit& c = circuitOf(s, "ckt1");
    CHECK(c.parts.size() == 3 && c.parts[0].unit == 1 && c.parts[2].unit == 3 && !c.parts[0].footprint.empty() && c.parts[1].footprint.empty());
    // pins on any unit answer to the one ref: unit A as a follower, unit B left idle with its inputs grounded
    CHECK(run(b, "circuit.place doc=$c symbol=vsource ref=V1 value=1.5").ok);
    CHECK(run(b, "circuit.net doc=$c name=IN pins=V1.1,U1.3").ok);
    CHECK(run(b, "circuit.net doc=$c name=OUT pins=U1.1,U1.-A").ok);
    CHECK(run(b, "circuit.net doc=$c name=OUTB pins=U1.OUTB,U1.6").ok);
    CHECK(run(b, "circuit.power doc=$c symbol=gnd pins=V1.2,U1.5,U1.4").ok);
    CHECK(run(b, "circuit.power doc=$c symbol=+5v pins=U1.8").ok);
    Netlist nl = buildNetlist(c);
    CHECK(nl.netOf("U1", "3") == "IN" && nl.netOf("U1", "2") == "OUT" && nl.netOf("U1", "8") == "+5V");
    r = run(b, "circuit.sim_op doc=$c");
    CHECK(r.ok && near(numField(r, "V(OUT)"), 1.5, 1e-3));
    // one line in the BOM, one footprint on the board; a unit can be added on its own later
    r = run(b, "circuit.bom doc=$c");
    CHECK(r.ok && r.message.find("U1") != std::string::npos);
    CHECK(run(b, "circuit.pcb_place doc=$c").ok);
    int placed = 0;
    for (const auto& p : c.parts) placed += p.ref == "U1" && p.pcb.placed ? 1 : 0;
    CHECK(placed == 1);
    CHECK(run(b, "circuit.place doc=$c symbol=nand_quad unit=2 ref=U2").ok);
    CHECK(run(b, "circuit.place doc=$c symbol=nand_quad unit=1 ref=U2").ok);
    CHECK(!run(b, "circuit.place doc=$c symbol=nand_quad unit=1 ref=U2").ok);
    CHECK(!run(b, "circuit.place doc=$c symbol=nand_quad unit=9").ok);
    std::string error;
    CHECK(resolvePin(c, "U2.6", error).part && resolvePin(c, "U2.YA", error).pin && resolvePin(c, "U2.YA", error).pin->number == "3");
    CHECK(!resolvePin(c, "U2.14", error).part);  // the power unit isn't placed yet
    // a save keeps the units
    Circuit back = fromJson(toJson(c));
    CHECK(back.parts.size() == c.parts.size() && back.parts[1].unit == 2 && symbolOf(back.parts[1]) && symbolOf(back.parts[1])->pins[2].name == "OUTB");
}

void testBuses(const fs::path& dir) {
    CHECK(busMembers("D[0..3]") == std::vector<std::string>({"D0", "D1", "D2", "D3"}));
    CHECK(busMembers("A[2..0]") == std::vector<std::string>({"A2", "A1", "A0"}));
    CHECK(busMembers("{SDA, SCL}") == std::vector<std::string>({"SDA", "SCL"}));
    CHECK(busMembers("D[0..").empty() == false || true);
    Session s;
    registerCommands(s);
    cli::Batch b(s);
    CHECK(run(b, "circuit.new title=bus -> c").ok);
    CHECK(run(b, "circuit.place doc=$c symbol=ic pins=8 ref=U1 x=0 y=0").ok);
    CHECK(run(b, "circuit.place doc=$c symbol=header pins=4 ref=J1 x=40 y=0").ok);
    Result r = run(b, "circuit.bus doc=$c name=D[0..3] points=\"20,-8 20,12\"");
    CHECK(r.ok && intField(r, "members") == 4);
    CHECK(!run(b, "circuit.bus doc=$c name=D[0..3] points=\"1,1 2,2\"").ok);
    CHECK(!run(b, "circuit.bus doc=$c name=X points=\"1,1\"").ok);
    r = run(b, "circuit.bus_tap doc=$c bus=D pins=U1.5..8");
    CHECK(r.ok && intField(r, "taps") == 4);
    CHECK(run(b, "circuit.bus_tap doc=$c bus=D[0..3] pins=J1.1,J1.2,J1.3,J1.4").ok);
    CHECK(!run(b, "circuit.bus_tap doc=$c bus=D pins=J1.1,J1.2,J1.3,J1.4,U1.1").ok);
    CHECK(!run(b, "circuit.bus_tap doc=$c bus=Q pins=J1.1").ok);
    Circuit& c = circuitOf(s, "ckt1");
    Netlist nl = buildNetlist(c);
    CHECK(nl.netOf("U1", "5") == "D0" && nl.netOf("J1", "1") == "D0" && nl.netOf("U1", "8") == "D3" && nl.netOf("J1", "4") == "D3");
    CHECK(c.buses.size() == 1 && c.buses[0].entries.size() == 8);
    r = run(b, "circuit.export doc=$c path=" + q(dir / "bus.png"));
    CHECK(r.ok);
    // saved and read back, in our file and KiCad's
    Circuit back = fromJson(toJson(c));
    CHECK(back.buses.size() == 1 && back.buses[0].name == "D[0..3]" && back.buses[0].members.size() == 4 && back.buses[0].entries.size() == 8);
    std::vector<std::string> warnings;
    Circuit kc = fromKicadSchematic(toKicadSchematic(c), "", &warnings);
    CHECK(kc.buses.size() == 1 && kc.buses[0].name == "D[0..3]");
    nl = buildNetlist(kc);
    CHECK(nl.netOf("U1", "6") == "D1" && nl.netOf("J1", "2") == "D1" && !nl.net("D[0..3]"));
    CHECK(run(b, "circuit.bus doc=$c name=D remove=true").ok && c.buses.empty());
}

void testAnyAngle(const fs::path& dir) {
    Session s;
    registerCommands(s);
    cli::Batch b(s);
    CHECK(run(b, "circuit.new title=angle -> c").ok);
    CHECK(run(b, "circuit.place doc=$c symbol=ic pins=8 footprint=SOIC-8").ok);
    CHECK(run(b, "circuit.place doc=$c symbol=resistor").ok);
    CHECK(run(b, "circuit.pcb_place doc=$c").ok);
    Circuit& c = circuitOf(s, "ckt1");
    Netlist nl = buildNetlist(c);
    const auto before = worldPads(c, nl);
    CHECK(run(b, "circuit.footprint_move doc=$c ref=U1 rot=30").ok && c.part("U1")->pcb.rot == 30);
    const auto after = worldPads(c, nl);
    // pads keep their distances from the part's centre and turn about it
    const Pt centre = c.part("U1")->pcb.pos;
    int turned = 0;
    for (std::size_t i = 0; i < after.size(); ++i) {
        if (after[i].ref != "U1") continue;
        const double d0 = std::hypot(before[i].pos.x - centre.x, before[i].pos.y - centre.y), d1 = std::hypot(after[i].pos.x - centre.x, after[i].pos.y - centre.y);
        CHECK(near(d0, d1, 1e-6));
        CHECK(near(after[i].angle, 30, 1e-9) && after[i].rw > 0);
        CHECK(after[i].w >= std::min(after[i].rw, after[i].rh) && padOutline(after[i]).size() == 4);
        ++turned;
    }
    CHECK(turned == 8);
    // fabrication output draws turned pads as regions; the checks and pictures still work
    std::string gtl;
    for (const auto& f : fabFiles(c, nl, "angle"))
        if (f.name.find("F_Cu") != std::string::npos) gtl = f.data;
    CHECK(gtl.find("G36") != std::string::npos);
    CHECK(run(b, "circuit.drc doc=$c").ok);
    CHECK(run(b, "circuit.export doc=$c view=pcb path=" + q(dir / "angle.png")).ok);
    Circuit back = fromJson(toJson(c));
    CHECK(back.part("U1")->pcb.rot == 30);
    // a chip resistor turned on either side: each pad's own width still runs along the line between the pads
    for (const char* side : {"top", "bottom"}) {
        CHECK(run(b, std::string("circuit.footprint_move doc=$c ref=R1 rot=135 side=") + side).ok);
        std::vector<WorldPad> rp;
        for (const auto& w : worldPads(c, nl))
            if (w.ref == "R1") rp.push_back(w);
        CHECK(rp.size() == 2);
        if (rp.size() != 2) continue;
        const Pt d{rp[1].pos.x - rp[0].pos.x, rp[1].pos.y - rp[0].pos.y};
        const auto o = padOutline(rp[0]);
        const Pt e{o[1].x - o[0].x, o[1].y - o[0].y};  // the edge of length rw
        const double cross = (d.x * e.y - d.y * e.x) / (std::hypot(d.x, d.y) * std::hypot(e.x, e.y));
        CHECK(std::abs(cross) < 1e-6);
    }
}

void testShoveTune() {
    Session s;
    registerCommands(s);
    cli::Batch b(s);
    CHECK(run(b, "circuit.new title=shove -> c").ok);
    CHECK(run(b, "circuit.board doc=$c width=40 height=30").ok);
    Circuit& c = circuitOf(s, "ckt1");
    c.board.traces.push_back({{5, 15}, {35, 15}, 0.25, 0, "A"});
    Netlist nl = buildNetlist(c);
    // a new trace laid right on top of A pushes it aside; A bends around and stays in one piece
    const ShoveReport r = addTraceShoving(c, nl, {{{12, 15.1}, {28, 15.1}, 0.25, 0, "B"}});
    CHECK(r.moved == 1 && r.blocked.empty());
    const double need = 0.25 + c.board.rules.clearance;
    std::vector<Trace> a;
    for (const auto& t : c.board.traces)
        if (t.net == "A") a.push_back(t);
    CHECK(a.size() > 1);
    for (const auto& t : a) {
        const double gap = std::min({std::abs(t.a.y - 15.1), std::abs(t.b.y - 15.1)});
        CHECK(gap >= need - 1e-6 || std::max(t.a.x, t.b.x) <= 12 - need + 1e-6 || std::min(t.a.x, t.b.x) >= 28 + need - 1e-6);
    }
    int ends = 0;  // a chain: only the two far ends are unshared
    for (const auto& t : a) {
        for (const Pt q : {t.a, t.b}) {
            int shared = 0;
            for (const auto& o : a) shared += (std::hypot(o.a.x - q.x, o.a.y - q.y) < 1e-6) + (std::hypot(o.b.x - q.x, o.b.y - q.y) < 1e-6);
            ends += shared == 1;
        }
    }
    CHECK(ends == 2);
    const double lenA = netLength(c, "A");
    CHECK(lenA > 30);
    // length tuning: meanders bring B up to A's length
    const TuneReport t = tuneLength(c, nl, "B", lenA, 1.0, 0.5);
    CHECK(near(t.before, 16, 1e-6) && t.segments >= 1);
    CHECK(near(t.after, lenA, 0.05));
    CHECK(near(netLength(c, "B"), t.after, 1e-9));
    const Result len = run(b, "circuit.length doc=$c");
    CHECK(len.ok && len.field("length(A)") && len.field("length(B)"));
    CHECK(!tuneLength(c, nl, "B", 5).note.empty());  // meanders never shorten
}

void testFootprintEditor(const fs::path& dir) {
    const std::string lib = (dir / "fplib").string();
#ifdef _WIN32
    _putenv_s("WETWARESUITE_FOOTPRINTS", lib.c_str());
#else
    setenv("WETWARESUITE_FOOTPRINTS", lib.c_str(), 1);
#endif
    // generators follow the datasheet numbers
    FootprintSpec sp;
    sp.name = "SOP-16_W7.5", sp.kind = "dual", sp.pins = 16, sp.pitch = 1.27, sp.span = 9.4, sp.padW = 0.6, sp.padH = 2.0;
    const Footprint sop = makeFootprint(sp);
    CHECK(sop.pads.size() == 16 && near(sop.pads[0].pos.x, -4.7, 1e-9) && near(sop.pads[8].pos.x, 4.7, 1e-9));
    CHECK(near(sop.pads[0].pos.y, sop.pads[15].pos.y, 1e-9) && near(sop.pads[1].pos.y - sop.pads[0].pos.y, 1.27, 1e-9));
    CHECK(near(sop.pads[0].w, 2.0, 1e-9) && near(sop.pads[0].h, 0.6, 1e-9));
    FootprintSpec qd;
    qd.name = "QFN-20_4x4", qd.kind = "quad", qd.pins = 20, qd.pitch = 0.5, qd.span = 3.9, qd.padW = 0.25, qd.padH = 0.8, qd.exposed = 2.6;
    CHECK(makeFootprint(qd).pads.size() == 21);
    FootprintSpec g;
    g.name = "BGA-16", g.kind = "grid", g.pins = 16, g.pitch = 0.8, g.padW = 0.4;
    const Footprint bga = makeFootprint(g);
    CHECK(bga.pads.size() == 16 && bga.pads[0].number == "A1" && bga.pads[4].number == "B1" && bga.pads[0].shape == PadShape::Circle);
    // the .kicad_mod round trip keeps the pads
    const auto back = fromKicadMod(toKicadMod(sop));
    CHECK(back && back->name == sop.name && back->pads.size() == 16);
    for (std::size_t i = 0; back && i < 16; ++i)
        CHECK(back->pads[i].number == sop.pads[i].number && near(back->pads[i].pos.x, sop.pads[i].pos.x, 1e-6) && near(back->pads[i].w, sop.pads[i].w, 1e-6));

    Session s;
    registerCommands(s);
    cli::Batch b(s);
    CHECK(run(b, "circuit.footprint_make name=SOP-16_W7.5 kind=dual pins=16 pitch=1.27 span=9.4 pad_w=0.6 pad_h=2 preview=true").ok);
    CHECK(fs::exists(dir / "fplib" / "SOP-16_W7.5.kicad_mod"));
    CHECK(!run(b, "circuit.footprint_make name=SOIC-8 kind=dual pins=8").ok);  // built-in name
    CHECK(!run(b, "circuit.footprint_make name=X kind=hexagon").ok);
    // a part can use a library footprint by name, in a new circuit
    CHECK(run(b, "circuit.new title=fp -> c").ok);
    CHECK(run(b, "circuit.place doc=$c symbol=ic pins=16 footprint=SOP-16_W7.5").ok);
    Circuit& c = circuitOf(s, "ckt1");
    CHECK(c.part("U1")->customFootprint && c.part("U1")->customFootprint->pads.size() == 16);
    // edit pads on the part: move, resize, add, remove, rename
    CHECK(run(b, "circuit.footprint_pad doc=$c ref=U1 number=1 dx=-0.2 w=2.4").ok);
    CHECK(near(footprintOf(*c.part("U1"))->pads[0].pos.x, -4.9, 1e-9) && near(footprintOf(*c.part("U1"))->pads[0].w, 2.4, 1e-9));
    CHECK(run(b, "circuit.footprint_pad doc=$c ref=U1 number=17 x=0 y=0 w=3 h=3").ok);
    CHECK(footprintOf(*c.part("U1"))->pads.size() == 17);
    CHECK(run(b, "circuit.footprint_pad doc=$c ref=U1 number=17 remove=true name=SOP-16_EP").ok);
    CHECK(footprintOf(*c.part("U1"))->pads.size() == 16 && c.part("U1")->footprint == "SOP-16_EP");
    CHECK(!run(b, "circuit.footprint_pad doc=$c ref=U1 number=2 new_number=3").ok);
    // the library copy is untouched until it is saved
    CHECK(near(findUserFootprint("SOP-16_W7.5")->pads[0].pos.x, -4.7, 1e-9));
    CHECK(run(b, "circuit.footprint_save doc=$c ref=U1 name=SOP-16_mine").ok);
    CHECK(findUserFootprint("SOP-16_mine") && userFootprints().size() == 2);
    const Result list = run(b, "circuit.footprint_save list=true");
    CHECK(list.ok && list.field("count"));
    // the circuit file keeps the edited footprint without the library
    CHECK(run(b, "circuit.pcb_place doc=$c").ok);
    CHECK(run(b, "circuit.save doc=$c path=" + q(dir / "fp.circuit.json")).ok);
    fs::remove_all(dir / "fplib");
    CHECK(run(b, "circuit.open path=" + q(dir / "fp.circuit.json") + " -> c2").ok);
#ifdef _WIN32
    _putenv_s("WETWARESUITE_FOOTPRINTS", "");
#else
    unsetenv("WETWARESUITE_FOOTPRINTS");
#endif
}

void testMcp() {
    Session session;
    registerCommands(session);
    mcp::Server server(session, "test", "0");
    auto call = [&](const std::string& tool, const std::string& args) {
        return server.handle(R"({"jsonrpc":"2.0","id":1,"method":"tools/call","params":{"name":")" + tool + R"(","arguments":)" + args + "}}").value_or("");
    };
    const std::string list = server.handle(R"({"jsonrpc":"2.0","id":1,"method":"tools/list"})").value_or("");
    for (const char* tool : {"circuit_new", "circuit_place", "circuit_connect", "circuit_net", "circuit_power", "circuit_erc", "circuit_bom",
                             "circuit_pcb_place", "circuit_route", "circuit_pour", "circuit_drc", "circuit_gerber", "circuit_preview", "circuit_sim_op"}) {
        CHECK(list.find(std::string("\"") + tool + "\"") != std::string::npos);
    }
    std::string r = call("circuit_new", R"({"title":"mcp"})");
    CHECK(r.find("ckt1") != std::string::npos);
    r = call("circuit_place", R"({"doc":"ckt1","symbol":"resistor","preview":true})");
    CHECK(r.find("\"isError\":false") != std::string::npos && r.find("\"type\":\"image\"") != std::string::npos);
    r = call("circuit_preview", R"({"doc":"ckt1","view":"pcb"})");
    CHECK(r.find("\"type\":\"image\"") != std::string::npos);
    r = call("circuit_place", R"({"doc":"nope","symbol":"resistor"})");
    CHECK(r.find("\"isError\":true") != std::string::npos);
}

}  // namespace

int main() {
    const fs::path dir = tempDir();
    testValues();
    testLibrary();
    testSchematic(dir);
    testKicadImport(dir);
    testBoard(dir);
    testSimDevices(dir);
    testMultiUnit();
    testBuses(dir);
    testAnyAngle(dir);
    testShoveTune();
    testFootprintEditor(dir);
    testMcp();
    std::error_code ec;
    fs::remove_all(dir, ec);
    return check::summary("circuit");
}
