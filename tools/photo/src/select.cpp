#include "select.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <initializer_list>

#include "wetwaresuite/raster/ops.hpp"
#include "wetwaresuite/raster/selection.hpp"

namespace wetwaresuite::photo {

using namespace raster;

namespace {

constexpr double kPi = 3.14159265358979323846;

std::string num(double v) {
    char buf[32];
    std::snprintf(buf, sizeof buf, "%g", std::round(v * 100) / 100);
    return buf;
}

std::string rectText(const PixelRect& r) {
    return std::to_string(r.x0) + "," + std::to_string(r.y0) + " " + std::to_string(r.width()) + "x" + std::to_string(r.height());
}

// Adds what the selection is now to a result.
Result selectionResult(const std::string& what, const ImageDoc& doc, const Params& p) {
    Result r = Result::success(what);
    r.with("doc", stringParam(p, "doc"));
    if (!doc.hasSelection() || !anySelected(doc.selection)) {
        r.message += doc.hasSelection() ? "; nothing is selected now" : "; no selection, edits change the whole layer";
        r.with("selected", false);
        return maybePreview(r, doc, p);
    }
    const PixelRect b = bounds(doc.selection);
    const double cov = coverage(doc.selection);
    r.message += "; selected " + rectText(b) + " (" + num(cov * 100) + "% of the image)";
    r.with("selected", true)
        .with("x", static_cast<std::int64_t>(b.x0))
        .with("y", static_cast<std::int64_t>(b.y0))
        .with("width", static_cast<std::int64_t>(b.width()))
        .with("height", static_cast<std::int64_t>(b.height()))
        .with("coverage", std::round(cov * 10000) / 10000);
    return maybePreview(r, doc, p);
}

ParamSpec opParam() {
    return {"mode", ParamType::String, "replace the selection, add to it, subtract from it or keep only the overlap", false,
            maskOpNames(), std::string("replace")};
}

// The picture the wand and colour range look at.
Image samplePicture(const ImageDoc& doc, const Params& p, int layer) {
    if (stringParam(p, "sample", "image") == "layer" && !doc.image.layers[static_cast<std::size_t>(layer)].image.empty()) return doc.image.layers[static_cast<std::size_t>(layer)].image;
    return doc.flatten();
}

// Selected area, or everything when nothing is selected.
Mask selectedOrAll(const ImageDoc& doc) {
    if (doc.hasSelection()) return doc.selection;
    return Mask(doc.image.width, doc.image.height, 255);
}

// The box the transform works on: the selection's bounds, else the layer's visible pixels, else the canvas.
PixelRect sourceBox(const ImageDoc& doc, const Layer& layer) {
    if (doc.hasSelection()) {
        PixelRect b = bounds(doc.selection);
        if (!b.empty()) return b;
    }
    PixelRect b = bounds(alphaMask(layer.image));
    if (!b.empty()) return b;
    return {0, 0, doc.image.width, doc.image.height};
}

Matrix3 translate(double x, double y) { return {1, 0, x, 0, 1, y, 0, 0, 1}; }

// Runs every layer, mask and the selection through the same warp onto a new canvas size.
void warpAll(ImageDoc& doc, const Matrix3& inverse, int width, int height) {
    transformDocument(doc, [&](const Image& img) { return warp(img, inverse, width, height); });
}

// The biggest axis-aligned rectangle inside a w x h rectangle turned by `a` radians.
void innerRect(double w, double h, double a, double& outW, double& outH) {
    const bool wide = w >= h;
    const double lng = wide ? w : h, shrt = wide ? h : w;
    const double s = std::fabs(std::sin(a)), c = std::fabs(std::cos(a));
    if (shrt <= 2 * s * c * lng || std::fabs(s - c) < 1e-10) {
        const double x = 0.5 * shrt;
        outW = wide ? x / s : x / c;
        outH = wide ? x / c : x / s;
    } else {
        const double c2 = c * c - s * s;
        outW = (w * c - h * s) / c2;
        outH = (h * c - w * s) / c2;
    }
}

std::vector<std::pair<double, double>> curvePreset(const std::string& name) {
    if (name == "contrast") return {{0, 0}, {64, 50}, {192, 205}, {255, 255}};
    if (name == "strong_contrast") return {{0, 0}, {64, 36}, {192, 219}, {255, 255}};
    if (name == "brighten") return {{0, 0}, {128, 160}, {255, 255}};
    if (name == "darken") return {{0, 0}, {128, 96}, {255, 255}};
    if (name == "fade") return {{0, 32}, {255, 224}};
    if (name == "invert") return {{0, 255}, {255, 0}};
    return {};
}

bool shiftArg(const Params& p, const std::string& key, ColorShift& out, std::string& error) {
    if (!p.count(key)) return true;
    NumberList v = listParam(p, key);
    if (v.size() != 3) {
        error = key + " needs three numbers -100..100: cyan-red, magenta-green, yellow-blue, e.g. [10, 0, -15]";
        return false;
    }
    out = {std::clamp(v[0], -100.0, 100.0), std::clamp(v[1], -100.0, 100.0), std::clamp(v[2], -100.0, 100.0)};
    return true;
}

template <typename Fn>
Result onLayersSel(ImageDoc& doc, const Params& p, Fn fn) {
    std::string error;
    if (boolParam(p, "all_layers", false)) {
        for (std::size_t i = 0; i < doc.image.layers.size(); ++i) {
            if (doc.image.layers[i].hasPixels() && pixelLayer(doc, static_cast<int>(i), error)) fn(doc.image.layers[i].image);
        }
        return Result::success("");
    }
    const int idx = pixelLayerIndex(doc, p, error);
    if (idx < 0) return Result::failure(error);
    fn(doc.image.layers[static_cast<std::size_t>(idx)].image);
    return Result::success("");
}

ParamSpec allLayers() { return {"all_layers", ParamType::Bool, "apply to every layer instead of one", false, {}, false}; }

void registerSelection(Session& session) {
    auto& reg = session.registry();

    reg.add({"photo.select",
             "Select",
             "Choose the part of the picture that edits change. shape: rect or ellipse (x, y, width, height), lasso (points "
             "as x,y pairs), wand (pixels like the one at x,y within tolerance; contiguous=false takes them from everywhere), "
             "color (pixels near color, or near the one at x,y), layer (the layer's visible pixels), all, none or invert. "
             "mode adds to, subtracts from or intersects the current selection; feather softens the edge. While something "
             "is selected, adjustments, effects, curves, painting and filling only change inside it.",
             {docParam(),
              {"shape", ParamType::String, "how to select", true, {"rect", "ellipse", "lasso", "wand", "color", "layer", "all", "none", "invert"}},
              {"x", ParamType::Number, "left edge (rect, ellipse) or the pixel to match (wand, color)", false},
              {"y", ParamType::Number, "top edge (rect, ellipse) or the pixel to match (wand, color)", false},
              {"width", ParamType::Number, "width (rect, ellipse)", false},
              {"height", ParamType::Number, "height (rect, ellipse)", false},
              {"points", ParamType::NumberList, "lasso outline as x,y pairs, e.g. [10,10, 200,30, 120,180]", false},
              {"tolerance", ParamType::Int, "how different a pixel may be, 0..255 (wand, color)", false, {}, std::int64_t{32}},
              {"contiguous", ParamType::Bool, "wand: only pixels joined to the clicked one", false, {}, true},
              colorParam("color", "colour to select (color shape; default: the pixel at x,y)", false, ""),
              {"fuzz", ParamType::Int, "color: extra range over which pixels fade out of the selection", false, {}, std::int64_t{0}},
              {"sample", ParamType::String, "wand/color look at the whole image or only the layer", false, {"image", "layer"}, std::string("image")},
              opParam(),
              {"feather", ParamType::Number, "soften the new shape's edge by this many px", false, {}, 0.0},
              {"antialias", ParamType::Bool, "smooth edges on ellipses and lassos", false, {}, true},
              layerParam(), previewParam()},
             true},
            imageCommand(session, [](ImageDoc& doc, const Params& p) {
                const std::string shape = stringParam(p, "shape");
                const int W = doc.image.width, H = doc.image.height;
                std::string error;
                const int layer = layerIndex(doc, p, error);
                if (layer < 0) return Result::failure(error);
                if (shape == "none") {
                    doc.selection = Mask();
                    return selectionResult("selection cleared", doc, p);
                }
                if (shape == "invert") {
                    Mask m = selectedOrAll(doc);
                    if (!doc.hasSelection()) m = Mask(W, H, 0);  // inverting "everything" selects nothing
                    invert(m);
                    doc.selection = std::move(m);
                    return selectionResult("selection inverted", doc, p);
                }
                auto op = parseMaskOp(stringParam(p, "mode", "replace"));
                if (!op) return Result::failure("mode must be replace, add, subtract or intersect");
                const bool aa = boolParam(p, "antialias", true);
                auto need = [&](std::initializer_list<const char*> keys) {
                    for (const char* k : keys) {
                        if (!p.count(k)) {
                            error = "shape=" + shape + " needs " + k;
                            return false;
                        }
                    }
                    return true;
                };
                Mask m;
                if (shape == "all") {
                    m = Mask(W, H, 255);
                } else if (shape == "rect" || shape == "ellipse") {
                    if (!need({"x", "y", "width", "height"})) return Result::failure(error);
                    const double x = numberParam(p, "x"), y = numberParam(p, "y"), w = numberParam(p, "width"), h = numberParam(p, "height");
                    if (w <= 0 || h <= 0) return Result::failure("width and height must be above 0");
                    m = shape == "rect" ? rectMask(W, H, static_cast<int>(std::lround(x)), static_cast<int>(std::lround(y)),
                                                   static_cast<int>(std::lround(w)), static_cast<int>(std::lround(h)))
                                        : ellipseMask(W, H, x, y, w, h, aa);
                } else if (shape == "lasso") {
                    NumberList pts = listParam(p, "points");
                    if (pts.size() < 6 || pts.size() % 2) return Result::failure("lasso needs at least 3 points as x,y pairs, e.g. [10,10, 200,30, 120,180]");
                    m = polygonMask(W, H, pts, aa);
                } else if (shape == "wand" || shape == "color") {
                    const bool haveXY = p.count("x") && p.count("y");
                    const int x = haveXY ? static_cast<int>(std::floor(numberParam(p, "x"))) : -1;
                    const int y = haveXY ? static_cast<int>(std::floor(numberParam(p, "y"))) : -1;
                    const int tol = static_cast<int>(std::clamp<std::int64_t>(intParam(p, "tolerance", 32), 0, 255));
                    const Image pic = samplePicture(doc, p, layer);
                    if (shape == "wand") {
                        if (!haveXY) return Result::failure("shape=wand needs x and y (the pixel to start from)");
                        if (!pic.contains(x, y)) return Result::failure("that point is outside the image");
                        m = wandMask(pic, x, y, tol, boolParam(p, "contiguous", true));
                    } else {
                        Color c;
                        if (!stringParam(p, "color", "").empty()) {
                            if (!colorArg(p, "color", "", c, error)) return Result::failure(error);
                        } else if (haveXY && pic.contains(x, y)) {
                            c = pic.at(x, y);
                        } else {
                            return Result::failure("shape=color needs color or x and y (a pixel of that colour)");
                        }
                        m = colorRangeMask(pic, c, tol, static_cast<int>(std::clamp<std::int64_t>(intParam(p, "fuzz", 0), 0, 255)));
                    }
                } else if (shape == "layer") {
                    m = alphaMask(doc.image.layers[static_cast<std::size_t>(layer)].image);
                    if (m.width != W || m.height != H) m = Mask(W, H);
                }
                const double fr = numberParam(p, "feather", 0);
                if (fr > 0) feather(m, fr);
                combine(doc.selection, m, *op);
                return selectionResult(shape + " " + (*op == MaskOp::Replace ? "selected" : stringParam(p, "mode") + "ed"), doc, p);
            }));

    reg.add({"photo.select_modify",
             "Change selection",
             "Change the current selection: grow or shrink it by px, feather (soften) its edge, or turn it into a border band.",
             {docParam(),
              {"action", ParamType::String, "what to do", true, {"grow", "shrink", "feather", "border"}},
              {"amount", ParamType::Number, "px", true},
              previewParam()},
             true},
            imageCommand(session, [](ImageDoc& doc, const Params& p) {
                if (!doc.hasSelection()) return Result::failure("nothing is selected; use photo.select first");
                const std::string action = stringParam(p, "action");
                const double amount = numberParam(p, "amount");
                if (amount <= 0) return Result::failure("amount must be above 0");
                const int r = static_cast<int>(std::lround(amount));
                if (action == "grow") grow(doc.selection, r);
                else if (action == "shrink") grow(doc.selection, -r);
                else if (action == "feather") feather(doc.selection, amount);
                else doc.selection = border(doc.selection, std::max(1, r));
                return selectionResult("selection " + (action == "grow" ? std::string("grown") : action == "shrink" ? "shrunk" : action == "feather" ? "feathered" : "turned into a border"), doc, p);
            }));

    reg.add({"photo.fill_selection",
             "Fill selection",
             "Fill the selected area of a layer with a colour (the whole layer when nothing is selected).",
             {docParam(), colorParam("color", "fill colour", true, ""),
              {"opacity", ParamType::Number, "0..100", false, {}, 100.0}, layerParam(), previewParam()},
             true},
            imageCommand(session, [](ImageDoc& doc, const Params& p) {
                Color c;
                std::string error;
                if (!colorArg(p, "color", "", c, error)) return Result::failure(error);
                const int layer = pixelLayerIndex(doc, p, error);
                if (layer < 0) return Result::failure(error);
                Image& img = doc.image.layers[static_cast<std::size_t>(layer)].image;
                Image fill(img.width(), img.height(), c);
                blendOnto(img, fill, 0, 0, Blend::Normal, std::clamp(numberParam(p, "opacity", 100), 0.0, 100.0) / 100);
                Result r = Result::success("filled " + std::string(doc.hasSelection() ? "the selection" : "the layer") + " with " + c.hex());
                r.with("doc", stringParam(p, "doc"));
                return maybePreview(r, doc, p);
            }));

    reg.add({"photo.delete_selected",
             "Delete selected pixels",
             "Make the selected pixels of a layer transparent.",
             {docParam(), layerParam(), previewParam()},
             true},
            imageCommand(session, [](ImageDoc& doc, const Params& p) {
                if (!doc.hasSelection()) return Result::failure("nothing is selected; use photo.select first (or paint.clear for the whole layer)");
                std::string error;
                const int layer = pixelLayerIndex(doc, p, error);
                if (layer < 0) return Result::failure(error);
                Image& img = doc.image.layers[static_cast<std::size_t>(layer)].image;
                img.fill({0, 0, 0, 0});  // the selection keeps what's outside it
                Result r = Result::success("deleted the selected pixels");
                r.with("doc", stringParam(p, "doc"));
                return maybePreview(r, doc, p);
            }));

    reg.add({"photo.copy_to_layer",
             "Selection to new layer",
             "Copy the selected pixels of a layer onto a new layer above it (cut=true also removes them from the original). "
             "The new layer becomes the active one.",
             {docParam(), {"cut", ParamType::Bool, "move the pixels instead of copying", false, {}, false},
              {"name", ParamType::String, "name for the new layer", false}, layerParam(), previewParam()},
             true},
            imageCommand(session, [](ImageDoc& doc, const Params& p) {
                std::string error;
                const int layer = pixelLayerIndex(doc, p, error);
                if (layer < 0) return Result::failure(error);
                const Mask sel = selectedOrAll(doc);
                Layer& src = doc.image.layers[static_cast<std::size_t>(layer)];
                Layer copy;
                copy.name = stringParam(p, "name", src.name + (boolParam(p, "cut", false) ? " (cut)" : " (copy)"));
                copy.image = src.image;
                applyMask(copy.image, sel);
                if (boolParam(p, "cut", false)) {
                    Mask keep = sel;
                    invert(keep);
                    applyMask(src.image, keep);
                }
                doc.image.layers.insert(doc.image.layers.begin() + layer + 1, std::move(copy));
                doc.active = layer + 1;
                Result r = Result::success(std::string(boolParam(p, "cut", false) ? "moved" : "copied") + " the selection to new layer " +
                                           std::to_string(layer + 1));
                r.with("doc", stringParam(p, "doc")).with("layer", static_cast<std::int64_t>(layer + 1));
                return maybePreview(r, doc, p);
            }));

    reg.add({"photo.crop_selection",
             "Crop to selection",
             "Crop the image to the box around the selection.",
             {docParam(), previewParam()},
             true},
            imageCommand(session, [](ImageDoc& doc, const Params& p) {
                if (!doc.hasSelection()) return Result::failure("nothing is selected; use photo.select first");
                const PixelRect b = bounds(doc.selection);
                if (b.empty()) return Result::failure("the selection is empty");
                warpAll(doc, translate(b.x0, b.y0), b.width(), b.height());
                Result r = Result::success("cropped to " + rectText(b));
                r.with("doc", stringParam(p, "doc"))
                    .with("width", static_cast<std::int64_t>(doc.image.width))
                    .with("height", static_cast<std::int64_t>(doc.image.height));
                return maybePreview(r, doc, p);
            }));
}

void registerMasks(Session& session) {
    auto& reg = session.registry();

    reg.add({"photo.mask",
             "Layer mask",
             "Hide parts of a layer without erasing them. add makes a mask (from: reveal_all, hide_all, selection or "
             "transparency); while a selection exists, paint and fill edit pixels, not the mask, so build masks from "
             "selections. invert flips it, disable/enable turns it off and on, apply bakes it into the pixels, delete "
             "throws it away, select loads it as the selection.",
             {docParam(),
              {"action", ParamType::String, "what to do", true, {"add", "invert", "enable", "disable", "apply", "delete", "select"}},
              {"from", ParamType::String, "add: what the new mask shows", false, {"reveal_all", "hide_all", "selection", "transparency"},
               std::string("selection")},
              layerParam(), previewParam()},
             true},
            imageCommand(session, [](ImageDoc& doc, const Params& p) {
                std::string error;
                const int idx = layerIndex(doc, p, error);
                if (idx < 0) return Result::failure(error);
                Layer& l = doc.image.layers[static_cast<std::size_t>(idx)];
                const std::string action = stringParam(p, "action");
                const int W = doc.image.width, H = doc.image.height;
                std::string msg;
                if (action == "add") {
                    const std::string from = stringParam(p, "from", "selection");
                    if (from == "reveal_all") l.mask = Mask(W, H, 255);
                    else if (from == "hide_all") l.mask = Mask(W, H, 0);
                    else if (from == "transparency") l.mask = alphaMask(l.image);
                    else {
                        if (!doc.hasSelection()) return Result::failure("nothing is selected; pick from=reveal_all or select first");
                        l.mask = doc.selection;
                        if (l.mask.width != W || l.mask.height != H) return Result::failure("the layer isn't the size of the canvas");
                        doc.selection = Mask();  // like other editors: the selection became the mask
                    }
                    l.maskEnabled = true;
                    msg = "added a mask (" + from + ") to layer " + std::to_string(idx);
                } else {
                    if (l.mask.empty()) return Result::failure("layer " + std::to_string(idx) + " has no mask; use action=add first");
                    if (action == "invert") invert(l.mask), msg = "mask inverted";
                    else if (action == "enable") l.maskEnabled = true, msg = "mask on";
                    else if (action == "disable") l.maskEnabled = false, msg = "mask off (kept, ignored)";
                    else if (action == "apply") {
                        if (l.maskEnabled) applyMask(l.image, l.mask);
                        l.mask = Mask();
                        msg = "mask applied to the pixels";
                    } else if (action == "delete") {
                        l.mask = Mask();
                        msg = "mask deleted";
                    } else {
                        doc.selection = l.mask;
                        msg = "mask loaded as the selection";
                    }
                }
                Result r = Result::success(msg);
                r.with("doc", stringParam(p, "doc")).with("layer", static_cast<std::int64_t>(idx));
                return maybePreview(r, doc, p);
            }));

    reg.add({"photo.clip",
             "Clipping mask",
             "Clip a layer to the one below it: it only shows where that layer (the bottom of the clipped group) has "
             "pixels, like putting a texture inside text. on=false unclips.",
             {docParam(), {"on", ParamType::Bool, "clip (true) or unclip (false)", false, {}, true}, layerParam(), previewParam()},
             true},
            imageCommand(session, [](ImageDoc& doc, const Params& p) {
                std::string error;
                const int idx = layerIndex(doc, p, error);
                if (idx < 0) return Result::failure(error);
                const bool on = boolParam(p, "on", true);
                if (on && idx == 0) return Result::failure("the bottom layer has nothing below it to clip to");
                doc.image.layers[static_cast<std::size_t>(idx)].clip = on;
                Result r = Result::success(on ? "layer " + std::to_string(idx) + " now clips to the layer below"
                                              : "layer " + std::to_string(idx) + " unclipped");
                r.with("doc", stringParam(p, "doc"));
                return maybePreview(r, doc, p);
            }));
}

void registerTone(Session& session) {
    auto& reg = session.registry();

    reg.add({"photo.curves",
             "Curves",
             "Bend the tones with a smooth curve through points (input, output 0..255), e.g. [0,0, 64,48, 192,210, 255,255] "
             "for more contrast, on all colours or one channel. Or pick a preset. Inside the selection when there is one.",
             {docParam(),
              {"points", ParamType::NumberList, "curve points as input,output pairs", false},
              {"preset", ParamType::String, "a ready-made curve instead of points", false,
               {"contrast", "strong_contrast", "brighten", "darken", "fade", "invert"}},
              {"channel", ParamType::String, "which channel", false, {"rgb", "red", "green", "blue"}, std::string("rgb")},
              layerParam(), allLayers(), previewParam()},
             true},
            imageCommand(session, [](ImageDoc& doc, const Params& p) {
                std::vector<std::pair<double, double>> pts;
                if (p.count("points")) {
                    NumberList v = listParam(p, "points");
                    if (v.size() < 4 || v.size() % 2) return Result::failure("points must be input,output pairs, at least two, e.g. [0,0, 128,160, 255,255]");
                    for (std::size_t i = 0; i < v.size(); i += 2) pts.emplace_back(v[i], v[i + 1]);
                } else if (p.count("preset")) {
                    pts = curvePreset(stringParam(p, "preset"));
                } else {
                    return Result::failure("give points=[in,out, ...] or a preset");
                }
                const auto lut = curveTable(pts);
                std::array<std::uint8_t, 256> same{};
                for (int i = 0; i < 256; ++i) same[static_cast<std::size_t>(i)] = static_cast<std::uint8_t>(i);
                const std::string ch = stringParam(p, "channel", "rgb");
                const auto& r = (ch == "rgb" || ch == "red") ? lut : same;
                const auto& g = (ch == "rgb" || ch == "green") ? lut : same;
                const auto& b = (ch == "rgb" || ch == "blue") ? lut : same;
                Result res = onLayersSel(doc, p, [&](Image& img) { applyCurves(img, r, g, b); });
                if (!res.ok) return res;
                Result out = Result::success("curves applied to " + ch + (doc.hasSelection() ? " inside the selection" : ""));
                out.with("doc", stringParam(p, "doc"));
                return maybePreview(out, doc, p);
            }));

    reg.add({"photo.color_balance",
             "Colour balance",
             "Shift the colours of the shadows, midtones and highlights separately. Each takes three numbers -100..100: "
             "cyan(-)/red(+), magenta(-)/green(+), yellow(-)/blue(+); e.g. midtones=[0,0,-20] warms the midtones.",
             {docParam(),
              {"shadows", ParamType::NumberList, "cyan-red, magenta-green, yellow-blue for dark tones", false},
              {"midtones", ParamType::NumberList, "the same for middle tones", false},
              {"highlights", ParamType::NumberList, "the same for bright tones", false},
              {"preserve_luminosity", ParamType::Bool, "keep brightness the same", false, {}, true},
              layerParam(), allLayers(), previewParam()},
             true},
            imageCommand(session, [](ImageDoc& doc, const Params& p) {
                ColorShift s, m, h;
                std::string error;
                if (!shiftArg(p, "shadows", s, error) || !shiftArg(p, "midtones", m, error) || !shiftArg(p, "highlights", h, error)) {
                    return Result::failure(error);
                }
                if (!p.count("shadows") && !p.count("midtones") && !p.count("highlights")) return Result::failure("give shadows, midtones or highlights");
                const bool keep = boolParam(p, "preserve_luminosity", true);
                Result res = onLayersSel(doc, p, [&](Image& img) { colorBalance(img, s, m, h, keep); });
                if (!res.ok) return res;
                Result out = Result::success("colour balance applied");
                out.with("doc", stringParam(p, "doc"));
                return maybePreview(out, doc, p);
            }));
}

void registerTransform(Session& session) {
    auto& reg = session.registry();

    reg.add({"photo.transform_box",
             "Transform box",
             "The box photo.transform works on: the selection's bounds, or the visible pixels of the layer when nothing is "
             "selected. The window draws its free-transform handles on it.",
             {docParam(), layerParam()},
             false,
             true},
            imageCommand(
                session,
                [](ImageDoc& doc, const Params& p) {
                    std::string error;
                    const int idx = pixelLayerIndex(doc, p, error);
                    if (idx < 0) return Result::failure(error);
                    const PixelRect b = sourceBox(doc, doc.image.layers[static_cast<std::size_t>(idx)]);
                    Result r = Result::success("box " + std::to_string(b.x0) + "," + std::to_string(b.y0) + " to " + std::to_string(b.x1) + "," +
                                               std::to_string(b.y1));
                    r.with("doc", stringParam(p, "doc"))
                        .with("x0", static_cast<std::int64_t>(b.x0))
                        .with("y0", static_cast<std::int64_t>(b.y0))
                        .with("x1", static_cast<std::int64_t>(b.x1))
                        .with("y1", static_cast<std::int64_t>(b.y1))
                        .with("layer", static_cast<std::int64_t>(idx))
                        .with("selection", doc.hasSelection());
                    return r;
                },
                false));

    reg.add({"photo.transform",
             "Free transform",
             "Rotate (any angle), scale, skew, move or distort the selected pixels, or the whole layer when nothing is "
             "selected. It turns around the middle of the selection (or of the layer's visible pixels) unless pivot_x/pivot_y "
             "are given. corners gives perspective instead: where the box's top-left, top-right, bottom-right and "
             "bottom-left corners go, as 8 numbers (from= says where they are now, to straighten a slanted poster or page).",
             {docParam(),
              {"angle", ParamType::Number, "rotate clockwise by degrees", false, {}, 0.0},
              {"scale", ParamType::Number, "scale both ways, percent", false, {}, 100.0},
              {"scale_x", ParamType::Number, "scale across, percent (overrides scale)", false},
              {"scale_y", ParamType::Number, "scale down, percent (overrides scale)", false},
              {"skew_x", ParamType::Number, "slant sideways, degrees", false, {}, 0.0},
              {"skew_y", ParamType::Number, "slant up/down, degrees", false, {}, 0.0},
              {"move_x", ParamType::Number, "move right by px", false, {}, 0.0},
              {"move_y", ParamType::Number, "move down by px", false, {}, 0.0},
              {"pivot_x", ParamType::Number, "point to turn and scale around", false},
              {"pivot_y", ParamType::Number, "point to turn and scale around", false},
              {"corners", ParamType::NumberList, "perspective: new top-left, top-right, bottom-right, bottom-left as x,y pairs", false},
              {"from", ParamType::NumberList, "perspective: where those four corners are now (default: the box's corners), "
               "e.g. a slanted poster's corners, to straighten it", false},
              {"smooth", ParamType::Bool, "smooth (bilinear) instead of blocky (nearest) pixels", false, {}, true},
              layerParam(), previewParam()},
             true},
            imageCommand(session, [](ImageDoc& doc, const Params& p) {
                std::string error;
                const int idx = pixelLayerIndex(doc, p, error);
                if (idx < 0) return Result::failure(error);
                Layer& layer = doc.image.layers[static_cast<std::size_t>(idx)];
                const PixelRect box = sourceBox(doc, layer);
                Matrix3 forward;
                std::string what;
                if (p.count("corners")) {
                    NumberList c = listParam(p, "corners");
                    if (c.size() != 8) return Result::failure("corners needs 8 numbers: top-left, top-right, bottom-right, bottom-left x,y");
                    std::array<double, 8> from = {double(box.x0), double(box.y0), double(box.x1), double(box.y0),
                                                  double(box.x1), double(box.y1), double(box.x0), double(box.y1)};
                    if (p.count("from")) {
                        NumberList f = listParam(p, "from");
                        if (f.size() != 8) return Result::failure("from needs 8 numbers: the four corners now, as x,y pairs");
                        std::copy(f.begin(), f.end(), from.begin());
                    }
                    std::array<double, 8> to{};
                    std::copy(c.begin(), c.end(), to.begin());
                    if (!homography(from, to, forward)) return Result::failure("those corners don't make a shape (three in a line?)");
                    what = "distorted";
                } else {
                    const double sx = numberParam(p, "scale_x", numberParam(p, "scale", 100)) / 100;
                    const double sy = numberParam(p, "scale_y", numberParam(p, "scale", 100)) / 100;
                    if (std::fabs(sx) < 0.001 || std::fabs(sy) < 0.001) return Result::failure("scale can't be 0");
                    const double a = numberParam(p, "angle", 0) * kPi / 180;
                    const double kx = std::tan(numberParam(p, "skew_x", 0) * kPi / 180), ky = std::tan(numberParam(p, "skew_y", 0) * kPi / 180);
                    const double px = p.count("pivot_x") ? numberParam(p, "pivot_x") : (box.x0 + box.x1) / 2.0;
                    const double py = p.count("pivot_y") ? numberParam(p, "pivot_y") : (box.y0 + box.y1) / 2.0;
                    const Matrix3 rot = {std::cos(a), -std::sin(a), 0, std::sin(a), std::cos(a), 0, 0, 0, 1};
                    const Matrix3 skew = {1, kx, 0, ky, 1, 0, 0, 0, 1};
                    const Matrix3 scale = {sx, 0, 0, 0, sy, 0, 0, 0, 1};
                    forward = multiply(translate(px + numberParam(p, "move_x", 0), py + numberParam(p, "move_y", 0)),
                                       multiply(rot, multiply(skew, multiply(scale, translate(-px, -py)))));
                    what = "transformed";
                }
                Matrix3 inverse;
                if (!invert(forward, inverse)) return Result::failure("that transform squashes everything flat");
                const int W = layer.image.width(), H = layer.image.height();
                const bool smooth = boolParam(p, "smooth", true);
                if (doc.hasSelection()) {
                    // lift the selected pixels, transform them and drop them back on what's left
                    Image lifted = layer.image;
                    applyMask(lifted, doc.selection);
                    Mask rest = doc.selection;
                    invert(rest);
                    applyMask(layer.image, rest);
                    blendOnto(layer.image, warp(lifted, inverse, W, H, smooth), 0, 0, Blend::Normal, 1.0);
                    doc.selection = imageToMask(warp(maskToImage(doc.selection), inverse, doc.image.width, doc.image.height, smooth));
                    doc.ignoreSelection = true;
                } else {
                    layer.image = warp(layer.image, inverse, W, H, smooth);
                    if (!layer.mask.empty()) layer.mask = imageToMask(warp(maskToImage(layer.mask), inverse, W, H, smooth));
                }
                Result r = Result::success(what + (doc.hasSelection() ? " the selection" : " layer " + std::to_string(idx)));
                r.with("doc", stringParam(p, "doc"));
                return maybePreview(r, doc, p);
            }));
}

}  // namespace

void rotateFree(ImageDoc& doc, double degrees, const std::string& mode, Color fill) {
    const double a = degrees * kPi / 180;
    const double W = doc.image.width, H = doc.image.height;
    const double c = std::cos(a), s = std::sin(a);
    double nw = W, nh = H;
    if (mode == "expand") {
        nw = std::fabs(W * c) + std::fabs(H * s);
        nh = std::fabs(W * s) + std::fabs(H * c);
    } else if (mode == "crop") {
        innerRect(W, H, a, nw, nh);
    }
    // crop rounds down (and loses a px at each edge) so no half-empty edge pixels are left
    const int outW = std::max(1, mode == "crop" ? static_cast<int>(nw) - 2 : static_cast<int>(std::lround(nw)));
    const int outH = std::max(1, mode == "crop" ? static_cast<int>(nh) - 2 : static_cast<int>(std::lround(nh)));
    // output centre -> rotate back -> source centre
    const Matrix3 rot = {c, s, 0, -s, c, 0, 0, 0, 1};  // inverse of a clockwise turn (y down)
    const Matrix3 inverse = multiply(translate(W / 2, H / 2), multiply(rot, translate(-outW / 2.0, -outH / 2.0)));
    warpAll(doc, inverse, outW, outH);
    if (fill.a > 0 && !doc.image.layers.empty() && doc.image.layers.front().hasPixels()) {
        Image& bottom = doc.image.layers.front().image;
        Image under(bottom.width(), bottom.height(), fill);
        blendOnto(under, bottom, 0, 0, Blend::Normal, 1.0);
        bottom = std::move(under);
    }
}

void registerSelectCommands(Session& session) {
    registerSelection(session);
    registerMasks(session);
    registerTone(session);
    registerTransform(session);
}

}  // namespace wetwaresuite::photo
