// board.* commands for arranging (align, distribute, layout, layers, themes),
// text to diagram (Mermaid, outlines) and looking (find, info, preview).
#include <algorithm>
#include <cmath>
#include <fstream>
#include <map>
#include <set>
#include <sstream>

#include "wetwaresuite/raster/codec.hpp"
#include "internal.hpp"

namespace wetwaresuite::board {

using namespace detail;

namespace {

namespace fs = std::filesystem;

bool has(const Params& p, const std::string& key) {
    auto it = p.find(key);
    return it != p.end() && !std::holds_alternative<std::monostate>(it->second);
}

std::string plural(std::size_t n, const std::string& word) { return std::to_string(n) + " " + word + (n == 1 ? "" : "s"); }

std::string joinList(const std::vector<std::string>& v) {
    std::string s;
    for (const auto& x : v) s += (s.empty() ? "" : ",") + x;
    return s;
}

ParamSpec docParam() { return {"doc", ParamType::String, "id of an open board, e.g. board1", true}; }
ParamSpec previewParam() { return {"preview", ParamType::Bool, "attach a PNG of the board to the result", false, {}, false}; }

Result edited(const std::string& message, const Params& p) {
    Result r = Result::success(message);
    r.with("doc", stringParam(p, "doc"));
    return r;
}

// Elements that move as one: an element inside a group moves with its outermost group.
std::vector<std::string> units(const Board& b, const std::vector<std::string>& ids) {
    std::vector<std::string> out;
    for (const auto& id : ids) {
        const Element* e = b.get(id);
        if (!e || e->kind == Kind::Connector) continue;
        std::string unit = e->id;
        const Element* cur = e;
        for (int d = 0; cur && !cur->parent.empty() && d < 32; ++d) {
            const Element* p = b.get(cur->parent);
            if (p && p->kind == Kind::Group && std::find(ids.begin(), ids.end(), p->id) == ids.end()) unit = p->id;
            cur = p;
        }
        if (std::find(out.begin(), out.end(), unit) == out.end()) out.push_back(unit);
    }
    return out;
}

std::string readText(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) throw std::runtime_error("can't read " + path.string());
    std::stringstream s;
    s << in.rdbuf();
    return s.str();
}

// ---- arrange --------------------------------------------------------------------------------------

void registerAlign(CommandRegistry& reg, Session& session) {
    reg.add({"board.align", "Align",
             "Line elements up: left, center (vertical centre line), right, top, middle (horizontal centre line) or "
             "bottom edges, against the selection's bounds or against one element (to=id). Grouped elements move "
             "with their group; connectors follow.",
             {docParam(), {"ids", ParamType::String, "elements to align, comma-separated", true},
              {"edge", ParamType::String, "what to line up", true, {"left", "center", "right", "top", "middle", "bottom"}},
              {"to", ParamType::String, "align to this element instead of the selection bounds", false},
              previewParam()},
             true},
            boardCommandFactory(session, [](BoardDoc& doc, const Params& p) {
                Board& b = doc.board;
                std::vector<std::string> ids;
                std::string error;
                if (!resolveIdList(b, stringParam(p, "ids"), ids, error)) return Result::failure(error);
                const auto us = units(b, ids);
                Box ref;
                if (has(p, "to")) {
                    const Element* t = b.get(stringParam(p, "to"));
                    if (!t) return Result::failure("no element '" + stringParam(p, "to") + "'");
                    ref = elementBox(b, *t);
                } else {
                    for (const auto& id : us) ref.add(elementBox(b, *b.get(id)));
                }
                const std::string edge = stringParam(p, "edge");
                for (const auto& id : us) {
                    Element& e = *b.get(id);
                    if (!unlockedElement(b, e, error)) return Result::failure(error);
                    const Box bx = elementBox(b, e);
                    double dx = 0, dy = 0;
                    if (edge == "left") dx = ref.x0 - bx.x0;
                    else if (edge == "right") dx = ref.x1 - bx.x1;
                    else if (edge == "center") dx = ref.cx() - bx.cx();
                    else if (edge == "top") dy = ref.y0 - bx.y0;
                    else if (edge == "bottom") dy = ref.y1 - bx.y1;
                    else dy = ref.cy() - bx.cy();
                    moveBy(b, e, dx, dy);
                }
                refreshGroups(b);
                return edited("aligned " + plural(us.size(), "element") + " " + edge, p);
            }, true));

    reg.add({"board.distribute", "Distribute",
             "Space elements evenly along an axis: equal gaps between the first and last, or a fixed gap= that "
             "packs them from the first. Order is by position.",
             {docParam(), {"ids", ParamType::String, "elements, comma-separated (3 or more for even gaps)", true},
              {"axis", ParamType::String, "horizontal (left to right) or vertical", false, {"horizontal", "vertical"}, std::string("horizontal")},
              {"gap", ParamType::Number, "fixed gap between neighbours", false},
              previewParam()},
             true},
            boardCommandFactory(session, [](BoardDoc& doc, const Params& p) {
                Board& b = doc.board;
                std::vector<std::string> ids;
                std::string error;
                if (!resolveIdList(b, stringParam(p, "ids"), ids, error)) return Result::failure(error);
                auto us = units(b, ids);
                if (us.size() < 2) return Result::failure("distribute needs at least two elements");
                const bool horiz = stringParam(p, "axis", "horizontal") == "horizontal";
                std::stable_sort(us.begin(), us.end(), [&](const std::string& a, const std::string& c) {
                    const Box ba = elementBox(b, *b.get(a)), bc = elementBox(b, *b.get(c));
                    return horiz ? ba.cx() < bc.cx() : ba.cy() < bc.cy();
                });
                double total = 0;
                std::vector<Box> boxes;
                for (const auto& id : us) {
                    boxes.push_back(elementBox(b, *b.get(id)));
                    total += horiz ? boxes.back().w() : boxes.back().h();
                }
                const double start = horiz ? boxes.front().x0 : boxes.front().y0;
                const double end = horiz ? boxes.back().x1 : boxes.back().y1;
                const double gap = has(p, "gap") ? numberParam(p, "gap") : (end - start - total) / static_cast<double>(us.size() - 1);
                double cur = start;
                for (std::size_t i = 0; i < us.size(); ++i) {
                    Element& e = *b.get(us[i]);
                    if (!unlockedElement(b, e, error)) return Result::failure(error);
                    const double d = cur - (horiz ? boxes[i].x0 : boxes[i].y0);
                    moveBy(b, e, horiz ? d : 0, horiz ? 0 : d);
                    cur += (horiz ? boxes[i].w() : boxes[i].h()) + gap;
                }
                refreshGroups(b);
                Result r = edited("distributed " + plural(us.size(), "element") + (horiz ? " horizontally" : " vertically") + " with gap " + fmtNum(std::round(gap * 10) / 10), p);
                r.with("gap", gap);
                return r;
            }, true));
}

void registerLayout(CommandRegistry& reg, Session& session) {
    reg.add({"board.layout", "Auto Layout",
             "Arrange shapes automatically from their connectors. layered = Sugiyama flowchart layout in ranks "
             "(direction TB/BT/LR/RL; cycles are handled, crossings reduced); tree = tidy tree; mindmap = root in the "
             "middle with branches either side; radial = rings around the root; force = force-directed (organic, for "
             "mind maps and networks); grid = rows and columns (no connectors needed). Frames refit around their "
             "contents; connectors follow. Without ids, every connected shape is laid out (or all shapes when "
             "nothing is connected).",
             {docParam(),
              {"algorithm", ParamType::String, "layout kind", false, {"layered", "tree", "mindmap", "radial", "force", "grid"}, std::string("layered")},
              {"direction", ParamType::String, "flow direction for layered/tree", false, {"TB", "TD", "BT", "LR", "RL"}, std::string("TB")},
              {"ids", ParamType::String, "only these shapes (comma-separated)", false},
              {"gap_x", ParamType::Number, "space between neighbours", false, {}, 60.0},
              {"gap_y", ParamType::Number, "space between ranks / rows", false, {}, 80.0},
              {"columns", ParamType::Int, "grid: columns (0 = square)", false, {}, std::int64_t{0}},
              {"seed", ParamType::Int, "force: random seed", false, {}, std::int64_t{1}},
              {"iterations", ParamType::Int, "force: simulation steps", false, {}, std::int64_t{300}},
              {"route", ParamType::String, "also set every connector's route", false, {"straight", "elbow", "curved"}},
              {"x", ParamType::Number, "put the result's left edge here (default: where the shapes were)", false},
              {"y", ParamType::Number, "put the result's top edge here", false},
              previewParam()},
             true},
            boardCommandFactory(session, [](BoardDoc& doc, const Params& p) {
                Board& b = doc.board;
                LayoutOptions o;
                o.algorithm = stringParam(p, "algorithm", "layered");
                o.direction = stringParam(p, "direction", "TB");
                o.gapX = std::max(0.0, numberParam(p, "gap_x", 60));
                o.gapY = std::max(0.0, numberParam(p, "gap_y", 80));
                o.columns = static_cast<int>(std::clamp<std::int64_t>(intParam(p, "columns", 0), 0, 1000));
                o.seed = static_cast<std::uint64_t>(intParam(p, "seed", 1));
                o.iterations = static_cast<int>(std::clamp<std::int64_t>(intParam(p, "iterations", 300), 10, 5000));
                std::string error;
                if (has(p, "ids") && !resolveIdList(b, stringParam(p, "ids"), o.ids, error)) return Result::failure(error);
                if (has(p, "x") || has(p, "y")) {
                    o.keepOrigin = false;
                    const Box c = contentBounds(b, false);
                    o.originX = has(p, "x") ? numberParam(p, "x") : (c.empty ? 0 : c.x0);
                    o.originY = has(p, "y") ? numberParam(p, "y") : (c.empty ? 0 : c.y0);
                }
                const int n = layout(b, o, error);
                if (n < 0) return Result::failure(error);
                int rerouted = 0;
                if (has(p, "route")) {
                    for (auto& e : b.elements) {
                        if (e.kind == Kind::Connector) {
                            e.route = stringParam(p, "route");
                            ++rerouted;
                        }
                    }
                }
                Result r = edited("laid out " + plural(static_cast<std::size_t>(n), "shape") + " with " + o.algorithm +
                                      (rerouted ? ", " + plural(static_cast<std::size_t>(rerouted), "connector") + " now " + stringParam(p, "route") : ""),
                                  p);
                const Box c = contentBounds(b);
                r.with("moved", static_cast<std::int64_t>(n)).with("width", std::round(c.w())).with("height", std::round(c.h()));
                return r;
            }, true));
}

void registerLayers(CommandRegistry& reg, Session& session) {
    reg.add({"board.layer", "Layers",
             "Manage layers (drawn bottom first): op=add a new one, set its visible/locked/opacity or rename it "
             "(new_name), move it to index (0 = bottom), or remove it (its elements move to the first remaining layer "
             "unless delete_elements=true). Elements go on a layer with layer= on board.add/board.set. Hidden layers "
             "aren't drawn or exported; locked layers refuse edits.",
             {docParam(),
              {"op", ParamType::String, "what to do", true, {"add", "set", "move", "remove"}},
              {"name", ParamType::String, "layer name", true},
              {"new_name", ParamType::String, "set: rename to", false},
              {"visible", ParamType::Bool, "shown and exported", false},
              {"locked", ParamType::Bool, "refuse edits to its elements", false},
              {"opacity", ParamType::Number, "0..1", false},
              {"index", ParamType::Int, "add/move: position, 0 = bottom (default: top)", false},
              {"delete_elements", ParamType::Bool, "remove: delete its elements too", false, {}, false},
              previewParam()},
             true},
            boardCommandFactory(session, [](BoardDoc& doc, const Params& p) {
                Board& b = doc.board;
                const std::string op = stringParam(p, "op"), name = stringParam(p, "name");
                if (name.empty()) return Result::failure("give the layer a name");
                auto indexOf = [&](const std::string& n) {
                    for (std::size_t i = 0; i < b.layers.size(); ++i) {
                        if (b.layers[i].name == n) return static_cast<int>(i);
                    }
                    return -1;
                };
                auto place = [&](int from) {
                    if (!has(p, "index")) return;
                    const int to = static_cast<int>(std::clamp<std::int64_t>(intParam(p, "index"), 0, static_cast<std::int64_t>(b.layers.size()) - 1));
                    Layer l = b.layers[static_cast<std::size_t>(from)];
                    b.layers.erase(b.layers.begin() + from);
                    b.layers.insert(b.layers.begin() + to, l);
                };
                auto setProps = [&](Layer& l) {
                    if (has(p, "visible")) l.visible = boolParam(p, "visible");
                    if (has(p, "locked")) l.locked = boolParam(p, "locked");
                    if (has(p, "opacity")) l.opacity = std::clamp(numberParam(p, "opacity"), 0.0, 1.0);
                };
                if (op == "add") {
                    if (indexOf(name) >= 0) return Result::failure("there is already a layer called '" + name + "'");
                    Layer l;
                    l.name = name;
                    setProps(l);
                    b.layers.push_back(l);
                    place(static_cast<int>(b.layers.size()) - 1);
                    return edited("added layer " + name, p);
                }
                const int i = indexOf(name);
                if (i < 0) return Result::failure("no layer called '" + name + "'");
                if (op == "set") {
                    Layer& l = b.layers[static_cast<std::size_t>(i)];
                    setProps(l);
                    if (has(p, "new_name")) {
                        const std::string nn = stringParam(p, "new_name");
                        if (nn.empty() || (nn != name && indexOf(nn) >= 0)) return Result::failure("can't rename to '" + nn + "'");
                        for (auto& e : b.elements) {
                            if (e.layer == name) e.layer = nn;
                        }
                        l.name = nn;
                    }
                    return edited("changed layer " + l.name, p);
                }
                if (op == "move") {
                    if (!has(p, "index")) return Result::failure("give index= (0 = bottom)");
                    place(i);
                    return edited("moved layer " + name, p);
                }
                if (b.layers.size() == 1) return Result::failure("a board needs at least one layer");
                b.layers.erase(b.layers.begin() + i);
                int affected = 0;
                if (boolParam(p, "delete_elements", false)) {
                    std::vector<std::string> ids;
                    for (const auto& e : b.elements) {
                        if (e.layer == name) ids.push_back(e.id);
                    }
                    affected = b.remove(ids);
                } else {
                    for (auto& e : b.elements) {
                        if (e.layer == name) {
                            e.layer = b.layers.front().name;
                            ++affected;
                        }
                    }
                }
                return edited("removed layer " + name + " (" + plural(static_cast<std::size_t>(affected), "element") +
                                  (boolParam(p, "delete_elements", false) ? " deleted)" : " moved to " + b.layers.front().name + ")"),
                              p);
            }, true));

    reg.add({"board.theme", "Theme",
             "Change the board's look: default (clean blue), sketchy (hand-drawn like Excalidraw: wobbly double "
             "strokes, hachure fills), dark, blueprint, pastel or mono. Elements keep their own colours unless "
             "clear_styles=true.",
             {docParam(), {"theme", ParamType::String, "theme name", true, themeNames()},
              {"background", ParamType::String, "canvas colour (\"theme\" = the theme's)", false},
              {"clear_styles", ParamType::Bool, "drop every element's own colours, fills and sketch settings", false, {}, false},
              previewParam()},
             true},
            boardCommandFactory(session, [](BoardDoc& doc, const Params& p) {
                Board& b = doc.board;
                b.theme = stringParam(p, "theme");
                if (has(p, "background")) {
                    const std::string bg = stringParam(p, "background");
                    if (bg == "theme" || bg.empty()) b.background.clear();
                    else if (!validColor(bg)) return Result::failure("background isn't a colour");
                    else b.background = bg;
                }
                int cleared = 0;
                if (boolParam(p, "clear_styles", false)) {
                    for (auto& e : b.elements) {
                        Style& s = e.style;
                        if (s.fill || s.stroke || s.textColor || s.sketch || s.fillStyle || s.roughness || s.font) ++cleared;
                        s.fill.reset();
                        s.stroke.reset();
                        s.textColor.reset();
                        s.sketch.reset();
                        s.fillStyle.reset();
                        s.roughness.reset();
                        s.font.reset();
                    }
                }
                return edited("theme is now " + b.theme + (cleared ? " (" + plural(static_cast<std::size_t>(cleared), "element") + " back to theme colours)" : ""), p);
            }, true));
}

// ---- text to diagram ------------------------------------------------------------------------------

ImportOptions importOptions(const Board& b, const Params& p) {
    ImportOptions o;
    o.route = stringParam(p, "route");
    o.layout = stringParam(p, "layout");
    o.direction = stringParam(p, "direction");
    if (o.direction == "TD") o.direction = "TB";
    o.layer = stringParam(p, "layer");
    o.colorBranches = boolParam(p, "colors", true);
    const Box c = contentBounds(b, false);
    o.x = has(p, "x") ? numberParam(p, "x") : (c.empty ? 0 : c.x1 + 100);
    o.y = has(p, "y") ? numberParam(p, "y") : (c.empty ? 0 : c.y0);
    return o;
}

Result importResult(const ImportStats& st, const Params& p, const std::string& doc) {
    std::string msg = std::string(st.kind == "outline" ? "made a mind map from the outline" : "made a " + st.kind) + " with " + plural(static_cast<std::size_t>(st.nodes), "node") + ", " +
                      plural(static_cast<std::size_t>(st.edges), st.kind == "sequence" ? "message" : "connector");
    if (st.frames) msg += ", " + plural(static_cast<std::size_t>(st.frames), "frame");
    for (const auto& w : st.warnings) msg += "\nwarning: " + w;
    Result r = Result::success(msg, doc);
    r.with("doc", doc)
        .with("kind", st.kind)
        .with("nodes", static_cast<std::int64_t>(st.nodes))
        .with("edges", static_cast<std::int64_t>(st.edges))
        .with("frames", static_cast<std::int64_t>(st.frames))
        .with("ids", joinList(st.ids));
    (void)p;
    return r;
}

std::string sourceText(const Params& p) {
    if (has(p, "text")) {
        std::string t = stringParam(p, "text");
        // the CLI passes "\n" literally; turn it into new lines
        std::string out;
        for (std::size_t i = 0; i < t.size(); ++i) {
            if (t[i] == '\\' && i + 1 < t.size() && t[i + 1] == 'n') {
                out += '\n';
                ++i;
            } else {
                out += t[i];
            }
        }
        return out;
    }
    if (has(p, "path")) return readText(stringParam(p, "path"));
    throw std::runtime_error("give text= (the diagram) or path= (a file holding it)");
}

ActionFactory diagramCommand(Session& session, std::function<ImportStats(Board&, const std::string&, const ImportOptions&, const Params&)> build) {
    return [&session, build](const Params& p) -> std::unique_ptr<Action> {
        if (has(p, "doc")) {
            return boardCommandFactory(session, [build](BoardDoc& doc, const Params& q) {
                const ImportStats st = build(doc.board, sourceText(q), importOptions(doc.board, q), q);
                return importResult(st, q, stringParam(q, "doc"));
            }, true)(p);
        }
        return createBoardAction(session, [build, p](std::shared_ptr<BoardDoc>& doc) {
            doc = std::make_shared<BoardDoc>();
            doc->board.theme = stringParam(p, "theme", "default");
            doc->board.title = stringParam(p, "title");
            const ImportStats st = build(doc->board, sourceText(p), importOptions(doc->board, p), p);
            Result r = importResult(st, p, "");
            Result out = Result::success(r.message);
            const auto nl = out.message.find('\n');
            out.message.insert(nl == std::string::npos ? out.message.size() : nl, " on {id}");
            for (auto& f : r.data) {
                if (f.first != "doc") out.data.push_back(f);
            }
            if (boolParam(p, "preview", false)) out.attach(*doc->preview());
            return out;
        });
    };
}

void registerDiagrams(CommandRegistry& reg, Session& session) {
    std::vector<ParamSpec> common{
        {"text", ParamType::String, "the diagram text (new lines as real line breaks or \\n)", false},
        {"path", ParamType::String, "or a file holding it", false},
        {"doc", ParamType::String, "board to add to (default: a new board)", false},
        {"theme", ParamType::String, "new board: theme", false, themeNames(), std::string("default")},
        {"title", ParamType::String, "new board: title", false},
        {"direction", ParamType::String, "override the flow direction", false, {"TB", "TD", "BT", "LR", "RL"}},
        {"x", ParamType::Number, "left edge of the diagram (default: right of what is on the board)", false},
        {"y", ParamType::Number, "top edge of the diagram", false},
        {"layer", ParamType::String, "layer for the new elements", false},
        previewParam()};
    auto mermaidSpecs = common;
    mermaidSpecs.push_back({"route", ParamType::String, "connector route (flowchart default elbow, mind map curved)", false, {"straight", "elbow", "curved"}});
    mermaidSpecs.push_back({"layout", ParamType::String, "flowchart/mind map layout (none = keep a simple placement)", false,
                            {"layered", "tree", "mindmap", "radial", "force", "grid", "none"}});
    reg.add({"board.from_mermaid", "From Mermaid",
             "Turn Mermaid text into a laid-out diagram. Supports flowcharts (`flowchart TD` / `graph LR`: node "
             "shapes [] () ([]) [[]] [()] (()) {} {{}} [/ /] [/ \\] >], links --> --- -.-> ==> --o --x <-->, labels "
             "-->|text| and -- text -->, chains A --> B --> C, A & B --> C, subgraph ... end as frames, classDef / "
             "class / ::: / style colours), sequence diagrams (`sequenceDiagram`: participant/actor ... as, ->> --> "
             "-x -) messages, Note left of/right of/over, loop/alt/else/opt/par/critical blocks, autonumber) and "
             "`mindmap` (indented). Without doc= it makes a new board. Nodes are named by their Mermaid ids, so "
             "board.set ids=A works afterwards.",
             mermaidSpecs, true},
            diagramCommand(session, [](Board& b, const std::string& text, const ImportOptions& o, const Params&) { return fromMermaid(b, text, o); }));

    auto outlineSpecs = common;
    outlineSpecs.push_back({"route", ParamType::String, "connector route", false, {"straight", "elbow", "curved"}, std::string("curved")});
    outlineSpecs.push_back({"layout", ParamType::String, "mind map layout", false, {"mindmap", "tree", "radial", "force", "layered", "none"}, std::string("mindmap")});
    outlineSpecs.push_back({"root", ParamType::String, "central topic when the outline has several top-level items", false});
    outlineSpecs.push_back({"colors", ParamType::Bool, "a colour per branch", false, {}, true});
    reg.add({"board.from_outline", "From Outline",
             "Turn an indented list into a mind map: bullets (-, *, +, 1.), Markdown headings (# root, ## branch) "
             "or plain indented lines. One top-level item is the centre; several get a root= topic (default \"Mind "
             "map\"). layout=mindmap puts branches on both sides; tree, radial, force (organic) and layered also work.",
             outlineSpecs, true},
            diagramCommand(session, [](Board& b, const std::string& text, const ImportOptions& o, const Params& p) {
                return fromOutline(b, text, o, stringParam(p, "root"));
            }));
}

// ---- looking ----------------------------------------------------------------------------------------

void registerQuery(CommandRegistry& reg, Session& session) {
    reg.add({"board.find", "Find Elements",
             "Find elements by text (case-insensitive, in labels and names), type, layer, what they connect to "
             "(connected=id gives its neighbours), or what's inside a frame/group (inside=id). Returns ids to "
             "pass to other commands.",
             {docParam(),
              {"text", ParamType::String, "text in the label or name", false},
              {"type", ParamType::String, "element type (rect, connector, ink, ...)", false},
              {"layer", ParamType::String, "layer name", false},
              {"connected", ParamType::String, "shapes joined to this one by a connector", false},
              {"inside", ParamType::String, "everything inside this frame or group", false},
              {"limit", ParamType::Int, "most results to list", false, {}, std::int64_t{50}}},
             false, true},
            boardCommandFactory(session, [](BoardDoc& doc, const Params& p) {
                const Board& b = doc.board;
                const std::string text = lower(stringParam(p, "text"));
                std::optional<Kind> kind;
                if (has(p, "type")) {
                    kind = parseKind(stringParam(p, "type"));
                    if (!kind) return Result::failure("unknown type '" + stringParam(p, "type") + "'");
                }
                std::set<std::string> neighbours, inside;
                if (has(p, "connected")) {
                    const Element* c = b.get(stringParam(p, "connected"));
                    if (!c) return Result::failure("no element '" + stringParam(p, "connected") + "'");
                    for (const auto& e : b.elements) {
                        if (e.kind != Kind::Connector) continue;
                        if (e.from == c->id && !e.to.empty()) neighbours.insert(e.to);
                        if (e.to == c->id && !e.from.empty()) neighbours.insert(e.from);
                    }
                }
                if (has(p, "inside")) {
                    const Element* c = b.get(stringParam(p, "inside"));
                    if (!c) return Result::failure("no element '" + stringParam(p, "inside") + "'");
                    for (const auto& d : b.descendants(c->id)) inside.insert(d);
                }
                std::vector<std::string> ids;
                std::string lines;
                const auto limit = static_cast<std::size_t>(std::clamp<std::int64_t>(intParam(p, "limit", 50), 1, 100000));
                for (const auto& e : b.elements) {
                    if (!text.empty() && lower(e.text).find(text) == std::string::npos && lower(e.name).find(text) == std::string::npos) continue;
                    if (kind && e.kind != *kind) continue;
                    if (has(p, "layer") && e.layer != stringParam(p, "layer")) continue;
                    if (has(p, "connected") && !neighbours.count(e.id)) continue;
                    if (has(p, "inside") && !inside.count(e.id)) continue;
                    ids.push_back(e.id);
                    if (ids.size() <= limit) lines += "\n  " + describeElement(b, e);
                }
                Result r = Result::success(std::to_string(ids.size()) + (ids.size() == 1 ? " match" : " matches") + lines, joinList(ids));
                if (ids.size() > limit) r.message += "\n  ... " + std::to_string(ids.size() - limit) + " more";
                r.with("doc", stringParam(p, "doc")).with("count", static_cast<std::int64_t>(ids.size())).with("ids", joinList(ids));
                return r;
            }, false));

    reg.add({"board.info", "Board Info",
             "Describe the board: title, theme, layers, content bounds and every element (id, type, name, label, "
             "position and size; connectors as from -> to). With id= it shows one element's full properties.",
             {docParam(), {"id", ParamType::String, "one element to describe in full", false},
              {"limit", ParamType::Int, "most elements to list", false, {}, std::int64_t{200}}},
             false, true},
            boardCommandFactory(session, [](BoardDoc& doc, const Params& p) {
                const Board& b = doc.board;
                if (has(p, "id")) {
                    const Element* e = b.get(stringParam(p, "id"));
                    if (!e) return Result::failure("no element '" + stringParam(p, "id") + "'");
                    const Resolved s = resolve(b, *e);
                    auto col = [](const std::optional<Color>& c) { return c ? "#" + c->hex().substr(1, 6) : std::string("none"); };
                    std::string m = describeElement(b, *e);
                    m += "\nfill " + col(s.fill) + ", stroke " + col(s.stroke) + " " + fmtNum(s.strokeWidth) + "px " + s.dash + ", text " +
                         col(std::optional<Color>(s.text)) + " " + fmtNum(s.fontSize) + "px " + s.font + (s.bold ? " bold" : "") +
                         (s.sketch ? ", sketchy" : "") + ", fill style " + s.fillStyle + (s.opacity < 1 ? ", opacity " + fmtNum(s.opacity) : "");
                    if (e->kind == Kind::Connector)
                        m += "\nroute " + e->route + ", arrows " + e->startArrow + " / " + e->endArrow + ", anchors " + e->fromAnchor + " / " + e->toAnchor;
                    const auto kids = b.children(e->id);
                    if (!kids.empty()) m += "\ncontains " + joinList(kids);
                    Result r = Result::success(m);
                    const Box bx = elementBox(b, *e);
                    r.with("id", e->id).with("type", std::string(toString(e->kind))).with("name", e->name).with("text", e->text)
                        .with("x", bx.x0).with("y", bx.y0).with("w", bx.w()).with("h", bx.h()).with("layer", e->layer).with("parent", e->parent);
                    if (e->kind == Kind::Connector) r.with("from", e->from).with("to", e->to).with("route", e->route);
                    return r;
                }
                std::string m = doc.summary();
                const Box c = contentBounds(b);
                if (!c.empty) m += "\ncontent: " + fmtNum(std::round(c.x0)) + "," + fmtNum(std::round(c.y0)) + " " + fmtNum(std::round(c.w())) + "x" + fmtNum(std::round(c.h()));
                m += "\nlayers:";
                for (const auto& l : b.layers) {
                    int n = 0;
                    for (const auto& e : b.elements) n += e.layer == l.name ? 1 : 0;
                    m += "\n  " + l.name + " (" + plural(static_cast<std::size_t>(n), "element") + (l.visible ? "" : ", hidden") + (l.locked ? ", locked" : "") +
                         (l.opacity < 1 ? ", opacity " + fmtNum(l.opacity) : "") + ")";
                }
                const auto limit = static_cast<std::size_t>(std::clamp<std::int64_t>(intParam(p, "limit", 200), 0, 100000));
                if (!b.elements.empty()) m += "\nelements:";
                for (std::size_t i = 0; i < b.elements.size() && i < limit; ++i) m += "\n  " + describeElement(b, b.elements[i]);
                if (b.elements.size() > limit) m += "\n  ... " + std::to_string(b.elements.size() - limit) + " more";
                Result r = Result::success(m);
                int shapes = 0, connectors = 0, ink = 0;
                for (const auto& e : b.elements) {
                    if (e.kind == Kind::Connector) ++connectors;
                    else if (e.kind == Kind::Ink) ++ink;
                    else ++shapes;
                }
                r.with("doc", stringParam(p, "doc")).with("title", b.title).with("theme", b.theme)
                    .with("elements", static_cast<std::int64_t>(b.elements.size())).with("shapes", static_cast<std::int64_t>(shapes))
                    .with("connectors", static_cast<std::int64_t>(connectors)).with("ink", static_cast<std::int64_t>(ink))
                    .with("layers", static_cast<std::int64_t>(b.layers.size()));
                if (!c.empty) r.with("bounds", NumberList{c.x0, c.y0, c.w(), c.h()});
                return r;
            }, false));

    auto previewSpecs = renderParamSpecs();
    previewSpecs.insert(previewSpecs.begin(), docParam());
    previewSpecs[1] = {"scale", ParamType::Number, "zoom (default: fit max_size)", false};
    previewSpecs.push_back({"max_size", ParamType::Int, "longest side in px when no scale is given", false, {}, std::int64_t{1024}});
    reg.add({"board.preview", "Preview Board",
             "Render the board (or a region, a frame with only=, or some layers) to a PNG attached to the result, "
             "without changing anything. Use it to check a diagram before exporting.",
             previewSpecs, false, true},
            boardCommandFactory(session, [](BoardDoc& doc, const Params& p) {
                RenderOptions o = renderOptionsFrom(p);
                const Box r = renderRegion(doc.board, o);
                if (!has(p, "scale")) {
                    const double maxSize = static_cast<double>(std::clamp<std::int64_t>(intParam(p, "max_size", 1024), 16, 8192));
                    o.scale = std::clamp(maxSize / std::max({r.w(), r.h(), 1.0}), 0.02, 4.0);
                }
                const raster::Image img = renderImage(doc.board, o);
                Result res = Result::success("rendered " + std::to_string(img.width()) + "x" + std::to_string(img.height()) + " px of " +
                                             fmtNum(std::round(r.w())) + "x" + fmtNum(std::round(r.h())) + " board units");
                res.with("doc", stringParam(p, "doc")).with("width", static_cast<std::int64_t>(img.width())).with("height", static_cast<std::int64_t>(img.height()));
                res.attach({"image/png", raster::encodePng(img), "board.png"});
                return res;
            }, false));

    reg.add({"board.view", "Board View",
             "The area of the board the window's picture shows (its own preview): x, y, width and height in board units "
             "and scale (picture px per unit). The window uses it to turn mouse clicks into board positions.",
             {docParam()},
             false, true},
            boardCommandFactory(session, [](BoardDoc& doc, const Params& p) {
                RenderOptions o;
                const Box r = renderRegion(doc.board, o);
                const double scale = std::clamp(768.0 / std::max({r.w(), r.h(), 1.0}), 0.05, 2.0);  // as BoardDoc::preview
                Result res = Result::success("shows " + fmtNum(std::round(r.x0)) + "," + fmtNum(std::round(r.y0)) + " " + fmtNum(std::round(r.w())) +
                                             "x" + fmtNum(std::round(r.h())) + " at " + fmtNum(scale) + " px per unit");
                res.with("doc", stringParam(p, "doc")).with("x", r.x0).with("y", r.y0).with("width", r.w()).with("height", r.h()).with("scale", scale);
                return res;
            }, false));

    reg.add({"board.hit", "Element At",
             "The element at a point (the top one): shapes by their box, connectors and ink within `tolerance` of the "
             "line. Returns id (empty when nothing is there), type, x, y, w, h and text. Groups are skipped (their "
             "members are hit); a frame is hit only where nothing inside it is.",
             {docParam(), {"x", ParamType::Number, "board x", true}, {"y", ParamType::Number, "board y", true},
              {"tolerance", ParamType::Number, "how close to a line counts, board units", false, {}, 6.0}},
             false, true},
            boardCommandFactory(session, [](BoardDoc& doc, const Params& p) {
                const Board& b = doc.board;
                const double x = numberParam(p, "x", 0), y = numberParam(p, "y", 0), tol = std::max(0.5, numberParam(p, "tolerance", 6));
                auto nearLine = [&](const std::vector<Pt>& pts) {
                    for (std::size_t i = 0; i + 1 < pts.size(); ++i) {
                        const double ax = pts[i].x, ay = pts[i].y, bx = pts[i + 1].x, by = pts[i + 1].y;
                        const double dx = bx - ax, dy = by - ay, len = dx * dx + dy * dy;
                        const double t = len > 0 ? std::clamp(((x - ax) * dx + (y - ay) * dy) / len, 0.0, 1.0) : 0;
                        if (std::hypot(ax + t * dx - x, ay + t * dy - y) <= tol) return true;
                    }
                    return false;
                };
                const Element* hit = nullptr;
                const Element* frame = nullptr;
                for (auto it = b.elements.rbegin(); it != b.elements.rend() && !hit; ++it) {
                    const Element& e = *it;
                    if (e.kind == Kind::Group) continue;
                    if (const Layer* l = b.layer(e.layer); l && (!l->visible || l->locked)) continue;
                    if (e.kind == Kind::Connector) {
                        if (nearLine(connectorPath(b, e))) hit = &e;
                    } else if (e.kind == Kind::Ink) {
                        std::vector<Pt> pts;
                        for (const auto& q : e.ink) pts.push_back({q.x, q.y});
                        if (nearLine(pts)) hit = &e;
                    } else if (x >= e.x && x <= e.x + e.w && y >= e.y && y <= e.y + e.h) {
                        if (e.kind == Kind::Frame) {
                            if (!frame) frame = &e;
                        } else {
                            hit = &e;
                        }
                    }
                }
                if (!hit) hit = frame;
                Result res = Result::success(hit ? describeElement(b, *hit) : std::string("nothing there"));
                res.with("doc", stringParam(p, "doc")).with("id", hit ? hit->id : std::string());
                if (hit) {
                    const Box box = elementBox(b, *hit);
                    res.with("type", std::string(toString(hit->kind))).with("x", box.x0).with("y", box.y0).with("w", box.w()).with("h", box.h()).with("text", hit->text);
                }
                return res;
            }, false));
}

}  // namespace

void registerArrangeCommands(Session& session) {
    registerAlign(session.registry(), session);
    registerLayout(session.registry(), session);
    registerLayers(session.registry(), session);
}

void registerDiagramCommands(Session& session) { registerDiagrams(session.registry(), session); }

void registerQueryCommands(Session& session) { registerQuery(session.registry(), session); }

}  // namespace wetwaresuite::board
