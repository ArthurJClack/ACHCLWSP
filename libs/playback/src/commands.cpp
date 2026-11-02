#include "wetwaresuite/playback/commands.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace wetwaresuite::playback {

namespace {

// MCP clients wait on a call, so a wait is capped; scripts can loop it.
constexpr double kMaxWait = 600;

std::string secs(double v) {
    char buf[32];
    std::snprintf(buf, sizeof buf, "%.2f s", v);
    return buf;
}

std::string describe(const Status& s) {
    if (s.source.empty()) return "nothing has played yet";
    std::string state = s.playing ? "playing" : s.paused ? "paused" : "stopped";
    std::string out = state + " " + s.source + " at " + audio::formatTime(s.position) + " of " + audio::formatTime(s.length);
    if (s.loop) out += ", looping " + audio::formatTime(s.start) + " to " + audio::formatTime(s.end);
    if (s.silent) out += " (no sound device, playing silently)";
    return out;
}

Result statusResult(const std::string& prefix) {
    const Status s = Player::instance().status();
    Result r = Result::success(prefix.empty() ? describe(s) : prefix + "; " + describe(s));
    statusFields(r, s);
    return r;
}

ActionFactory simple(std::function<Result(const Params&)> fn) {
    return [fn](const Params& p) { return std::make_unique<FunctionAction>([fn, p] { return fn(p); }); };
}

}  // namespace

void statusFields(Result& r, const Status& s) {
    r.with("playing", s.playing)
        .with("paused", s.paused)
        .with("position", s.position)
        .with("start", s.start)
        .with("end", s.end)
        .with("length", s.length)
        .with("loop", s.loop)
        .with("volume_db", audio::gainToDb(s.volume))
        .with("source", s.source)
        .with("device", s.device)
        .with("silent", s.silent);
}

std::vector<ParamSpec> playParams() {
    return {{"start", ParamType::Number, "start time in seconds", false, {}, 0.0},
            {"end", ParamType::Number, "stop time in seconds (-1 = the end)", false, {}, -1.0},
            {"loop", ParamType::Bool, "loop start..end until stopped", false, {}, false},
            {"wait", ParamType::Bool, "block until it finishes (max 600 s; ignored when looping)", false, {}, false},
            {"volume", ParamType::Number, "playback volume in dB (0 = as is), kept for later plays", false}};
}

Result startPlayback(std::shared_ptr<const audio::Buffer> buffer, const std::string& source, const Params& p) {
    Player& player = Player::instance();
    if (p.count("volume")) player.setVolume(audio::dbToGain(std::clamp(numberParam(p, "volume", 0.0), -60.0, 12.0)));
    PlayRequest rq;
    rq.buffer = std::move(buffer);
    rq.start = numberParam(p, "start", 0.0);
    rq.end = numberParam(p, "end", -1.0);
    rq.loop = boolParam(p, "loop", false);
    rq.source = source;
    std::string error;
    if (!player.play(rq, error)) return Result::failure(error);
    if (boolParam(p, "wait", false) && !rq.loop) {
        const bool done = player.wait(kMaxWait);
        return statusResult(done ? "played " + source : "still playing after " + secs(kMaxWait));
    }
    return statusResult("");
}

void registerTransportCommands(Session& session, const std::string& tool, Renderer render) {
    auto& reg = session.registry();
    const std::string t = tool + ".";
    if (render) {
        reg.add({t + "refresh", "Hear edits while playing",
                 "Swap the latest version of what's playing into playback without stopping, so an edit made while it "
                 "plays (an EQ tweak, a moved note, a new clip) is heard straight away from the same spot. The window "
                 "does this after every edit; scripts call it after theirs.",
                 {}, false, true},
                simple([render](const Params&) {
                    const Status st = Player::instance().status();
                    if (!st.playing && !st.paused) return statusResult("nothing is playing");
                    std::string error;
                    auto buffer = render(st.source, error);
                    if (!buffer) return error.empty() ? statusResult("not a live source") : Result::failure(error);
                    if (!Player::instance().replace(std::move(buffer), error)) return Result::failure(error);
                    return statusResult("refreshed");
                }));
    }

    reg.add({t + "pause", "Pause playback", "Pause what's playing (any tool's playback); resume carries on from the same spot.", {}, false, true},
            simple([](const Params&) {
                Player::instance().pause();
                return statusResult("paused");
            }));
    reg.add({t + "resume", "Resume playback", "Carry on playing after a pause.", {}, false, true}, simple([](const Params&) {
                Player::instance().resume();
                return statusResult("resumed");
            }));
    reg.add({t + "stop", "Stop playback", "Stop whatever is playing.", {}, false, true}, simple([](const Params&) {
                Player::instance().stop();
                return statusResult("stopped");
            }));
    reg.add({t + "seek", "Seek", "Move the playhead to a time (seconds), while playing or paused.",
             {{"seconds", ParamType::Number, "where to jump to", true}}, false, true},
            simple([](const Params& p) {
                Player::instance().seek(numberParam(p, "seconds", 0.0));
                return statusResult("moved the playhead");
            }));
    reg.add({t + "playhead", "Playback status",
             "Where playback is: playing/paused, position, the range, loop, volume and the output device.", {}, false, true},
            simple([](const Params&) { return statusResult(""); }));
    reg.add({t + "volume", "Playback volume", "Set the playback volume in dB (0 = unchanged, -6 = half as loud). Doesn't change the audio.",
             {{"db", ParamType::Number, "volume in dB, -60..12", true}}, false, true},
            simple([](const Params& p) {
                Player::instance().setVolume(audio::dbToGain(std::clamp(numberParam(p, "db", 0.0), -60.0, 12.0)));
                return statusResult("volume set");
            }));
    reg.add({t + "loop", "Loop playback", "Turn looping of the playing range on or off.",
             {{"on", ParamType::Bool, "true to loop", false, {}, true}}, false, true},
            simple([](const Params& p) {
                Player::instance().setLoop(boolParam(p, "on", true));
                return statusResult(boolParam(p, "on", true) ? "looping" : "not looping");
            }));
    reg.add({t + "wait", "Wait for playback", "Block until playback finishes (or timeout seconds, max 600). Useful in batch scripts.",
             {{"timeout", ParamType::Number, "give up after this many seconds", false, {}, 600.0}}, false, true},
            simple([](const Params& p) {
                const double timeout = std::clamp(numberParam(p, "timeout", kMaxWait), 0.0, kMaxWait);
                const bool done = Player::instance().wait(timeout);
                return statusResult(done ? "playback finished" : "still playing");
            }));
    reg.add({t + "devices", "Output devices", "List the sound output devices playback can use.", {}, false, true},
            simple([](const Params&) {
                const auto list = Player::instance().devices();
                std::string text;
                for (const auto& d : list) text += (text.empty() ? "" : "\n") + d;
                Result r = Result::success(text);
                r.with("count", static_cast<std::int64_t>(list.size()));
                for (std::size_t i = 0; i < list.size(); ++i) r.with("device_" + std::to_string(i), list[i]);
                return r;
            }));
    reg.add({t + "output_device", "Pick output device", "Play through a named device from " + t + "devices (\"default\" for the system's).",
             {{"name", ParamType::String, "device name", true}}, false, true},
            simple([](const Params& p) {
                std::string error;
                if (!Player::instance().selectDevice(stringParam(p, "name"), error)) return Result::failure(error);
                return Result::success("playback will use " + stringParam(p, "name"));
            }));
}

}  // namespace wetwaresuite::playback
