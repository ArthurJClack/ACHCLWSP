// The compositor: one timeline frame from its tracks. Each visible clip is
// fetched (a decoded still, a video frame through ffmpeg, a colour matte, a
// drawn title, a nested sequence's frame), graded and given its effects, then
// placed by its box, fit, crop and keyframed transform and blended onto the
// frame with its opacity, fades and blend mode. Clips joined by a transition
// are drawn into two layers and mixed first.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <stdexcept>

#include "media.hpp"
#include "proxy.hpp"
#include "render_internal.hpp"
#include "wetwaresuite/raster/codec.hpp"
#include "wetwaresuite/raster/ops.hpp"
#include "wetwaresuite/raster/parallel.hpp"

namespace wetwaresuite::cut {

using raster::Color;
using raster::Image;

namespace {

constexpr double kEps = 1e-6;
constexpr double kPi = 3.14159265358979323846;

std::mutex& hookLock() {
    static std::mutex m;
    return m;
}
VideoFrameFn& hook() {
    static VideoFrameFn fn;
    return fn;
}

std::string tnum(double v) {
    char buf[48];
    std::snprintf(buf, sizeof buf, "%.6f", v);
    return buf;
}

// 2x3 affine: (x, y) -> (a x + b y + c, d x + e y + f)
struct Aff {
    double a = 1, b = 0, c = 0, d = 0, e = 1, f = 0;
    static Aff translate(double x, double y) { return {1, 0, x, 0, 1, y}; }
    static Aff scale(double sx, double sy) { return {sx, 0, 0, 0, sy, 0}; }
    static Aff rotate(double degrees) {
        const double r = degrees * kPi / 180, cs = std::cos(r), sn = std::sin(r);
        return {cs, -sn, 0, sn, cs, 0};
    }
    Aff then(const Aff& o) const {  // o after this
        return {o.a * a + o.b * d, o.a * b + o.b * e, o.a * c + o.b * f + o.c, o.d * a + o.e * d, o.d * b + o.e * e, o.d * c + o.e * f + o.f};
    }
    void apply(double x, double y, double& ox, double& oy) const {
        ox = a * x + b * y + c;
        oy = d * x + e * y + f;
    }
    bool invert(Aff& out) const {
        const double det = a * e - b * d;
        if (std::abs(det) < 1e-12) return false;
        out = {e / det, -b / det, (b * f - e * c) / det, -d / det, a / det, (d * c - a * f) / det};
        return true;
    }
};

struct SrcRect {
    double x0 = 0, y0 = 0, x1 = 0, y1 = 0;
};

std::optional<raster::Blend> blendOf(const std::string& name) {
    if (name.empty() || name == "normal") return raster::Blend::Normal;
    return raster::parseBlend(name);
}

// Draws src's rectangle r through m (src px -> dst px) onto dst with an opacity and blend mode.
void drawLayer(Image& dst, const Image& src, const SrcRect& r, const Aff& m, double opacity, raster::Blend mode) {
    if (src.empty() || opacity <= 1e-4 || r.x1 <= r.x0 || r.y1 <= r.y0) return;
    Aff inv;
    if (!m.invert(inv)) return;
    double bx0 = 1e30, by0 = 1e30, bx1 = -1e30, by1 = -1e30;
    for (double cx : {r.x0, r.x1}) {
        for (double cy : {r.y0, r.y1}) {
            double x, y;
            m.apply(cx, cy, x, y);
            bx0 = std::min(bx0, x), by0 = std::min(by0, y), bx1 = std::max(bx1, x), by1 = std::max(by1, y);
        }
    }
    const int x0 = std::max(0, static_cast<int>(std::floor(bx0))), y0 = std::max(0, static_cast<int>(std::floor(by0)));
    const int x1 = std::min(dst.width(), static_cast<int>(std::ceil(bx1))), y1 = std::min(dst.height(), static_cast<int>(std::ceil(by1)));
    if (x1 <= x0 || y1 <= y0) return;
    const double kx = std::hypot(m.a, m.d), ky = std::hypot(m.b, m.e);  // dst px per src px along each source axis
    const int sw = src.width(), sh = src.height();
    const int lx = std::clamp(static_cast<int>(std::floor(r.x0)), 0, sw - 1), hx = std::clamp(static_cast<int>(std::ceil(r.x1)) - 1, 0, sw - 1);
    const int ly = std::clamp(static_cast<int>(std::floor(r.y0)), 0, sh - 1), hy = std::clamp(static_cast<int>(std::ceil(r.y1)) - 1, 0, sh - 1);
    const float op = static_cast<float>(std::clamp(opacity, 0.0, 1.0));
    raster::parallelRows(y1 - y0, static_cast<std::size_t>(x1 - x0) * (mode == raster::Blend::Normal ? 2 : 8), [&](int ra, int rb) {
        for (int yy = y0 + ra; yy < y0 + rb; ++yy) {
            std::uint8_t* out = dst.pixel(x0, yy);
            for (int xx = x0; xx < x1; ++xx, out += 4) {
                const double u = inv.a * (xx + 0.5) + inv.b * (yy + 0.5) + inv.c;
                const double v = inv.d * (xx + 0.5) + inv.e * (yy + 0.5) + inv.f;
                const double cx = std::min(u - r.x0, r.x1 - u) * kx + 0.5, cy = std::min(v - r.y0, r.y1 - v) * ky + 0.5;
                if (cx <= 0 || cy <= 0) continue;
                const float cov = static_cast<float>(std::min(1.0, cx) * std::min(1.0, cy));
                // bilinear, premultiplied
                const double fu = u - 0.5, fv = v - 0.5;
                int iu = static_cast<int>(std::floor(fu)), iv = static_cast<int>(std::floor(fv));
                const float tu = static_cast<float>(fu - iu), tv = static_cast<float>(fv - iv);
                const int u0 = std::clamp(iu, lx, hx), u1 = std::clamp(iu + 1, lx, hx);
                const int v0 = std::clamp(iv, ly, hy), v1 = std::clamp(iv + 1, ly, hy);
                const std::uint8_t* p00 = src.pixel(u0, v0);
                const std::uint8_t* p10 = src.pixel(u1, v0);
                const std::uint8_t* p01 = src.pixel(u0, v1);
                const std::uint8_t* p11 = src.pixel(u1, v1);
                const float w00 = (1 - tu) * (1 - tv) * p00[3], w10 = tu * (1 - tv) * p10[3], w01 = (1 - tu) * tv * p01[3], w11 = tu * tv * p11[3];
                const float sa = (w00 + w10 + w01 + w11) / 255.0f;
                if (sa <= 0) continue;
                float s[3];
                for (int k = 0; k < 3; ++k) s[k] = (p00[k] * w00 + p10[k] * w10 + p01[k] * w01 + p11[k] * w11) / (255.0f * 255.0f * sa);
                const float a = sa * op * cov;
                if (a <= 0) continue;
                const float da = out[3] / 255.0f;
                float d[3] = {out[0] / 255.0f, out[1] / 255.0f, out[2] / 255.0f};
                if (mode != raster::Blend::Normal && da > 0) {
                    float mixed[3];
                    raster::blendColor(mode, d, s, mixed);
                    for (int k = 0; k < 3; ++k) s[k] = s[k] * (1 - da) + mixed[k] * da;
                }
                const float oa = a + da * (1 - a);
                for (int k = 0; k < 3; ++k) {
                    const float vv = (s[k] * a + d[k] * da * (1 - a)) / oa;
                    out[k] = static_cast<std::uint8_t>(std::lround(std::clamp(vv, 0.0f, 1.0f) * 255));
                }
                out[3] = static_cast<std::uint8_t>(std::lround(std::clamp(oa, 0.0f, 1.0f) * 255));
            }
        }
    });
}

// The output frame's mapping from timeline px.
struct View {
    double s = 1, ox = 0, oy = 0;
    int w = 0, h = 0;
    Aff aff() const { return Aff::scale(s, s).then(Aff::translate(ox, oy)); }
};

double easeOut(double u) { return 1 - (1 - u) * (1 - u); }

double fadeFactor(const Clip& c, double local) {
    double f = 1;
    const double len = c.length();
    if (c.fadeIn > kEps && local < c.fadeIn) f *= std::clamp(local / c.fadeIn, 0.0, 1.0);
    if (c.fadeOut > kEps && local > len - c.fadeOut) f *= std::clamp((len - local) / c.fadeOut, 0.0, 1.0);
    return f;
}

bool needsLook(const Clip& c) {
    if (!c.keyColor.empty() || !c.lut.empty() || c.grayscale || c.flipH || c.flipV) return true;
    if (!c.curve.empty() || !c.curveR.empty() || !c.curveG.empty() || !c.curveB.empty()) return true;
    for (const auto* w : {&c.wheelLift, &c.wheelGamma, &c.wheelGain}) {
        for (double v : *w) {
            if (std::abs(v) > kEps) return true;
        }
    }
    const Clip d;
    for (const char* p : {"exposure", "temperature", "tint", "brightness", "contrast", "saturation", "gamma", "blur", "sharpen", "vignette"}) {
        if (c.keys.count(p)) return true;
        if (std::abs(*propertyField(c, p) - *propertyField(d, p)) > kEps) return true;
    }
    return false;
}

int evenUp(double v) {
    int i = std::max(2, static_cast<int>(std::lround(v)));
    return i + (i % 2);
}

// A source picture for one clip at one moment, ready to place: its pixels and their size in source px.
struct Picture {
    std::shared_ptr<const Image> shared;
    Image owned;
    double srcW = 0, srcH = 0;  // the source's full size (what crop and fit are measured in)
    const Image& image() const { return shared ? *shared : owned; }
};

}  // namespace

void setVideoFrameSource(VideoFrameFn fn) {
    std::lock_guard<std::mutex> g(hookLock());
    hook() = std::move(fn);
}

// ---- video reader ----

struct VideoReader::Proc {
    Stream stream;
};

VideoReader::VideoReader(std::string path, int width, int height, double rate)
    : path_(std::move(path)), w_(width), h_(height), rate_(rate > 0 && rate <= 240 ? rate : 30) {}

VideoReader::~VideoReader() = default;

bool VideoReader::restart(double at, std::string& error) {
    proc_ = std::make_unique<Proc>();
    const std::string vf = "fps=" + tnum(rate_) + ",scale=" + std::to_string(w_) + ":" + std::to_string(h_) + ":flags=bilinear,format=rgba";
    std::vector<std::string> args = {"ffmpeg", "-hide_banner", "-nostdin", "-loglevel", "error"};
    if (at > 0) args.insert(args.end(), {"-ss", tnum(at)});
    args.insert(args.end(), {"-i", path_, "-an", "-sn", "-dn", "-vf", vf, "-f", "rawvideo", "-pix_fmt", "rgba", "pipe:1"});
    if (!proc_->stream.start(args, false, true, error)) {
        proc_.reset();
        return false;
    }
    t0_ = at;
    next_ = static_cast<long>(std::llround(at * rate_));
    return true;
}

bool VideoReader::frame(double sourceTime, Image& out, std::string& error) {
    const long n = std::max(0L, static_cast<long>(std::floor(sourceTime * rate_ + 1e-3)));
    if (n == lastIndex_ && !last_.empty()) {
        out = last_;
        return true;
    }
    if (auto it = cache_.find(n); it != cache_.end()) {
        out = it->second;
        return true;
    }
    const std::size_t bytes = static_cast<std::size_t>(w_) * h_ * 4;
    const bool backwards = lastIndex_ >= 0 && n < lastIndex_ && lastIndex_ - n < static_cast<long>(rate_ * 4);
    if (backwards) {
        // read a run of frames ending at n and keep them, so playing backwards doesn't seek every frame
        const long budget = std::clamp(static_cast<long>((64u << 20) / std::max<std::size_t>(1, bytes)), 2L, static_cast<long>(rate_));
        const long first = std::max(0L, n - budget + 1);
        cache_.clear();
        if (!restart(first / rate_, error)) return false;
        for (long i = first; i <= n; ++i) {
            Image img(w_, h_);
            if (!proc_->stream.read(img.data(), bytes)) break;
            cache_[i] = std::move(img);
        }
        proc_.reset();
        next_ = -1;
        if (cache_.empty()) {
            error = "couldn't decode " + path_ + " at " + tnum(sourceTime) + "s";
            return false;
        }
        auto it = cache_.upper_bound(n);
        out = std::prev(it)->second;
        lastIndex_ = n;
        last_ = out;
        return true;
    }
    if (!proc_ || n < next_ || n > next_ + static_cast<long>(rate_ * 2)) {
        cache_.clear();
        if (!restart(n / rate_, error)) return false;
    }
    while (next_ <= n) {
        Image img(w_, h_);
        if (!proc_->stream.read(img.data(), bytes)) {
            // past the end: hold the last frame
            const std::string why = proc_->stream.error();
            proc_.reset();
            next_ = -1;
            if (last_.empty()) {
                error = "ffmpeg couldn't decode " + path_ + (why.empty() ? std::string() : ": " + why);
                return false;
            }
            break;
        }
        last_ = std::move(img);
        lastIndex_ = next_;
        ++next_;
    }
    out = last_;
    return true;
}

// ---- the context ----

FrameContext::FrameContext() : impl_(std::make_unique<Impl>()) {}
FrameContext::~FrameContext() = default;

std::shared_ptr<const Image> FrameContext::Impl::still(const std::string& path) {
    if (auto it = stills.find(path); it != stills.end()) return it->second;
    std::shared_ptr<Image> img;
    try {
        img = std::make_shared<Image>(raster::load(path));
    } catch (const std::exception& e) {
        // formats stb can't read: one frame through ffmpeg
        if (!ffmpegPath()) throw std::runtime_error("can't read the image " + path + ": " + e.what());
        MediaInfo m = probe(path);
        if (m.width <= 0 || m.height <= 0) throw std::runtime_error("can't read the image " + path);
        VideoReader r(path, m.width, m.height, 1);
        Image frame;
        std::string err;
        if (!r.frame(0, frame, err)) throw std::runtime_error(err);
        img = std::make_shared<Image>(std::move(frame));
    }
    if (stills.size() > 32) stills.clear();
    stills[path] = img;
    return img;
}

bool FrameContext::Impl::video(const std::string& path, double sourceTime, int width, int height, double rate, Image& out) {
    {
        std::lock_guard<std::mutex> g(hookLock());
        if (hook()) return hook()(path, sourceTime, width, height, out);
    }
    const std::string& file = preview ? previewSource(path) : path;
    if (approximate && scrubFrame(file, sourceTime, width, height, out)) return true;
    const std::string key = file + "|" + std::to_string(width) + "x" + std::to_string(height);
    auto it = readers.find(key);
    if (it == readers.end()) {
        if (!ffmpegPath()) throw std::runtime_error(missingFfmpeg());
        if (readers.size() >= 8) {
            // close the one used longest ago
            auto oldest = std::min_element(readerUse.begin(), readerUse.end(), [](const auto& a, const auto& b) { return a.second < b.second; });
            readers.erase(oldest->first);
            readerUse.erase(oldest);
        }
        it = readers.emplace(key, std::make_unique<VideoReader>(file, width, height, rate)).first;
    }
    readerUse[key] = ++useCounter;
    std::string err;
    if (!it->second->frame(sourceTime, out, err)) throw std::runtime_error(err);
    return true;
}

std::shared_ptr<const TitleImage> FrameContext::Impl::title(const Clip& c, const Settings& s, double scale, int chars) {
    Clip key = c;
    key.id.clear();
    key.start = key.in = key.out = 0;
    key.keys.clear();
    key.fadeIn = key.fadeOut = 0;
    key.opacity = 1;
    key.animIn = key.animOut = "";
    key.caption = false;
    key.scale = 1;
    key.rotation = 0;
    const std::string k = key.text + "|" + key.subtitle + "|" + key.font + "|" + key.color + "|" + key.position + "|" + key.boxColor + "|" +
                          key.align + "|" + key.outlineColor + "|" + key.shadowColor + "|" + key.style + "|" + key.accentColor + "|" +
                          tnum(key.fontSize) + "|" + tnum(key.x) + "|" + tnum(key.y) + "|" + tnum(key.outline) + "|" + tnum(key.shadow) +
                          (key.box ? "b" : "-") + (key.bold ? "B" : "-") + (key.italic ? "I" : "-") + "|" + std::to_string(s.width) + "x" +
                          std::to_string(s.height) + "|" + tnum(scale) + "|" + std::to_string(chars);
    if (auto it = titles.find(k); it != titles.end()) return it->second;
    auto img = std::make_shared<TitleImage>(drawTitle(c, s, scale, chars));
    if (titles.size() > 64) titles.clear();
    titles[k] = img;
    return img;
}

const Lut3d* FrameContext::Impl::lut(const std::string& path) {
    if (path.empty()) return nullptr;
    if (auto it = luts.find(path); it != luts.end()) return it->second.get();
    auto l = std::make_shared<const Lut3d>(loadCube(path));
    luts[path] = l;
    return l.get();
}

FrameContext& previewContext() {
    static FrameContext ctx;
    static const bool marked = (ctx.impl().preview = true);
    (void)marked;
    return ctx;
}

void clearCaches() {
    auto& impl = previewContext().impl();
    std::lock_guard<std::mutex> g(impl.lock);
    impl.stills.clear();
    impl.titles.clear();
    impl.luts.clear();
    impl.readers.clear();
    impl.readerUse.clear();
    clearSoundCache();
    clearStrips();
}

namespace {

class Compositor {
public:
    Compositor(const Timeline& tl, FrameContext::Impl& ctx, const View& view) : tl_(tl), ctx_(ctx), view_(view) {}

    // Draws one clip at timeline time t onto dst.
    void drawClip(Image& dst, const Clip& c, double t) {
        const double local = t - c.start;
        if (c.kind == "title") {
            drawTitleClip(dst, c, local);
            return;
        }
        Picture pic;
        if (!fetch(c, local, pic)) return;
        const Image* img = &pic.image();
        if (img->empty()) return;
        const double fx = img->width() / std::max(1.0, pic.srcW), fy = img->height() / std::max(1.0, pic.srcH);
        // the source rectangle (pre-crop), in image px
        SrcRect r{0, 0, static_cast<double>(img->width()), static_cast<double>(img->height())};
        if (c.cropW > 0 && c.cropH > 0 && c.kind != "color") {
            r = {c.cropX * fx, c.cropY * fy, (c.cropX + c.cropW) * fx, (c.cropY + c.cropH) * fy};
            r.x0 = std::clamp(r.x0, 0.0, static_cast<double>(img->width()));
            r.x1 = std::clamp(r.x1, r.x0, static_cast<double>(img->width()));
            r.y0 = std::clamp(r.y0, 0.0, static_cast<double>(img->height()));
            r.y1 = std::clamp(r.y1, r.y0, static_cast<double>(img->height()));
        }
        // the box on the frame (timeline px)
        const double W = tl_.settings.width, H = tl_.settings.height;
        double bw = c.value("width", local), bh = c.value("height", local);
        const double sw = c.kind == "color" ? W : (r.x1 - r.x0) / fx, sh = c.kind == "color" ? H : (r.y1 - r.y0) / fy;
        if (bw <= 0 && bh <= 0) {
            bw = W;
            bh = H;
        } else if (bw <= 0) {
            bw = bh * sw / std::max(1e-9, sh);
        } else if (bh <= 0) {
            bh = bw * sh / std::max(1e-9, sw);
        }
        const double bx = c.value("x", local), by = c.value("y", local);
        // the picture inside the box
        double px0 = bx, py0 = by, px1 = bx + bw, py1 = by + bh;
        if (c.kind != "color" && c.fit != "stretch") {
            const double k = c.fit == "fill" ? std::max(bw / sw, bh / sh) : std::min(bw / sw, bh / sh);
            if (c.fit == "fill") {
                // show the middle of the source that covers the box
                const double needW = bw / k * fx, needH = bh / k * fy;
                const double cx = (r.x0 + r.x1) / 2, cy = (r.y0 + r.y1) / 2;
                r = {cx - needW / 2, cy - needH / 2, cx + needW / 2, cy + needH / 2};
            } else {
                const double pw = sw * k, ph = sh * k;
                px0 = bx + (bw - pw) / 2;
                py0 = by + (bh - ph) / 2;
                px1 = px0 + pw;
                py1 = py0 + ph;
            }
        }
        // edge crops (% of the picture)
        const double cl = std::clamp(c.value("crop_left", local), 0.0, 100.0) / 100, cr = std::clamp(c.value("crop_right", local), 0.0, 100.0) / 100;
        const double ct = std::clamp(c.value("crop_top", local), 0.0, 100.0) / 100, cb = std::clamp(c.value("crop_bottom", local), 0.0, 100.0) / 100;
        if (cl + cr >= 1 || ct + cb >= 1) return;
        const double rw = r.x1 - r.x0, rh = r.y1 - r.y0, pw = px1 - px0, ph = py1 - py0;
        // the mapping from image px to timeline px (before cropping the edges)
        Aff m = Aff::translate(-r.x0, -r.y0).then(Aff::scale(pw / rw, ph / rh)).then(Aff::translate(px0, py0));
        SrcRect shown{r.x0 + rw * cl, r.y0 + rh * ct, r.x1 - rw * cr, r.y1 - rh * cb};
        // transform around the box centre
        const double scale = c.value("scale", local), rotation = c.value("rotation", local);
        if (scale <= 1e-4) return;
        const double cx = bx + bw / 2, cy = by + bh / 2;
        if (std::abs(scale - 1) > kEps || std::abs(rotation) > kEps) {
            m = m.then(Aff::translate(-cx, -cy)).then(Aff::scale(scale, scale)).then(Aff::rotate(rotation)).then(Aff::translate(cx, cy));
        }
        m = m.then(view_.aff());
        const double alpha = std::clamp(c.value("opacity", local), 0.0, 1.0) * fadeFactor(c, local);
        drawLayer(dst, *img, shown, m, alpha, blendOf(c.blend).value_or(raster::Blend::Normal));
    }

    // The picture of a media clip: decoded, pre-sized, graded.
    bool fetch(const Clip& c, double local, Picture& pic) {
        const double W = tl_.settings.width, H = tl_.settings.height;
        if (c.kind == "color") {
            pic.owned = Image(1, 1, colourOr(c.color, {0, 0, 0, 255}));
            pic.srcW = pic.srcH = 1;
            return true;
        }
        if (c.kind == "nest") {
            if (!c.nested) return false;
            const Timeline& n = *c.nested;
            FrameRequest rq;
            rq.t = c.sourceTime(local);
            rq.width = std::max(2, static_cast<int>(std::lround(n.settings.width * view_.s)));
            rq.height = std::max(2, static_cast<int>(std::lround(n.settings.height * view_.s)));
            rq.transparent = true;
            pic.owned = renderNested(n, rq);
            pic.srcW = n.settings.width;
            pic.srcH = n.settings.height;
        } else if (c.kind == "image") {
            auto full = ctx_.still(c.source);
            pic.srcW = full->width();
            pic.srcH = full->height();
            // big photos: work on a copy about the size they'll show at
            const double need = std::max(W * view_.s / pic.srcW, H * view_.s / pic.srcH) * std::max(1.0, c.scale) *
                                (c.cropW > 0 ? pic.srcW / c.cropW : 1.0);
            int level = 0;
            while (level < 6 && need * (1 << (level + 1)) <= 1.0) ++level;
            if (level == 0) {
                pic.shared = full;
            } else {
                const std::string key = c.source + "#" + std::to_string(level);
                auto it = ctx_.stills.find(key);
                if (it == ctx_.stills.end()) {
                    auto small = std::make_shared<Image>(raster::resize(*full, std::max(1, full->width() >> level), std::max(1, full->height() >> level),
                                                                        raster::Filter::Area));
                    it = ctx_.stills.emplace(key, small).first;
                }
                pic.shared = it->second;
            }
        } else if (c.kind == "video") {
            const double sw = c.sourceWidth > 0 ? c.sourceWidth : W, sh = c.sourceHeight > 0 ? c.sourceHeight : H;
            double f = std::max(W * view_.s / sw, H * view_.s / sh) * std::max(1.0, c.scale);
            if (c.cropW > 0) f *= sw / c.cropW;
            f = std::min(1.0, f);
            const int dw = evenUp(sw * f), dh = evenUp(sh * f);
            ctx_.video(c.source, c.sourceTime(local), dw, dh, c.sourceFps > 0 ? c.sourceFps : tl_.settings.fps, pic.owned);
            pic.srcW = sw;
            pic.srcH = sh;
        } else {
            return false;
        }
        if (needsLook(c)) {
            if (pic.shared) {
                pic.owned = *pic.shared;
                pic.shared.reset();
            }
            const double pixelScale = pic.owned.width() / std::max(1.0, pic.srcW) * (pic.srcW / std::max(1.0, W));
            applyLook(pic.owned, c, local, std::max(0.05, pixelScale), ctx_.lut(c.lut));
        }
        return true;
    }

    Image renderNested(const Timeline& n, const FrameRequest& rq);

    void drawTitleClip(Image& dst, const Clip& c, double local) {
        const double len = c.length(), d = std::max(0.01, std::min(c.animDuration, len / 2));
        const double pin = std::clamp(local / d, 0.0, 1.0), pout = std::clamp((len - local) / d, 0.0, 1.0);
        int chars = -1;
        if (c.animIn == "typewriter" && pin < 1) chars = static_cast<int>(std::floor(pin * static_cast<double>(titleLength(c)) + 1e-9));
        if (c.animOut == "typewriter" && pout < 1) chars = static_cast<int>(std::floor(pout * static_cast<double>(titleLength(c)) + 1e-9));
        auto ti = ctx_.title(c, tl_.settings, view_.s, chars);
        if (ti->image.empty()) return;
        double alpha = std::clamp(c.value("opacity", local), 0.0, 1.0) * fadeFactor(c, local);
        double dx = 0, dy = 0, zoom = 1;
        SrcRect r{0, 0, static_cast<double>(ti->image.width()), static_cast<double>(ti->image.height())};
        auto animate = [&](const std::string& kind, double p) {
            if (kind == "none" || kind.empty() || p >= 1) return;
            const double e = easeOut(p);
            const double H = tl_.settings.height, W = tl_.settings.width;
            if (kind == "fade") alpha *= p;
            else if (kind == "slide_up") dy += (1 - e) * H * 0.08, alpha *= p;
            else if (kind == "slide_down") dy -= (1 - e) * H * 0.08, alpha *= p;
            else if (kind == "slide_left") dx += (1 - e) * W * 0.1, alpha *= p;
            else if (kind == "slide_right") dx -= (1 - e) * W * 0.1, alpha *= p;
            else if (kind == "zoom") zoom *= 0.5 + 0.5 * e, alpha *= p;
            else if (kind == "wipe") r.x1 = r.x0 + (r.x1 - r.x0) * e;
        };
        animate(c.animIn, pin);
        animate(c.animOut, pout);
        if (c.position == "custom") {
            dx += c.value("x", local) - c.x;
            dy += c.value("y", local) - c.y;
        }
        const double scale = c.value("scale", local) * zoom, rotation = c.value("rotation", local);
        if (scale <= 1e-4 || alpha <= 1e-4) return;
        const double cx = ti->x + ti->w / 2, cy = ti->y + ti->h / 2;
        const double k = ti->w / ti->image.width();
        Aff m = Aff::scale(k, ti->h / ti->image.height()).then(Aff::translate(ti->x + dx, ti->y + dy));
        if (std::abs(scale - 1) > kEps || std::abs(rotation) > kEps) {
            m = m.then(Aff::translate(-cx - dx, -cy - dy)).then(Aff::scale(scale, scale)).then(Aff::rotate(rotation)).then(Aff::translate(cx + dx, cy + dy));
        }
        m = m.then(view_.aff());
        drawLayer(dst, ti->image, r, m, alpha, blendOf(c.blend).value_or(raster::Blend::Normal));
    }

    void drawTrack(Image& frame, const Track& track, double t) {
        std::vector<const Clip*> active;
        for (const auto& c : track.clips) {
            if (!c.visual()) continue;
            if (t >= c.start - kEps && t < c.end() - kEps) active.push_back(&c);
        }
        std::vector<bool> used(active.size(), false);
        for (std::size_t i = 0; i < active.size(); ++i) {
            if (used[i]) continue;
            const Clip& a = *active[i];
            if (a.media()) {
                // is a later clip blending in from this one?
                for (std::size_t j = i + 1; j < active.size(); ++j) {
                    const Clip& b = *active[j];
                    if (used[j] || !b.media() || b.transition.empty() || b.transition == "none") continue;
                    if (tl_.previousMedia(track, b) != &a) continue;
                    const double overlap = a.end() - b.start;
                    if (overlap <= 1e-3 || overlap > std::min(a.length(), b.length()) + kEps) continue;
                    Image la(frame.width(), frame.height()), lb(frame.width(), frame.height());
                    drawClip(la, a, t);
                    drawClip(lb, b, t);
                    const double p = std::clamp((t - b.start) / overlap, 0.0, 1.0);
                    Image mixed = mixTransition(b.transition, la, lb, p);
                    drawLayer(frame, mixed, {0, 0, static_cast<double>(mixed.width()), static_cast<double>(mixed.height())}, Aff{}, 1.0,
                              raster::Blend::Normal);
                    used[i] = used[j] = true;
                    break;
                }
                if (used[i]) continue;
            }
            drawClip(frame, a, t);
            used[i] = true;
        }
    }

    static Image mixTransition(const std::string& type, const Image& a, const Image& b, double p);

private:
    const Timeline& tl_;
    FrameContext::Impl& ctx_;
    View view_;
};

// premultiplied lerp of two straight-alpha pixels
inline void lerpPx(const std::uint8_t* a, const std::uint8_t* b, float t, std::uint8_t* out) {
    const float aa = a[3] / 255.0f * (1 - t), ba = b[3] / 255.0f * t;
    const float oa = aa + ba;
    if (oa <= 0) {
        out[0] = out[1] = out[2] = out[3] = 0;
        return;
    }
    for (int k = 0; k < 3; ++k) out[k] = static_cast<std::uint8_t>(std::lround((a[k] * aa + b[k] * ba) / oa));
    out[3] = static_cast<std::uint8_t>(std::lround(std::min(1.0f, oa) * 255));
}

const std::uint8_t* sampleAt(const Image& img, int x, int y) {
    static const std::uint8_t clear[4] = {0, 0, 0, 0};
    if (x < 0 || y < 0 || x >= img.width() || y >= img.height()) return clear;
    return img.pixel(x, y);
}

Image zoomed(const Image& src, double k) {
    Image out(src.width(), src.height());
    const double cx = src.width() / 2.0, cy = src.height() / 2.0;
    const Aff m = Aff::translate(-cx, -cy).then(Aff::scale(k, k)).then(Aff::translate(cx, cy));
    drawLayer(out, src, {0, 0, static_cast<double>(src.width()), static_cast<double>(src.height())}, m, 1.0, raster::Blend::Normal);
    return out;
}

Image Compositor::mixTransition(const std::string& type, const Image& a, const Image& b, double p) {
    const int w = a.width(), h = a.height();
    Image out(w, h);
    const float pf = static_cast<float>(p);
    auto each = [&](auto fn) {
        raster::parallelRows(h, static_cast<std::size_t>(w) * 2, [&](int y0, int y1) {
            for (int y = y0; y < y1; ++y) {
                for (int x = 0; x < w; ++x) fn(x, y, out.pixel(x, y));
            }
        });
    };
    if (type == "dissolve") {
        each([&](int x, int y, std::uint8_t* o) {
            std::uint32_t hsh = static_cast<std::uint32_t>(x) * 73856093u ^ static_cast<std::uint32_t>(y) * 19349663u;
            hsh ^= hsh >> 13;
            hsh *= 0x5bd1e995u;
            hsh ^= hsh >> 15;
            const float r = (hsh & 0xffff) / 65535.0f;
            const std::uint8_t* s = r < pf ? b.pixel(x, y) : a.pixel(x, y);
            std::copy(s, s + 4, o);
        });
    } else if (type == "fadeblack" || type == "fadewhite") {
        const std::uint8_t v = type == "fadeblack" ? 0 : 255;
        const std::uint8_t c[4] = {v, v, v, 255};
        each([&](int x, int y, std::uint8_t* o) {
            if (pf < 0.5f) lerpPx(a.pixel(x, y), c, pf * 2, o);
            else lerpPx(c, b.pixel(x, y), pf * 2 - 1, o);
        });
    } else if (type.rfind("wipe", 0) == 0 || type == "smoothleft" || type == "smoothright") {
        const bool soft = type.rfind("smooth", 0) == 0;
        const float edge = soft ? 0.15f : 0.0f;
        each([&](int x, int y, std::uint8_t* o) {
            // f: 0 at the leading edge of B, growing into B
            float pos;
            if (type == "wipeleft" || type == "smoothleft") pos = 1 - (x + 0.5f) / w;
            else if (type == "wiperight" || type == "smoothright") pos = (x + 0.5f) / w;
            else if (type == "wipeup") pos = 1 - (y + 0.5f) / h;
            else pos = (y + 0.5f) / h;
            float m;
            if (edge > 0) m = std::clamp((pf * (1 + edge) - pos) / edge, 0.0f, 1.0f);
            else m = pos < pf ? 1.0f : 0.0f;
            lerpPx(a.pixel(x, y), b.pixel(x, y), m, o);
        });
    } else if (type.rfind("slide", 0) == 0 || type.rfind("push", 0) == 0) {
        const bool push = type.rfind("push", 0) == 0;
        const std::string dir = type.substr(push ? 4 : 5);
        int bx = 0, by = 0, ax = 0, ay = 0;
        const double q = 1 - p;
        if (dir == "left") bx = static_cast<int>(std::lround(w * q)), ax = static_cast<int>(std::lround(-w * p));
        else if (dir == "right") bx = static_cast<int>(std::lround(-w * q)), ax = static_cast<int>(std::lround(w * p));
        else if (dir == "up") by = static_cast<int>(std::lround(h * q)), ay = static_cast<int>(std::lround(-h * p));
        else by = static_cast<int>(std::lround(-h * q)), ay = static_cast<int>(std::lround(h * p));
        if (!push) ax = ay = 0;
        each([&](int x, int y, std::uint8_t* o) {
            const std::uint8_t* pa = sampleAt(a, x - ax, y - ay);
            const std::uint8_t* pb = sampleAt(b, x - bx, y - by);
            // B over A
            const float ba = pb[3] / 255.0f, aa = pa[3] / 255.0f;
            const float oa = ba + aa * (1 - ba);
            if (oa <= 0) {
                o[0] = o[1] = o[2] = o[3] = 0;
                return;
            }
            for (int k = 0; k < 3; ++k) o[k] = static_cast<std::uint8_t>(std::lround((pb[k] * ba + pa[k] * aa * (1 - ba)) / oa));
            o[3] = static_cast<std::uint8_t>(std::lround(oa * 255));
        });
    } else if (type == "zoomin" || type == "zoomout") {
        const Image za = type == "zoomin" ? zoomed(a, 1 + p * 0.6) : a;
        const Image zb = type == "zoomout" ? zoomed(b, 1.6 - p * 0.6) : b;
        each([&](int x, int y, std::uint8_t* o) { lerpPx(za.pixel(x, y), zb.pixel(x, y), pf, o); });
    } else if (type == "circleopen" || type == "circleclose") {
        const double rmax = std::hypot(w / 2.0, h / 2.0);
        const double r = (type == "circleopen" ? p : 1 - p) * rmax;
        each([&](int x, int y, std::uint8_t* o) {
            const double d = std::hypot(x + 0.5 - w / 2.0, y + 0.5 - h / 2.0);
            const float inside = static_cast<float>(std::clamp(r - d + 0.5, 0.0, 1.0));
            const float m = type == "circleopen" ? inside : 1 - inside;
            lerpPx(a.pixel(x, y), b.pixel(x, y), m, o);
        });
    } else if (type == "radial") {
        each([&](int x, int y, std::uint8_t* o) {
            double ang = std::atan2(x + 0.5 - w / 2.0, -(y + 0.5 - h / 2.0));  // 0 at 12 o'clock, clockwise
            if (ang < 0) ang += 2 * kPi;
            const float m = ang < p * 2 * kPi ? 1.0f : 0.0f;
            lerpPx(a.pixel(x, y), b.pixel(x, y), m, o);
        });
    } else if (type == "pixelize") {
        const double amount = p < 0.5 ? p * 2 : 2 - p * 2;
        const int block = std::max(1, static_cast<int>(std::lround(amount * std::max(w, h) * 0.05)));
        const Image& src = p < 0.5 ? a : b;
        each([&](int x, int y, std::uint8_t* o) {
            const int sx = std::min(w - 1, (x / block) * block + block / 2), sy = std::min(h - 1, (y / block) * block + block / 2);
            const std::uint8_t* s = src.pixel(sx, sy);
            std::copy(s, s + 4, o);
        });
    } else {  // crossfade, fade, distance and anything older
        each([&](int x, int y, std::uint8_t* o) { lerpPx(a.pixel(x, y), b.pixel(x, y), pf, o); });
    }
    return out;
}

Image renderWith(const Timeline& tl, const FrameRequest& rq, FrameContext::Impl& ctx) {
    const int TW = tl.settings.width, TH = tl.settings.height;
    int w = rq.width, h = rq.height;
    if (w <= 0 && h <= 0) w = TW, h = TH;
    else if (w <= 0) w = std::max(1, static_cast<int>(std::lround(static_cast<double>(h) * TW / TH)));
    else if (h <= 0) h = std::max(1, static_cast<int>(std::lround(static_cast<double>(w) * TH / TW)));
    View view;
    view.w = w;
    view.h = h;
    view.s = rq.reframe == "fill" ? std::max(static_cast<double>(w) / TW, static_cast<double>(h) / TH)
                                  : std::min(static_cast<double>(w) / TW, static_cast<double>(h) / TH);
    view.ox = (w - TW * view.s) / 2;
    view.oy = (h - TH * view.s) / 2;
    Image frame(w, h, rq.transparent ? Color{0, 0, 0, 0} : colourOr(tl.settings.background, {0, 0, 0, 255}));
    if (!rq.transparent) {
        for (int y = 0; y < h; ++y) {
            for (int x = 0; x < w; ++x) frame.pixel(x, y)[3] = 255;
        }
    }
    Compositor comp(tl, ctx, view);
    for (const auto& track : tl.tracks) {
        if (!tl.shows(track)) continue;
        comp.drawTrack(frame, track, rq.t);
    }
    return frame;
}

Image Compositor::renderNested(const Timeline& n, const FrameRequest& rq) { return renderWith(n, rq, ctx_); }

}  // namespace

Image renderFrame(const Timeline& tl, const FrameRequest& rq, FrameContext* context) {
    if (context) return renderWith(tl, rq, context->impl());
    FrameContext local;
    return renderWith(tl, rq, local.impl());
}

std::unique_ptr<Attachment> Timeline::preview() const {
    if (duration() <= 0) return nullptr;
    try {
        FrameRequest rq;
        rq.t = std::clamp(cursor, 0.0, std::max(0.0, duration() - 1.0 / settings.fps));
        rq.width = std::min(settings.width, 960);
        auto& ctx = previewContext();
        std::lock_guard<std::mutex> g(ctx.impl().lock);
        Image img = renderFrame(*this, rq, &ctx);
        auto a = std::make_unique<Attachment>();
        a->mime = "image/png";
        a->bytes = raster::encodePng(img);
        a->name = "timeline.png";
        return a;
    } catch (const std::exception&) {
        return nullptr;
    }
}

}  // namespace wetwaresuite::cut
