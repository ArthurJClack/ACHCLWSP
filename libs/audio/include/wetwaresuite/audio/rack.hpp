// The effects rack: a chain of effects (EQ, dynamics, reverb, delay,
// modulation, distortion...) that each track and the master of a project or
// song run their sound through, with automation lanes that move any effect
// setting, the volume or the pan over time. The same effects also apply
// destructively to part of an audio document (audio.chorus, audio.effect...).
//
// Effects are plain data (a type name and named numeric settings), so racks
// copy freely for undo and save as JSON. Processing builds a stateful
// Processor per effect and runs the audio through in short blocks, so
// automated settings change smoothly while the sound plays.
#pragma once

#include <cstddef>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "wetwaresuite/audio/buffer.hpp"

namespace wetwaresuite::audio {

// One setting of an effect type. Settings with choices hold the index of the choice.
struct EffectParam {
    std::string name;
    double min = 0, max = 1, def = 0;
    std::string unit;  // "dB", "Hz", "ms", "" ...
    std::string help;
    std::vector<std::string> choices;
};

struct EffectType {
    std::string name;   // "compressor"
    std::string title;  // "Compressor"
    std::string description;
    std::vector<EffectParam> params;
    const EffectParam* param(const std::string& name) const;
};

// eq, graphic_eq, filter, compressor, limiter, gate, expander, deesser, reverb, delay,
// chorus, flanger, phaser, tremolo, distortion, utility.
const std::vector<EffectType>& effectTypes();
const EffectType* findEffectType(const std::string& name);
const std::vector<std::string>& effectTypeNames();

// The parametric EQ has this many bands: b1_type, b1_freq, b1_gain, b1_q ... b8_q.
constexpr int kEqBands = 8;
// Graphic EQ band centres (ISO octaves, 31.5 Hz .. 16 kHz), settings g31 .. g16k.
const std::vector<double>& graphicEqFrequencies();

struct Effect {
    std::string type;       // a built-in type, or "clap" for a plugin (clap_host.hpp)
    std::string plugin;     // clap: "path#id"
    std::map<std::string, double> params;  // missing settings use the type's default
    bool bypass = false;
    double get(const std::string& name) const;
};
using Rack = std::vector<Effect>;

// "threshold=-20 ratio=4" (spaces, commas or semicolons between) into settings of `type`;
// choices by name (mode=hard), numbers clamped to the setting's range is an error instead.
// The parametric EQ also takes bands="lowshelf 120 3, peaking 1000 -4 2, highpass 80"
// (type, Hz, dB, Q per band, filling b1, b2, ... and turning the rest off).
bool parseEffectValues(const std::string& type, const std::string& text, std::map<std::string, double>& out, std::string& error);
// Builds an effect with defaults and `text` applied; false with error on a bad type or value.
bool makeEffect(const std::string& type, const std::string& text, Effect& out, std::string& error);
// "-20 dB", "4", "lowshelf".
std::string formatEffectValue(const EffectParam& p, double v);
// "compressor: threshold -20 dB, ratio 4, ..." (only the active bands of an EQ).
std::string describeEffect(const Effect& e);

// ---- processing ----
class Processor {
public:
    virtual ~Processor() = default;
    // Change a setting; takes effect from the next process() call.
    void set(const std::string& name, double value);
    // In place over `channels` (one pointer per channel, all of length n).
    virtual void process(float* const* channels, int channelCount, std::size_t n) = 0;
    // Frames the effect delays its output by (the limiter looks ahead).
    virtual std::size_t latency() const { return 0; }

protected:
    std::map<std::string, double> v_;
    bool dirty_ = true;
    double get(const std::string& name) const;
};

std::unique_ptr<Processor> makeProcessor(const Effect& e, int rate, int channels);

// Seconds an effect keeps sounding after its input stops (reverb, delay); 0 for most.
double effectTail(const Effect& e);
double rackTail(const Rack& r);

// ---- automation ----
struct AutomationPoint {
    double time = 0;  // seconds (beats in a beats song)
    double value = 0;
};

// target: "volume" (dB), "pan" (-1..1), or "fx<i>.<setting>" (effect i of the rack, from 0).
struct Automation {
    std::string target;
    std::vector<AutomationPoint> points;  // kept sorted by time
    // Linear between points, held before the first and after the last.
    double valueAt(double time) const;
    void sort();
};

// Splits "fx2.cutoff" into 2 and "cutoff"; false for other targets.
bool parseFxTarget(const std::string& target, int& index, std::string& param);
// Checks a target against a rack and clamps nothing; error explains what's allowed.
bool validTarget(const std::string& target, const Rack& rack, std::string& error);
// The value range a target takes (volume -60..12 dB, pan -1..1, else the setting's range).
bool targetRange(const std::string& target, const Rack& rack, double& lo, double& hi);
const Automation* findLane(const std::vector<Automation>& lanes, const std::string& target);

// Runs the rack over the whole buffer in place. Lanes for fx targets move settings every
// 64 frames; `startTime` is the buffer's first frame on the lanes' clock and
// `secondsPerUnit` converts lane time to seconds (1 for projects, seconds per beat for songs).
// Look-ahead latency is compensated, so the output lines up with the input.
void applyRack(Buffer& b, const Rack& rack, const std::vector<Automation>& lanes = {}, double startTime = 0,
               double secondsPerUnit = 1.0);
// One effect over part of a buffer (blended in and out at the range's edges), for the
// destructive audio.* effect commands.
void applyEffect(Buffer& b, const Effect& e, Range r, const std::vector<int>& channels = {});

// Volume (dB) and pan lanes applied to a buffer, per frame. Pan needs 2 channels.
void applyVolumeLane(Buffer& b, const Automation& lane, double startTime = 0, double secondsPerUnit = 1.0);
void applyPanLane(Buffer& b, const Automation& lane, double startTime = 0, double secondsPerUnit = 1.0);

}  // namespace wetwaresuite::audio
