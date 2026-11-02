// Putting media on the timeline: video files (with their sound linked on an
// audio track), stills, sound files and colour mattes.
#include <algorithm>

#include "internal.hpp"

namespace wetwaresuite::cut {

namespace {

ParamSpec audioTrackParam() {
    return {"audio_track", ParamType::String,
            "where a video's sound goes: auto (the first free audio track, linked to the picture), a1/a2/..., "
            "embedded (the video clip plays it itself), or none (drop it)", false, {}, std::string("auto")};
}

}  // namespace

void registerAddCommands(Session& session) {
    auto& reg = session.registry();
    {
        std::vector<ParamSpec> s{docParam(), {"path", ParamType::String, "video file (a still image works too)", true},
                                 {"track", ParamType::String, "video track: v1 (bottom, default), v2, ...", false, {}, std::string("v1")},
                                 audioTrackParam()};
        for (auto& x : propSpecs({"start", "in", "out", "duration", "speed", "reverse", "volume", "mute"})) s.push_back(x);
        for (auto& x : propSpecs(kPictureProps)) s.push_back(x);
        s.push_back(previewParam());
        reg.add({"cut.add_clip", "Add Clip",
                 "Put a video file on the timeline. By default it goes at the end of track v1 and plays the whole "
                 "file; in/out pick a part of the source, start places it. Its sound goes on a linked clip on the "
                 "first free audio track (a1, a2, ...), so moving, trimming or cutting one changes both (cut.unlink "
                 "separates them). For picture-in-picture put it on v2 with width (and x, y) or use cut.transform. "
                 "Returns the clip id, e.g. c1, and the sound clip's id as `sound`.",
                 s, true},
                cutCommand(session, [](Timeline& tl, const Params& p) {
                    Clip c = clipForFile(stringParam(p, "path"), false);
                    return addClip(tl, p, c, "v1", c.kind == "image" ? "still" : "clip");
                }));
    }
    {
        std::vector<ParamSpec> s{docParam(), {"path", ParamType::String, "image file (png, jpg, webp, bmp, tga, ...)", true},
                                 {"track", ParamType::String, "video track", false, {}, std::string("v1")},
                                 {"duration", ParamType::Number, "how long it shows, in seconds", false, {}, 5.0}};
        for (auto& x : propSpecs({"start"})) s.push_back(x);
        for (auto& x : propSpecs(kPictureProps)) s.push_back(x);
        for (auto& x : propSpecs({"brightness", "contrast", "saturation", "grayscale"})) s.push_back(x);
        s.push_back(previewParam());
        reg.add({"cut.add_image", "Add Image",
                 "Put a still image on the timeline for a while (a photo, a slide, a logo; PNG transparency is kept). "
                 "Same placing and picture options as cut.add_clip. Needs no ffmpeg for PNG, JPEG, BMP, TGA and the "
                 "other formats the photo tool reads.",
                 s, true},
                cutCommand(session, [](Timeline& tl, const Params& p) {
                    Clip c = clipForFile(stringParam(p, "path"), false);
                    if (c.kind != "image") throw std::runtime_error(stringParam(p, "path") + " isn't an image (use cut.add_clip for video)");
                    Params q = p;
                    if (!has(q, "duration")) q["duration"] = 5.0;
                    return addClip(tl, q, c, "v1", "image");
                }));
    }
    {
        std::vector<ParamSpec> s{docParam(), {"path", ParamType::String, "audio file (or a video, for its sound)", true},
                                 {"track", ParamType::String, "audio track: a1 (default), a2, ...", false, {}, std::string("a1")}};
        for (auto& x : propSpecs({"start", "in", "out", "duration", "volume", "pan", "fade_in", "fade_out", "speed", "reverse"})) s.push_back(x);
        reg.add({"cut.add_audio", "Add Audio",
                 "Put music, a voice-over or sound effects on an audio track. By default it starts at the end of the "
                 "track (0 for the first); in/out and duration trim it, volume sets the level (0.3 for background "
                 "music), pan places it, fade_in/fade_out ease it. WAV, MP3, FLAC and Ogg Vorbis need no ffmpeg.",
                 s, true},
                cutCommand(session, [](Timeline& tl, const Params& p) {
                    Clip c = clipForFile(stringParam(p, "path"), true);
                    return addClip(tl, p, c, "a1", "audio");
                }));
    }
    {
        std::vector<ParamSpec> s{docParam(), {"color", ParamType::String, "the colour: a name, #rrggbb, or with alpha like black@0.5", false, {}, std::string("black")},
                                 {"track", ParamType::String, "video track", false, {}, std::string("v1")},
                                 {"duration", ParamType::Number, "how long it shows, in seconds", false, {}, 3.0}};
        for (auto& x : propSpecs({"start"})) s.push_back(x);
        for (auto& x : propSpecs(kPictureProps)) s.push_back(x);
        s.push_back(previewParam());
        reg.add({"cut.add_color", "Add Colour Matte",
                 "Put a solid colour on the timeline: a black slate, a coloured background for titles, or (with width, "
                 "height and opacity) a box behind something. Works with transitions like any clip.",
                 s, true},
                cutCommand(session, [](Timeline& tl, const Params& p) {
                    Clip c;
                    c.kind = "color";
                    c.color = stringParam(p, "color", "black");
                    c.out = 3;
                    Params q = p;
                    if (!has(q, "duration")) q["duration"] = 3.0;
                    q.erase("color");
                    return addClip(tl, q, c, "v1", "matte");
                }));
    }

    // ---- the media bin ----
    reg.add({"cut.bin_add", "Import Media",
             "Bring a video, picture or sound file into the media bin without putting it on the timeline (files added "
             "with cut.add_* land there too). The window shows the bin above the timeline; drag a file from it onto a track.",
             {docParam(), {"path", ParamType::String, "the file", true}},
             true},
            cutCommand(session, [](Timeline& tl, const Params& p) {
                const std::string path = requireInput(p, "path");
                const std::string full = absolute(path);
                if (std::find(tl.bin.begin(), tl.bin.end(), full) != tl.bin.end()) return Result::success(full + " is already in the bin");
                tl.bin.push_back(full);
                Result r = Result::success("added " + std::filesystem::path(full).filename().string() + " to the media bin (" +
                                           std::to_string(tl.bin.size()) + " file(s))");
                return r.with("path", full).with("count", static_cast<std::int64_t>(tl.bin.size()));
            }));
    reg.add({"cut.bin_remove", "Remove From Bin", "Take a file out of the media bin (clips already on the timeline stay).",
             {docParam(), {"path", ParamType::String, "the file, as cut.bin lists it", true}},
             true},
            cutCommand(session, [](Timeline& tl, const Params& p) {
                const std::string path = stringParam(p, "path");
                auto it = std::find(tl.bin.begin(), tl.bin.end(), path);
                if (it == tl.bin.end()) it = std::find(tl.bin.begin(), tl.bin.end(), absolute(path));
                if (it == tl.bin.end()) return Result::failure(path + " isn't in the bin (cut.bin lists it)");
                tl.bin.erase(it);
                return Result::success("removed " + path + " from the bin");
            }));
    reg.add({"cut.bin", "Media Bin",
             "List the media bin: every file brought in plus any other file a clip plays. rows: path, kind (video, "
             "image, audio), clips using it.",
             {docParam()},
             false, true},
            cutCommand(
                session,
                [](Timeline& tl, const Params&) {
                    TimelineLayout layout;
                    tl.timeline(layout);
                    std::string rows, text;
                    for (const auto& m : layout.media) {
                        int uses = 0;
                        std::string kind;
                        for (const auto& t : tl.tracks)
                            for (const auto& c : t.clips)
                                if (c.source == m) {
                                    ++uses;
                                    if (kind.empty() || c.kind == "video") kind = c.kind;
                                }
                        if (kind.empty()) {
                            const std::string e = extOf(m);
                            static const char* sounds[] = {"wav", "mp3", "flac", "ogg", "oga", "opus", "m4a", "aac", "wma", "aif", "aiff"};
                            static const char* stills[] = {"png", "jpg", "jpeg", "webp", "bmp", "tga", "tif", "tiff", "psd"};
                            kind = std::find(std::begin(sounds), std::end(sounds), e) != std::end(sounds)   ? "audio"
                                   : std::find(std::begin(stills), std::end(stills), e) != std::end(stills) ? "image"
                                                                                                            : "video";
                        }
                        rows += m + "\t" + kind + "\t" + std::to_string(uses) + "\n";
                        text += "\n  " + std::filesystem::path(m).filename().string() + " (" + kind + (uses ? ", on the timeline" : "") + ")";
                    }
                    Result r = Result::success(layout.media.empty() ? "the media bin is empty" : std::to_string(layout.media.size()) + " file(s) in the bin:" + text);
                    return r.with("rows", rows).with("count", static_cast<std::int64_t>(layout.media.size()));
                },
                false));
}

}  // namespace wetwaresuite::cut
