// Helpers every cut.* command file shares: params, property application and
// checks, the undoable-edit wrapper, linked groups, ripples and splits.
#pragma once

#include <filesystem>
#include <functional>
#include <set>
#include <string>
#include <vector>

#include "wetwaresuite/cut/cut.hpp"

namespace wetwaresuite::cut {

constexpr double kEps = 1e-6;

std::string num(double v);
std::string lower(std::string s);
std::string extOf(const std::filesystem::path& p);
bool has(const Params& p, const std::string& key);

ParamSpec docParam();
ParamSpec clipParam(const std::string& what = "clip id, e.g. c2 (cut.info lists them)");
ParamSpec previewParam();
ParamSpec overwriteParam();
ParamSpec linkedParam();

// Clip properties as params; which ones a command offers is picked by name.
std::vector<ParamSpec> propSpecs(const std::vector<std::string>& names);
const std::vector<std::string>& allPropNames();
extern const std::vector<std::string> kPictureProps;
extern const std::vector<std::string> kColourProps;
extern const std::vector<std::string> kEffectProps;
extern const std::vector<std::string> kTitleProps;
// Applies whichever properties are present. Returns an error, empty when fine.
std::string applyProps(Clip& c, const Params& p);
// Why a clip is broken, empty when it's fine.
std::string validate(const Clip& c);
std::string describe(const Timeline& tl, const Track& track, const Clip& c);

using Edit = std::function<Result(Timeline&, const Params&)>;
// A command on an open timeline: undoable edits run inside a TimelineEdit.
ActionFactory cutCommand(Session& session, Edit edit, bool undoable = true);
// The reply to an edit of one clip: its id, place and the timeline's length, plus a preview if asked.
Result clipResult(const std::string& message, Timeline& tl, const Params& p, const std::string& clipId);
Result& maybePreview(Result& r, const Timeline& tl, const Params& p, double at);
// One frame as PNG bytes.
std::string framePng(const Timeline& tl, double at, int width);
Clip* requireClip(Timeline& tl, const Params& p, Track** owner = nullptr, const std::string& key = "clip");
Track* trackOf(Timeline& tl, const std::string& clipId);
void fillFromProbe(Clip& c, const MediaInfo& m);
std::string absolute(const std::string& path);
std::string requireInput(const Params& p, const char* key = "input");
std::string checkOutput(const std::filesystem::path& output, const Params& p);
Result written(const std::string& message, const std::filesystem::path& output);
std::vector<std::string> splitList(const std::string& text);
std::string settingsError(const Settings& s);

// ---- media ----

// A clip for a file: video (with its sound), image or audio, measured with the built-in
// decoders when they can (stills, WAV/MP3/FLAC/Ogg) and ffprobe otherwise.
Clip clipForFile(const std::string& path, bool wantAudio);
// Adds a clip to a track at its end (or start=), checking it and giving it an id.
Result addClip(Timeline& tl, const Params& p, Clip c, const std::string& defaultTrack, const std::string& what);
// A video file's sound as a linked clip on an audio track. Returns the audio clip's id (empty when it has none).
std::string addLinkedSound(Timeline& tl, Clip& video, const std::string& audioTrack);

// ---- edits ----

// The clip and, when `linked`, every clip linked to it (pointers stay valid until clips are added or removed).
std::vector<Clip*> group(Timeline& tl, const Clip& c, bool linked);
// Moves every clip on `track` that starts at or after `from` by `delta` seconds (skipping `skip`).
void rippleTrack(Track& track, double from, double delta, const std::set<std::string>& skip = {});
// Cuts a clip in two at a timeline time. The second part gets a new id (returned); fades,
// keyframes and transitions stay with the right part.
std::string splitClip(Timeline& tl, Track& track, const std::string& clipId, double at);
// Removes a clip and clears a transition that blended from it.
void removeClip(Timeline& tl, Track& track, const std::string& clipId);
// Removes [from, to) from a track: clips inside go, clips across the edges are trimmed or split.
// Returns the right-hand parts that were split off (with the link they came from) for relinkParts.
std::vector<std::pair<std::string, std::string>> clearRange(Timeline& tl, Track& track, double from, double to);
// Moves a clip's keyframes by `delta` seconds (after its start moved but its media stayed).
void shiftKeys(Clip& c, double delta);
// Checks a timing change keeps the clip inside its source.
std::string timingError(const Clip& c);
// Gives the clips linked to `before` the same timing change it went through to `after` (start,
// in and out moved by as much, speed and reverse copied). Throws when one ends up broken.
void followLinked(Timeline& tl, const Clip& before, const Clip& after);
// After splitting linked clips: the right-hand parts (pairs of the link they came from and their
// id) are linked among themselves with a fresh link, or unlinked when alone.
void relinkParts(Timeline& tl, const std::vector<std::pair<std::string, std::string>>& parts);
// The tracks an edit may touch: `list` (comma separated ids) or every unlocked track.
std::vector<std::string> targetTracks(const Timeline& tl, const std::string& list);
// Seconds a clip overlaps the one before it through its transition (0 when none).
double incomingOverlap(const Timeline& tl, const Track& track, const Clip& c);
// Moves a clip's edges, keeping what's shown in place: trimEnd changes the length at the end by
// `d` seconds (more = longer); trimStart moves the start later by `d` (shorter) and keeps the rest
// where it was. Speed, reverse and freeze frames are taken into account; keyframes stay with the media.
void trimEnd(Clip& c, double d);
void trimStart(Clip& c, double d);
// The clip right after / before `c` on its track that touches it (adjacent or overlapping through a transition).
Clip* nextTouching(Track& track, const Clip& c);
Clip* prevTouching(Track& track, const Clip& c);
// The top video track holding only titles (captions: only captions), or a new track above everything.
std::string titleTrack(Timeline& tl, bool captions);

// ---- captions (cmd_titles.cpp) ----

// A caption title: bottom of the frame, boxed, sized for the frame (no id yet).
Clip makeCaption(const Timeline& tl, const std::string& text, double start, double end);
// Adds the cues as caption clips on `track` (made if needed), offset by `offset` seconds, with the
// style params of `p` (font_size, color, ...). Returns how many were added.
int addCaptions(Timeline& tl, const std::vector<Cue>& cues, const std::string& track, double offset, const Params& p);

// ---- exporting (cmd_export.cpp) ----

// Writes the timeline (or part of it) to a file: frames from the compositor and the mixed sound
// piped to ffmpeg, stream copy for plain cuts, or a WAV written directly with no ffmpeg at all.
// progress (0..1) is told how far it got; returning false from it cancels the export.
Result exportTimeline(const Timeline& tl, const std::filesystem::path& output, const Params& p, ExportRequest rq,
                      const std::function<bool(double)>& progress = {});

// ---- registration, per file ----
void registerDocumentCommands(Session& session);
void registerAddCommands(Session& session);
void registerEditCommands(Session& session);
void registerTrimCommands(Session& session);
void registerSequenceCommands(Session& session);
void registerEffectCommands(Session& session);
void registerTitleCommands(Session& session);
void registerAudioCommands(Session& session);
void registerExportCommands(Session& session);
void registerFileCommands(Session& session);
void registerProxyCommands(Session& session);

}  // namespace wetwaresuite::cut
