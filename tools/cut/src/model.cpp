// The timeline document: tracks, clips, keyframes, markers, .wwcut JSON and snapshot undo.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cctype>
#include <iterator>
#include <map>
#include <filesystem>
#include <stdexcept>

#include <nlohmann/json.hpp>

#include "wetwaresuite/cut/cut.hpp"

namespace wetwaresuite::cut {

using json = nlohmann::json;
namespace fs = std::filesystem;

namespace {

constexpr double kEps = 1e-6;

bool validTrackId(const std::string& id) {
    if (id.size() < 2 || id.size() > 4 || (id[0] != 'v' && id[0] != 'a')) return false;
    for (std::size_t i = 1; i < id.size(); ++i) {
        if (id[i] < '0' || id[i] > '9') return false;
    }
    return id[1] != '0';
}

int trackNumber(const std::string& id) { return std::atoi(id.c_str() + 1); }

std::string num(double v) {
    char buf[32];
    std::snprintf(buf, sizeof buf, "%.2f", v);
    return buf;
}

std::string t4(double v) {
    char buf[40];
    std::snprintf(buf, sizeof buf, "%.4f", v);
    return buf;
}

struct PropertyEntry {
    const char* name;
    double Clip::*field;
};

const std::vector<PropertyEntry>& propertyTable() {
    static const std::vector<PropertyEntry> t = {
        {"x", &Clip::x},
        {"y", &Clip::y},
        {"width", &Clip::width},
        {"height", &Clip::height},
        {"scale", &Clip::scale},
        {"rotation", &Clip::rotation},
        {"opacity", &Clip::opacity},
        {"volume", &Clip::volume},
        {"pan", &Clip::pan},
        {"crop_left", &Clip::cropLeft},
        {"crop_right", &Clip::cropRight},
        {"crop_top", &Clip::cropTop},
        {"crop_bottom", &Clip::cropBottom},
        {"blur", &Clip::blur},
        {"sharpen", &Clip::sharpen},
        {"vignette", &Clip::vignette},
        {"brightness", &Clip::brightness},
        {"contrast", &Clip::contrast},
        {"saturation", &Clip::saturation},
        {"gamma", &Clip::gamma},
        {"exposure", &Clip::exposure},
        {"temperature", &Clip::temperature},
        {"tint", &Clip::tint},
    };
    return t;
}

double ease(const std::string& e, double u) {
    if (e == "ease_in") return u * u;
    if (e == "ease_out") return 1 - (1 - u) * (1 - u);
    if (e == "ease_in_out" || e == "ease") return u * u * (3 - 2 * u);
    if (e == "hold") return 0;
    return u;
}

}  // namespace

const std::vector<std::string>& animatableProperties() {
    static const std::vector<std::string> names = [] {
        std::vector<std::string> v;
        for (const auto& e : propertyTable()) v.push_back(e.name);
        return v;
    }();
    return names;
}

double* propertyField(Clip& c, const std::string& property) {
    for (const auto& e : propertyTable()) {
        if (property == e.name) return &(c.*(e.field));
    }
    return nullptr;
}

const double* propertyField(const Clip& c, const std::string& property) {
    for (const auto& e : propertyTable()) {
        if (property == e.name) return &(c.*(e.field));
    }
    return nullptr;
}

const std::vector<std::string>& easeNames() {
    static const std::vector<std::string> names = {"linear", "ease_in", "ease_out", "ease_in_out", "hold"};
    return names;
}

double evaluateKeys(const std::vector<Key>& keys, double t) {
    if (keys.empty()) return 0;
    if (t <= keys.front().t) return keys.front().v;
    if (t >= keys.back().t) return keys.back().v;
    for (std::size_t i = 0; i + 1 < keys.size(); ++i) {
        const Key& a = keys[i];
        const Key& b = keys[i + 1];
        if (t < b.t) {
            const double span = b.t - a.t;
            const double u = span > kEps ? (t - a.t) / span : 1;
            return a.v + (b.v - a.v) * ease(a.ease, u);
        }
    }
    return keys.back().v;
}

bool Clip::sound() const {
    if (kind == "audio") return true;
    if (kind == "video") return hasAudio && ownSound;
    return kind == "nest";
}

double Clip::value(const std::string& property, double local) const {
    auto it = keys.find(property);
    if (it != keys.end() && !it->second.empty()) return evaluateKeys(it->second, local);
    const double* f = propertyField(*this, property);
    return f ? *f : 0;
}

double Clip::sourceTime(double local) const {
    if (hold) return in;
    const double d = std::max(0.0, local) * speed;
    if (reverse) return std::max(in, out - d - 1e-4);
    return std::min(in + d, std::max(in, out - 1e-4));
}

std::string Timeline::summary() const {
    int clips = 0;
    for (const auto& t : tracks) clips += static_cast<int>(t.clips.size());
    char fps[32];
    std::snprintf(fps, sizeof fps, "%g", settings.fps);
    return std::to_string(settings.width) + "x" + std::to_string(settings.height) + " " + fps + "fps, " +
           std::to_string(tracks.size()) + (tracks.size() == 1 ? " track, " : " tracks, ") + std::to_string(clips) +
           (clips == 1 ? " clip, " : " clips, ") + num(duration()) + "s" + (path.empty() ? "" : ", " + path);
}

bool Timeline::timeline(TimelineLayout& out) const {
    out.docParam = "doc";
    out.tracks.clear();
    out.tools = true;
    out.cursor = cursor;
    out.inPoint = inPoint;
    out.outPoint = outPoint;
    out.markers.clear();
    for (const auto& m : markers) out.markers.push_back({m.id, m.t, m.name, m.color});
    // the media bin: what was brought in, then any other file a clip plays
    out.media = bin;
    for (const auto& t : tracks)
        for (const auto& c : t.clips)
            if (!c.source.empty() && c.kind != "nest" && std::find(out.media.begin(), out.media.end(), c.source) == out.media.end()) out.media.push_back(c.source);
    // video tracks top-down as editors show them (v2 above v1), then audio
    std::vector<const Track*> order;
    for (auto it = tracks.rbegin(); it != tracks.rend(); ++it) if (it->kind == "video") order.push_back(&*it);
    for (const auto& t : tracks) if (t.kind != "video") order.push_back(&t);
    for (const Track* t : order) {
        TimelineTrack tt;
        tt.id = t->id;
        tt.name = t->id;
        tt.kind = t->kind;
        tt.toggles.push_back({"locked", "L", "Lock the track so edits leave it alone", t->locked});
        if (t->kind == "video") tt.toggles.push_back({"hidden", "H", "Hide the track's picture", t->hidden});
        tt.toggles.push_back({"muted", "M", "Mute the track's sound", t->muted});
        tt.toggles.push_back({"solo", "S", "Solo: only soloed tracks of this kind play", t->solo});
        if (t->kind == "audio") tt.toggles.push_back({"duck", "D", "Duck: this track (music) gets quieter while the other audio tracks (voices) talk", !t->duckUnder.empty()});
        for (const auto& c : t->clips) {
            TimelineClip tc;
            tc.id = c.id;
            if (c.kind == "title") tc.label = (c.caption ? "cc \"" : "\"") + c.text + "\"";
            else if (c.kind == "color") tc.label = "matte " + c.color;
            else if (c.kind == "nest") tc.label = "nest " + c.source;
            else tc.label = std::filesystem::path(c.source).filename().string();
            if (std::abs(c.speed - 1) > kEps) tc.label += " " + num(c.speed * 100).substr(0, num(c.speed * 100).find('.')) + "%";
            if (c.reverse) tc.label += " rev";
            if (c.hold) tc.label += " hold";
            tc.start = c.start;
            tc.length = c.length();
            tc.link = c.link;
            tc.picture = c.kind == "video" || c.kind == "image";
            if (c.kind != "nest") tc.source = c.source;
            tc.sound = c.kind == "audio" || (c.kind == "video" && c.ownSound && c.hasAudio);
            tc.sourceIn = c.hold ? c.in : c.reverse ? c.out : c.in;
            tc.speed = c.hold || c.kind == "image" ? 0 : c.reverse ? -c.speed : c.speed;
            const bool fromSource = (c.kind == "video" || c.kind == "audio") && !c.hold && !c.reverse;
            tc.trimStart = c.kind != "nest" || c.in > 0;
            if (fromSource && c.speed > 0) {
                tc.headroom = c.in / c.speed;
                tc.tailroom = c.sourceDuration > 0 ? std::max(0.0, c.sourceDuration - c.out) / c.speed : -1;
            }
            if (!c.transition.empty()) {
                const Clip* prev = c.media() ? previousMedia(*t, c) : previousClip(*t, c);
                if (prev && prev->end() > c.start + 1e-3) tc.transitionIn = std::min(prev->end() - c.start, c.length());
            }
            for (const auto& [name, ks] : c.keys) {
                for (const auto& k : ks) {
                    if (std::find_if(tc.keyframes.begin(), tc.keyframes.end(), [&](double v) { return std::abs(v - (c.start + k.t)) < 1e-4; }) ==
                        tc.keyframes.end()) {
                        tc.keyframes.push_back(c.start + k.t);
                    }
                }
            }
            std::sort(tc.keyframes.begin(), tc.keyframes.end());
            // the first animated property with more than one value, as a curve the window draws
            for (const auto& [name, ks] : c.keys) {
                if (ks.size() < 2) continue;
                double lo = ks[0].v, hi = ks[0].v;
                for (const auto& k : ks) lo = std::min(lo, k.v), hi = std::max(hi, k.v);
                if (hi - lo < 1e-9) continue;
                tc.curveName = name;
                const int n = 64;
                for (int i = 0; i < n; ++i) {
                    const double v = evaluateKeys(ks, c.length() * i / (n - 1));
                    tc.curve.push_back(static_cast<float>((v - lo) / (hi - lo)));
                }
                break;
            }
            tt.clips.push_back(tc);
        }
        out.tracks.push_back(std::move(tt));
    }
    return true;
}

std::string Timeline::timelineCommand(const wetwaresuite::TimelineEdit& e) const {
    switch (e.kind) {
        case TimelineEditKind::Cursor: return "cut.set_cursor t=" + t4(std::max(0.0, e.at));
        case TimelineEditKind::Razor: return "cut.razor at=" + t4(e.at) + (e.track.empty() ? "" : " tracks=" + e.track);
        case TimelineEditKind::Marker: {
            // over the selected clip: a clip marker that moves with it
            const Clip* c = e.clip.empty() ? nullptr : findClip(e.clip);
            const bool on = c && e.at >= c->start - 1e-6 && e.at <= c->end() + 1e-6;
            return "cut.add_marker t=" + t4(std::max(0.0, e.at)) + (on ? " clip=" + c->id : std::string());
        }
        case TimelineEditKind::InPoint: return "cut.set_in_out in=" + t4(std::max(0.0, e.at));
        case TimelineEditKind::OutPoint: return "cut.set_in_out out=" + t4(std::max(0.0, e.at));
        case TimelineEditKind::AddFile: {
            // sound files go on an audio track, everything else (video, stills) on a video track
            std::string ext = fs::path(e.name).extension().string();
            for (auto& ch : ext) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
            static const char* sounds[] = {".wav", ".mp3", ".flac", ".ogg", ".oga", ".opus", ".m4a", ".aac", ".wma", ".aif", ".aiff"};
            const bool sound = std::find(std::begin(sounds), std::end(sounds), ext) != std::end(sounds);
            std::string path;
            for (char ch : e.name) {
                if (ch == '"' || ch == '$' || ch == '#' || std::isspace(static_cast<unsigned char>(ch))) path += '\\';
                path += ch;
            }
            std::string line = std::string(sound ? "cut.add_audio" : "cut.add_clip") + " path=" + path;
            if (const Track* t = track(e.track); t && (t->kind == "audio") == sound) line += " track=" + t->id;
            if (e.place >= 0) line += " start=" + t4(e.place);
            return line;
        }
        case TimelineEditKind::MoveKey: {
            const Clip* c = findClip(e.clip);
            if (!c || c->keys.empty()) return {};
            return "cut.move_keyframe clip=" + c->id + " from=" + t4(e.at - c->start) + " to=" + t4(std::clamp(e.start - c->start, 0.0, c->length()));
        }
        case TimelineEditKind::SetProperty: {
            const Clip* c = findClip(e.clip);
            if (!c || e.name.empty() || e.name.find_first_not_of("abcdefghijklmnopqrstuvwxyz_") != std::string::npos) return {};
            const bool flag = e.name == "mute" || e.name == "grayscale" || e.name == "flip_h" || e.name == "flip_v";
            return "cut.set_clip clip=" + c->id + " " + e.name + "=" + (flag ? (e.at != 0 ? "true" : "false") : t4(e.at));
        }
        case TimelineEditKind::Toggle: {
            const Track* t = track(e.track);
            if (t && e.name == "duck") return "cut.set_track track=" + t->id + " duck_under=" + (t->duckUnder.empty() ? "auto" : "off");
            if (!t || (e.name != "locked" && e.name != "hidden" && e.name != "muted" && e.name != "solo")) return {};
            const bool now = e.name == "locked" ? t->locked : e.name == "hidden" ? t->hidden : e.name == "muted" ? t->muted : t->solo;
            return "cut.set_track track=" + t->id + " " + e.name + "=" + (now ? "false" : "true");
        }
        default: break;
    }
    const Clip* c = findClip(e.clip);
    if (!c) return {};
    switch (e.kind) {
        case TimelineEditKind::Move: {
            std::string line = "cut.move_clip clip=" + c->id + " start=" + t4(std::max(0.0, e.start));
            if (!e.toTrack.empty() && e.toTrack != e.track) line += " track=" + e.toTrack;
            return line;
        }
        case TimelineEditKind::TrimEnd: return "cut.trim_clip clip=" + c->id + " duration=" + t4(std::max(0.04, e.length));
        case TimelineEditKind::TrimStart: {
            const double start = std::clamp(e.start, 0.0, c->end() - 0.04);
            if ((c->kind == "video" || c->kind == "audio" || c->kind == "nest") && !c->hold && !c->reverse) {
                const double in = std::max(0.0, c->in + (start - c->start) * c->speed);
                return "cut.set_clip clip=" + c->id + " start=" + t4(c->start + (in - c->in) / c->speed) + " in=" + t4(in);
            }
            return "cut.set_clip clip=" + c->id + " start=" + t4(start) + " duration=" + t4(c->end() - start);
        }
        case TimelineEditKind::Split: return "cut.split_clip clip=" + c->id + " at=" + t4(e.at);
        case TimelineEditKind::Delete: return "cut.delete_clip clip=" + c->id;
        case TimelineEditKind::RippleDelete: return "cut.delete_clip clip=" + c->id + " ripple=true";
        case TimelineEditKind::Transition: {
            const std::string type = e.name.empty() ? "crossfade" : e.name;
            return "cut.transition clip=" + c->id + " type=" + type;
        }
        default: break;
    }
    return {};
}

double Timeline::duration() const {
    double d = 0;
    for (const auto& t : tracks) {
        for (const auto& c : t.clips) d = std::max(d, c.end());
    }
    return d;
}

Track* Timeline::track(const std::string& id) {
    for (auto& t : tracks) {
        if (t.id == id) return &t;
    }
    return nullptr;
}

const Track* Timeline::track(const std::string& id) const {
    for (const auto& t : tracks) {
        if (t.id == id) return &t;
    }
    return nullptr;
}

Track& Timeline::ensureTrack(const std::string& id) {
    if (Track* t = track(id)) return *t;
    if (!validTrackId(id)) throw std::runtime_error("track names are v1, v2, ... for video and a1, a2, ... for audio (got '" + id + "')");
    Track t;
    t.id = id;
    t.kind = id[0] == 'v' ? "video" : "audio";
    tracks.push_back(t);
    // video tracks first (bottom-up by number), then audio tracks
    std::stable_sort(tracks.begin(), tracks.end(), [](const Track& a, const Track& b) {
        if (a.kind != b.kind) return a.kind == "video";
        return trackNumber(a.id) < trackNumber(b.id);
    });
    return *track(id);
}

Clip* Timeline::findClip(const std::string& id, Track** owner) {
    for (auto& t : tracks) {
        for (auto& c : t.clips) {
            if (c.id == id) {
                if (owner) *owner = &t;
                return &c;
            }
        }
    }
    return nullptr;
}

const Clip* Timeline::findClip(const std::string& id) const {
    for (const auto& t : tracks) {
        for (const auto& c : t.clips) {
            if (c.id == id) return &c;
        }
    }
    return nullptr;
}

void Timeline::sortClips() {
    for (auto& t : tracks) {
        std::stable_sort(t.clips.begin(), t.clips.end(), [](const Clip& a, const Clip& b) { return a.start < b.start; });
    }
}

double Timeline::trackEnd(const std::string& id) const {
    double e = 0;
    if (const Track* t = track(id)) {
        for (const auto& c : t->clips) e = std::max(e, c.end());
    }
    return e;
}

const Clip* Timeline::previousMedia(const Track& t, const Clip& clip) const {
    const Clip* best = nullptr;
    for (const auto& c : t.clips) {
        if (c.id == clip.id || !c.media() || c.start > clip.start + 1e-9) continue;
        if (c.start == clip.start && &c > &clip) continue;
        if (!best || c.start >= best->start) best = &c;
    }
    return best;
}

const Clip* Timeline::previousClip(const Track& t, const Clip& clip) const {
    const Clip* best = nullptr;
    for (const auto& c : t.clips) {
        if (c.id == clip.id || c.kind != clip.kind || c.start > clip.start + 1e-9) continue;
        if (c.start == clip.start && &c > &clip) continue;
        if (!best || c.start >= best->start) best = &c;
    }
    return best;
}

std::vector<std::string> Timeline::linkedTo(const Clip& clip) const {
    std::vector<std::string> out;
    if (clip.link.empty()) return out;
    for (const auto& t : tracks) {
        for (const auto& c : t.clips) {
            if (c.id != clip.id && c.link == clip.link) out.push_back(c.id);
        }
    }
    return out;
}

bool Timeline::anySolo(const std::string& kind) const {
    for (const auto& t : tracks) {
        if (t.kind == kind && t.solo) return true;
    }
    return false;
}

bool Timeline::shows(const Track& t) const {
    if (t.kind != "video" || t.hidden) return false;
    return !anySolo("video") || t.solo;
}

bool Timeline::sounds(const Track& t) const {
    if (t.muted) return false;
    return !anySolo(t.kind) || t.solo;
}

Result TimelineEdit::apply() {
    before_ = {doc_->settings, doc_->tracks, doc_->markers, doc_->inPoint, doc_->outPoint, doc_->masterVolume,
               doc_->nextClip, doc_->nextLink, doc_->nextMarker, doc_->bin};
    // clip markers: where each sat on its clip before the edit
    struct Seat {
        double t = 0, clipStart = 0;
        bool had = false;
    };
    std::map<std::string, Seat> seats;
    for (const auto& m : doc_->markers) {
        if (m.clip.empty()) continue;
        const Clip* c = static_cast<const Timeline&>(*doc_).findClip(m.clip);
        seats[m.id] = {m.t, c ? c->start : m.t, c != nullptr};
    }
    Result r;
    try {
        r = fn_(*doc_);
    } catch (const std::exception& e) {
        r = Result::failure(e.what());
    }
    if (r.ok) {
        doc_->sortClips();
        // clip markers follow their clip; one moved on purpose stays where it was put
        for (auto& m : doc_->markers) {
            if (m.clip.empty()) continue;
            const Clip* c = static_cast<const Timeline&>(*doc_).findClip(m.clip);
            auto seat = seats.find(m.id);
            if (!c) {
                m.clip.clear();
                continue;
            }
            if (seat == seats.end() || !seat->second.had || std::abs(m.t - seat->second.t) > 1e-6) {
                if (m.t < c->start - 1e-6 || m.t > c->end() + 1e-6) m.clip.clear();
                continue;
            }
            const double offset = seat->second.t - seat->second.clipStart;
            if (offset > c->length() + 1e-6) m.clip.clear();  // cut off the end of a trimmed clip: it stays put
            else m.t = c->start + offset;
        }
        std::sort(doc_->markers.begin(), doc_->markers.end(), [](const Marker& a, const Marker& b) { return a.t < b.t; });
        // a locked track keeps its clips exactly as they were
        for (const auto& old : before_.tracks) {
            if (!old.locked) continue;
            const Track* now = doc_->track(old.id);
            if (!now || now->clips != old.clips || now->volume != old.volume) {
                r = Result::failure("track " + old.id + " is locked; unlock it first (cut.set_track track=" + old.id + " locked=false)");
                break;
            }
        }
    }
    if (!r.ok) {
        revert();
        return r;
    }
    return r;
}

void TimelineEdit::revert() {
    doc_->settings = before_.settings;
    doc_->tracks = before_.tracks;
    doc_->markers = before_.markers;
    doc_->inPoint = before_.inPoint;
    doc_->outPoint = before_.outPoint;
    doc_->masterVolume = before_.masterVolume;
    doc_->nextClip = before_.nextClip;
    doc_->nextLink = before_.nextLink;
    doc_->nextMarker = before_.nextMarker;
    doc_->bin = before_.bin;
}

namespace {

json timelineJson(const Timeline& tl);
std::shared_ptr<Timeline> timelineFrom(const json& j, const fs::path& baseDir);

bool defaultWheel(const std::vector<double>& w) {
    return std::all_of(w.begin(), w.end(), [](double v) { return std::abs(v) < 1e-12; });
}

json clipJson(const Clip& c) {
    json j = {{"id", c.id}, {"kind", c.kind}, {"start", c.start}, {"in", c.in}, {"out", c.out}};
    const Clip d;
    auto put = [&](const char* key, auto value, auto fallback) {
        if (value != fallback) j[key] = value;
    };
    put("source", c.source, d.source);
    put("speed", c.speed, d.speed);
    put("reverse", c.reverse, d.reverse);
    put("hold", c.hold, d.hold);
    put("link", c.link, d.link);
    put("own_sound", c.ownSound, d.ownSound);
    put("volume", c.volume, d.volume);
    put("mute", c.mute, d.mute);
    put("pan", c.pan, d.pan);
    put("denoise", c.denoise, d.denoise);
    put("fade_in", c.fadeIn, d.fadeIn);
    put("fade_out", c.fadeOut, d.fadeOut);
    put("opacity", c.opacity, d.opacity);
    put("blend", c.blend, d.blend);
    put("x", c.x, d.x);
    put("y", c.y, d.y);
    put("width", c.width, d.width);
    put("height", c.height, d.height);
    put("fit", c.fit, d.fit);
    if (c.cropW > 0) j["crop"] = {c.cropX, c.cropY, c.cropW, c.cropH};
    put("scale", c.scale, d.scale);
    put("rotation", c.rotation, d.rotation);
    put("crop_left", c.cropLeft, d.cropLeft);
    put("crop_right", c.cropRight, d.cropRight);
    put("crop_top", c.cropTop, d.cropTop);
    put("crop_bottom", c.cropBottom, d.cropBottom);
    put("flip_h", c.flipH, d.flipH);
    put("flip_v", c.flipV, d.flipV);
    put("exposure", c.exposure, d.exposure);
    put("temperature", c.temperature, d.temperature);
    put("tint", c.tint, d.tint);
    if (!defaultWheel(c.wheelLift)) j["wheel_lift"] = c.wheelLift;
    if (!defaultWheel(c.wheelGamma)) j["wheel_gamma"] = c.wheelGamma;
    if (!defaultWheel(c.wheelGain)) j["wheel_gain"] = c.wheelGain;
    put("brightness", c.brightness, d.brightness);
    put("contrast", c.contrast, d.contrast);
    put("saturation", c.saturation, d.saturation);
    put("gamma", c.gamma, d.gamma);
    put("grayscale", c.grayscale, d.grayscale);
    if (!c.curve.empty()) j["curve"] = c.curve;
    if (!c.curveR.empty()) j["curve_r"] = c.curveR;
    if (!c.curveG.empty()) j["curve_g"] = c.curveG;
    if (!c.curveB.empty()) j["curve_b"] = c.curveB;
    put("lut", c.lut, d.lut);
    put("blur", c.blur, d.blur);
    put("sharpen", c.sharpen, d.sharpen);
    put("vignette", c.vignette, d.vignette);
    if (!c.keyColor.empty()) {
        j["key_color"] = c.keyColor;
        j["key_similarity"] = c.keySimilarity;
        j["key_softness"] = c.keySoftness;
        j["key_spill"] = c.keySpill;
    }
    if (!c.transition.empty()) {
        j["transition"] = c.transition;
        j["transition_duration"] = c.transitionDuration;
    }
    if (c.kind == "title" || c.kind == "color") put("color", c.color, std::string());
    if (c.kind == "title") {
        j["text"] = c.text;
        j["font_size"] = c.fontSize;
        j["position"] = c.position;
        put("font", c.font, d.font);
        put("box", c.box, d.box);
        put("box_color", c.boxColor, d.boxColor);
        put("bold", c.bold, d.bold);
        put("italic", c.italic, d.italic);
        put("align", c.align, d.align);
        put("outline", c.outline, d.outline);
        put("outline_color", c.outlineColor, d.outlineColor);
        put("shadow", c.shadow, d.shadow);
        put("shadow_color", c.shadowColor, d.shadowColor);
        put("anim_in", c.animIn, d.animIn);
        put("anim_out", c.animOut, d.animOut);
        put("anim_duration", c.animDuration, d.animDuration);
        put("style", c.style, d.style);
        put("subtitle", c.subtitle, d.subtitle);
        put("accent_color", c.accentColor, d.accentColor);
        put("caption", c.caption, d.caption);
    }
    if (!c.keys.empty()) {
        json k = json::object();
        for (const auto& [name, ks] : c.keys) {
            json arr = json::array();
            for (const auto& key : ks) {
                json kj = {{"t", key.t}, {"v", key.v}};
                if (key.ease != "linear") kj["ease"] = key.ease;
                arr.push_back(kj);
            }
            k[name] = arr;
        }
        j["keys"] = k;
    }
    if (c.nested) j["nested"] = timelineJson(*c.nested);
    if (c.kind == "video" || c.kind == "image" || c.kind == "audio") {
        json src = {{"duration", c.sourceDuration}, {"width", c.sourceWidth}, {"height", c.sourceHeight},
                    {"fps", c.sourceFps}, {"has_audio", c.hasAudio}};
        if (!c.videoCodec.empty()) src["video_codec"] = c.videoCodec;
        if (!c.audioCodec.empty()) src["audio_codec"] = c.audioCodec;
        j["probe"] = src;
    }
    return j;
}

std::vector<double> numbers(const json& j, const char* key, std::vector<double> fallback) {
    if (!j.contains(key) || !j[key].is_array()) return fallback;
    std::vector<double> v;
    for (const auto& x : j[key]) v.push_back(x.get<double>());
    return v;
}

std::vector<double> wheel(const json& j, const char* key) {
    std::vector<double> v = numbers(j, key, {0, 0, 0, 0});
    v.resize(4, 0.0);
    return v;
}

Clip clipFrom(const json& j, const fs::path& baseDir) {
    Clip c;
    const Clip d;
    c.id = j.at("id").get<std::string>();
    c.kind = j.at("kind").get<std::string>();
    if (c.kind != "video" && c.kind != "image" && c.kind != "title" && c.kind != "audio" && c.kind != "color" && c.kind != "nest") {
        throw std::runtime_error("unknown clip kind '" + c.kind + "'");
    }
    c.source = j.value("source", d.source);
    if (!c.source.empty() && c.kind != "nest") {
        fs::path p = fs::path(c.source);
        if (p.is_relative() && !baseDir.empty()) c.source = (baseDir / p).lexically_normal().string();
    }
    c.start = j.value("start", 0.0);
    c.in = j.value("in", 0.0);
    c.out = j.value("out", 0.0);
    c.speed = j.value("speed", d.speed);
    c.reverse = j.value("reverse", d.reverse);
    c.hold = j.value("hold", d.hold);
    c.link = j.value("link", d.link);
    c.ownSound = j.value("own_sound", d.ownSound);
    c.volume = j.value("volume", d.volume);
    c.mute = j.value("mute", d.mute);
    c.pan = j.value("pan", d.pan);
    c.denoise = j.value("denoise", d.denoise);
    c.fadeIn = j.value("fade_in", d.fadeIn);
    c.fadeOut = j.value("fade_out", d.fadeOut);
    c.opacity = j.value("opacity", d.opacity);
    c.blend = j.value("blend", d.blend);
    c.x = j.value("x", d.x);
    c.y = j.value("y", d.y);
    c.width = j.value("width", d.width);
    c.height = j.value("height", d.height);
    c.fit = j.value("fit", d.fit);
    if (j.contains("crop") && j["crop"].is_array() && j["crop"].size() == 4) {
        c.cropX = j["crop"][0].get<double>();
        c.cropY = j["crop"][1].get<double>();
        c.cropW = j["crop"][2].get<double>();
        c.cropH = j["crop"][3].get<double>();
    }
    c.scale = j.value("scale", d.scale);
    c.rotation = j.value("rotation", d.rotation);
    c.cropLeft = j.value("crop_left", d.cropLeft);
    c.cropRight = j.value("crop_right", d.cropRight);
    c.cropTop = j.value("crop_top", d.cropTop);
    c.cropBottom = j.value("crop_bottom", d.cropBottom);
    c.flipH = j.value("flip_h", d.flipH);
    c.flipV = j.value("flip_v", d.flipV);
    c.exposure = j.value("exposure", d.exposure);
    c.temperature = j.value("temperature", d.temperature);
    c.tint = j.value("tint", d.tint);
    c.wheelLift = wheel(j, "wheel_lift");
    c.wheelGamma = wheel(j, "wheel_gamma");
    c.wheelGain = wheel(j, "wheel_gain");
    c.brightness = j.value("brightness", d.brightness);
    c.contrast = j.value("contrast", d.contrast);
    c.saturation = j.value("saturation", d.saturation);
    c.gamma = j.value("gamma", d.gamma);
    c.grayscale = j.value("grayscale", d.grayscale);
    c.curve = numbers(j, "curve", {});
    c.curveR = numbers(j, "curve_r", {});
    c.curveG = numbers(j, "curve_g", {});
    c.curveB = numbers(j, "curve_b", {});
    c.lut = j.value("lut", d.lut);
    c.blur = j.value("blur", d.blur);
    c.sharpen = j.value("sharpen", d.sharpen);
    c.vignette = j.value("vignette", d.vignette);
    c.keyColor = j.value("key_color", d.keyColor);
    c.keySimilarity = j.value("key_similarity", d.keySimilarity);
    c.keySoftness = j.value("key_softness", d.keySoftness);
    c.keySpill = j.value("key_spill", d.keySpill);
    c.transition = j.value("transition", d.transition);
    c.transitionDuration = j.value("transition_duration", d.transitionDuration);
    c.text = j.value("text", d.text);
    c.font = j.value("font", d.font);
    c.fontSize = j.value("font_size", d.fontSize);
    c.color = j.value("color", d.color);
    c.position = j.value("position", d.position);
    c.box = j.value("box", d.box);
    c.boxColor = j.value("box_color", d.boxColor);
    c.bold = j.value("bold", d.bold);
    c.italic = j.value("italic", d.italic);
    c.align = j.value("align", d.align);
    c.outline = j.value("outline", d.outline);
    c.outlineColor = j.value("outline_color", d.outlineColor);
    c.shadow = j.value("shadow", d.shadow);
    c.shadowColor = j.value("shadow_color", d.shadowColor);
    c.animIn = j.value("anim_in", d.animIn);
    c.animOut = j.value("anim_out", d.animOut);
    c.animDuration = j.value("anim_duration", d.animDuration);
    c.style = j.value("style", d.style);
    c.subtitle = j.value("subtitle", d.subtitle);
    c.accentColor = j.value("accent_color", d.accentColor);
    c.caption = j.value("caption", d.caption);
    if (j.contains("keys") && j["keys"].is_object()) {
        for (const auto& [name, arr] : j["keys"].items()) {
            if (!propertyField(c, name)) throw std::runtime_error("clip " + c.id + " animates an unknown property '" + name + "'");
            std::vector<Key> ks;
            for (const auto& kj : arr) ks.push_back({kj.value("t", 0.0), kj.value("v", 0.0), kj.value("ease", std::string("linear"))});
            std::sort(ks.begin(), ks.end(), [](const Key& a, const Key& b) { return a.t < b.t; });
            if (!ks.empty()) c.keys[name] = ks;
        }
    }
    if (j.contains("nested") && j["nested"].is_object()) c.nested = timelineFrom(j["nested"], baseDir);
    if (c.kind == "nest" && !c.nested) throw std::runtime_error("nested clip " + c.id + " has no sequence inside");
    if (j.contains("probe")) {
        const json& s = j["probe"];
        c.sourceDuration = s.value("duration", 0.0);
        c.sourceWidth = s.value("width", 0);
        c.sourceHeight = s.value("height", 0);
        c.sourceFps = s.value("fps", 0.0);
        c.hasAudio = s.value("has_audio", false);
        c.videoCodec = s.value("video_codec", std::string());
        c.audioCodec = s.value("audio_codec", std::string());
    }
    if (!(c.speed > 0) || c.out < c.in) throw std::runtime_error("clip " + c.id + " has a bad speed or in/out range");
    return c;
}

json timelineJson(const Timeline& tl) {
    json tracks = json::array();
    for (const auto& t : tl.tracks) {
        json clips = json::array();
        for (const auto& c : t.clips) clips.push_back(clipJson(c));
        json tj = {{"id", t.id}, {"kind", t.kind}, {"clips", clips}};
        if (t.muted) tj["muted"] = true;
        if (t.hidden) tj["hidden"] = true;
        if (t.locked) tj["locked"] = true;
        if (t.solo) tj["solo"] = true;
        if (t.volume != 1) tj["volume"] = t.volume;
        if (!t.duckUnder.empty()) tj["duck_under"] = t.duckUnder, tj["duck_db"] = t.duckDb;
        tracks.push_back(tj);
    }
    json j = {{"format", "wwcut"},
              {"version", 2},
              {"settings",
               {{"width", tl.settings.width},
                {"height", tl.settings.height},
                {"fps", tl.settings.fps},
                {"background", tl.settings.background}}},
              {"next_clip", tl.nextClip},
              {"tracks", tracks}};
    if (tl.nextLink > 1) j["next_link"] = tl.nextLink;
    if (!tl.markers.empty()) {
        json m = json::array();
        for (const auto& mk : tl.markers) {
            json mj = {{"id", mk.id}, {"t", mk.t}, {"name", mk.name}, {"color", mk.color}};
            if (!mk.clip.empty()) mj["clip"] = mk.clip;
            if (mk.duration > 0) mj["duration"] = mk.duration;
            m.push_back(mj);
        }
        j["markers"] = m;
        j["next_marker"] = tl.nextMarker;
    }
    if (tl.inPoint >= 0) j["in_point"] = tl.inPoint;
    if (tl.outPoint >= 0) j["out_point"] = tl.outPoint;
    if (tl.masterVolume != 1) j["master_volume"] = tl.masterVolume;
    if (!tl.bin.empty()) j["bin"] = tl.bin;
    return j;
}

std::shared_ptr<Timeline> timelineFrom(const json& j, const fs::path& baseDir) {
    const std::string format = j.is_object() ? j.value("format", std::string()) : std::string();
    if (format != "wwcut" && format != "achcut") {  // "achcut": timelines saved before the rename
        throw std::runtime_error("not a .wwcut timeline (missing \"format\": \"wwcut\")");
    }
    auto tl = std::make_shared<Timeline>();
    try {
        if (j.contains("settings")) {
            const json& s = j["settings"];
            tl->settings.width = s.value("width", 1920);
            tl->settings.height = s.value("height", 1080);
            tl->settings.fps = s.value("fps", 30.0);
            tl->settings.background = s.value("background", std::string("black"));
        }
        int highest = 0, highestLink = 0;
        for (const auto& tj : j.value("tracks", json::array())) {
            Track& t = tl->ensureTrack(tj.at("id").get<std::string>());
            t.muted = tj.value("muted", false);
            t.hidden = tj.value("hidden", false);
            t.locked = tj.value("locked", false);
            t.solo = tj.value("solo", false);
            t.volume = tj.value("volume", 1.0);
            t.duckUnder = tj.value("duck_under", std::string());
            t.duckDb = tj.value("duck_db", -12.0);
            for (const auto& cj : tj.value("clips", json::array())) {
                Clip c = clipFrom(cj, baseDir);
                if (tl->findClip(c.id)) throw std::runtime_error("two clips are called " + c.id);
                if ((t.kind == "audio") != (c.kind == "audio")) {
                    throw std::runtime_error("clip " + c.id + " is on the wrong kind of track");
                }
                if (c.id.size() > 1 && c.id[0] == 'c') highest = std::max(highest, std::atoi(c.id.c_str() + 1));
                if (c.link.size() > 1 && c.link[0] == 'L') highestLink = std::max(highestLink, std::atoi(c.link.c_str() + 1));
                t.clips.push_back(c);
            }
        }
        tl->nextClip = std::max(j.value("next_clip", 1), highest + 1);
        tl->nextLink = std::max(j.value("next_link", 1), highestLink + 1);
        int highestMarker = 0;
        for (const auto& mj : j.value("markers", json::array())) {
            Marker m;
            m.id = mj.value("id", std::string());
            m.t = mj.value("t", 0.0);
            m.duration = mj.value("duration", 0.0);
            m.name = mj.value("name", std::string());
            m.color = mj.value("color", std::string("green"));
            m.clip = mj.value("clip", std::string());
            if (m.id.size() > 1 && m.id[0] == 'm') highestMarker = std::max(highestMarker, std::atoi(m.id.c_str() + 1));
            tl->markers.push_back(m);
        }
        tl->nextMarker = std::max(j.value("next_marker", 1), highestMarker + 1);
        for (const auto& bj : j.value("bin", json::array())) {
            if (!bj.is_string()) continue;
            fs::path p = bj.get<std::string>();
            if (p.is_relative() && !baseDir.empty()) p = (baseDir / p).lexically_normal();
            tl->bin.push_back(p.string());
        }
        tl->inPoint = j.value("in_point", -1.0);
        tl->outPoint = j.value("out_point", -1.0);
        tl->masterVolume = j.value("master_volume", 1.0);
    } catch (const json::exception& e) {
        throw std::runtime_error(std::string("broken .wwcut file: ") + e.what());
    }
    if (tl->settings.width < 16 || tl->settings.height < 16 || !(tl->settings.fps > 0)) {
        throw std::runtime_error("broken .wwcut file: bad resolution or fps");
    }
    tl->sortClips();
    return tl;
}

}  // namespace

std::string toJson(const Timeline& tl) { return timelineJson(tl).dump(2) + "\n"; }

std::shared_ptr<Timeline> fromJson(const std::string& text, const fs::path& baseDir) {
    json j;
    try {
        j = json::parse(text);
    } catch (const std::exception& e) {
        throw std::runtime_error(std::string("not a valid .wwcut file: ") + e.what());
    }
    return timelineFrom(j, baseDir);
}

const std::vector<std::string>& transitionNames() {
    static const std::vector<std::string> names = {
        "none", "crossfade", "dissolve", "fadeblack", "fadewhite", "wipeleft", "wiperight", "wipeup", "wipedown",
        "slideleft", "slideright", "slideup", "slidedown", "pushleft", "pushright", "pushup", "pushdown", "zoomin",
        "zoomout", "circleopen", "circleclose", "radial", "pixelize", "smoothleft", "smoothright"};
    return names;
}

bool knownTransition(const std::string& type) {
    if (type == "fade" || type == "distance") return true;  // older names (ffmpeg xfade's)
    return std::find(transitionNames().begin(), transitionNames().end(), type) != transitionNames().end();
}

const std::vector<std::string>& blendModeNames() {
    static const std::vector<std::string> names = {"normal", "multiply", "screen", "overlay", "add", "subtract", "darken",
                                                   "lighten", "difference", "exclusion", "soft_light", "hard_light",
                                                   "color_dodge", "color_burn", "hue", "saturation", "color", "luminosity"};
    return names;
}

const std::vector<std::string>& titleAnimations() {
    static const std::vector<std::string> names = {"none", "fade", "slide_up", "slide_down", "slide_left", "slide_right",
                                                   "zoom", "typewriter", "wipe"};
    return names;
}

MediaInfo parseProbe(const std::string& text) {
    json j = json::parse(text);
    MediaInfo m;
    auto number = [](const json& v) -> double {
        if (v.is_number()) return v.get<double>();
        if (v.is_string()) {
            try {
                return std::stod(v.get<std::string>());
            } catch (const std::exception&) {
                return 0;
            }
        }
        return 0;
    };
    auto rate = [](const std::string& r) -> double {
        auto slash = r.find('/');
        try {
            if (slash == std::string::npos) return std::stod(r);
            double a = std::stod(r.substr(0, slash)), b = std::stod(r.substr(slash + 1));
            return b > 0 ? a / b : 0;
        } catch (const std::exception&) {
            return 0;
        }
    };
    if (j.contains("format")) {
        m.format = j["format"].value("format_name", std::string());
        if (j["format"].contains("duration")) m.duration = number(j["format"]["duration"]);
    }
    for (const auto& s : j.value("streams", json::array())) {
        StreamInfo si;
        si.type = s.value("codec_type", std::string());
        si.codec = s.value("codec_name", std::string());
        if (si.type == "video") {
            si.width = s.value("width", 0);
            si.height = s.value("height", 0);
            si.fps = rate(s.value("avg_frame_rate", std::string()));
            if (!(si.fps > 0)) si.fps = rate(s.value("r_frame_rate", std::string()));
            const bool cover = s.contains("disposition") && s["disposition"].value("attached_pic", 0) == 1;
            if (!m.hasVideo && !cover) {
                m.hasVideo = true;
                m.width = si.width;
                m.height = si.height;
                m.fps = si.fps;
                m.videoCodec = si.codec;
            }
        } else if (si.type == "audio") {
            si.sampleRate = static_cast<int>(number(s.value("sample_rate", json("0"))));
            si.channels = s.value("channels", 0);
            if (!m.hasAudio) {
                m.hasAudio = true;
                m.audioCodec = si.codec;
            }
        }
        if (!(m.duration > 0) && s.contains("duration")) m.duration = number(s["duration"]);
        m.streams.push_back(si);
    }
    static const std::vector<std::string> stillCodecs = {"png", "mjpeg", "bmp", "webp", "tiff", "gif", "targa", "ppm", "jpegls", "pam"};
    const bool imageFormat = m.format.find("image2") != std::string::npos || m.format.find("_pipe") != std::string::npos ||
                             m.format == "png" || m.format == "webp";
    m.still = m.hasVideo && !m.hasAudio && imageFormat &&
              std::find(stillCodecs.begin(), stillCodecs.end(), m.videoCodec) != stillCodecs.end();
    return m;
}

std::vector<ClipProperty> Timeline::clipProperties(const std::string& clipId) const {
    const Clip* c = findClip(clipId);
    if (!c) return {};
    std::vector<ClipProperty> out;
    auto add = [&](std::string name, std::string label, std::string group, double value, double lo, double hi, std::string format = "%.2f") {
        ClipProperty p;
        p.name = std::move(name);
        p.label = std::move(label);
        p.group = std::move(group);
        p.value = value;
        p.min = lo;
        p.max = hi;
        p.format = std::move(format);
        out.push_back(std::move(p));
    };
    auto toggle = [&](std::string name, std::string label, std::string group, bool on) {
        add(std::move(name), std::move(label), std::move(group), on ? 1 : 0, 0, 1);
        out.back().toggle = true;
    };
    const bool picture = c->kind != "audio";
    const bool sound = c->kind == "audio" || c->kind == "nest" || (c->kind == "video" && c->ownSound && c->hasAudio);
    const double len = std::max(0.1, c->length());
    if (c->kind == "video" || c->kind == "audio" || c->kind == "nest") add("speed", "Speed", "Timing", c->speed, 0.25, 4, "%.2fx");
    add("fade_in", "Fade in", "Timing", c->fadeIn, 0, std::min(5.0, len), "%.1f s");
    add("fade_out", "Fade out", "Timing", c->fadeOut, 0, std::min(5.0, len), "%.1f s");
    if (picture) {
        add("opacity", "Opacity", "Picture", c->opacity, 0, 1);
        add("scale", "Scale", "Picture", c->scale, 0.1, 4, "%.2fx");
        add("rotation", "Rotation", "Picture", c->rotation, -180, 180, "%.0f deg");
        if (c->kind != "title") {
            add("brightness", "Brightness", "Colour", c->brightness, -1, 1);
            add("contrast", "Contrast", "Colour", c->contrast, 0, 3);
            add("saturation", "Saturation", "Colour", c->saturation, 0, 3);
            toggle("grayscale", "Black and white", "Colour", c->grayscale);
        }
    }
    if (sound) {
        add("volume", "Volume", "Sound", c->volume, 0, 2);
        add("pan", "Pan", "Sound", c->pan, -1, 1);
        add("denoise", "Clean up noise (dB)", "Sound", c->denoise, 0, 30);
        toggle("mute", "Mute", "Sound", c->mute);
    }
    return out;
}

}  // namespace wetwaresuite::cut
