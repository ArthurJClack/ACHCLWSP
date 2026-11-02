#include "wetwaresuite/audio/document.hpp"

#include <algorithm>
#include <cstdio>

#include "wetwaresuite/audio/analysis.hpp"
#include "wetwaresuite/audio/io.hpp"
#include "wetwaresuite/raster/codec.hpp"

namespace wetwaresuite::audio {

namespace {

std::string wavePng(const Buffer& b, Range r) {
    WaveformStyle st;
    st.width = 1024;
    st.height = std::clamp(140 * std::max(1, b.channelCount()), 200, 560);
    return raster::encodePng(waveform(b, r, st));
}

}  // namespace

std::string AudioDoc::summary() const {
    std::string s = describe(audio);
    if (!path.empty()) s += ", " + path;
    return s;
}

std::unique_ptr<Attachment> AudioDoc::preview() const {
    return std::make_unique<Attachment>(Attachment{"image/png", wavePng(audio, all(audio)), "waveform.png"});
}

std::string ProjectDoc::summary() const {
    std::size_t clips = 0;
    for (const auto& t : project.tracks) clips += t.clips.size();
    std::string s = std::to_string(project.tracks.size()) + " tracks, " + std::to_string(clips) + " clips, " +
                    formatTime(project.seconds()) + ", " + std::to_string(project.rate) + " Hz " +
                    (project.channels == 1 ? "mono" : project.channels == 2 ? "stereo" : std::to_string(project.channels) + " ch");
    if (!path.empty()) s += ", " + path;
    return s;
}

std::unique_ptr<Attachment> ProjectDoc::preview() const {
    return std::make_unique<Attachment>(Attachment{"image/png", raster::encodePng(renderProject(project)), "project.png"});
}

bool ProjectDoc::timeline(TimelineLayout& out) const {
    out.docParam = "project";
    out.tracks.clear();
    for (std::size_t ti = 0; ti < project.tracks.size(); ++ti) {
        const Track& t = project.tracks[ti];
        TimelineTrack tt;
        tt.id = std::to_string(ti);
        tt.name = (t.name.empty() ? "track " + std::to_string(ti) : t.name) + (t.mute ? " (muted)" : t.solo ? " (solo)" : "");
        tt.kind = "audio";
        for (std::size_t ci = 0; ci < t.clips.size(); ++ci) {
            const Clip& c = t.clips[ci];
            TimelineClip tc;
            tc.id = std::to_string(ci);
            tc.label = c.name;
            tc.start = c.at;
            tc.length = c.seconds();
            tc.headroom = c.offset;
            tc.tailroom = std::max(0.0, c.sourceSeconds() - c.offset - c.seconds());
            tc.fadeIn = c.fadeIn;
            tc.fadeOut = c.fadeOut;
            tc.gainDb = c.gainDb;
            tc.gainHandle = true;
            tt.clips.push_back(tc);
        }
        out.tracks.push_back(std::move(tt));
    }
    return true;
}

std::string ProjectDoc::timelineCommand(const TimelineEdit& e) const {
    char buf[48];
    auto t4 = [&buf](double v) {
        std::snprintf(buf, sizeof buf, "%.4f", v);
        return std::string(buf);
    };
    const std::string which = " track=" + e.track + " clip=" + e.clip;
    switch (e.kind) {
        case TimelineEditKind::Move: {
            std::string line = "audio.clip_set" + which + " at=" + t4(std::max(0.0, e.start));
            if (!e.toTrack.empty() && e.toTrack != e.track) line += " to_track=" + e.toTrack;
            return line;
        }
        case TimelineEditKind::TrimStart: return "audio.clip_trim" + which + " start=" + t4(std::max(0.0, e.start));
        case TimelineEditKind::TrimEnd: return "audio.clip_trim" + which + " length=" + t4(std::max(0.001, e.length));
        case TimelineEditKind::Split: return "audio.clip_split" + which + " at=" + t4(e.at);
        case TimelineEditKind::Delete: return "audio.clip_remove" + which;
        case TimelineEditKind::SetProperty:
            if (e.name == "fade_in" || e.name == "fade_out") return "audio.clip_set" + which + " " + e.name + "=" + t4(std::max(0.0, e.at));
            if (e.name == "gain") return "audio.clip_set" + which + " gain=" + t4(std::clamp(e.at, -60.0, 24.0));
            break;
        default: break;
    }
    return {};
}

std::shared_ptr<AudioDoc> newAudio(Buffer b) {
    auto doc = std::make_shared<AudioDoc>();
    doc->audio = std::move(b);
    return doc;
}

std::shared_ptr<AudioDoc> openAudio(const std::filesystem::path& path) {
    auto doc = newAudio(load(path));
    doc->path = path.string();
    return doc;
}

Result AudioEdit::apply() {
    before_ = doc_->audio;
    beforeNoise_ = doc_->noise;
    beforeMarkers_ = doc_->markers;
    Result r;
    try {
        r = fn_(*doc_);
    } catch (const std::exception& e) {
        r = Result::failure(e.what());
    }
    if (!r.ok) revert();
    return r;
}

void AudioEdit::revert() {
    doc_->audio = before_;
    doc_->noise = beforeNoise_;
    doc_->markers = beforeMarkers_;
}

Result ProjectEdit::apply() {
    before_ = doc_->project;
    Result r;
    try {
        r = fn_(*doc_);
    } catch (const std::exception& e) {
        r = Result::failure(e.what());
    }
    if (!r.ok) revert();
    return r;
}

void ProjectEdit::revert() { doc_->project = before_; }

ParamSpec docParam() { return {"doc", ParamType::String, "id of an open audio document, e.g. aud1", true}; }
ParamSpec projectParam() { return {"project", ParamType::String, "id of an open multitrack project, e.g. proj1", true}; }
ParamSpec previewParam() { return {"preview", ParamType::Bool, "attach a waveform PNG of the result", false, {}, false}; }
ParamSpec startParam() { return {"start", ParamType::Number, "start of the time range in seconds", false, {}, 0.0}; }
ParamSpec endParam() { return {"end", ParamType::Number, "end of the time range in seconds (default: the end)", false}; }
ParamSpec channelsParam() {
    return {"channels", ParamType::NumberList, "channels to change, 0 = left, 1 = right (default: all)", false};
}

namespace {

template <typename DocT>
ActionFactory makeFactory(Session& session, const std::string& key, const std::string& what,
                          std::function<Result(DocT&, const Params&)> edit, bool undoable,
                          std::function<std::unique_ptr<Action>(std::shared_ptr<DocT>, std::function<Result(DocT&)>)> wrap) {
    return [&session, key, what, edit, undoable, wrap](const Params& p) -> std::unique_ptr<Action> {
        const std::string id = stringParam(p, key);
        auto doc = session.workspace().getAs<DocT>(id);
        if (!doc) {
            return std::make_unique<FunctionAction>([id, what] { return Result::failure("no open " + what + " called '" + id + "'"); });
        }
        if (!undoable) {
            return std::make_unique<FunctionAction>([doc, edit, p] {
                try {
                    return edit(*doc, p);
                } catch (const std::exception& e) {
                    return Result::failure(e.what());
                }
            });
        }
        return wrap(doc, [edit, p](DocT& d) { return edit(d, p); });
    };
}

}  // namespace

ActionFactory audioCommand(Session& session, std::function<Result(AudioDoc&, const Params&)> edit, bool undoable) {
    return makeFactory<AudioDoc>(session, "doc", "audio document", std::move(edit), undoable,
                                 [](std::shared_ptr<AudioDoc> d, std::function<Result(AudioDoc&)> fn) -> std::unique_ptr<Action> {
                                     return std::make_unique<AudioEdit>(std::move(d), std::move(fn));
                                 });
}

ActionFactory projectCommand(Session& session, std::function<Result(ProjectDoc&, const Params&)> edit, bool undoable) {
    return makeFactory<ProjectDoc>(session, "project", "project", std::move(edit), undoable,
                                   [](std::shared_ptr<ProjectDoc> d, std::function<Result(ProjectDoc&)> fn) -> std::unique_ptr<Action> {
                                       return std::make_unique<ProjectEdit>(std::move(d), std::move(fn));
                                   });
}

bool rangeArg(const Buffer& b, const Params& p, Range& out, std::string& error) {
    const double start = numberParam(p, "start", 0.0);
    const double end = p.count("end") ? numberParam(p, "end", -1.0) : -1.0;
    if (start < 0) {
        error = "start can't be negative";
        return false;
    }
    if (start > b.seconds() + 1e-9) {
        error = "start (" + formatTime(start) + ") is past the end of the audio (" + formatTime(b.seconds()) + ")";
        return false;
    }
    if (p.count("end") && end <= start) {
        error = "end must be after start";
        return false;
    }
    out = rangeOf(b, start, end);
    return true;
}

bool channelsArg(const Buffer& b, const Params& p, Channels& out, std::string& error) {
    out.clear();
    for (double v : listParam(p, "channels")) {
        const int c = static_cast<int>(v);
        if (c < 0 || c >= b.channelCount() || c != v) {
            error = "channel " + toText(v) + " doesn't exist (the audio has " + std::to_string(b.channelCount()) + ", numbered from 0)";
            return false;
        }
        out.push_back(c);
    }
    return true;
}

Result& maybePreview(Result& r, const AudioDoc& doc, const Params& p) {
    if (!r.ok || !boolParam(p, "preview", false)) return r;
    r.attach({"image/png", wavePng(doc.audio, all(doc.audio)), "waveform.png"});
    return r;
}

Result& maybePreview(Result& r, const ProjectDoc& doc, const Params& p) {
    if (!r.ok || !boolParam(p, "preview", false)) return r;
    r.attach({"image/png", raster::encodePng(renderProject(doc.project)), "project.png"});
    return r;
}

Result audioResult(const std::string& message, const AudioDoc& doc, const Params& p) {
    Result r = Result::success(message);
    r.with("doc", stringParam(p, "doc"))
        .with("seconds", doc.audio.seconds())
        .with("rate", static_cast<std::int64_t>(doc.audio.rate))
        .with("channels", static_cast<std::int64_t>(doc.audio.channelCount()));
    return maybePreview(r, doc, p);
}

}  // namespace wetwaresuite::audio
