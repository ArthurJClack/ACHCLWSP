#include "wetwaresuite/photo/photo.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <functional>

#include "wetwaresuite/raster/codec.hpp"
#include "wetwaresuite/raster/document.hpp"
#include "wetwaresuite/raster/ops.hpp"
#include "heal.hpp"
#include "wetwaresuite/raster/parallel.hpp"
#include "wetwaresuite/raster/pyramid.hpp"
#include "wetwaresuite/raster/selection.hpp"
#include "select.hpp"
#include "commands.hpp"
#include "wetwaresuite/raster/simd.hpp"
#include "wetwaresuite/vector/vector.hpp"

namespace wetwaresuite::photo {

namespace fs = std::filesystem;
using namespace raster;

namespace {

std::string joinList(const std::vector<std::string>& items) {
    std::string out;
    for (const auto& i : items) out += (out.empty() ? "" : ", ") + i;
    return out;
}

// Result describing a document after an edit, with its id filled in.
Result docResult(const std::string& message, const ImageDoc& doc, const Params& p) {
    Result r = Result::success(message);
    r.with("doc", stringParam(p, "doc"))
        .with("width", static_cast<std::int64_t>(doc.image.width))
        .with("height", static_cast<std::int64_t>(doc.image.height));
    return maybePreview(r, doc, p);
}

// Applies fn to the chosen layer, or every layer when all_layers is set.
template <typename Fn>
Result onLayers(ImageDoc& doc, const Params& p, Fn fn) {
    std::string error;
    if (boolParam(p, "all_layers", false)) {
        for (std::size_t i = 0; i < doc.image.layers.size(); ++i) {
            if (doc.image.layers[i].hasPixels() && pixelLayer(doc, static_cast<int>(i), error)) fn(doc.image.layers[i].image);
        }
        return Result::success("");
    }
    int idx = pixelLayerIndex(doc, p, error);
    if (idx < 0) return Result::failure(error);
    fn(doc.image.layers[static_cast<std::size_t>(idx)].image);
    return Result::success("");
}

ParamSpec allLayersParam() {
    return {"all_layers", ParamType::Bool, "apply to every layer instead of one", false, {}, false};
}

// Geometry changes apply to every layer so they stay aligned.
// Masks and the selection go through the same change.
void transformAll(ImageDoc& doc, const std::function<Image(const Image&)>& fn) { transformDocument(doc, fn); }

std::string lowerExt(const fs::path& p) {
    std::string e = p.extension().string();
    if (!e.empty() && e[0] == '.') e.erase(0, 1);
    std::transform(e.begin(), e.end(), e.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return e;
}

// A test picture with gradients and soft edges, so every operation has real work to do.
Image benchmarkImage(int n) {
    Image img(n, n);
    for (int y = 0; y < n; ++y) {
        for (int x = 0; x < n; ++x) {
            const auto v = static_cast<std::uint8_t>((x * 255) / std::max(1, n - 1));
            const auto u = static_cast<std::uint8_t>((y * 255) / std::max(1, n - 1));
            img.set(x, y, {v, u, static_cast<std::uint8_t>((x ^ y) & 255), static_cast<std::uint8_t>(128 + ((x + y) & 127))});
        }
    }
    return img;
}

Result runBenchmark(const Params& p) {
    const int n = static_cast<int>(intParam(p, "size", 2048));
    if (n < 16 || n > 16384) return Result::failure("size must be 16..16384");
    const int threads = static_cast<int>(std::clamp<std::int64_t>(intParam(p, "threads", 0), 0, 256));
    const bool compare = boolParam(p, "compare", false);
    const bool useSimd = boolParam(p, "simd", true) && simdAvailable();
    const int previous = setThreadCount(threads);
    const bool previousSimd = setSimdEnabled(useSimd);
    using Clock = std::chrono::steady_clock;
    auto time = [](const auto& fn) {  // best of 3, so one slow run (a busy machine) doesn't skew it
        double best = 1e300;
        for (int i = 0; i < 3; ++i) {
            const auto t0 = Clock::now();
            fn();
            best = std::min(best, std::chrono::duration<double, std::milli>(Clock::now() - t0).count());
        }
        return best;
    };
    const Image src = benchmarkImage(n);
    const std::vector<Layer> layers = {{"a", src}, {"b", src, 0.5, Blend::Multiply}, {"c", src, 0.7, Blend::Screen}};
    struct Op {
        const char* key;
        const char* label;
        std::function<void()> fn;
    };
    const std::vector<Op> ops = {
        {"blur_ms", "blur (radius 8)", [&] {
             Image img = src;
             boxBlur(img, 8);
         }},
        {"resize_bilinear_ms", "resize to half, bilinear", [&] { (void)resize(src, n / 2, n / 2, Filter::Bilinear); }},
        {"resize_area_ms", "resize to a quarter, area", [&] { (void)resize(src, n / 4, n / 4, Filter::Area); }},
        {"resize_lanczos_ms", "resize to half, lanczos", [&] { (void)resize(src, n / 2, n / 2, Filter::Lanczos); }},
        {"composite_ms", "blend 3 layers", [&] { (void)composite(layers, n, n); }},
        {"adjust_ms", "saturation adjust", [&] {
             Image img = src;
             adjust(img, Adjust::Saturation, 30);
         }},
        {"pyramid_ms", "mipmap pyramid", [&] {
             LayeredImage li;
             li.width = li.height = n;
             li.layers.push_back({"a", src});
             Pyramid pyr;
             pyr.build(li);
         }},
    };
    Result r = Result::success("");
    std::string msg = std::to_string(n) + "x" + std::to_string(n) + " px on " + std::to_string(threadCount()) +
                      (threadCount() == 1 ? " thread" : " threads") + ", " + (useSimd ? simdName() : "scalar") + ":";
    for (const auto& op : ops) {
        const double ms = time(op.fn);
        r.with(op.key, std::round(ms * 10) / 10);
        char buf[96];
        std::snprintf(buf, sizeof buf, " %.1f ms", ms);
        msg += std::string("\n  ") + op.label + buf;
        if (compare && useSimd) {  // the same op again on the scalar loops
            setSimdEnabled(false);
            const double scalar = time(op.fn);
            setSimdEnabled(true);
            const std::string key = std::string(op.key);
            r.with(key.substr(0, key.size() - 3) + "_scalar_ms", std::round(scalar * 10) / 10);
            std::snprintf(buf, sizeof buf, " (scalar %.1f ms, %.2fx)", scalar, ms > 0 ? scalar / ms : 0.0);
            msg += buf;
        }
    }
    setThreadCount(previous);
    setSimdEnabled(previousSimd);
    r.message = msg;
    return r.with("size", static_cast<std::int64_t>(n))
        .with("threads", static_cast<std::int64_t>(threads > 0 ? threads : threadCount()))
        .with("simd", std::string(useSimd ? simdName() : "scalar"));
}

bool isSvg(const std::string& path) {
    std::string e = std::filesystem::path(path).extension().string();
    for (auto& c : e) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return e == ".svg";
}

// SVG drawn into pixels, as Photoshop does when it opens one: at its own size, scaled up when
// small so icons don't open as a blur. Saving goes to a .png beside it (pixels can't go back to SVG).
std::shared_ptr<ImageDoc> openSvg(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) throw std::runtime_error("can't read " + path);
    std::stringstream text;
    text << in.rdbuf();
    auto vec = vector::fromSvg(text.str());
    vec->background = {0, 0, 0, 0};  // see-through where the SVG draws nothing
    const double longest = std::max(vec->width, vec->height);
    const double scale = longest > 0 && longest < 1024 ? 1024 / longest : 1.0;
    Image img = vector::render(*vec, scale);
    auto doc = std::make_shared<ImageDoc>();
    doc->image.width = img.width();
    doc->image.height = img.height();
    doc->image.layers.push_back({"Background", std::move(img)});
    doc->active = 0;
    std::filesystem::path png = std::filesystem::path(path).replace_extension(".png");
    if (std::filesystem::exists(png)) png.replace_filename(png.stem().string() + "-pixels.png");
    doc->path = png.string();
    return doc;
}

}  // namespace

void registerCommands(Session& session) {
    auto& reg = session.registry();
    Session* s = &session;

    reg.add({"photo.open",
             "Open image",
             "Open an image file as a new document. Reads PNG, JPEG, BMP, TGA, GIF, HDR, PNM, WebP, TIFF, camera raw .dng "
             "(developed as shot; change it with photo.develop_raw), layered OpenRaster (.ora, "
             "everything kept), Photoshop .psd (layers, groups, masks, blend modes; flattened when it can't read the layers) "
             "and SVG (turned into pixels at its own size, at least 1024 pixels across; saves as .png next to it). "
             "Returns the document id to use with other commands.",
             {{"path", ParamType::String, "path to the image file", true}},
             false, true},
            [s](const Params& p) {
                return std::make_unique<FunctionAction>([s, path = stringParam(p, "path")] {
                    const bool svg = isSvg(path);
                    if (!svg && !canOpenImage(path)) return Result::failure("can't open " + path + ": unsupported format");
                    try {
                        auto doc = svg ? openSvg(path) : openImage(path);
                        std::string id = s->workspace().add(doc, "img");
                        std::string msg = "opened " + path + " as " + id;
                        if (lowerExt(path) == "dng") msg += " (camera raw, developed as shot: photo.develop_raw changes exposure, white balance...)";
                        else if (doc->sourceBits == 16) msg += " (16-bit: edits run at 8 bits, .png/.tif saves keep 16)";
                        if (!doc->colorProfile.empty()) msg += "\ncolour profile: " + doc->colorProfile + " (shown and edited as sRGB)";
                        Result r = Result::success(msg);
                        return r.with("doc", id)
                            .with("bits", static_cast<std::int64_t>(doc->sourceBits))
                            .with("profile", doc->colorProfile)
                            .with("width", static_cast<std::int64_t>(doc->image.width))
                            .with("height", static_cast<std::int64_t>(doc->image.height))
                            .with("layers", static_cast<std::int64_t>(doc->image.layers.size()));
                    } catch (const std::exception& e) {
                        return Result::failure(e.what());
                    }
                });
            });

    reg.add({"photo.new",
             "New image",
             "Create a blank image document.",
             {{"width", ParamType::Int, "width in pixels", true},
              {"height", ParamType::Int, "height in pixels", true},
              colorParam("background", "background colour", false, "#ffffff")},
             false, true},
            [s](const Params& p) {
                return std::make_unique<FunctionAction>([s, p] {
                    auto w = intParam(p, "width"), h = intParam(p, "height");
                    if (w < 1 || h < 1 || w > 30000 || h > 30000) return Result::failure("width and height must be 1..30000");
                    Color bg;
                    std::string error;
                    if (!colorArg(p, "background", "#ffffff", bg, error)) return Result::failure(error);
                    auto doc = newImage(static_cast<int>(w), static_cast<int>(h), bg);
                    std::string id = s->workspace().add(doc, "img");
                    return Result::success("created " + id).with("doc", id).with("width", w).with("height", h);
                });
            });

    reg.add({"photo.info",
             "Image info",
             "Describe an open image: size, layers top first (index, name, kind, opacity, blend, visibility, groups, masks, "
             "clipping, locks, styles, adjustment settings) and the active layer.",
             {docParam()},
             false, true},
            imageCommand(
                session,
                [](ImageDoc& doc, const Params& p) {
                    std::string text = doc.summary();
                    for (std::size_t i = doc.image.layers.size(); i-- > 0;) {
                        const auto& l = doc.image.layers[i];
                        std::string kind;
                        if (l.kind == LayerKind::Group) kind = l.passThrough ? "group, " : "group (isolated), ";
                        else if (l.kind == LayerKind::Adjustment) kind = (l.settings.count("type") ? l.settings.at("type") : "") + " adjustment, ";
                        else if (l.kind != LayerKind::Pixel) kind = toString(l.kind) + ", ";
                        std::string extra;
                        if (l.kind == LayerKind::Adjustment || l.kind == LayerKind::Text || l.kind == LayerKind::Shape) {
                            for (const auto& [k, v] : l.settings) {
                                if (k != "type") extra += (extra.empty() ? "" : " ") + k + "=" + v;
                            }
                            if (!extra.empty()) extra = " [" + extra + "]";
                        }
                        const std::string style = styleToText(l.style);
                        text += "\n  " + std::string(static_cast<std::size_t>(l.depth) * 2, ' ') + std::to_string(i) + ": " + l.name + " (" + kind +
                                toString(l.blend) + ", opacity " + std::to_string(static_cast<int>(std::lround(l.opacity * 100))) + "%" +
                                (l.fill < 1 ? ", fill " + std::to_string(static_cast<int>(std::lround(l.fill * 100))) + "%" : "") +
                                (l.visible ? "" : ", hidden") + (l.mask.empty() ? "" : l.maskEnabled ? ", masked" : ", mask off") +
                                (l.clip ? ", clipped" : "") + (l.lockAlpha ? ", alpha locked" : "") + (l.collapsed ? ", folded" : "") +
                                (static_cast<int>(i) == doc.active ? ", active" : "") + ")" + extra + (style.empty() ? "" : " style: " + style);
                    }
                    if (doc.hasSelection()) {
                        const PixelRect b = bounds(doc.selection);
                        text += b.empty() ? "\nselection: empty"
                                          : "\nselection: " + std::to_string(b.x0) + "," + std::to_string(b.y0) + " " +
                                                std::to_string(b.width()) + "x" + std::to_string(b.height());
                    }
                    if (doc.sourceBits == 16) text += "\nopened from a 16-bit file: .png/.tif saves keep 16 bits";
                    if (!doc.colorProfile.empty()) text += "\ncolour profile of the file: " + doc.colorProfile + " (converted to sRGB)";
                    Result r = Result::success(text);
                    return r.with("doc", stringParam(p, "doc"))
                        .with("width", static_cast<std::int64_t>(doc.image.width))
                        .with("height", static_cast<std::int64_t>(doc.image.height))
                        .with("layers", static_cast<std::int64_t>(doc.image.layers.size()))
                        .with("active_layer", static_cast<std::int64_t>(doc.active));
                },
                false));

    reg.add({"photo.preview",
             "Preview image",
             "Render the image (all visible layers) as a PNG so you can see it. Large images are scaled down.",
             {docParam(), {"max_size", ParamType::Int, "longest side of the preview in px", false, {}, std::int64_t{512}}},
             false, true},
            imageCommand(
                session,
                [](ImageDoc& doc, const Params& p) {
                    int size = static_cast<int>(std::clamp<std::int64_t>(intParam(p, "max_size", 512), 16, 4096));
                    Result r = Result::success("preview of " + stringParam(p, "doc"));
                    r.attach({"image/png", previewPng(doc.flatten(), size), stringParam(p, "doc") + ".png"});
                    return r;
                },
                false));

    reg.add({"photo.save",
             "Save image",
             "Save the image. The extension picks the format: .ora keeps everything (layers, groups, adjustment, text and "
             "shape layers, styles); .psd keeps layers, groups, masks and blend modes for Photoshop, GIMP and Krita (the "
             "result says what it had to flatten); .png, .jpg, .bmp, .tga, .tif, .webp and .ppm save the flattened image. "
             ".png and .tif can be 16 bits per channel: images opened from 16-bit files save that way unless bits=8.",
             {docParam(),
              {"path", ParamType::String, "file to write, e.g. out.png", true},
              {"quality", ParamType::Int, "JPEG quality 1..100", false, {}, std::int64_t{90}},
              {"bits", ParamType::Int, "bits per channel for .png/.tif: 8 or 16 (default: what the opened file had)", false}},
             false},
            imageCommand(
                session,
                [](ImageDoc& doc, const Params& p) {
                    std::string path = stringParam(p, "path");
                    if (auto parent = fs::path(path).parent_path(); !parent.empty()) {
                        std::error_code dirEc;
                        fs::create_directories(parent, dirEc);
                    }
                    if (!canSaveImage(path)) {
                        return Result::failure("can't save ." + lowerExt(path) + "; use one of: ora, psd, " + joinList(writableExtensions()));
                    }
                    const auto bits = intParam(p, "bits", 0);
                    if (bits != 0 && bits != 8 && bits != 16) return Result::failure("bits must be 8 or 16");
                    std::vector<std::string> notes;
                    try {
                        saveImage(doc, path, static_cast<int>(intParam(p, "quality", 90)), &notes, static_cast<int>(bits));
                    } catch (const std::exception& e) {
                        return Result::failure(e.what());
                    }
                    doc.path = path;
                    std::error_code ec;
                    auto bytes = static_cast<std::int64_t>(fs::file_size(path, ec));
                    std::string msg = "saved " + path;
                    for (const auto& n : notes) msg += "\n  note: " + n;
                    return Result::success(msg).with("path", path).with("bytes", bytes).with("notes", static_cast<std::int64_t>(notes.size()));
                },
                false));

    reg.add({"photo.close", "Close image", "Close an open image document (unsaved changes are lost).", {docParam()}, false},
            [s](const Params& p) {
                return std::make_unique<FunctionAction>([s, id = stringParam(p, "doc")] {
                    if (!s->workspace().getAs<ImageDoc>(id)) return Result::failure("no open image called '" + id + "'");
                    s->workspace().take(id);
                    return Result::success("closed " + id);
                });
            });

    reg.add({"photo.resize",
             "Resize image",
             "Scale the whole image. Give width, height or both; with only one, the other keeps the aspect ratio.",
             {docParam(),
              {"width", ParamType::Int, "new width in px (0 = keep aspect)", false},
              {"height", ParamType::Int, "new height in px (0 = keep aspect)", false},
              {"percent", ParamType::Number, "scale by a percentage instead, e.g. 50", false},
              {"filter", ParamType::String,
               "resampling filter: bilinear (smooth, default; averages areas when shrinking a lot), area (average), "
               "lanczos (sharpest) or nearest (blocky)",
               false, filterNames(), std::string("bilinear")},
              previewParam()},
             true},
            imageCommand(session, [](ImageDoc& doc, const Params& p) {
                double w = static_cast<double>(intParam(p, "width", 0)), h = static_cast<double>(intParam(p, "height", 0));
                double pct = numberParam(p, "percent", 0);
                const double ow = doc.image.width, oh = doc.image.height;
                if (pct > 0) {
                    w = ow * pct / 100;
                    h = oh * pct / 100;
                } else if (w <= 0 && h <= 0) {
                    return Result::failure("give width, height or percent");
                } else if (w <= 0) {
                    w = ow * h / oh;
                } else if (h <= 0) {
                    h = oh * w / ow;
                }
                int nw = std::max(1, static_cast<int>(std::lround(w))), nh = std::max(1, static_cast<int>(std::lround(h)));
                if (nw > 30000 || nh > 30000) return Result::failure("result would be larger than 30000 px");
                Filter f = parseFilter(stringParam(p, "filter", "bilinear")).value_or(Filter::Bilinear);
                transformAll(doc, [&](const Image& img) { return resize(img, nw, nh, f); });
                return docResult("resized to " + std::to_string(nw) + "x" + std::to_string(nh), doc, p);
            }));

    reg.add({"photo.crop",
             "Crop image",
             "Cut the image down to a rectangle (in pixels from the top-left).",
             {docParam(),
              {"x", ParamType::Int, "left edge", true},
              {"y", ParamType::Int, "top edge", true},
              {"width", ParamType::Int, "width of the area to keep", true},
              {"height", ParamType::Int, "height of the area to keep", true},
              previewParam()},
             true},
            imageCommand(session, [](ImageDoc& doc, const Params& p) {
                int x = static_cast<int>(intParam(p, "x")), y = static_cast<int>(intParam(p, "y"));
                int w = static_cast<int>(intParam(p, "width")), h = static_cast<int>(intParam(p, "height"));
                if (w < 1 || h < 1) return Result::failure("width and height must be at least 1");
                if (x >= doc.image.width || y >= doc.image.height || x + w <= 0 || y + h <= 0) {
                    return Result::failure("crop area is outside the image");
                }
                transformAll(doc, [&](const Image& img) { return crop(img, x, y, w, h); });
                return docResult("cropped to " + std::to_string(doc.image.width) + "x" + std::to_string(doc.image.height), doc, p);
            }));

    reg.add({"photo.canvas_size",
             "Canvas size",
             "Change the canvas size without scaling the pixels; new area is transparent.",
             {docParam(),
              {"width", ParamType::Int, "new canvas width", true},
              {"height", ParamType::Int, "new canvas height", true},
              {"anchor", ParamType::String, "where the existing image sits", false,
               {"center", "top-left", "top", "top-right", "left", "right", "bottom-left", "bottom", "bottom-right"},
               std::string("center")},
              previewParam()},
             true},
            imageCommand(session, [](ImageDoc& doc, const Params& p) {
                int w = static_cast<int>(intParam(p, "width")), h = static_cast<int>(intParam(p, "height"));
                if (w < 1 || h < 1 || w > 30000 || h > 30000) return Result::failure("width and height must be 1..30000");
                std::string a = stringParam(p, "anchor", "center");
                double ax = a.find("left") != std::string::npos ? 0 : a.find("right") != std::string::npos ? 1 : 0.5;
                double ay = a.find("top") != std::string::npos ? 0 : a.find("bottom") != std::string::npos ? 1 : 0.5;
                transformAll(doc, [&](const Image& img) { return resizeCanvas(img, w, h, ax, ay); });
                return docResult("canvas is now " + std::to_string(w) + "x" + std::to_string(h), doc, p);
            }));

    reg.add({"photo.rotate",
             "Rotate image",
             "Rotate the whole image clockwise by any angle (negative = counter-clockwise). 90, 180 and 270 turn it "
             "exactly. Other angles: fit=expand grows the canvas so nothing is cut off, keep keeps the size, crop "
             "(straighten) cuts away the empty corners; new corners get the background colour.",
             {docParam(), {"angle", ParamType::Number, "degrees clockwise, e.g. 90 or -2.5", true},
              {"fit", ParamType::String, "for angles that aren't quarter turns", false, {"expand", "keep", "crop"}, std::string("expand")},
              colorParam("background", "colour for the new corners", false, "transparent"),
              previewParam()},
             true},
            imageCommand(session, [](ImageDoc& doc, const Params& p) {
                const double angle = numberParam(p, "angle");
                const double turnsExact = angle / 90;
                if (std::fabs(turnsExact - std::round(turnsExact)) < 1e-9) {
                    int turns = static_cast<int>(std::lround(turnsExact));
                    transformAll(doc, [&](const Image& img) { return rotate90(img, turns); });
                } else {
                    Color bg;
                    std::string error;
                    if (!colorArg(p, "background", "transparent", bg, error)) return Result::failure(error);
                    rotateFree(doc, angle, stringParam(p, "fit", "expand"), bg);
                }
                char buf[48];
                std::snprintf(buf, sizeof buf, "%g", angle);
                return docResult(std::string("rotated ") + buf + " degrees", doc, p);
            }));

    reg.add({"photo.flip",
             "Flip image",
             "Mirror the whole image.",
             {docParam(),
              {"direction", ParamType::String, "which way to flip", true, {"horizontal", "vertical"}},
              previewParam()},
             true},
            imageCommand(session, [](ImageDoc& doc, const Params& p) {
                bool h = stringParam(p, "direction") == "horizontal";
                transformAll(doc, [&](const Image& img) { return h ? flipHorizontal(img) : flipVertical(img); });
                return docResult(std::string("flipped ") + (h ? "horizontally" : "vertically"), doc, p);
            }));

    reg.add({"photo.adjust",
             "Adjust colours",
             "Tonal or colour adjustment on a layer. Amounts: brightness, contrast, saturation, vibrance and temperature "
             "-100..100; hue in degrees; exposure in stops (e.g. 0.5); gamma 0.1..10 (1 = unchanged).",
             {docParam(),
              {"kind", ParamType::String, "what to adjust", true, adjustmentKinds()},
              {"amount", ParamType::Number, "how much (see description)", true},
              layerParam(), allLayersParam(), previewParam()},
             true},
            imageCommand(session, [](ImageDoc& doc, const Params& p) {
                auto kind = *parseAdjust(stringParam(p, "kind"));
                double amount = numberParam(p, "amount");
                Result r = onLayers(doc, p, [&](Image& img) { adjust(img, kind, amount); });
                if (!r.ok) return r;
                return docResult(stringParam(p, "kind") + " adjusted by " + toText(amount), doc, p);
            }));

    reg.add({"photo.levels",
             "Levels",
             "Remap tones: pixels at or below black become black, at or above white become white, gamma bends the "
             "midtones (>1 brighter). Use auto=true to stretch contrast automatically.",
             {docParam(),
              {"black", ParamType::Number, "input black point 0..255", false, {}, 0.0},
              {"white", ParamType::Number, "input white point 0..255", false, {}, 255.0},
              {"gamma", ParamType::Number, "midtone gamma", false, {}, 1.0},
              {"auto", ParamType::Bool, "auto contrast instead of manual levels", false, {}, false},
              layerParam(), allLayersParam(), previewParam()},
             true},
            imageCommand(session, [](ImageDoc& doc, const Params& p) {
                bool automatic = boolParam(p, "auto", false);
                double black = numberParam(p, "black", 0), white = numberParam(p, "white", 255), gamma = numberParam(p, "gamma", 1);
                Result r = onLayers(doc, p, [&](Image& img) {
                    if (automatic) autoContrast(img);
                    else levels(img, black, white, gamma);
                });
                if (!r.ok) return r;
                return docResult(automatic ? "auto contrast applied" : "levels applied", doc, p);
            }));

    reg.add({"photo.effect",
             "Apply effect",
             "Filter a layer. amount means: blur/sharpen radius in px, threshold level 0..255, posterize levels, "
             "pixelate block size, noise strength 0..100; ignored for grayscale, invert and sepia.",
             {docParam(),
              {"kind", ParamType::String, "which effect", true, effectKinds()},
              {"amount", ParamType::Number, "strength (see description)", false},
              layerParam(), allLayersParam(), previewParam()},
             true},
            imageCommand(session, [](ImageDoc& doc, const Params& p) {
                auto kind = *parseEffect(stringParam(p, "kind"));
                double amount = numberParam(p, "amount", 0);
                Result r = onLayers(doc, p, [&](Image& img) { applyEffect(img, kind, amount); });
                if (!r.ok) return r;
                return docResult(stringParam(p, "kind") + " applied", doc, p);
            }));

    reg.add({"photo.pick_color",
             "Pick colour",
             "Read the colour of one pixel, from the flattened image or one layer.",
             {docParam(),
              {"x", ParamType::Int, "x in px", true},
              {"y", ParamType::Int, "y in px", true},
              {"layer", ParamType::Int, "read this layer instead of the flattened image", false}},
             false, true},
            imageCommand(
                session,
                [](ImageDoc& doc, const Params& p) {
                    int x = static_cast<int>(intParam(p, "x")), y = static_cast<int>(intParam(p, "y"));
                    if (x < 0 || y < 0 || x >= doc.image.width || y >= doc.image.height) return Result::failure("point is outside the image");
                    Color c;
                    if (p.count("layer")) {
                        std::string error;
                        int idx = layerIndex(doc, p, error);
                        if (idx < 0) return Result::failure(error);
                        c = doc.image.layers[static_cast<std::size_t>(idx)].image.at(x, y);
                    } else {
                        c = doc.flatten().at(x, y);
                    }
                    return Result::success(c.hex()).with("color", c.hex()).with("r", static_cast<std::int64_t>(c.r))
                        .with("g", static_cast<std::int64_t>(c.g)).with("b", static_cast<std::int64_t>(c.b))
                        .with("a", static_cast<std::int64_t>(c.a));
                },
                false));

    reg.add({"photo.benchmark",
             "Speed test",
             "Time the main picture operations (blur, resize, layer blending, colour adjust, mipmaps) on a generated test "
             "image and report milliseconds for each (best of 3). compare=true also times the scalar loops. Nothing is opened or saved.",
             {{"size", ParamType::Int, "test image width and height in px", false, {}, std::int64_t{2048}},
              {"threads", ParamType::Int, "threads to use (0 = all CPU cores)", false, {}, std::int64_t{0}},
              {"simd", ParamType::Bool, "use the SSE2/NEON loops (false = the plain scalar loops)", false, {}, true},
              {"compare", ParamType::Bool, "also time each operation on the scalar loops and show the speed-up", false, {}, false}},
             false},
            [](const Params& p) {
                return std::make_unique<FunctionAction>([p] { return runBenchmark(p); });
            });

    reg.add({"photo.batch",
             "Batch process folder",
             "Process every image in a folder in one go: optional resize to fit, adjustments and effect, then save in "
             "a chosen format to an output folder. Originals are never replaced; existing results are skipped unless overwrite=true.",
             {{"input_dir", ParamType::String, "folder with the source images", true},
              {"output_dir", ParamType::String, "folder for results (created if missing)", true},
              {"format", ParamType::String, "output format", false, {"png", "jpg", "bmp", "tga", "ppm", "ora"}, std::string("jpg")},
              {"max_size", ParamType::Int, "fit within this many px on the longest side (0 = keep size)", false, {}, std::int64_t{0}},
              {"quality", ParamType::Int, "JPEG quality 1..100", false, {}, std::int64_t{90}},
              {"auto_contrast", ParamType::Bool, "apply auto contrast", false, {}, false},
              {"effect", ParamType::String, "optional effect", false, effectKinds()},
              {"effect_amount", ParamType::Number, "effect strength", false},
              {"recursive", ParamType::Bool, "include subfolders", false, {}, false},
              {"overwrite", ParamType::Bool, "replace results that already exist (sources are never replaced)", false, {}, false}},
             false},
            [](const Params& p) {
                return std::make_unique<FunctionAction>([p] {
                    fs::path in = stringParam(p, "input_dir"), out = stringParam(p, "output_dir");
                    std::error_code ec;
                    if (!fs::is_directory(in, ec)) return Result::failure("not a folder: " + in.string());
                    fs::create_directories(out, ec);
                    if (ec) return Result::failure("can't create " + out.string() + ": " + ec.message());
                    const std::string format = stringParam(p, "format", "jpg");
                    const int maxSize = static_cast<int>(intParam(p, "max_size", 0));
                    const bool overwrite = boolParam(p, "overwrite", false);
                    const fs::path outAbs = fs::weakly_canonical(out, ec);
                    std::vector<fs::path> files;
                    auto consider = [&](const fs::directory_entry& e) {
                        if (!e.is_regular_file() || !canOpenImage(e.path())) return;
                        // results written into a subfolder of the input are not sources
                        auto rel = fs::weakly_canonical(e.path(), ec).lexically_relative(outAbs);
                        if (outAbs != fs::weakly_canonical(in, ec) && !rel.empty() && *rel.begin() != "..") return;
                        files.push_back(e.path());
                    };
                    if (boolParam(p, "recursive", false)) {
                        for (const auto& e : fs::recursive_directory_iterator(in, ec)) consider(e);
                    } else {
                        for (const auto& e : fs::directory_iterator(in, ec)) consider(e);
                    }
                    std::sort(files.begin(), files.end());
                    int done = 0, skipped = 0;
                    std::string failures;
                    for (const auto& f : files) {
                        fs::path target = out / fs::relative(f, in, ec);
                        target.replace_extension(format);
                        if (fs::exists(target, ec) && fs::equivalent(target, f, ec)) {
                            failures += "\n  " + f.string() + ": would replace the original; pick another output_dir or format";
                            continue;
                        }
                        if (!overwrite && fs::exists(target, ec)) {
                            ++skipped;
                            continue;
                        }
                        try {
                            auto doc = openImage(f);
                            if (maxSize > 0 && std::max(doc->image.width, doc->image.height) > maxSize) {
                                double scale = static_cast<double>(maxSize) / std::max(doc->image.width, doc->image.height);
                                int nw = std::max(1, static_cast<int>(std::lround(doc->image.width * scale)));
                                int nh = std::max(1, static_cast<int>(std::lround(doc->image.height * scale)));
                                transformAll(*doc, [&](const Image& img) { return resize(img, nw, nh); });
                            }
                            for (auto& layer : doc->image.layers) {
                                if (boolParam(p, "auto_contrast", false)) autoContrast(layer.image);
                                if (p.count("effect")) {
                                    applyEffect(layer.image, *parseEffect(stringParam(p, "effect")), numberParam(p, "effect_amount", 0));
                                }
                            }
                            fs::create_directories(target.parent_path(), ec);
                            saveImage(*doc, target, static_cast<int>(intParam(p, "quality", 90)));
                            ++done;
                        } catch (const std::exception& e) {
                            failures += "\n  " + f.string() + ": " + e.what();
                        }
                    }
                    if (files.empty()) return Result::failure("no images found in " + in.string());
                    std::string msg = "processed " + std::to_string(done) + " of " + std::to_string(files.size()) +
                                      " images into " + out.string() +
                                      (skipped ? " (" + std::to_string(skipped) + " already there; overwrite=true to redo)" : "") + failures;
                    Result r = done > 0 || (skipped > 0 && failures.empty()) ? Result::success(msg) : Result::failure(msg);
                    return r.with("processed", static_cast<std::int64_t>(done))
                        .with("skipped", static_cast<std::int64_t>(skipped))
                        .with("failed", static_cast<std::int64_t>(files.size() - static_cast<std::size_t>(done + skipped)));
                });
            });

    registerHealCommands(session);
    registerSelectCommands(session);
    registerAdjustmentCommands(session);
    registerFilterCommands(session);
    registerDistortCommands(session);
    registerSubjectCommands(session);
}

}  // namespace wetwaresuite::photo
