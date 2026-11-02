// circuit.* file, library and output commands.
#include "cmd_common.hpp"

#include <algorithm>

namespace wetwaresuite::circuit::cmd {

namespace {

class CloseAction : public Action {
public:
    CloseAction(Session& session, std::string id) : session_(session), id_(std::move(id)) {}
    Result apply() override {
        doc_ = session_.workspace().take(id_);
        if (!doc_) return Result::failure("no open document called '" + id_ + "'");
        return Result::success("closed " + id_);
    }
    void revert() override {
        if (doc_) session_.workspace().put(id_, doc_);
    }

private:
    Session& session_;
    std::string id_;
    std::shared_ptr<Document> doc_;
};

Result created(Session& session, std::shared_ptr<CircuitDoc> doc, const std::string& verb, const Params& p) {
    const std::string id = session.workspace().add(doc, "ckt");
    Result r = Result::success(verb + " " + id + ": " + doc->summary(), id);
    r.with("doc", id).with("parts", static_cast<std::int64_t>(doc->circuit.parts.size()));
    return maybePreview(r, *doc, p, View::Schematic);
}

SheetStyle sheetStyle(const Params& p) {
    SheetStyle s;
    s.grid = boolParam(p, "grid", false);
    s.highlightNet = stringParam(p, "highlight");
    s.pinNumbers = boolParam(p, "pin_numbers", true);
    return s;
}

PcbStyle pcbStyle(const Params& p) {
    PcbStyle s;
    const std::string layers = lower(stringParam(p, "layers", "both"));
    s.top = layers == "both" || layers == "all" || layers == "top" || layers == "f";
    s.bottom = layers == "both" || layers == "all" || layers == "bottom" || layers == "b";
    s.inner = layers == "both" || layers == "all" || layers == "inner";
    s.mirror = boolParam(p, "from_below", false);
    s.ratsnest = boolParam(p, "ratsnest", true);
    s.labels = boolParam(p, "net_names", false);
    s.highlightNet = stringParam(p, "highlight");
    return s;
}

std::vector<ParamSpec> viewParams() {
    return {{"view", ParamType::String, "what to draw: the schematic, the board's copper (pcb) or the assembled board in 3D (3d)", false,
             {"schematic", "pcb", "3d"}, std::string("schematic")},
            {"grid", ParamType::Bool, "schematic: draw the 0.1 in grid", false, {}, false},
            {"highlight", ParamType::String, "net name to draw in a highlight colour", false},
            {"layers", ParamType::String, "pcb: top, bottom, inner or both (all)", false, {"both", "top", "bottom", "inner"}, std::string("both")},
            {"from_below", ParamType::Bool, "pcb: mirror to look from the bottom side", false, {}, false},
            {"ratsnest", ParamType::Bool, "pcb: draw unrouted connections", false, {}, true},
            {"net_names", ParamType::Bool, "pcb: write net names on pads", false, {}, false},
            {"angle", ParamType::Number, "3d: turn the board by this many degrees", false, {}, -25.0},
            {"tilt", ParamType::Number, "3d: view elevation in degrees (90 = from straight above)", false, {}, 50.0},
            {"mask", ParamType::String, "3d: solder mask colour", false, {"green", "black", "blue", "red", "white", "purple"}, std::string("green")}};
}

View3d view3d(const Params& p) {
    View3d v;
    v.yaw = numberParam(p, "angle", -25);
    v.elevation = numberParam(p, "tilt", 50);
    v.fromBelow = boolParam(p, "from_below", false);
    const std::string m = lower(stringParam(p, "mask", "green"));
    if (m == "black") v.maskColor = 0x222222;
    else if (m == "blue") v.maskColor = 0x1a3f9a;
    else if (m == "red") v.maskColor = 0x9a1c1c;
    else if (m == "white") v.maskColor = 0xe8e8e8;
    else if (m == "purple") v.maskColor = 0x5a2a8a;
    return v;
}

Attachment board3dPng(const Circuit& c, const Params& p, int w, int h) {
    return {"image/png", raster::encodePng(renderBoard3d(c, buildNetlist(c), w, h, view3d(p))), "board3d.png"};
}

}  // namespace

void registerFileCommands(Session& session) {
    auto& reg = session.registry();
    reg.add({"circuit.new", "New Circuit",
             "Create an empty circuit (schematic + board) and return its id, e.g. ckt1. Then place parts (circuit.place), join pins "
             "(circuit.connect, circuit.net, circuit.power), check with circuit.erc, and lay out the board (circuit.board, "
             "circuit.pcb_place, circuit.route, circuit.drc, circuit.gerber).",
             {{"title", ParamType::String, "title shown in the title block", false}, previewParam()},
             false, true},
            [&session](const Params& p) -> std::unique_ptr<Action> {
                return std::make_unique<FunctionAction>([&session, p] {
                    auto doc = std::make_shared<CircuitDoc>();
                    doc->circuit.title = stringParam(p, "title");
                    return created(session, doc, "created", p);
                });
            });

    reg.add({"circuit.open", "Open Circuit",
             "Open a .circuit.json file or a KiCad 6/7/8 design: a schematic (.kicad_sch; sub-sheets are flattened onto one sheet and "
             "the .kicad_pcb beside it is loaded too), a board on its own (.kicad_pcb; parts get boxed symbols labelled with their "
             "nets), a project (.kicad_pro) or a netlist (.net: parts mapped to library symbols, laid out on a grid, joined with labels). "
             "KiCad's own symbol and footprint drawings are kept.",
             {{"path", ParamType::String, "file to open (.circuit.json, .json, .kicad_sch, .kicad_pcb, .kicad_pro or .net)", true},
              {"board", ParamType::Bool, ".kicad_sch: also load the .kicad_pcb of the same name when there is one", false, {}, true},
              previewParam()},
             false, true},
            [&session](const Params& p) -> std::unique_ptr<Action> {
                return std::make_unique<FunctionAction>([&session, p] {
                    try {
                        const fs::path path = stringParam(p, "path");
                        const std::string text = readFile(path);
                        auto doc = std::make_shared<CircuitDoc>();
                        std::vector<std::string> warnings;
                        const std::string ext = lower(path.extension().string());
                        fs::path sch = path;
                        if (ext == ".kicad_pro") sch = fs::path(path).replace_extension(".kicad_sch");
                        const std::string head = text.substr(0, std::min<std::size_t>(text.size(), 200));
                        if (ext == ".kicad_pro" || ext == ".kicad_sch" || head.find("(kicad_sch") != std::string::npos) {
                            doc->circuit = fromKicadSchematic(sch == path ? text : readFile(sch), sch.string(), &warnings);
                            const fs::path pcb = fs::path(sch).replace_extension(".kicad_pcb");
                            std::error_code ec;
                            if (boolParam(p, "board", true) && fs::exists(pcb, ec)) {
                                mergeKicadPcb(doc->circuit, readFile(pcb), &warnings);
                                warnings.insert(warnings.begin(), "board loaded from " + pcb.string());
                            }
                        } else if (ext == ".kicad_pcb" || head.find("(kicad_pcb") != std::string::npos) {
                            doc->circuit = fromKicadPcb(text, &warnings);
                            if (doc->circuit.title.empty()) doc->circuit.title = path.stem().string();
                        } else if (ext == ".net" || text.find("(export") != std::string::npos) {
                            doc->circuit = fromKicadNetlist(text, &warnings);
                        } else {
                            doc->circuit = fromJson(text);
                        }
                        doc->path = path.string();
                        Result r = created(session, doc, "opened " + path.string() + " as", p);
                        for (const auto& w : warnings) r.message += "\nwarning: " + w;
                        return r;
                    } catch (const std::exception& e) {
                        return Result::failure(e.what());
                    }
                });
            });

    reg.add({"circuit.save", "Save Circuit",
             "Save the circuit (schematic and board) as .circuit.json, or for KiCad 7+: .kicad_sch (the schematic, symbols "
             "embedded), .kicad_pcb (the board: footprints, tracks, vias, pours, outline) or .kicad_pro (a project with both).",
             {docParam(), {"path", ParamType::String, "output file, e.g. blinky.circuit.json or blinky.kicad_pro", true}}, false},
            circuitCommand(
                session,
                [](CircuitDoc& doc, const Params& p) {
                    const fs::path path = stringParam(p, "path");
                    const std::string ext = lower(path.extension().string());
                    std::string error;
                    std::vector<std::string> written;
                    auto put = [&](const fs::path& f, const std::string& data) {
                        if (!writeFile(f, data, error)) return false;
                        written.push_back(f.string());
                        return true;
                    };
                    bool ok = true;
                    if (ext == ".kicad_sch") {
                        ok = put(path, toKicadSchematic(doc.circuit));
                    } else if (ext == ".kicad_pcb") {
                        ok = put(path, toKicadPcb(doc.circuit));
                    } else if (ext == ".kicad_pro") {
                        const std::string name = path.filename().string();
                        ok = put(path, "{\n  \"meta\": {\n    \"filename\": \"" + name + "\",\n    \"version\": 1\n  }\n}\n") &&
                             put(fs::path(path).replace_extension(".kicad_sch"), toKicadSchematic(doc.circuit)) &&
                             put(fs::path(path).replace_extension(".kicad_pcb"), toKicadPcb(doc.circuit));
                    } else {
                        ok = put(path, toJson(doc.circuit));
                        doc.path = path.string();
                    }
                    if (!ok) return Result::failure(error);
                    std::string files;
                    for (const auto& f : written) files += (files.empty() ? "" : ", ") + f;
                    return edited("saved " + files, p).with("path", path.string());
                },
                View::Schematic, false));

    reg.add({"circuit.close", "Close Circuit", "Close an open circuit (undo reopens it).", {docParam()}, true},
            [&session](const Params& p) -> std::unique_ptr<Action> { return std::make_unique<CloseAction>(session, stringParam(p, "doc")); });

    reg.add({"circuit.info", "Circuit Info", "Describe a circuit: parts, nets, wires, board size, traces, vias and pours.", {docParam(), previewParam()},
             false, true},
            circuitCommand(
                session,
                [](CircuitDoc& doc, const Params& p) {
                    const Circuit& c = doc.circuit;
                    const Netlist nl = buildNetlist(c);
                    Result r = edited(doc.summary(), p);
                    if (!c.title.empty()) r.message = c.title + ": " + r.message;
                    int placed = 0, withFp = 0;
                    for (const auto& part : c.parts) {
                        if (!footprintOf(part)) continue;
                        ++withFp;
                        if (part.pcb.placed) ++placed;
                    }
                    if (withFp) r.message += "\nfootprints on the board: " + std::to_string(placed) + " of " + std::to_string(withFp);
                    if (c.board.outline.size() >= 3) r.message += "\ncopper layers: " + std::to_string(c.board.layers);
                    for (const auto& z : c.board.zones) r.message += "\npour: " + z.net + " on " + layerName(z.layer) + (z.thermal ? "" : " (solid pads)");
                    r.with("parts", static_cast<std::int64_t>(c.parts.size()))
                        .with("nets", static_cast<std::int64_t>(nl.nets.size()))
                        .with("wires", static_cast<std::int64_t>(c.wires.size()))
                        .with("labels", static_cast<std::int64_t>(c.labels.size()))
                        .with("board_width", boardWidth(c.board))
                        .with("board_height", boardHeight(c.board))
                        .with("traces", static_cast<std::int64_t>(c.board.traces.size()))
                        .with("vias", static_cast<std::int64_t>(c.board.vias.size()));
                    return r;
                },
                View::Schematic, false));

    reg.add({"circuit.symbols", "List Symbols",
             "The built-in symbol library: name, category, what the pins are and the default footprint. Use the name (or an alias "
             "such as R, C, LED, npn, uno) with circuit.place.",
             {{"category", ParamType::String, "only this category", false,
               {"passive", "semiconductor", "ic", "switch", "connector", "module", "power", "source"}}},
             false, true},
            [](const Params& p) -> std::unique_ptr<Action> {
                return std::make_unique<FunctionAction>([p] {
                    const std::string cat = stringParam(p, "category");
                    std::string s;
                    std::int64_t n = 0;
                    for (const auto& e : symbolCatalog()) {
                        if (!cat.empty() && e.category != cat) continue;
                        s += "\n" + e.name + " [" + e.category + "]: " + e.description + (e.aliases.empty() ? "" : " (aliases: " + e.aliases + ")");
                        ++n;
                    }
                    Result r = Result::success(std::to_string(n) + " symbols:" + s);
                    r.with("count", n);
                    return r;
                });
            });

    reg.add({"circuit.footprints", "List Footprints",
             "The built-in footprints (SMD 0603/0805/1206, SOT-23, SOIC-N, through-hole axial, radial, DIP-N, TO-92, TO-220, pin "
             "headers, switches, dev boards). Names with N take a pin count, e.g. SOIC-8, DIP-28, PinHeader_1x06. KiCad library "
             "names such as Resistor_SMD:R_0805_2012Metric are understood too, and so are footprints in your own library "
             "(circuit.footprint_make / footprint_save).",
             {}, false, true},
            [](const Params&) -> std::unique_ptr<Action> {
                return std::make_unique<FunctionAction>([] {
                    std::string s;
                    for (const auto& e : footprintCatalog()) s += "\n" + e.name + ": " + e.description;
                    const auto mine = userFootprints();
                    if (!mine.empty()) {
                        s += "\n\nyour library (" + userFootprintDir().string() + "):";
                        for (const auto& n : mine) s += "\n" + n;
                    }
                    return Result::success(std::to_string(footprintCatalog().size()) + " footprint families:" + s)
                        .with("user", static_cast<std::int64_t>(mine.size()));
                });
            });

    reg.add({"circuit.symbol_info", "Symbol Info",
             "Pins of a library symbol (number, name, type, position relative to the part origin in grid units) and its default "
             "footprint, with an optional picture.",
             {{"symbol", ParamType::String, "symbol name or alias", true},
              {"pins", ParamType::Int, "pin count for ic, header and header2", false},
              {"pin_names", ParamType::String, "ic: comma-separated pin names in pin order", false},
              previewParam()},
             false, true},
            [](const Params& p) -> std::unique_ptr<Action> {
                return std::make_unique<FunctionAction>([p] {
                    std::string error;
                    const auto names = has(p, "pin_names") ? splitList(stringParam(p, "pin_names")) : std::vector<std::string>{};
                    auto sym = findSymbol(stringParam(p, "symbol"), static_cast<int>(intParam(p, "pins", 0)), names, &error);
                    if (!sym) return Result::failure(error);
                    std::string s = sym->name + ": " + sym->title + "\nprefix " + sym->prefix + ", default value " + sym->value +
                                    ", footprint " + (sym->footprint.empty() ? "none (simulation only)" : sym->footprint) + "\npins:";
                    for (const auto& pin : sym->pins) {
                        s += "\n  " + pin.number + (pin.name != pin.number ? " " + pin.name : "") + " (" + toString(pin.type) + ") at " + pt(pin.pos);
                    }
                    Result r = Result::success(s);
                    r.with("pins", static_cast<std::int64_t>(sym->pins.size())).with("footprint", sym->footprint);
                    if (boolParam(p, "preview", false)) {
                        Circuit c;
                        Part part;
                        part.ref = sym->prefix + "1";
                        part.symbol = sym->name;
                        part.value = sym->value;
                        part.pins = static_cast<int>(intParam(p, "pins", 0));
                        part.pinNames = names;
                        if (sym->name == "power") part.symbol = stringParam(p, "symbol");
                        c.parts.push_back(part);
                        SheetStyle st;
                        st.titleBlock = false;
                        st.grid = true;
                        r.attach(schematicPng(c, st, 600));
                    }
                    return r;
                });
            });

    reg.add({"circuit.preview", "Preview Circuit",
             "Render the schematic or the board to a PNG without changing anything. view=pcb draws copper (top red, bottom blue, "
             "inner layers yellow and teal), pads, vias, silkscreen, pours and the ratsnest; view=3d draws the assembled board.",
             [] {
                 std::vector<ParamSpec> v = {docParam()};
                 for (auto& s : viewParams()) v.push_back(s);
                 v.push_back({"size", ParamType::Int, "longest side in pixels", false, {}, std::int64_t{1400}});
                 return v;
             }(),
             false, true},
            circuitCommand(
                session,
                [](CircuitDoc& doc, const Params& p) {
                    const double size = static_cast<double>(std::clamp<std::int64_t>(intParam(p, "size", 1400), 200, 6000));
                    Result r = edited("preview of " + stringParam(p, "doc"), p);
                    const std::string view = stringParam(p, "view", "schematic");
                    if (view == "3d") r.attach(board3dPng(doc.circuit, p, static_cast<int>(size), static_cast<int>(size * 0.72)));
                    else if (view == "pcb") r.attach(pcbPng(doc.circuit, pcbStyle(p), size));
                    else r.attach(schematicPng(doc.circuit, sheetStyle(p), size));
                    return r;
                },
                View::Schematic, false));

    reg.add({"circuit.export", "Export Drawing",
             "Write the schematic or the board as a picture: .svg, .pdf (schematic only) or .png (view=3d: the assembled board); "
             "the extension picks the format. .step/.stp, .stl, .obj, .glb or .3mf write the assembled board as solids (board with "
             "mounting holes, parts as boxes) to open in cad and design an enclosure around. For fabrication files use circuit.gerber, for KiCad circuit.save to .kicad_sch.",
             [] {
                 std::vector<ParamSpec> v = {docParam(), {"path", ParamType::String, "output file (.svg, .pdf, .png; .step/.stl/.obj/.glb/.3mf: the board in 3D)", true}};
                 for (auto& s : viewParams()) v.push_back(s);
                 v.push_back({"scale", ParamType::Number, "png: pixels per grid unit (schematic), per mm (pcb), or the width (3d)", false});
                 return v;
             }(),
             false},
            circuitCommand(
                session,
                [](CircuitDoc& doc, const Params& p) {
                    const fs::path path = stringParam(p, "path");
                    const std::string ext = lower(path.extension().string());
                    const bool pcb = stringParam(p, "view", "schematic") == "pcb";
                    std::string data;
                    const bool model = ext == ".step" || ext == ".stp" || ext == ".stl" || ext == ".obj" || ext == ".glb" || ext == ".3mf";
                    if (model) {  // the assembled board as solids, for designing an enclosure in cad
                        if (!doc.circuit.board.outline.size() && std::none_of(doc.circuit.parts.begin(), doc.circuit.parts.end(), [](const Part& q) { return q.pcb.placed; }))
                            return Result::failure("no board yet: circuit.board or circuit.pcb_place first");
                        data = boardModel(doc.circuit, buildNetlist(doc.circuit), ext);
                    } else if (stringParam(p, "view", "schematic") == "3d") {
                        if (ext != ".png") return Result::failure("the 3D view exports as .png (or the board as solids: .step, .stl, .obj, .glb, .3mf)");
                        const int w = has(p, "scale") ? static_cast<int>(std::clamp(numberParam(p, "scale"), 200.0, 8000.0)) : 2000;
                        data = board3dPng(doc.circuit, p, w, static_cast<int>(w * 0.72)).bytes;
                    } else if (pcb) {
                        const Netlist nl = buildNetlist(doc.circuit);
                        if (ext == ".svg") data = pcbSvg(doc.circuit, nl, pcbStyle(p));
                        else if (ext == ".png") data = raster::encodePng(renderPcb(doc.circuit, nl, has(p, "scale") ? numberParam(p, "scale") : pcbScale(doc.circuit, 2000), pcbStyle(p)));
                        else return Result::failure("the board exports as .svg or .png (Gerber via circuit.gerber)");
                    } else {
                        const pdf::Page page = schematicPage(doc.circuit, sheetStyle(p));
                        if (ext == ".svg") data = pageSvg(page);
                        else if (ext == ".pdf") {
                            pdf::DocInfo info;
                            info.title = doc.circuit.title;
                            data = pdf::writePdf({page}, info);
                        } else if (ext == ".png") {
                            const double scale = has(p, "scale") ? numberParam(p, "scale") / 7.2 : std::clamp(2400.0 / std::max(page.width, page.height), 1.0, 4.0);
                            data = raster::encodePng(pdf::renderPage(page, scale));
                        } else {
                            return Result::failure("the schematic exports as .svg, .pdf or .png");
                        }
                    }
                    std::string error;
                    if (!writeFile(path, data, error)) return Result::failure(error);
                    return edited("wrote " + path.string() + " (" + std::to_string(data.size()) + " bytes)", p).with("path", path.string());
                },
                View::Schematic, false));
}

}  // namespace wetwaresuite::circuit::cmd
