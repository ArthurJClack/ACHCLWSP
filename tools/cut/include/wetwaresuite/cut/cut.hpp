// cut: a video editor. A timeline document holds video and audio tracks of
// clips (video files, stills, colour mattes, titles, captions, audio, nested
// sequences) with trims, speed, linked sound, keyframed transforms, colour
// correction, effects, blend modes and transitions. Pictures are composited by
// this library (render.hpp) and sound is mixed by it too; ffmpeg only decodes
// video/odd audio files and encodes the result, so stills, titles, mattes and
// WAV/MP3/FLAC/Ogg sound work without it.
#pragma once

#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "wetwaresuite/core/command.hpp"

namespace wetwaresuite::cut {

struct Settings {
    int width = 1920;
    int height = 1080;
    double fps = 30;
    std::string background = "black";
    bool operator==(const Settings&) const = default;
};

// One keyframe: a property's value at `t` seconds from the clip's start. `ease`
// shapes the way to the next keyframe: linear, ease_in, ease_out, ease_in_out, hold.
struct Key {
    double t = 0;
    double v = 0;
    std::string ease = "linear";
    bool operator==(const Key&) const = default;
};
using Keys = std::map<std::string, std::vector<Key>>;  // property name -> keys sorted by t

class Timeline;

// Everything a clip can be: a video file, a still image, a colour matte, a title, an audio file or a nested sequence.
struct Clip {
    std::string id;     // c1, c2, ...
    std::string kind;   // video, image, color, title, audio, nest
    std::string source; // absolute path (empty for titles, mattes and nests)

    // timing: the clip plays source seconds [in, out) starting at `start` on the timeline
    double in = 0, out = 0, start = 0;
    double speed = 1;
    bool reverse = false;  // plays its source range backwards
    bool hold = false;     // freeze frame: shows the source frame at `in` for its whole length
    std::string link;      // clips sharing a link id (a video and its sound) edit together
    bool ownSound = true;  // video: plays the file's sound itself (false when it sits on a linked audio clip)

    // sound
    double volume = 1;
    bool mute = false;
    double pan = 0;  // -1 left .. 1 right
    double denoise = 0;  // voice cleanup: dB of steady noise (hiss, hum, fans) taken out, 0 = off
    double fadeIn = 0, fadeOut = 0;  // seconds on the timeline (picture and sound)
    double opacity = 1;
    std::string blend = "normal";

    // picture: a box on the frame (0 width = the whole frame) and how the picture fills it, then a transform
    double x = 0, y = 0, width = 0, height = 0;
    std::string fit = "fit";  // fit (letterbox), fill (crop to fill), stretch
    double cropX = 0, cropY = 0, cropW = 0, cropH = 0;  // source pixels, cropW 0 = no crop
    double scale = 1, rotation = 0;                      // around the box centre; rotation in degrees clockwise
    double cropLeft = 0, cropRight = 0, cropTop = 0, cropBottom = 0;  // % of the picture cut off each edge
    bool flipH = false, flipV = false;

    // colour (applied in this order: exposure, white balance, wheels, brightness/contrast/gamma, curves, saturation, LUT)
    double exposure = 0, temperature = 0, tint = 0;
    std::vector<double> wheelLift = {0, 0, 0, 0}, wheelGamma = {0, 0, 0, 0}, wheelGain = {0, 0, 0, 0};  // r, g, b, master
    double brightness = 0, contrast = 1, saturation = 1, gamma = 1;
    bool grayscale = false;
    std::vector<double> curve, curveR, curveG, curveB;  // x0,y0,x1,y1,... in 0..1 (empty = none)
    std::string lut;  // .cube file

    // effects
    double blur = 0, sharpen = 0, vignette = 0;
    std::string keyColor;  // chroma key colour ("" = off), e.g. #00ff00
    double keySimilarity = 0.4, keySoftness = 0.1, keySpill = 0.5;

    // transition from the previous clip on the same track (they overlap by transitionDuration)
    std::string transition;
    double transitionDuration = 0;

    // titles (and the colour of a matte)
    std::string text, font, color = "white", position = "center", boxColor = "black@0.5";
    double fontSize = 72;
    bool box = false, bold = false, italic = false;
    std::string align = "center";
    double outline = 0;  // px
    std::string outlineColor = "black";
    double shadow = 0;   // px offset down-right
    std::string shadowColor = "black@0.6";
    std::string animIn = "none", animOut = "none";
    double animDuration = 0.5;
    std::string style = "plain";  // plain, lower_third
    std::string subtitle;         // lower thirds: the second, smaller line
    std::string accentColor = "#e8a33d";
    bool caption = false;  // part of the captions (exported by cut.export_captions)

    Keys keys;

    // nested sequences: the timeline this clip plays (in/out are its times)
    std::shared_ptr<const Timeline> nested;

    // what the source is (filled by ffprobe or the built-in decoders when added, saved in the document)
    double sourceDuration = 0;
    int sourceWidth = 0, sourceHeight = 0;
    double sourceFps = 0;
    bool hasAudio = false;
    std::string videoCodec, audioCodec;

    double length() const { return speed > 0 ? (out - in) / speed : 0; }
    double end() const { return start + length(); }
    bool visual() const { return kind == "video" || kind == "image" || kind == "title" || kind == "color" || kind == "nest"; }
    // pictures that fill a box and can take transitions
    bool media() const { return kind == "video" || kind == "image" || kind == "color" || kind == "nest"; }
    bool timed() const { return kind == "video" || kind == "audio" || kind == "nest"; }
    bool sound() const;
    // A property at `local` seconds into the clip: its keyframes if it has any, else its fixed value.
    double value(const std::string& property, double local) const;
    // Source time shown `local` seconds into the clip (speed, reverse and hold applied).
    double sourceTime(double local) const;
    bool operator==(const Clip&) const = default;
};

// The properties keyframes can animate.
const std::vector<std::string>& animatableProperties();
// The member a property name stands for (null for an unknown name).
double* propertyField(Clip& c, const std::string& property);
const double* propertyField(const Clip& c, const std::string& property);
const std::vector<std::string>& easeNames();
// Interpolates sorted keys at t (clamped to the first/last key).
double evaluateKeys(const std::vector<Key>& keys, double t);

struct Track {
    std::string id;    // v1, v2 (v1 is at the bottom), a1, a2
    std::string kind;  // video or audio
    bool muted = false;
    bool hidden = false;
    bool locked = false;
    bool solo = false;
    double volume = 1;
    // ducking (audio tracks): drop by duckDb while other sound plays. "" = off, "auto" = under every
    // audio track that isn't ducked itself (the voices), or one track's id
    std::string duckUnder;
    double duckDb = -12;
    std::vector<Clip> clips;  // kept sorted by start
    bool operator==(const Track&) const = default;
};

struct Marker {
    std::string id;  // m1, m2, ...
    double t = 0;
    double duration = 0;
    std::string name;
    std::string color = "green";
    std::string clip;  // a clip marker: moves with this clip (empty = stays at its time)
    bool operator==(const Marker&) const = default;
};

class Timeline : public Document {
public:
    Settings settings;
    std::vector<Track> tracks;  // video tracks bottom-up, then audio tracks
    std::vector<Marker> markers;
    double inPoint = -1, outPoint = -1;  // the sequence in/out (-1 = not set)
    double masterVolume = 1;
    int nextClip = 1, nextLink = 1, nextMarker = 1;
    double cursor = 0;  // the playhead: razor, markers and inserts default to it; previews show it
    std::vector<std::string> bin;  // the media bin: files brought in (cut.bin_add and every file added), to drag onto tracks

    std::string kind() const override { return "timeline"; }
    std::string summary() const override;
    std::unique_ptr<Attachment> preview() const override;
    bool timeline(TimelineLayout& out) const override;
    std::string timelineCommand(const wetwaresuite::TimelineEdit& edit) const override;
    bool clipMedia(const std::string& clipId, MediaStrip& out) const override;  // strips.cpp
    std::unique_ptr<FramePlayer> player() const override;
    std::vector<ClipProperty> clipProperties(const std::string& clipId) const override;  // model.cpp

    double duration() const;
    Track* track(const std::string& id);
    const Track* track(const std::string& id) const;
    // Finds or creates a track named v<n> or a<n>. Throws std::runtime_error for other names.
    Track& ensureTrack(const std::string& id);
    Clip* findClip(const std::string& id, Track** owner = nullptr);
    const Clip* findClip(const std::string& id) const;
    std::string newClipId() { return "c" + std::to_string(nextClip++); }
    std::string newLinkId() { return "L" + std::to_string(nextLink++); }
    void sortClips();
    // End of the last clip on a track (0 when empty or missing).
    double trackEnd(const std::string& id) const;
    // The media clip right before `clip` on its track, if any.
    const Clip* previousMedia(const Track& track, const Clip& clip) const;
    // The clip right before / after `clip` on its track (any kind), if any.
    const Clip* previousClip(const Track& track, const Clip& clip) const;
    // Ids of the clips linked to `clip` (not including it).
    std::vector<std::string> linkedTo(const Clip& clip) const;
    bool anySolo(const std::string& kind) const;
    // Whether a track's pictures / sound are part of the output (hidden, muted and solo applied).
    bool shows(const Track& t) const;
    bool sounds(const Track& t) const;
};

std::string toJson(const Timeline& timeline);
// Throws std::runtime_error with a readable reason.
std::shared_ptr<Timeline> fromJson(const std::string& json, const std::filesystem::path& baseDir = {});

// One undo step: snapshots the timeline, runs the edit, restores it on failure or undo.
// An edit that changes a locked track fails.
class TimelineEdit : public Action {
public:
    TimelineEdit(std::shared_ptr<Timeline> doc, std::function<Result(Timeline&)> fn) : doc_(std::move(doc)), fn_(std::move(fn)) {}
    Result apply() override;
    void revert() override;

private:
    struct Snapshot {
        Settings settings;
        std::vector<Track> tracks;
        std::vector<Marker> markers;
        double inPoint = -1, outPoint = -1, masterVolume = 1;
        int nextClip = 1, nextLink = 1, nextMarker = 1;
        std::vector<std::string> bin;
    };
    std::shared_ptr<Timeline> doc_;
    std::function<Result(Timeline&)> fn_;
    Snapshot before_;
};

// What ffprobe says about a media file.
struct StreamInfo {
    std::string type;  // video, audio, subtitle, data
    std::string codec;
    int width = 0, height = 0;
    double fps = 0;
    int sampleRate = 0, channels = 0;
};

struct MediaInfo {
    double duration = 0;
    std::string format;
    bool hasVideo = false, hasAudio = false, still = false;
    int width = 0, height = 0;
    double fps = 0;
    std::string videoCodec, audioCodec;
    std::vector<StreamInfo> streams;
};

// Parses `ffprobe -print_format json -show_format -show_streams` output. Throws on bad JSON.
MediaInfo parseProbe(const std::string& json);
// Runs ffprobe. Throws std::runtime_error (including "install ffmpeg" when it is missing).
MediaInfo probe(const std::filesystem::path& path);

// ---- export ----

// A ready-made set of export settings (cut.presets lists them).
struct Preset {
    std::string name, description;
    std::string ext;        // default extension
    int width = 0, height = 0;  // 0 = the timeline's
    std::string reframe = "fit";  // when the shape differs: fit (letterbox) or fill (crop)
    std::string videoCodec;  // h264, vp9, prores, gif, none
    std::string audioCodec;  // aac, opus, pcm, mp3, none
    int crf = 22;
    std::string speed = "veryfast";
    int fps = 0;  // 0 = the timeline's
    // a platform's upload limits (0 = none): longer edits are refused, and a file size limit
    // sets the bitrate (and shrinks big pictures) so the file fits
    double maxSeconds = 0;
    double maxMB = 0;  // megabytes of 1,000,000 bytes
    int maxFps = 0;    // the timeline's rate, but no faster than this
};
const std::vector<Preset>& presets();
const Preset* findPreset(const std::string& name);

struct ExportRequest {
    std::string output;            // file to write; its extension picks the format
    std::string preset;            // empty = from the extension and quality
    std::string quality = "good";  // draft, good, best
    std::string mode = "auto";     // auto, copy (stream copy, no re-encode), encode
    double start = 0, end = -1;    // the range to export (end < 0 = the end)
    int width = 0, height = 0;     // 0 = the preset's or the timeline's
    std::string reframe;           // fit or fill (empty = the preset's)
    std::string subtitles;         // an .srt/.vtt file burned in on top
    int gifFps = 15, gifWidth = 480;
    std::string listPath;          // where the caller will write the concat list for stream copy
    std::string audioPath;         // where the caller writes the mixed sound (WAV) for encoding
    double shrink = 1;             // a retry after missing a file size limit: less bitrate (gif: smaller, fewer frames)
    // H.264 encoder: auto (the graphics card's when ffmpeg can use it, except quality=best), cpu (libx264),
    // or one of nvenc, qsv, amf, videotoolbox
    std::string encoder = "auto";
};

// The ffmpeg run that encodes composited frames read from stdin (rawvideo rgba) plus a mixed WAV.
struct EncodePlan {
    std::vector<std::string> args;  // argv; args[0] is "ffmpeg"
    int width = 0, height = 0;
    double fps = 30;
    double start = 0, duration = 0;
    long frames = 0;
    bool audio = false;     // reads the mix from audioPath
    bool video = true;
    std::string reframe = "fit";
    bool streamCopy = false;
    std::string concatList;
    double maxBytes = 0;  // the preset's file size limit, checked after encoding
    int videoKbps = 0;    // the bitrate cap that limit set (0 = none)
    std::string encoder;  // the ffmpeg video encoder used ("libx264", "h264_nvenc", ...; empty without video)
};
// The hardware H.264 encoders that work here, best first ("nvenc", "qsv", "amf", "videotoolbox"):
// each is tried once on a tiny clip and remembered. WETWARESUITE_HW_ENCODER=none turns them off,
// or names one to use without trying it.
std::vector<std::string> hardwareEncoders();
// ffmpeg arguments that decode on the graphics card when this machine has a hardware decoder ffmpeg
// knows (VideoToolbox, D3D11VA/DXVA2, CUDA, VA-API): {"-hwaccel", "auto"}, which falls back to the
// CPU by itself if the card can't take a file. Empty otherwise, or with WETWARESUITE_HW_DECODE=off.
const std::vector<std::string>& hardwareDecodeArgs();
// "TikTok: up to 10:00, 287 MB, 30 fps" (empty when the preset has no limits).
std::string presetLimits(const Preset& preset);
// Plans an export. Pure: touches no files. Throws std::runtime_error with a readable reason.
EncodePlan planExport(const Timeline& timeline, const ExportRequest& request, bool hasSound);
// Why the timeline can't be rendered by stream copy to this extension, or empty if it can.
std::string streamCopyBlocker(const Timeline& timeline, const std::string& outputExt);

// ffmpeg filter option escaping (both levels: option value, then filtergraph).
std::string escapeOption(const std::string& value);
std::string escapeGraph(const std::string& text);
// name=k1=v1:k2=v2 with every value escaped for use inside -filter_complex.
std::string filter(const std::string& name, const std::vector<std::pair<std::string, std::string>>& options = {});

// A TrueType/OpenType font file, or empty if none was found.
std::string findFont();

const std::vector<std::string>& transitionNames();
bool knownTransition(const std::string& type);
const std::vector<std::string>& blendModeNames();
const std::vector<std::string>& titleAnimations();

// ---- captions ----
struct Cue {
    double start = 0, end = 0;
    std::string text;
};
// Reads SubRip (.srt) or WebVTT (.vtt) text. Throws std::runtime_error when nothing parses.
std::vector<Cue> parseCaptions(const std::string& text);
std::string writeSrt(const std::vector<Cue>& cues);
std::string writeVtt(const std::vector<Cue>& cues);

void registerCommands(Session& session);

}  // namespace wetwaresuite::cut
