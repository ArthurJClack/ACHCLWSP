// Sequence-level editing: insert and overwrite edits at the playhead, lift and
// extract between in/out points, speed changes (rate stretch, reverse) and
// freeze frames, markers, and nested sequences.
#include <algorithm>
#include <cmath>
#include <map>
#include <set>

#include "internal.hpp"
#include "wetwaresuite/cut/render.hpp"

namespace wetwaresuite::cut {

namespace {

const std::vector<std::string> kMarkerColours = {"green", "red", "blue", "yellow", "orange", "purple", "cyan", "pink", "white"};

// The clip a file makes, picture or sound (picked by the track when given).
Clip fileClip(const Params& p) {
    const std::string path = stringParam(p, "path");
    if (has(p, "track")) return clipForFile(path, stringParam(p, "track")[0] == 'a');
    try {
        return clipForFile(path, false);
    } catch (const std::exception&) {
        return clipForFile(path, true);
    }
}

// How long a clip will be once the timing params are applied.
double plannedLength(Clip c, const Params& p) {
    Params q;
    for (const char* k : {"in", "out", "duration", "speed"}) {
        if (has(p, k)) q[k] = p.at(k);
    }
    if (std::string err = applyProps(c, q); !err.empty()) throw std::runtime_error(err);
    if (std::string err = validate(c); !err.empty()) throw std::runtime_error(err);
    return c.length();
}

std::vector<ParamSpec> placeSpecs() {
    std::vector<ParamSpec> s{docParam(), {"path", ParamType::String, "video, image or audio file", true},
                             {"track", ParamType::String, "track to put it on (default v1, or a1 for sound files)", false},
                             {"at", ParamType::Number, "timeline time (default the playhead)", false},
                             {"audio_track", ParamType::String, "where a video's sound goes: auto, a1/a2/..., embedded or none", false, {}, std::string("auto")}};
    for (auto& x : propSpecs({"in", "out", "duration", "speed"})) s.push_back(x);
    s.push_back(previewParam());
    return s;
}

Result placeEdit(Timeline& tl, const Params& p, bool insert) {
    Clip c = fileClip(p);
    const std::string track = stringParam(p, "track", c.kind == "audio" ? "a1" : "v1");
    const double at = std::max(0.0, numberParam(p, "at", tl.cursor));
    const double len = plannedLength(c, p);
    Params q = p;
    q["start"] = at;
    q["track"] = track;
    tl.ensureTrack(track);
    std::vector<std::pair<std::string, std::string>> parts;
    if (insert) {
        // every unlocked track opens up at `at` so everything after stays in sync
        for (auto& t : tl.tracks) {
            if (t.locked) continue;
            std::vector<std::string> under;
            for (const auto& x : t.clips) {
                if (at > x.start + 1e-3 && at < x.end() - 1e-3) under.push_back(x.id);
            }
            for (const auto& id : under) {
                const std::string link = tl.findClip(id)->link;
                parts.push_back({link, splitClip(tl, t, id, at)});
            }
            rippleTrack(t, at, len);
        }
    } else {
        std::vector<std::string> clear{track};
        const std::string audioTrack = stringParam(p, "audio_track", "auto");
        if (c.kind == "video" && c.hasAudio && audioTrack != "none" && audioTrack != "embedded") {
            const std::string a = audioTrack == "auto" ? std::string("a1") : audioTrack;
            q["audio_track"] = a;
            clear.push_back(a);
        }
        for (const auto& id : clear) tl.ensureTrack(id);
        for (const auto& id : clear) {
            auto made = clearRange(tl, *tl.track(id), at, at + len);
            parts.insert(parts.end(), made.begin(), made.end());
        }
    }
    relinkParts(tl, parts);
    tl.cursor = at + len;
    Result r = addClip(tl, q, c, track, c.kind == "audio" ? "audio" : c.kind == "image" ? "still" : "clip");
    r.message = std::string(insert ? "inserted" : "overwrote with") + r.message.substr(std::string("added").size());
    return r;
}

ParamSpec rangeTracks() { return {"tracks", ParamType::String, "only these tracks, comma separated (default every unlocked track)", false}; }

Result rangeEdit(Timeline& tl, const Params& p, bool extract) {
    const double from = has(p, "from") ? numberParam(p, "from") : tl.inPoint;
    const double to = has(p, "to") ? numberParam(p, "to") : tl.outPoint;
    if (from < 0 || to < 0) return Result::failure("set in and out points first (cut.set_in_out), or give from and to");
    if (to <= from + 1e-3) return Result::failure("the out point must be after the in point");
    std::vector<std::pair<std::string, std::string>> parts;
    const auto tracks = targetTracks(tl, stringParam(p, "tracks"));
    for (const auto& id : tracks) {
        auto made = clearRange(tl, *tl.track(id), from, to);
        parts.insert(parts.end(), made.begin(), made.end());
        if (extract) rippleTrack(*tl.track(id), from, -(to - from));
    }
    relinkParts(tl, parts);
    tl.cursor = from;
    Result r = Result::success(std::string(extract ? "extracted " : "lifted ") + num(from) + "-" + num(to) + "s from " +
                               std::to_string(tracks.size()) + " track" + (tracks.size() == 1 ? "" : "s") +
                               (extract ? " and closed the gap" : ", leaving a gap") + " (timeline " + num(tl.duration()) + "s)");
    return r.with("doc", stringParam(p, "doc")).with("duration", tl.duration());
}

void registerSpeedCommands(Session& session) {
    auto& reg = session.registry();
    reg.add({"cut.clip_speed", "Clip Speed",
             "Change a clip's speed (and its linked sound's): 2 = twice as fast, 0.5 = slow motion, or give "
             "duration to stretch it to fit (rate stretch). reverse=true plays it backwards. The sound keeps its "
             "pitch. With ripple (default) the rest of the track moves to fit the new length.",
             {docParam(), clipParam(), {"speed", ParamType::Number, "new speed, 0.05..20", false},
              {"duration", ParamType::Number, "or: the new length on the timeline, in seconds", false},
              {"reverse", ParamType::Bool, "play backwards", false},
              {"ripple", ParamType::Bool, "move the clips after it", false, {}, true}, linkedParam(), previewParam()},
             true},
            cutCommand(session, [](Timeline& tl, const Params& p) {
                Clip* c = requireClip(tl, p);
                if (!c->timed()) return Result::failure("only video, audio and nested clips have a speed; set duration on stills and titles");
                if (c->hold) return Result::failure("a freeze frame has no speed; set its duration");
                if (!has(p, "speed") && !has(p, "duration") && !has(p, "reverse")) return Result::failure("give speed, duration or reverse");
                const std::string first = c->id;
                double speed = c->speed;
                if (has(p, "speed")) speed = numberParam(p, "speed");
                if (has(p, "duration")) {
                    const double d = numberParam(p, "duration");
                    if (d <= 0.01) return Result::failure("duration must be more than 0");
                    speed = (c->out - c->in) / d;
                }
                std::vector<std::string> ids{first};
                if (boolParam(p, "linked", true)) {
                    for (Clip* x : group(tl, *c, true))
                        if (x->id != first) ids.push_back(x->id);
                }
                for (const auto& id : ids) {
                    Track* t = trackOf(tl, id);
                    Clip& x = *tl.findClip(id);
                    if (!x.timed() || x.hold) continue;
                    const double oldLen = x.length();
                    x.speed = speed;
                    if (has(p, "reverse")) x.reverse = boolParam(p, "reverse");
                    if (std::string err = validate(x); !err.empty()) return Result::failure(err);
                    const double newLen = x.length();
                    for (auto& [name, ks] : x.keys) {
                        for (auto& k : ks) k.t *= newLen / oldLen;
                    }
                    if (boolParam(p, "ripple", true)) rippleTrack(*t, x.start + 1e-3, newLen - oldLen, {x.id});
                }
                const Clip* now = tl.findClip(first);
                return clipResult(first + " now plays at " + num(now->speed * 100) + "%" + (now->reverse ? " in reverse" : "") + ", " +
                                      num(now->length()) + "s long",
                                  tl, p, first);
            }));

    reg.add({"cut.freeze_frame", "Freeze Frame",
             "Hold one frame of a video clip still for a while: the clip is cut at `at` (default the playhead), a "
             "still of that frame is put in for `duration` seconds, and the rest of the clip (and the track, and its "
             "linked sound) moves later.",
             {docParam(), clipParam(), {"at", ParamType::Number, "timeline time of the frame (default the playhead)", false},
              {"duration", ParamType::Number, "how long to hold it, in seconds", false, {}, 2.0}, linkedParam(), previewParam()},
             true},
            cutCommand(session, [](Timeline& tl, const Params& p) {
                Clip* c = requireClip(tl, p);
                if (c->kind != "video" && c->kind != "nest") return Result::failure("freeze frames are made from video clips");
                const double at = numberParam(p, "at", tl.cursor);
                const double dur = numberParam(p, "duration", 2.0);
                if (dur <= 0.01 || dur > 3600) return Result::failure("duration must be 0.01..3600 s");
                if (at < c->start - 1e-3 || at >= c->end() - 1e-3) {
                    return Result::failure("at must be inside " + c->id + " (" + num(c->start) + "-" + num(c->end()) + "s)");
                }
                const std::string id = c->id;
                Clip hold = *c;
                const double local = std::max(0.0, at - c->start);
                for (const auto& prop : animatableProperties()) {
                    if (double* f = propertyField(hold, prop)) *f = c->value(prop, local);
                }
                hold.keys.clear();
                const double src = c->sourceTime(local);
                std::set<std::string> tracks;
                std::vector<std::pair<std::string, std::string>> parts;
                for (Clip* x : group(tl, *c, boolParam(p, "linked", true))) tracks.insert(trackOf(tl, x->id)->id);
                for (const auto& tid : tracks) {
                    Track& t = *tl.track(tid);
                    std::vector<std::string> under;
                    for (const auto& x : t.clips) {
                        if (at > x.start + 1e-3 && at < x.end() - 1e-3 && (x.id == id || (!hold.link.empty() && x.link == hold.link))) under.push_back(x.id);
                    }
                    for (const auto& u : under) {
                        const std::string link = tl.findClip(u)->link;
                        parts.push_back({link, splitClip(tl, t, u, at)});
                    }
                    rippleTrack(t, at, dur);
                }
                relinkParts(tl, parts);
                hold.id = tl.newClipId();
                hold.hold = true;
                hold.in = src;
                hold.out = src + dur;
                hold.speed = 1;
                hold.reverse = false;
                hold.start = at;
                hold.link.clear();
                hold.ownSound = false;
                hold.fadeIn = hold.fadeOut = 0;
                hold.transition.clear();
                hold.transitionDuration = 0;
                trackOf(tl, id)->clips.push_back(hold);
                return clipResult("froze the frame at " + num(src) + "s of the source for " + num(dur) + "s as " + hold.id, tl, p, hold.id);
            }));
}

void registerMarkerCommands(Session& session) {
    auto& reg = session.registry();
    reg.add({"cut.add_marker", "Add Marker",
             "Mark a moment on the timeline (default the playhead) with a name and colour: a beat, a note for later, a "
             "chapter. duration makes it a range. cut.set_cursor to=next_marker jumps between them.",
             {docParam(), {"t", ParamType::Number, "timeline time (default the playhead)", false},
              {"name", ParamType::String, "what it marks", false},
              {"color", ParamType::String, "marker colour", false, kMarkerColours, std::string("green")},
              {"duration", ParamType::Number, "seconds it spans (0 = a point)", false, {}, 0.0},
              {"clip", ParamType::String, "make it a clip marker: it moves with this clip (its time must be on the clip)", false}},
             true},
            cutCommand(session, [](Timeline& tl, const Params& p) {
                Marker m;
                m.id = "m" + std::to_string(tl.nextMarker++);
                m.t = std::max(0.0, numberParam(p, "t", tl.cursor));
                m.name = stringParam(p, "name");
                m.color = stringParam(p, "color", "green");
                m.duration = std::max(0.0, numberParam(p, "duration", 0));
                if (std::find(kMarkerColours.begin(), kMarkerColours.end(), m.color) == kMarkerColours.end()) return Result::failure("unknown marker colour '" + m.color + "'");
                if (has(p, "clip")) {
                    const Clip* c = requireClip(tl, p);
                    if (m.t < c->start - 1e-6 || m.t > c->end() + 1e-6) return Result::failure("the marker's time isn't on " + c->id + " (" + num(c->start) + "-" + num(c->end()) + "s)");
                    m.clip = c->id;
                }
                tl.markers.push_back(m);
                std::sort(tl.markers.begin(), tl.markers.end(), [](const Marker& a, const Marker& b) { return a.t < b.t; });
                Result r = Result::success("marker " + m.id + " at " + num(m.t) + "s" + (m.name.empty() ? "" : " \"" + m.name + "\""), m.id);
                return r.with("doc", stringParam(p, "doc")).with("marker", m.id).with("t", m.t);
            }));

    reg.add({"cut.set_marker", "Change Marker", "Move, rename or recolour a marker.",
             {docParam(), {"marker", ParamType::String, "marker id, e.g. m1", true}, {"t", ParamType::Number, "new time", false},
              {"name", ParamType::String, "new name", false}, {"color", ParamType::String, "new colour", false, kMarkerColours},
              {"duration", ParamType::Number, "seconds it spans", false}},
             true},
            cutCommand(session, [](Timeline& tl, const Params& p) {
                const std::string id = stringParam(p, "marker");
                auto it = std::find_if(tl.markers.begin(), tl.markers.end(), [&](const Marker& m) { return m.id == id; });
                if (it == tl.markers.end()) return Result::failure("no marker called '" + id + "'");
                if (has(p, "t")) it->t = std::max(0.0, numberParam(p, "t"));
                if (has(p, "name")) it->name = stringParam(p, "name");
                if (has(p, "duration")) it->duration = std::max(0.0, numberParam(p, "duration"));
                if (has(p, "color")) {
                    it->color = stringParam(p, "color");
                    if (std::find(kMarkerColours.begin(), kMarkerColours.end(), it->color) == kMarkerColours.end()) return Result::failure("unknown marker colour");
                }
                const Marker m = *it;
                std::sort(tl.markers.begin(), tl.markers.end(), [](const Marker& a, const Marker& b) { return a.t < b.t; });
                Result r = Result::success("marker " + m.id + " at " + num(m.t) + "s");
                return r.with("doc", stringParam(p, "doc")).with("marker", m.id).with("t", m.t);
            }));

    reg.add({"cut.remove_marker", "Remove Marker", "Delete a marker, or every marker with all=true.",
             {docParam(), {"marker", ParamType::String, "marker id", false}, {"all", ParamType::Bool, "remove them all", false, {}, false}}, true},
            cutCommand(session, [](Timeline& tl, const Params& p) {
                if (boolParam(p, "all", false)) {
                    const auto n = tl.markers.size();
                    tl.markers.clear();
                    return Result::success("removed " + std::to_string(n) + " markers");
                }
                const std::string id = stringParam(p, "marker");
                auto it = std::find_if(tl.markers.begin(), tl.markers.end(), [&](const Marker& m) { return m.id == id; });
                if (it == tl.markers.end()) return Result::failure("no marker called '" + id + "'");
                tl.markers.erase(it);
                return Result::success("removed marker " + id);
            }));

    reg.add({"cut.markers", "List Markers", "List the markers in time order.", {docParam()}, false, true},
            cutCommand(session, [](Timeline& tl, const Params&) {
                std::string text = tl.markers.empty() ? "no markers (cut.add_marker adds one)" : "";
                for (const auto& m : tl.markers) {
                    text += (text.empty() ? "" : "\n") + m.id + " " + num(m.t) + "s " + m.color + (m.duration > 0 ? " " + num(m.duration) + "s" : "") +
                            (m.name.empty() ? "" : " " + m.name) + (m.clip.empty() ? "" : " (on " + m.clip + ")");
                }
                Result r = Result::success(text);
                return r.with("count", static_cast<std::int64_t>(tl.markers.size()));
            }, false));

    reg.add({"cut.set_in_out", "Set In/Out",
             "Mark the in and out points of the sequence: cut.lift and cut.extract work between them and cut.render "
             "range=in_out exports just that part. clear=true removes them.",
             {docParam(), {"in", ParamType::Number, "in point, seconds", false}, {"out", ParamType::Number, "out point, seconds", false},
              {"clear", ParamType::Bool, "remove both", false, {}, false}},
             true},
            cutCommand(session, [](Timeline& tl, const Params& p) {
                if (boolParam(p, "clear", false)) {
                    tl.inPoint = tl.outPoint = -1;
                    return Result::success("cleared the in and out points");
                }
                if (!has(p, "in") && !has(p, "out")) return Result::failure("give in, out, or clear=true");
                if (has(p, "in")) tl.inPoint = std::max(0.0, numberParam(p, "in"));
                if (has(p, "out")) tl.outPoint = std::max(0.0, numberParam(p, "out"));
                if (tl.inPoint >= 0 && tl.outPoint >= 0 && tl.outPoint <= tl.inPoint + 1e-3) {
                    // a new in after the old out (or the other way) drops the other one, like an editor does
                    if (has(p, "in") && !has(p, "out")) tl.outPoint = -1;
                    else if (has(p, "out") && !has(p, "in")) tl.inPoint = -1;
                    else return Result::failure("the out point must be after the in point");
                }
                Result r = Result::success("in " + (tl.inPoint >= 0 ? num(tl.inPoint) + "s" : std::string("-")) + ", out " +
                                           (tl.outPoint >= 0 ? num(tl.outPoint) + "s" : std::string("-")));
                return r.with("in", tl.inPoint).with("out", tl.outPoint);
            }));
}

void registerNestCommands(Session& session) {
    auto& reg = session.registry();
    reg.add({"cut.nest", "Nest Clips",
             "Put clips (and their linked clips) into a nested sequence that sits on the timeline as one clip: move, "
             "trim, fade, grade or speed it as a whole. cut.unnest puts them back.",
             {docParam(), {"clips", ParamType::String, "clip ids, comma separated", true},
              {"name", ParamType::String, "what to call the nest", false}, previewParam()},
             true},
            cutCommand(session, [](Timeline& tl, const Params& p) {
                std::set<std::string> ids;
                for (const auto& id : splitList(stringParam(p, "clips"))) {
                    Clip* c = tl.findClip(id);
                    if (!c) return Result::failure("no clip called '" + id + "'");
                    for (Clip* x : group(tl, *c, true)) ids.insert(x->id);
                }
                if (ids.empty()) return Result::failure("give the clips to nest");
                double from = 1e18, to = 0;
                std::string place;
                for (const auto& t : tl.tracks) {
                    for (const auto& c : t.clips) {
                        if (!ids.count(c.id)) continue;
                        from = std::min(from, c.start);
                        to = std::max(to, c.end());
                        if (place.empty() || (t.kind == "video" && tl.track(place)->kind == "audio")) place = t.id;
                    }
                }
                auto n = std::make_shared<Timeline>();
                n->settings = tl.settings;
                n->nextClip = tl.nextClip;
                n->nextLink = tl.nextLink;
                for (const auto& t : tl.tracks) n->ensureTrack(t.id);
                for (auto& t : tl.tracks) {
                    for (const auto& c : t.clips) {
                        if (!ids.count(c.id)) continue;
                        Clip x = c;
                        x.start -= from;
                        n->track(t.id)->clips.push_back(x);
                    }
                }
                for (const auto& id : ids) removeClip(tl, *trackOf(tl, id), id);
                n->tracks.erase(std::remove_if(n->tracks.begin(), n->tracks.end(), [](const Track& t) { return t.clips.empty(); }), n->tracks.end());
                n->sortClips();
                for (const auto& c : tl.track(place)->clips) {
                    if (c.start < to - 1e-3 && c.end() > from + 1e-3) return Result::failure(c.id + " is in the way on " + place + "; nest it too or move it");
                }
                Clip nc;
                nc.kind = "nest";
                nc.source = stringParam(p, "name", "Nest " + std::to_string(tl.nextClip));
                nc.in = 0;
                nc.out = n->duration();
                nc.start = from;
                nc.sourceDuration = nc.out;
                nc.sourceWidth = tl.settings.width;
                nc.sourceHeight = tl.settings.height;
                nc.hasAudio = hasSound(*n);
                nc.nested = n;
                nc.id = tl.newClipId();
                tl.track(place)->clips.push_back(nc);
                Result r = clipResult("nested " + std::to_string(ids.size()) + " clips as " + nc.id + " on " + place, tl, p, nc.id);
                return r;
            }));

    reg.add({"cut.unnest", "Unnest", "Put a nested sequence's clips back on the timeline in its place (as far as it's trimmed).",
             {docParam(), clipParam("the nest clip")}, true},
            cutCommand(session, [](Timeline& tl, const Params& p) {
                Clip* c = requireClip(tl, p);
                if (c->kind != "nest" || !c->nested) return Result::failure(c->id + " isn't a nested sequence");
                if (std::abs(c->speed - 1) > kEps || c->reverse || c->hold) return Result::failure("set the nest's speed back to 1 (no reverse or freeze) before unnesting");
                const Clip nest = *c;
                removeClip(tl, *trackOf(tl, nest.id), nest.id);
                const Timeline inside = *nest.nested;
                for (const auto& t : inside.tracks) tl.ensureTrack(t.id);
                std::map<std::string, std::string> links;
                std::int64_t count = 0;
                for (const auto& t : inside.tracks) {
                    Track& dest = *tl.track(t.id);
                    for (Clip x : t.clips) {
                        if (x.end() <= nest.in + 1e-3 || x.start >= nest.out - 1e-3) continue;
                        if (x.start < nest.in) trimStart(x, nest.in - x.start);
                        if (x.end() > nest.out) trimEnd(x, nest.out - x.end());
                        x.start += nest.start - nest.in;
                        x.id = tl.newClipId();
                        if (!x.link.empty()) {
                            if (!links.count(x.link)) links[x.link] = tl.newLinkId();
                            x.link = links[x.link];
                        }
                        for (const auto& o : dest.clips) {
                            if (o.start < x.end() - 1e-3 && o.end() > x.start + 1e-3) return Result::failure(o.id + " on " + t.id + " is in the way");
                        }
                        dest.clips.push_back(x);
                        ++count;
                    }
                }
                Result r = Result::success("unnested " + nest.id + " into " + std::to_string(count) + " clips");
                return r.with("doc", stringParam(p, "doc")).with("clips", count);
            }));
}

}  // namespace

void registerSequenceCommands(Session& session) {
    auto& reg = session.registry();
    reg.add({"cut.insert", "Insert Edit",
             "Put a file on the timeline at the playhead (or at) and push everything after it later on every unlocked "
             "track, splitting clips under the playhead: an insert edit. in/out pick the part of the source.",
             placeSpecs(), true},
            cutCommand(session, [](Timeline& tl, const Params& p) { return placeEdit(tl, p, true); }));
    reg.add({"cut.overwrite", "Overwrite Edit",
             "Put a file on the timeline at the playhead (or at), replacing whatever is there on its track (and its "
             "sound's track) for its length: an overwrite edit. Nothing else moves.",
             placeSpecs(), true},
            cutCommand(session, [](Timeline& tl, const Params& p) { return placeEdit(tl, p, false); }));
    reg.add({"cut.lift", "Lift",
             "Remove everything between the in and out points (or from/to) on every unlocked track (or tracks), "
             "leaving a gap.",
             {docParam(), {"from", ParamType::Number, "start (default the in point)", false}, {"to", ParamType::Number, "end (default the out point)", false}, rangeTracks()},
             true},
            cutCommand(session, [](Timeline& tl, const Params& p) { return rangeEdit(tl, p, false); }));
    reg.add({"cut.extract", "Extract",
             "Remove everything between the in and out points (or from/to) on every unlocked track (or tracks) and "
             "close the gap.",
             {docParam(), {"from", ParamType::Number, "start (default the in point)", false}, {"to", ParamType::Number, "end (default the out point)", false}, rangeTracks()},
             true},
            cutCommand(session, [](Timeline& tl, const Params& p) { return rangeEdit(tl, p, true); }));
    registerSpeedCommands(session);
    registerMarkerCommands(session);
    registerNestCommands(session);
}

}  // namespace wetwaresuite::cut
