// Multitrack projects: tracks, clips, mixing, ducking, export.
#include <algorithm>
#include <cmath>

#include "common.hpp"
#include "wetwaresuite/audio/rack.hpp"

namespace wetwaresuite::audio::tool {

namespace {

ParamSpec trackParam() { return {"track", ParamType::Int, "track number, from 0", true}; }
ParamSpec clipParam() { return {"clip", ParamType::Int, "clip number on the track, from 0", true}; }

bool trackArg(ProjectDoc& d, const Params& p, Track*& out, std::string& error) {
    const auto t = intParam(p, "track", -1);
    if (t < 0 || t >= static_cast<std::int64_t>(d.project.tracks.size())) {
        error = "no track " + std::to_string(t) + " (the project has " + std::to_string(d.project.tracks.size()) + ", numbered from 0)";
        return false;
    }
    out = &d.project.tracks[static_cast<std::size_t>(t)];
    return true;
}

bool clipArg(Track& t, const Params& p, Clip*& out, std::string& error) {
    const auto c = intParam(p, "clip", -1);
    if (c < 0 || c >= static_cast<std::int64_t>(t.clips.size())) {
        error = "no clip " + std::to_string(c) + " on that track (it has " + std::to_string(t.clips.size()) + ")";
        return false;
    }
    out = &t.clips[static_cast<std::size_t>(c)];
    return true;
}

Result projectResult(const std::string& message, const ProjectDoc& d, const Params& p) {
    Result r = Result::success(message);
    r.with("project", stringParam(p, "project"))
        .with("tracks", static_cast<std::int64_t>(d.project.tracks.size()))
        .with("seconds", d.project.seconds());
    return maybePreview(r, d, p);
}

std::string listing(const Project& pr) {
    std::string s;
    bool anySolo = false;
    for (const auto& t : pr.tracks) anySolo = anySolo || t.solo;
    for (std::size_t i = 0; i < pr.tracks.size(); ++i) {
        const Track& t = pr.tracks[i];
        s += "\ntrack " + std::to_string(i) + " \"" + t.name + "\": gain " + dbText(t.gainDb) + ", pan " + num(t.pan, 3);
        if (t.mute) s += ", muted";
        if (t.solo) s += ", solo";
        if (anySolo && !t.solo) s += " (silent: another track is solo)";
        if (t.duckUnder >= 0) s += ", ducks " + dbText(t.duckDb) + " under track " + std::to_string(t.duckUnder);
        if (!t.envelope.empty()) s += ", volume envelope with " + std::to_string(t.envelope.size()) + " points";
        for (const auto& snd : t.sends) s += ", sends " + dbText(snd.levelDb) + " to bus " + std::to_string(snd.bus);
        for (std::size_t f = 0; f < t.fx.size(); ++f) s += "\n  fx " + std::to_string(f) + " " + describeEffect(t.fx[f]);
        for (const auto& l : t.automation)
            if (!l.points.empty()) s += "\n  automation " + l.target + " (" + std::to_string(l.points.size()) + " points)";
        for (std::size_t c = 0; c < t.clips.size(); ++c) {
            const Clip& cl = t.clips[c];
            s += "\n  clip " + std::to_string(c) + " \"" + cl.name + "\": " + formatTime(cl.at) + " to " + formatTime(cl.end());
            if (cl.gainDb != 0) s += ", gain " + dbText(cl.gainDb);
            if (cl.fadeIn > 0) s += ", fade in " + formatTime(cl.fadeIn);
            if (cl.fadeOut > 0) s += ", fade out " + formatTime(cl.fadeOut);
        }
    }
    for (std::size_t b = 0; b < pr.buses.size(); ++b) {
        const Bus& bus = pr.buses[b];
        s += "\nbus " + std::to_string(b) + " \"" + bus.name + "\": gain " + dbText(bus.gainDb) + (bus.mute ? ", muted" : "");
        for (std::size_t f = 0; f < bus.fx.size(); ++f) s += "\n  fx " + std::to_string(f) + " " + describeEffect(bus.fx[f]);
    }
    for (std::size_t f = 0; f < pr.masterFx.size(); ++f) s += "\nmaster fx " + std::to_string(f) + " " + describeEffect(pr.masterFx[f]);
    for (const auto& l : pr.masterAutomation)
        if (!l.points.empty()) s += "\nmaster automation " + l.target + " (" + std::to_string(l.points.size()) + " points)";
    for (const auto& m : pr.markers) s += "\nmarker \"" + m.name + "\" " + (m.region() ? formatTime(m.start) + " to " + formatTime(m.end) : "at " + formatTime(m.start));
    return s;
}

std::vector<ParamSpec> clipSettings() {
    return {{"gain", ParamType::Number, "clip gain in dB", false},
            {"fade_in", ParamType::Number, "clip fade-in, seconds", false},
            {"fade_out", ParamType::Number, "clip fade-out, seconds", false},
            {"fade_in_curve", ParamType::String, "fade-in shape (default s_curve)", false, fadeCurveNames()},
            {"fade_out_curve", ParamType::String, "fade-out shape (default s_curve)", false, fadeCurveNames()},
            {"name", ParamType::String, "clip name", false}};
}

void applyClipSettings(Clip& c, const Params& p) {
    if (p.count("gain")) c.gainDb = numberParam(p, "gain");
    if (p.count("fade_in")) c.fadeIn = std::max(0.0, numberParam(p, "fade_in"));
    if (p.count("fade_out")) c.fadeOut = std::max(0.0, numberParam(p, "fade_out"));
    if (p.count("name")) c.name = stringParam(p, "name");
    if (p.count("fade_in_curve")) parseFadeCurve(stringParam(p, "fade_in_curve"), c.fadeInCurve);
    if (p.count("fade_out_curve")) parseFadeCurve(stringParam(p, "fade_out_curve"), c.fadeOutCurve);
}

// Mixdown, optionally normalized.
bool renderMix(const ProjectDoc& d, const Params& p, Buffer& out, std::string& note, std::string& error) {
    if (d.project.seconds() <= 0) {
        error = "the project has no clips yet (add some with audio.clip_add)";
        return false;
    }
    out = mixdown(d.project);
    const std::string mode = stringParam(p, "normalize", "none");
    if (mode == "peak") {
        const double t = numberParam(p, "target", -1.0);
        note = ", peak normalized (" + dbText(normalizePeak(out, t, all(out))) + ")";
    } else if (mode == "loudness") {
        const double t = numberParam(p, "target", -16.0);
        normalizeLoudness(out, t, numberParam(p, "ceiling", -1.0));
        note = ", loudness " + num(integratedLoudness(out, all(out)), 3) + " LUFS";
    }
    return true;
}

std::vector<ParamSpec> normalizeParams() {
    return {{"normalize", ParamType::String, "level the mix", false, {"none", "peak", "loudness"}, std::string("none")},
            {"target", ParamType::Number, "peak dBFS (default -1) or LUFS (default -16)", false},
            {"ceiling", ParamType::Number, "loudness mode: peak ceiling, dBFS", false, {}, -1.0}};
}

}  // namespace

void registerProjectCommands(Session& session) {
    auto& reg = session.registry();
    Session* s = &session;

    reg.add({"audio.project_new",
             "New project",
             "Start a multitrack project (proj1): tracks of clips at time offsets, mixed down to one file. Clips are "
             "copies of audio documents or files, converted to the project's rate and channels.",
             {{"rate", ParamType::Int, "sample rate in Hz", false, {}, std::int64_t{44100}},
              {"channels", ParamType::Int, "1 = mono, 2 = stereo", false, {}, std::int64_t{2}},
              {"tracks", ParamType::Int, "how many empty tracks to start with", false, {}, std::int64_t{0}}},
             false, true},
            [s](const Params& p) {
                return std::make_unique<FunctionAction>([s, p] {
                    const auto rate = intParam(p, "rate", 44100), ch = intParam(p, "channels", 2), tracks = intParam(p, "tracks", 0);
                    if (rate < 1000 || rate > 768000) return failed("rate must be 1000..768000 Hz");
                    if (ch < 1 || ch > 8) return failed("channels must be 1..8");
                    if (tracks < 0 || tracks > 64) return failed("tracks must be 0..64");
                    auto doc = std::make_shared<ProjectDoc>();
                    doc->project.rate = static_cast<int>(rate);
                    doc->project.channels = static_cast<int>(ch);
                    for (std::int64_t i = 0; i < tracks; ++i) doc->project.tracks.push_back({"Track " + std::to_string(i + 1)});
                    const std::string id = s->workspace().add(doc, "proj");
                    return Result::success("made " + id + ": " + doc->summary()).with("project", id);
                });
            });

    reg.add({"audio.project_open",
             "Open project",
             "Open a project saved with audio.project_save (a .json file plus its _clips folder).",
             {{"path", ParamType::String, "project .json file", true}, previewParam()},
             false, true},
            [s](const Params& p) {
                return std::make_unique<FunctionAction>([s, p] {
                    try {
                        auto doc = std::make_shared<ProjectDoc>();
                        doc->project = loadProject(stringParam(p, "path"));
                        doc->path = stringParam(p, "path");
                        const std::string id = s->workspace().add(doc, "proj");
                        Params q = p;
                        q["project"] = id;
                        return projectResult("opened " + doc->path + " as " + id + ": " + doc->summary() + listing(doc->project), *doc, q);
                    } catch (const std::exception& e) {
                        return failed(e.what());
                    }
                });
            });

    reg.add({"audio.project_save",
             "Save project",
             "Save the project as JSON, with each clip's audio as a 32-bit float WAV in <name>_clips/ beside it.",
             {projectParam(), {"path", ParamType::String, "file to write, e.g. episode.json", true}},
             false},
            projectCommand(
                session,
                [](ProjectDoc& d, const Params& p) {
                    const fs::path path = stringParam(p, "path");
                    saveProject(d.project, path);
                    d.path = path.string();
                    return Result::success("saved " + path.string() + " (clips in " + (path.parent_path() / (path.stem().string() + "_clips")).string() + ")")
                        .with("path", path.string());
                },
                false));

    reg.add({"audio.project_info",
             "Project info",
             "List a project's tracks (gain, pan, mute, solo, ducking) and clips (position, length, fades). preview=true "
             "draws the tracks as lanes (numbered, each drawn as it will sound, muted tracks grey); audio.render "
             "project= output= writes that picture to a file.",
             {projectParam(), previewParam()},
             false, true},
            projectCommand(
                session, [](ProjectDoc& d, const Params& p) { return projectResult(stringParam(p, "project") + ": " + d.summary() + listing(d.project), d, p); },
                false));

    reg.add({"audio.track_add",
             "Add track",
             "Add an empty track to the project; returns its number.",
             {projectParam(), {"name", ParamType::String, "track name", false}, {"gain", ParamType::Number, "dB", false, {}, 0.0},
              {"pan", ParamType::Number, "-1 left .. 1 right", false, {}, 0.0}},
             true},
            projectCommand(session, [](ProjectDoc& d, const Params& p) {
                Track t;
                t.name = stringParam(p, "name", "Track " + std::to_string(d.project.tracks.size() + 1));
                t.gainDb = numberParam(p, "gain", 0.0);
                t.pan = std::clamp(numberParam(p, "pan", 0.0), -1.0, 1.0);
                d.project.tracks.push_back(t);
                const auto index = static_cast<std::int64_t>(d.project.tracks.size() - 1);
                return projectResult("added track " + std::to_string(index) + " \"" + t.name + "\"", d, p).with("track", index);
            }));

    reg.add({"audio.track_set",
             "Track settings",
             "Change a track: name, gain (dB), pan (-1..1), mute, solo; ducking (duck_under=1 makes this track drop by "
             "duck_db whenever track 1 is louder than duck_threshold, e.g. music under a voice; -1 turns it off); and a "
             "volume envelope as time,dB pairs (envelope=0,0,10,-6,12,-60 fades to -6 dB by 10 s, then out).",
             {projectParam(), trackParam(), {"name", ParamType::String, "track name", false}, {"gain", ParamType::Number, "dB", false},
              {"pan", ParamType::Number, "-1 left .. 1 right", false}, {"mute", ParamType::Bool, "silence this track", false},
              {"solo", ParamType::Bool, "play only solo tracks", false},
              {"duck_under", ParamType::Int, "track whose sound turns this one down (-1 = off)", false},
              {"duck_db", ParamType::Number, "how far to duck, dB", false}, {"duck_threshold", ParamType::Number, "dBFS that counts as the key track sounding", false},
              {"duck_attack", ParamType::Number, "seconds to duck down", false}, {"duck_release", ParamType::Number, "seconds to come back", false},
              {"envelope", ParamType::NumberList, "time,dB pairs; an empty list clears it", false}, previewParam()},
             true},
            projectCommand(session, [](ProjectDoc& d, const Params& p) {
                Track* t = nullptr;
                std::string error;
                if (!trackArg(d, p, t, error)) return failed(error);
                if (p.count("name")) t->name = stringParam(p, "name");
                if (p.count("gain")) t->gainDb = numberParam(p, "gain");
                if (p.count("pan")) {
                    const double pan = numberParam(p, "pan");
                    if (pan < -1 || pan > 1) return failed("pan must be -1..1");
                    t->pan = pan;
                }
                if (p.count("mute")) t->mute = boolParam(p, "mute");
                if (p.count("solo")) t->solo = boolParam(p, "solo");
                if (p.count("duck_under")) {
                    const auto k = intParam(p, "duck_under");
                    if (k == intParam(p, "track")) return failed("a track can't duck under itself");
                    if (k >= static_cast<std::int64_t>(d.project.tracks.size())) return failed("no track " + std::to_string(k) + " to duck under");
                    t->duckUnder = static_cast<int>(std::max<std::int64_t>(-1, k));
                }
                if (p.count("duck_db")) t->duckDb = -std::fabs(numberParam(p, "duck_db"));
                if (p.count("duck_threshold")) t->duckThresholdDb = numberParam(p, "duck_threshold");
                if (p.count("duck_attack")) t->duckAttack = std::max(0.0, numberParam(p, "duck_attack"));
                if (p.count("duck_release")) t->duckRelease = std::max(0.0, numberParam(p, "duck_release"));
                if (p.count("envelope")) {
                    const NumberList e = listParam(p, "envelope");
                    if (e.size() % 2) return failed("envelope takes time,dB pairs");
                    t->envelope.clear();
                    for (std::size_t i = 0; i + 1 < e.size(); i += 2) t->envelope.push_back({e[i], e[i + 1]});
                }
                return projectResult("track " + std::to_string(intParam(p, "track")) + " updated" + listing([&] {
                                         Project one;
                                         one.tracks = {*t};
                                         return one;
                                     }()),
                                     d, p);
            }));

    reg.add({"audio.track_remove", "Remove track", "Delete a track and its clips (later tracks move up one number).", {projectParam(), trackParam()}, true},
            projectCommand(session, [](ProjectDoc& d, const Params& p) {
                Track* t = nullptr;
                std::string error;
                if (!trackArg(d, p, t, error)) return failed(error);
                const auto index = intParam(p, "track");
                d.project.tracks.erase(d.project.tracks.begin() + index);
                for (auto& other : d.project.tracks) {
                    if (other.duckUnder == index) other.duckUnder = -1;
                    else if (other.duckUnder > index) --other.duckUnder;
                }
                return projectResult("removed track " + std::to_string(index), d, p);
            }));

    reg.add({"audio.clip_add",
             "Add clip",
             "Put audio on a track at `at` seconds (default: right after the track's last clip): an open document "
             "(doc=aud1) or a file (input=). start/end take just part of it. The clip is a copy, so later edits to the "
             "document don't change it.",
             concat({projectParam(), trackParam(), {"doc", ParamType::String, "audio document to place", false},
                     {"input", ParamType::String, "or an audio file to place", false}, {"at", ParamType::Number, "position in seconds", false},
                     {"start", ParamType::Number, "use the source from here, seconds", false}, {"end", ParamType::Number, "until here, seconds", false}},
                    concat(clipSettings(), {previewParam()})),
             true},
            [s](const Params& p) -> std::unique_ptr<Action> {
                Buffer src;
                std::string error, label;
                if (p.count("doc")) {
                    if (auto a = s->workspace().getAs<AudioDoc>(stringParam(p, "doc"))) {
                        src = a->audio;
                        label = a->path.empty() ? stringParam(p, "doc") : fs::path(a->path).stem().string();
                    } else error = "no open audio document called '" + stringParam(p, "doc") + "'";
                } else if (p.count("input")) {
                    try {
                        src = load(stringParam(p, "input"));
                        label = fs::path(stringParam(p, "input")).stem().string();
                    } catch (const std::exception& e) {
                        error = e.what();
                    }
                } else {
                    error = "give doc=aud1 or input=file.wav";
                }
                if (!error.empty()) return std::make_unique<FunctionAction>([error] { return failed(error); });
                return projectCommand(*s, [src, label](ProjectDoc& d, const Params& q) {
                    Track* t = nullptr;
                    std::string err;
                    if (!trackArg(d, q, t, err)) return failed(err);
                    Range r;
                    if (!rangeArg(src, q, r, err)) return failed(err);
                    Buffer part = slice(src, r);
                    if (part.empty()) return failed("that part of the source is empty");
                    if (part.rate != d.project.rate) part = resample(part, d.project.rate);
                    Clip c;
                    c.name = label;
                    c.audio = std::make_shared<const Buffer>(std::move(part));
                    c.at = q.count("at") ? numberParam(q, "at") : t->end();
                    if (c.at < 0) return failed("at can't be negative");
                    applyClipSettings(c, q);
                    t->clips.push_back(c);
                    const auto index = static_cast<std::int64_t>(t->clips.size() - 1);
                    return projectResult("added clip " + std::to_string(index) + " \"" + c.name + "\" to track " + std::to_string(intParam(q, "track")) +
                                             " at " + formatTime(c.at) + " (" + formatTime(c.seconds()) + ")",
                                         d, q)
                        .with("clip", index)
                        .with("clip_end", c.end());
                })(p);
            });

    reg.add({"audio.clip_set",
             "Clip settings",
             "Move a clip (at = new position in seconds, to_track = another track) or change its gain, fades or name.",
             concat({projectParam(), trackParam(), clipParam(), {"at", ParamType::Number, "new position in seconds", false},
                     {"to_track", ParamType::Int, "move it to this track", false}},
                    concat(clipSettings(), {previewParam()})),
             true},
            projectCommand(session, [](ProjectDoc& d, const Params& p) {
                Track* t = nullptr;
                Clip* c = nullptr;
                std::string error;
                if (!trackArg(d, p, t, error) || !clipArg(*t, p, c, error)) return failed(error);
                if (p.count("at")) {
                    if (numberParam(p, "at") < 0) return failed("at can't be negative");
                    c->at = numberParam(p, "at");
                }
                applyClipSettings(*c, p);
                if (p.count("to_track")) {
                    const auto to = intParam(p, "to_track");
                    if (to < 0 || to >= static_cast<std::int64_t>(d.project.tracks.size())) return failed("no track " + std::to_string(to));
                    Clip moved = *c;
                    t->clips.erase(t->clips.begin() + intParam(p, "clip"));
                    d.project.tracks[static_cast<std::size_t>(to)].clips.push_back(moved);
                    return projectResult("moved the clip to track " + std::to_string(to), d, p);
                }
                return projectResult("clip now at " + formatTime(c->at) + " to " + formatTime(c->end()), d, p);
            }));

    reg.add({"audio.clip_remove", "Remove clip", "Delete a clip from a track.", {projectParam(), trackParam(), clipParam()}, true},
            projectCommand(session, [](ProjectDoc& d, const Params& p) {
                Track* t = nullptr;
                Clip* c = nullptr;
                std::string error;
                if (!trackArg(d, p, t, error) || !clipArg(*t, p, c, error)) return failed(error);
                t->clips.erase(t->clips.begin() + intParam(p, "clip"));
                return projectResult("removed clip " + std::to_string(intParam(p, "clip")), d, p);
            }));

    reg.add({"audio.clip_trim",
             "Trim clip",
             "Change which part of a clip plays, without losing the rest: start = new start on the timeline (the end "
             "stays put, earlier reveals audio cut off before), length = new length in seconds (the start stays put).",
             {projectParam(), trackParam(), clipParam(), {"start", ParamType::Number, "new start in seconds; the end stays", false},
              {"length", ParamType::Number, "new length in seconds; the start stays", false}, previewParam()},
             true},
            projectCommand(session, [](ProjectDoc& d, const Params& p) {
                Track* t = nullptr;
                Clip* c = nullptr;
                std::string error;
                if (!trackArg(d, p, t, error) || !clipArg(*t, p, c, error)) return failed(error);
                if (!p.count("start") && !p.count("length")) return failed("give start and/or length");
                if (p.count("start")) {
                    const double end = c->end();
                    // can't reveal more than the audio has before the current offset, or go past the end
                    const double start = std::clamp(numberParam(p, "start"), std::max(0.0, c->at - c->offset), end - 0.001);
                    c->offset += start - c->at;
                    c->at = start;
                    c->length = end - start;
                }
                if (p.count("length")) {
                    const double len = numberParam(p, "length");
                    if (len <= 0) return failed("length must be more than 0");
                    c->length = std::min(len, c->sourceSeconds() - c->offset);
                }
                return projectResult("clip now plays " + formatTime(c->at) + " to " + formatTime(c->end()), d, p)
                    .with("start", c->at)
                    .with("clip_end", c->end());
            }));

    reg.add({"audio.clip_split",
             "Split clip",
             "Cut a clip in two at a time on the timeline. The second part is added after it on the same track (its number is returned).",
             {projectParam(), trackParam(), clipParam(), {"at", ParamType::Number, "timeline time to cut at, in seconds", true}},
             true},
            projectCommand(session, [](ProjectDoc& d, const Params& p) {
                Track* t = nullptr;
                Clip* c = nullptr;
                std::string error;
                if (!trackArg(d, p, t, error) || !clipArg(*t, p, c, error)) return failed(error);
                const double at = numberParam(p, "at");
                if (at <= c->at + 1e-3 || at >= c->end() - 1e-3) return failed("at must be inside the clip (" + formatTime(c->at) + " to " + formatTime(c->end()) + ")");
                Clip second = *c;
                const double cut = at - c->at;
                c->length = cut;
                c->fadeOut = 0;
                second.at = at;
                second.offset = c->offset + cut;
                second.length = second.length < 0 ? -1 : std::max(0.0, second.length - cut);
                second.fadeIn = 0;
                const auto index = static_cast<std::int64_t>(intParam(p, "clip")) + 1;
                t->clips.insert(t->clips.begin() + index, second);
                return projectResult("split at " + formatTime(at), d, p).with("clip", index);
            }));

    reg.add({"audio.mixdown",
             "Mix down",
             "Mix every audible track into a new audio document (aud1...) to edit, measure or save; the project stays as it is.",
             concat({projectParam()}, concat(normalizeParams(), {previewParam()})),
             false, true},
            projectCommand(
                session,
                [s](ProjectDoc& d, const Params& p) {
                    Buffer mix;
                    std::string note, error;
                    if (!renderMix(d, p, mix, note, error)) return failed(error);
                    auto doc = newAudio(std::move(mix));
                    const std::string id = s->workspace().add(doc, "aud");
                    Params q = p;
                    q["doc"] = id;
                    const Levels lv = measure(doc->audio, all(doc->audio), false);
                    Result r = audioResult("mixed " + stringParam(p, "project") + " into " + id + ": " + describe(doc->audio) + note + ", peak " +
                                               dbText(lv.peakDb) + (lv.clipped ? " (clipping: normalize or lower track gains)" : ""),
                                           *doc, q);
                    return r.with("peak_db", lv.peakDb).with("lufs", lv.lufs);
                },
                false));

    reg.add({"audio.export",
             "Export mix",
             "Mix the project down and write it straight to a file (wav, flac, mp3, ogg, opus, m4a...), optionally "
             "normalized: normalize=loudness target=-16 for a podcast.",
             concat({projectParam(), {"path", ParamType::String, "file to write, e.g. episode.mp3", true},
                     {"bits", ParamType::String, "WAV/FLAC sample format", false, {"8", "16", "24", "32", "32f"}, std::string("16")},
                     {"bitrate", ParamType::Int, "kbit/s for lossy formats (0 = default)", false, {}, std::int64_t{0}},
                     {"rate", ParamType::Int, "write at this sample rate (default: the project's)", false}},
                    normalizeParams()),
             false},
            projectCommand(
                session,
                [](ProjectDoc& d, const Params& p) {
                    const fs::path path = stringParam(p, "path");
                    if (!canWrite(path)) return failed("can't write ." + lowerExt(path) + " here (non-WAV formats need ffmpeg on PATH)");
                    SaveOptions o;
                    Buffer mix;
                    std::string note, error;
                    if (!saveOptionsArg(p, o, error) || !renderMix(d, p, mix, note, error)) return failed(error);
                    const auto rate = intParam(p, "rate", 0);
                    if (p.count("rate") && (rate < 1000 || rate > 768000)) return failed("rate must be 1000..768000 Hz");
                    if (rate > 0 && rate != mix.rate) mix = resample(mix, static_cast<int>(rate));
                    save(mix, path, o);
                    const Levels lv = measure(mix, all(mix), false);
                    std::error_code ec;
                    return Result::success("exported " + path.string() + ": " + describe(mix) + note + ", peak " + dbText(lv.peakDb))
                        .with("path", path.string())
                        .with("seconds", mix.seconds())
                        .with("lufs", lv.lufs)
                        .with("peak_db", lv.peakDb)
                        .with("bytes", static_cast<std::int64_t>(fs::file_size(path, ec)));
                },
                false));
}

}  // namespace wetwaresuite::audio::tool
