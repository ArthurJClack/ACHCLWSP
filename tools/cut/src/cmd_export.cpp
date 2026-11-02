// Seeing and exporting: previews and single frames from the compositor (no
// ffmpeg), and exports with presets, ranges and burned-in captions: frames
// piped to ffmpeg with the mixed sound, stream copy for plain cuts, or WAV
// written directly.
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdlib>
#include <map>
#include <mutex>
#include <thread>

#include "internal.hpp"
#include "media.hpp"
#include "render_internal.hpp"
#include "wetwaresuite/audio/io.hpp"
#include "wetwaresuite/raster/codec.hpp"

namespace wetwaresuite::cut {

namespace fs = std::filesystem;

namespace {

bool imageExt(const std::string& e) {
    return e == "png" || e == "jpg" || e == "jpeg" || e == "webp" || e == "bmp" || e == "tga" || e == "tif" || e == "tiff";
}

std::string missingSource(const Timeline& tl) {
    std::error_code ec;
    for (const auto& t : tl.tracks) {
        for (const auto& c : t.clips) {
            if (c.kind == "nest" && c.nested) {
                if (std::string m = missingSource(*c.nested); !m.empty()) return m;
            } else if (!c.source.empty() && c.kind != "nest" && !fs::is_regular_file(c.source, ec)) {
                return "clip " + c.id + "'s source is missing: " + c.source;
            }
        }
    }
    return {};
}

Result writeFrame(const Timeline& tl, double at, const fs::path& output, int width, bool transparent) {
    if (tl.duration() <= 0) return Result::failure("the timeline is empty");
    if (at < 0 || at > tl.duration()) return Result::failure("t must be between 0 and " + num(tl.duration()) + "s");
    at = std::min(at, std::max(0.0, tl.duration() - 0.5 / tl.settings.fps));
    FrameRequest rq;
    rq.t = at;
    rq.width = width > 0 ? width : 0;
    rq.transparent = transparent;
    FrameContext ctx;
    raster::Image img = renderFrame(tl, rq, &ctx);
    raster::save(img, output);
    Result r = Result::success("saved the frame at " + num(at) + "s to " + output.string() + " (" + std::to_string(img.width()) + "x" +
                                   std::to_string(img.height()) + ")",
                               output.string());
    std::error_code ec;
    return r.with("output", output.string()).with("t", at).with("width", static_cast<std::int64_t>(img.width()))
        .with("height", static_cast<std::int64_t>(img.height())).with("bytes", static_cast<std::int64_t>(fs::file_size(output, ec)));
}

// Exports running in the background (cut.render background=true), watched with cut.export_status.
struct Job {
    std::atomic<double> progress{0};
    std::atomic<bool> cancel{false}, done{false};
    std::mutex lock;
    Result result;  // set when done
    std::string output;
};

std::mutex& jobsLock() {
    static std::mutex m;
    return m;
}
std::map<std::string, std::shared_ptr<Job>>& jobs() {
    static auto* j = new std::map<std::string, std::shared_ptr<Job>>;  // never destroyed: jobs may outlive main
    return *j;
}

std::shared_ptr<Job> findJob(const Params& p) {
    const std::string id = stringParam(p, "job");
    std::lock_guard<std::mutex> g(jobsLock());
    if (id.empty()) {  // the latest one
        std::shared_ptr<Job> last;
        int best = 0;
        for (const auto& [k, j] : jobs()) {
            const int n = std::atoi(k.c_str() + 6);
            if (n > best) best = n, last = j;
        }
        return last;
    }
    auto it = jobs().find(id);
    return it == jobs().end() ? nullptr : it->second;
}

}  // namespace

Result exportTimeline(const Timeline& tl, const fs::path& output, const Params& p, ExportRequest rq, const std::function<bool(double)>& progress) {
    if (std::string m = missingSource(tl); !m.empty()) return Result::failure(m);
    const std::string ext = extOf(output);
    if (std::string err = checkOutput(output, p); !err.empty()) return Result::failure(err);
    if (imageExt(ext)) return writeFrame(tl, numberParam(p, "at", rq.start), output, rq.width, false);
    // captions from a file are drawn by the compositor like any caption
    const Timeline* src = &tl;
    Timeline withCaptions;
    if (!rq.subtitles.empty()) {
        withCaptions = tl;
        addCaptions(withCaptions, parseCaptions(readFile(rq.subtitles)), titleTrack(withCaptions, true), 0, {});
        src = &withCaptions;
    }
    std::error_code ec;
    rq.output = fs::absolute(output, ec).string();
    const double total = src->duration();
    const double from = std::clamp(rq.start, 0.0, total), to = rq.end < 0 ? total : std::min(rq.end, total);
    // WAV needs no encoder: the mix is written directly
    if (ext == "wav" && (rq.preset.empty() || rq.preset == "audio_wav")) {
        if (to <= from + 1e-3) return Result::failure("the range to export is empty");
        if (!hasSound(*src)) return Result::failure("the timeline has no sound to export (every clip is silent or muted)");
        audio::save(mixAudio(*src, from, to), output);
        Result r = written("exported the sound of " + num(from) + "-" + num(to) + "s to " + output.string(), output);
        return r.with("mode", std::string("mix"));
    }
    if (!ffmpegPath()) return Result::failure(missingFfmpeg());
    TempDir tmp;
    rq.listPath = (tmp.path() / "list.txt").string();
    rq.audioPath = (tmp.path() / "mix.wav").string();
    EncodePlan plan;
    std::uintmax_t size = 0;
    std::string hwFallback;
    // a platform's file size limit can take a few tries: each one with less bitrate (gifs: smaller and fewer frames)
    for (int attempt = 0;; ++attempt) {
        try {
            plan = planExport(*src, rq, hasSound(*src));
        } catch (const std::exception& e) {
            return Result::failure(e.what());
        }
        if (attempt == 0 && plan.audio) {
            audio::SaveOptions o;
            o.format = audio::SampleFormat::Float32;
            audio::save(mixAudio(*src, plan.start, plan.start + plan.duration), rq.audioPath, o);
        }
        if (plan.streamCopy) {
            if (!writeFile(rq.listPath, plan.concatList)) return Result::failure("can't write a temporary file");
            if (std::string err = runFfmpeg(plan.args); !err.empty()) return Result::failure(err);
        } else if (!plan.video) {
            if (std::string err = runFfmpeg(plan.args); !err.empty()) return Result::failure(err);
        } else {
            Stream enc;
            std::string err;
            if (!enc.start(plan.args, true, false, err)) return Result::failure(err);
            FrameContext ctx;
            const std::size_t bytes = static_cast<std::size_t>(plan.width) * plan.height * 4;
            bool ok = true;
            for (long i = 0; i < plan.frames && ok; ++i) {
                FrameRequest fr;
                fr.t = plan.start + static_cast<double>(i) / plan.fps;
                fr.width = plan.width;
                fr.height = plan.height;
                fr.reframe = plan.reframe;
                raster::Image img;
                try {
                    img = renderFrame(*src, fr, &ctx);
                } catch (const std::exception& e) {
                    enc.stop();
                    return Result::failure(std::string("frame at ") + num(fr.t) + "s: " + e.what());
                }
                if (img.width() != plan.width || img.height() != plan.height) {
                    raster::Image fixed(plan.width, plan.height, {0, 0, 0, 255});
                    for (int y = 0; y < std::min(plan.height, img.height()); ++y) {
                        std::copy_n(img.pixel(0, y), std::min(plan.width, img.width()) * 4, fixed.pixel(0, y));
                    }
                    img = std::move(fixed);
                }
                ok = enc.write(img.data(), bytes);
                if (progress && !progress((attempt + static_cast<double>(i + 1) / plan.frames) / (attempt + 1))) {
                    enc.stop();
                    fs::remove(output, ec);
                    return Result::failure("export cancelled");
                }
            }
            enc.closeInput();
            const int code = enc.wait();
            if (!ok || code != 0) {
                // a graphics card encoder that tried and failed (busy, out of sessions, old driver): again on the CPU
                if (plan.encoder.rfind("h264_", 0) == 0) {
                    hwFallback = plan.encoder + ": " + enc.error();
                    rq.encoder = "cpu";
                    fs::remove(output, ec);
                    continue;
                }
                return Result::failure("ffmpeg failed: " + enc.error());
            }
        }
        size = fs::file_size(output, ec);
        if (plan.maxBytes <= 0 || static_cast<double>(size) <= plan.maxBytes) break;
        if (attempt == 3) {
            fs::remove(output, ec);
            return Result::failure("couldn't get it under " + num(plan.maxBytes / 1e6) + " MB (the last try was " + num(std::round(size / 1e5) / 10) +
                                   " MB); export a shorter part");
        }
        rq.shrink *= std::clamp(plan.maxBytes / static_cast<double>(size) * 0.9, 0.3, 0.85);
    }
    if (progress) progress(1);
    Result r = written("exported " + output.string() + (plan.streamCopy ? " by stream copy (no re-encoding)" : "") +
                           (plan.maxBytes > 0 ? " (" + num(std::round(size / 1e5) / 10) + " MB, under the " + num(plan.maxBytes / 1e6) + " MB limit)" : ""),
                       output);
    r.with("mode", std::string(plan.streamCopy ? "copy" : "encode"));
    if (!plan.encoder.empty() && !plan.streamCopy) {
        r.with("encoder", plan.encoder);
        if (plan.encoder.rfind("h264_", 0) == 0) r.message += " (encoded on the graphics card: " + plan.encoder + ")";
    }
    if (!hwFallback.empty()) r.with("hw_fallback", hwFallback);
    if (plan.video && !plan.streamCopy) r.with("hw_decode", !hardwareDecodeArgs().empty());
    if (plan.video) r.with("frames", static_cast<std::int64_t>(plan.frames)).with("fps", plan.fps);
    if (plan.maxBytes > 0) r.with("limit_mb", plan.maxBytes / 1e6);
    return r;
}

void registerExportCommands(Session& session) {
    auto& reg = session.registry();

    reg.add({"cut.preview", "Preview Frame",
             "Render the frame at time t (default the playhead) to a PNG and attach it, so you can see the edit: "
             "layers, picture-in-picture, titles, colour, keys, transitions. Drawn by the built-in compositor; only "
             "video files need ffmpeg to decode.",
             {docParam(), {"t", ParamType::Number, "time in seconds (default the playhead)", false},
              {"width", ParamType::Int, "preview width in px", false, {}, std::int64_t{640}}},
             false, true},
            cutCommand(session, [](Timeline& tl, const Params& p) {
                const double at = numberParam(p, "t", tl.cursor);
                if (tl.duration() <= 0) return Result::failure("the timeline is empty");
                if (at < 0 || at >= tl.duration()) return Result::failure("t must be between 0 and " + num(tl.duration()) + "s");
                const int width = static_cast<int>(std::clamp<std::int64_t>(intParam(p, "width", 640), 32, tl.settings.width));
                Result r = Result::success("frame at " + num(at) + "s of " + stringParam(p, "doc"));
                r.attach({"image/png", framePng(tl, at, width), stringParam(p, "doc") + "-" + num(at) + "s.png"});
                return r.with("t", at);
            }, false));

    reg.add({"cut.export_frame", "Export Frame",
             "Save one frame (default the playhead) as an image: .png, .jpg, .webp, .bmp, .tga or .tiff. "
             "transparent=true leaves the background out (PNG/WebP keep it). No ffmpeg needed for stills and titles.",
             {docParam(), {"output", ParamType::String, "image file to write", true},
              {"t", ParamType::Number, "time in seconds (default the playhead)", false},
              {"width", ParamType::Int, "image width (default the timeline's)", false},
              {"transparent", ParamType::Bool, "no background", false, {}, false}, overwriteParam()},
             false},
            cutCommand(session, [](Timeline& tl, const Params& p) {
                const fs::path out = stringParam(p, "output");
                if (!imageExt(extOf(out))) return Result::failure("export a frame as .png, .jpg, .webp, .bmp, .tga or .tiff");
                if (std::string err = checkOutput(out, p); !err.empty()) return Result::failure(err);
                return writeFrame(tl, numberParam(p, "t", tl.cursor), out, static_cast<int>(intParam(p, "width", 0)), boolParam(p, "transparent", false));
            }, false));

    reg.add({"cut.presets", "Export Presets",
             "List the export presets cut.render takes (preset=...): size, shape, codecs, extension and the platform's limits "
             "(length, file size, frame rate).",
             {}, false, true},
            [](const Params&) -> std::unique_ptr<Action> {
                return std::make_unique<FunctionAction>([] {
                    std::string text;
                    std::string names;
                    for (const auto& pr : presets()) {
                        const std::string limits = presetLimits(pr);
                        text += (text.empty() ? "" : "\n") + pr.name + " (." + pr.ext + "): " + pr.description + (limits.empty() ? "" : " [" + limits + "]");
                        names += (names.empty() ? "" : ",") + pr.name;
                    }
                    Result r = Result::success(text);
                    return r.with("presets", names);
                });
            });

    std::vector<std::string> presetNames;
    for (const auto& pr : presets()) presetNames.push_back(pr.name);
    reg.add({"cut.render", "Export",
             "Export the timeline: .mp4/.mov/.mkv (H.264 + AAC), .webm (VP9 + Opus), .gif, sound only (.wav needs no "
             "ffmpeg; .mp3, .m4a, .flac, .ogg) or one frame (.png/.jpg at `at`). preset picks ready-made settings, "
             "including each platform's size, length and file size limits (tiktok, instagram_reels, youtube_shorts, x, "
             "discord under 10 MB, whatsapp, email, gif_discord, gif_small, youtube_1080p, prores, draft, audio_*; "
             "cut.presets lists them with their limits). range=in_out (or start/end) exports a part. Frames come from the "
             "built-in compositor and are encoded by ffmpeg; mode=auto stream-copies when the timeline is only plain "
             "cuts of identical sources (fast, lossless, cuts snap to keyframes).",
             {docParam(), {"output", ParamType::String, "file to write; the extension picks the format", true},
              {"preset", ParamType::String, "ready-made export settings", false, presetNames},
              {"quality", ParamType::String, "encoding effort: draft (fast, small), good, best (slow)", false, {"draft", "good", "best"}, std::string("good")},
              {"mode", ParamType::String, "auto, copy (fail unless stream copy is possible) or encode", false, {"auto", "copy", "encode"}, std::string("auto")},
              {"range", ParamType::String, "all, or in_out (between the in and out points)", false, {"all", "in_out"}, std::string("all")},
              {"start", ParamType::Number, "export from this time", false}, {"end", ParamType::Number, "export up to this time", false},
              {"width", ParamType::Int, "output width (default the preset's or the timeline's)", false},
              {"height", ParamType::Int, "output height", false},
              {"reframe", ParamType::String, "when the shape differs: fit (letterbox) or fill (crop)", false, {"fit", "fill"}},
              {"at", ParamType::Number, "png/jpg: time of the frame", false, {}, 0.0},
              {"subtitles", ParamType::String, "burn the captions of this .srt/.vtt file into the picture", false},
              {"gif_fps", ParamType::Int, "gif: frames per second", false, {}, std::int64_t{15}},
              {"gif_width", ParamType::Int, "gif: width in px", false, {}, std::int64_t{480}},
              {"encoder", ParamType::String,
               "H.264 encoder: auto uses the graphics card (NVIDIA, Intel, AMD, Apple) when ffmpeg can, except at quality=best; cpu always uses libx264",
               false, {"auto", "cpu", "nvenc", "qsv", "amf", "videotoolbox"}, std::string("auto")},
              overwriteParam(),
              {"background", ParamType::Bool,
               "return right away and export in the background; cut.export_status shows how far it got (the window does this)", false, {}, false}},
             false},
            cutCommand(session, [](Timeline& tl, const Params& p) {
                ExportRequest rq;
                rq.preset = stringParam(p, "preset");
                rq.quality = stringParam(p, "quality", "good");
                rq.mode = stringParam(p, "mode", "auto");
                rq.encoder = stringParam(p, "encoder", "auto");
                if (stringParam(p, "range", "all") == "in_out") {
                    if (tl.inPoint < 0 && tl.outPoint < 0) return Result::failure("there are no in/out points (cut.set_in_out sets them)");
                    rq.start = std::max(0.0, tl.inPoint);
                    rq.end = tl.outPoint;
                }
                if (has(p, "start")) rq.start = numberParam(p, "start");
                if (has(p, "end")) rq.end = numberParam(p, "end");
                rq.width = static_cast<int>(intParam(p, "width", 0));
                rq.height = static_cast<int>(intParam(p, "height", 0));
                rq.reframe = stringParam(p, "reframe");
                rq.gifFps = static_cast<int>(intParam(p, "gif_fps", 15));
                rq.gifWidth = static_cast<int>(intParam(p, "gif_width", 480));
                if (has(p, "subtitles")) rq.subtitles = requireInput(p, "subtitles");
                fs::path out = stringParam(p, "output");
                if (out.extension().empty() && !rq.preset.empty()) {
                    if (const Preset* pr = findPreset(rq.preset)) out += "." + pr->ext;
                }
                if (!boolParam(p, "background", false)) return exportTimeline(tl, out, p, rq);
                // a copy of the edit, so it can keep changing while this runs
                auto job = std::make_shared<Job>();
                job->output = out.string();
                std::string id;
                {
                    static int counter = 0;
                    std::lock_guard<std::mutex> g(jobsLock());
                    id = "export" + std::to_string(++counter);
                    jobs()[id] = job;
                }
                std::thread([job, copy = std::make_shared<Timeline>(tl), out, p, rq] {
                    Result r;
                    try {
                        r = exportTimeline(*copy, out, p, rq, [&job](double f) {
                            job->progress = f;
                            return !job->cancel.load();
                        });
                    } catch (const std::exception& e) {
                        r = Result::failure(e.what());
                    }
                    std::lock_guard<std::mutex> g(job->lock);
                    job->result = std::move(r);
                    job->done = true;
                }).detach();
                Result r = Result::success("exporting " + out.string() + " in the background (" + id + "; cut.export_status shows how far it got)", id);
                return r.with("job", id).with("output", out.string());
            }, false));

    reg.add({"cut.export_status", "Export Progress",
             "How far a background export (cut.render background=true) got: progress 0..1, and once done its result "
             "(the file, or why it failed).",
             {{"job", ParamType::String, "the job id cut.render returned (default the latest)", false}}, false, true},
            [](const Params& p) -> std::unique_ptr<Action> {
                return std::make_unique<FunctionAction>([p] {
                    auto job = findJob(p);
                    if (!job) return Result::failure("no background export called '" + stringParam(p, "job") + "'");
                    if (!job->done) {
                        const double f = job->progress;
                        Result r = Result::success("exporting " + job->output + ": " + std::to_string(static_cast<int>(f * 100)) + "%");
                        return r.with("progress", f).with("done", false).with("output", job->output);
                    }
                    std::lock_guard<std::mutex> g(job->lock);
                    Result r = job->result;
                    return r.with("progress", 1.0).with("done", true);
                });
            });

    reg.add({"cut.export_cancel", "Cancel Export", "Stop a background export (cut.render background=true); the half-written file is removed.",
             {{"job", ParamType::String, "the job id (default the latest)", false}}, false, true},
            [](const Params& p) -> std::unique_ptr<Action> {
                return std::make_unique<FunctionAction>([p] {
                    auto job = findJob(p);
                    if (!job) return Result::failure("no background export called '" + stringParam(p, "job") + "'");
                    if (job->done) return Result::success("that export already finished");
                    job->cancel = true;
                    return Result::success("cancelling the export of " + job->output);
                });
            });
}

}  // namespace wetwaresuite::cut
