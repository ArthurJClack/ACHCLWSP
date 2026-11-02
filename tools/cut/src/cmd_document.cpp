// Timeline documents: new, open, save, close, info, settings, tracks, the
// playhead, and probing media. registerCommands (the tool's entry point) is here.
#include <algorithm>
#include <cmath>
#include <cstdio>

#include "internal.hpp"
#include "media.hpp"
#include "render_internal.hpp"
#include "wetwaresuite/playback/commands.hpp"

namespace wetwaresuite::cut {

namespace fs = std::filesystem;

namespace {

class CloseAction : public Action {
public:
    CloseAction(Session& session, std::string id) : session_(session), id_(std::move(id)) {}
    Result apply() override {
        doc_ = session_.workspace().take(id_);
        if (!doc_) return Result::failure("no open document called '" + id_ + "'");
        return Result::success("closed " + id_);
    }
    void revert() override {
        if (doc_) session_.workspace().put(id_, doc_);
    }

private:
    Session& session_;
    std::string id_;
    std::shared_ptr<Document> doc_;
};

Result docCreated(Session& session, std::shared_ptr<Timeline> doc, const std::string& verb) {
    std::string id = session.workspace().add(doc, "cut");
    Result r = Result::success(verb + " " + id + " (" + doc->summary() + ")", id);
    r.with("doc", id)
        .with("width", static_cast<std::int64_t>(doc->settings.width))
        .with("height", static_cast<std::int64_t>(doc->settings.height))
        .with("fps", doc->settings.fps)
        .with("duration", doc->duration());
    return r;
}

std::string trackText(const Track& t) {
    std::string s = t.id + " (" + t.kind;
    if (t.locked) s += ", locked";
    if (t.muted) s += ", muted";
    if (t.hidden) s += ", hidden";
    if (t.solo) s += ", solo";
    if (std::abs(t.volume - 1) > kEps) s += ", volume " + num(t.volume);
    if (!t.duckUnder.empty()) s += ", ducks " + num(t.duckDb) + " dB under " + (t.duckUnder == "auto" ? std::string("the other tracks") : t.duckUnder);
    return s + ")";
}

// Every edit point on the timeline (clip starts and ends), sorted.
std::vector<double> editPoints(const Timeline& tl) {
    std::vector<double> v{0};
    for (const auto& t : tl.tracks) {
        for (const auto& c : t.clips) {
            v.push_back(c.start);
            v.push_back(c.end());
        }
    }
    std::sort(v.begin(), v.end());
    return v;
}

// A new timeline holding one file: a video (with its sound) at its own size and rate,
// a still for five seconds, or a sound on a1.
std::shared_ptr<Timeline> timelineFor(const std::string& path) {
    Clip c;
    try {
        c = clipForFile(path, false);
    } catch (const std::exception& picture) {
        try {
            c = clipForFile(path, true);  // a sound file
        } catch (const std::exception&) {
            throw std::runtime_error(picture.what());
        }
    }
    auto tl = std::make_shared<Timeline>();
    if (c.kind != "audio" && c.sourceWidth > 0 && c.sourceHeight > 0) {
        // even sizes (encoders want them), at most 4K on the long side
        const double shrink = std::min(1.0, 3840.0 / std::max(c.sourceWidth, c.sourceHeight));
        tl->settings.width = std::max(2, static_cast<int>(c.sourceWidth * shrink) / 2 * 2);
        tl->settings.height = std::max(2, static_cast<int>(c.sourceHeight * shrink) / 2 * 2);
    }
    if (c.sourceFps >= 1 && c.sourceFps <= 240) tl->settings.fps = c.sourceFps;
    Params q;
    if (c.kind == "image") q["duration"] = 5.0;
    addClip(*tl, q, c, c.kind == "audio" ? "a1" : "v1", c.kind);
    return tl;
}

}  // namespace

void registerDocumentCommands(Session& session) {
    auto& reg = session.registry();

    reg.add({"cut.new", "New Timeline",
             "Create an empty video timeline and return its id (e.g. cut1). Add clips with cut.add_clip, stills with "
             "cut.add_image, titles with cut.add_title and music with cut.add_audio, then cut.render. Video track v1 is "
             "the bottom layer; v2 and up sit on top (picture-in-picture, titles). A video's sound goes on a linked "
             "audio clip (a1, a2, ...).",
             {{"width", ParamType::Int, "frame width in px (even)", false, {}, std::int64_t{1920}},
              {"height", ParamType::Int, "frame height in px (even); 1920x1080 landscape, 1080x1920 for shorts", false, {}, std::int64_t{1080}},
              {"fps", ParamType::Number, "frames per second", false, {}, 30.0},
              {"background", ParamType::String, "colour behind everything: a name, #rrggbb", false, {}, std::string("black")}},
             false, true},
            [&session](const Params& p) -> std::unique_ptr<Action> {
                return std::make_unique<FunctionAction>([&session, p] {
                    auto tl = std::make_shared<Timeline>();
                    tl->settings.width = static_cast<int>(intParam(p, "width", 1920));
                    tl->settings.height = static_cast<int>(intParam(p, "height", 1080));
                    tl->settings.fps = numberParam(p, "fps", 30);
                    tl->settings.background = stringParam(p, "background", "black");
                    if (std::string err = settingsError(tl->settings); !err.empty()) return Result::failure(err);
                    return docCreated(session, tl, "created");
                });
            });

    reg.add({"cut.open", "Open Timeline",
             "Open a .wwcut timeline saved by cut.save (older .achcut files open too), or a video, image or sound "
             "file as a new timeline with that file on it (the timeline takes the video's size and frame rate). "
             "Returns the document id.",
             {{"path", ParamType::String, "a .wwcut file, or a video, image or sound file to start a timeline with", true}}, false, true},
            [&session](const Params& p) -> std::unique_ptr<Action> {
                return std::make_unique<FunctionAction>([&session, p] {
                    const fs::path path = stringParam(p, "path");
                    std::error_code ec;
                    if (!fs::is_regular_file(path, ec)) return Result::failure("file not found: " + path.string());
                    const std::string ext = extOf(path);
                    try {
                        if (ext == "wwcut" || ext == "achcut") {  // achcut: the name before the rename
                            auto tl = fromJson(readFile(path), fs::absolute(path, ec).parent_path());
                            tl->path = path.string();
                            return docCreated(session, tl, "opened");
                        }
                        return docCreated(session, timelineFor(path.string()), "opened " + path.filename().string() + " as");
                    } catch (const std::exception& e) {
                        return Result::failure(e.what());
                    }
                });
            });

    reg.add({"cut.save", "Save Timeline",
             "Save a timeline as a .wwcut project file (JSON: settings, tracks, clips, keyframes, markers). It refers to "
             "the media files by absolute path; it doesn't copy them. To make a video file, use cut.render.",
             {docParam(), {"path", ParamType::String, "where to save, e.g. edit.wwcut", true}}, false},
            cutCommand(session, [](Timeline& tl, const Params& p) {
                const fs::path path = stringParam(p, "path");
                const std::string ext = extOf(path);
                if (ext != "wwcut" && ext != "achcut") return Result::failure("timelines save as .wwcut; to export a video, use cut.render");
                std::error_code ec;
                if (!path.parent_path().empty()) fs::create_directories(path.parent_path(), ec);
                if (!writeFile(path, toJson(tl))) return Result::failure("can't write " + path.string());
                tl.path = path.string();
                Result r = Result::success("saved " + path.string(), path.string());
                return r.with("path", path.string());
            }, false));

    reg.add({"cut.close", "Close Timeline", "Close a timeline (undo reopens it).", {docParam()}, true},
            [&session](const Params& p) -> std::unique_ptr<Action> {
                const std::string id = stringParam(p, "doc");
                if (!session.workspace().getAs<Timeline>(id)) {
                    return std::make_unique<FunctionAction>([id] { return Result::failure("no open timeline called '" + id + "'"); });
                }
                return std::make_unique<CloseAction>(session, id);
            });

    reg.add({"cut.info", "Timeline Info",
             "Describe a timeline: size, fps, length, the playhead, in/out points, markers, and every track (locked, "
             "muted, hidden, solo) with its clips in order (id, kind, source, place on the timeline, source in/out, "
             "links, keyframes and every changed property, transitions).",
             {docParam()}, false, true},
            cutCommand(session, [](Timeline& tl, const Params& p) {
                char fps[32];
                std::snprintf(fps, sizeof fps, "%g", tl.settings.fps);
                std::string text = stringParam(p, "doc") + ": " + std::to_string(tl.settings.width) + "x" +
                                   std::to_string(tl.settings.height) + " " + fps + "fps, background " +
                                   tl.settings.background + ", " + num(tl.duration()) + "s, playhead " + num(tl.cursor) + "s";
                if (tl.inPoint >= 0 || tl.outPoint >= 0) {
                    text += ", in " + (tl.inPoint >= 0 ? num(tl.inPoint) + "s" : std::string("-")) + " out " +
                            (tl.outPoint >= 0 ? num(tl.outPoint) + "s" : std::string("-"));
                }
                if (std::abs(tl.masterVolume - 1) > kEps) text += ", master volume " + num(tl.masterVolume);
                std::int64_t clips = 0;
                if (tl.tracks.empty()) text += "\nno tracks yet (add clips with cut.add_clip)";
                for (const auto& t : tl.tracks) {
                    text += "\n" + trackText(t);
                    if (t.clips.empty()) text += ": empty";
                    for (const auto& c : t.clips) {
                        text += "\n  " + describe(tl, t, c);
                        ++clips;
                    }
                }
                if (!tl.markers.empty()) {
                    text += "\nmarkers:";
                    for (const auto& m : tl.markers) {
                        text += "\n  " + m.id + " at " + num(m.t) + "s" + (m.duration > 0 ? " for " + num(m.duration) + "s" : std::string()) +
                                " " + m.color + (m.name.empty() ? std::string() : " \"" + m.name + "\"");
                    }
                }
                Result r = Result::success(text);
                return r.with("doc", stringParam(p, "doc"))
                    .with("width", static_cast<std::int64_t>(tl.settings.width))
                    .with("height", static_cast<std::int64_t>(tl.settings.height))
                    .with("fps", tl.settings.fps)
                    .with("duration", tl.duration())
                    .with("cursor", tl.cursor)
                    .with("tracks", static_cast<std::int64_t>(tl.tracks.size()))
                    .with("clips", clips)
                    .with("markers", static_cast<std::int64_t>(tl.markers.size()));
            }, false));

    reg.add({"cut.settings", "Timeline Settings",
             "Change a timeline's frame size, frame rate, background colour or master volume. Clips keep their places; "
             "full-frame clips refit to the new size.",
             {docParam(),
              {"width", ParamType::Int, "frame width in px (even)", false},
              {"height", ParamType::Int, "frame height in px (even)", false},
              {"fps", ParamType::Number, "frames per second", false},
              {"background", ParamType::String, "colour behind everything", false},
              {"master_volume", ParamType::Number, "level of the whole mix: 1 = unchanged (0..4)", false}},
             true},
            cutCommand(session, [](Timeline& tl, const Params& p) {
                if (has(p, "width")) tl.settings.width = static_cast<int>(intParam(p, "width"));
                if (has(p, "height")) tl.settings.height = static_cast<int>(intParam(p, "height"));
                if (has(p, "fps")) tl.settings.fps = numberParam(p, "fps");
                if (has(p, "background")) tl.settings.background = stringParam(p, "background");
                if (has(p, "master_volume")) tl.masterVolume = numberParam(p, "master_volume");
                if (std::string err = settingsError(tl.settings); !err.empty()) return Result::failure(err);
                if (tl.masterVolume < 0 || tl.masterVolume > 4) return Result::failure("master_volume must be 0..4");
                Result r = Result::success("timeline is now " + tl.summary() + (std::abs(tl.masterVolume - 1) > kEps ? ", master volume " + num(tl.masterVolume) : ""));
                return r.with("doc", stringParam(p, "doc"));
            }));

    reg.add({"cut.set_track", "Set Track",
             "Change a whole track: lock it (edits leave it alone), mute its sound, hide its picture, solo it (only "
             "soloed tracks of its kind play) or set its volume. Creates the track if it doesn't exist yet.",
             {docParam(),
              {"track", ParamType::String, "track id: v1, v2, a1, ...", true},
              {"locked", ParamType::Bool, "lock the track: edits that would change it fail", false},
              {"muted", ParamType::Bool, "silence the track", false},
              {"hidden", ParamType::Bool, "don't show the track's picture", false},
              {"solo", ParamType::Bool, "play only soloed tracks (of the same kind)", false},
              {"volume", ParamType::Number, "track volume: 1 = unchanged (0..4)", false},
              {"duck_under", ParamType::String,
               "auto ducking (audio tracks): auto = get quieter while the other audio tracks (voices) play, a track id = under that one, off", false},
              {"duck_db", ParamType::Number, "how far it ducks, in dB (-40..0, default -12)", false}},
             true},
            cutCommand(session, [](Timeline& tl, const Params& p) {
                Track& t = tl.ensureTrack(stringParam(p, "track"));
                if (has(p, "locked")) t.locked = boolParam(p, "locked");
                if (has(p, "muted")) t.muted = boolParam(p, "muted");
                if (has(p, "solo")) t.solo = boolParam(p, "solo");
                if (has(p, "hidden")) {
                    if (t.kind != "video") return Result::failure("only video tracks can be hidden");
                    t.hidden = boolParam(p, "hidden");
                }
                if (has(p, "volume")) {
                    if (t.locked && !has(p, "locked")) return Result::failure("track " + t.id + " is locked; unlock it first");
                    t.volume = numberParam(p, "volume");
                    if (t.volume < 0 || t.volume > 4) return Result::failure("volume must be 0..4");
                }
                if (has(p, "duck_under")) {
                    std::string d = stringParam(p, "duck_under");
                    if (d == "off" || d == "none") d.clear();
                    if (!d.empty() && t.kind != "audio") return Result::failure("only audio tracks duck (music on a2 ducking under a voice on a1)");
                    if (!d.empty() && d != "auto") {
                        const Track* key = tl.track(d);
                        if (!key || key->kind != "audio" || key->id == t.id) return Result::failure("duck_under is auto, off or another audio track's id");
                    }
                    t.duckUnder = d;
                }
                if (has(p, "duck_db")) {
                    t.duckDb = numberParam(p, "duck_db");
                    if (t.duckDb < -40 || t.duckDb > 0) return Result::failure("duck_db must be -40..0");
                }
                Result r = Result::success(trackText(t));
                return r.with("doc", stringParam(p, "doc")).with("track", t.id).with("locked", t.locked).with("muted", t.muted)
                    .with("hidden", t.hidden).with("solo", t.solo).with("volume", t.volume);
            }));

    reg.add({"cut.set_cursor", "Move Playhead",
             "Move the playhead: razor, markers, insert/overwrite and freeze frames use it, and the preview shows its "
             "frame. Give t (seconds), frames (a step from where it is, e.g. -1), or to: start, end, next_edit, "
             "prev_edit, next_marker, prev_marker, in, out.",
             {docParam(), {"t", ParamType::Number, "time in seconds", false},
              {"frames", ParamType::Int, "move by this many frames", false},
              {"to", ParamType::String, "jump to a place", false,
               {"start", "end", "next_edit", "prev_edit", "next_marker", "prev_marker", "in", "out"}}},
             false, true},
            cutCommand(session, [](Timeline& tl, const Params& p) {
                double t = tl.cursor;
                if (has(p, "t")) t = numberParam(p, "t");
                if (has(p, "frames")) t += static_cast<double>(intParam(p, "frames")) / tl.settings.fps;
                if (has(p, "to")) {
                    const std::string to = stringParam(p, "to");
                    const double eps = 0.5 / tl.settings.fps;
                    if (to == "start") t = 0;
                    else if (to == "end") t = tl.duration();
                    else if (to == "in" || to == "out") {
                        const double v = to == "in" ? tl.inPoint : tl.outPoint;
                        if (v < 0) return Result::failure("there's no " + to + " point (cut.set_in_out sets it)");
                        t = v;
                    } else if (to == "next_edit" || to == "prev_edit" || to == "next_marker" || to == "prev_marker") {
                        std::vector<double> pts;
                        if (to == "next_edit" || to == "prev_edit") pts = editPoints(tl);
                        else
                            for (const auto& m : tl.markers) pts.push_back(m.t);
                        std::sort(pts.begin(), pts.end());
                        bool found = false;
                        if (to[0] == 'n') {
                            for (double x : pts)
                                if (!found && x > t + eps) t = x, found = true;
                        } else {
                            for (auto it = pts.rbegin(); it != pts.rend(); ++it)
                                if (!found && *it < t - eps) t = *it, found = true;
                        }
                        if (!found) return Result::failure("there's no " + to.substr(5) + " " + (to[0] == 'n' ? "after" : "before") + " " + num(tl.cursor) + "s");
                    } else {
                        return Result::failure("unknown place '" + to + "'");
                    }
                }
                tl.cursor = std::max(0.0, t);
                Result r = Result::success("playhead at " + num(tl.cursor) + "s", tl.cursor);
                return r.with("doc", stringParam(p, "doc")).with("t", tl.cursor);
            }, false));

    reg.add({"cut.probe", "Probe Media",
             "Ask ffprobe about a media file: duration, container, and every stream with its codec, size, frame rate, "
             "sample rate and channels.",
             {{"path", ParamType::String, "video, audio or image file", true}}, false, true},
            [](const Params& p) -> std::unique_ptr<Action> {
                return std::make_unique<FunctionAction>([p] {
                    try {
                        const std::string path = stringParam(p, "path");
                        MediaInfo m = probe(path);
                        std::string text = path + ": " + m.format + ", " + num(m.duration) + "s";
                        if (m.still) text += " (still image)";
                        int i = 0;
                        for (const auto& s : m.streams) {
                            text += "\n  stream " + std::to_string(i++) + ": " + s.type + " " + s.codec;
                            if (s.type == "video") text += " " + std::to_string(s.width) + "x" + std::to_string(s.height) + " " + num(s.fps) + "fps";
                            if (s.type == "audio") text += " " + std::to_string(s.sampleRate) + "Hz " + std::to_string(s.channels) + "ch";
                        }
                        Result r = Result::success(text);
                        r.with("path", path).with("duration", m.duration).with("format", m.format).with("has_video", m.hasVideo)
                            .with("has_audio", m.hasAudio).with("still", m.still);
                        if (m.hasVideo) {
                            r.with("width", static_cast<std::int64_t>(m.width)).with("height", static_cast<std::int64_t>(m.height))
                                .with("fps", m.fps).with("video_codec", m.videoCodec);
                        }
                        if (m.hasAudio) r.with("audio_codec", m.audioCodec);
                        r.with("streams", static_cast<std::int64_t>(m.streams.size()));
                        return r;
                    } catch (const std::exception& e) {
                        return Result::failure(e.what());
                    }
                });
            });
}

void registerCommands(Session& session) {
    registerDocumentCommands(session);
    registerAddCommands(session);
    registerEditCommands(session);
    registerTrimCommands(session);
    registerSequenceCommands(session);
    registerEffectCommands(session);
    registerTitleCommands(session);
    registerAudioCommands(session);
    registerExportCommands(session);
    registerFileCommands(session);
    registerProxyCommands(session);
}

}  // namespace wetwaresuite::cut
