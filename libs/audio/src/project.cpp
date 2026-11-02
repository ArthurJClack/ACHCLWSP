// Multitrack mixing, project pictures and the project file format.
#include "wetwaresuite/audio/project.hpp"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <stdexcept>

#include <nlohmann/json.hpp>

#include "wetwaresuite/audio/dsp.hpp"
#include "wetwaresuite/audio/io.hpp"
#include "draw.hpp"

namespace wetwaresuite::audio {

namespace fs = std::filesystem;
using json = nlohmann::ordered_json;

double Track::end() const {
    double e = 0;
    for (const auto& c : clips) e = std::max(e, c.end());
    return e;
}

double Project::seconds() const {
    double e = 0;
    for (const auto& t : tracks) e = std::max(e, t.end());
    return e;
}

double Project::tailSeconds() const {
    double tail = 0;
    for (const auto& t : tracks) tail = std::max(tail, rackTail(t.fx));
    for (const auto& b : buses) tail = std::max(tail, rackTail(b.fx));
    return std::min(15.0, tail + rackTail(masterFx));
}

std::vector<Automation> trackLanes(const Track& t) {
    std::vector<Automation> out;
    if (!t.envelope.empty()) {
        Automation v{"volume", {}};
        for (const auto& e : t.envelope) v.points.push_back({e.time, e.gainDb});
        v.sort();
        out.push_back(std::move(v));
    }
    for (const auto& l : t.automation)
        if (!l.points.empty()) out.push_back(l);
    return out;
}

void setLane(std::vector<Automation>& lanes, const std::string& target, std::vector<AutomationPoint> points) {
    lanes.erase(std::remove_if(lanes.begin(), lanes.end(), [&](const Automation& l) { return l.target == target; }), lanes.end());
    if (points.empty()) return;
    Automation a{target, std::move(points)};
    a.sort();
    lanes.push_back(std::move(a));
}

void setTrackLane(Track& t, const std::string& target, std::vector<AutomationPoint> points) {
    if (target == "volume") {
        t.envelope.clear();
        for (const auto& pt : points) t.envelope.push_back({pt.time, pt.value});
        std::stable_sort(t.envelope.begin(), t.envelope.end(), [](const EnvelopePoint& a, const EnvelopePoint& b) { return a.time < b.time; });
        return;
    }
    setLane(t.automation, target, std::move(points));
}

void renumberLanes(std::vector<Automation>& lanes, int removed, int inserted, int moveFrom, int moveTo) {
    std::vector<Automation> kept;
    for (auto& l : lanes) {
        int i = 0;
        std::string prm;
        if (!parseFxTarget(l.target, i, prm)) {
            kept.push_back(std::move(l));
            continue;
        }
        if (removed >= 0) {
            if (i == removed) continue;
            if (i > removed) --i;
        }
        if (inserted >= 0 && i >= inserted) ++i;
        if (moveFrom >= 0 && moveTo >= 0) {
            if (i == moveFrom) i = moveTo;
            else if (moveFrom < moveTo && i > moveFrom && i <= moveTo) --i;
            else if (moveFrom > moveTo && i >= moveTo && i < moveFrom) ++i;
        }
        l.target = "fx" + std::to_string(i) + "." + prm;
        kept.push_back(std::move(l));
    }
    lanes = std::move(kept);
}

namespace {

std::size_t framesOf(const Project& p) {
    return static_cast<std::size_t>(std::ceil((p.seconds() + p.tailSeconds()) * p.rate));
}

// Clips (fades, clip gain), the track's effects, track gain and envelope; no ducking or pan.
Buffer renderDry(const Project& p, const Track& t, std::size_t frames) {
    Buffer out(p.channels, frames, p.rate);
    for (const auto& clip : t.clips) {
        if (!clip.audio || clip.audio->empty() || clip.seconds() <= 0) continue;
        const bool trimmed = clip.offset > 0 || clip.length >= 0;
        Buffer a = conform(trimmed ? slice(*clip.audio, rangeOf(*clip.audio, clip.offset, clip.offset + clip.seconds())) : *clip.audio,
                           p.rate, p.channels);
        if (clip.fadeIn > 0) fadeIn(a, clip.fadeIn, all(a), clip.fadeInCurve);
        if (clip.fadeOut > 0) fadeOut(a, clip.fadeOut, all(a), clip.fadeOutCurve);
        mixInto(out, a, static_cast<std::size_t>(std::llround(std::max(0.0, clip.at) * p.rate)), clip.gainDb);
    }
    out.resize(frames);
    applyRack(out, t.fx, t.automation);
    if (t.gainDb != 0) gain(out, t.gainDb, all(out));
    if (!t.envelope.empty()) {
        auto pts = t.envelope;
        std::sort(pts.begin(), pts.end(), [](const EnvelopePoint& a, const EnvelopePoint& b) { return a.time < b.time; });
        std::size_t k = 0;
        for (std::size_t i = 0; i < frames; ++i) {
            const double time = static_cast<double>(i) / p.rate;
            while (k + 1 < pts.size() && pts[k + 1].time <= time) ++k;
            double db = pts[k].gainDb;
            if (time < pts[0].time) db = pts[0].gainDb;
            else if (k + 1 < pts.size()) {
                const double span = pts[k + 1].time - pts[k].time;
                const double f = span > 0 ? (time - pts[k].time) / span : 1.0;
                db = pts[k].gainDb + (pts[k + 1].gainDb - pts[k].gainDb) * f;
            }
            const auto g = static_cast<float>(dbToGain(db));
            for (auto& c : out.channels) c[i] *= g;
        }
    }
    return out;
}

// Gain curve (dB per frame) that drops by duckDb while `key` is sounding.
std::vector<float> duckCurve(const Track& t, const Buffer& key, int rate) {
    const std::size_t n = key.frames();
    const std::size_t win = std::max<std::size_t>(1, static_cast<std::size_t>(rate / 50));  // 20 ms
    const std::size_t windows = (n + win - 1) / win;
    std::vector<char> on(windows, 0);
    const double thr = dbToGain(t.duckThresholdDb);
    for (std::size_t w = 0; w < windows; ++w) {
        double loudest = 0;
        for (const auto& c : key.channels) {
            double s = 0;
            const std::size_t e = std::min(n, (w + 1) * win);
            for (std::size_t i = w * win; i < e; ++i) s += static_cast<double>(c[i]) * c[i];
            loudest = std::max(loudest, std::sqrt(s / static_cast<double>(e - w * win)));
        }
        on[w] = loudest > thr;
    }
    // hold 300 ms through gaps between words, and start early by the attack time
    const auto hold = static_cast<std::size_t>(0.3 * 50), lead = static_cast<std::size_t>(std::max(0.0, t.duckAttack) * 50);
    std::vector<char> held = on;
    std::size_t since = hold + 1;
    for (std::size_t w = 0; w < windows; ++w) {
        since = on[w] ? 0 : since + 1;
        if (since <= hold) held[w] = 1;
    }
    for (std::size_t w = 0; w < windows; ++w) {
        if (on[w] && (w == 0 || !on[w - 1])) {
            for (std::size_t k = w > lead ? w - lead : 0; k < w; ++k) held[k] = 1;
        }
    }
    const double depth = std::min(0.0, t.duckDb);
    const double down = -depth / std::max(0.01, t.duckAttack) / rate, up = -depth / std::max(0.01, t.duckRelease) / rate;
    std::vector<float> curve(n);
    double cur = 0;
    for (std::size_t i = 0; i < n; ++i) {
        const double target = held[i / win] ? depth : 0.0;
        if (cur > target) cur = std::max(target, cur - down);
        else cur = std::min(target, cur + up);
        curve[i] = static_cast<float>(cur);
    }
    return curve;
}

void applyPan(Buffer& b, double position) {
    if (b.channelCount() < 2 || position == 0) return;
    pan(b, position, all(b));
}

}  // namespace

Buffer renderTrack(const Project& p, int index) {
    if (index < 0 || index >= static_cast<int>(p.tracks.size())) throw std::runtime_error("no track " + std::to_string(index));
    const std::size_t frames = framesOf(p);
    const Track& t = p.tracks[static_cast<std::size_t>(index)];
    Buffer out = renderDry(p, t, frames);
    if (t.duckUnder >= 0 && t.duckUnder < static_cast<int>(p.tracks.size()) && t.duckUnder != index) {
        const Buffer key = renderDry(p, p.tracks[static_cast<std::size_t>(t.duckUnder)], frames);
        const auto curve = duckCurve(t, key, p.rate);
        for (std::size_t i = 0; i < frames; ++i) {
            const auto g = static_cast<float>(dbToGain(curve[i]));
            for (auto& c : out.channels) c[i] *= g;
        }
    }
    if (const Automation* lane = findLane(t.automation, "pan"); lane && out.channelCount() >= 2) applyPanLane(out, *lane);
    else applyPan(out, t.pan);
    return out;
}

Buffer mixdown(const Project& p) {
    const std::size_t frames = framesOf(p);
    Buffer out(p.channels, frames, p.rate);
    bool anySolo = false;
    for (const auto& t : p.tracks) anySolo = anySolo || t.solo;
    std::vector<Buffer> busIn;
    for (std::size_t i = 0; i < p.buses.size(); ++i) busIn.emplace_back(p.channels, frames, p.rate);
    for (std::size_t i = 0; i < p.tracks.size(); ++i) {
        const Track& t = p.tracks[i];
        if (anySolo ? !t.solo : t.mute) continue;
        const Buffer tr = renderTrack(p, static_cast<int>(i));
        mixInto(out, tr, 0, 0.0);
        for (const auto& snd : t.sends)
            if (snd.bus >= 0 && snd.bus < static_cast<int>(busIn.size()) && snd.levelDb > -90) mixInto(busIn[static_cast<std::size_t>(snd.bus)], tr, 0, snd.levelDb);
    }
    for (std::size_t i = 0; i < p.buses.size(); ++i) {
        const Bus& b = p.buses[i];
        if (b.mute) continue;
        Buffer& in = busIn[i];
        applyRack(in, b.fx, b.automation);
        if (const Automation* lane = findLane(b.automation, "volume")) applyVolumeLane(in, *lane);
        mixInto(out, in, 0, b.gainDb);
    }
    if (p.masterGainDb != 0) gain(out, p.masterGainDb, all(out));
    applyRack(out, p.masterFx, p.masterAutomation);
    if (const Automation* lane = findLane(p.masterAutomation, "volume")) applyVolumeLane(out, *lane);
    if (const Automation* lane = findLane(p.masterAutomation, "pan"); lane && out.channelCount() >= 2) applyPanLane(out, *lane);
    // a tail that rang out early: drop its silent end (never shorter than the clips)
    const std::size_t music = static_cast<std::size_t>(std::ceil(p.seconds() * p.rate));
    if (out.frames() > music) {
        std::size_t last = music;
        for (std::size_t i = out.frames(); i > music; --i) {
            bool loud = false;
            for (const auto& c : out.channels) loud = loud || std::fabs(c[i - 1]) > 3e-5f;
            if (loud) {
                last = i;
                break;
            }
        }
        out.resize(last == music ? music : std::min(out.frames(), last + static_cast<std::size_t>(p.rate / 100)));
    }
    return out;
}

raster::Image renderProject(const Project& p, int width, int laneHeight) {
    using raster::Color;
    width = std::clamp(width, 64, 8192);
    laneHeight = std::clamp(laneHeight, 24, 1024);
    const int lanes = std::max<int>(1, static_cast<int>(p.tracks.size()));
    const int ruler = 16;
    raster::Image img(width, lanes * laneHeight + ruler, {24, 26, 31, 255});
    const double total = std::max(1.0, p.seconds());
    bool anySolo = false;
    for (const auto& t : p.tracks) anySolo = anySolo || t.solo;
    const Color palette[] = {{90, 190, 255, 255}, {255, 170, 80, 255}, {120, 220, 140, 255}, {230, 120, 200, 255}, {240, 220, 100, 255}};
    for (std::size_t ti = 0; ti < p.tracks.size(); ++ti) {
        const Track& t = p.tracks[ti];
        const int top = static_cast<int>(ti) * laneHeight;
        const bool audible = anySolo ? t.solo : !t.mute;
        const Color col = audible ? palette[ti % 5] : Color{110, 112, 118, 255};
        detail::hline(img, 0, width - 1, top + laneHeight - 1, {70, 74, 82, 255});
        // clip boxes, then the track as it will sound: fades, gain, envelope and ducking included
        for (const auto& clip : t.clips) {
            if (!clip.audio) continue;
            const int x0 = static_cast<int>(clip.at / total * (width - 1));
            const int x1 = std::max(x0 + 1, static_cast<int>(clip.end() / total * (width - 1)));
            for (int x = x0; x <= x1; ++x) detail::vline(img, x, top + 3, top + laneHeight - 4, col, 0.18f);
            detail::vline(img, x0, top + 3, top + laneHeight - 4, col);
            detail::vline(img, x1, top + 3, top + laneHeight - 4, col, 0.5f);
        }
        if (t.clips.empty()) continue;
        const Buffer a = renderTrack(p, static_cast<int>(ti));
        const double mid = top + laneHeight / 2.0, half = laneHeight / 2.0 - 6;
        const double perPixel = total * p.rate / width;
        for (int x = 0; x < width; ++x) {
            const auto i0 = static_cast<std::size_t>(x * perPixel);
            const auto i1 = std::min(a.frames(), std::max(i0 + 1, static_cast<std::size_t>((x + 1) * perPixel)));
            if (i0 >= a.frames()) break;
            float lo = 0, hi = 0;
            for (const auto& c : a.channels) {
                for (std::size_t i = i0; i < i1; ++i) lo = std::min(lo, c[i]), hi = std::max(hi, c[i]);
            }
            if (hi - lo < 1e-4f) continue;
            const int y0 = static_cast<int>(mid - std::clamp(static_cast<double>(hi), -1.0, 1.0) * half);
            const int y1 = static_cast<int>(mid - std::clamp(static_cast<double>(lo), -1.0, 1.0) * half);
            detail::vline(img, x, y0, y1, col);
        }
        // track number in the corner
        detail::text(img, 4, top + 4, std::to_string(ti), {230, 230, 235, 255});
    }
    detail::timeGrid(img, 0, total, {60, 64, 72, 255}, true);
    return img;
}

namespace {

json rackJson(const Rack& r) {
    json a = json::array();
    for (const auto& e : r) {
        json prm = json::object();
        for (const auto& [k, v] : e.params) prm[k] = v;
        a.push_back({{"type", e.type}, {"bypass", e.bypass}, {"params", prm}});
    }
    return a;
}

Rack rackOf(const json& a) {
    Rack r;
    if (!a.is_array()) return r;
    for (const auto& je : a) {
        Effect e;
        e.type = je.value("type", std::string());
        if (!findEffectType(e.type)) throw std::runtime_error("unknown effect \"" + e.type + "\" in the file");
        e.bypass = je.value("bypass", false);
        const json prm = je.value("params", json::object());
        for (const auto& [k, v] : prm.items())
            if (v.is_number()) e.params[k] = v.get<double>();
        r.push_back(std::move(e));
    }
    return r;
}

json lanesJson(const std::vector<Automation>& lanes) {
    json a = json::array();
    for (const auto& l : lanes) {
        json pts = json::array();
        for (const auto& pt : l.points) pts.push_back({pt.time, pt.value});
        a.push_back({{"target", l.target}, {"points", pts}});
    }
    return a;
}

std::vector<Automation> lanesOf(const json& a) {
    std::vector<Automation> out;
    if (!a.is_array()) return out;
    for (const auto& jl : a) {
        Automation l;
        l.target = jl.value("target", std::string());
        for (const auto& pt : jl.value("points", json::array()))
            if (pt.is_array() && pt.size() == 2) l.points.push_back({pt[0].get<double>(), pt[1].get<double>()});
        l.sort();
        if (!l.target.empty()) out.push_back(std::move(l));
    }
    return out;
}

std::string curveName(FadeCurve c) { return fadeCurveNames()[static_cast<std::size_t>(c)]; }

FadeCurve curveOf(const json& j, const char* key) {
    FadeCurve c = FadeCurve::SCurve;
    parseFadeCurve(j.value(key, std::string("s_curve")), c);
    return c;
}

}  // namespace

std::string rackToJson(const Rack& r) { return rackJson(r).dump(); }
Rack rackFromJson(const std::string& text) {
    try {
        return rackOf(json::parse(text));
    } catch (const json::exception& e) {
        throw std::runtime_error(std::string("bad effects rack: ") + e.what());
    }
}
std::string lanesToJson(const std::vector<Automation>& lanes) { return lanesJson(lanes).dump(); }
std::vector<Automation> lanesFromJson(const std::string& text) {
    try {
        return lanesOf(json::parse(text));
    } catch (const json::exception& e) {
        throw std::runtime_error(std::string("bad automation: ") + e.what());
    }
}

void saveProject(const Project& p, const fs::path& path) {
    const fs::path dir = path.parent_path() / (path.stem().string() + "_clips");
    std::error_code ec;
    fs::create_directories(dir, ec);
    if (ec) throw std::runtime_error("can't create " + dir.string() + ": " + ec.message());
    json j = {{"wetwaresuite_audio_project", 1}, {"rate", p.rate}, {"channels", p.channels}, {"master_gain", p.masterGainDb}};
    json tracks = json::array();
    SaveOptions wav;
    wav.format = SampleFormat::Float32;
    for (std::size_t ti = 0; ti < p.tracks.size(); ++ti) {
        const Track& t = p.tracks[ti];
        json clips = json::array();
        for (std::size_t ci = 0; ci < t.clips.size(); ++ci) {
            const Clip& c = t.clips[ci];
            const std::string file = "t" + std::to_string(ti + 1) + "_c" + std::to_string(ci + 1) + ".wav";
            if (c.audio) save(*c.audio, dir / file, wav);
            clips.push_back({{"name", c.name}, {"file", (fs::path(dir.filename()) / file).generic_string()}, {"at", c.at}, {"gain", c.gainDb},
                             {"fade_in", c.fadeIn}, {"fade_out", c.fadeOut}, {"fade_in_curve", curveName(c.fadeInCurve)},
                             {"fade_out_curve", curveName(c.fadeOutCurve)}, {"offset", c.offset}, {"length", c.length}});
        }
        json env = json::array();
        for (const auto& e : t.envelope) env.push_back({e.time, e.gainDb});
        tracks.push_back({{"name", t.name}, {"gain", t.gainDb}, {"pan", t.pan}, {"mute", t.mute}, {"solo", t.solo},
                          {"duck_under", t.duckUnder}, {"duck_db", t.duckDb}, {"duck_threshold", t.duckThresholdDb},
                          {"duck_attack", t.duckAttack}, {"duck_release", t.duckRelease}, {"envelope", env},
                          {"fx", rackJson(t.fx)}, {"automation", lanesJson(t.automation)}, {"clips", clips}});
        if (!t.sends.empty()) {
            json sends = json::array();
            for (const auto& snd : t.sends) sends.push_back({{"bus", snd.bus}, {"level", snd.levelDb}});
            tracks.back()["sends"] = sends;
        }
    }
    if (!p.buses.empty()) {
        json buses = json::array();
        for (const auto& b : p.buses)
            buses.push_back({{"name", b.name}, {"gain", b.gainDb}, {"mute", b.mute}, {"fx", rackJson(b.fx)}, {"automation", lanesJson(b.automation)}});
        j["buses"] = buses;
    }
    j["tracks"] = tracks;
    j["master_fx"] = rackJson(p.masterFx);
    j["master_automation"] = lanesJson(p.masterAutomation);
    json markers = json::array();
    for (const auto& m : p.markers) markers.push_back({{"name", m.name}, {"start", m.start}, {"end", m.end}});
    j["markers"] = markers;
    std::ofstream out(path);
    if (!(out << j.dump(2) << "\n")) throw std::runtime_error("can't write " + path.string());
}

Project loadProject(const fs::path& path) {
    std::ifstream in(path);
    if (!in) throw std::runtime_error("can't open " + path.string());
    json j;
    try {
        j = json::parse(in);
    } catch (const std::exception& e) {
        throw std::runtime_error(path.string() + " isn't a project file: " + e.what());
    }
    // achclwss_audio_project: projects saved before the rename
    if (!j.contains("wetwaresuite_audio_project") && !j.contains("achclwss_audio_project")) {
        throw std::runtime_error(path.string() + " isn't a wetwaresuite audio project");
    }
    Project p;
    p.rate = j.value("rate", 44100);
    p.channels = std::clamp(j.value("channels", 2), 1, 32);
    p.masterGainDb = j.value("master_gain", 0.0);
    p.masterFx = rackOf(j.value("master_fx", json::array()));
    p.masterAutomation = lanesOf(j.value("master_automation", json::array()));
    for (const auto& jb : j.value("buses", json::array())) {
        Bus b;
        b.name = jb.value("name", std::string());
        b.gainDb = jb.value("gain", 0.0);
        b.mute = jb.value("mute", false);
        b.fx = rackOf(jb.value("fx", json::array()));
        b.automation = lanesOf(jb.value("automation", json::array()));
        p.buses.push_back(std::move(b));
    }
    for (const auto& jm : j.value("markers", json::array())) p.markers.push_back({jm.value("name", std::string()), jm.value("start", 0.0), jm.value("end", -1.0)});
    for (const auto& jt : j.value("tracks", json::array())) {
        Track t;
        t.name = jt.value("name", std::string());
        t.gainDb = jt.value("gain", 0.0);
        t.pan = jt.value("pan", 0.0);
        t.mute = jt.value("mute", false);
        t.solo = jt.value("solo", false);
        t.duckUnder = jt.value("duck_under", -1);
        t.duckDb = jt.value("duck_db", -12.0);
        t.duckThresholdDb = jt.value("duck_threshold", -40.0);
        t.duckAttack = jt.value("duck_attack", 0.15);
        t.duckRelease = jt.value("duck_release", 0.6);
        t.fx = rackOf(jt.value("fx", json::array()));
        t.automation = lanesOf(jt.value("automation", json::array()));
        for (const auto& js : jt.value("sends", json::array())) t.sends.push_back({js.value("bus", 0), js.value("level", 0.0)});
        for (const auto& e : jt.value("envelope", json::array())) {
            if (e.is_array() && e.size() == 2) t.envelope.push_back({e[0].get<double>(), e[1].get<double>()});
        }
        for (const auto& jc : jt.value("clips", json::array())) {
            Clip c;
            c.name = jc.value("name", std::string());
            c.at = jc.value("at", 0.0);
            c.gainDb = jc.value("gain", 0.0);
            c.fadeIn = jc.value("fade_in", 0.0);
            c.fadeOut = jc.value("fade_out", 0.0);
            c.fadeInCurve = curveOf(jc, "fade_in_curve");
            c.fadeOutCurve = curveOf(jc, "fade_out_curve");
            c.offset = std::max(0.0, jc.value("offset", 0.0));
            c.length = jc.value("length", -1.0);
            const fs::path file = path.parent_path() / jc.value("file", std::string());
            Buffer a = load(file);
            if (a.rate != p.rate) a = resample(a, p.rate);
            c.audio = std::make_shared<const Buffer>(std::move(a));
            t.clips.push_back(std::move(c));
        }
        p.tracks.push_back(std::move(t));
    }
    return p;
}

}  // namespace wetwaresuite::audio
