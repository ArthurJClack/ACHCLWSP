// beats.new/open/save/close/info/set, render, bounce, MIDI, generate, preview,
// presets, plus the helpers every beats command shares.
#include <algorithm>
#include <cctype>
#include <map>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <sstream>

#include "wetwaresuite/audio/analysis.hpp"
#include "wetwaresuite/audio/document.hpp"
#include "wetwaresuite/audio/io.hpp"
#include "wetwaresuite/beats/beats.hpp"
#include "wetwaresuite/playback/commands.hpp"
#include "wetwaresuite/raster/codec.hpp"
#include "common.hpp"

namespace wetwaresuite::beats {

namespace fs = std::filesystem;

void registerCommands(Session& session) {
    tool::registerSongCommands(session);
    tool::registerOutputCommands(session);
    tool::registerTrackCommands(session);
    tool::registerPatternCommands(session);
    tool::registerArrangeCommands(session);
    tool::registerNoteCommands(session);
    tool::registerMixCommands(session);
}

std::string SongDoc::summary() const {
    std::string s = song.name + ": " + tool::num(song.bpm) + " bpm " + tool::sigText(song) + ", " + std::to_string(song.tracks.size()) +
                    " tracks, " + std::to_string(song.patterns.size()) + " patterns, " + std::to_string(song.bars()) + " bars (" +
                    audio::formatTime(song.seconds()) + ")";
    if (!path.empty()) s += ", " + path;
    return s;
}

std::unique_ptr<Attachment> SongDoc::preview() const {
    return std::make_unique<Attachment>(Attachment{"image/png", tool::pngOf(drawArrangement(song)), "arrangement.png"});
}

bool SongDoc::timeline(TimelineLayout& out) const {
    out.docParam = "song";
    out.tracks.clear();
    const double bar = song.barSeconds();
    for (std::size_t ti = 0; ti < song.tracks.size(); ++ti) {
        const Track& t = song.tracks[ti];
        TimelineTrack tt;
        tt.id = std::to_string(ti);
        tt.name = t.name + (t.mute ? " (muted)" : t.solo ? " (solo)" : "");
        tt.kind = "pattern";
        for (std::size_t pi = 0; pi < song.arrangement.size(); ++pi) {
            const Placement& pl = song.arrangement[pi];
            if (pl.track != static_cast<int>(ti)) continue;
            const Pattern* pat = song.findPattern(pl.pattern);
            TimelineClip c;
            c.id = std::to_string(pi);
            c.label = pl.pattern + (pl.repeat > 1 ? " x" + std::to_string(pl.repeat) : "");
            c.start = pl.bar * bar;
            c.length = (pat ? pat->bars : 1) * pl.repeat * bar;
            c.headroom = 0;  // the start only moves (whole bars); the end sets the repeats
            tt.clips.push_back(c);
        }
        out.tracks.push_back(std::move(tt));
    }
    return true;
}

std::string SongDoc::timelineCommand(const TimelineEdit& e) const {
    const std::size_t pi = static_cast<std::size_t>(std::atoi(e.clip.c_str()));
    if (pi >= song.arrangement.size()) return {};
    const Placement& pl = song.arrangement[pi];
    const double bar = song.barSeconds();
    switch (e.kind) {
        case TimelineEditKind::Move: {
            std::string line = "beats.arrange_move placement=" + e.clip + " bar=" + std::to_string(std::max(0, static_cast<int>(std::lround(e.start / bar))));
            if (!e.toTrack.empty() && e.toTrack != e.track) line += " to_track=" + e.toTrack;
            return line;
        }
        case TimelineEditKind::TrimEnd: {
            const Pattern* pat = song.findPattern(pl.pattern);
            const double one = (pat ? pat->bars : 1) * bar;
            return "beats.arrange_move placement=" + e.clip + " repeat=" + std::to_string(std::max(1, static_cast<int>(std::lround(e.length / one))));
        }
        case TimelineEditKind::Delete:
            return "beats.arrange_remove track=" + std::to_string(pl.track) + " bar=" + std::to_string(pl.bar) + " pattern=" + pl.pattern;
        default: return {};  // no trimming the start or splitting a placement
    }
}

Result SongEdit::apply() {
    before_ = doc_->song;
    Result r;
    try {
        r = fn_(*doc_);
    } catch (const std::exception& e) {
        r = Result::failure(e.what());
    }
    if (!r.ok) revert();
    return r;
}

namespace tool {

ParamSpec songParam() { return {"song", ParamType::String, "id of an open song, e.g. song1", true}; }
ParamSpec trackParam(bool required) { return {"track", ParamType::String, "track number (from 0) or name, e.g. 0 or Drums", required}; }
ParamSpec patternParam(bool required) { return {"pattern", ParamType::String, "pattern name, e.g. beat", required}; }
ParamSpec previewParam(const std::string& what) {
    return {"preview", ParamType::Bool, "attach a PNG of " + what, false, {}, false};
}

ParamSpec songOrFileParam() { return {"song", ParamType::String, "id of an open song, e.g. song1 (or give input=)", false}; }
ParamSpec inputParam() {
    return {"input", ParamType::String, "instead of song=: a .beats.json or .mid file, for one-shot use from the command line", false};
}

std::shared_ptr<SongDoc> songFromFile(const std::string& path) {
    auto doc = std::make_shared<SongDoc>();
    std::string lower = path;
    std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (lower.size() > 4 && (lower.compare(lower.size() - 4, 4, ".mid") == 0 || (lower.size() > 5 && lower.compare(lower.size() - 5, 5, ".midi") == 0))) {
        std::ifstream in(path, std::ios::binary);
        if (!in) throw std::runtime_error("file not found: " + path);
        std::ostringstream ss;
        ss << in.rdbuf();
        doc->song = importMidi(ss.str(), fs::path(path).stem().string());
    } else {
        doc->song = loadSong(path);
    }
    doc->path = path;
    return doc;
}

ActionFactory songCommand(Session& session, std::function<Result(SongDoc&, const Params&)> fn, bool undoable) {
    return [&session, fn, undoable](const Params& p) -> std::unique_ptr<Action> {
        if (!undoable && !p.count("song") && p.count("input")) {
            return std::make_unique<FunctionAction>([fn, p] {
                try {
                    auto doc = songFromFile(stringParam(p, "input"));
                    Result r = fn(*doc, p);
                    auto& f = r.data;
                    f.erase(std::remove_if(f.begin(), f.end(), [](const auto& kv) { return kv.first == "song"; }), f.end());
                    return r;
                } catch (const std::exception& e) {
                    return failed(e.what());
                }
            });
        }
        if (!p.count("song")) return std::make_unique<FunctionAction>([] { return failed("give song=song1 (an open song) or input=file.beats.json"); });
        const std::string id = stringParam(p, "song");
        auto doc = session.workspace().getAs<SongDoc>(id);
        if (!doc) {
            return std::make_unique<FunctionAction>([id] { return failed("no open song called '" + id + "' (make one with beats.new or beats.open)"); });
        }
        if (!undoable) {
            return std::make_unique<FunctionAction>([doc, fn, p] {
                try {
                    return fn(*doc, p);
                } catch (const std::exception& e) {
                    return failed(e.what());
                }
            });
        }
        return std::make_unique<SongEdit>(doc, [fn, p](SongDoc& d) { return fn(d, p); });
    };
}

bool trackArg(const Song& s, const Params& p, int& out, std::string& error) {
    const std::string ref = stringParam(p, "track");
    out = findTrack(s, ref);
    if (out < 0) {
        std::string names;
        for (std::size_t i = 0; i < s.tracks.size(); ++i) names += (names.empty() ? "" : ", ") + std::to_string(i) + " " + s.tracks[i].name;
        error = "no track '" + ref + "'" + (names.empty() ? " (the song has no tracks yet; add one with beats.track_add)" : " (tracks: " + names + ")");
        return false;
    }
    return true;
}

Pattern* patternArg(Song& s, const Params& p, std::string& error, const std::string& key) {
    const std::string name = stringParam(p, key);
    Pattern* pat = s.findPattern(name);
    if (!pat) {
        std::string names;
        for (const auto& x : s.patterns) names += (names.empty() ? "" : ", ") + x.name;
        error = "no pattern '" + name + "'" + (names.empty() ? " (add one with beats.pattern_add)" : " (patterns: " + names + ")");
    }
    return pat;
}

std::string pngOf(const raster::Image& im) { return raster::encodePng(im); }

std::string sigText(const Song& s) { return std::to_string(s.beatsPerBar) + "/" + std::to_string(s.beatUnit); }

bool parseSignature(const std::string& text, int& top, int& bottom) {
    const auto slash = text.find('/');
    if (slash == std::string::npos) return false;
    try {
        top = std::stoi(text.substr(0, slash));
        bottom = std::stoi(text.substr(slash + 1));
    } catch (const std::exception&) {
        return false;
    }
    return top >= 1 && top <= 32 && (bottom == 1 || bottom == 2 || bottom == 4 || bottom == 8 || bottom == 16 || bottom == 32);
}

Result songResult(const std::string& message, const SongDoc& d, const Params& p) {
    Result r = Result::success(message);
    r.with("song", stringParam(p, "song"))
        .with("tracks", static_cast<std::int64_t>(d.song.tracks.size()))
        .with("patterns", static_cast<std::int64_t>(d.song.patterns.size()))
        .with("bars", static_cast<std::int64_t>(d.song.bars()))
        .with("seconds", d.song.seconds());
    if (r.ok && boolParam(p, "preview", false)) r.attach({"image/png", pngOf(drawArrangement(d.song)), "arrangement.png"});
    return r;
}

TrackKind patternKind(const Song& s, const Pattern& pat) {
    for (const auto& pl : s.arrangement) {
        if (pl.pattern == pat.name && pl.track >= 0 && pl.track < static_cast<int>(s.tracks.size())) return s.tracks[static_cast<std::size_t>(pl.track)].kind;
    }
    if (!pat.lanes.empty()) {
        for (const auto& [lane, st] : pat.lanes) {
            if (parseNote(lane) >= 0) return TrackKind::Synth;
        }
        return TrackKind::Drum;
    }
    return TrackKind::Synth;
}

Result patternResult(const std::string& message, const SongDoc& d, const Pattern& pat, const Params& p) {
    Result r = Result::success(message);
    const auto steps = static_cast<std::size_t>(std::lround(pat.bars * d.song.beatsInBar() * pat.stepsPerBeat));
    for (const auto& [lane, st] : pat.lanes) {
        if (st.empty()) continue;
        if (st.size() > steps) {
            r.message += "\nnote: lane " + lane + " has " + std::to_string(st.size()) + " steps but the pattern has " + std::to_string(steps) +
                         "; the extra steps don't play (make the pattern longer with beats.pattern_set bars=)";
        } else if (steps % st.size() != 0) {
            r.message += "\nnote: lane " + lane + " has " + std::to_string(st.size()) + " steps, which doesn't divide the pattern's " + std::to_string(steps) +
                         ", so its repeats drift";
        }
    }
    r.with("song", stringParam(p, "song"))
        .with("pattern", pat.name)
        .with("bars", static_cast<std::int64_t>(pat.bars))
        .with("lanes", static_cast<std::int64_t>(pat.lanes.size()))
        .with("notes", static_cast<std::int64_t>(pat.notes.size()));
    if (r.ok && boolParam(p, "preview", false)) r.attach({"image/png", pngOf(drawPattern(d.song, pat, patternKind(d.song, pat))), "pattern.png"});
    return r;
}

bool parseLanes(const std::string& text, std::vector<std::pair<std::string, std::string>>& out, std::string& error) {
    out.clear();
    std::string item;
    auto flush = [&]() -> bool {
        std::string s = item;
        item.clear();
        const auto first = s.find_first_not_of(" \t\r");
        if (first == std::string::npos) return true;
        const auto colon = s.find_first_of(":=");
        if (colon == std::string::npos) {
            error = "write each lane as name: steps, e.g. \"kick: x...x...x...x...; snare: ....x.......x...\"";
            return false;
        }
        std::string name = s.substr(0, colon), steps;
        name.erase(0, name.find_first_not_of(" \t"));
        name.erase(name.find_last_not_of(" \t") + 1);
        if (name.empty()) {
            error = "a lane needs a name before the ':'";
            return false;
        }
        if (!cleanSteps(s.substr(colon + 1), steps, error)) return false;
        out.emplace_back(name, steps);
        return true;
    };
    for (char c : text) {
        if (c == ';' || c == ',' || c == '\n') {
            if (!flush()) return false;
        } else {
            item += c;
        }
    }
    return flush();
}

void setLane(Pattern& p, const std::string& lane, const std::string& steps) {
    for (auto it = p.lanes.begin(); it != p.lanes.end(); ++it) {
        if (it->first == lane) {
            if (steps.empty()) p.lanes.erase(it);
            else it->second = steps;
            return;
        }
    }
    if (!steps.empty()) p.lanes.emplace_back(lane, steps);
}

std::string listing(const Song& s) {
    std::string out;
    bool anySolo = false;
    for (const auto& t : s.tracks) anySolo = anySolo || t.solo;
    for (std::size_t i = 0; i < s.tracks.size(); ++i) {
        const Track& t = s.tracks[i];
        out += "\ntrack " + std::to_string(i) + " \"" + t.name + "\" (" + toString(t.kind);
        if (!t.presetName.empty()) out += ", " + t.presetName;
        out += "): volume " + num(t.volumeDb, 3) + " dB, pan " + num(t.pan, 3);
        if (t.mute) out += ", muted";
        if (t.solo) out += ", solo";
        if (anySolo && !t.solo) out += " (silent: another track is solo)";
        if (t.kind == TrackKind::Sampler) {
            out += ", sample " + (t.sample.path.empty() ? std::string("(none yet)") : t.sample.path) + (t.sample.path.empty() || t.sample.audio ? "" : " (missing)");
            out += " at " + noteName(t.root);
        }
        for (const auto& [lane, smp] : t.slots) out += ", slot " + lane + " = " + smp.path + (smp.audio ? "" : " (missing)");
        std::string fx;
        if (t.fx.distortion > 0) fx += " distortion " + num(t.fx.distortion, 2);
        if (t.fx.compress > 0) fx += " compress " + num(t.fx.compress, 2);
        if (t.fx.delayMix > 0) fx += " delay " + num(t.fx.delayMix, 2) + " (" + num(t.fx.delayBeats, 3) + " beats)";
        if (t.fx.reverb > 0) fx += " reverb " + num(t.fx.reverb, 2);
        if (!fx.empty()) out += ", effects:" + fx;
        if (t.kind == TrackKind::Synth && (t.synth.unison > 1 || t.synth.filter != FilterMode::LowPass || t.synth.lfoTarget != LfoTarget::Off))
            out += ", " + filterModeNames()[static_cast<std::size_t>(t.synth.filter)] + (t.synth.unison > 1 ? ", unison " + std::to_string(t.synth.unison) : "") +
                   (t.synth.lfoTarget != LfoTarget::Off ? ", lfo " + lfoTargetNames()[static_cast<std::size_t>(t.synth.lfoTarget)] + " " + num(t.synth.lfoRate, 3) + " Hz" : "");
        for (const auto& [lane, pd] : t.pads)
            out += ", pad " + lane + " (" + num(pd.volumeDb, 3) + " dB, " + num(pd.tune, 3) + " st" + (pd.decay > 0 ? ", decay " + num(pd.decay, 3) + " s" : "") + ")";
        for (std::size_t k = 0; k < t.rack.size(); ++k) out += "\n  fx " + std::to_string(k) + " " + audio::describeEffect(t.rack[k]);
        for (const auto& lane : t.automation) out += "\n  automation " + lane.target + ": " + std::to_string(lane.points.size()) + " points";
        for (const auto& pl : s.arrangement) {
            if (pl.track != static_cast<int>(i)) continue;
            const Pattern* p = s.findPattern(pl.pattern);
            const int len = p ? p->bars : 1;
            out += "\n  bar " + std::to_string(pl.bar) + "-" + std::to_string(pl.bar + len * pl.repeat - 1) + ": " + pl.pattern;
            if (pl.repeat > 1) out += " x" + std::to_string(pl.repeat);
            if (!p) out += " (missing pattern)";
        }
    }
    if (!s.masterRack.empty() || !s.masterAutomation.empty() || s.metronome) {
        out += "\nmaster" + std::string(s.metronome ? " (metronome on at " + num(s.metronomeDb, 3) + " dB)" : "") + ":";
        for (std::size_t k = 0; k < s.masterRack.size(); ++k) out += "\n  fx " + std::to_string(k) + " " + audio::describeEffect(s.masterRack[k]);
        for (const auto& lane : s.masterAutomation) out += "\n  automation " + lane.target + ": " + std::to_string(lane.points.size()) + " points";
    }
    for (const auto& p : s.patterns) {
        out += "\npattern \"" + p.name + "\": " + std::to_string(p.bars) + (p.bars == 1 ? " bar" : " bars");
        if (!p.lanes.empty()) {
            out += ", " + std::to_string(p.stepsPerBeat) + " steps per beat";
            for (const auto& [lane, steps] : p.lanes) out += "\n  " + lane + ": " + steps;
        }
        if (!p.notes.empty()) {
            out += "\n  " + std::to_string(p.notes.size()) + " notes: ";
            std::vector<Note> shown(p.notes.begin(), p.notes.begin() + static_cast<std::ptrdiff_t>(std::min<std::size_t>(p.notes.size(), 24)));
            out += formatNotes(shown);
            if (p.notes.size() > shown.size()) out += ", ...";
        }
    }
    return out;
}

}  // namespace tool

namespace {

using namespace tool;

bool renderArgs(const Song& s, const Params& p, RenderOptions& o, std::string& error) {
    o.startBar = static_cast<int>(intParam(p, "start_bar", 0));
    o.bars = p.count("bars") ? static_cast<int>(intParam(p, "bars")) : -1;
    o.tailSeconds = p.count("tail") ? numberParam(p, "tail") : -1.0;
    o.limit = boolParam(p, "limit", true);
    if (p.count("click")) o.click = boolParam(p, "click") ? 1 : 0;
    if (o.startBar < 0 || (p.count("bars") && o.bars < 1)) {
        error = "start_bar must be >= 0 and bars >= 1";
        return false;
    }
    if (p.count("tail") && (o.tailSeconds < 0 || o.tailSeconds > 30)) {
        error = "tail is 0..30 seconds";
        return false;
    }
    if (p.count("track")) {
        if (!trackArg(s, p, o.onlyTrack, error)) return false;
    }
    if (s.arrangement.empty()) {
        error = "nothing is placed in the arrangement yet (use beats.arrange_place, or beats.generate)";
        return false;
    }
    if (o.bars < 0 && o.startBar >= s.bars()) {
        error = "start_bar is past the end of the song (" + std::to_string(s.bars()) + " bars)";
        return false;
    }
    return true;
}

std::vector<ParamSpec> renderParams() {
    return {{"start_bar", ParamType::Int, "first bar to render, from 0", false, {}, std::int64_t{0}},
            {"bars", ParamType::Int, "how many bars (default: to the end of the arrangement)", false},
            {"track", ParamType::String, "render just this track (number or name), ignoring mute and solo", false},
            {"tail", ParamType::Number, "seconds to let sound ring past the end (default: automatic, trimmed)", false},
            {"limit", ParamType::Bool, "brickwall limiter at -1 dBFS on the master", false, {}, true},
            {"click", ParamType::Bool, "add the metronome click (default: the song's metronome setting)", false}};
}

std::string waveformPng(const audio::Buffer& b) {
    audio::WaveformStyle st;
    st.width = 1024;
    st.height = 280;
    return raster::encodePng(audio::waveform(b, audio::all(b), st));
}

std::string readFile(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    if (!in) throw std::runtime_error("file not found: " + p.string());
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

std::string lowerName(const fs::path& p) {
    std::string s = p.filename().string();
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

Result picture(SongDoc& d, const Params& p) {
                    const std::string view = p.count("pattern") && !p.count("view") ? "pattern" : stringParam(p, "view", "arrangement");
                    const int width = static_cast<int>(std::clamp<std::int64_t>(intParam(p, "width", 1200), 200, 4000));
                    Result r = Result::success("");
                    r.with("song", stringParam(p, "song"));
                    if (view == "pattern") {
                        std::string error;
                        if (!p.count("pattern")) return failed("view=pattern needs pattern=<name>");
                        const Pattern* pat = patternArg(d.song, p, error);
                        if (!pat) return failed(error);
                        r.message = "pattern " + pat->name + ": " + std::to_string(pat->lanes.size()) + " step lanes, " + std::to_string(pat->notes.size()) + " notes";
                        r.attach({"image/png", pngOf(drawPattern(d.song, *pat, patternKind(d.song, *pat), width)), "pattern.png"});
                    } else if (view == "waveform") {
                        RenderOptions o;
                        std::string error;
                        if (!renderArgs(d.song, p, o, error)) return failed(error);
                        const audio::Buffer mix = render(d.song, o);
                        audio::WaveformStyle st;
                        st.width = width;
                        st.height = 280;
                        r.message = "waveform of " + audio::formatTime(mix.seconds());
                        r.attach({"image/png", raster::encodePng(audio::waveform(mix, audio::all(mix), st)), "waveform.png"});
                    } else {
                        r.message = "arrangement: " + d.summary();
                        r.attach({"image/png", pngOf(drawArrangement(d.song, width)), "arrangement.png"});
                    }
                    return r;
}

// beats.generate without song=: makes a new song; undo closes it again.
class GenerateNew : public Action {
public:
    GenerateNew(Session& s, GenerateOptions o, Params p, std::string error) : s_(s), o_(std::move(o)), p_(std::move(p)), error_(std::move(error)) {}
    Result apply() override {
        if (!error_.empty()) return Result::failure(error_);
        if (!doc_) {
            doc_ = std::make_shared<SongDoc>();
            doc_->song.name = o_.genre;
            try {
                what_ = generate(doc_->song, o_);
            } catch (const std::exception& e) {
                doc_.reset();
                return Result::failure(e.what());
            }
            id_ = s_.workspace().add(doc_, "song");
        } else {
            s_.workspace().put(id_, doc_);
        }
        Params q = p_;
        q["song"] = id_;
        std::string saved;
        if (p_.count("save")) {
            try {
                saveSong(doc_->song, stringParam(p_, "save"));
            } catch (const std::exception& e) {
                return Result::failure(e.what());
            }
            doc_->path = stringParam(p_, "save");
            saved = "; saved " + doc_->path;
        }
        return tool::songResult("made " + id_ + ": " + what_ + saved, *doc_, q);
    }
    void revert() override {
        if (!id_.empty()) s_.workspace().take(id_);
    }

private:
    Session& s_;
    GenerateOptions o_;
    Params p_;
    std::string error_, id_, what_;
    std::shared_ptr<SongDoc> doc_;
};

bool endsWith(const std::string& s, const std::string& end) { return s.size() >= end.size() && s.compare(s.size() - end.size(), end.size(), end) == 0; }

std::string stemOf(const fs::path& p) {
    std::string n = p.filename().string();
    for (const char* e : {".beats.json", ".json", ".mid", ".midi"}) {
        if (endsWith(lowerName(p), e)) return n.substr(0, n.size() - std::string(e).size());
    }
    return p.stem().string();
}

}  // namespace

namespace tool {

void registerSongCommands(Session& session) {
    auto& reg = session.registry();
    Session* s = &session;

    reg.add({"beats.new",
             "New song",
             "Start an empty song (song1) with a tempo, time signature and swing. Then add tracks (beats.track_add), patterns "
             "(beats.pattern_add) and place them (beats.arrange_place), or let beats.generate make a starting point.",
             {{"name", ParamType::String, "song name", false, {}, std::string("song")},
              {"bpm", ParamType::Number, "tempo in beats (quarter notes) per minute", false, {}, 120.0},
              {"time_signature", ParamType::String, "e.g. 4/4, 3/4, 6/8, 7/8", false, {}, std::string("4/4")},
              {"swing", ParamType::Number, "0..1: pushes every second step late (0.33 is about a triplet shuffle)", false, {}, 0.0},
              {"rate", ParamType::Int, "sample rate for rendering, Hz", false, {}, std::int64_t{44100}}},
             false, true},
            [s](const Params& p) {
                return std::make_unique<FunctionAction>([s, p] {
                    auto doc = std::make_shared<SongDoc>();
                    Song& g = doc->song;
                    g.name = stringParam(p, "name", "song");
                    g.bpm = numberParam(p, "bpm", 120);
                    g.swing = numberParam(p, "swing", 0);
                    g.rate = static_cast<int>(intParam(p, "rate", 44100));
                    if (g.bpm < 20 || g.bpm > 400) return failed("bpm must be 20..400");
                    if (g.swing < 0 || g.swing > 1) return failed("swing is 0..1");
                    if (g.rate < 8000 || g.rate > 192000) return failed("rate must be 8000..192000 Hz");
                    if (!parseSignature(stringParam(p, "time_signature", "4/4"), g.beatsPerBar, g.beatUnit)) {
                        return failed("time_signature is like 4/4, 3/4 or 6/8 (bottom 1, 2, 4, 8, 16 or 32)");
                    }
                    const std::string id = s->workspace().add(doc, "song");
                    return Result::success("made " + id + ": " + doc->summary()).with("song", id);
                });
            });

    reg.add({"beats.open",
             "Open song",
             "Open a song saved with beats.save (.beats.json). Samples it uses are loaded again from their files (paths are "
             "relative to the song file when they were saved next to it). To open a .mid file use beats.midi_import.",
             {{"path", ParamType::String, "the .beats.json file", true}, previewParam()},
             false, true},
            [s](const Params& p) {
                return std::make_unique<FunctionAction>([s, p] {
                    const std::string path = stringParam(p, "path");
                    if (endsWith(lowerName(path), ".mid") || endsWith(lowerName(path), ".midi")) return failed("that's a MIDI file; open it with beats.midi_import");
                    auto doc = std::make_shared<SongDoc>();
                    try {
                        doc->song = loadSong(path);
                    } catch (const std::exception& e) {
                        return failed(e.what());
                    }
                    doc->path = path;
                    const std::string id = s->workspace().add(doc, "song");
                    Params q = p;
                    q["song"] = id;
                    return songResult("opened " + id + ": " + doc->summary(), *doc, q);
                });
            });

    reg.add({"beats.save",
             "Save song",
             "Save the song as .beats.json (editable; reopen with beats.open). Sample files are referenced, not copied.",
             {songParam(), {"path", ParamType::String, "file to write, ending .beats.json (default: where it was opened or last saved)", false}},
             false},
            songCommand(
                session,
                [](SongDoc& d, const Params& p) {
                    std::string path = stringParam(p, "path", d.path);
                    if (path.empty()) return failed("give path=name.beats.json");
                    if (!endsWith(lowerName(path), ".json")) path += ".beats.json";
                    saveSong(d.song, path);
                    d.path = path;
                    return Result::success("saved " + path).with("song", stringParam(p, "song")).with("path", path);
                },
                false));

    reg.add({"beats.close",
             "Close song",
             "Close an open song (unsaved changes are lost).",
             {songParam()},
             false},
            [s](const Params& p) {
                return std::make_unique<FunctionAction>([s, id = stringParam(p, "song")] {
                    if (!s->workspace().getAs<SongDoc>(id)) return failed("no open song called '" + id + "'");
                    s->workspace().take(id);
                    return Result::success("closed " + id);
                });
            });

    reg.add({"beats.info",
             "Song info",
             "Tempo, time signature, swing, length, every track with its sound, effects and placements, and every pattern with "
             "its steps and notes.",
             {songOrFileParam(), inputParam(), previewParam()},
             false, true},
            songCommand(
                session,
                [](SongDoc& d, const Params& p) {
                    const Song& g = d.song;
                    Result r = songResult(d.summary() + (g.swing > 0 ? ", swing " + num(g.swing, 3) : "") + (g.key.empty() ? "" : ", key " + g.key) +
                                              listing(g),
                                          d, p);
                    r.with("name", g.name).with("bpm", g.bpm).with("time_signature", sigText(g)).with("swing", g.swing).with("rate", static_cast<std::int64_t>(g.rate));
                    if (!g.key.empty()) r.with("key", g.key);
                    std::string names;
                    for (const auto& t : g.tracks) names += (names.empty() ? "" : ", ") + t.name;
                    r.with("track_names", names);
                    std::string pats;
                    for (const auto& pt : g.patterns) pats += (pats.empty() ? "" : ", ") + pt.name;
                    r.with("pattern_names", pats);
                    return r;
                },
                false));

    reg.add({"beats.set",
             "Song settings",
             "Change the tempo, time signature, swing, master volume, name or key label of a song.",
             {songParam(),
              {"bpm", ParamType::Number, "tempo, 20..400", false},
              {"time_signature", ParamType::String, "e.g. 4/4, 3/4, 6/8", false},
              {"swing", ParamType::Number, "0..1", false},
              {"master", ParamType::Number, "master volume in dB", false},
              {"name", ParamType::String, "song name", false},
              {"key", ParamType::String, "key label, e.g. A minor (informational)", false},
              previewParam()},
             true},
            songCommand(session, [](SongDoc& d, const Params& p) {
                Song& g = d.song;
                if (p.count("bpm")) {
                    const double v = numberParam(p, "bpm");
                    if (v < 20 || v > 400) return failed("bpm must be 20..400");
                    g.bpm = v;
                }
                if (p.count("time_signature") && !parseSignature(stringParam(p, "time_signature"), g.beatsPerBar, g.beatUnit)) {
                    return failed("time_signature is like 4/4, 3/4 or 6/8");
                }
                if (p.count("swing")) {
                    const double v = numberParam(p, "swing");
                    if (v < 0 || v > 1) return failed("swing is 0..1");
                    g.swing = v;
                }
                if (p.count("master")) g.masterDb = std::clamp(numberParam(p, "master"), -60.0, 12.0);
                if (p.count("name")) g.name = stringParam(p, "name");
                if (p.count("key")) g.key = stringParam(p, "key");
                return songResult("song: " + d.summary(), d, p);
            }));

}

void registerOutputCommands(Session& session) {
    auto& reg = session.registry();
    Session* s = &session;
    auto played = std::make_shared<std::map<std::string, RenderOptions>>();  // how each song was last played

    reg.add({"beats.play",
             "Play song",
             "Play the arrangement (or start_bar/bars of it, or one track) through the speakers. Returns at once; "
             "beats.playhead says where it is and beats.pause/resume/stop/seek control it. loop=true loops it while you "
             "edit (play again to hear the changes). From the command line the program stays open until it ends.",
             [] {
                 std::vector<ParamSpec> v = {songOrFileParam(), inputParam()};
                 for (auto& x : renderParams()) v.push_back(x);
                 for (auto& x : playback::playParams()) v.push_back(x);
                 return v;
             }(),
             false, true},
            songCommand(
                session,
                [played](SongDoc& d, const Params& p) {
                    RenderOptions o;
                    std::string error;
                    if (!renderArgs(d.song, p, o, error)) return failed(error);
                    auto mix = std::make_shared<const audio::Buffer>(render(d.song, o));
                    const std::string source = p.count("song") ? stringParam(p, "song") : d.song.name.empty() ? std::string("song") : d.song.name;
                    (*played)[source] = o;
                    return playback::startPlayback(std::move(mix), source, p);
                },
                false));
    // live playback: the song re-rendered (same part, same track) after an edit
    playback::registerTransportCommands(session, "beats", [s, played](const std::string& source, std::string&) -> std::shared_ptr<const audio::Buffer> {
        auto doc = s->workspace().getAs<SongDoc>(source);
        if (!doc) return nullptr;
        const auto it = played->find(source);
        return std::make_shared<const audio::Buffer>(render(doc->song, it != played->end() ? it->second : RenderOptions{}));
    });

    reg.add({"beats.render",
             "Render song",
             "Mix the arrangement to an audio file: WAV built in; mp3, ogg, flac, opus and m4a through ffmpeg when it's "
             "installed. The extension picks the format. preview=true attaches the waveform.",
             [] {
                 std::vector<ParamSpec> v = {songOrFileParam(), inputParam(), {"path", ParamType::String, "output file, e.g. song.wav or song.mp3", true}};
                 for (auto& x : renderParams()) v.push_back(x);
                 v.push_back({"bits", ParamType::String, "WAV/FLAC sample format", false, {"16", "24", "32", "32f"}, std::string("16")});
                 v.push_back({"bitrate", ParamType::Int, "kbit/s for mp3/ogg/opus/m4a (0 = encoder default)", false, {}, std::int64_t{0}});
                 v.push_back(previewParam("the rendered waveform"));
                 return v;
             }(),
             false},
            songCommand(
                session,
                [](SongDoc& d, const Params& p) {
                    RenderOptions o;
                    std::string error;
                    if (!renderArgs(d.song, p, o, error)) return failed(error);
                    const std::string path = stringParam(p, "path");
                    if (!audio::canWrite(path)) {
                        const std::string ext = fs::path(path).extension().string();
                        if (!audio::ffmpegAvailable() && !ext.empty() && ext != ".wav") return failed("saving " + ext + " needs ffmpeg on PATH; render to .wav instead");
                        return failed("can't write " + path + "; use .wav, .mp3, .ogg, .flac, .opus or .m4a");
                    }
                    audio::SaveOptions so;
                    if (!audio::parseSampleFormat(stringParam(p, "bits", "16"), so.format)) return failed("bits must be 16, 24, 32 or 32f");
                    so.bitrateKbps = static_cast<int>(std::clamp<std::int64_t>(intParam(p, "bitrate", 0), 0, 512));
                    const audio::Buffer mix = render(d.song, o);
                    audio::save(mix, path, so);
                    const auto lv = audio::measure(mix, audio::all(mix), false);
                    Result r = Result::success("rendered " + path + ": " + audio::formatTime(mix.seconds()) + ", peak " + num(lv.peakDb, 3) + " dBFS, " +
                                               num(lv.lufs, 3) + " LUFS");
                    r.with("song", stringParam(p, "song")).with("path", path).with("seconds", mix.seconds()).with("peak_db", lv.peakDb).with("lufs", lv.lufs);
                    if (boolParam(p, "preview", false)) r.attach({"image/png", waveformPng(mix), "waveform.png"});
                    return r;
                },
                false));

    reg.add({"beats.bounce",
             "Bounce to audio",
             "Render the song (or some bars, or one track) into a new audio document (aud1) in the session, for further "
             "editing with the audio.* commands (audio.normalize, audio.save...).",
             [] {
                 std::vector<ParamSpec> v = {songParam()};
                 for (auto& x : renderParams()) v.push_back(x);
                 v.push_back(previewParam("the waveform"));
                 return v;
             }(),
             false, true},
            [s](const Params& p) {
                return songCommand(
                    *s,
                    [s](SongDoc& d, const Params& q) {
                        RenderOptions o;
                        std::string error;
                        if (!renderArgs(d.song, q, o, error)) return failed(error);
                        auto doc = audio::newAudio(render(d.song, o));
                        const std::string id = s->workspace().add(doc, "aud");
                        Result r = Result::success("bounced to " + id + ": " + doc->summary());
                        r.with("doc", id).with("seconds", doc->audio.seconds());
                        if (boolParam(q, "preview", false)) r.attach({"image/png", waveformPng(doc->audio), "waveform.png"});
                        return r;
                    },
                    false)(p);
            });

    reg.add({"beats.midi_export",
             "Export MIDI",
             "Write the arrangement as a Standard MIDI File (.mid, format 1, 480 ticks per beat): tempo and time signature, one "
             "MIDI track per song track, drum tracks on channel 10 with General MIDI drum notes, swing baked into the timing.",
             {songOrFileParam(), inputParam(), {"path", ParamType::String, "output .mid file", true}},
             false},
            songCommand(
                session,
                [](SongDoc& d, const Params& p) {
                    if (d.song.arrangement.empty()) return failed("nothing is placed in the arrangement yet");
                    const std::string path = stringParam(p, "path");
                    if (auto parent = fs::path(path).parent_path(); !parent.empty()) {
                        std::error_code ec;
                        fs::create_directories(parent, ec);
                    }
                    const std::string bytes = exportMidi(d.song);
                    std::ofstream out(path, std::ios::binary);
                    if (!out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()))) return failed("can't write " + path);
                    return Result::success("wrote " + path + " (" + std::to_string(d.song.tracks.size()) + " tracks, " + std::to_string(bytes.size()) + " bytes)")
                        .with("song", stringParam(p, "song"))
                        .with("path", path)
                        .with("bytes", static_cast<std::int64_t>(bytes.size()));
                },
                false));

    reg.add({"beats.midi_import",
             "Import MIDI",
             "Open a .mid file (format 0 or 1) as a new song: its tempo and time signature, one track per MIDI channel (channel "
             "10 becomes a drum track playing the kit), each holding one pattern with all its notes, placed at bar 0. Synth "
             "presets are guessed from the General MIDI program.",
             {{"path", ParamType::String, "the .mid file", true}, previewParam()},
             false, true},
            [s](const Params& p) {
                return std::make_unique<FunctionAction>([s, p] {
                    const std::string path = stringParam(p, "path");
                    auto doc = std::make_shared<SongDoc>();
                    try {
                        doc->song = importMidi(readFile(path), stemOf(path));
                    } catch (const std::exception& e) {
                        return failed(e.what());
                    }
                    const std::string id = s->workspace().add(doc, "song");
                    Params q = p;
                    q["song"] = id;
                    return songResult("imported " + id + ": " + doc->summary() + listing(doc->song), *doc, q);
                });
            });

    reg.add({"beats.generate",
             "Generate a beat",
             "Make a starting point from a genre, a key and a seed: a drum groove (with fills every 4 bars), a bass line, a "
             "chord progression and a melody, each its own track and pattern you can then edit. Without song= it makes a new "
             "song; with song= it adds the tracks to that song and sets its tempo. Same seed, same result.",
             {{"genre", ParamType::String, "style", false, genreNames(), std::string("house")},
              {"key", ParamType::String, "e.g. A minor, Am, F# major, Eb", false, {}, std::string("C minor")},
              {"seed", ParamType::Int, "change it for a different variation", false, {}, std::int64_t{1}},
              {"bars", ParamType::Int, "length in bars (multiples of 4 work best)", false, {}, std::int64_t{8}},
              {"bpm", ParamType::Number, "tempo (default: the genre's)", false},
              {"drums", ParamType::Bool, "make a drum track", false, {}, true},
              {"bass", ParamType::Bool, "make a bass track", false, {}, true},
              {"chords", ParamType::Bool, "make a chords track", false, {}, true},
              {"melody", ParamType::Bool, "make a melody track", false, {}, true},
              {"song", ParamType::String, "add to this open song instead of making a new one", false},
              {"save", ParamType::String, "also save the new song to this .beats.json file", false},
              previewParam()},
             true},
            [s](const Params& p) -> std::unique_ptr<Action> {
                GenerateOptions o;
                o.genre = stringParam(p, "genre", "house");
                o.key = stringParam(p, "key", "C minor");
                o.seed = static_cast<unsigned>(intParam(p, "seed", 1));
                o.bars = static_cast<int>(intParam(p, "bars", 8));
                o.bpm = numberParam(p, "bpm", 0);
                o.drums = boolParam(p, "drums", true);
                o.bass = boolParam(p, "bass", true);
                o.chords = boolParam(p, "chords", true);
                o.melody = boolParam(p, "melody", true);
                auto check = [o]() -> std::string {
                    if (o.bars < 1 || o.bars > 256) return "bars must be 1..256";
                    if (o.bpm != 0 && (o.bpm < 20 || o.bpm > 400)) return "bpm must be 20..400";
                    if (!o.drums && !o.bass && !o.chords && !o.melody) return "turn on at least one of drums, bass, chords, melody";
                    return {};
                };
                if (p.count("song")) {
                    return songCommand(*s, [o, check](SongDoc& d, const Params& q) {
                        if (auto e = check(); !e.empty()) return failed(e);
                        std::string what = generate(d.song, o);
                        if (q.count("save")) {
                            saveSong(d.song, stringParam(q, "save"));
                            d.path = stringParam(q, "save");
                            what += "; saved " + d.path;
                        }
                        return songResult("added " + what, d, q);
                    })(p);
                }
                return std::make_unique<GenerateNew>(*s, o, p, check());
            });

    auto pictureParams = [] {
        return std::vector<ParamSpec>{
            songOrFileParam(),
            inputParam(),
            {"view", ParamType::String, "what to draw", false, {"arrangement", "pattern", "waveform"}, std::string("arrangement")},
            patternParam(false),
            {"width", ParamType::Int, "image width in pixels", false, {}, std::int64_t{1200}},
            {"start_bar", ParamType::Int, "waveform: first bar", false, {}, std::int64_t{0}},
            {"bars", ParamType::Int, "waveform: how many bars", false},
            {"track", ParamType::String, "waveform: just this track", false}};
    };
    reg.add({"beats.preview",
             "Preview song",
             "A picture of the song: view=arrangement (a lane per track, bars across, notes and hits drawn in), view=pattern "
             "(one pattern's step grid and piano roll; pattern= alone picks this view) or view=waveform (renders the audio). "
             "beats.draw writes the same picture to a PNG file.",
             pictureParams(),
             false, true},
            songCommand(session, picture, false));

    reg.add({"beats.draw",
             "Draw song",
             "Write a picture of the song to a PNG file: the arrangement, one pattern's step grid and piano roll, or the "
             "rendered waveform (the same pictures beats.preview attaches).",
             [&] {
                 auto v = pictureParams();
                 v.insert(v.begin() + 1, {"path", ParamType::String, "PNG file to write", true});
                 return v;
             }(),
             false},
            songCommand(
                session,
                [](SongDoc& d, const Params& p) {
                    Result r = picture(d, p);
                    if (!r.ok || r.attachments.empty()) return r;
                    const std::string path = stringParam(p, "path");
                    if (auto parent = fs::path(path).parent_path(); !parent.empty()) {
                        std::error_code ec;
                        fs::create_directories(parent, ec);
                    }
                    std::ofstream out(path, std::ios::binary);
                    const std::string& bytes = r.attachments[0].bytes;
                    if (!out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()))) return failed("can't write " + path);
                    r.attachments.clear();
                    r.message = "wrote " + path + " (" + r.message + ")";
                    r.with("path", path);
                    return r;
                },
                false));

    reg.add({"beats.presets",
             "Sounds and genres",
             "Lists what's built in: synth presets with their settings, oscillator shapes, drum kit voices (with their General "
             "MIDI numbers), effects and generator genres.",
             {},
             false, true},
            [](const Params&) {
                return std::make_unique<FunctionAction>([] {
                    std::string m = "synth presets:";
                    for (const auto& n : presetNames()) {
                        SynthSettings st;
                        preset(n, st);
                        m += "\n  " + n + ": " + toString(st.osc1) + (st.osc2 != Osc::Off ? " + " + toString(st.osc2) : "") + ", attack " + num(st.attack, 3) +
                             " s, decay " + num(st.decay, 3) + " s, sustain " + num(st.sustain, 2) + ", release " + num(st.release, 3) + " s, cutoff " +
                             num(st.cutoff, 4) + " Hz, resonance " + num(st.resonance, 2);
                    }
                    std::string osc, voices, genres, presets;
                    for (const auto& n : oscNames()) osc += (osc.empty() ? "" : ", ") + n;
                    for (const auto& n : drumVoices()) voices += (voices.empty() ? "" : ", ") + n + " (" + std::to_string(drumMidi(n)) + ")";
                    for (const auto& n : genreNames()) genres += (genres.empty() ? "" : ", ") + n;
                    for (const auto& n : presetNames()) presets += (presets.empty() ? "" : ", ") + n;
                    m += "\noscillators: " + osc + "\ndrum voices: " + voices + "\neffects: distortion, compress, delay, reverb\ngenres: " + genres;
                    return Result::success(m).with("presets", presets).with("oscillators", osc).with("drum_voices", voices).with("genres", genres);
                });
            });
}

}  // namespace tool
}  // namespace wetwaresuite::beats
