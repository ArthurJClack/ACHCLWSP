// circuit: headless schematics and PCB layout. A circuit document holds a
// schematic (parts placed from a built-in symbol library, wires, net labels,
// no-connect markers) and a two-layer board (outline, footprints placed from
// the schematic, traces, vias, copper pours). Connectivity is geometric, as in
// every schematic editor: pins, wire ends and labels that touch are one net.
//
// Units: schematic coordinates are grid units of 0.1 inch (2.54 mm), y down;
// PCB coordinates are millimetres, y down (flipped to y up in Gerber files).
#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "wetwaresuite/core/command.hpp"
#include "wetwaresuite/pdf/page.hpp"
#include "wetwaresuite/raster/image.hpp"

namespace wetwaresuite::circuit {

struct Pt {
    double x = 0, y = 0;
    bool operator==(const Pt&) const = default;
};

// ---- symbol library -------------------------------------------------------------------

enum class PinType { Passive, Input, Output, Bidirectional, PowerIn, PowerOut, OpenCollector, NotConnected };
const char* toString(PinType type);

struct PinDef {
    std::string number, name;  // name may hold aliases split by '/', e.g. "D1/TX"
    Pt pos;                    // connection point, symbol space
    Pt end;                    // where the pin line meets the body
    PinType type = PinType::Passive;
};

struct Shape {
    enum Kind { Poly, Circle, Rect, Text } kind = Poly;
    std::vector<Pt> pts;  // Poly: points; Circle: centre; Rect: two corners; Text: anchor (baseline centre)
    double r = 0;
    bool closed = false, fill = false;
    std::string text;
    double size = 0.7;  // text height in grid units
};

struct Symbol {
    std::string name, title, category, prefix, value, footprint;
    std::vector<PinDef> pins;
    std::vector<Shape> shapes;
    bool power = false;          // a power/ground symbol: its value names a global net
    bool module = false;         // a dev board: same-named pins are joined, unused pins are fine
    bool joinSameName = false;   // an IC whose repeated power pins (GND, VCC) are one net, as KiCad's stacked pins
    bool inBom = true;           // false for mounting holes, test points ...
    bool showPinNumbers = false;
    bool showPinNames = false;
    Pt refPos{0, -1.2}, valuePos{0, 1.6};
    double minX = -1, minY = -1, maxX = 1, maxY = 1;  // bounds with pins (filled in by the library)
    double bodyMinX = 0, bodyMinY = 0, bodyMaxX = 0, bodyMaxY = 0;  // drawing only, without pin lines
};

struct SymbolEntry {
    std::string name, category, description, aliases;
};
const std::vector<SymbolEntry>& symbolCatalog();
// Library symbol by name or alias. `pins` sizes ic/header/header2; `pinNames` names an ic's pins.
// Returns null when unknown; `error` says why.
std::shared_ptr<const Symbol> findSymbol(const std::string& name, int pins = 0, const std::vector<std::string>& pinNames = {},
                                         std::string* error = nullptr);
// The canonical library name for a name or alias ("R" -> "resistor"), empty if unknown.
std::string canonicalSymbol(const std::string& name);
// Multi-unit library parts (opamp_dual, opamp_quad, nand_quad, inverter_hex): how many units
// (0 for an ordinary symbol, the power unit counted), and one unit's symbol (1-based).
int symbolUnits(const std::string& name);
std::shared_ptr<const Symbol> findSymbolUnit(const std::string& name, int unit);

// ---- footprints ---------------------------------------------------------------------

enum class PadShape { Rect, Circle, Oval };

struct PadDef {
    std::string number;
    Pt pos;  // mm, footprint space
    double w = 1, h = 1;
    PadShape shape = PadShape::Rect;
    double drill = 0;  // > 0: through hole on every copper layer; 0: SMD on the part's side
    bool plated = true;  // false: a non-plated hole (mounting hole), no copper
};

struct Footprint {
    std::string name, description;
    std::vector<PadDef> pads;
    std::vector<std::pair<Pt, Pt>> silk;  // silkscreen lines, mm
    double x0 = -1, y0 = -1, x1 = 1, y1 = 1;  // courtyard
};

struct FootprintEntry {
    std::string name, description;
};
const std::vector<FootprintEntry>& footprintCatalog();
// "R_0805", "SOIC-8", "DIP-14", "PinHeader_1x04", "PinHeader_2x10", ... Null when unknown.
std::shared_ptr<const Footprint> findFootprint(const std::string& name);
// A boxed symbol with one pin per (number, name), half left and half right, for parts that come
// from a board file with no schematic.
Symbol makeBoxSymbol(const std::string& name, const std::string& prefix, const std::string& value,
                     const std::vector<std::pair<std::string, std::string>>& pins);
// Fills in a symbol's bounds after its shapes and pins are set.
void finishSymbol(Symbol& s);
// Same for a footprint's courtyard.
void finishFootprint(Footprint& f);

// ---- the document model ---------------------------------------------------------------

struct PcbPlace {
    bool placed = false;
    Pt pos;           // mm
    int rot = 0;      // degrees counter-clockwise, any whole degree (0..359)
    bool bottom = false;
};

struct Part {
    std::string ref, symbol, value, footprint;
    int pins = 0;                        // parameterised symbols: pin count
    std::vector<std::string> pinNames;   // ic: pin names
    Pt pos;                              // grid units
    int rot = 0;                         // degrees counter-clockwise, multiples of 90
    bool mirror = false;
    std::map<std::string, std::string> fields;  // mpn, note, ... (go to the BOM)
    PcbPlace pcb;
    // a symbol or footprint that isn't in the built-in library (read from a KiCad file); `symbol` and
    // `footprint` then hold their names. Shared and never changed, so copies (undo) stay cheap.
    std::shared_ptr<const Symbol> customSymbol;
    std::shared_ptr<const Footprint> customFootprint;
    int unit = 0;  // multi-unit KiCad parts: 1, 2 ... share one ref (and one footprint, on unit 1)
};

struct Wire {
    std::vector<Pt> pts;  // polyline, grid units
};

// A bus: a thick line that groups related signals (D0..D7). It is a drawing aid, as in KiCad: what
// connects is the net labels on its members, which bus_tap puts on each pin with an entry line.
struct Bus {
    std::string name;                  // as written, e.g. D[0..7]
    std::vector<std::string> members;  // expanded: D0, D1, ...
    std::vector<Pt> pts;               // polyline, grid units
    std::vector<std::pair<Pt, Pt>> entries;  // short lines from member labels to the bus
};
// "D[0..7]" -> D0..D7, "A[3..0]" -> A3..A0, "{SDA,SCL}" or "SDA,SCL" -> SDA, SCL.
std::vector<std::string> busMembers(const std::string& name);

struct Label {
    std::string name;
    Pt pos;
    std::string kind;  // "" (local) or "global": both join every label of the same name (one sheet)
};

// Copper layers: 0 = F.Cu (top), 1 = B.Cu (bottom), 2 = In1.Cu, 3 = In2.Cu (4-layer boards).
// Layer masks use bit (1 << layer); through holes and vias reach every layer (kAllCopper).
constexpr int kAllCopper = 0xF;
inline int layerBit(int layer) { return 1 << layer; }
const char* layerName(int layer);  // "F.Cu", "B.Cu", "In1.Cu", "In2.Cu"
const char* layerShort(int layer);  // "F", "B", "In1", "In2"
int parseLayer(const std::string& text);  // F/top/F.Cu, B/bottom, In1, In2; -1 if unknown

struct Trace {
    Pt a, b;
    double width = 0.25;
    int layer = 0;  // see layerName
    std::string net;
};

struct Via {
    Pt pos;
    double drill = 0.4, diameter = 0.8;
    std::string net;
};

struct Zone {
    std::string net;
    int layer = 1;
    double clearance = 0.3;
    double minWidth = 0.25;
    bool thermal = true;  // join pads of its net through four spokes (thermal relief) instead of solidly
    double spoke = 0.5;   // spoke width, mm
};

struct Rules {
    double trace = 0.25;        // default trace width
    double powerTrace = 0.5;    // width for nets named like power (GND, VCC, +5V, ...)
    double clearance = 0.2;
    double minTrace = 0.15;
    double viaDrill = 0.4, viaDiameter = 0.8;
    double minDrill = 0.3, minAnnular = 0.13;
    double edgeClearance = 0.3;
    double grid = 0.127;  // autorouter grid
};

struct Board {
    int layers = 2;           // copper layers: 2 or 4
    std::vector<Pt> outline;  // closed polygon, mm; empty = no board yet
    std::vector<Trace> traces;
    std::vector<Via> vias;
    std::vector<Zone> zones;
    Rules rules;
};

struct Circuit {
    std::string title;
    std::vector<Part> parts;
    std::vector<Wire> wires;
    std::vector<Label> labels;
    std::vector<Pt> noConnects;
    std::vector<Bus> buses;
    Board board;

    Part* part(const std::string& ref);
    const Part* part(const std::string& ref) const;
};

// The symbol a part draws with (cached by the library).
std::shared_ptr<const Symbol> symbolOf(const Part& part);
std::shared_ptr<const Footprint> footprintOf(const Part& part);

// Symbol space -> schematic space for a part.
Pt toSheet(const Part& part, Pt local);
Pt pinPos(const Part& part, const PinDef& pin);

struct PinRef {
    const Part* part = nullptr;
    const PinDef* pin = nullptr;
    Pt pos;
};
// "R1.1", "U1.VCC", "A1.D13" (number, name or one of a name's '/' aliases). Null part on error.
PinRef resolvePin(const Circuit& c, const std::string& text, std::string& error);

// Next free designator for a prefix: "R" -> "R3".
std::string nextRef(const Circuit& c, const std::string& prefix);

// Value with SI suffix: "4k7" -> 4700, "100n" -> 1e-7, "1meg" -> 1e6, "5V" -> 5. Nullopt if not a number.
std::optional<double> parseValue(const std::string& text);
std::string formatValue(double v);  // 4700 -> "4.7k"

// ---- connectivity ---------------------------------------------------------------------

struct NetNode {
    std::string ref, pin, pinName;
    PinType type = PinType::Passive;
};

struct Net {
    std::string name;
    std::vector<NetNode> nodes;         // pins of real parts (power symbols excluded)
    std::vector<std::string> names;     // every label/power name on it
    std::vector<std::string> powerNames;
    int wires = 0;                      // wire segments on the net
    bool named = false;                 // named by a label or power symbol
};

struct Netlist {
    std::vector<Net> nets;
    std::map<std::string, int> pinNet;  // "R1.1" -> index into nets (pins of every part)
    std::vector<std::string> isolatedPins;  // pins nothing touches (power symbols included)
    std::vector<std::string> danglingWireEnds;  // "x,y" of wire ends that touch nothing

    const Net* net(const std::string& name) const;
    std::string netOf(const std::string& ref, const std::string& pin) const;  // "" if none
};

Netlist buildNetlist(const Circuit& c);

struct Issue {
    std::string severity;  // error, warning or info
    std::string code;      // unconnected_pin, shorted_power, duplicate_ref, ...
    std::string message;
};

std::vector<Issue> erc(const Circuit& c, const Netlist& nl);

// One row per value+footprint (grouped) or per part: Item,Qty,References,Value,Footprint,Symbol,+fields.
std::string bomCsv(const Circuit& c, bool grouped);

// Nets as text, KiCad s-expression netlist (.net), or JSON.
std::string netlistText(const Circuit& c, const Netlist& nl);
std::string kicadNetlist(const Circuit& c, const Netlist& nl);
std::string netlistJson(const Circuit& c, const Netlist& nl);

// A circuit from a KiCad .net netlist: parts mapped to library symbols where the
// name matches (R, C, LED, ...), others as generic ICs, laid out on a grid and
// joined with net labels. `warnings` collects what didn't map.
Circuit fromKicadNetlist(const std::string& text, std::vector<std::string>* warnings);

// KiCad 6/7/8 files. Reading keeps KiCad's own symbol and footprint drawings (as custom symbols and
// footprints on the parts); hierarchical sheets are flattened onto one sheet. `path` lets the schematic
// reader find sub-sheet files. They throw std::runtime_error on files they can't read.
Circuit fromKicadSchematic(const std::string& text, const std::string& path, std::vector<std::string>* warnings);
// A board on its own: parts get a boxed symbol whose pins are labelled with the pads' nets.
Circuit fromKicadPcb(const std::string& text, std::vector<std::string>* warnings);
// Puts a KiCad board (footprints, tracks, vias, zones, outline) onto a circuit whose parts match by reference.
void mergeKicadPcb(Circuit& c, const std::string& text, std::vector<std::string>* warnings);
std::string toKicadSchematic(const Circuit& c);
std::string toKicadPcb(const Circuit& c);
// One KiCad footprint file (.kicad_mod), both ways.
std::shared_ptr<const Footprint> fromKicadMod(const std::string& text, std::vector<std::string>* warnings = nullptr);
std::string toKicadMod(const Footprint& fp);

// ---- footprint editor ------------------------------------------------------------------
// A footprint made from a few numbers, the way package datasheets give them.
struct FootprintSpec {
    std::string name, kind;  // kind: smd2, row, dual, quad, grid, blank
    int pins = 2;
    double pitch = 1.27;     // pad to pad, mm
    double span = 5.4;       // dual: row centre to row centre; quad: across; grid: unused
    double padW = 0.6, padH = 1.5;  // dual/quad pads are padH long, pointing away from the body
    double drill = 0;        // > 0: through hole
    PadShape shape = PadShape::Rect;
    int columns = 0;         // grid: per row (default: square)
    double exposed = 0;      // quad: centre pad size (0: none)
};
Footprint makeFootprint(const FootprintSpec& spec);
// The user's own footprint library: a folder of .kicad_mod files (WETWARESUITE_FOOTPRINTS, or
// "footprints" next to the suite's settings). Looked up after the built-in footprints.
std::filesystem::path userFootprintDir();
std::shared_ptr<const Footprint> findUserFootprint(const std::string& name);
std::vector<std::string> userFootprints();
// Writes a footprint into the user library; returns the file.
std::filesystem::path saveUserFootprint(const Footprint& fp);

// ---- schematic editing helpers ------------------------------------------------------

// Adds a wire from a to b with one bend (horizontal first unless verticalFirst).
void addWire(Circuit& c, Pt a, Pt b, bool verticalFirst = false);
// Moves a part; wire ends that touched its pins follow (with a bend added when needed).
void movePart(Circuit& c, Part& part, Pt pos, int rot, bool mirror);
// True when nothing (pin, wire, label, no-connect mark) touches the point.
bool sheetPointFree(const Circuit& c, Pt p);
// Joins a pin to a named net: a one-unit stub wire away from the body and a label on its end.
void labelPin(Circuit& c, const Part& part, const PinDef& pin, const std::string& name);
// Removes wires, labels and markers touching nothing but this part's pins, then the part.
void removePart(Circuit& c, const std::string& ref);
// Bounding box of everything on the sheet, grid units.
void sheetBounds(const Circuit& c, double& x0, double& y0, double& x1, double& y1);

// ---- PCB ----------------------------------------------------------------------------

struct WorldPad {
    std::string ref, number, net;
    Pt pos;
    double w = 1, h = 1;  // after rotation
    PadShape shape = PadShape::Rect;
    double drill = 0;
    int layers = 1;  // layer mask: bit 0 = top, bit 1 = bottom, bits 2, 3 = inner layers
    bool plated = true;  // false: a non-plated hole (no copper, no net)
    // a part turned by something other than a multiple of 90: the pad's own size and its turn left over
    // after the nearest quarter turn (w, h above are then its upright bounding box, for clearances)
    double angle = 0, rw = 0, rh = 0;
};
// The outline of a pad as a polygon (exact for turned pads; arcs as short segments).
std::vector<Pt> padOutline(const WorldPad& pad, double grow = 0);

std::vector<WorldPad> worldPads(const Circuit& c, const Netlist& nl);
Pt toBoard(const PcbPlace& place, Pt local);  // footprint space -> board space

bool isPowerName(const std::string& net);
double boardWidth(const Board& b);
double boardHeight(const Board& b);
void boardBounds(const Board& b, double& x0, double& y0, double& x1, double& y1);
bool insideBoard(const Board& b, Pt p);

// Footprints of every part not yet placed (all of them when replaceAll) go onto the board in rows,
// grouped by connectivity. Makes a board to fit when there is none. Returns how many were placed.
int placeFootprints(Circuit& c, const Netlist& nl, bool replaceAll, double spacing);

struct Airwire {
    std::string net;
    Pt a, b;
    std::string from, to;  // "R1.1"
};
// Unrouted connections: per net, the shortest links joining its copper islands.
std::vector<Airwire> ratsnest(const Circuit& c, const Netlist& nl);

// Filled copper of one zone: a grid of cells, 1 = copper.
struct ZoneFill {
    int zone = 0, layer = 1;
    std::string net;
    double x0 = 0, y0 = 0, cell = 0.1;
    int nx = 0, ny = 0;
    std::vector<std::uint8_t> cells;
    bool at(int x, int y) const { return x >= 0 && y >= 0 && x < nx && y < ny && cells[static_cast<std::size_t>(y) * nx + x]; }
};
std::vector<ZoneFill> fillZones(const Circuit& c, const Netlist& nl);

struct RouteOptions {
    std::vector<std::string> nets;  // empty = every net with unrouted links
    std::vector<std::string> skip;  // nets to leave alone (e.g. GND under a pour)
    int layers = 2;                 // 1 = top only
    double viaCost = 8;             // in grid steps
    int maxSeconds = 60;
};
struct RouteReport {
    int connections = 0, routed = 0, failed = 0, segments = 0, vias = 0;
    std::vector<std::string> failedLinks;  // "GND: R1.2 -> C1.2"
};
RouteReport autoroute(Circuit& c, const Netlist& nl, const RouteOptions& options);

std::vector<Issue> drc(const Circuit& c, const Netlist& nl);

// A trace and a via from every SMD pad of the nets (default: nets with a pour) so they reach a plane
// on another layer. Pads a plane already reaches, or that already have a via, are left alone.
struct FanoutReport {
    int vias = 0, already = 0;
    std::vector<std::string> failed;  // "U1.4 (GND)"
};
FanoutReport fanout(Circuit& c, const Netlist& nl, const std::vector<std::string>& nets);

// Push-and-shove: adds the trace segments, pushing traces of other nets on the same layer aside
// (they bend around with 45-degree jogs and stay connected; pushed traces push others, maxDepth deep).
// Pads and vias don't move; a push that would hit one is skipped and listed in `blocked`.
struct ShoveReport {
    int moved = 0;
    std::vector<std::string> blocked;
};
ShoveReport addTraceShoving(Circuit& c, const Netlist& nl, const std::vector<Trace>& segs, double maxDepth = 3);

// Copper length of a net (its traces), mm.
double netLength(const Circuit& c, const std::string& net);
// Adds meanders (trombone bumps of up to `amplitude` mm, `spacing` mm apart) to a net's straight traces
// until it is `target` mm long, where they fit without touching other copper.
struct TuneReport {
    double before = 0, after = 0;
    int segments = 0;  // traces meandered
    std::string note;
};
TuneReport tuneLength(Circuit& c, const Netlist& nl, const std::string& net, double target, double amplitude = 1.0, double spacing = 0.5);

struct OutFile {
    std::string name, data;
};
// Gerber RS-274X (X2 attributes) for copper, mask, paste, silk and edge cuts, plus an Excellon drill file.
std::vector<OutFile> fabFiles(const Circuit& c, const Netlist& nl, const std::string& base);
// Pick-and-place (CPL) CSV: Designator,Val,Package,Mid X,Mid Y,Rotation,Layer in mm, y up; origin at the
// board's bottom-left corner (or the board's own origin when bottomLeftOrigin is false).
std::string placementCsv(const Circuit& c, bool bottomLeftOrigin = true);

// ---- drawing ------------------------------------------------------------------------

struct SheetStyle {
    bool grid = false;
    bool pinNumbers = true;
    bool titleBlock = true;
    std::string highlightNet;
    // the window's sheet: at least 60 x 40 grid units from the origin, so the first parts placed
    // don't change its size (and the picture under the mouse doesn't jump)
    bool roomy = false;
};
// The schematic as a page in points (1 grid unit = 7.2 pt, i.e. true size), for PDF, PNG and SVG.
pdf::Page schematicPage(const Circuit& c, const SheetStyle& style = {});
std::string pageSvg(const pdf::Page& page);

struct PcbStyle {
    bool top = true, bottom = true, inner = true, silk = true, ratsnest = true, zones = true, labels = false;
    bool mirror = false;  // view from below
    std::string highlightNet;
};
raster::Image renderPcb(const Circuit& c, const Netlist& nl, double pxPerMm, const PcbStyle& style = {});
std::string pcbSvg(const Circuit& c, const Netlist& nl, const PcbStyle& style = {});

// The assembled board in 3D (boxes for parts, by package), orthographic, flat shaded.
struct View3d {
    double yaw = -25;        // degrees, turning the board counter-clockwise
    double elevation = 50;   // degrees above the board (90 = straight down)
    bool fromBelow = false;
    unsigned maskColor = 0x1d6b3c;  // solder mask (green); 0x202020 black, 0x1a3f9a blue, 0x8a1a1a red
    unsigned background = 0x1b1d22;
};
raster::Image renderBoard3d(const Circuit& c, const Netlist& nl, int width, int height, const View3d& view = {});
// The assembled board as solids (board with its mounting holes, parts as boxes) for a CAD app, encoded
// by extension: .step/.stp (one solid per body), .stl, .obj, .glb, .3mf. Millimetres, y up.
std::string boardModel(const Circuit& c, const Netlist& nl, const std::string& ext);

// The window's picture of a circuit (CircuitDoc::preview): view "schematic", "pcb" or "3d", and how
// its pixels map back: model point = origin + pixel / scale (grid units on the sheet, mm on the board;
// scale 0 for 3d, which can't be clicked).
struct ViewFrame {
    std::string view = "schematic";
    double scale = 1;
    Pt origin;
};
ViewFrame viewFrame(const Circuit& c, const std::string& view);
raster::Image viewImage(const Circuit& c, const std::string& view);

// Line strokes of a small vector font (silkscreen text), height h, starting at (x, baseline y).
std::vector<std::vector<Pt>> strokeText(const std::string& text, double x, double y, double h);
double strokeTextWidth(const std::string& text, double h);

// ---- simulation (SPICE-lite) ---------------------------------------------------------

struct SimOptions {
    double tstop = 0, step = 0;  // transient when tstop > 0
    std::vector<std::string> probes;  // net names to record (empty = all)
    bool ignoreUnsupported = false;
    // AC sweep (small signal around the operating point) when acStop > 0: log steps from acStart
    double acStart = 0, acStop = 0;
    int acPerDecade = 20;
};
struct SimResult {
    std::vector<std::string> nets;    // node names, ground excluded
    std::vector<double> voltages;     // operating point (or final values for transient)
    std::vector<std::pair<std::string, double>> currents;  // through sources, A (into + terminal)
    std::vector<double> times;
    std::vector<std::string> probes;
    std::vector<std::vector<double>> series;  // per probe, per time
    std::vector<std::string> warnings;
    int iterations = 0;
    // AC sweep: per probe, per frequency, gain in dB (against a 1 V source) and phase in degrees
    std::vector<double> freqs;
    std::vector<std::string> acProbes;
    std::vector<std::vector<double>> acMag, acPhase;
};
// DC operating point (tstop == 0), a fixed-step transient, and/or an AC sweep. Resistors,
// capacitors, inductors, diodes/LEDs, NPN/PNP transistors (Ebers-Moll: fields beta, br, is),
// N/P MOSFETs (square law: fields vth, k, lambda), switches, potentiometers, ideal op-amps,
// voltage/current sources and batteries; power symbols named like +5V become sources. Ground is
// the GND (or 0) net. Throws std::runtime_error.
SimResult simulate(const Circuit& c, const Netlist& nl, const SimOptions& options);
raster::Image plotSeries(const SimResult& r, int width, int height);
raster::Image plotBode(const SimResult& r, int width, int height);  // gain (dB) above phase, log frequency

// ---- files --------------------------------------------------------------------------

std::string toJson(const Circuit& c);
Circuit fromJson(const std::string& text);  // throws std::runtime_error

// ---- the document -------------------------------------------------------------------

class CircuitDoc : public Document {
public:
    std::string kind() const override { return "circuit"; }
    std::string summary() const override;
    std::unique_ptr<Attachment> preview() const override;
    Circuit circuit;
    std::string view = "schematic";  // what the window shows: schematic, pcb or 3d (circuit.view)
};

// One undo step: snapshots the circuit before the edit, restores it on undo or failure.
class CircuitEdit : public Action {
public:
    CircuitEdit(std::shared_ptr<CircuitDoc> doc, std::function<Result(CircuitDoc&)> fn) : doc_(std::move(doc)), fn_(std::move(fn)) {}
    Result apply() override;
    void revert() override;

private:
    std::shared_ptr<CircuitDoc> doc_;
    std::function<Result(CircuitDoc&)> fn_;
    Circuit before_;
};

// Registers every circuit.* command.
void registerCommands(Session& session);

}  // namespace wetwaresuite::circuit
