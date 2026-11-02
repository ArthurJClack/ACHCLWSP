// Artboards, guides, grid and snapping, symbols, repeats, selection queries
// and image trace.
#include <algorithm>
#include <cmath>
#include <map>

#include "wetwaresuite/raster/codec.hpp"
#include "internal.hpp"

namespace wetwaresuite::vector::detail {

namespace {


int findArtboard(const VectorDoc& doc, const std::string& name) {
    for (std::size_t k = 0; k < doc.artboards.size(); ++k) {
        if (doc.artboards[k].name == name || std::to_string(k + 1) == name) return static_cast<int>(k);
    }
    return -1;
}

std::string artboardList(const VectorDoc& doc) {
    std::string text;
    for (int k = 0; k < doc.artboardCount(); ++k) {
        const Artboard a = doc.artboard(k);
        text += "\n" + std::to_string(k + 1) + " " + a.name + " " + num(a.x) + "," + num(a.y) + " " + num(a.w) + "x" + num(a.h);
    }
    return text;
}

Matrix about(const Matrix& m, Point c) { return Matrix::translate(c.x, c.y) * m * Matrix::translate(-c.x, -c.y); }

void registerBoardAndSymbolCommands(Session& session) {
    auto& reg = session.registry();

    reg.add({"vector.add_artboard", "Add Artboard",
             "Add an artboard: a named page area on the canvas (the canvas grows to hold it). Each artboard "
             "exports on its own (vector.save artboard=) and becomes a page in PDF. Without artboards the canvas "
             "is the one page.",
             {docParam(), {"name", ParamType::String, "name (letters, digits, _ - .)", false}, numParam("x", "left edge"), numParam("y", "top edge"),
              numParam("width", "width in px"), numParam("height", "height in px")},
             true},
            vectorCommand(session, [](VectorDoc& doc, const Params& p) {
                Artboard a;
                a.name = stringParam(p, "name", "Artboard" + std::to_string(doc.artboards.size() + 1));
                if (!validName(a.name)) return Result::failure("artboard names use letters, digits, _ - and . only");
                if (findArtboard(doc, a.name) >= 0) return Result::failure("there's already an artboard '" + a.name + "'");
                a.x = numberParam(p, "x"), a.y = numberParam(p, "y"), a.w = numberParam(p, "width"), a.h = numberParam(p, "height");
                if (a.w <= 0 || a.h <= 0 || a.w > 30000 || a.h > 30000) return Result::failure("width and height must be 1..30000");
                doc.artboards.push_back(a);
                doc.width = std::max(doc.width, static_cast<int>(std::ceil(a.x + a.w)));
                doc.height = std::max(doc.height, static_cast<int>(std::ceil(a.y + a.h)));
                return Result::success("added artboard " + std::to_string(doc.artboards.size()) + " " + a.name, a.name)
                    .with("name", a.name)
                    .with("index", static_cast<std::int64_t>(doc.artboards.size()));
            }));

    reg.add({"vector.remove_artboard", "Remove Artboard", "Remove an artboard (by name or number). The shapes on it stay.",
             {docParam(), {"artboard", ParamType::String, "name or number (1 = first)", true}}, true},
            vectorCommand(session, [](VectorDoc& doc, const Params& p) {
                const int k = findArtboard(doc, stringParam(p, "artboard"));
                if (k < 0) return Result::failure("no artboard '" + stringParam(p, "artboard") + "'");
                const std::string name = doc.artboards[static_cast<std::size_t>(k)].name;
                doc.artboards.erase(doc.artboards.begin() + k);
                return Result::success("removed artboard " + name);
            }));

    reg.add({"vector.artboards", "Artboards", "List the artboards (number, name, position and size).", {docParam()}, false, true},
            vectorCommand(session, [](VectorDoc& doc, const Params&) {
                std::string names;
                for (int k = 0; k < doc.artboardCount(); ++k) names += (k ? "," : "") + doc.artboard(k).name;
                return Result::success(std::to_string(doc.artboardCount()) + " artboard(s)" + artboardList(doc))
                    .with("artboards", names)
                    .with("count", static_cast<std::int64_t>(doc.artboardCount()));
            }, false));

    reg.add({"vector.guides", "Guides",
             "Add guides: x= vertical guide positions, y= horizontal ones (lists). clear=true removes all first. "
             "With snap_guides on (vector.grid), moves and the window snap to them.",
             {docParam(), {"x", ParamType::NumberList, "vertical guides at these x", false}, {"y", ParamType::NumberList, "horizontal guides at these y", false},
              {"clear", ParamType::Bool, "remove the existing guides first", false, {}, false}},
             true},
            vectorCommand(session, [](VectorDoc& doc, const Params& p) {
                if (boolParam(p, "clear", false)) doc.guidesX.clear(), doc.guidesY.clear();
                for (double v : listParam(p, "x")) doc.guidesX.push_back(v);
                for (double v : listParam(p, "y")) doc.guidesY.push_back(v);
                return Result::success(std::to_string(doc.guidesX.size()) + " vertical and " + std::to_string(doc.guidesY.size()) + " horizontal guides")
                    .with("vertical", static_cast<std::int64_t>(doc.guidesX.size()))
                    .with("horizontal", static_cast<std::int64_t>(doc.guidesY.size()));
            }));

    reg.add({"vector.grid", "Grid and Snapping",
             "Set the grid spacing (size=0 hides it) and what moving things snaps to: snap_grid, snap_guides, snap_points (other anchors).",
             {docParam(), numParam("size", "grid spacing in px", false), {"snap_grid", ParamType::Bool, "snap to the grid", false},
              {"snap_guides", ParamType::Bool, "snap to guides", false}, {"snap_points", ParamType::Bool, "snap to anchors", false}},
             true},
            vectorCommand(session, [](VectorDoc& doc, const Params& p) {
                if (has(p, "size")) {
                    if (numberParam(p, "size") < 0) return Result::failure("size can't be negative");
                    doc.gridSize = numberParam(p, "size");
                }
                if (has(p, "snap_grid")) doc.snapGrid = boolParam(p, "snap_grid", false);
                if (has(p, "snap_guides")) doc.snapGuides = boolParam(p, "snap_guides", false);
                if (has(p, "snap_points")) doc.snapPoints = boolParam(p, "snap_points", false);
                return Result::success("grid " + num(doc.gridSize) + " px; snapping to" + (doc.snapGrid ? " grid" : "") + (doc.snapGuides ? " guides" : "") +
                                       (doc.snapPoints ? " points" : "") + (!doc.snapGrid && !doc.snapGuides && !doc.snapPoints ? " nothing" : ""));
            }));

    reg.add({"vector.snap", "Snap Point", "Where a point lands after snapping (grid, guides, anchors as vector.grid set), within radius px.",
             {docParam(), numParam("x", "x"), numParam("y", "y"), numParam("radius", "how close counts, px", false, 8.0)}, false, true},
            vectorCommand(session, [](VectorDoc& doc, const Params& p) {
                std::string what;
                const Point q = snapPoint(doc, {numberParam(p, "x"), numberParam(p, "y")}, numberParam(p, "radius", 8), &what);
                return Result::success(num(q.x) + "," + num(q.y) + " (" + what + ")").with("x", q.x).with("y", q.y).with("snapped", what);
            }, false));

    reg.add({"vector.symbol", "Make Symbol",
             "Turn shapes into a reusable symbol: they're replaced by an instance of it (keep=true leaves them). "
             "Using an existing name redefines the symbol, and every instance changes. Place more with vector.place_symbol.",
             {docParam(), {"name", ParamType::String, "symbol name (letters, digits, _ - .)", true},
              {"shapes", ParamType::String, "the shapes, comma separated", true}, {"keep", ParamType::Bool, "keep the original shapes", false, {}, false}},
             true},
            vectorCommand(session, [](VectorDoc& doc, const Params& p) {
                const std::string name = stringParam(p, "name");
                if (!validName(name)) return Result::failure("symbol names use letters, digits, _ - and . only");
                std::vector<int> idx;
                std::string error;
                if (!shapeList(doc, stringParam(p, "shapes"), idx, error)) return Result::failure(error);
                idx = topmost(doc, idx);
                std::vector<int> all;
                for (int i : idx) {
                    for (int k : doc.subtree(i)) all.push_back(k);
                }
                std::sort(all.begin(), all.end());
                Symbol sym;
                sym.name = name;
                for (int k : all) {
                    Shape s = doc.shapes[static_cast<std::size_t>(k)];
                    if (s.kind == ShapeKind::Instance && s.symbol == name) return Result::failure("a symbol can't hold an instance of itself");
                    if (std::find(idx.begin(), idx.end(), k) != idx.end()) s.parent.clear();
                    s.locked = false;
                    sym.shapes.push_back(std::move(s));
                }
                auto it = std::find_if(doc.symbols.begin(), doc.symbols.end(), [&](const Symbol& y) { return y.name == name; });
                const bool redefined = it != doc.symbols.end();
                if (redefined) *it = sym;
                else doc.symbols.push_back(sym);
                std::string inst;
                if (!boolParam(p, "keep", false)) {
                    const int top = *std::max_element(idx.begin(), idx.end());
                    Shape s;
                    s.kind = ShapeKind::Instance;
                    s.symbol = name;
                    s.parent = doc.shapes[static_cast<std::size_t>(top)].parent;
                    s.name = name;
                    inst = insertShape(doc, std::move(s), top + 1);
                    std::vector<std::string> ids;
                    for (int i : idx) ids.push_back(doc.shapes[static_cast<std::size_t>(i)].id);
                    for (const auto& id : ids) eraseSubtree(doc, doc.find(id));
                }
                Result r = Result::success(std::string(redefined ? "redefined" : "made") + " symbol " + name + " from " + std::to_string(all.size()) + " shapes" +
                                               (inst.empty() ? "" : "; instance " + inst),
                                           inst.empty() ? name : inst);
                r.with("name", name);
                if (!inst.empty()) r.with("shape", inst);
                return r;
            }));

    reg.add({"vector.place_symbol", "Place Symbol",
             "Place an instance of a symbol with its top-left corner at x,y (default: where the symbol was made). "
             "Move, scale and rotate it like any shape; it redraws when the symbol changes. Returns its id.",
             {docParam(), {"name", ParamType::String, "symbol name", true}, numParam("x", "left edge", false), numParam("y", "top edge", false),
              previewParam()},
             true},
            vectorCommand(session, [](VectorDoc& doc, const Params& p) {
                const std::string name = stringParam(p, "name");
                if (!doc.findSymbol(name)) return Result::failure("no symbol '" + name + "' (vector.symbol makes one)");
                Shape s;
                s.kind = ShapeKind::Instance;
                s.symbol = name;
                s.name = name;
                const std::string id = doc.add(std::move(s));
                doc.refresh();
                Shape& made = doc.shapes[static_cast<std::size_t>(doc.find(id))];
                if (has(p, "x")) made.cx += numberParam(p, "x") - made.x;
                if (has(p, "y")) made.cy += numberParam(p, "y") - made.y;
                Result r = Result::success("placed symbol " + name + " as " + id, id);
                r.with("shape", id);
                return maybePreview(r, doc, p);
            }));

    reg.add({"vector.expand_symbol", "Expand Symbol", "Replace a symbol instance with an editable copy of its shapes (in a group). Returns the group id.",
             {docParam(), shapeParam(), previewParam()}, true},
            vectorCommand(session, onShape([](VectorDoc& doc, int idx, const Params&) {
                const Shape inst = doc.shapes[static_cast<std::size_t>(idx)];
                if (inst.kind != ShapeKind::Instance) return Result::failure(inst.id + " isn't a symbol instance");
                const Symbol* sym = doc.findSymbol(inst.symbol);
                if (!sym) return Result::failure("symbol '" + inst.symbol + "' is missing");
                Shape g;
                g.kind = ShapeKind::Group;
                g.parent = inst.parent;
                g.name = inst.name;
                g.opacity = inst.opacity;
                g.blend = inst.blend;
                g.effects = inst.effects;
                g.visible = inst.visible;
                const std::string gid = insertShape(doc, std::move(g), idx + 1);
                std::map<std::string, std::string> ids;
                std::vector<Shape> copies = sym->shapes;
                for (auto& c : copies) ids[c.id] = freshId(doc);
                for (auto& c : copies) {
                    c.id = ids[c.id];
                    c.parent = c.parent.empty() ? gid : ids[c.parent];
                    if (!c.clip.empty()) c.clip = ids[c.clip];
                    if (!c.mask.empty()) c.mask = ids[c.mask];
                    if (ids.count(c.onPath)) c.onPath = ids[c.onPath];
                }
                int at = doc.find(gid) + 1;
                for (auto& c : copies) insertShape(doc, std::move(c), at++);
                doc.refresh();
                applyMatrix(doc, doc.find(gid), shapeMatrix(inst) * Matrix::translate(inst.cx, inst.cy));
                eraseSubtree(doc, doc.find(inst.id));
                return Result::success("expanded " + inst.id + " into group " + gid, gid).with("shape", gid);
            })));
}

void registerRepeatAndSelectCommands(Session& session) {
    auto& reg = session.registry();

    reg.add({"vector.repeat", "Repeat",
             "Repeat a shape or group: mode=grid (rows x columns with gap_x/gap_y px between copies), radial "
             "(count copies turned around cx,cy, default a radius below the shape's centre), or mirror (a flipped "
             "copy across a vertical line at x= or a horizontal one at y=, default the shape's right or bottom edge). "
             "The original and copies end up in one group; returns its id.",
             {docParam(), shapeParam(), {"mode", ParamType::String, "kind of repeat", true, {"grid", "radial", "mirror"}},
              {"rows", ParamType::Int, "grid rows", false, {}, std::int64_t{2}}, {"columns", ParamType::Int, "grid columns", false, {}, std::int64_t{2}},
              numParam("gap_x", "grid gap across, px", false, 10.0), numParam("gap_y", "grid gap down, px", false, 10.0),
              {"count", ParamType::Int, "radial copies (the original included)", false, {}, std::int64_t{6}},
              numParam("cx", "radial centre x", false), numParam("cy", "radial centre y", false), numParam("radius", "radial radius (default: the shape's size)", false),
              {"axis", ParamType::String, "mirror across a vertical or horizontal line", false, {"vertical", "horizontal"}, std::string("vertical")},
              numParam("x", "mirror line x (axis=vertical)", false), numParam("y", "mirror line y (axis=horizontal)", false), previewParam()},
             true},
            vectorCommand(session, onShape([](VectorDoc& doc, int idx, const Params& p) {
                std::string error;
                if (!editable(doc, idx, error)) return Result::failure(error);
                const std::string mode = stringParam(p, "mode");
                const Bounds b = shapeBounds(doc, idx);
                if (b.empty) return Result::failure("nothing to repeat");
                const Point c{(b.x0 + b.x1) / 2, (b.y0 + b.y1) / 2};
                std::vector<Matrix> placements;
                if (mode == "grid") {
                    const auto rows = intParam(p, "rows", 2), cols = intParam(p, "columns", 2);
                    if (rows < 1 || cols < 1 || rows * cols > 2500) return Result::failure("rows x columns must be 1..2500");
                    for (std::int64_t r = 0; r < rows; ++r) {
                        for (std::int64_t k = 0; k < cols; ++k) {
                            if (r || k) {
                                placements.push_back(Matrix::translate(static_cast<double>(k) * (b.width() + numberParam(p, "gap_x", 10)),
                                                                       static_cast<double>(r) * (b.height() + numberParam(p, "gap_y", 10))));
                            }
                        }
                    }
                } else if (mode == "radial") {
                    const auto n = intParam(p, "count", 6);
                    if (n < 2 || n > 720) return Result::failure("count must be 2..720");
                    const double radius = numberParam(p, "radius", std::max(b.width(), b.height()));
                    const Point centre{numberParam(p, "cx", c.x), numberParam(p, "cy", c.y + radius)};
                    for (std::int64_t k = 1; k < n; ++k) placements.push_back(about(Matrix::rotate(360.0 * static_cast<double>(k) / static_cast<double>(n)), centre));
                } else {
                    const bool vertical = stringParam(p, "axis", "vertical") == "vertical";
                    const double at = vertical ? numberParam(p, "x", b.x1) : numberParam(p, "y", b.y1);
                    placements.push_back(vertical ? about(Matrix::scale(-1, 1), {at, 0}) : about(Matrix::scale(1, -1), {0, at}));
                }
                const std::string orig = doc.shapes[static_cast<std::size_t>(idx)].id;
                std::vector<std::string> ids{orig};
                for (const auto& m : placements) {
                    const int o = doc.find(orig);
                    const std::string copy = duplicateSubtree(doc, o, doc.shapes[static_cast<std::size_t>(o)].parent);
                    applyMatrix(doc, doc.find(copy), m);
                    ids.push_back(copy);
                }
                std::vector<int> all;
                for (const auto& id : ids) all.push_back(doc.find(id));
                const std::string gid = groupShapes(doc, all, mode + " repeat");
                return Result::success(mode + " repeat of " + orig + ": " + std::to_string(placements.size()) + " copies in group " + gid, gid)
                    .with("shape", gid)
                    .with("copies", static_cast<std::int64_t>(placements.size()));
            })));

    reg.add({"vector.hit", "Hit Test",
             "What's under a point: the topmost visible, unlocked shape at x,y (`shape`) and what a click there "
             "selects (`select`: its outermost group below the layers). Empty when nothing is there.",
             {docParam(), numParam("x", "x"), numParam("y", "y"), numParam("tolerance", "how close to an outline counts, px", false, 3.0)}, false, true},
            vectorCommand(session, [](VectorDoc& doc, const Params& p) {
                const int i = hitTest(doc, {numberParam(p, "x"), numberParam(p, "y")}, numberParam(p, "tolerance", 3));
                if (i < 0) return Result::success("nothing there").with("shape", std::string()).with("select", std::string());
                const std::string leaf = doc.shapes[static_cast<std::size_t>(i)].id, sel = doc.shapes[static_cast<std::size_t>(selectable(doc, i))].id;
                return Result::success(leaf + (sel != leaf ? " (in " + sel + ")" : ""), sel).with("shape", leaf).with("select", sel);
            }, false));

    reg.add({"vector.bounds", "Shape Bounds", "A shape's (or group's) box on the canvas: x, y, width, height.", {docParam(), shapeParam()}, false, true},
            vectorCommand(session, onShape([](VectorDoc& doc, int idx, const Params&) {
                const Bounds b = shapeBounds(doc, idx);
                if (b.empty) return Result::failure(doc.shapes[static_cast<std::size_t>(idx)].id + " covers nothing");
                const std::string text = num(b.x0) + "," + num(b.y0) + "," + num(b.width()) + "," + num(b.height());
                return Result::success(text, text)
                    .with("x", b.x0)
                    .with("y", b.y0)
                    .with("width", b.width())
                    .with("height", b.height())
                    .with("kind", std::string(toString(doc.shapes[static_cast<std::size_t>(idx)].kind)));
            }), false));

    reg.add({"vector.select_area", "Select Area",
             "The shapes a drag-box selects: those wholly inside x,y,width,height (touch=true: any that touch it), "
             "as their outermost groups below the layers.",
             {docParam(), numParam("x", "left"), numParam("y", "top"), numParam("width", "width"), numParam("height", "height"),
              {"touch", ParamType::Bool, "count shapes that only touch the box", false, {}, false}},
             false, true},
            vectorCommand(session, [](VectorDoc& doc, const Params& p) {
                const double x0 = numberParam(p, "x"), y0 = numberParam(p, "y");
                const double x1 = x0 + numberParam(p, "width"), y1 = y0 + numberParam(p, "height");
                const bool touch = boolParam(p, "touch", false);
                std::vector<int> picked;
                for (int i : paintOrder(doc)) {
                    if (doc.lockedOrInside(i)) continue;
                    const int s = selectable(doc, i);
                    if (std::find(picked.begin(), picked.end(), s) != picked.end()) continue;
                    const Bounds b = shapeBounds(doc, s);
                    if (b.empty) continue;
                    const bool in = touch ? (b.x1 >= x0 && b.x0 <= x1 && b.y1 >= y0 && b.y0 <= y1) : (b.x0 >= x0 && b.x1 <= x1 && b.y0 >= y0 && b.y1 <= y1);
                    if (in) picked.push_back(s);
                }
                const std::string ids = idText(doc, picked);
                return Result::success(std::to_string(picked.size()) + " selected" + (ids.empty() ? "" : ": " + ids), ids)
                    .with("shapes", ids)
                    .with("count", static_cast<std::int64_t>(picked.size()));
            }, false));

    reg.add({"vector.boxes", "Shape Boxes",
             "Every pickable shape's box, top-level groups as one: lines of \"id x y width height\". What the window's "
             "smart guides snap to; except= leaves one out (the shape being dragged).",
             {docParam(), {"except", ParamType::String, "shape id to leave out", false}},
             false, true},
            vectorCommand(session, [](VectorDoc& doc, const Params& p) {
                const std::string except = stringParam(p, "except", "");
                std::vector<int> seen;
                std::string text;
                for (int i : paintOrder(doc)) {
                    if (doc.lockedOrInside(i)) continue;
                    const int s = selectable(doc, i);
                    if (std::find(seen.begin(), seen.end(), s) != seen.end()) continue;
                    seen.push_back(s);
                    const auto& sh = doc.shapes[static_cast<std::size_t>(s)];
                    if (sh.id == except || (!sh.name.empty() && sh.name == except)) continue;
                    const Bounds b = shapeBounds(doc, s);
                    if (b.empty) continue;
                    text += (text.empty() ? "" : "\n") + sh.id + " " + num(b.x0) + " " + num(b.y0) + " " + num(b.width()) + " " + num(b.height());
                }
                return Result::success(text.empty() ? std::string("no shapes") : text, text).with("boxes", text);
            }, false));

    reg.add({"vector.trace", "Image Trace",
             "Turn a bitmap (png, jpg, ...) into filled vector paths. colors=2 traces black and white by threshold "
             "(darker than threshold is ink); more colours sorts the picture into that many. smooth (0 = pixel "
             "corners, 1 = curves), tolerance (px the curves may stray), min_area (px, drops specks), "
             "ignore_white. Placed at x,y and scaled to width= px (default: the image's size). Returns the group id.",
             {docParam(), {"path", ParamType::String, "image file", true}, {"colors", ParamType::Int, "2 = black and white, up to 64", false, {}, std::int64_t{2}},
              {"threshold", ParamType::Int, "black and white cut-off 0..255", false, {}, std::int64_t{128}}, numParam("smooth", "0..1", false, 1.0),
              numParam("tolerance", "px", false, 1.0), numParam("min_area", "px", false, 4.0),
              {"ignore_white", ParamType::Bool, "leave white areas empty", false, {}, true}, numParam("x", "left", false, 0.0), numParam("y", "top", false, 0.0),
              numParam("width", "traced width in px", false), {"name", ParamType::String, "group name", false}, previewParam()},
             true},
            vectorCommand(session, [](VectorDoc& doc, const Params& p) {
                raster::Image img;
                try {
                    img = raster::load(stringParam(p, "path"));
                } catch (const std::exception& e) {
                    return Result::failure(e.what());
                }
                TraceSettings st;
                st.colors = static_cast<int>(std::clamp<std::int64_t>(intParam(p, "colors", 2), 2, 64));
                st.threshold = static_cast<int>(std::clamp<std::int64_t>(intParam(p, "threshold", 128), 0, 256));
                st.smooth = numberParam(p, "smooth", 1.0);
                st.tolerance = numberParam(p, "tolerance", 1.0);
                st.minArea = numberParam(p, "min_area", 4.0);
                st.ignoreWhite = boolParam(p, "ignore_white", true);
                const auto regions = traceImage(img, st);
                if (regions.empty()) return Result::failure("nothing to trace (try another threshold or more colours)");
                const double k = has(p, "width") && img.width() > 0 ? numberParam(p, "width") / img.width() : 1.0;
                const Matrix m = Matrix::translate(numberParam(p, "x", 0), numberParam(p, "y", 0)) * Matrix::scale(k, k);
                Shape g;
                g.kind = ShapeKind::Group;
                g.name = stringParam(p, "name", "trace");
                const std::string gid = doc.add(std::move(g));
                for (const auto& r : regions) {
                    BezierPath bp = parsePath(r.d);
                    transformPath(bp, m);
                    Shape s;
                    s.kind = ShapeKind::Path;
                    s.d = pathData(bp);
                    s.fill = r.color;
                    s.stroke.reset();
                    s.parent = gid;
                    doc.add(std::move(s));
                }
                Result res = Result::success("traced " + stringParam(p, "path") + " into " + std::to_string(regions.size()) + " paths in group " + gid, gid);
                res.with("shape", gid).with("paths", static_cast<std::int64_t>(regions.size()));
                return maybePreview(res, doc, p);
            }));
}

}  // namespace

void registerBoardCommands(Session& session) {
    registerBoardAndSymbolCommands(session);
    registerRepeatAndSelectCommands(session);
}

}  // namespace wetwaresuite::vector::detail
