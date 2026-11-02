// Shared pieces of the cut.* commands: clip properties as params (with their
// checks), the undoable-edit wrapper, adding media with linked sound, and the
// split / ripple / range edits the editing commands are built from.
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <map>

#include "internal.hpp"
#include "media.hpp"
#include "render_internal.hpp"
#include "wetwaresuite/audio/io.hpp"
#include "wetwaresuite/raster/codec.hpp"

namespace wetwaresuite::cut {

namespace fs = std::filesystem;

std::string num(double v) {
    if (std::abs(v) < 5e-7) return "0";
    char buf[48];
    std::snprintf(buf, sizeof buf, "%.3f", v);
    std::string s = buf;
    while (!s.empty() && s.back() == '0') s.pop_back();
    if (!s.empty() && s.back() == '.') s.pop_back();
    return s;
}

std::string lower(std::string s) {
    for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

std::string extOf(const fs::path& p) {
    std::string e = p.extension().string();
    if (!e.empty() && e[0] == '.') e.erase(0, 1);
    return lower(e);
}

bool has(const Params& p, const std::string& key) {
    auto it = p.find(key);
    return it != p.end() && !std::holds_alternative<std::monostate>(it->second);
}

ParamSpec docParam() { return {"doc", ParamType::String, "id of an open timeline, e.g. cut1", true}; }
ParamSpec clipParam(const std::string& what) { return {"clip", ParamType::String, what, true}; }
ParamSpec previewParam() {
    return {"preview", ParamType::Bool, "attach a PNG of the frame in the middle of the clip", false, {}, false};
}
ParamSpec overwriteParam() { return {"overwrite", ParamType::Bool, "replace the output if it exists", false, {}, false}; }
ParamSpec linkedParam() {
    return {"linked", ParamType::Bool,
            "also change the clips linked to it (a video and its sound); false changes this clip alone, for J and L cuts", false, {}, true};
}

namespace {

const std::vector<std::string> kFits = {"fit", "fill", "stretch"};
const std::vector<std::string> kPositions = {"center", "top", "bottom", "lower-third", "top-left", "top-right",
                                             "bottom-left", "bottom-right", "custom"};
const std::vector<std::string> kAligns = {"left", "center", "right"};
const std::vector<std::string> kStyles = {"plain", "lower_third"};

const std::vector<ParamSpec>& allSpecs() {
    static const std::vector<ParamSpec> all = {
        {"start", ParamType::Number, "where the clip begins on the timeline, in seconds", false},
        {"in", ParamType::Number, "source time the clip starts from, in seconds", false},
        {"out", ParamType::Number, "source time the clip stops at, in seconds", false},
        {"duration", ParamType::Number, "length on the timeline in seconds (sets out = in + duration x speed)", false},
        {"speed", ParamType::Number, "playback speed: 2 = twice as fast, 0.5 = slow motion (0.05..20); sound keeps its pitch", false},
        {"reverse", ParamType::Bool, "play the source range backwards", false},
        {"volume", ParamType::Number, "sound level: 1 = unchanged, 0.3 = quiet, 2 = louder (keyframable)", false},
        {"mute", ParamType::Bool, "silence this clip's sound", false},
        {"pan", ParamType::Number, "-1 (left) .. 1 (right), 0 = centre (keyframable)", false},
        {"denoise", ParamType::Number, "voice cleanup: take this many dB of steady noise out (hiss, hum, fans, room tone); 0 = off, 12 is a good start (0..40)", false},
        {"fade_in", ParamType::Number, "fade in over this many seconds (picture from transparent, sound from silence)", false},
        {"fade_out", ParamType::Number, "fade out over this many seconds", false},
        {"opacity", ParamType::Number, "0 (invisible) .. 1 (solid) (keyframable)", false},
        {"blend", ParamType::String, "how the picture mixes with what's below it", false, blendModeNames()},
        {"x", ParamType::Number, "left edge of the picture box in px (keyframable; titles with position=custom)", false},
        {"y", ParamType::Number, "top edge of the picture box in px (keyframable)", false},
        {"width", ParamType::Number, "picture box width in px; 0 = the whole frame (set it for picture-in-picture)", false},
        {"height", ParamType::Number, "picture box height in px; 0 = from width and the source's aspect", false},
        {"fit", ParamType::String, "how the picture fills its box", false, kFits},
        {"crop", ParamType::NumberList, "crop the source first: x,y,width,height in source px (0,0,0,0 = no crop)", false},
        {"scale", ParamType::Number, "size around the box centre: 1 = 100%, 0.5 = half (keyframable)", false},
        {"rotation", ParamType::Number, "turn around the box centre, degrees clockwise (keyframable)", false},
        {"crop_left", ParamType::Number, "cut this % off the picture's left edge (keyframable)", false},
        {"crop_right", ParamType::Number, "cut this % off the right edge (keyframable)", false},
        {"crop_top", ParamType::Number, "cut this % off the top edge (keyframable)", false},
        {"crop_bottom", ParamType::Number, "cut this % off the bottom edge (keyframable)", false},
        {"flip_h", ParamType::Bool, "mirror left to right", false},
        {"flip_v", ParamType::Bool, "turn upside down", false},
        {"exposure", ParamType::Number, "-5 .. 5 stops, 0 = unchanged (keyframable)", false},
        {"temperature", ParamType::Number, "white balance -100 (cooler, blue) .. 100 (warmer, orange) (keyframable)", false},
        {"tint", ParamType::Number, "-100 (green) .. 100 (magenta) (keyframable)", false},
        {"wheel_lift", ParamType::NumberList, "lift wheel (shadows): r,g,b[,master] each -1..1, 0 = none; one number moves all", false},
        {"wheel_gamma", ParamType::NumberList, "gamma wheel (midtones): r,g,b[,master] each -1..1 (positive brightens)", false},
        {"wheel_gain", ParamType::NumberList, "gain wheel (highlights): r,g,b[,master] each -1..3 (0.2 = 20% brighter)", false},
        {"brightness", ParamType::Number, "-1 .. 1, 0 = unchanged (keyframable)", false},
        {"contrast", ParamType::Number, "0 .. 3, 1 = unchanged (keyframable)", false},
        {"saturation", ParamType::Number, "0 (grey) .. 3, 1 = unchanged (keyframable)", false},
        {"gamma", ParamType::Number, "0.1 .. 10, 1 = unchanged (keyframable)", false},
        {"grayscale", ParamType::Bool, "black and white", false},
        {"curve", ParamType::NumberList, "tone curve: x,y pairs in 0..1, e.g. 0,0,0.25,0.18,0.75,0.85,1,1 (empty to remove)", false},
        {"curve_r", ParamType::NumberList, "red channel curve: x,y pairs in 0..1", false},
        {"curve_g", ParamType::NumberList, "green channel curve: x,y pairs in 0..1", false},
        {"curve_b", ParamType::NumberList, "blue channel curve: x,y pairs in 0..1", false},
        {"lut", ParamType::String, "colour grade with a .cube 3D LUT file (empty to remove)", false},
        {"blur", ParamType::Number, "gaussian blur radius in px, 0 = none (keyframable)", false},
        {"sharpen", ParamType::Number, "0 .. 5, 0 = none (keyframable)", false},
        {"vignette", ParamType::Number, "darken the corners: 0 .. 1 (keyframable)", false},
        {"key_color", ParamType::String, "chroma key: make this colour transparent, e.g. #00ff00 for a green screen (empty = off)", false},
        {"key_similarity", ParamType::Number, "chroma key: how close a colour must be to vanish, 0..1 (0.4)", false},
        {"key_softness", ParamType::Number, "chroma key: how soft the edge is, 0..1 (0.1)", false},
        {"key_spill", ParamType::Number, "chroma key: remove the key colour's glow from what's left, 0..1 (0.5)", false},
        {"text", ParamType::String, "title text; \\n starts a new line", false},
        {"font", ParamType::String, "title font: a .ttf/.otf file or a family name (sans, serif, mono, Arial, ...)", false},
        {"font_size", ParamType::Number, "title size in px", false},
        {"color", ParamType::String, "title (or matte) colour: a name (white, yellow), #rrggbb, or with alpha like white@0.8", false},
        {"position", ParamType::String, "where the title sits (custom uses x and y)", false, kPositions},
        {"box", ParamType::Bool, "draw a box behind the title", false},
        {"box_color", ParamType::String, "colour of the title box, e.g. black@0.5", false},
        {"bold", ParamType::Bool, "bold face (system fonts)", false},
        {"italic", ParamType::Bool, "italic face (system fonts)", false},
        {"align", ParamType::String, "line alignment inside the title", false, kAligns},
        {"outline", ParamType::Number, "outline (stroke) width around the letters in px, 0 = none", false},
        {"outline_color", ParamType::String, "outline colour", false},
        {"shadow", ParamType::Number, "drop shadow distance in px, 0 = none", false},
        {"shadow_color", ParamType::String, "shadow colour, e.g. black@0.6", false},
        {"anim_in", ParamType::String, "how the title comes in", false, titleAnimations()},
        {"anim_out", ParamType::String, "how the title goes out", false, titleAnimations()},
        {"anim_duration", ParamType::Number, "seconds the in/out animations take", false},
        {"style", ParamType::String, "plain text, or a lower third (accent bar, box and a subtitle line)", false, kStyles},
        {"subtitle", ParamType::String, "lower thirds: the smaller second line (a job title, a place)", false},
        {"accent_color", ParamType::String, "lower thirds: the colour of the bar", false},
        {"caption", ParamType::Bool, "count this title as a caption (cut.export_captions writes them)", false},
    };
    return all;
}

void setWheel(std::vector<double>& w, const NumberList& l) {
    if (l.size() == 1) w = {0, 0, 0, l[0]};
    else {
        w = {0, 0, 0, 0};
        for (std::size_t i = 0; i < std::min<std::size_t>(4, l.size()); ++i) w[i] = l[i];
    }
}

bool colourOk(const std::string& s) { return parseColour(s).has_value(); }

}  // namespace

const std::vector<std::string> kPictureProps = {"opacity", "blend", "x", "y", "width", "height", "fit", "crop", "scale", "rotation",
                                                "crop_left", "crop_right", "crop_top", "crop_bottom", "flip_h", "flip_v", "fade_in", "fade_out"};
const std::vector<std::string> kColourProps = {"exposure", "temperature", "tint", "wheel_lift", "wheel_gamma", "wheel_gain", "brightness", "contrast",
                                               "saturation", "gamma", "grayscale", "curve", "curve_r", "curve_g", "curve_b", "lut"};
const std::vector<std::string> kEffectProps = {"blur", "sharpen", "vignette", "key_color", "key_similarity", "key_softness", "key_spill"};
const std::vector<std::string> kTitleProps = {"text", "font", "font_size", "color", "position", "box", "box_color", "bold", "italic", "align",
                                              "outline", "outline_color", "shadow", "shadow_color", "anim_in", "anim_out", "anim_duration",
                                              "style", "subtitle", "accent_color", "caption"};

std::vector<ParamSpec> propSpecs(const std::vector<std::string>& names) {
    std::vector<ParamSpec> out;
    for (const auto& n : names) {
        for (const auto& s : allSpecs()) {
            if (s.name == n) out.push_back(s);
        }
    }
    return out;
}

const std::vector<std::string>& allPropNames() {
    static const std::vector<std::string> names = [] {
        std::vector<std::string> v;
        for (const auto& s : allSpecs()) v.push_back(s.name);
        return v;
    }();
    return names;
}

std::string applyProps(Clip& c, const Params& p) {
    auto n = [&](const char* key, double& field) {
        if (has(p, key)) field = numberParam(p, key);
    };
    auto b = [&](const char* key, bool& field) {
        if (has(p, key)) field = boolParam(p, key);
    };
    auto s = [&](const char* key, std::string& field) {
        if (has(p, key)) field = stringParam(p, key);
    };
    auto l = [&](const char* key, std::vector<double>& field) {
        if (has(p, key)) {
            NumberList v = listParam(p, key);
            field.assign(v.begin(), v.end());
        }
    };
    const bool timed = c.timed();
    if (has(p, "speed") || has(p, "reverse")) {
        if (!timed) return "only video, audio and nested clips have a speed";
        if (c.hold) return "a freeze frame has no speed";
        if (has(p, "speed")) {
            // the clip keeps its source range, so its length changes
            c.speed = numberParam(p, "speed");
        }
        b("reverse", c.reverse);
    }
    n("start", c.start);
    if (has(p, "in")) {
        if (!timed) return "stills, mattes and titles have no in point; set duration instead";
        c.in = numberParam(p, "in");
    }
    if (has(p, "out")) {
        if (!timed) return "stills, mattes and titles have no out point; set duration instead";
        c.out = numberParam(p, "out");
    }
    if (has(p, "duration")) {
        const double d = numberParam(p, "duration");
        if (d <= 0) return "duration must be more than 0";
        if (c.reverse && timed) c.in = c.out - d * c.speed;
        else c.out = c.in + d * (timed ? c.speed : 1);
    }
    n("volume", c.volume);
    b("mute", c.mute);
    n("pan", c.pan);
    n("denoise", c.denoise);
    n("fade_in", c.fadeIn);
    n("fade_out", c.fadeOut);
    n("opacity", c.opacity);
    s("blend", c.blend);
    n("x", c.x);
    n("y", c.y);
    n("width", c.width);
    n("height", c.height);
    s("fit", c.fit);
    if (has(p, "crop")) {
        NumberList v = listParam(p, "crop");
        if (v.size() != 4) return "crop takes four numbers: x,y,width,height";
        c.cropX = v[0];
        c.cropY = v[1];
        c.cropW = v[2];
        c.cropH = v[3];
    }
    n("scale", c.scale);
    n("rotation", c.rotation);
    n("crop_left", c.cropLeft);
    n("crop_right", c.cropRight);
    n("crop_top", c.cropTop);
    n("crop_bottom", c.cropBottom);
    b("flip_h", c.flipH);
    b("flip_v", c.flipV);
    n("exposure", c.exposure);
    n("temperature", c.temperature);
    n("tint", c.tint);
    if (has(p, "wheel_lift")) setWheel(c.wheelLift, listParam(p, "wheel_lift"));
    if (has(p, "wheel_gamma")) setWheel(c.wheelGamma, listParam(p, "wheel_gamma"));
    if (has(p, "wheel_gain")) setWheel(c.wheelGain, listParam(p, "wheel_gain"));
    n("brightness", c.brightness);
    n("contrast", c.contrast);
    n("saturation", c.saturation);
    n("gamma", c.gamma);
    b("grayscale", c.grayscale);
    l("curve", c.curve);
    l("curve_r", c.curveR);
    l("curve_g", c.curveG);
    l("curve_b", c.curveB);
    s("lut", c.lut);
    n("blur", c.blur);
    n("sharpen", c.sharpen);
    n("vignette", c.vignette);
    s("key_color", c.keyColor);
    n("key_similarity", c.keySimilarity);
    n("key_softness", c.keySoftness);
    n("key_spill", c.keySpill);
    if (c.kind == "title") {
        s("text", c.text);
        s("font", c.font);
        n("font_size", c.fontSize);
        s("color", c.color);
        s("position", c.position);
        b("box", c.box);
        s("box_color", c.boxColor);
        b("bold", c.bold);
        b("italic", c.italic);
        s("align", c.align);
        n("outline", c.outline);
        s("outline_color", c.outlineColor);
        n("shadow", c.shadow);
        s("shadow_color", c.shadowColor);
        s("anim_in", c.animIn);
        s("anim_out", c.animOut);
        n("anim_duration", c.animDuration);
        s("style", c.style);
        s("subtitle", c.subtitle);
        s("accent_color", c.accentColor);
        b("caption", c.caption);
        if ((has(p, "x") || has(p, "y")) && !has(p, "position")) c.position = "custom";
    } else {
        for (const auto& key : kTitleProps) {
            if (key == "color" && c.kind == "color") continue;
            if (has(p, key)) return key + " only applies to titles";
        }
        if (c.kind == "color") s("color", c.color);
    }
    if (c.kind == "audio") {
        for (const auto* group : {&kPictureProps, &kColourProps, &kEffectProps}) {
            for (const auto& key : *group) {
                if (key == "fade_in" || key == "fade_out") continue;
                if (has(p, key)) return key + " only applies to pictures, not audio clips";
            }
        }
    }
    if ((c.kind == "title" || c.kind == "color" || c.kind == "image") && (has(p, "volume") || has(p, "mute") || has(p, "pan") || has(p, "denoise"))) {
        return "a " + c.kind + " has no sound";
    }
    return {};
}

std::string validate(const Clip& c) {
    if (!(c.speed >= 0.05 && c.speed <= 20)) return "speed must be between 0.05 and 20";
    if (c.start < -kEps) return "start can't be before 0";
    if (c.in < -kEps) return "in can't be before 0";
    if (c.out <= c.in + 1e-3) return "the clip must be longer than that (out must be after in)";
    if (c.sourceDuration > 0 && (c.kind == "video" || c.kind == "audio") && !c.hold && c.out > c.sourceDuration + 0.05) {
        return "out (" + num(c.out) + "s) is past the end of the source, which is " + num(c.sourceDuration) + "s long";
    }
    if (c.kind == "nest" && c.nested && c.out > c.nested->duration() + 0.05) return "out is past the end of the nested sequence";
    if (c.volume < 0 || c.volume > 10) return "volume must be between 0 and 10";
    if (c.pan < -1 || c.pan > 1) return "pan must be -1..1";
    if (c.denoise < 0 || c.denoise > 40) return "denoise must be 0..40 dB";
    if (c.opacity < 0 || c.opacity > 1) return "opacity must be between 0 and 1";
    if (c.fadeIn < 0 || c.fadeOut < 0) return "fades can't be negative";
    if (c.fadeIn + c.fadeOut > c.length() + 1e-3) return "fade_in + fade_out is longer than the clip (" + num(c.length()) + "s)";
    if (c.width < 0 || c.height < 0 || c.width > 16384 || c.height > 16384) return "width and height must be 0..16384";
    if (c.cropW < 0 || c.cropH < 0 || c.cropX < 0 || c.cropY < 0) return "crop values can't be negative";
    if (c.cropW > 0 && c.sourceWidth > 0 && (c.cropX + c.cropW > c.sourceWidth + 0.5 || c.cropY + c.cropH > c.sourceHeight + 0.5)) {
        return "the crop goes outside the " + std::to_string(c.sourceWidth) + "x" + std::to_string(c.sourceHeight) + " source";
    }
    if ((c.cropW > 0) != (c.cropH > 0)) return "crop needs both a width and a height";
    if (c.scale < 0 || c.scale > 20) return "scale must be 0..20 (1 = 100%)";
    if (std::abs(c.rotation) > 36000) return "rotation must be within -36000..36000 degrees";
    for (double v : {c.cropLeft, c.cropRight, c.cropTop, c.cropBottom}) {
        if (v < 0 || v > 100) return "edge crops are 0..100 %";
    }
    if (c.cropLeft + c.cropRight >= 100 || c.cropTop + c.cropBottom >= 100) return "the edge crops leave nothing of the picture";
    if (c.exposure < -5 || c.exposure > 5) return "exposure must be -5..5 stops";
    if (std::abs(c.temperature) > 100 || std::abs(c.tint) > 100) return "temperature and tint must be -100..100";
    for (const auto* w : {&c.wheelLift, &c.wheelGamma}) {
        for (double v : *w) {
            if (v < -1 || v > 1) return "lift and gamma wheel values must be -1..1";
        }
    }
    for (double v : c.wheelGain) {
        if (v < -1 || v > 3) return "gain wheel values must be -1..3";
    }
    if (c.brightness < -1 || c.brightness > 1) return "brightness must be -1..1";
    if (c.contrast < 0 || c.contrast > 3) return "contrast must be 0..3";
    if (c.saturation < 0 || c.saturation > 3) return "saturation must be 0..3";
    if (c.gamma < 0.1 || c.gamma > 10) return "gamma must be 0.1..10";
    for (const auto* cv : {&c.curve, &c.curveR, &c.curveG, &c.curveB}) {
        if (cv->empty()) continue;
        if (cv->size() % 2 || cv->size() < 4) return "a curve takes x,y pairs (at least two points)";
        for (double v : *cv) {
            if (v < 0 || v > 1) return "curve points are 0..1";
        }
    }
    std::error_code ec;
    if (!c.lut.empty() && !fs::is_regular_file(c.lut, ec)) return "LUT file not found: " + c.lut;
    if (!c.lut.empty() && extOf(c.lut) != "cube") return "LUTs are .cube files";
    if (c.blur < 0 || c.blur > 500) return "blur must be 0..500 px";
    if (c.sharpen < 0 || c.sharpen > 5) return "sharpen must be 0..5";
    if (c.vignette < 0 || c.vignette > 1) return "vignette must be 0..1";
    if (!c.keyColor.empty() && !colourOk(c.keyColor)) return "key_color '" + c.keyColor + "' isn't a colour";
    for (double v : {c.keySimilarity, c.keySoftness, c.keySpill}) {
        if (v < 0 || v > 1) return "key_similarity, key_softness and key_spill are 0..1";
    }
    if (!raster::parseBlend(c.blend.empty() ? "normal" : c.blend)) return "unknown blend mode '" + c.blend + "'";
    if (c.fit != "fit" && c.fit != "fill" && c.fit != "stretch") return "fit is fit, fill or stretch";
    if (c.kind == "title") {
        if (c.text.empty()) return "a title needs text";
        if (c.fontSize < 4 || c.fontSize > 1000) return "font_size must be 4..1000";
        for (const auto* col : {&c.color, &c.boxColor, &c.outlineColor, &c.shadowColor, &c.accentColor}) {
            if (!colourOk(*col)) return "'" + *col + "' isn't a colour";
        }
        if (c.outline < 0 || c.outline > 100) return "outline must be 0..100 px";
        if (c.shadow < 0 || c.shadow > 200) return "shadow must be 0..200 px";
        auto known = [](const std::string& a) {
            return std::find(titleAnimations().begin(), titleAnimations().end(), a) != titleAnimations().end();
        };
        if (!known(c.animIn) || !known(c.animOut)) return "unknown title animation (use none, fade, slide_up, slide_down, slide_left, slide_right, zoom, typewriter or wipe)";
        if (c.animDuration < 0.05 || c.animDuration > 10) return "anim_duration must be 0.05..10 s";
        if (c.align != "left" && c.align != "center" && c.align != "right") return "align is left, center or right";
        if (c.style != "plain" && c.style != "lower_third") return "style is plain or lower_third";
        if (std::find(kPositions.begin(), kPositions.end(), c.position) == kPositions.end()) return "unknown position '" + c.position + "'";
    }
    if (c.kind == "color" && !colourOk(c.color)) return "'" + c.color + "' isn't a colour";
    for (const auto& [name, ks] : c.keys) {
        for (const auto& k : ks) {
            if (k.t < -kEps) return "keyframes can't be before the clip starts";
        }
    }
    return {};
}

std::string describe(const Timeline& tl, const Track& track, const Clip& c) {
    const Clip d;
    std::string s = c.id + " " + c.kind;
    if (c.kind == "title") s += " \"" + (c.text.size() > 40 ? c.text.substr(0, 37) + "..." : c.text) + "\"";
    else if (c.kind == "color") s += " " + c.color;
    else if (c.kind == "nest") s += " (" + std::to_string(c.nested ? c.nested->tracks.size() : 0) + " tracks inside)";
    else s += " " + fs::path(c.source).filename().string();
    s += " at " + num(c.start) + "-" + num(c.end()) + "s (" + num(c.length()) + "s)";
    if (c.timed()) s += " source " + num(c.in) + "-" + num(c.out);
    auto add = [&](const std::string& key, double v, double def) {
        if (std::abs(v - def) > kEps) s += " " + key + "=" + num(v);
    };
    add("speed", c.speed, 1);
    if (c.reverse) s += " reversed";
    if (c.hold) s += " freeze-frame";
    if (!c.link.empty()) s += " linked(" + c.link + ")";
    add("volume", c.volume, 1);
    if (c.mute) s += " muted";
    add("pan", c.pan, 0);
    add("denoise", c.denoise, 0);
    add("fade_in", c.fadeIn, 0);
    add("fade_out", c.fadeOut, 0);
    add("opacity", c.opacity, 1);
    if (c.blend != "normal") s += " blend=" + c.blend;
    if (c.kind != "title" && (c.width > 0 || c.height > 0 || c.x != 0 || c.y != 0)) {
        s += " box=" + num(c.x) + "," + num(c.y) + " " + num(c.width) + "x" + num(c.height);
    }
    if (c.fit != d.fit) s += " fit=" + c.fit;
    if (c.cropW > 0) s += " crop=" + num(c.cropX) + "," + num(c.cropY) + "," + num(c.cropW) + "," + num(c.cropH);
    add("scale", c.scale, 1);
    add("rotation", c.rotation, 0);
    add("crop_left", c.cropLeft, 0);
    add("crop_right", c.cropRight, 0);
    add("crop_top", c.cropTop, 0);
    add("crop_bottom", c.cropBottom, 0);
    if (c.flipH) s += " flip_h";
    if (c.flipV) s += " flip_v";
    add("exposure", c.exposure, 0);
    add("temperature", c.temperature, 0);
    add("tint", c.tint, 0);
    auto wheel = [&](const char* key, const std::vector<double>& w) {
        if (std::any_of(w.begin(), w.end(), [](double v) { return std::abs(v) > kEps; })) {
            s += std::string(" ") + key + "=" + num(w[0]) + "," + num(w[1]) + "," + num(w[2]) + "," + num(w[3]);
        }
    };
    wheel("wheel_lift", c.wheelLift);
    wheel("wheel_gamma", c.wheelGamma);
    wheel("wheel_gain", c.wheelGain);
    add("brightness", c.brightness, 0);
    add("contrast", c.contrast, 1);
    add("saturation", c.saturation, 1);
    add("gamma", c.gamma, 1);
    if (c.grayscale) s += " grayscale";
    if (!c.curve.empty() || !c.curveR.empty() || !c.curveG.empty() || !c.curveB.empty()) s += " curves";
    if (!c.lut.empty()) s += " lut=" + fs::path(c.lut).filename().string();
    add("blur", c.blur, 0);
    add("sharpen", c.sharpen, 0);
    add("vignette", c.vignette, 0);
    if (!c.keyColor.empty()) s += " key=" + c.keyColor;
    if (c.kind == "title") {
        s += " size=" + num(c.fontSize) + " color=" + c.color + " position=" + c.position;
        if (c.position == "custom") s += " (" + num(c.x) + "," + num(c.y) + ")";
        if (c.box) s += " box=" + c.boxColor;
        if (c.outline > 0) s += " outline=" + num(c.outline);
        if (c.shadow > 0) s += " shadow=" + num(c.shadow);
        if (c.animIn != "none") s += " in=" + c.animIn;
        if (c.animOut != "none") s += " out=" + c.animOut;
        if (c.style != "plain") s += " " + c.style;
        if (c.caption) s += " caption";
    }
    if (!c.keys.empty()) {
        s += " keyframes:";
        for (const auto& [name, ks] : c.keys) s += " " + name + "(" + std::to_string(ks.size()) + ")";
    }
    if (!c.transition.empty()) {
        const Clip* prev = c.media() ? tl.previousMedia(track, c) : tl.previousClip(track, c);
        const double overlap = prev ? prev->end() - c.start : 0;
        if (prev && overlap > 1e-3 && overlap <= std::min(prev->length(), c.length()) + kEps) {
            s += " transition=" + c.transition + " " + num(overlap) + "s from " + prev->id;
        } else {
            s += " transition=" + c.transition + " (inactive: doesn't overlap the clip before)";
        }
    }
    if (c.kind == "video" && !c.hasAudio) s += " (no sound)";
    else if (c.kind == "video" && !c.ownSound && c.link.empty()) s += " (sound removed)";
    return s;
}

std::string framePng(const Timeline& tl, double at, int width) {
    FrameRequest rq;
    rq.t = at;
    rq.width = width;
    auto& ctx = previewContext();
    std::lock_guard<std::mutex> g(ctx.impl().lock);
    raster::Image img = renderFrame(tl, rq, &ctx);
    return raster::encodePng(img);
}

Result& maybePreview(Result& r, const Timeline& tl, const Params& p, double at) {
    if (!r.ok || !boolParam(p, "preview", false)) return r;
    try {
        r.attach({"image/png", framePng(tl, at, std::min(tl.settings.width, 640)), "preview.png"});
    } catch (const std::exception& e) {
        r.message += " (no preview: " + std::string(e.what()) + ")";
    }
    return r;
}

ActionFactory cutCommand(Session& session, Edit edit, bool undoable) {
    return [&session, edit, undoable](const Params& p) -> std::unique_ptr<Action> {
        const std::string id = stringParam(p, "doc");
        auto doc = session.workspace().getAs<Timeline>(id);
        if (!doc) {
            return std::make_unique<FunctionAction>(
                [id] { return Result::failure("no open timeline called '" + id + "' (use cut.new or cut.open first)"); });
        }
        if (!undoable) {
            return std::make_unique<FunctionAction>([doc, edit, p] {
                try {
                    return edit(*doc, p);
                } catch (const std::exception& e) {
                    return Result::failure(e.what());
                }
            });
        }
        return std::make_unique<TimelineEdit>(doc, [edit, p](Timeline& d) { return edit(d, p); });
    };
}

Result clipResult(const std::string& message, Timeline& tl, const Params& p, const std::string& clipId) {
    tl.sortClips();
    const Clip* c = tl.findClip(clipId);
    Result r = Result::success(message + " (timeline " + num(tl.duration()) + "s)", clipId);
    r.with("clip", clipId);
    if (c) r.with("start", c->start).with("end", c->end()).with("length", c->length());
    r.with("duration", tl.duration());
    return maybePreview(r, tl, p, c ? c->start + c->length() / 2 : 0);
}

Clip* requireClip(Timeline& tl, const Params& p, Track** owner, const std::string& key) {
    const std::string id = stringParam(p, key);
    Clip* c = tl.findClip(id, owner);
    if (!c) throw std::runtime_error("no clip called '" + id + "' (cut.info lists the clips)");
    return c;
}

Track* trackOf(Timeline& tl, const std::string& clipId) {
    Track* t = nullptr;
    tl.findClip(clipId, &t);
    return t;
}

void fillFromProbe(Clip& c, const MediaInfo& m) {
    c.sourceDuration = m.duration;
    c.sourceWidth = m.width;
    c.sourceHeight = m.height;
    c.sourceFps = m.fps;
    c.hasAudio = m.hasAudio;
    c.videoCodec = m.videoCodec;
    c.audioCodec = m.audioCodec;
}

std::string absolute(const std::string& path) {
    std::error_code ec;
    fs::path a = fs::absolute(path, ec);
    return ec ? path : a.lexically_normal().string();
}

std::string requireInput(const Params& p, const char* key) {
    std::string in = stringParam(p, key);
    std::error_code ec;
    if (!fs::is_regular_file(in, ec)) throw std::runtime_error("file not found: " + in);
    return in;
}

std::string checkOutput(const fs::path& output, const Params& p) {
    std::error_code ec;
    if (fs::exists(output, ec) && !boolParam(p, "overwrite", false)) {
        return output.string() + " already exists (pass overwrite=true to replace it)";
    }
    if (!output.parent_path().empty()) fs::create_directories(output.parent_path(), ec);
    return {};
}

Result written(const std::string& message, const fs::path& output) {
    std::error_code ec;
    auto bytes = static_cast<std::int64_t>(fs::file_size(output, ec));
    Result r = Result::success(message, output.string());
    r.with("output", output.string()).with("bytes", ec ? std::int64_t{0} : bytes);
    if (ffprobePath()) {
        try {
            MediaInfo m = probe(output);
            if (m.duration > 0 && !m.still) r.with("duration", m.duration);
            if (m.width > 0) r.with("width", static_cast<std::int64_t>(m.width)).with("height", static_cast<std::int64_t>(m.height));
        } catch (const std::exception&) {
        }
    }
    return r;
}

std::vector<std::string> splitList(const std::string& text) {
    std::vector<std::string> out;
    const char sep = text.find('\n') != std::string::npos ? '\n' : (text.find('|') != std::string::npos ? '|' : ',');
    std::string cur;
    auto flush = [&] {
        auto a = cur.find_first_not_of(" \t\r"), b = cur.find_last_not_of(" \t\r");
        if (a != std::string::npos) out.push_back(cur.substr(a, b - a + 1));
        cur.clear();
    };
    for (char ch : text) {
        if (ch == sep) flush();
        else cur += ch;
    }
    flush();
    return out;
}

std::string settingsError(const Settings& s) {
    if (s.width < 16 || s.height < 16 || s.width > 8192 || s.height > 8192) return "width and height must be 16..8192";
    if (s.width % 2 || s.height % 2) return "width and height must be even (video encoders need it)";
    if (!(s.fps >= 1 && s.fps <= 240)) return "fps must be 1..240";
    if (s.background.empty() || !parseColour(s.background)) return "background needs a colour";
    return {};
}

// ---- media ----

Clip clipForFile(const std::string& path, bool wantAudio) {
    std::error_code ec;
    if (!fs::is_regular_file(path, ec)) throw std::runtime_error("file not found: " + path);
    const std::string ext = extOf(path);
    Clip c;
    c.source = absolute(path);
    const auto& builtinAudio = audio::builtinReadExtensions();
    const bool nativeAudio = std::find(builtinAudio.begin(), builtinAudio.end(), ext) != builtinAudio.end();
    if (!wantAudio && ext != "gif" && raster::canRead(path) && !nativeAudio) {
        raster::Image img = raster::load(path);
        c.kind = "image";
        c.sourceWidth = img.width();
        c.sourceHeight = img.height();
        c.out = 5;
        return c;
    }
    if (nativeAudio) {
        audio::Buffer b = audio::load(path);
        c.kind = "audio";
        c.hasAudio = true;
        c.sourceDuration = b.seconds();
        c.audioCodec = ext;
        c.out = b.seconds();
        if (!wantAudio) throw std::runtime_error(path + " has no picture; use cut.add_audio for sound files");
        return c;
    }
    MediaInfo m = probe(path);
    fillFromProbe(c, m);
    if (wantAudio) {
        if (!m.hasAudio) throw std::runtime_error(path + " has no sound");
        c.kind = "audio";
        c.hasAudio = true;
        c.out = m.duration;
        if (!(m.duration > 0)) throw std::runtime_error("ffprobe couldn't tell how long " + path + " is");
        return c;
    }
    if (!m.hasVideo) throw std::runtime_error(path + " has no picture; use cut.add_audio for sound files");
    if (m.still) {
        c.kind = "image";
        c.hasAudio = false;
        c.out = 5;
        return c;
    }
    if (!(m.duration > 0)) throw std::runtime_error("ffprobe couldn't tell how long " + path + " is");
    c.kind = "video";
    c.out = m.duration;
    return c;
}

namespace {

bool trackFree(const Timeline& tl, const std::string& id, double from, double to) {
    const Track* t = tl.track(id);
    if (!t) return true;
    for (const auto& c : t->clips) {
        if (c.start < to - 1e-3 && c.end() > from + 1e-3) return false;
    }
    return true;
}

}  // namespace

Result addClip(Timeline& tl, const Params& p, Clip c, const std::string& defaultTrack, const std::string& what) {
    const std::string trackId = stringParam(p, "track", defaultTrack);
    tl.ensureTrack(trackId);
    if ((tl.track(trackId)->kind == "audio") != (c.kind == "audio")) {
        throw std::runtime_error(c.kind == "audio" ? "audio goes on an audio track (a1, a2, ...)" : "pictures go on a video track (v1, v2, ...)");
    }
    c.start = tl.trackEnd(trackId);
    std::string err = applyProps(c, p);
    if (err.empty()) err = validate(c);
    if (!err.empty()) throw std::runtime_error(err);
    c.id = tl.newClipId();
    std::string soundId;
    const std::string audioTrack = stringParam(p, "audio_track", "auto");
    if (c.kind == "video" && c.hasAudio) {
        if (audioTrack == "none") {
            c.ownSound = false;
        } else if (audioTrack != "embedded") {
            soundId = addLinkedSound(tl, c, audioTrack == "auto" ? std::string() : audioTrack);
        }
    }
    tl.track(trackId)->clips.push_back(c);
    if (!c.source.empty() && std::find(tl.bin.begin(), tl.bin.end(), c.source) == tl.bin.end()) tl.bin.push_back(c.source);
    Result r = clipResult("added " + what + " " + c.id + " on " + trackId + " at " + num(c.start) + "s" +
                              (soundId.empty() ? std::string() : " with its sound linked as " + soundId + " on " + trackOf(tl, soundId)->id),
                          tl, p, c.id);
    if (!soundId.empty()) r.with("sound", soundId);
    return r;
}

std::string addLinkedSound(Timeline& tl, Clip& video, const std::string& audioTrack) {
    if (!video.hasAudio) return {};
    std::string id = audioTrack;
    if (id.empty()) {
        for (int n = 1; n < 100; ++n) {
            id = "a" + std::to_string(n);
            if (trackFree(tl, id, video.start, video.end())) break;
        }
    }
    tl.ensureTrack(id);
    if (tl.track(id)->kind != "audio") throw std::runtime_error("audio_track must be an audio track (a1, a2, ...), none or embedded");
    if (video.link.empty()) video.link = tl.newLinkId();
    Clip a;
    a.kind = "audio";
    a.source = video.source;
    a.in = video.in;
    a.out = video.out;
    a.start = video.start;
    a.speed = video.speed;
    a.reverse = video.reverse;
    a.volume = video.volume;
    a.mute = video.mute;
    a.fadeIn = video.fadeIn;
    a.fadeOut = video.fadeOut;
    a.hasAudio = true;
    a.sourceDuration = video.sourceDuration;
    a.audioCodec = video.audioCodec;
    a.link = video.link;
    a.id = tl.newClipId();
    video.ownSound = false;
    video.volume = 1;
    video.mute = false;
    tl.track(id)->clips.push_back(a);
    return a.id;
}

// ---- edits ----

std::vector<Clip*> group(Timeline& tl, const Clip& c, bool linked) {
    std::vector<Clip*> out;
    for (auto& t : tl.tracks) {
        for (auto& x : t.clips) {
            if (x.id == c.id || (linked && !c.link.empty() && x.link == c.link)) out.push_back(&x);
        }
    }
    return out;
}

void rippleTrack(Track& track, double from, double delta, const std::set<std::string>& skip) {
    for (auto& x : track.clips) {
        if (skip.count(x.id)) continue;
        if (x.start >= from - 1e-4) x.start = std::max(0.0, x.start + delta);
    }
}

void shiftKeys(Clip& c, double delta) {
    for (auto& [name, ks] : c.keys) {
        for (auto& k : ks) k.t += delta;
        ks.erase(std::remove_if(ks.begin(), ks.end(), [](const Key& k) { return k.t < -1e-6; }), ks.end());
    }
    for (auto it = c.keys.begin(); it != c.keys.end();) it = it->second.empty() ? c.keys.erase(it) : std::next(it);
}

std::string timingError(const Clip& c) { return validate(c); }

std::string splitClip(Timeline& tl, Track& track, const std::string& clipId, double at) {
    auto it = std::find_if(track.clips.begin(), track.clips.end(), [&](const Clip& x) { return x.id == clipId; });
    if (it == track.clips.end()) throw std::runtime_error("no clip called " + clipId);
    Clip& c = *it;
    if (at <= c.start + 1e-3 || at >= c.end() - 1e-3) {
        throw std::runtime_error("at must be inside " + c.id + ", between " + num(c.start) + " and " + num(c.end()) + "s");
    }
    const double local = at - c.start;
    Clip second = c;
    const double rate = c.timed() ? c.speed : 1;
    if (c.hold) {
        c.out = c.in + local;
        second.out = second.in + (second.out - second.in) - local;
    } else if (c.reverse) {
        c.in = c.out - local * rate;
        second.out = c.in;
    } else {
        c.out = c.in + local * rate;
        second.in = c.out;
    }
    c.fadeOut = 0;
    second.id = tl.newClipId();
    second.start = at;
    second.fadeIn = 0;
    second.transition.clear();
    second.transitionDuration = 0;
    // keyframes: both parts keep the curve, with a key at the cut
    for (auto& [name, ks] : c.keys) {
        const double v = evaluateKeys(ks, local);
        std::vector<Key> left, right;
        for (const auto& k : ks) {
            if (k.t < local - 1e-6) left.push_back(k);
            if (k.t > local + 1e-6) right.push_back({k.t - local, k.v, k.ease});
        }
        const std::string ease = left.empty() ? std::string("linear") : left.back().ease;
        left.push_back({local, v, ease});
        right.insert(right.begin(), {0, v, ease});
        ks = left;
        second.keys[name] = right;
    }
    const std::string id = second.id;
    track.clips.push_back(second);
    return id;
}

void removeClip(Timeline& tl, Track& track, const std::string& clipId) {
    auto it = std::find_if(track.clips.begin(), track.clips.end(), [&](const Clip& x) { return x.id == clipId; });
    if (it == track.clips.end()) return;
    const Clip gone = *it;
    // a transition that blended from it has nothing to blend with any more
    for (auto& x : track.clips) {
        if (x.id == gone.id || x.transition.empty()) continue;
        const Clip* prev = x.media() ? tl.previousMedia(track, x) : tl.previousClip(track, x);
        if (prev && prev->id == gone.id) {
            x.transition.clear();
            x.transitionDuration = 0;
        }
    }
    track.clips.erase(std::find_if(track.clips.begin(), track.clips.end(), [&](const Clip& x) { return x.id == clipId; }));
}

std::vector<std::pair<std::string, std::string>> clearRange(Timeline& tl, Track& track, double from, double to) {
    std::vector<std::pair<std::string, std::string>> parts;
    std::vector<std::string> ids;
    for (const auto& c : track.clips) {
        if (c.start < to - 1e-4 && c.end() > from + 1e-4) ids.push_back(c.id);
    }
    for (std::string id : ids) {
        auto find = [&](const std::string& x) {
            return std::find_if(track.clips.begin(), track.clips.end(), [&](const Clip& y) { return y.id == x; });
        };
        auto it = find(id);
        if (it->start < from - 1e-3) id = splitClip(tl, track, id, from);
        it = find(id);
        if (it->end() > to + 1e-3) {
            const std::string link = it->link;
            parts.push_back({link, splitClip(tl, track, id, to)});
        }
        removeClip(tl, track, id);
    }
    return parts;
}

void followLinked(Timeline& tl, const Clip& before, const Clip& after) {
    if (before.link.empty()) return;
    for (Clip* x : group(tl, before, true)) {
        if (x->id == before.id) continue;
        x->start += after.start - before.start;
        if (x->timed()) {
            x->in += after.in - before.in;
            x->out += after.out - before.out;
            x->speed = after.speed;
            x->reverse = after.reverse;
        } else {
            x->out += (after.length() - before.length());
        }
        if (std::string err = validate(*x); !err.empty()) throw std::runtime_error("linked clip " + x->id + ": " + err + " (linked=false edits this clip alone)");
    }
}

void relinkParts(Timeline& tl, const std::vector<std::pair<std::string, std::string>>& parts) {
    std::map<std::string, std::vector<std::string>> byLink;
    for (const auto& [link, id] : parts) {
        if (!link.empty()) byLink[link].push_back(id);
    }
    for (const auto& [link, ids] : byLink) {
        const std::string fresh = ids.size() > 1 ? tl.newLinkId() : std::string();
        for (const auto& id : ids) {
            if (Clip* c = tl.findClip(id)) c->link = fresh;
        }
    }
}

std::vector<std::string> targetTracks(const Timeline& tl, const std::string& list) {
    std::vector<std::string> out;
    if (list.empty()) {
        for (const auto& t : tl.tracks) {
            if (!t.locked) out.push_back(t.id);
        }
        return out;
    }
    for (const auto& id : splitList(list)) {
        const Track* t = tl.track(id);
        if (!t) throw std::runtime_error("no track called '" + id + "'");
        if (t->locked) throw std::runtime_error("track " + id + " is locked");
        out.push_back(id);
    }
    return out;
}

double incomingOverlap(const Timeline& tl, const Track& track, const Clip& c) {
    if (c.transition.empty() || c.transition == "none") return 0;
    const Clip* prev = c.media() ? tl.previousMedia(track, c) : tl.previousClip(track, c);
    if (!prev) return 0;
    const double o = prev->end() - c.start;
    if (o <= 1e-3 || o > std::min(prev->length(), c.length()) + kEps) return 0;
    return o;
}

namespace {

void fitFades(Clip& c) {
    const double len = std::max(0.0, c.length());
    if (c.fadeIn + c.fadeOut > len && c.fadeIn + c.fadeOut > 0) {
        const double k = len / (c.fadeIn + c.fadeOut);
        c.fadeIn *= k;
        c.fadeOut *= k;
    }
}

}  // namespace

void trimEnd(Clip& c, double d) {
    if (!c.timed() || c.hold) c.out += d;
    else if (c.reverse) c.in -= d * c.speed;
    else c.out += d * c.speed;
    fitFades(c);
}

void trimStart(Clip& c, double d) {
    c.start += d;
    if (!c.timed() || c.hold) c.out -= d;
    else if (c.reverse) c.out -= d * c.speed;
    else c.in += d * c.speed;
    shiftKeys(c, -d);
    fitFades(c);
}

Clip* nextTouching(Track& track, const Clip& c) {
    Clip* best = nullptr;
    for (auto& x : track.clips) {
        if (x.id == c.id || x.start <= c.start + 1e-4 || x.start > c.end() + 1e-3) continue;
        if (!best || x.start < best->start) best = &x;
    }
    return best;
}

Clip* prevTouching(Track& track, const Clip& c) {
    Clip* best = nullptr;
    for (auto& x : track.clips) {
        if (x.id == c.id || x.start >= c.start - 1e-4 || x.end() < c.start - 1e-3) continue;
        if (!best || x.start > best->start) best = &x;
    }
    return best;
}

std::string titleTrack(Timeline& tl, bool captions) {
    std::string track;
    int highest = 0;
    for (const auto& t : tl.tracks) {
        if (t.kind != "video") continue;
        highest = std::max(highest, std::atoi(t.id.c_str() + 1));
        const bool only = std::all_of(t.clips.begin(), t.clips.end(), [&](const Clip& x) { return x.kind == "title" && x.caption == captions; });
        if (only && !t.clips.empty()) track = t.id;
    }
    if (track.empty() || std::atoi(track.c_str() + 1) != highest) track = "v" + std::to_string(highest + 1);
    return track;
}

}  // namespace wetwaresuite::cut
