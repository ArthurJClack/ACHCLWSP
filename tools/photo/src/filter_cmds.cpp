// The Filter menu as commands: blur, sharpen, noise, stylize, render, distort,
// liquify, plus select subject and refine edge. Pixel filters work on one
// layer (or all of them) and stay inside the selection when there is one.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <iterator>

#include "commands.hpp"
#include "wetwaresuite/raster/filters.hpp"
#include "wetwaresuite/raster/ops.hpp"
#include "wetwaresuite/raster/raw.hpp"
#include "wetwaresuite/raster/selection.hpp"

namespace wetwaresuite::photo {

using namespace raster;

Result forPixelLayers(ImageDoc& doc, const Params& p, const std::function<void(Image&)>& fn) {
    std::string error;
    if (boolParam(p, "all_layers", false)) {
        for (int i = 0; i < static_cast<int>(doc.image.layers.size()); ++i) {
            if (doc.image.layers[static_cast<std::size_t>(i)].hasPixels() && pixelLayer(doc, i, error)) fn(doc.image.layers[static_cast<std::size_t>(i)].image);
        }
        return Result::success("");
    }
    const int idx = pixelLayerIndex(doc, p, error);
    if (idx < 0) return Result::failure(error);
    fn(doc.image.layers[static_cast<std::size_t>(idx)].image);
    return Result::success("");
}

namespace {

std::string num(double v) {
    char buf[32];
    std::snprintf(buf, sizeof buf, "%g", std::round(v * 100) / 100);
    return buf;
}

ParamSpec numberSpec(const char* name, const char* description, double fallback) {
    return {name, ParamType::Number, description, false, {}, fallback};
}

std::vector<ParamSpec> withLayer(std::vector<ParamSpec> v) {
    v.insert(v.begin(), docParam());
    v.push_back(layerParam());
    v.push_back({"all_layers", ParamType::Bool, "apply to every layer instead of one", false, {}, false});
    v.push_back(previewParam());
    return v;
}

// A filter command: run fn on the layer(s) and say what happened.
ActionFactory filter(Session& s, std::function<std::string(ImageDoc&, const Params&, Image&)> fn) {
    return imageCommand(s, [fn](ImageDoc& doc, const Params& p) {
        std::string what;
        Result r = forPixelLayers(doc, p, [&](Image& img) { what = fn(doc, p, img); });
        if (!r.ok) return r;
        if (what.rfind("error:", 0) == 0) return Result::failure(what.substr(6));
        Result out = Result::success(what + (doc.hasSelection() ? " inside the selection" : ""));
        out.with("doc", stringParam(p, "doc"));
        return maybePreview(out, doc, p);
    });
}

double centre(const Params& p, const char* key, int size) { return p.count(key) ? numberParam(p, key) : size / 2.0; }

}  // namespace

void registerFilterCommands(Session& session) {
    auto& reg = session.registry();

    reg.add({"photo.blur",
             "Blur",
             "Blur a layer: gaussian (soft, radius px), motion (streaks along angle, distance px), spin (turns around a "
             "centre by amount degrees) or zoom (streaks out from a centre, amount 0..100).",
             withLayer({{"kind", ParamType::String, "which blur", false, {"gaussian", "motion", "spin", "zoom"}, std::string("gaussian")},
                        numberSpec("radius", "gaussian: radius in px", 4), numberSpec("angle", "motion: direction in degrees, 0 = sideways", 0),
                        numberSpec("distance", "motion: streak length in px", 20), numberSpec("amount", "spin: degrees; zoom: 0..100", 10),
                        {"center_x", ParamType::Number, "spin/zoom centre (default: the middle)", false},
                        {"center_y", ParamType::Number, "spin/zoom centre (default: the middle)", false}}),
             true},
            filter(session, [](ImageDoc& doc, const Params& p, Image& img) {
                const std::string kind = stringParam(p, "kind", "gaussian");
                if (kind == "motion") {
                    motionBlur(img, numberParam(p, "angle", 0), numberParam(p, "distance", 20));
                    return "motion blur " + num(numberParam(p, "distance", 20)) + " px at " + num(numberParam(p, "angle", 0)) + " degrees";
                }
                if (kind == "spin" || kind == "zoom") {
                    const double cx = centre(p, "center_x", doc.image.width), cy = centre(p, "center_y", doc.image.height);
                    kind == "spin" ? spinBlur(img, cx, cy, numberParam(p, "amount", 10)) : zoomBlur(img, cx, cy, numberParam(p, "amount", 10));
                    return kind + " blur";
                }
                gaussianBlur(img, numberParam(p, "radius", 4));
                return "gaussian blur " + num(numberParam(p, "radius", 4)) + " px";
            }));

    reg.add({"photo.sharpen",
             "Sharpen",
             "Sharpen a layer. unsharp_mask: amount % (100 = strong), radius px, threshold 0..255 leaves smooth areas alone. "
             "high_pass: keeps only the fine detail on gray (put it on a copy set to overlay to sharpen).",
             withLayer({{"kind", ParamType::String, "how", false, {"unsharp_mask", "high_pass"}, std::string("unsharp_mask")},
                        numberSpec("amount", "unsharp mask: 1..500 %", 100), numberSpec("radius", "px", 2),
                        {"threshold", ParamType::Int, "unsharp mask: 0..255", false, {}, std::int64_t{0}}}),
             true},
            filter(session, [](ImageDoc&, const Params& p, Image& img) {
                if (stringParam(p, "kind", "unsharp_mask") == "high_pass") {
                    highPass(img, numberParam(p, "radius", 2));
                    return std::string("high pass");
                }
                unsharpMaskThreshold(img, numberParam(p, "radius", 2), std::clamp(numberParam(p, "amount", 100), 0.0, 500.0),
                                     static_cast<int>(std::clamp<std::int64_t>(intParam(p, "threshold", 0), 0, 255)));
                return std::string("unsharp mask");
            }));

    reg.add({"photo.noise",
             "Noise",
             "Add or remove noise. add: amount 0..400 (gaussian or uniform, monochrome = no colour speckles). median: each "
             "pixel takes the middle value around it (radius). despeckle: a gentle median that keeps edges. dust_scratches: "
             "median only where a pixel stands out by more than threshold.",
             withLayer({{"kind", ParamType::String, "what to do", false, {"add", "median", "despeckle", "dust_scratches"}, std::string("add")},
                        numberSpec("amount", "add: 0..400", 10),
                        {"distribution", ParamType::String, "add: kind of noise", false, {"gaussian", "uniform"}, std::string("gaussian")},
                        {"monochrome", ParamType::Bool, "add: the same noise in every channel", false, {}, false},
                        {"radius", ParamType::Int, "median, dust_scratches: px", false, {}, std::int64_t{2}},
                        {"threshold", ParamType::Int, "dust_scratches, despeckle: 0..255", false, {}, std::int64_t{20}},
                        {"seed", ParamType::Int, "add: change for a different pattern", false, {}, std::int64_t{1}}}),
             true},
            filter(session, [](ImageDoc&, const Params& p, Image& img) {
                const std::string kind = stringParam(p, "kind", "add");
                const int radius = static_cast<int>(std::clamp<std::int64_t>(intParam(p, "radius", 2), 1, 50));
                const int threshold = static_cast<int>(std::clamp<std::int64_t>(intParam(p, "threshold", 20), 0, 255));
                if (kind == "median") medianFilter(img, radius);
                else if (kind == "despeckle") dustAndScratches(img, 1, threshold);
                else if (kind == "dust_scratches") dustAndScratches(img, radius, threshold);
                else addNoise(img, numberParam(p, "amount", 10), stringParam(p, "distribution", "gaussian") == "gaussian", boolParam(p, "monochrome", false),
                              static_cast<std::uint32_t>(intParam(p, "seed", 1)));
                return kind == "add" ? std::string("noise added") : kind + " applied";
            }));

    reg.add({"photo.stylize",
             "Stylize",
             "emboss: a gray relief lit from angle (height px, amount %). find_edges: dark lines where colours change on "
             "white. oil_paint: flat painterly strokes the size of radius that keep edges.",
             withLayer({{"kind", ParamType::String, "which effect", true, {"emboss", "find_edges", "oil_paint"}},
                        numberSpec("angle", "emboss: light direction, degrees", 135), numberSpec("height", "emboss: px", 2),
                        numberSpec("amount", "emboss: %", 100), {"radius", ParamType::Int, "oil paint: brush size px", false, {}, std::int64_t{4}}}),
             true},
            filter(session, [](ImageDoc&, const Params& p, Image& img) {
                const std::string kind = stringParam(p, "kind");
                if (kind == "emboss") emboss(img, numberParam(p, "angle", 135), numberParam(p, "height", 2), numberParam(p, "amount", 100));
                else if (kind == "find_edges") findEdges(img);
                else oilPaint(img, static_cast<int>(std::clamp<std::int64_t>(intParam(p, "radius", 4), 1, 40)));
                return kind + " applied";
            }));

    reg.add({"photo.clouds",
             "Render clouds",
             "Fill a layer with soft cloud-like noise between two colours (blend=difference for difference clouds).",
             withLayer({colorParam("from", "dark colour", false, "#000000"), colorParam("to", "light colour", false, "#ffffff"),
                        numberSpec("scale", "size of the biggest puffs, px", 128), {"seed", ParamType::Int, "change for different clouds", false, {}, std::int64_t{1}},
                        {"blend", ParamType::String, "how the clouds go onto the layer", false, blendNames(), std::string("normal")},
                        numberSpec("opacity", "0..100", 100)}),
             true},
            filter(session, [](ImageDoc&, const Params& p, Image& img) {
                Color a, b;
                std::string error;
                if (!colorArg(p, "from", "#000000", a, error) || !colorArg(p, "to", "#ffffff", b, error)) return "error:" + error;
                Image clouds(img.width(), img.height());
                renderClouds(clouds, a, b, numberParam(p, "scale", 128), static_cast<std::uint32_t>(intParam(p, "seed", 1)));
                const Blend mode = *parseBlend(stringParam(p, "blend", "normal"));
                if (mode == Blend::Normal && numberParam(p, "opacity", 100) >= 100) img = std::move(clouds);
                else blendOnto(img, clouds, 0, 0, mode, std::clamp(numberParam(p, "opacity", 100), 0.0, 100.0) / 100);
                return std::string("clouds rendered");
            }));

    reg.add({"photo.develop_raw",
             "Develop raw",
             "Develop the camera raw (.dng) this image was opened from again with new settings: exposure in stops, white "
             "balance (as_shot, auto, a preset, or custom with temperature in kelvin and tint -100 green..100 magenta), "
             "highlights and shadows -100..100 (negative highlights pull bright areas back, positive shadows lift dark ones), "
             "contrast and saturation -100..100, half_size for a quick 4x faster look. Replaces the bottom layer's pixels; "
             "layers you added on top stay.",
             {docParam(), numberSpec("exposure", "stops, -5..5", 0),
              {"white_balance", ParamType::String, "white balance", false,
               {"as_shot", "auto", "daylight", "cloudy", "shade", "tungsten", "fluorescent", "flash", "custom"}, std::string("as_shot")},
              numberSpec("temperature", "custom: kelvin 2000..12000", 5500), numberSpec("tint", "custom: -100 green .. 100 magenta", 0),
              numberSpec("highlights", "-100..100", 0), numberSpec("shadows", "-100..100", 0), numberSpec("contrast", "-100..100", 0),
              numberSpec("saturation", "-100..100", 0), {"half_size", ParamType::Bool, "half the size, 4x faster", false, {}, false},
              previewParam()},
             true},
            imageCommand(session, [](ImageDoc& doc, const Params& p) {
                std::ifstream in(doc.path, std::ios::binary);
                const std::string bytes = in ? std::string((std::istreambuf_iterator<char>(in)), {}) : std::string();
                if (!isDng(bytes)) return Result::failure("this image wasn't opened from a camera raw (.dng) file");
                RawDevelop set;
                set.exposure = std::clamp(numberParam(p, "exposure", 0), -5.0, 5.0);
                set.whiteBalance = stringParam(p, "white_balance", "as_shot");
                set.temperature = std::clamp(numberParam(p, "temperature", 5500), 2000.0, 12000.0);
                set.tint = numberParam(p, "tint", 0);
                set.highlights = numberParam(p, "highlights", 0);
                set.shadows = numberParam(p, "shadows", 0);
                set.contrast = numberParam(p, "contrast", 0);
                set.saturation = numberParam(p, "saturation", 0);
                set.halfSize = boolParam(p, "half_size", false);
                Image img;
                try {
                    img = developDng(bytes, set);
                } catch (const std::exception& e) {
                    return Result::failure(e.what());
                }
                if (doc.image.layers.empty()) return Result::failure("the image has no layers");
                if ((img.width() != doc.image.width || img.height() != doc.image.height)) {
                    for (auto& l : doc.image.layers) {  // other layers keep their place, scaled with the canvas
                        if (&l != &doc.image.layers.front() && !l.image.empty()) l.image = resize(l.image, img.width(), img.height());
                        if (!l.mask.empty()) l.mask = Mask();
                    }
                    doc.image.width = img.width();
                    doc.image.height = img.height();
                    doc.selection = Mask();
                }
                doc.image.layers.front().image = std::move(img);
                const RawInfo info = dngInfo(bytes);
                Result r = Result::success("developed " + info.camera + " raw (" + info.layout + ", " + std::to_string(info.bits) + "-bit), white balance " +
                                           set.whiteBalance + ", exposure " + num(set.exposure));
                r.with("doc", stringParam(p, "doc")).with("width", static_cast<std::int64_t>(doc.image.width)).with("height", static_cast<std::int64_t>(doc.image.height));
                return maybePreview(r, doc, p);
            }));
}

void registerDistortCommands(Session& session) {
    auto& reg = session.registry();

    reg.add({"photo.displace",
             "Displace",
             "Move pixels by a displacement map: its red channel pushes them sideways and green up/down (mid gray = stay), "
             "by up to scale px. The map is another layer or an image file, stretched to fit.",
             withLayer({{"map_layer", ParamType::Int, "layer to use as the map", false}, {"map_path", ParamType::String, "image file to use as the map", false},
                        numberSpec("scale_x", "px at full red", 10), numberSpec("scale_y", "px at full green", 10),
                        {"wrap", ParamType::Bool, "wrap around the edges instead of repeating them", false, {}, false}}),
             true},
            filter(session, [](ImageDoc& doc, const Params& p, Image& img) {
                Image map;
                if (p.count("map_layer")) {
                    const auto n = intParam(p, "map_layer");
                    if (n < 0 || n >= static_cast<std::int64_t>(doc.image.layers.size())) return std::string("error:map_layer doesn't exist");
                    map = doc.image.layers[static_cast<std::size_t>(n)].image;
                } else if (p.count("map_path")) {
                    try {
                        map = openImage(stringParam(p, "map_path"))->flatten();
                    } catch (const std::exception& e) {
                        return std::string("error:can't open the map: ") + e.what();
                    }
                } else {
                    return std::string("error:give map_layer or map_path");
                }
                if (map.empty()) return std::string("error:the map has no pixels");
                img = displace(img, map, numberParam(p, "scale_x", 10), numberParam(p, "scale_y", 10), boolParam(p, "wrap", false));
                return std::string("displaced");
            }));

    reg.add({"photo.lens_correction",
             "Lens correction",
             "Fix lens faults: distortion -100..100 (negative straightens barrel bulging, positive pincushion), vignette "
             "-100..100 (positive brightens dark corners), chromatic -10..10 px (removes red/blue colour fringes).",
             withLayer({numberSpec("distortion", "-100..100", 0), numberSpec("vignette", "-100..100", 0), numberSpec("chromatic", "-10..10 px", 0)}),
             true},
            filter(session, [](ImageDoc&, const Params& p, Image& img) {
                img = lensCorrect(img, numberParam(p, "distortion", 0), numberParam(p, "vignette", 0), numberParam(p, "chromatic", 0));
                return std::string("lens corrected");
            }));

    reg.add({"photo.warp",
             "Warp",
             "Bend a layer smoothly. A preset (arc, bulge, wave, flag, rise, twist) with bend -100..100, or your own grid: "
             "(cols+1) x (rows+1) control points spread over the image, and `moves` says how far each moves (dx,dy pairs, "
             "row by row from the top-left).",
             withLayer({{"preset", ParamType::String, "a ready-made warp", false, {"none", "arc", "bulge", "wave", "flag", "rise", "twist"}, std::string("none")},
                        numberSpec("bend", "preset strength -100..100", 30),
                        {"cols", ParamType::Int, "grid cells across", false, {}, std::int64_t{4}},
                        {"rows", ParamType::Int, "grid cells down", false, {}, std::int64_t{4}},
                        {"moves", ParamType::NumberList, "dx,dy for each control point", false}}),
             true},
            filter(session, [](ImageDoc&, const Params& p, Image& img) {
                int cols = static_cast<int>(std::clamp<std::int64_t>(intParam(p, "cols", 4), 1, 64));
                int rows = static_cast<int>(std::clamp<std::int64_t>(intParam(p, "rows", 4), 1, 64));
                std::vector<double> moves = listParam(p, "moves");
                const std::string preset = stringParam(p, "preset", "none");
                if (preset != "none") {
                    cols = rows = 8;
                    moves.clear();
                    const double bend = std::clamp(numberParam(p, "bend", 30), -100.0, 100.0) / 100, W = img.width(), H = img.height();
                    for (int j = 0; j <= rows; ++j) {
                        for (int i = 0; i <= cols; ++i) {
                            const double u = i / double(cols), v = j / double(rows), cu = 2 * u - 1, cv = 2 * v - 1;
                            double dx = 0, dy = 0;
                            if (preset == "arc") dy = -bend * (1 - cu * cu) * H / 4;
                            else if (preset == "bulge") dx = bend * cu * (1 - cv * cv) * W / 8, dy = bend * cv * (1 - cu * cu) * H / 8;
                            else if (preset == "wave") dy = bend * std::sin(u * 2 * 3.14159265) * H / 8;
                            else if (preset == "flag") dy = bend * std::sin(u * 2 * 3.14159265) * u * H / 6;
                            else if (preset == "rise") dy = -bend * u * H / 4;
                            else {  // twist: turn the middle more than the edges
                                const double t = bend * (1 - std::min(1.0, std::hypot(cu, cv))) * 1.2;
                                const double rx = cu * W / 2, ry = cv * H / 2;
                                dx = rx * std::cos(t) - ry * std::sin(t) - rx, dy = rx * std::sin(t) + ry * std::cos(t) - ry;
                            }
                            moves.push_back(dx);
                            moves.push_back(dy);
                        }
                    }
                }
                const std::size_t need = static_cast<std::size_t>((cols + 1) * (rows + 1) * 2);
                if (moves.size() != need) {
                    return "error:moves needs " + std::to_string(need) + " numbers (dx,dy for each of the " + std::to_string((cols + 1) * (rows + 1)) +
                           " control points), or pick a preset";
                }
                img = warpGrid(img, cols, rows, moves);
                return preset == "none" ? std::string("warped") : preset + " warp";
            }));

    reg.add({"photo.liquify",
             "Liquify",
             "Push pixels around with a brush: push drags them along the stroke (points = x,y pairs), bloat swells and "
             "pucker shrinks what's under the brush, twirl_right/twirl_left turn it. One point = hold the brush still.",
             withLayer({{"mode", ParamType::String, "what the brush does", false, {"push", "bloat", "pucker", "twirl_right", "twirl_left"}, std::string("push")},
                        {"points", ParamType::NumberList, "brush path as x,y pairs", true}, numberSpec("size", "brush diameter px", 60),
                        numberSpec("pressure", "0..100", 50)}),
             true},
            filter(session, [](ImageDoc&, const Params& p, Image& img) {
                const NumberList pts = listParam(p, "points");
                if (pts.size() < 2 || pts.size() % 2) return std::string("error:points must be x,y pairs");
                const std::string m = stringParam(p, "mode", "push");
                const LiquifyMode mode = m == "bloat" ? LiquifyMode::Bloat : m == "pucker" ? LiquifyMode::Pucker : m == "twirl_right" ? LiquifyMode::TwirlRight
                                         : m == "twirl_left" ? LiquifyMode::TwirlLeft : LiquifyMode::Push;
                img = liquify(img, mode, pts, numberParam(p, "size", 60), std::clamp(numberParam(p, "pressure", 50), 0.0, 100.0) / 100);
                return "liquify " + m;
            }));
}

void registerSubjectCommands(Session& session) {
    auto& reg = session.registry();

    reg.add({"photo.select_subject",
             "Select subject",
             "Select the main thing in the picture: everything unlike the colours around the edges, joined up and with its "
             "holes filled. Works best on plain or simple backgrounds; refine it with photo.refine_edge.",
             {docParam(), {"tolerance", ParamType::Int, "how different from the background a colour must be, 1..255", false, {}, std::int64_t{40}},
              {"mode", ParamType::String, "replace, add, subtract or intersect", false, maskOpNames(), std::string("replace")}, previewParam()},
             true},
            imageCommand(session, [](ImageDoc& doc, const Params& p) {
                Mask m = subjectMask(doc.flatten(), static_cast<int>(std::clamp<std::int64_t>(intParam(p, "tolerance", 40), 1, 255)));
                if (!anySelected(m)) return Result::failure("couldn't find a subject that stands out from the background; try a lower tolerance");
                Mask sel = doc.selection;
                combine(sel, m, parseMaskOp(stringParam(p, "mode", "replace")).value_or(MaskOp::Replace));
                doc.selection = std::move(sel);
                const PixelRect b = bounds(doc.selection);
                Result r = Result::success("selected the subject: " + std::to_string(b.width()) + "x" + std::to_string(b.height()) + " at " +
                                           std::to_string(b.x0) + "," + std::to_string(b.y0));
                r.with("doc", stringParam(p, "doc")).with("coverage", std::round(coverage(doc.selection) * 10000) / 10000);
                return maybePreview(r, doc, p);
            }));

    reg.add({"photo.refine_edge",
             "Refine edge",
             "Make a selection's (or a layer mask's) edge follow the picture: hair, fur and soft edges come out cleaner. "
             "radius px is how far it looks; smooth px rounds it off; contrast 0..100 hardens it; shift -100..100 grows or shrinks it.",
             {docParam(), {"target", ParamType::String, "what to refine", false, {"selection", "mask"}, std::string("selection")},
              {"radius", ParamType::Int, "px", false, {}, std::int64_t{4}}, numberSpec("smooth", "px", 0), numberSpec("contrast", "0..100", 0),
              numberSpec("shift", "-100..100", 0), layerParam(), previewParam()},
             true},
            imageCommand(session, [](ImageDoc& doc, const Params& p) {
                const bool mask = stringParam(p, "target", "selection") == "mask";
                std::string error;
                const int idx = layerIndex(doc, p, error);
                if (idx < 0) return Result::failure(error);
                Mask* m = mask ? &doc.image.layers[static_cast<std::size_t>(idx)].mask : &doc.selection;
                if (m->empty()) return Result::failure(mask ? "layer " + std::to_string(idx) + " has no mask" : std::string("nothing is selected"));
                *m = refineEdge(*m, doc.flatten(), static_cast<int>(std::clamp<std::int64_t>(intParam(p, "radius", 4), 0, 200)), numberParam(p, "smooth", 0),
                                numberParam(p, "contrast", 0), numberParam(p, "shift", 0));
                Result r = Result::success(mask ? "refined the mask of layer " + std::to_string(idx) : std::string("refined the selection edge"));
                r.with("doc", stringParam(p, "doc"));
                return maybePreview(r, doc, p);
            }));
}

}  // namespace wetwaresuite::photo
