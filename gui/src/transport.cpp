// The play bar over sound documents (clips, audio projects, songs, video
// timelines): Play/Pause, Stop, a seek bar, loop and volume, plus Space to play
// and pause and a playhead on the waveform. Everything goes through the
// <tool>.play / pause / seek ... commands, so the window does exactly what a
// script or Claude would.
#include <algorithm>
#include <cmath>
#include <cstdio>

#include "shell.hpp"

namespace wetwaresuite::gui {

namespace {

std::string clock(double seconds) {
    seconds = std::max(0.0, seconds);
    const int m = static_cast<int>(seconds / 60);
    char buf[32];
    std::snprintf(buf, sizeof buf, "%d:%04.1f", m, seconds - m * 60);
    return buf;
}

double numberField(const Result& r, const char* key) {
    if (const Value* v = r.field(key)) {
        if (auto d = std::get_if<double>(v)) return *d;
        if (auto i = std::get_if<std::int64_t>(v)) return static_cast<double>(*i);
    }
    return 0;
}

bool boolField(const Result& r, const char* key) {
    if (const Value* v = r.field(key)) {
        if (auto b = std::get_if<bool>(v)) return *b;
    }
    return false;
}

std::string stringField(const Result& r, const char* key) {
    if (const Value* v = r.field(key)) {
        if (auto s = std::get_if<std::string>(v)) return *s;
    }
    return {};
}

}  // namespace

bool Shell::playTarget(std::string& tool, Params& target) const {
    auto doc = session_.workspace().get(currentDoc_);
    if (!doc) return false;
    const std::string kind = doc->kind();
    if (kind == "audio") tool = "audio", target = {{"doc", currentDoc_}};
    else if (kind == "audio project") tool = "audio", target = {{"project", currentDoc_}};
    else if (kind == "song") tool = "beats", target = {{"song", currentDoc_}};
    else if (kind == "timeline") tool = "cut", target = {{"doc", currentDoc_}};
    else return false;
    return find(tool + ".play") != nullptr;
}

void Shell::pollTransport() {
    Transport& t = transport_;
    if (t.tool.empty() || !find(t.tool + ".playhead")) return;
    const Result r = session_.run(t.tool + ".playhead", {});
    if (!r.ok) return;
    t.playing = boolField(r, "playing");
    t.paused = boolField(r, "paused");
    t.position = numberField(r, "position");
    t.length = numberField(r, "length");
    t.start = numberField(r, "start");
    t.end = numberField(r, "end");
    t.source = stringField(r, "source");
    t.silent = boolField(r, "silent");
}

void Shell::startPlaying(double from) {
    std::string tool;
    Params params;
    if (!playTarget(tool, params)) return;
    transport_.tool = tool;
    params["start"] = std::max(0.0, from);
    params["loop"] = transport_.loop;
    params["volume"] = static_cast<double>(transport_.volumeDb);
    Result r = session_.run(tool + ".play", params);
    if (!r.ok) {
        resultFor_ = "Play";
        r.message = friendlyError(r.message);
        result_ = std::move(r);
        scrollToResult_ = true;
        return;
    }
    transport_.doc = currentDoc_;
    transport_.liveSeen = changes_;
    pollTransport();
}

void Shell::togglePlay() {
    Transport& t = transport_;
    const bool ours = t.doc == currentDoc_ && t.source == currentDoc_;
    // a timeline has its own playhead: play starts there, and pausing leaves it where the picture stopped
    TimelineLayout layout;
    auto doc = session_.workspace().get(currentDoc_);
    const bool ownPlayhead = doc && doc->timeline(layout) && layout.cursor >= 0;
    if (ours && t.playing) {
        session_.run(t.tool + ".pause", {});
        pollTransport();
        if (ownPlayhead) runTimelineEdit(layout, {TimelineEditKind::Cursor, {}, {}, {}, 0, 0, t.position});
    } else if (ours && t.paused) {
        session_.run(t.tool + ".resume", {});
    } else {
        // start from where the playhead was left, unless it ran to the end
        const bool atEnd = !ours || t.length <= 0 || t.position >= t.end - 0.01;
        double from = atEnd ? t.cue : t.position;
        if (atEnd && ownPlayhead) {
            double end = 0;
            for (const auto& tr : layout.tracks)
                for (const auto& c : tr.clips) end = std::max(end, c.start + c.length);
            from = layout.cursor < end - 0.05 ? layout.cursor : 0.0;  // from the top once it reached the end
        }
        startPlaying(from);
    }
    pollTransport();
}

void Shell::seekTo(double seconds) {
    Transport& t = transport_;
    t.cue = std::max(0.0, seconds);
    if (t.doc == currentDoc_ && (t.playing || t.paused)) {
        session_.run(t.tool + ".seek", {{"seconds", t.cue}});
        pollTransport();
    }
}

void Shell::transportBar() {
    std::string tool;
    Params target;
    if (!playTarget(tool, target)) {
        playheadFraction_ = -1;
        return;
    }
    Transport& t = transport_;
    if (t.tool.empty()) t.tool = tool;
    pollTransport();
    const bool ours = t.doc == currentDoc_ && t.source == currentDoc_;
    const bool playing = ours && t.playing;
    // live playback: an edit made while it plays is heard at once, from the same spot
    if (ours && (t.playing || t.paused) && changes_ != t.liveSeen && find(t.tool + ".refresh")) {
        session_.run(t.tool + ".refresh", {});
        pollTransport();
    }
    t.liveSeen = changes_;

    // Space plays and pauses, unless someone is typing
    if (!ImGui::GetIO().WantTextInput && ImGui::IsKeyPressed(ImGuiKey_Space, false)) togglePlay();

    if (accentButton(playing ? "Pause" : (ours && t.paused) ? "Resume" : "Play", {ImGui::GetFontSize() * 5, 0})) togglePlay();
    ImGui::SetItemTooltip("Play or pause (Space)");
    ImGui::SameLine();
    if (ImGui::Button("Stop")) {
        session_.run(tool + ".stop", {});
        t.cue = 0;
        pollTransport();
    }
    ImGui::SameLine();
    if (ImGui::Checkbox("Loop", &t.loop) && ours) session_.run(tool + ".loop", {{"on", t.loop}});
    ImGui::SetItemTooltip("Play the sound over and over");
    ImGui::SameLine();
    if (tool == "audio" && find("audio.record_start")) {
        recordButton();
        ImGui::SameLine();
    }
    if (tool == "cut" && find("cut.proxies")) {
        // big sources (4K and up) get small copies for smooth playback, made in the background
        const double now = ImGui::GetTime();
        if (t.proxySeen != changes_ || t.proxyDoc != currentDoc_) {
            t.proxySeen = changes_;
            t.proxyDoc = currentDoc_;
            const Result r = session_.run("cut.proxies", {{"doc", currentDoc_}, {"action", std::string(t.proxiesOn ? "make" : "status")}, {"wait", false}});
            t.proxyPolled = -10;
            (void)r;
        }
        if (now - t.proxyPolled > 1.0) {
            t.proxyPolled = now;
            const Result r = session_.run("cut.proxies", {{"doc", currentDoc_}});
            t.proxyCount = static_cast<int>(numberField(r, "count"));
            t.proxyReady = static_cast<int>(numberField(r, "ready"));
            t.proxyMaking = stringField(r, "rows").find("\tmaking\t") != std::string::npos;
            t.proxiesOn = boolField(r, "on");
        }
        if (t.proxyMaking) animating_ = true;  // keep drawing so the count moves while the window is idle
        if (t.proxyCount > 0) {
            bool on = t.proxiesOn;
            const std::string label = t.proxyMaking ? "Proxies " + std::to_string(t.proxyReady) + "/" + std::to_string(t.proxyCount) + "..." : std::string("Proxies");
            if (ImGui::Checkbox(label.c_str(), &on)) {
                t.proxiesOn = on;
                session_.run("cut.proxies", {{"doc", currentDoc_}, {"action", std::string(on ? "on" : "off")}});
                if (on) session_.run("cut.proxies", {{"doc", currentDoc_}, {"action", std::string("make")}, {"wait", false}});
                t.proxyPolled = -10;
            }
            ImGui::SetItemTooltip("%s", t.proxyMaking ? "Making small copies of the big clips so they play smoothly; export still uses the originals"
                                        : on ? "Playback reads small copies of the big clips; export still uses the originals"
                                             : "Playback reads the full-size clips (can stutter on 4K)");
            ImGui::SameLine();
        }
    }

    // a timeline keeps its own playhead: the bar shows and moves that one while stopped
    auto doc = session_.workspace().get(currentDoc_);
    TimelineLayout layout;
    const bool ownPlayhead = doc && doc->timeline(layout) && layout.cursor >= 0;
    const bool running = ours && (t.playing || t.paused);
    double length = ours && t.length > 0 ? t.length : 0;
    if (ownPlayhead && length <= 0) {
        for (const auto& tr : layout.tracks)
            for (const auto& c : tr.clips) length = std::max(length, c.start + c.length);
    }
    const double shown = running ? t.position : ownPlayhead ? layout.cursor : t.cue;
    ImGui::TextUnformatted((clock(shown) + (length > 0 ? " / " + clock(length) : std::string())).c_str());
    ImGui::SameLine();
    const float volumeWidth = ImGui::GetFontSize() * 7;
    ImGui::SetNextItemWidth(std::max(ImGui::GetFontSize() * 6, ImGui::GetContentRegionAvail().x - volumeWidth - ImGui::GetStyle().ItemSpacing.x));
    float pos = static_cast<float>(shown);
    const float maxPos = static_cast<float>(length > 0 ? length : std::max(1.0, shown));
    if (ImGui::SliderFloat("##playhead", &pos, 0.0f, maxPos, "", ImGuiSliderFlags_NoInput)) {
        if (ownPlayhead && !running) runTimelineEdit(layout, {TimelineEditKind::Cursor, {}, {}, {}, 0, 0, pos});
        else seekTo(pos);
    }
    ImGui::SetItemTooltip("Drag to move the playhead");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(volumeWidth);
    if (ImGui::SliderFloat("##volume", &t.volumeDb, -40.0f, 6.0f, "vol %.0f dB")) session_.run(tool + ".volume", {{"db", static_cast<double>(t.volumeDb)}});
    ImGui::SetItemTooltip("Playback volume (doesn't change the file)");
    if (ours && t.silent) ImGui::TextDisabled("No sound device found, so it plays silently.");

    // the playhead line only lines up with a clip's waveform
    playheadFraction_ = doc && doc->kind() == "audio" && length > 0 ? static_cast<float>(shown / length) : -1;
}

// Record: a new clip from the microphone; in a project, onto the picked track (or a new
// one) at the timeline cursor, with the project playing along so you can overdub.
void Shell::recordButton() {
    Transport& t = transport_;
    const float unit = ImGui::GetFontSize();
    auto doc = session_.workspace().get(currentDoc_);
    if (t.recording) {
        const Result st = session_.run("audio.record_status", {});
        t.recSeconds = numberField(st, "seconds");
        t.recPeakDb = numberField(st, "peak_db");
        t.recClipped = t.recClipped || boolField(st, "clipped");
        const bool stillGoing = boolField(st, "recording");
        ImGui::PushStyleColor(ImGuiCol_Button, IM_COL32(190, 40, 40, 255));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, IM_COL32(220, 60, 60, 255));
        const bool stop = ImGui::Button(("Stop rec " + clock(t.recSeconds)).c_str()) || !stillGoing;
        ImGui::PopStyleColor(2);
        ImGui::SetItemTooltip("Stop and keep the take");
        ImGui::SameLine();
        // input level: green, amber near the top, red once it clipped
        const ImVec2 o = ImGui::GetCursorScreenPos();
        const ImVec2 size{unit * 5, ImGui::GetFrameHeight()};
        ImGui::Dummy(size);
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const float level = static_cast<float>(std::clamp((t.recPeakDb + 60.0) / 60.0, 0.0, 1.0));
        dl->AddRectFilled(o, {o.x + size.x, o.y + size.y}, ImGui::GetColorU32(ImGuiCol_FrameBg), 3);
        const ImU32 col = t.recClipped ? IM_COL32(230, 60, 60, 255) : t.recPeakDb > -6 ? IM_COL32(230, 180, 60, 255) : IM_COL32(80, 200, 110, 255);
        dl->AddRectFilled({o.x + 2, o.y + 3}, {o.x + 2 + (size.x - 4) * level, o.y + size.y - 3}, col, 2);
        ImGui::SetItemTooltip(t.recClipped ? "It clipped: turn the input down and record again" : "Input level");
        if (stop) {
            t.recording = false;
            if (!t.recProject.empty()) session_.run("audio.stop", {});
            Result r = run("audio.record_stop", {});
            if (!r.ok) return;
            const std::string take = stringField(r, "doc");
            if (!t.recProject.empty() && session_.workspace().get(t.recProject) && !take.empty()) {
                // onto the picked track, else a new one
                std::int64_t track = -1;
                if (!timeline_.selTrack.empty() && timeline_.doc == t.recProject) track = std::atoi(timeline_.selTrack.c_str());
                if (track < 0) {
                    Result added = run("audio.track_add", {{"project", t.recProject}, {"name", std::string("Recording")}});
                    if (!added.ok) return;
                    track = static_cast<std::int64_t>(numberField(added, "track"));
                }
                run("audio.clip_add", {{"project", t.recProject}, {"track", track}, {"doc", take}, {"at", t.recAt}});
                currentDoc_ = t.recProject;
                canvas_.dirty = true;
            }
        }
        return;
    }
    ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(235, 80, 80, 255));
    const bool start = ImGui::Button("Rec");
    ImGui::PopStyleColor();
    const bool project = doc && doc->kind() == "audio project";
    ImGui::SetItemTooltip(project ? "Record onto the picked track at the cursor, with the project playing along" : "Record from the microphone into a new clip");
    if (!start) return;
    Result r = run("audio.record_start", {});
    if (!r.ok) return;
    t.recording = true;
    t.recClipped = false;
    t.recSeconds = 0;
    t.recProject = project ? currentDoc_ : std::string();
    t.recAt = project ? (timeline_.doc == currentDoc_ ? timeline_.cursor : 0.0) : 0.0;
    if (project) {
        TimelineLayout layout;
        double end = 0;
        if (doc->timeline(layout))
            for (const auto& tr : layout.tracks)
                for (const auto& c : tr.clips) end = std::max(end, c.start + c.length);
        if (end > t.recAt + 0.05) startPlaying(t.recAt);
    }
}

}  // namespace wetwaresuite::gui
