// Export planning: the presets, the ffmpeg run that encodes composited frames
// (read raw from stdin) with the mixed sound, the concat-demuxer stream copy for
// timelines that are only cuts of identical files, and captions in and out.
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <sstream>
#include <stdexcept>

#include "media.hpp"
#include "wetwaresuite/core/process.hpp"
#include "wetwaresuite/cut/cut.hpp"

namespace wetwaresuite::cut {

namespace fs = std::filesystem;

std::string escapeOption(const std::string& value) {
    std::string out;
    for (char c : value) {
        if (c == '\\' || c == '\'' || c == ':') out += '\\';
        out += c;
    }
    return out;
}

std::string escapeGraph(const std::string& text) {
    std::string out;
    for (char c : text) {
        if (c == '\\' || c == '\'' || c == '[' || c == ']' || c == ',' || c == ';') out += '\\';
        out += c;
    }
    return out;
}

std::string filter(const std::string& name, const std::vector<std::pair<std::string, std::string>>& options) {
    std::string args;
    for (const auto& [k, v] : options) {
        if (!args.empty()) args += ':';
        if (!k.empty()) args += k + "=";
        args += escapeOption(v);
    }
    return args.empty() ? name : name + "=" + escapeGraph(args);
}

const std::vector<Preset>& presets() {
    // the platforms' limits as of 2026, kept a little under the real ones so uploads don't bounce
    auto platform = [](std::string name, std::string desc, std::string ext, int w, int h, std::string reframe, std::string vcodec,
                       std::string acodec, int crf, int fps, double maxSeconds, double maxMB, int maxFps) {
        Preset p{std::move(name), std::move(desc), std::move(ext), w, h, std::move(reframe), std::move(vcodec), std::move(acodec), crf,
                 vcodec == "h264" ? "veryfast" : "", fps};
        p.maxSeconds = maxSeconds;
        p.maxMB = maxMB;
        p.maxFps = maxFps;
        return p;
    };
    static const std::vector<Preset> p = {
        {"youtube_1080p", "YouTube / Vimeo 1080p: H.264 + AAC, 1920x1080", "mp4", 1920, 1080, "fit", "h264", "aac", 18, "veryfast", 0},
        {"youtube_4k", "YouTube 4K: H.264 + AAC, 3840x2160", "mp4", 3840, 2160, "fit", "h264", "aac", 18, "veryfast", 0},
        {"youtube_720p", "720p for quick uploads: H.264 + AAC, 1280x720", "mp4", 1280, 720, "fit", "h264", "aac", 20, "veryfast", 0},
        platform("youtube_shorts", "YouTube Shorts: 1080x1920 (9:16), a landscape edit is cropped to fill", "mp4", 1080, 1920, "fill", "h264", "aac",
                 20, 0, 180, 0, 60),
        platform("tiktok", "TikTok: 1080x1920 (9:16), cropped to fill", "mp4", 1080, 1920, "fill", "h264", "aac", 20, 0, 600, 287, 60),
        platform("instagram_reels", "Instagram Reels: 1080x1920 (9:16), cropped to fill", "mp4", 1080, 1920, "fill", "h264", "aac", 20, 0, 180, 0, 30),
        platform("instagram_story", "Instagram / Facebook Stories: 1080x1920 (9:16), cropped to fill", "mp4", 1080, 1920, "fill", "h264", "aac", 20, 0,
                 60, 0, 30),
        platform("instagram_feed", "Instagram feed post: 1080x1350 (4:5), cropped to fill", "mp4", 1080, 1350, "fill", "h264", "aac", 20, 0, 3600, 0,
                 30),
        platform("facebook", "Facebook feed: 1080x1080 square, cropped to fill", "mp4", 1080, 1080, "fill", "h264", "aac", 20, 0, 14400, 0, 30),
        platform("x", "X (Twitter): 1280x720 H.264 + AAC", "mp4", 1280, 720, "fit", "h264", "aac", 21, 0, 140, 512, 40),
        platform("linkedin", "LinkedIn: 1920x1080 H.264 + AAC", "mp4", 1920, 1080, "fit", "h264", "aac", 20, 0, 900, 5000, 30),
        platform("discord", "Discord without Nitro: 1280x720, squeezed under the 10 MB upload limit", "mp4", 1280, 720, "fit", "h264", "aac", 23, 0, 0,
                 10, 30),
        platform("whatsapp", "WhatsApp: 1280x720, squeezed under the 16 MB limit for videos", "mp4", 1280, 720, "fit", "h264", "aac", 23, 0, 0, 16, 30),
        platform("email", "Email attachment: 1280x720, under 25 MB (Gmail, Outlook)", "mp4", 1280, 720, "fit", "h264", "aac", 23, 0, 0, 25, 30),
        {"vertical", "Any 9:16 phone video: 1080x1920, a landscape edit is cropped to fill", "mp4", 1080, 1920, "fill", "h264", "aac", 20,
         "veryfast", 0},
        {"square", "Square 1080x1080 for feeds, cropped to fill", "mp4", 1080, 1080, "fill", "h264", "aac", 20, "veryfast", 0},
        {"high_quality", "Master copy: H.264 at near-lossless quality (big files), the timeline's size", "mp4", 0, 0, "fit", "h264", "aac", 12,
         "medium", 0},
        {"prores", "ProRes 422 HQ in .mov with uncompressed sound, for handing to other editors", "mov", 0, 0, "fit", "prores", "pcm", 0, "", 0},
        {"webm", "WebM for the web: VP9 + Opus", "webm", 0, 0, "fit", "vp9", "opus", 32, "", 0},
        {"gif", "Animated GIF, 480 px wide at 15 fps, no sound", "gif", 480, 0, "fit", "gif", "none", 0, "", 15},
        platform("gif_discord", "GIF for Discord, Slack or GitHub: 480 px wide, made smaller until it's under 10 MB", "gif", 480, 0, "fit", "gif",
                 "none", 0, 15, 0, 10, 0),
        platform("gif_x", "GIF for X (Twitter): 640 px wide, under 15 MB", "gif", 640, 0, "fit", "gif", "none", 0, 15, 0, 15, 0),
        platform("gif_small", "Small reaction GIF: 320 px wide at 12 fps, under 2 MB", "gif", 320, 0, "fit", "gif", "none", 0, 12, 0, 2, 0),
        {"draft", "Quick review copy: 640 px wide, fast and small", "mp4", 640, 0, "fit", "h264", "aac", 30, "ultrafast", 0},
        {"audio_wav", "Sound only: 16-bit WAV (no ffmpeg needed)", "wav", 0, 0, "fit", "none", "pcm", 0, "", 0},
        {"audio_mp3", "Sound only: MP3", "mp3", 0, 0, "fit", "none", "mp3", 0, "", 0},
        {"audio_m4a", "Sound only: AAC in .m4a", "m4a", 0, 0, "fit", "none", "aac", 0, "", 0},
    };
    return p;
}

const Preset* findPreset(const std::string& name) {
    std::string n;
    for (char c : name) n += c == '-' || c == ' ' ? '_' : static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (n == "shorts" || n == "youtube_short") n = "youtube_shorts";
    if (n == "reels" || n == "instagram" || n == "ig" || n == "ig_reels") n = "instagram_reels";
    if (n == "story" || n == "stories" || n == "instagram_stories") n = "instagram_story";
    if (n == "twitter" || n == "x_twitter") n = "x";
    if (n == "9:16" || n == "vertical_9x16") n = "vertical";
    if (n == "youtube" || n == "1080p") n = "youtube_1080p";
    if (n == "4k" || n == "uhd") n = "youtube_4k";
    if (n == "master") n = "high_quality";
    for (const auto& p : presets()) {
        if (p.name == n) return &p;
    }
    return nullptr;
}

std::string presetLimits(const Preset& p) {
    std::string out;
    auto add = [&out](const std::string& s) { out += (out.empty() ? "" : ", ") + s; };
    if (p.maxSeconds > 0) {
        char buf[32];
        std::snprintf(buf, sizeof buf, "up to %d:%02d", static_cast<int>(p.maxSeconds) / 60, static_cast<int>(p.maxSeconds) % 60);
        add(buf);
    }
    if (p.maxMB > 0) add(std::to_string(static_cast<int>(p.maxMB)) + " MB");
    if (p.maxFps > 0) add(std::to_string(p.maxFps) + " fps");
    return out;
}

namespace {

constexpr double kEps = 1e-6;

std::string t(double v) {
    if (std::abs(v) < 5e-7) return "0";
    char buf[48];
    std::snprintf(buf, sizeof buf, "%.6f", v);
    std::string s = buf;
    while (!s.empty() && s.back() == '0') s.pop_back();
    if (!s.empty() && s.back() == '.') s.pop_back();
    return s;
}

std::string lower(std::string s) {
    for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

std::string extOf(const std::string& path) {
    std::string e = fs::path(path).extension().string();
    if (!e.empty() && e[0] == '.') e.erase(0, 1);
    return lower(e);
}

int even(double v) {
    int i = static_cast<int>(std::lround(v));
    if (i % 2) ++i;
    return std::max(2, i);
}

bool isVideoExt(const std::string& e) { return e == "mp4" || e == "mov" || e == "mkv" || e == "m4v" || e == "webm" || e == "gif"; }
bool isAudioExt(const std::string& e) {
    return e == "mp3" || e == "wav" || e == "m4a" || e == "aac" || e == "flac" || e == "ogg" || e == "opus";
}

std::string clockOf(double seconds) {
    const int s = static_cast<int>(std::ceil(seconds - 1e-6));
    char buf[32];
    std::snprintf(buf, sizeof buf, "%d:%02d", s / 60, s % 60);
    return buf;
}

std::vector<std::string> head() { return {"ffmpeg", "-hide_banner", "-nostats", "-loglevel", "error", "-y"}; }

const std::vector<std::string>& hwNames() {
    static const std::vector<std::string> names = {"nvenc", "qsv", "amf", "videotoolbox"};
    return names;
}

// The H.264 encoder arguments for one encoder name; crf is the libx264-style quality (18 best .. 28 draft).
std::vector<std::string> h264Args(const std::string& enc, int crf, bool draft, const std::string& speed, int width, int height, double fps) {
    const std::string q = std::to_string(crf);
    if (enc == "nvenc") return {"-c:v", "h264_nvenc", "-preset", draft ? "p1" : "p4", "-rc", "vbr", "-cq", q, "-b:v", "0", "-pix_fmt", "yuv420p"};
    if (enc == "qsv") return {"-c:v", "h264_qsv", "-preset", draft ? "veryfast" : "medium", "-global_quality", q, "-pix_fmt", "nv12"};
    if (enc == "amf") return {"-c:v", "h264_amf", "-quality", draft ? "speed" : "balanced", "-rc", "cqp", "-qp_i", q, "-qp_p", q, "-pix_fmt", "yuv420p"};
    if (enc == "videotoolbox") {
        // a bitrate from the picture size: every Mac's encoder takes -b:v, not all take a quality
        const double bitsPerPixel = draft ? 0.05 : crf <= 20 ? 0.12 : 0.08;
        const long kbps = std::max(500L, static_cast<long>(width * static_cast<double>(height) * fps * bitsPerPixel / 1000));
        return {"-c:v", "h264_videotoolbox", "-b:v", std::to_string(kbps) + "k", "-pix_fmt", "yuv420p"};
    }
    return {"-c:v", "libx264", "-preset", speed, "-crf", q, "-pix_fmt", "yuv420p"};
}

}  // namespace

const std::vector<std::string>& hardwareDecodeArgs() {
    static const std::vector<std::string> args = [] {
        const char* env = std::getenv("WETWARESUITE_HW_DECODE");
        const std::string v = env ? lower(env) : "";
        if (v == "off" || v == "none" || v == "cpu" || v == "0") return std::vector<std::string>{};
        auto ffmpeg = ffmpegPath();
        if (!ffmpeg) return std::vector<std::string>{};
        const std::string listed = runProgram({*ffmpeg, "-hide_banner", "-hwaccels"}).output;
        auto has = [&](const char* name) { return listed.find(std::string("\n") + name) != std::string::npos; };
        std::error_code ec;
#if defined(__APPLE__)
        const bool card = has("videotoolbox");
#elif defined(_WIN32)
        const bool card = has("d3d11va") || has("dxva2");
#else
        const bool card = (has("cuda") && fs::exists("/dev/nvidia0", ec)) || (has("vaapi") && fs::exists("/dev/dri/renderD128", ec));
#endif
        if (!card && v != "on") return std::vector<std::string>{};
        return std::vector<std::string>{"-hwaccel", "auto"};
    }();
    return args;
}

std::vector<std::string> hardwareEncoders() {
    static std::once_flag once;
    static std::vector<std::string> found;
    std::call_once(once, [] {
        const char* env = std::getenv("WETWARESUITE_HW_ENCODER");
        const std::string forced = env ? lower(env) : "";
        if (forced == "none" || forced == "cpu" || forced == "off" || forced == "0") return;
        if (!forced.empty()) {
            if (std::find(hwNames().begin(), hwNames().end(), forced) != hwNames().end()) found.push_back(forced);
            return;
        }
        auto ffmpeg = ffmpegPath();
        if (!ffmpeg) return;
#if defined(__APPLE__)
        const std::vector<std::string> tryThese = {"videotoolbox"};
#else
        const std::vector<std::string> tryThese = {"nvenc", "qsv", "amf"};
#endif
        // listed isn't enough (a build can list nvenc with no NVIDIA card): encode a few frames with each
        const ProcessResult listed = runProgram({*ffmpeg, "-hide_banner", "-encoders"});
        for (const auto& name : tryThese) {
            if (listed.output.find("h264_" + name) == std::string::npos) continue;
            std::vector<std::string> args = {*ffmpeg, "-hide_banner", "-nostdin", "-loglevel", "error", "-f", "lavfi", "-i", "color=c=gray:s=640x360:r=30:d=0.2"};
            for (const auto& a : h264Args(name, 23, true, "ultrafast", 640, 360, 30)) args.push_back(a);
            args.insert(args.end(), {"-frames:v", "6", "-f", "null", "-"});
            if (runProgram(args).exitCode == 0) found.push_back(name);
        }
    });
    return found;
}

std::string streamCopyBlocker(const Timeline& tl, const std::string& outExtRaw) {
    const std::string outExt = lower(outExtRaw);
    const Track* video = nullptr;
    const Track* sound = nullptr;
    for (const auto& tr : tl.tracks) {
        if (tr.clips.empty()) continue;
        if (tr.kind == "video") {
            if (video) return "it has more than one video track with clips";
            video = &tr;
        } else {
            if (sound) return "it has more than one audio track with clips";
            sound = &tr;
        }
    }
    if (!video) return tl.tracks.empty() || !sound ? "the timeline is empty" : "there is no picture, only sound";
    if (video->hidden || video->muted || (sound && sound->muted)) return "a track is hidden or muted";
    if (video->volume != 1 || (sound && sound->volume != 1) || tl.masterVolume != 1) return "a track's volume is changed";
    const Clip& f = video->clips.front();
    const std::string srcExt = extOf(f.source);
    auto family = [](const std::string& e) { return (e == "mp4" || e == "mov" || e == "m4v") ? std::string("mp4") : e; };
    if (family(srcExt) != family(outExt)) return "the output format (." + outExt + ") differs from the source (." + srcExt + ")";
    double expected = 0;
    for (const auto& c : video->clips) {
        if (c.kind != "video") return "clip " + c.id + " is a " + c.kind + ", not a video";
        if (c.videoCodec != f.videoCodec || c.audioCodec != f.audioCodec || c.sourceWidth != f.sourceWidth ||
            c.sourceHeight != f.sourceHeight || std::abs(c.sourceFps - f.sourceFps) > 0.01 || c.hasAudio != f.hasAudio) {
            return "clip " + c.id + " comes from a source with different codecs, size or frame rate";
        }
        if (c.sourceWidth != tl.settings.width || c.sourceHeight != tl.settings.height) {
            return "the sources are " + std::to_string(c.sourceWidth) + "x" + std::to_string(c.sourceHeight) + ", not the timeline's " +
                   std::to_string(tl.settings.width) + "x" + std::to_string(tl.settings.height);
        }
        if (c.sourceFps > 0 && std::abs(c.sourceFps - tl.settings.fps) > 0.01) return "the sources' frame rate differs from the timeline's";
        if (std::abs(c.speed - 1) > kEps || c.reverse || c.hold) return "clip " + c.id + " changes speed";
        if (std::abs(c.volume - 1) > kEps || c.mute != f.mute || std::abs(c.pan) > kEps) return "clip " + c.id + " changes its sound";
        if (c.fadeIn > kEps || c.fadeOut > kEps || c.opacity < 1 - kEps || c.blend != "normal") return "clip " + c.id + " has fades or opacity";
        if (c.width > 0 || c.height > 0 || c.cropW > 0 || c.x != 0 || c.y != 0 || c.scale != 1 || c.rotation != 0 || c.cropLeft > 0 ||
            c.cropRight > 0 || c.cropTop > 0 || c.cropBottom > 0 || c.flipH || c.flipV) {
            return "clip " + c.id + " is resized, moved or cropped";
        }
        if (!c.keys.empty()) return "clip " + c.id + " has keyframes";
        if (std::abs(c.brightness) > kEps || std::abs(c.contrast - 1) > kEps || std::abs(c.saturation - 1) > kEps ||
            std::abs(c.gamma - 1) > kEps || c.grayscale || !c.lut.empty() || c.exposure != 0 || c.temperature != 0 || c.tint != 0 ||
            !c.curve.empty() || !c.curveR.empty() || !c.curveG.empty() || !c.curveB.empty() || c.wheelLift != Clip().wheelLift ||
            c.wheelGamma != Clip().wheelGamma || c.wheelGain != Clip().wheelGain) {
            return "clip " + c.id + " has colour correction";
        }
        if (c.blur > 0 || c.sharpen > 0 || c.vignette > 0 || !c.keyColor.empty()) return "clip " + c.id + " has effects";
        if (!c.transition.empty() && c.transition != "none") return "clip " + c.id + " has a transition";
        if (std::abs(c.start - expected) > 1.0 / std::max(1.0, tl.settings.fps)) return "there is a gap or overlap before clip " + c.id;
        expected = c.end();
    }
    if (sound) {
        // only the video clips' own sound, linked one to one
        for (const auto& a : sound->clips) {
            const Clip* v = nullptr;
            for (const auto& c : video->clips) {
                if (!c.link.empty() && c.link == a.link) v = &c;
            }
            if (!v || v->source != a.source || std::abs(v->in - a.in) > 1e-3 || std::abs(v->out - a.out) > 1e-3 || std::abs(v->start - a.start) > 1e-3) {
                return "the sound on " + sound->id + " isn't just the video clips' own sound";
            }
            if (std::abs(a.speed - 1) > kEps || a.reverse || std::abs(a.volume - 1) > kEps || a.mute || a.fadeIn > kEps || a.fadeOut > kEps ||
                !a.keys.empty() || std::abs(a.pan) > kEps || (!a.transition.empty() && a.transition != "none")) {
                return "clip " + a.id + " changes the sound";
            }
        }
        for (const auto& v : video->clips) {
            if (!v.hasAudio || v.ownSound) continue;
            bool found = false;
            for (const auto& a : sound->clips) found = found || (!v.link.empty() && a.link == v.link);
            if (!found) return "clip " + v.id + "'s sound was removed";
        }
    } else {
        for (const auto& v : video->clips) {
            if (v.hasAudio && !v.ownSound) return "clip " + v.id + "'s sound was removed";
        }
    }
    return {};
}

EncodePlan planExport(const Timeline& tl, const ExportRequest& rq, bool hasSound) {
    const Preset* preset = nullptr;
    if (!rq.preset.empty()) {
        preset = findPreset(rq.preset);
        if (!preset) throw std::runtime_error("no preset called '" + rq.preset + "' (cut.presets lists them)");
    }
    const std::string ext = extOf(rq.output);
    if (ext.empty()) throw std::runtime_error("the output needs an extension (.mp4, .mov, .webm, .mkv, .gif, .mp3, .wav, .m4a, ...)");
    if (!isVideoExt(ext) && !isAudioExt(ext)) {
        throw std::runtime_error("can't export to ." + ext + "; use mp4, mov, mkv, webm, gif, mp3, wav, m4a, flac, ogg or opus (cut.export_frame for one picture)");
    }
    if (rq.mode != "auto" && rq.mode != "copy" && rq.mode != "encode") throw std::runtime_error("mode is auto, copy or encode");
    const double total = tl.duration();
    if (total <= kEps) throw std::runtime_error("the timeline is empty; add clips first (cut.add_clip, cut.add_image, cut.add_title)");
    EncodePlan plan;
    plan.start = std::clamp(rq.start, 0.0, total);
    const double end = rq.end < 0 ? total : std::min(rq.end, total);
    if (end <= plan.start + 1e-3) throw std::runtime_error("the range to export is empty (start " + t(plan.start) + "s, end " + t(end) + "s)");
    plan.duration = end - plan.start;
    if (preset && preset->maxSeconds > 0 && plan.duration > preset->maxSeconds + 0.05) {
        throw std::runtime_error(preset->name + " takes videos up to " + clockOf(preset->maxSeconds) + " and this is " + clockOf(plan.duration) +
                                 "; export a part (start/end, or range=in_out) or trim the edit");
    }
    const bool whole = plan.start < kEps && std::abs(end - total) < kEps;

    // stream copy: cuts of identical sources, no re-encoding
    if (isVideoExt(ext) && ext != "gif" && rq.mode != "encode" && rq.subtitles.empty() && !preset && rq.width <= 0 && rq.height <= 0) {
        std::string why = whole ? streamCopyBlocker(tl, ext) : "only a part is exported";
        if (rq.mode == "copy" && !why.empty()) throw std::runtime_error("can't stream-copy: " + why + " (use mode=encode or auto)");
        if (why.empty()) {
            if (rq.listPath.empty()) throw std::runtime_error("stream copy needs a list file path");
            plan.streamCopy = true;
            const Track* track = nullptr;
            for (const auto& tr : tl.tracks) {
                if (!tr.clips.empty() && tr.kind == "video") track = &tr;
            }
            for (const auto& c : track->clips) {
                std::string p = fs::path(c.source).generic_string(), quoted;
                for (char ch : p) {
                    if (ch == '\'') quoted += "'\\''";
                    else quoted += ch;
                }
                plan.concatList += "file '" + quoted + "'\ninpoint " + t(c.in) + "\noutpoint " + t(c.out) + "\n";
            }
            plan.args = head();
            plan.args.insert(plan.args.end(), {"-f", "concat", "-safe", "0", "-i", rq.listPath, "-map", "0:v:0"});
            const Clip& f = track->clips.front();
            if (f.hasAudio && !f.mute) plan.args.insert(plan.args.end(), {"-map", "0:a:0?"});
            plan.args.insert(plan.args.end(), {"-c", "copy"});
            if (ext == "mp4" || ext == "mov" || ext == "m4v") plan.args.insert(plan.args.end(), {"-movflags", "+faststart"});
            plan.args.push_back(rq.output);
            plan.width = tl.settings.width;
            plan.height = tl.settings.height;
            plan.fps = tl.settings.fps;
            return plan;
        }
    }
    if (rq.mode == "copy") throw std::runtime_error("can't stream-copy to ." + ext + " (use mode=encode or auto)");

    const std::string q = rq.quality;
    std::string vcodec, acodec;
    if (isAudioExt(ext)) vcodec = "none";
    else if (ext == "gif") vcodec = "gif";
    else if (ext == "webm") vcodec = "vp9";
    else if (preset && preset->videoCodec == "prores" && ext == "mov") vcodec = "prores";
    else vcodec = "h264";
    if (ext == "gif") acodec = "none";
    else if (ext == "webm" || ext == "ogg" || ext == "opus") acodec = "opus";
    else if (ext == "mp3") acodec = "mp3";
    else if (ext == "wav") acodec = "pcm";
    else if (ext == "flac") acodec = "flac";
    else if (vcodec == "prores") acodec = "pcm";
    else acodec = "aac";
    plan.video = vcodec != "none";
    plan.audio = hasSound && acodec != "none";
    if (!plan.video && !plan.audio) throw std::runtime_error("the timeline has no sound to export (every clip is silent or muted)");

    // size, shape and rate
    int w = rq.width > 0 ? rq.width : preset ? preset->width : 0;
    int h = rq.height > 0 ? rq.height : preset ? preset->height : 0;
    if (vcodec == "gif" && w <= 0 && h <= 0) w = std::max(16, rq.gifWidth);
    const double TW = tl.settings.width, TH = tl.settings.height;
    if (w <= 0 && h <= 0) {
        w = static_cast<int>(TW);
        h = static_cast<int>(TH);
    } else if (h <= 0) {
        h = static_cast<int>(std::lround(w * TH / TW));
    } else if (w <= 0) {
        w = static_cast<int>(std::lround(h * TW / TH));
    }
    if (vcodec == "gif" && rq.shrink < 1) w = static_cast<int>(w * rq.shrink), h = static_cast<int>(h * rq.shrink);
    // a file size limit: the bitrate that fits it, and a smaller picture when that bitrate is low
    const double maxBytes = preset && preset->maxMB > 0 ? preset->maxMB * 1e6 : 0;
    int audioKbps = 0;
    if (maxBytes > 0 && vcodec == "h264") {
        const double budget = maxBytes * 8 / 1000 * 0.92 * std::clamp(rq.shrink, 0.1, 1.0) / plan.duration;  // kbit/s for everything
        audioKbps = hasSound ? (budget < 700 ? 64 : 128) : 0;
        plan.videoKbps = static_cast<int>(budget) - audioKbps;
        if (plan.videoKbps < 120) {
            throw std::runtime_error(preset->name + " allows " + std::to_string(static_cast<int>(preset->maxMB)) + " MB, and " + clockOf(plan.duration) +
                                     " of video can't fit in that; export a shorter part (about " + clockOf(maxBytes * 8 / 1000 * 0.92 * std::clamp(rq.shrink, 0.1, 1.0) / 300) +
                                     " fits)");
        }
        const int longSide = plan.videoKbps < 500 ? 640 : plan.videoKbps < 1000 ? 854 : plan.videoKbps < 2500 ? 1280 : 1920;
        const double ow = w > 0 ? w : TW, oh = h > 0 ? h : TH;
        if (std::max(ow, oh) > longSide) {
            const double k = longSide / std::max(ow, oh);
            w = static_cast<int>(ow * k);
            h = static_cast<int>(oh * k);
        }
    }
    plan.maxBytes = maxBytes;
    plan.width = even(w);
    plan.height = even(h);
    plan.reframe = !rq.reframe.empty() ? rq.reframe : preset ? preset->reframe : "fit";
    if (plan.reframe != "fit" && plan.reframe != "fill") throw std::runtime_error("reframe is fit or fill");
    plan.fps = tl.settings.fps;
    if (vcodec == "gif") plan.fps = preset && preset->fps > 0 ? preset->fps : std::clamp(rq.gifFps, 1, 50);
    else if (preset && preset->fps > 0) plan.fps = preset->fps;
    if (preset && preset->maxFps > 0) plan.fps = std::min(plan.fps, static_cast<double>(preset->maxFps));
    if (vcodec == "gif" && rq.shrink < 1) plan.fps = std::max(8.0, std::round(plan.fps * std::sqrt(rq.shrink)));
    plan.frames = plan.video ? std::max(1L, static_cast<long>(std::ceil(plan.duration * plan.fps - 1e-6))) : 0;

    plan.args = head();
    if (plan.video) {
        plan.args.insert(plan.args.end(), {"-f", "rawvideo", "-pix_fmt", "rgba", "-s", std::to_string(plan.width) + "x" + std::to_string(plan.height), "-r",
                                           t(plan.fps), "-i", "pipe:0"});
    }
    if (plan.audio) plan.args.insert(plan.args.end(), {"-i", rq.audioPath.empty() ? std::string("mix.wav") : rq.audioPath});
    const int audioInput = plan.video ? 1 : 0;
    const bool draft = q == "draft", best = q == "best";
    if (vcodec == "gif") {
        const std::string graph = "[0:v]split[g1][g2];[g1]" + filter("palettegen", {{"stats_mode", "diff"}}) + "[pal];[g2][pal]" +
                                  filter("paletteuse", {{"dither", "bayer"}, {"bayer_scale", "4"}}) + "[out]";
        plan.args.insert(plan.args.end(), {"-filter_complex", graph, "-map", "[out]", "-loop", "0"});
        plan.encoder = "gif";
    } else if (plan.video) {
        plan.args.insert(plan.args.end(), {"-map", "0:v"});
        if (vcodec == "vp9") {
            const int crf = preset ? preset->crf : draft ? 42 : best ? 24 : 33;
            plan.args.insert(plan.args.end(), {"-c:v", "libvpx-vp9", "-crf", std::to_string(crf), "-b:v", "0", "-deadline", draft ? "realtime" : "good",
                                               "-cpu-used", draft ? "8" : best ? "2" : "5", "-row-mt", "1", "-pix_fmt", "yuv420p"});
            plan.encoder = "libvpx-vp9";
        } else if (vcodec == "prores") {
            plan.args.insert(plan.args.end(), {"-c:v", "prores_ks", "-profile:v", "3", "-pix_fmt", "yuv422p10le"});
            plan.encoder = "prores_ks";
        } else {
            const int crf = preset ? preset->crf : draft ? 28 : best ? 18 : 22;
            const std::string speed = preset && !preset->speed.empty() ? preset->speed : draft ? "ultrafast" : best ? "slow" : "veryfast";
            std::string enc = lower(rq.encoder.empty() ? std::string("auto") : rq.encoder);
            if (enc == "auto") {
                const auto hw = best ? std::vector<std::string>{} : hardwareEncoders();
                enc = hw.empty() ? "cpu" : hw.front();
            } else if (enc != "cpu" && std::find(hwNames().begin(), hwNames().end(), enc) == hwNames().end()) {
                throw std::runtime_error("encoder is auto, cpu, nvenc, qsv, amf or videotoolbox");
            }
            const auto args = h264Args(enc, crf, draft, speed, plan.width, plan.height, plan.fps);
            plan.args.insert(plan.args.end(), args.begin(), args.end());
            plan.encoder = args[1];
            if (plan.videoKbps > 0) {
                plan.args.insert(plan.args.end(), {"-maxrate", std::to_string(plan.videoKbps) + "k", "-bufsize", std::to_string(plan.videoKbps) + "k"});
            }
        }
    }
    if (plan.audio) {
        plan.args.insert(plan.args.end(), {"-map", std::to_string(audioInput) + ":a"});
        const std::string rate = audioKbps > 0 ? std::to_string(audioKbps) + "k" : draft ? "128k" : best ? "256k" : "192k";
        if (acodec == "aac") plan.args.insert(plan.args.end(), {"-c:a", "aac", "-b:a", rate});
        else if (acodec == "opus") plan.args.insert(plan.args.end(), {"-c:a", "libopus", "-b:a", rate});
        else if (acodec == "mp3") plan.args.insert(plan.args.end(), {"-c:a", "libmp3lame", "-q:a", draft ? "6" : best ? "0" : "2"});
        else if (acodec == "flac") plan.args.insert(plan.args.end(), {"-c:a", "flac"});
        else plan.args.insert(plan.args.end(), {"-c:a", "pcm_s16le"});
    }
    if (plan.video && (ext == "mp4" || ext == "mov" || ext == "m4v") && vcodec != "prores") plan.args.insert(plan.args.end(), {"-movflags", "+faststart"});
    plan.args.insert(plan.args.end(), {"-t", t(plan.duration), rq.output});
    return plan;
}

// ---- captions ----

namespace {

bool parseTime(const std::string& s, double& out) {
    // [hh:]mm:ss[,.]mmm
    std::vector<double> parts;
    std::string cur;
    for (char c : s) {
        if (c == ':') {
            if (cur.empty()) return false;
            parts.push_back(std::stod(cur));
            cur.clear();
        } else if (c == ',' || c == '.') {
            cur += '.';
        } else if (std::isdigit(static_cast<unsigned char>(c))) {
            cur += c;
        } else if (!std::isspace(static_cast<unsigned char>(c))) {
            return false;
        }
    }
    if (cur.empty()) return false;
    parts.push_back(std::stod(cur));
    if (parts.size() < 2 || parts.size() > 3) return false;
    out = 0;
    for (double p : parts) out = out * 60 + p;
    return true;
}

std::string stripTags(const std::string& s) {
    std::string out;
    bool tag = false;
    for (char c : s) {
        if (c == '<') tag = true;
        else if (c == '>' && tag) tag = false;
        else if (!tag) out += c;
    }
    return out;
}

std::string stamp(double s, char sep) {
    if (s < 0) s = 0;
    const long ms = std::lround(s * 1000);
    char buf[32];
    std::snprintf(buf, sizeof buf, "%02ld:%02ld:%02ld%c%03ld", ms / 3600000, (ms / 60000) % 60, (ms / 1000) % 60, sep, ms % 1000);
    return buf;
}

}  // namespace

std::vector<Cue> parseCaptions(const std::string& text) {
    std::vector<Cue> cues;
    std::istringstream in(text);
    std::string line;
    Cue* cur = nullptr;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.size() >= 3 && static_cast<unsigned char>(line[0]) == 0xEF && static_cast<unsigned char>(line[1]) == 0xBB &&
            static_cast<unsigned char>(line[2]) == 0xBF) {
            line.erase(0, 3);
        }
        const auto arrow = line.find("-->");
        if (arrow != std::string::npos) {
            std::string a = line.substr(0, arrow), b = line.substr(arrow + 3);
            // WebVTT puts cue settings after the end time
            const auto bs = b.find_first_not_of(' ');
            if (bs != std::string::npos) b = b.substr(bs);
            if (auto sp = b.find(' '); sp != std::string::npos) b = b.substr(0, sp);
            Cue c;
            try {
                if (!parseTime(a, c.start) || !parseTime(b, c.end)) continue;
            } catch (const std::exception&) {
                continue;
            }
            cues.push_back(c);
            cur = &cues.back();
            continue;
        }
        if (line.find_first_not_of(" \t") == std::string::npos) {
            cur = nullptr;
            continue;
        }
        if (cur) {
            if (!cur->text.empty()) cur->text += "\n";
            cur->text += stripTags(line);
        }
    }
    cues.erase(std::remove_if(cues.begin(), cues.end(), [](const Cue& c) { return c.text.empty() || c.end <= c.start; }), cues.end());
    if (cues.empty()) throw std::runtime_error("no captions found (expected SubRip .srt or WebVTT .vtt cues like 00:00:01,000 --> 00:00:02,500)");
    std::sort(cues.begin(), cues.end(), [](const Cue& a, const Cue& b) { return a.start < b.start; });
    return cues;
}

std::string writeSrt(const std::vector<Cue>& cues) {
    std::string out;
    int n = 1;
    for (const auto& c : cues) {
        out += std::to_string(n++) + "\n" + stamp(c.start, ',') + " --> " + stamp(c.end, ',') + "\n" + c.text + "\n\n";
    }
    return out;
}

std::string writeVtt(const std::vector<Cue>& cues) {
    std::string out = "WEBVTT\n\n";
    for (const auto& c : cues) out += stamp(c.start, '.') + " --> " + stamp(c.end, '.') + "\n" + c.text + "\n\n";
    return out;
}

}  // namespace wetwaresuite::cut
