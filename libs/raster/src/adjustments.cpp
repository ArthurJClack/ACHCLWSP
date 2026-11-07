#include "wetwaresuite/raster/adjustments.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>

#include "wetwaresuite/raster/filters.hpp"
#include "wetwaresuite/raster/ops.hpp"
#include "wetwaresuite/raster/parallel.hpp"
#include "wetwaresuite/raster/selection.hpp"

namespace wetwaresuite::raster {

namespace {

using Kind = AdjustmentField::Kind;
using Lut = std::array<std::uint8_t, 256>;

inline std::uint8_t clamp8(float v) { return static_cast<std::uint8_t>(std::clamp(v, 0.0f, 255.0f) + 0.5f); }

AdjustmentField num(std::string key, double min, double max, std::string fallback, std::string description) {
    return {std::move(key), Kind::Number, std::move(description), min, max, std::move(fallback), {}};
}
AdjustmentField flag(std::string key, std::string fallback, std::string description) {
    return {std::move(key), Kind::Bool, std::move(description), 0, 0, std::move(fallback), {}};
}
AdjustmentField list(std::string key, std::string fallback, std::string description) {
    return {std::move(key), Kind::List, std::move(description), 0, 0, std::move(fallback), {}};
}
AdjustmentField color(std::string key, std::string fallback, std::string description) {
    return {std::move(key), Kind::Color, std::move(description), 0, 0, std::move(fallback), {}};
}
AdjustmentField choice(std::string key, std::vector<std::string> choices, std::string fallback, std::string description) {
    return {std::move(key), Kind::Choice, std::move(description), 0, 0, std::move(fallback), std::move(choices)};
}

const std::vector<std::string>& channelChoices() {
    static const std::vector<std::string> c = {"rgb", "red", "green", "blue"};
    return c;
}

const std::map<std::string, std::vector<AdjustmentField>>& fieldTable() {
    static const std::map<std::string, std::vector<AdjustmentField>> t = {
        {"brightness_contrast",
         {num("brightness", -100, 100, "0", "brightness -100..100"), num("contrast", -100, 100, "0", "contrast -100..100")}},
        {"levels",
         {num("black", 0, 255, "0", "input black point 0..255"), num("white", 0, 255, "255", "input white point 0..255"),
          num("gamma", 0.1, 9.99, "1", "midtone gamma (>1 brighter)"), num("out_black", 0, 255, "0", "output black 0..255"),
          num("out_white", 0, 255, "255", "output white 0..255"),
          choice("channel", channelChoices(), "rgb", "which channel")}},
        {"curves",
         {list("points", "", "curve points as input,output pairs 0..255, e.g. 0,0,64,48,192,210,255,255"),
          choice("preset", {"", "contrast", "strong_contrast", "brighten", "darken", "fade", "invert"}, "", "a ready-made curve"),
          choice("channel", channelChoices(), "rgb", "which channel")}},
        {"exposure",
         {num("exposure", -5, 5, "0", "exposure in stops"), num("offset", -0.5, 0.5, "0", "offset, darkens or lifts the shadows"),
          num("gamma", 0.1, 9.99, "1", "gamma correction (>1 brighter)")}},
        {"vibrance",
         {num("vibrance", -100, 100, "0", "vibrance -100..100 (dull colours change most)"),
          num("saturation", -100, 100, "0", "saturation -100..100")}},
        {"hue_saturation",
         {num("hue", -180, 180, "0", "hue shift in degrees (colorize: the hue 0..360)"),
          num("saturation", -100, 100, "0", "saturation -100..100 (colorize: 0..100)"),
          num("lightness", -100, 100, "0", "lightness -100..100"),
          choice("range", {"master", "reds", "yellows", "greens", "cyans", "blues", "magentas"}, "master", "which colours change"),
          flag("colorize", "false", "tint everything with one hue")}},
        {"color_balance",
         {list("shadows", "0,0,0", "cyan-red, magenta-green, yellow-blue -100..100 for dark tones"),
          list("midtones", "0,0,0", "the same for middle tones"), list("highlights", "0,0,0", "the same for bright tones"),
          flag("preserve_luminosity", "true", "keep brightness the same")}},
        {"black_white",
         {num("reds", -200, 300, "40", "how bright reds become, %"), num("yellows", -200, 300, "60", "yellows, %"),
          num("greens", -200, 300, "40", "greens, %"), num("cyans", -200, 300, "60", "cyans, %"),
          num("blues", -200, 300, "20", "blues, %"), num("magentas", -200, 300, "80", "magentas, %"),
          flag("tint", "false", "tint the gray"), color("tint_color", "#e1d3b3", "tint colour")}},
        {"photo_filter",
         {color("color", "#ec8a00", "filter colour (warming #ec8a00, cooling #006dff)"),
          num("density", 0, 100, "25", "strength 0..100"), flag("preserve_luminosity", "true", "keep brightness the same")}},
        {"channel_mixer",
         {list("red", "100,0,0,0", "output red from red, green, blue % and a constant %"),
          list("green", "0,100,0,0", "output green from red, green, blue %, constant %"),
          list("blue", "0,0,100,0", "output blue from red, green, blue %, constant %"),
          flag("monochrome", "false", "gray from the red row")}},
        {"invert", {}},
        {"posterize", {num("levels", 2, 255, "4", "tones per channel 2..255")}},
        {"threshold", {num("level", 1, 255, "128", "brighter than this becomes white, 1..255")}},
        {"gradient_map",
         {{"colors", Kind::Colors, "colours from shadows to highlights, e.g. #000000,#ff8800,#ffffff", 0, 0, "#000000,#ffffff", {}},
          flag("reverse", "false", "run the gradient the other way")}},
        {"selective_color",
         {choice("range", selectiveColorRanges(), "reds", "which colours to change"),
          num("cyan", -100, 100, "0", "cyan -100..100 for that range"), num("magenta", -100, 100, "0", "magenta -100..100"),
          num("yellow", -100, 100, "0", "yellow -100..100"), num("black", -100, 100, "0", "black -100..100"),
          choice("method", {"relative", "absolute"}, "relative", "relative scales with the ink already there")}},
        // filters: with clip on they act as live (smart) filters on one layer, editable and removable any time
        {"gaussian_blur", {num("radius", 0, 250, "4", "blur radius in pixels 0..250")}},
        {"motion_blur",
         {num("angle", -180, 180, "0", "direction in degrees"), num("distance", 0, 500, "12", "streak length in pixels 0..500")}},
        {"sharpen",
         {num("radius", 0.1, 100, "1.5", "unsharp mask radius in pixels"), num("amount", 0, 500, "80", "strength % 0..500"),
          num("threshold", 0, 255, "0", "skip edges softer than this 0..255")}},
        {"high_pass", {num("radius", 0.1, 250, "3", "keep detail finer than this radius")}},
        {"median", {num("radius", 1, 50, "2", "despeckle radius 1..50")}},
        {"noise",
         {num("amount", 0, 100, "10", "noise strength % 0..100"), flag("gaussian", "true", "gaussian (soft) instead of uniform"),
          flag("monochrome", "false", "same noise on every channel"), num("seed", 0, 1e9, "1", "random seed")}},
        {"oil_paint", {num("radius", 1, 20, "3", "brush size 1..20")}},
        {"find_edges", {}},
        // local tone
        {"shadows_highlights",
         {num("shadow_amount", 0, 100, "35", "lift dark areas 0..100"), num("highlight_amount", 0, 100, "0", "bring down bright areas 0..100"),
          num("radius", 1, 250, "30", "how big an area counts as dark or bright, px")}},
        {"clarity", {num("amount", -100, 100, "30", "midtone contrast -100..100 (negative softens)"), num("radius", 1, 250, "40", "detail size, px")}},
        {"dehaze", {num("amount", -100, 100, "40", "remove haze -100..100 (negative adds it)")}},
    };
    return t;
}

std::vector<std::string> split(const std::string& text, bool keepParens) {
    std::vector<std::string> out;
    std::string cur;
    int depth = 0;
    for (char c : text) {
        if (keepParens && c == '(') ++depth;
        if (keepParens && c == ')') --depth;
        if ((c == ',' || c == ' ' || c == ';') && depth <= 0) {
            if (!cur.empty()) out.push_back(cur);
            cur.clear();
            continue;
        }
        cur += c;
    }
    if (!cur.empty()) out.push_back(cur);
    return out;
}

std::string fallbackOf(const std::string& type, const std::string& key) {
    for (const auto& f : adjustmentFields(type)) {
        if (f.key == key) return f.fallback;
    }
    return "";
}

double numberOf(const std::string& type, const Settings& s, const std::string& key) {
    const std::string fb = fallbackOf(type, key);
    return settingNumber(s, key, fb.empty() ? 0.0 : std::atof(fb.c_str()));
}

std::vector<double> listOf(const std::string& type, const Settings& s, const std::string& key) {
    if (s.count(key)) return settingList(s, key);
    Settings tmp{{key, fallbackOf(type, key)}};
    return settingList(tmp, key);
}

Color colorOf(const std::string& type, const Settings& s, const std::string& key) {
    auto it = s.find(key);
    if (it != s.end()) {
        if (auto c = Color::parse(it->second)) return *c;
    }
    return Color::parse(fallbackOf(type, key)).value_or(Color{});
}

// Visits every pixel across threads: per-pixel maps only.
template <typename Fn>
void eachPixel(Image& img, Fn fn) {
    const int w = img.width();
    parallelRows(img.height(), static_cast<std::size_t>(w) * 2, [&](int b, int e) {
        for (int y = b; y < e; ++y) {
            std::uint8_t* p = img.pixel(0, y);
            for (int x = 0; x < w; ++x) fn(p + static_cast<std::size_t>(x) * 4);
        }
    });
}

void applyLuts(Image& img, const Lut& r, const Lut& g, const Lut& b) {
    eachPixel(img, [&](std::uint8_t* p) {
        p[0] = r[p[0]];
        p[1] = g[p[1]];
        p[2] = b[p[2]];
    });
}

Lut identity() {
    Lut l{};
    for (int i = 0; i < 256; ++i) l[static_cast<std::size_t>(i)] = static_cast<std::uint8_t>(i);
    return l;
}

void byChannel(Image& img, const std::string& channel, const Lut& lut) {
    const Lut same = identity();
    applyLuts(img, channel == "rgb" || channel == "red" ? lut : same, channel == "rgb" || channel == "green" ? lut : same,
              channel == "rgb" || channel == "blue" ? lut : same);
}

float luma(const std::uint8_t* p) { return 0.299f * p[0] + 0.587f * p[1] + 0.114f * p[2]; }

void rgbToHsl(float r, float g, float b, float& h, float& s, float& l) {
    const float mx = std::max({r, g, b}), mn = std::min({r, g, b});
    l = (mx + mn) / 2;
    if (mx == mn) {
        h = s = 0;
        return;
    }
    const float d = mx - mn;
    s = l > 0.5f ? d / (2 - mx - mn) : d / (mx + mn);
    if (mx == r) h = (g - b) / d + (g < b ? 6 : 0);
    else if (mx == g) h = (b - r) / d + 2;
    else h = (r - g) / d + 4;
    h /= 6;
}

float hueToRgb(float p, float q, float t) {
    if (t < 0) t += 1;
    if (t > 1) t -= 1;
    if (t < 1.0f / 6) return p + (q - p) * 6 * t;
    if (t < 0.5f) return q;
    if (t < 2.0f / 3) return p + (q - p) * (2.0f / 3 - t) * 6;
    return p;
}

void hslToRgb(float h, float s, float l, float& r, float& g, float& b) {
    if (s <= 0) {
        r = g = b = l;
        return;
    }
    const float q = l < 0.5f ? l * (1 + s) : l + s - l * s;
    const float p = 2 * l - q;
    r = hueToRgb(p, q, h + 1.0f / 3);
    g = hueToRgb(p, q, h);
    b = hueToRgb(p, q, h - 1.0f / 3);
}

// How much a hue (0..1) belongs to a named range: full within 15 degrees of its centre, fading to none at 45.
float rangeWeight(const std::string& range, float hue) {
    static const std::map<std::string, float> centres = {{"reds", 0},      {"yellows", 60}, {"greens", 120},
                                                         {"cyans", 180},  {"blues", 240},  {"magentas", 300}};
    auto it = centres.find(range);
    if (it == centres.end()) return 1;
    float d = std::fabs(hue * 360 - it->second);
    d = std::min(d, 360 - d);
    if (d <= 15) return 1;
    if (d >= 45) return 0;
    return (45 - d) / 30;
}

void hueSaturation(Image& img, const Settings& s) {
    const std::string t = "hue_saturation";
    const float hue = static_cast<float>(numberOf(t, s, "hue"));
    const float sat = static_cast<float>(std::clamp(numberOf(t, s, "saturation"), -100.0, 100.0) / 100);
    const float light = static_cast<float>(std::clamp(numberOf(t, s, "lightness"), -100.0, 100.0) / 100);
    const bool colorize = settingBool(s, "colorize", false);
    const std::string range = s.count("range") ? s.at("range") : "master";
    eachPixel(img, [&](std::uint8_t* p) {
        float h, sa, l;
        rgbToHsl(p[0] / 255.0f, p[1] / 255.0f, p[2] / 255.0f, h, sa, l);
        float w = 1;
        if (colorize) {
            h = std::fmod(hue / 360.0f + 1.0f, 1.0f);
            sa = std::clamp(sat < 0 ? 0.0f : sat, 0.0f, 1.0f);
            if (s.count("saturation") == 0) sa = 0.25f;
        } else {
            w = range == "master" ? 1.0f : rangeWeight(range, h) * std::min(1.0f, sa * 4);
            if (w <= 0) return;
            h = std::fmod(h + w * hue / 360.0f + 2.0f, 1.0f);
            const float k = sat * w;
            sa = std::clamp(k < 0 ? sa * (1 + k) : sa + (1 - sa) * k * sa, 0.0f, 1.0f);
        }
        const float lk = light * w;
        l = lk >= 0 ? l + (1 - l) * lk : l * (1 + lk);
        float r, g, b;
        hslToRgb(h, sa, l, r, g, b);
        p[0] = clamp8(r * 255), p[1] = clamp8(g * 255), p[2] = clamp8(b * 255);
    });
}

void blackWhite(Image& img, const Settings& s) {
    const std::string t = "black_white";
    // weights for the primary (max channel) and secondary (two highest) colours
    const float red = static_cast<float>(numberOf(t, s, "reds") / 100), yellow = static_cast<float>(numberOf(t, s, "yellows") / 100);
    const float green = static_cast<float>(numberOf(t, s, "greens") / 100), cyan = static_cast<float>(numberOf(t, s, "cyans") / 100);
    const float blue = static_cast<float>(numberOf(t, s, "blues") / 100), magenta = static_cast<float>(numberOf(t, s, "magentas") / 100);
    const bool tint = settingBool(s, "tint", false);
    const Color tc = colorOf(t, s, "tint_color");
    const float tcol[3] = {tc.r / 255.0f, tc.g / 255.0f, tc.b / 255.0f};
    eachPixel(img, [&](std::uint8_t* p) {
        const float r = p[0], g = p[1], b = p[2];
        const float mn = std::min({r, g, b}), mx = std::max({r, g, b});
        const float mid = r + g + b - mn - mx;
        float primary, secondary;
        if (mx == r) {
            primary = red;
            secondary = mid == g ? yellow : magenta;
        } else if (mx == g) {
            primary = green;
            secondary = mid == r ? yellow : cyan;
        } else {
            primary = blue;
            secondary = mid == g ? cyan : magenta;
        }
        const float gray = std::clamp(mn + (mid - mn) * secondary + (mx - mid) * primary, 0.0f, 255.0f);
        if (!tint) {
            p[0] = p[1] = p[2] = clamp8(gray);
            return;
        }
        const float gv[3] = {gray / 255, gray / 255, gray / 255};
        float out[3];
        blendColor(Blend::Color, gv, tcol, out);
        p[0] = clamp8(out[0] * 255), p[1] = clamp8(out[1] * 255), p[2] = clamp8(out[2] * 255);
    });
}

void photoFilter(Image& img, const Settings& s) {
    const std::string t = "photo_filter";
    const Color c = colorOf(t, s, "color");
    const float d = static_cast<float>(std::clamp(numberOf(t, s, "density"), 0.0, 100.0) / 100);
    const bool keep = settingBool(s, "preserve_luminosity", true);
    const float f[3] = {c.r / 255.0f, c.g / 255.0f, c.b / 255.0f};
    eachPixel(img, [&](std::uint8_t* p) {
        const float before = luma(p);
        float v[3];
        for (int i = 0; i < 3; ++i) v[i] = p[i] * (1 - d + d * f[i]);
        if (keep) {
            const float after = 0.299f * v[0] + 0.587f * v[1] + 0.114f * v[2];
            if (after > 0) {
                const float k = before / after;
                for (float& x : v) x *= k;
            }
        }
        p[0] = clamp8(v[0]), p[1] = clamp8(v[1]), p[2] = clamp8(v[2]);
    });
}

void channelMixer(Image& img, const Settings& s) {
    const std::string t = "channel_mixer";
    std::array<std::array<float, 4>, 3> m{};
    const char* rows[3] = {"red", "green", "blue"};
    for (int i = 0; i < 3; ++i) {
        std::vector<double> v = listOf(t, s, rows[i]);
        for (int j = 0; j < 4; ++j) m[i][j] = j < static_cast<int>(v.size()) ? static_cast<float>(v[j] / 100) : 0.0f;
    }
    const bool mono = settingBool(s, "monochrome", false);
    if (mono) m[1] = m[2] = m[0];
    eachPixel(img, [&](std::uint8_t* p) {
        const float r = p[0], g = p[1], b = p[2];
        float out[3];
        for (int i = 0; i < 3; ++i) out[i] = m[i][0] * r + m[i][1] * g + m[i][2] * b + m[i][3] * 255;
        p[0] = clamp8(out[0]), p[1] = clamp8(out[1]), p[2] = clamp8(out[2]);
    });
}

void gradientMap(Image& img, const Settings& s) {
    std::vector<Color> stops;
    auto it = s.find("colors");
    if (it == s.end() || !parseColorList(it->second, stops) || stops.size() < 2) parseColorList("#000000,#ffffff", stops);
    if (settingBool(s, "reverse", false)) std::reverse(stops.begin(), stops.end());
    std::array<std::array<std::uint8_t, 3>, 256> lut{};
    const int segments = static_cast<int>(stops.size()) - 1;
    for (int i = 0; i < 256; ++i) {
        const float t = i / 255.0f * static_cast<float>(segments);
        const int k = std::min(segments - 1, static_cast<int>(t));
        const float f = t - static_cast<float>(k);
        const Color& a = stops[static_cast<std::size_t>(k)];
        const Color& b = stops[static_cast<std::size_t>(k + 1)];
        lut[static_cast<std::size_t>(i)] = {clamp8(a.r + (b.r - a.r) * f), clamp8(a.g + (b.g - a.g) * f), clamp8(a.b + (b.b - a.b) * f)};
    }
    eachPixel(img, [&](std::uint8_t* p) {
        const auto& c = lut[clamp8(luma(p))];
        p[0] = c[0], p[1] = c[1], p[2] = c[2];
    });
}

// Selective colour: how much of a pixel belongs to each range, and CMYK pushes per range.
float selectiveWeight(const std::string& range, float r, float g, float b) {
    const float mx = std::max({r, g, b}), mn = std::min({r, g, b});
    if (range == "reds") return mx == r ? r - std::max(g, b) : 0;
    if (range == "greens") return mx == g ? g - std::max(r, b) : 0;
    if (range == "blues") return mx == b ? b - std::max(r, g) : 0;
    if (range == "yellows") return mn == b ? std::min(r, g) - b : 0;
    if (range == "cyans") return mn == r ? std::min(g, b) - r : 0;
    if (range == "magentas") return mn == g ? std::min(r, b) - g : 0;
    if (range == "whites") return std::max(0.0f, (mn - 0.5f) * 2);
    if (range == "blacks") return std::max(0.0f, (0.5f - mx) * 2);
    // neutrals: everything that isn't pure white, black or fully saturated
    return std::max(0.0f, 1 - (std::fabs(mx - 0.5f) + std::fabs(mn - 0.5f)));
}

void selectiveColor(Image& img, const Settings& s) {
    struct Push {
        std::string range;
        float cmyk[4];
    };
    std::vector<Push> pushes;
    for (const auto& range : selectiveColorRanges()) {
        Push p{range, {}};
        bool any = false;
        const char* inks[4] = {"cyan", "magenta", "yellow", "black"};
        for (int i = 0; i < 4; ++i) {
            p.cmyk[i] = static_cast<float>(std::clamp(settingNumber(s, range + "." + inks[i], 0), -100.0, 100.0) / 100);
            any = any || p.cmyk[i] != 0;
        }
        if (any) pushes.push_back(p);
    }
    if (pushes.empty()) return;
    const bool relative = !s.count("method") || s.at("method") != "absolute";
    eachPixel(img, [&](std::uint8_t* p) {
        const float r = p[0] / 255.0f, g = p[1] / 255.0f, b = p[2] / 255.0f;
        float v[3] = {r, g, b};
        for (const auto& push : pushes) {
            const float w = selectiveWeight(push.range, r, g, b);
            if (w <= 0) continue;
            for (int c = 0; c < 3; ++c) {
                // more cyan = less red, more magenta = less green, more yellow = less blue; black darkens all
                const float ink = 1 - v[c];
                float delta = push.cmyk[c] + push.cmyk[3];
                if (relative) delta *= ink;
                v[c] = std::clamp(v[c] - delta * w, 0.0f, 1.0f);
            }
        }
        p[0] = clamp8(v[0] * 255), p[1] = clamp8(v[1] * 255), p[2] = clamp8(v[2] * 255);
    });
}

float toLinear(float v) { return v <= 0.04045f ? v / 12.92f : std::pow((v + 0.055f) / 1.055f, 2.4f); }
float toSrgb(float v) { return v <= 0.0031308f ? v * 12.92f : 1.055f * std::pow(v, 1 / 2.4f) - 0.055f; }

// Brightness 0..1 of every pixel, and the same blurred over `radius` (the area around it).
void lumaPair(const Image& img, double radius, std::vector<float>& L, std::vector<float>& B) {
    const int w = img.width(), h = img.height();
    const std::size_t n = static_cast<std::size_t>(w) * h;
    L.resize(n);
    Image g(w, h, Color{0, 0, 0, 255});
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            const Color c = img.at(x, y);
            const float l = (0.2126f * c.r + 0.7152f * c.g + 0.0722f * c.b) / 255.0f;
            L[static_cast<std::size_t>(y) * w + x] = l;
            const auto v = static_cast<std::uint8_t>(std::lround(l * 255));
            g.set(x, y, Color{v, v, v, 255});
        }
    }
    gaussianBlur(g, radius);
    B.resize(n);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) B[static_cast<std::size_t>(y) * w + x] = g.at(x, y).r / 255.0f;
}

void shadowsHighlights(Image& img, double shadows, double highlights, double radius) {
    std::vector<float> L, B;
    lumaPair(img, radius, L, B);
    const float s = static_cast<float>(shadows / 100), hl = static_cast<float>(highlights / 100);
    const int w = img.width();
    parallelRows(img.height(), static_cast<std::size_t>(w) * 8, [&](int y0, int y1) {
        for (int y = y0; y < y1; ++y) {
            for (int x = 0; x < w; ++x) {
                const std::size_t i = static_cast<std::size_t>(y) * w + x;
                const float l = L[i], b = B[i];
                const float ws = (1 - b) * (1 - b), wh = b * b;
                // dark surroundings: brighten towards a gamma lift; bright ones: pull down
                float target = l + s * ws * (std::sqrt(l) - l) * 1.6f - hl * wh * (l - l * l) * 1.6f;
                target = std::clamp(target, 0.0f, 1.0f);
                const float k = l > 1e-3f ? std::min(4.0f, target / l) : 1.0f + s * 3;
                Color c = img.at(x, y);
                auto mix = [&](std::uint8_t v) {
                    const float add = l <= 1e-3f ? target * 255 : 0;
                    return clamp8(v * k + add);
                };
                c.r = mix(c.r), c.g = mix(c.g), c.b = mix(c.b);
                img.set(x, y, c);
            }
        }
    });
}

void clarity(Image& img, double amount, double radius) {
    std::vector<float> L, B;
    lumaPair(img, radius, L, B);
    const float a = static_cast<float>(amount / 100) * 1.5f;
    const int w = img.width();
    parallelRows(img.height(), static_cast<std::size_t>(w) * 8, [&](int y0, int y1) {
        for (int y = y0; y < y1; ++y) {
            for (int x = 0; x < w; ++x) {
                const std::size_t i = static_cast<std::size_t>(y) * w + x;
                const float l = L[i];
                const float mid = 1 - (2 * l - 1) * (2 * l - 1);  // midtones most, black and white not at all
                const float d = a * (l - B[i]) * mid * 255;
                Color c = img.at(x, y);
                c.r = clamp8(c.r + d), c.g = clamp8(c.g + d), c.b = clamp8(c.b + d);
                img.set(x, y, c);
            }
        }
    });
}

// Dark channel prior, simplified: haze is the light that fills in the darkest channel.
void dehaze(Image& img, double amount) {
    const int w = img.width(), h = img.height();
    if (w == 0 || h == 0) return;
    const float a = static_cast<float>(std::clamp(amount, -100.0, 100.0) / 100);
    // the haze colour: the average of the brightest 0.1% of the dark channel
    std::vector<std::pair<int, int>> dark;
    dark.reserve(static_cast<std::size_t>(w) * h);
    Image dc(w, h, Color{0, 0, 0, 255});
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            const Color c = img.at(x, y);
            const std::uint8_t m = std::min({c.r, c.g, c.b});
            dc.set(x, y, Color{m, m, m, 255});
            dark.push_back({m, y * w + x});
        }
    }
    const std::size_t top = std::max<std::size_t>(1, dark.size() / 1000);
    std::nth_element(dark.begin(), dark.begin() + static_cast<std::ptrdiff_t>(top), dark.end(), [](auto p, auto q) { return p.first > q.first; });
    double ar = 0, ag = 0, ab = 0;
    for (std::size_t i = 0; i < top; ++i) {
        const Color c = img.at(dark[i].second % w, dark[i].second / w);
        ar += c.r, ag += c.g, ab += c.b;
    }
    const float A[3] = {static_cast<float>(std::max(1.0, ar / top)), static_cast<float>(std::max(1.0, ag / top)), static_cast<float>(std::max(1.0, ab / top))};
    gaussianBlur(dc, std::max(3.0, std::min(w, h) / 60.0));
    parallelRows(h, static_cast<std::size_t>(w) * 8, [&](int y0, int y1) {
        for (int y = y0; y < y1; ++y) {
            for (int x = 0; x < w; ++x) {
                Color c = img.at(x, y);
                const float amin = std::min({A[0], A[1], A[2]});
                const float d = dc.at(x, y).r / amin;
                float v[3] = {static_cast<float>(c.r), static_cast<float>(c.g), static_cast<float>(c.b)};
                if (a >= 0) {
                    const float t = std::max(0.15f, 1 - 0.95f * a * d);
                    for (int k = 0; k < 3; ++k) v[k] = (v[k] - A[k]) / t + A[k];
                } else {
                    for (int k = 0; k < 3; ++k) v[k] = v[k] * (1 + a * 0.6f) - a * 0.6f * A[k];
                }
                c.r = clamp8(v[0]), c.g = clamp8(v[1]), c.b = clamp8(v[2]);
                img.set(x, y, c);
            }
        }
    });
}

}  // namespace

const std::vector<std::string>& selectiveColorRanges() {
    static const std::vector<std::string> r = {"reds", "yellows", "greens", "cyans", "blues", "magentas", "whites", "neutrals", "blacks"};
    return r;
}

const std::vector<std::string>& adjustmentLayerTypes() {
    static const std::vector<std::string> t = {"brightness_contrast", "levels",     "curves",        "exposure",       "vibrance",
                                               "hue_saturation",      "color_balance", "black_white", "photo_filter",  "channel_mixer",
                                               "invert",              "posterize",  "threshold",     "gradient_map",   "selective_color",
                                               "gaussian_blur",       "motion_blur", "sharpen",      "high_pass",      "median",
                                               "noise",               "oil_paint",  "find_edges",    "shadows_highlights", "clarity",
                                               "dehaze"};
    return t;
}

const std::vector<AdjustmentField>& adjustmentFields(const std::string& type) {
    static const std::vector<AdjustmentField> none;
    auto it = fieldTable().find(type);
    return it == fieldTable().end() ? none : it->second;
}

const std::vector<AdjustmentField>& allAdjustmentFields() {
    static const std::vector<AdjustmentField> all = [] {
        std::vector<AdjustmentField> v;
        for (const auto& type : adjustmentLayerTypes()) {
            for (const auto& f : adjustmentFields(type)) {
                auto same = std::find_if(v.begin(), v.end(), [&](const AdjustmentField& o) { return o.key == f.key; });
                if (same == v.end()) {
                    v.push_back(f);
                    v.back().description = type + ": " + f.description;
                } else {
                    same->description += "; " + type + ": " + f.description;
                    if (same->kind == Kind::Choice) {
                        for (const auto& c : f.choices) {
                            if (std::find(same->choices.begin(), same->choices.end(), c) == same->choices.end()) same->choices.push_back(c);
                        }
                    }
                }
            }
        }
        return v;
    }();
    return all;
}

double settingNumber(const Settings& s, const std::string& key, double fallback) {
    auto it = s.find(key);
    if (it == s.end() || it->second.empty()) return fallback;
    char* end = nullptr;
    const double v = std::strtod(it->second.c_str(), &end);
    return end == it->second.c_str() ? fallback : v;
}

bool settingBool(const Settings& s, const std::string& key, bool fallback) {
    auto it = s.find(key);
    if (it == s.end()) return fallback;
    return it->second == "true" || it->second == "1" || it->second == "yes" || it->second == "on";
}

std::vector<double> settingList(const Settings& s, const std::string& key) {
    std::vector<double> out;
    auto it = s.find(key);
    if (it == s.end()) return out;
    for (const auto& part : split(it->second, false)) {
        char* end = nullptr;
        const double v = std::strtod(part.c_str(), &end);
        if (end != part.c_str()) out.push_back(v);
    }
    return out;
}

bool parseColorList(const std::string& text, std::vector<Color>& out) {
    out.clear();
    for (const auto& part : split(text, true)) {
        auto c = Color::parse(part);
        if (!c) return false;
        out.push_back(*c);
    }
    return !out.empty();
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

std::string checkAdjustment(const std::string& type, const Settings& s) {
    if (!fieldTable().count(type)) {
        std::string all;
        for (const auto& t : adjustmentLayerTypes()) all += (all.empty() ? "" : ", ") + t;
        return "'" + type + "' isn't an adjustment; use one of: " + all;
    }
    for (const auto& f : adjustmentFields(type)) {
        auto it = s.find(f.key);
        if (it == s.end()) continue;
        const std::string& v = it->second;
        switch (f.kind) {
            case Kind::Number: {
                const double x = settingNumber(s, f.key, 1e300);
                if (x == 1e300) return f.key + " must be a number";
                if (f.max > f.min && (x < f.min || x > f.max)) {
                    char buf[160];
                    std::snprintf(buf, sizeof buf, "%s must be %g..%g (got %g)", f.key.c_str(), f.min, f.max, x);
                    return buf;
                }
                break;
            }
            case Kind::Color:
                if (!Color::parse(v)) return f.key + ": '" + v + "' isn't a colour";
                break;
            case Kind::Colors: {
                std::vector<Color> c;
                if (!parseColorList(v, c) || c.size() < 2) return f.key + " needs at least two colours, e.g. #000000,#ffffff";
                break;
            }
            case Kind::Choice:
                if (std::find(f.choices.begin(), f.choices.end(), v) == f.choices.end()) return f.key + ": '" + v + "' isn't one of the choices";
                break;
            default: break;
        }
    }
    if (type == "curves") {
        std::vector<double> pts = settingList(s, "points");
        const bool preset = s.count("preset") && !s.at("preset").empty();
        if (s.count("points") && (pts.size() < 4 || pts.size() % 2)) return "points must be input,output pairs, at least two, e.g. 0,0,128,160,255,255";
        if (!s.count("points") && !preset) return "curves needs points=in,out,... or a preset";
    }
    if (type == "color_balance") {
        for (const char* k : {"shadows", "midtones", "highlights"}) {
            if (s.count(k) && settingList(s, k).size() != 3) return std::string(k) + " needs three numbers -100..100, e.g. 10,0,-15";
        }
    }
    if (type == "channel_mixer") {
        for (const char* k : {"red", "green", "blue"}) {
            if (s.count(k)) {
                const auto n = settingList(s, k).size();
                if (n != 3 && n != 4) return std::string(k) + " needs red, green, blue percentages and optionally a constant, e.g. 100,0,0";
            }
        }
    }
    return "";
}

void applyAdjustment(Image& img, const std::string& type, const Settings& s) {
    if (img.empty()) return;
    if (type == "brightness_contrast") {
        adjust(img, Adjust::Brightness, numberOf(type, s, "brightness"));
        adjust(img, Adjust::Contrast, numberOf(type, s, "contrast"));
    } else if (type == "levels") {
        const double black = numberOf(type, s, "black"), white = std::max(black + 1, numberOf(type, s, "white"));
        const double g = std::max(0.01, numberOf(type, s, "gamma"));
        const double ob = numberOf(type, s, "out_black"), ow = numberOf(type, s, "out_white");
        Lut lut{};
        for (int i = 0; i < 256; ++i) {
            const double t = std::pow(std::clamp((i - black) / (white - black), 0.0, 1.0), 1.0 / g);
            lut[static_cast<std::size_t>(i)] = clamp8(static_cast<float>(ob + (ow - ob) * t));
        }
        byChannel(img, s.count("channel") ? s.at("channel") : "rgb", lut);
    } else if (type == "curves") {
        std::vector<std::pair<double, double>> pts;
        std::vector<double> v = settingList(s, "points");
        for (std::size_t i = 0; i + 1 < v.size(); i += 2) pts.emplace_back(v[i], v[i + 1]);
        if (pts.size() < 2 && s.count("preset")) pts = curvePreset(s.at("preset"));
        if (pts.size() < 2) return;
        byChannel(img, s.count("channel") ? s.at("channel") : "rgb", curveTable(pts));
    } else if (type == "exposure") {
        const float f = static_cast<float>(std::pow(2.0, numberOf(type, s, "exposure")));
        const float off = static_cast<float>(numberOf(type, s, "offset"));
        const float g = static_cast<float>(std::max(0.01, numberOf(type, s, "gamma")));
        Lut lut{};
        for (int i = 0; i < 256; ++i) {
            const float lin = std::max(0.0f, toLinear(i / 255.0f) * f + off);
            lut[static_cast<std::size_t>(i)] = clamp8(toSrgb(std::min(1.0f, std::pow(lin, 1 / g))) * 255);
        }
        byChannel(img, "rgb", lut);
    } else if (type == "vibrance") {
        adjust(img, Adjust::Vibrance, numberOf(type, s, "vibrance"));
        adjust(img, Adjust::Saturation, numberOf(type, s, "saturation"));
    } else if (type == "hue_saturation") {
        hueSaturation(img, s);
    } else if (type == "color_balance") {
        auto shift = [&](const char* key) {
            std::vector<double> v = listOf(type, s, key);
            v.resize(3, 0);
            return ColorShift{v[0], v[1], v[2]};
        };
        colorBalance(img, shift("shadows"), shift("midtones"), shift("highlights"), settingBool(s, "preserve_luminosity", true));
    } else if (type == "black_white") {
        blackWhite(img, s);
    } else if (type == "photo_filter") {
        photoFilter(img, s);
    } else if (type == "channel_mixer") {
        channelMixer(img, s);
    } else if (type == "invert") {
        applyEffect(img, Effect::Invert, 0);
    } else if (type == "posterize") {
        applyEffect(img, Effect::Posterize, numberOf(type, s, "levels"));
    } else if (type == "threshold") {
        applyEffect(img, Effect::Threshold, numberOf(type, s, "level"));
    } else if (type == "gradient_map") {
        gradientMap(img, s);
    } else if (type == "selective_color") {
        selectiveColor(img, s);
    } else if (type == "gaussian_blur") {
        gaussianBlur(img, numberOf(type, s, "radius"));
    } else if (type == "motion_blur") {
        motionBlur(img, numberOf(type, s, "angle"), numberOf(type, s, "distance"));
    } else if (type == "sharpen") {
        unsharpMaskThreshold(img, numberOf(type, s, "radius"), numberOf(type, s, "amount"),
                             static_cast<int>(numberOf(type, s, "threshold")));
    } else if (type == "high_pass") {
        highPass(img, numberOf(type, s, "radius"));
    } else if (type == "median") {
        medianFilter(img, static_cast<int>(numberOf(type, s, "radius")));
    } else if (type == "noise") {
        addNoise(img, numberOf(type, s, "amount"), settingBool(s, "gaussian", true), settingBool(s, "monochrome", false),
                 static_cast<std::uint32_t>(numberOf(type, s, "seed")));
    } else if (type == "oil_paint") {
        oilPaint(img, static_cast<int>(numberOf(type, s, "radius")));
    } else if (type == "find_edges") {
        findEdges(img);
    } else if (type == "shadows_highlights") {
        shadowsHighlights(img, numberOf(type, s, "shadow_amount"), numberOf(type, s, "highlight_amount"), numberOf(type, s, "radius"));
    } else if (type == "clarity") {
        clarity(img, numberOf(type, s, "amount"), numberOf(type, s, "radius"));
    } else if (type == "dehaze") {
        dehaze(img, numberOf(type, s, "amount"));
    }
}

}  // namespace wetwaresuite::raster
