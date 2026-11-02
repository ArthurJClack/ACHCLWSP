// A multitrack project: tracks holding clips at time offsets, per-track gain,
// pan, mute, solo, an effects rack, automation (volume envelope, pan and
// effect settings), automatic ducking under another track, a master rack and
// markers, mixed down to one Buffer. Clips share their audio (copy a project
// freely, e.g. for undo snapshots).
#pragma once

#include <algorithm>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "wetwaresuite/audio/buffer.hpp"
#include "wetwaresuite/audio/dsp.hpp"
#include "wetwaresuite/audio/rack.hpp"
#include "wetwaresuite/raster/image.hpp"

namespace wetwaresuite::audio {

struct Clip {
    std::string name;
    std::shared_ptr<const Buffer> audio;  // already at the project's rate
    double at = 0;        // seconds from the project start
    double gainDb = 0;
    double fadeIn = 0;    // seconds
    double fadeOut = 0;
    FadeCurve fadeInCurve = FadeCurve::SCurve;
    FadeCurve fadeOutCurve = FadeCurve::SCurve;
    // the part of `audio` that plays (trimming is non-destructive): from offset, for length seconds
    double offset = 0;
    double length = -1;  // < 0 = to the end of the audio
    double sourceSeconds() const { return audio ? audio->seconds() : 0.0; }
    double seconds() const {
        const double rest = std::max(0.0, sourceSeconds() - offset);
        return length < 0 ? rest : std::min(length, rest);
    }
    double end() const { return at + seconds(); }
};

struct EnvelopePoint {
    double time = 0;    // seconds
    double gainDb = 0;  // linear-in-dB between points, held before the first and after the last
};

// A send: part of a track's sound (after its volume and pan) also goes to a bus.
struct Send {
    int bus = 0;
    double levelDb = 0;
};

// A shared effects bus (a reverb or delay several tracks feed): the sends come in,
// run through its rack, and the result joins the mix before the master.
struct Bus {
    std::string name;
    double gainDb = 0;
    bool mute = false;
    Rack fx;
    std::vector<Automation> automation;  // volume and fx<i>.<setting> lanes
};

struct Track {
    std::string name;
    double gainDb = 0;
    double pan = 0;  // -1..1
    bool mute = false;
    bool solo = false;
    std::vector<Clip> clips;
    std::vector<EnvelopePoint> envelope;
    // Ducking: this track drops by duckDb whenever track `duckUnder` is louder than duckThresholdDb.
    int duckUnder = -1;
    double duckDb = -12;
    double duckThresholdDb = -40;
    double duckAttack = 0.15;   // seconds to go down
    double duckRelease = 0.6;   // seconds to come back
    Rack fx;                          // effects, in order, before the volume and pan
    std::vector<Automation> automation;  // pan and fx<i>.<setting> lanes (volume is `envelope`)
    std::vector<Send> sends;
    double end() const;
};

// A marker (end < 0) or a region (start..end) on the timeline, in seconds.
struct Marker {
    std::string name;
    double start = 0;
    double end = -1;
    bool region() const { return end > start; }
};

struct Project {
    int rate = 44100;
    int channels = 2;
    std::vector<Track> tracks;
    double masterGainDb = 0;
    Rack masterFx;                             // after the tracks are summed
    std::vector<Automation> masterAutomation;  // volume and fx<i>.<setting> lanes of the master
    std::vector<Marker> markers;
    std::vector<Bus> buses;

    double seconds() const;
    // Reverb and delay tails of the racks: the mix runs this much past the last clip.
    double tailSeconds() const;
};

// Lanes of a track as shown and edited: "volume" (from the envelope) plus its automation.
std::vector<Automation> trackLanes(const Track& t);
// Sets one lane ("volume" writes the envelope); empty points clear it.
void setTrackLane(Track& t, const std::string& target, std::vector<AutomationPoint> points);
void setLane(std::vector<Automation>& lanes, const std::string& target, std::vector<AutomationPoint> points);
// Effects were added or removed: drops lanes that point at effects that no longer exist and
// renumbers the rest. `removed` is the index taken out (or -1), `inserted` the index added (or -1),
// `moveFrom`/`moveTo` a reorder.
void renumberLanes(std::vector<Automation>& lanes, int removed, int inserted, int moveFrom = -1, int moveTo = -1);

// JSON text of racks, lanes and markers (shared by the project and song files).
std::string rackToJson(const Rack& r);
Rack rackFromJson(const std::string& text);  // throws std::runtime_error
std::string lanesToJson(const std::vector<Automation>& lanes);
std::vector<Automation> lanesFromJson(const std::string& text);

// One track rendered (clips, fades, gain, envelope, ducking, pan) at the project's channels.
Buffer renderTrack(const Project& p, int track);
// Every audible track summed (solo wins over mute) with what they send to the buses, times the master gain.
Buffer mixdown(const Project& p);

// Lanes per track with each clip's waveform, muted tracks greyed out.
raster::Image renderProject(const Project& p, int width = 1200, int laneHeight = 90);

// Saves as JSON with every clip written as a 32-bit float WAV in `<stem>_clips/` next to it.
void saveProject(const Project& p, const std::filesystem::path& path);
Project loadProject(const std::filesystem::path& path);  // throws std::runtime_error

}  // namespace wetwaresuite::audio
