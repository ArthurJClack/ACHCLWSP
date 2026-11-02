// audio.play and the transport (pause, stop, seek...): hear a clip, a project
// mix or a file through the speakers, headless or from the window.
#include <chrono>
#include <thread>

#include "common.hpp"
#include "wetwaresuite/audio/project.hpp"
#include "wetwaresuite/playback/commands.hpp"

namespace wetwaresuite::audio::tool {

void registerPlayCommands(Session& session) {
    auto& reg = session.registry();
    Session* s = &session;

    std::vector<ParamSpec> params = {
        {"doc", ParamType::String, "an open audio document, e.g. aud1", false},
        {"project", ParamType::String, "an open project to play the mix of, e.g. proj1", false},
        {"path", ParamType::String, "or an audio file to play without opening it", false}};
    for (auto& p : playback::playParams()) params.push_back(p);

    reg.add({"audio.play",
             "Play",
             "Play audio through the speakers: an open clip (doc), a project's mix (project) or a file (path), from "
             "start to end, optionally looping. Returns at once (wait=true blocks until done); audio.playhead says where "
             "it is, audio.pause/resume/stop/seek control it. From the command line the program stays open until the "
             "sound finishes. With no sound card it plays silently so scripts still work.",
             params, false, true},
            [s](const Params& p) {
                return std::make_unique<FunctionAction>([s, p] {
                    try {
                        std::shared_ptr<const Buffer> buffer;
                        std::string source;
                        if (p.count("doc")) {
                            source = stringParam(p, "doc");
                            auto doc = s->workspace().getAs<AudioDoc>(source);
                            if (!doc) return failed("no audio document " + source + " (open one with audio.open)");
                            buffer = std::make_shared<const Buffer>(doc->audio);
                        } else if (p.count("project")) {
                            source = stringParam(p, "project");
                            auto doc = s->workspace().getAs<ProjectDoc>(source);
                            if (!doc) return failed("no audio project " + source);
                            if (doc->project.seconds() <= 0) return failed("the project has no clips yet (add some with audio.clip_add)");
                            buffer = std::make_shared<const Buffer>(mixdown(doc->project));
                        } else if (p.count("path")) {
                            const fs::path path = stringParam(p, "path");
                            source = path.filename().string();
                            buffer = std::make_shared<const Buffer>(load(path));
                        } else {
                            return failed("say what to play: doc=aud1, project=proj1 or path=file.wav");
                        }
                        return playback::startPlayback(std::move(buffer), source, p);
                    } catch (const std::exception& e) {
                        return failed(e.what());
                    }
                });
            });

    // live playback: re-render the playing clip or project mix after an edit
    playback::registerTransportCommands(session, "audio", [s](const std::string& source, std::string&) -> std::shared_ptr<const Buffer> {
        if (auto doc = s->workspace().getAs<AudioDoc>(source)) return std::make_shared<const Buffer>(doc->audio);
        if (auto doc = s->workspace().getAs<ProjectDoc>(source)) return std::make_shared<const Buffer>(mixdown(doc->project));
        return nullptr;
    });

    // ---- recording ----
    auto statusResult = [](const std::string& message) {
        const playback::RecordStatus st = playback::Recorder::instance().status();
        Result r = Result::success(message);
        r.with("recording", st.recording)
            .with("seconds", st.seconds)
            .with("peak_db", st.peakDb)
            .with("max_peak_db", st.maxPeakDb)
            .with("clipped", st.maxPeakDb > -0.1)
            .with("device", st.device)
            .with("silent", st.silent);
        return r;
    };
    auto finish = [s, statusResult](const std::string& how) {
        const playback::RecordStatus st = playback::Recorder::instance().status();
        Buffer take = playback::Recorder::instance().stop();
        if (take.empty()) return failed("nothing was recorded (start with audio.record_start)");
        auto doc = newAudio(std::move(take));
        const std::string id = s->workspace().add(doc, "aud");
        Result r = statusResult(how + " " + id + ": " + describe(doc->audio) + " from " + st.device +
                                (st.maxPeakDb > -0.1 ? " (it clipped: turn the input down)" : "") + (st.silent ? " (no input device: silence)" : ""));
        r.with("doc", id).with("seconds", doc->audio.seconds());
        return r;
    };
    const std::vector<ParamSpec> recordParams = {
        {"device", ParamType::String, "input device from audio.record_devices, or \"tone\" for a 440 Hz test tone", false, {}, std::string("default")},
        {"rate", ParamType::Int, "sample rate in Hz", false, {}, std::int64_t{44100}},
        {"channels", ParamType::Int, "1 = mono (a mic), 2 = stereo", false, {}, std::int64_t{1}}};

    auto start = [](const Params& p, double maxSeconds) -> std::string {
        std::string error;
        playback::Recorder::instance().start(static_cast<int>(intParam(p, "rate", 44100)), static_cast<int>(intParam(p, "channels", 1)),
                                             stringParam(p, "device", "default"), maxSeconds, error);
        return error;
    };
    {
        auto params = recordParams;
        params.push_back({"max_seconds", ParamType::Number, "stop by itself after this long", false, {}, 3600.0});
        reg.add({"audio.record_start", "Start recording",
                 "Record from a microphone or line in until audio.record_stop, which makes a new audio document of the "
                 "take. audio.record_status gives the time and the input level while it runs. With no sound card it "
                 "records silence, so scripts still work.",
                 params, false, true},
                [start, statusResult](const Params& p) {
                    return std::make_unique<FunctionAction>([p, start, statusResult] {
                        const std::string error = start(p, numberParam(p, "max_seconds", 3600.0));
                        if (!error.empty()) return failed(error);
                        return statusResult("recording from " + playback::Recorder::instance().status().device);
                    });
                });
    }
    reg.add({"audio.record_stop", "Stop recording",
             "End the take started by audio.record_start and open it as a new audio document. To drop it on a project "
             "track, follow with audio.clip_add doc=<it> project=... track=... at=....",
             {}, false, true},
            [finish](const Params&) { return std::make_unique<FunctionAction>([finish] { return finish("recorded"); }); });
    reg.add({"audio.record_status", "Recording status", "Whether it's recording, for how long, and the input level (peak_db now, max_peak_db so far).",
             {}, false, true},
            [statusResult](const Params&) {
                return std::make_unique<FunctionAction>([statusResult] {
                    const auto st = playback::Recorder::instance().status();
                    return statusResult(st.recording ? "recording, " + formatTime(st.seconds) + ", level " + dbText(st.peakDb) : "not recording");
                });
            });
    {
        auto params = recordParams;
        params.insert(params.begin(), {"seconds", ParamType::Number, "how long to record", true});
        reg.add({"audio.record", "Record",
                 "Record for a set number of seconds (blocks until done, max 600) and open the take as a new audio "
                 "document: the one-step version of record_start + record_stop for scripts.",
                 params, false, true},
                [start, finish](const Params& p) {
                    return std::make_unique<FunctionAction>([p, start, finish] {
                        const double secs = numberParam(p, "seconds", 0.0);
                        if (!(secs > 0 && secs <= 600)) return failed("seconds must be 0..600");
                        const std::string error = start(p, secs);
                        if (!error.empty()) return failed(error);
                        const auto until = std::chrono::steady_clock::now() + std::chrono::duration<double>(secs + 2);
                        while (playback::Recorder::instance().status().recording && std::chrono::steady_clock::now() < until)
                            std::this_thread::sleep_for(std::chrono::milliseconds(20));
                        return finish("recorded");
                    });
                });
    }
    reg.add({"audio.record_devices", "Input devices", "List the microphones and inputs audio.record_start can use (\"tone\" is a built-in test tone).",
             {}, false, true},
            [](const Params&) {
                return std::make_unique<FunctionAction>([] {
                    const auto list = playback::Recorder::instance().devices();
                    std::string text;
                    for (const auto& d : list) text += (text.empty() ? "" : "\n") + d;
                    Result r = Result::success(text);
                    r.with("count", static_cast<std::int64_t>(list.size()));
                    for (std::size_t i = 0; i < list.size(); ++i) r.with("device_" + std::to_string(i), list[i]);
                    return r;
                });
            });
}

}  // namespace wetwaresuite::audio::tool
