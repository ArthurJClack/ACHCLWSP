// Runs on the silent device (WETWARESUITE_AUDIO=null), which still moves the
// playhead in real time, so it passes on machines with no sound card.
#include <cmath>
#include <chrono>
#include <cstdlib>
#include <thread>

#include "wetwaresuite/playback/commands.hpp"
#include "check.hpp"

using namespace wetwaresuite;
using namespace wetwaresuite::playback;

namespace {

std::shared_ptr<const audio::Buffer> tone(double seconds, int channels = 2) {
    auto b = std::make_shared<audio::Buffer>(channels, static_cast<std::size_t>(seconds * 48000), 48000);
    for (auto& c : b->channels)
        for (std::size_t i = 0; i < c.size(); ++i) c[i] = 0.1f * static_cast<float>((i % 100) / 50.0 - 1.0);
    return b;
}

void sleepMs(int ms) { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); }

// the silent device moves in chunks, so poll instead of guessing a sleep
template <typename F>
bool within(int ms, F f) {
    for (int t = 0; t < ms; t += 5) {
        if (f()) return true;
        sleepMs(5);
    }
    return f();
}

}  // namespace

int main() {
#ifdef _WIN32
    _putenv_s("WETWARESUITE_AUDIO", "null");
#else
    setenv("WETWARESUITE_AUDIO", "null", 1);
#endif
    Player& p = Player::instance();
    std::string error;

    // nothing to play
    CHECK(!p.play({}, error) && !error.empty());
    CHECK(p.status().source.empty() && !p.status().playing);

    // plays, moves, finishes
    error.clear();
    CHECK(p.play({tone(0.3), 0, -1, false, "beep"}, error));
    CHECK(error.empty());
    CHECK(within(1000, [&] { return p.status().position > 0.0; }));
    Status s = p.status();
    CHECK(s.playing && s.silent && s.source == "beep" && s.position < 0.3);
    CHECK(p.wait(5) && !p.status().playing);
    CHECK(p.status().position >= 0.29);

    // range, pause holds the playhead, resume carries on
    CHECK(p.play({tone(2.0), 0.5, 1.5, false, "range"}, error));
    s = p.status();
    CHECK(s.start > 0.49 && s.start < 0.51 && s.end > 1.49 && s.end < 1.51 && s.position >= 0.5);
    p.pause();
    const double held = p.status().position;
    sleepMs(100);
    CHECK(p.status().paused && !p.status().playing && p.status().position == held);
    p.resume();
    CHECK(within(1000, [&] { return p.status().position > held; }));
    CHECK(p.status().playing);
    p.seek(1.4);
    CHECK(p.status().position >= 1.39);
    CHECK(p.wait(5));
    CHECK(p.status().position <= 1.51);

    // looping never finishes until stopped; wait refuses to block on it
    CHECK(p.play({tone(0.1, 1), 0, -1, true, "loop"}, error));
    CHECK(within(2000, [&] { return p.status().position > 0.05; }));
    sleepMs(150);  // past the end at least once
    CHECK(p.status().playing && p.status().loop);
    CHECK(!p.wait(0.05));
    p.stop();
    CHECK(!p.status().playing);

    // volume is kept, mono plays on a stereo device
    p.setVolume(0.5);
    CHECK(p.status().volume > 0.49 && p.status().volume < 0.51);
    CHECK(p.devices().front() == "default");
    CHECK(p.selectDevice("default", error));
    CHECK(!p.selectDevice("no such device", error));

    // the commands
    Session session;
    registerTransportCommands(session, "audio");
    for (const char* id : {"audio.pause", "audio.resume", "audio.stop", "audio.seek", "audio.playhead", "audio.volume", "audio.loop",
                           "audio.wait", "audio.devices", "audio.output_device"})
        CHECK(session.registry().find(id) != nullptr);
    Result r = startPlayback(tone(0.2), "cmd", {{"wait", true}, {"volume", -6.0}});
    CHECK(r.ok && r.field("playing") && std::get<bool>(*r.field("playing")) == false);
    CHECK(std::get<std::string>(*r.field("source")) == "cmd");
    CHECK(std::get<double>(*r.field("volume_db")) > -6.1 && std::get<double>(*r.field("volume_db")) < -5.9);
    r = startPlayback(tone(1.0), "cmd2", {{"loop", true}});
    CHECK(r.ok && std::get<bool>(*r.field("loop")));
    r = session.run("audio.stop", {});
    CHECK(r.ok && !std::get<bool>(*r.field("playing")));
    r = session.run("audio.playhead", {});
    CHECK(r.ok && r.message.find("stopped cmd2") != std::string::npos);
    CHECK(!session.run("audio.seek", {}).ok);  // seconds is required
    CHECK(startPlayback(tone(1.0), "x", {{"start", 5.0}}).ok == false);  // past the end

    // live replace: a longer version swaps in at the same time without stopping
    CHECK(p.play({tone(1.0), 0, -1, false, "live"}, error));
    CHECK(within(2000, [&] { return p.status().position > 0.2; }));
    const double before = p.status().position;
    CHECK(p.replace(tone(3.0), error));
    s = p.status();
    CHECK(s.playing && s.length > 2.9 && s.end > 2.9 && s.position >= before && s.position < before + 0.3);
    p.stop();
    CHECK(!p.replace(tone(1.0), error));  // nothing playing
    // refresh through the transport command
    Session live;
    auto version = std::make_shared<double>(1.0);
    registerTransportCommands(live, "beats", [version](const std::string& src, std::string&) -> std::shared_ptr<const audio::Buffer> {
        return src == "song1" ? tone(*version) : nullptr;
    });
    CHECK(live.registry().find("beats.refresh") != nullptr);
    CHECK(startPlayback(tone(1.0), "song1", {}).ok);
    *version = 4.0;
    r = live.run("beats.refresh", {});
    CHECK(r.ok && std::get<double>(*r.field("length")) > 3.9);
    p.stop();

    // recording: the test tone, in real time, then handed over as a buffer
    Recorder& rec = Recorder::instance();
    CHECK(rec.start(22050, 1, "tone", 60, error));
    CHECK(!rec.start(22050, 1, "tone", 60, error));  // one take at a time
    CHECK(within(3000, [&] { return rec.status().seconds > 0.3; }));
    const RecordStatus rs = rec.status();
    CHECK(rs.recording && rs.peakDb > -7 && rs.peakDb < -5 && rs.device.find("tone") != std::string::npos);
    audio::Buffer take = rec.stop();
    CHECK(take.rate == 22050 && take.channelCount() == 1 && take.seconds() > 0.3 && !rec.status().recording);
    float peak = 0;
    for (float v : take.channels[0]) peak = std::max(peak, std::fabs(v));
    CHECK(peak > 0.45f && peak < 0.55f);
    CHECK(rec.stop().empty());
    // a cap ends the take by itself; the silent device records silence
    CHECK(rec.start(8000, 2, "default", 0.2, error));
    CHECK(within(3000, [&] { return !rec.status().recording; }));
    take = rec.stop();
    CHECK(take.channelCount() == 2 && std::fabs(take.seconds() - 0.2) < 0.01);

    finishBeforeExit();  // returns at once: nothing playing
    return check::summary("playback");
}
