#include <chrono>
#include <thread>
#include <cmath>
#include <filesystem>

#include <nlohmann/json.hpp>

#include "wetwaresuite/audio/commands.hpp"
#include "wetwaresuite/audio/document.hpp"
#include "wetwaresuite/audio/dsp.hpp"
#include "wetwaresuite/audio/io.hpp"
#include "wetwaresuite/core/cli.hpp"
#include "wetwaresuite/mcp/server.hpp"
#include "check.hpp"

using namespace wetwaresuite;
using json = nlohmann::ordered_json;
namespace fs = std::filesystem;

namespace {

std::string str(const Result& r, const char* key) {
    const Value* v = r.field(key);
    return v ? toText(*v) : "";
}

double numField(const Result& r, const char* key) {
    const Value* v = r.field(key);
    if (!v) return -9999;
    if (auto d = std::get_if<double>(v)) return *d;
    if (auto i = std::get_if<std::int64_t>(v)) return static_cast<double>(*i);
    return -9999;
}

bool near(double a, double b, double eps) { return std::fabs(a - b) <= eps; }

json call(mcp::Server& server, int id, const std::string& tool, const json& args) {
    json req = {{"jsonrpc", "2.0"}, {"id", id}, {"method", "tools/call"}, {"params", {{"name", tool}, {"arguments", args}}}};
    return json::parse(*server.handle(req.dump()))["result"];
}

}  // namespace

int main() {
    const fs::path dir = fs::temp_directory_path() / "wetwaresuite_test_audio_tool";
    fs::remove_all(dir);
    fs::create_directories(dir);

    Session s;
    audio::registerCommands(s);
    for (const auto& info : s.registry().list()) CHECK(info.id.rfind("audio.", 0) == 0);
    CHECK(s.registry().list().size() >= 45);
    CHECK(s.registry().find("audio.measure")->readOnly && !s.registry().find("audio.save")->readOnly);
    CHECK(s.registry().find("audio.denoise")->undoable && !s.registry().find("audio.batch")->undoable);

    // ---- generate, measure, save, open ----
    Result made = s.run("audio.new", {{"kind", std::string("tone")}, {"seconds", 2.0}, {"frequency", 440.0}, {"level", -6.0}});
    CHECK(made.ok && str(made, "doc") == "aud1" && near(numField(made, "seconds"), 2.0, 1e-9));
    Result m = s.run("audio.measure", {{"doc", std::string("aud1")}});
    CHECK(m.ok && near(numField(m, "peak_db"), -6, 0.05) && numField(m, "clipped") == 0);
    CHECK(!s.run("audio.measure", {}).ok);
    CHECK(s.run("audio.save", {{"doc", std::string("aud1")}, {"path", (dir / "tone.wav").string()}, {"bits", std::string("24")}}).ok);
    CHECK(fs::file_size(dir / "tone.wav") == 44 + 2 * 44100 * 2 * 3);

    // ---- live playback (silent device in tests) ----
    {
        Result pl = s.run("audio.play", {{"doc", std::string("aud1")}, {"start", 1.5}});
        CHECK(pl.ok && str(pl, "source") == "aud1" && near(numField(pl, "start"), 1.5, 1e-3) && near(numField(pl, "length"), 2.0, 1e-6));
        Result w = s.run("audio.wait", {{"timeout", 5.0}});
        CHECK(w.ok && !std::get<bool>(*w.field("playing")) && numField(w, "position") >= 1.99);
        Result f = s.run("audio.play", {{"path", (dir / "tone.wav").string()}, {"end", 0.2}, {"wait", true}});
        CHECK(f.ok && str(f, "source") == "tone.wav" && f.message.find("played tone.wav") != std::string::npos);
        CHECK(s.run("audio.play", {{"doc", std::string("aud1")}, {"loop", true}}).ok);
        CHECK(std::get<bool>(*s.run("audio.playhead", {}).field("loop")));
        CHECK(s.run("audio.stop", {}).ok);
        CHECK(!s.run("audio.play", {}).ok);
        CHECK(!s.run("audio.play", {{"doc", std::string("aud9")}}).ok);
        CHECK(!s.run("audio.play", {{"path", (dir / "missing.wav").string()}}).ok);
    }
    CHECK(!s.run("audio.save", {{"doc", std::string("aud1")}, {"path", (dir / "tone.xyz").string()}}).ok);
    Result opened = s.run("audio.open", {{"path", (dir / "tone.wav").string()}});
    CHECK(opened.ok && str(opened, "doc") == "aud2" && numField(opened, "channels") == 2);
    CHECK(!s.run("audio.open", {{"path", (dir / "nope.wav").string()}}).ok);
    CHECK(near(numField(s.run("audio.measure", {{"input", (dir / "tone.wav").string()}}), "peak_db"), -6, 0.05));
    CHECK(s.run("audio.info", {{"doc", std::string("aud2")}}).ok);

    // ---- edits and undo ----
    const std::string a = "aud2";
    CHECK(s.run("audio.gain", {{"doc", a}, {"db", -6.0}}).ok);
    CHECK(near(numField(s.run("audio.measure", {{"doc", a}}), "peak_db"), -12, 0.05));
    CHECK(s.history().undo());
    CHECK(near(numField(s.run("audio.measure", {{"doc", a}}), "peak_db"), -6, 0.05));
    CHECK(s.run("audio.gain", {{"doc", a}, {"db", -20.0}, {"start", 1.0}, {"channels", NumberList{1}}}).ok);
    {
        auto doc = s.workspace().getAs<audio::AudioDoc>(a);
        CHECK(std::fabs(doc->audio.channels[1][70000]) < 0.06 && std::fabs(doc->audio.channels[0][70000]) > 0.0);
    }
    CHECK(!s.run("audio.gain", {{"doc", a}, {"db", 3.0}, {"channels", NumberList{5}}}).ok);
    CHECK(!s.run("audio.gain", {{"doc", a}, {"db", 3.0}, {"start", 9.0}}).ok);
    CHECK(!s.run("audio.gain", {{"doc", std::string("aud9")}, {"db", 3.0}}).ok);
    CHECK(s.history().undo());

    Result norm = s.run("audio.normalize", {{"doc", a}, {"mode", std::string("loudness")}, {"target", -16.0}});
    CHECK(norm.ok && near(numField(norm, "lufs"), -16, 0.3));
    CHECK(s.run("audio.normalize", {{"doc", a}, {"target", -1.0}}).ok);
    CHECK(near(numField(s.run("audio.measure", {{"doc", a}}), "peak_db"), -1, 0.05));
    CHECK(s.run("audio.fade", {{"doc", a}, {"in", 0.5}, {"out", 0.5}, {"curve", std::string("s_curve")}}).ok);
    CHECK(!s.run("audio.fade", {{"doc", a}}).ok);
    CHECK(s.run("audio.eq", {{"doc", a}, {"type", std::string("highpass")}, {"frequency", 80.0}}).ok);
    CHECK(!s.run("audio.eq", {{"doc", a}, {"type", std::string("highpass")}, {"frequency", 30000.0}}).ok);
    CHECK(s.run("audio.compress", {{"doc", a}, {"threshold", -20.0}, {"ratio", 3.0}}).ok);
    CHECK(s.run("audio.limit", {{"doc", a}, {"ceiling", -3.0}}).ok);
    CHECK(numField(s.run("audio.measure", {{"doc", a}, {"true_peak", false}}), "peak_db") <= -2.99);
    CHECK(s.run("audio.gate", {{"doc", a}, {"threshold", -50.0}}).ok);
    CHECK(s.run("audio.remove_dc", {{"doc", a}}).ok);
    CHECK(s.run("audio.reverb", {{"doc", a}, {"wet", 0.3}, {"tail", 1.0}}).ok);
    CHECK(near(numField(s.run("audio.info", {{"doc", a}}), "seconds"), 3.0, 1e-3));
    CHECK(s.run("audio.echo", {{"doc", a}, {"delay", 0.2}, {"ping_pong", true}}).ok);
    CHECK(s.run("audio.reverse", {{"doc", a}, {"start", 0.0}, {"end", 1.0}}).ok);
    CHECK(s.run("audio.pan", {{"doc", a}, {"position", -0.5}}).ok);
    const auto steps = s.history().labels().size();
    CHECK(steps >= 12);
    while (s.history().canUndo()) s.history().undo();
    CHECK(near(numField(s.run("audio.measure", {{"doc", a}}), "peak_db"), -6, 0.05));
    CHECK(near(numField(s.run("audio.info", {{"doc", a}}), "seconds"), 2.0, 1e-9));
    CHECK(s.history().redo());

    // cut / copy / paste / silence / trim
    const std::string b = str(s.run("audio.new", {{"kind", std::string("tone")}, {"seconds", 3.0}, {"rate", std::int64_t{8000}}, {"channels", std::int64_t{1}}}), "doc");
    CHECK(s.run("audio.cut", {{"doc", b}, {"start", 1.0}, {"end", 2.0}}).ok);
    CHECK(near(numField(s.run("audio.info", {{"doc", b}}), "seconds"), 2.0, 1e-9));
    CHECK(s.run("audio.paste", {{"doc", b}, {"at", 0.0}}).ok);
    CHECK(near(numField(s.run("audio.info", {{"doc", b}}), "seconds"), 3.0, 1e-9));
    CHECK(s.run("audio.copy", {{"doc", b}, {"start", 0.0}, {"end", 0.5}}).ok);
    CHECK(s.run("audio.paste", {{"doc", b}, {"mode", std::string("mix")}, {"at", 2.9}}).ok);
    CHECK(near(numField(s.run("audio.info", {{"doc", b}}), "seconds"), 3.4, 1e-3));
    CHECK(!s.run("audio.paste", {{"doc", b}, {"at", 99.0}}).ok);
    CHECK(s.run("audio.insert_silence", {{"doc", b}, {"seconds", 1.0}, {"at", 0.0}}).ok);
    CHECK(s.run("audio.insert_silence", {{"doc", b}, {"seconds", 0.5}}).ok);
    Result trimmed = s.run("audio.trim", {{"doc", b}, {"silence", true}, {"pad", 0.0}});
    CHECK(trimmed.ok && near(numField(trimmed, "seconds"), 3.4, 0.02));
    CHECK(s.run("audio.trim", {{"doc", b}, {"start", 0.5}, {"end", 1.5}}).ok);
    CHECK(near(numField(s.run("audio.info", {{"doc", b}}), "seconds"), 1.0, 1e-3));
    CHECK(!s.run("audio.trim", {{"doc", b}}).ok);

    // channels, rate, tempo, pitch, speed
    CHECK(s.run("audio.channels", {{"doc", b}, {"mode", std::string("stereo")}}).ok && numField(s.run("audio.info", {{"doc", b}}), "channels") == 2);
    CHECK(s.run("audio.channels", {{"doc", b}, {"mode", std::string("swap")}}).ok);
    CHECK(s.run("audio.channels", {{"doc", b}, {"mode", std::string("mono")}}).ok && numField(s.run("audio.info", {{"doc", b}}), "channels") == 1);
    CHECK(!s.run("audio.channels", {{"doc", b}, {"mode", std::string("right")}}).ok);
    CHECK(s.run("audio.resample", {{"doc", b}, {"rate", std::int64_t{16000}}}).ok && numField(s.run("audio.info", {{"doc", b}}), "rate") == 16000);
    CHECK(s.run("audio.stretch", {{"doc", b}, {"tempo", 0.5}}).ok);
    CHECK(near(numField(s.run("audio.info", {{"doc", b}}), "seconds"), 2.0, 1e-3));
    CHECK(s.run("audio.stretch", {{"doc", b}, {"from_bpm", 120.0}, {"to_bpm", 240.0}}).ok);
    CHECK(near(numField(s.run("audio.info", {{"doc", b}}), "seconds"), 1.0, 1e-3));
    CHECK(s.run("audio.stretch", {{"doc", b}, {"seconds", 1.5}, {"start", 0.0}, {"end", 0.5}}).ok);
    CHECK(near(numField(s.run("audio.info", {{"doc", b}}), "seconds"), 2.0, 1e-3));
    CHECK(!s.run("audio.stretch", {{"doc", b}}).ok);
    CHECK(s.run("audio.pitch", {{"doc", b}, {"semitones", 3.0}}).ok);
    CHECK(near(numField(s.run("audio.info", {{"doc", b}}), "seconds"), 2.0, 1e-3));
    CHECK(s.run("audio.speed", {{"doc", b}, {"factor", 2.0}}).ok);
    CHECK(near(numField(s.run("audio.info", {{"doc", b}}), "seconds"), 1.0, 1e-3));

    // mix in and noise reduction
    const std::string hiss = str(s.run("audio.new", {{"kind", std::string("noise")}, {"seconds", 3.0}, {"level", -30.0}, {"channels", std::int64_t{1}}}), "doc");
    const std::string voice = str(s.run("audio.new", {{"kind", std::string("tone")}, {"seconds", 1.0}, {"frequency", 300.0}, {"channels", std::int64_t{1}}}), "doc");
    CHECK(s.run("audio.mix", {{"doc", hiss}, {"source", voice}, {"at", 1.5}}).ok);
    CHECK(!s.run("audio.mix", {{"doc", hiss}}).ok);
    const double noisy = numField(s.run("audio.measure", {{"doc", hiss}, {"start", 0.1}, {"end", 1.2}}), "rms_db");
    CHECK(s.run("audio.noise_profile", {{"doc", hiss}, {"start", 0.0}, {"end", 1.2}}).ok);
    CHECK(str(s.run("audio.info", {{"doc", hiss}}), "noise_profile") == "true");
    CHECK(s.run("audio.denoise", {{"doc", hiss}, {"reduction", 20.0}}).ok);
    const double clean = numField(s.run("audio.measure", {{"doc", hiss}, {"start", 0.1}, {"end", 1.2}}), "rms_db");
    CHECK(clean < noisy - 12);
    CHECK(near(numField(s.run("audio.measure", {{"doc", hiss}, {"start", 1.7}, {"end", 2.3}}), "peak_db"), -6, 1.0));
    CHECK(s.history().undo());
    CHECK(s.run("audio.denoise", {{"doc", hiss}, {"noise_start", 0.0}, {"noise_end", 1.0}}).ok);
    CHECK(s.run("audio.denoise", {{"doc", voice}}).ok);  // estimated profile

    // previews
    Result wave = s.run("audio.preview", {{"doc", hiss}, {"width", std::int64_t{300}}});
    CHECK(wave.ok && wave.attachments.size() == 1 && wave.attachments[0].mime == "image/png");
    Result spec = s.run("audio.render", {{"doc", hiss}, {"spectrogram", true}, {"output", (dir / "spec.png").string()}});
    CHECK(spec.ok && fs::exists(dir / "spec.png") && numField(spec, "height") == 400);
    CHECK(s.run("audio.preview", {{"input", (dir / "tone.wav").string()}, {"spectrogram", true}, {"log_frequency", false}}).attachments.size() == 1);
    CHECK(!s.run("audio.preview", {{"doc", hiss}, {"spectrogram", true}, {"channel", std::int64_t{3}}}).ok);
    CHECK(s.registry().find("audio.preview")->readOnly && !s.registry().find("audio.render")->readOnly);
    CHECK(s.run("audio.gain", {{"doc", hiss}, {"db", 1.0}, {"preview", true}}).attachments.size() == 1);

    // silence detection and splitting
    {
        auto parts = audio::silence(1, 5.0, 8000);
        audio::overwrite(parts, 4000, audio::tone(1, 1.0, 8000, 220, 0.5));
        audio::overwrite(parts, 24000, audio::tone(1, 1.0, 8000, 330, 0.5));
        audio::save(parts, dir / "takes.wav");
        Result q = s.run("audio.detect_silence", {{"input", (dir / "takes.wav").string()}});
        CHECK(q.ok && numField(q, "count") == 3);
        Result sp = s.run("audio.split_on_silence", {{"input", (dir / "takes.wav").string()}, {"output_dir", (dir / "split").string()}});
        CHECK(sp.ok && numField(sp, "count") == 2 && fs::exists(dir / "split" / "takes_001.wav") && fs::exists(dir / "split" / "takes_002.wav"));
    }

    // batch
    {
        fs::create_directories(dir / "samples");
        audio::save(audio::tone(2, 0.5, 44100, 440, 0.1), dir / "samples" / "quiet.wav");
        audio::save(audio::tone(2, 0.5, 44100, 440, 0.9), dir / "samples" / "loud.wav");
        Result bt = s.run("audio.batch", {{"input_dir", (dir / "samples").string()}, {"output_dir", (dir / "normalized").string()},
                                          {"normalize", std::string("peak")}, {"target", -3.0}});
        CHECK(bt.ok && numField(bt, "processed") == 2);
        CHECK(near(numField(s.run("audio.measure", {{"input", (dir / "normalized" / "quiet.wav").string()}}), "peak_db"), -3, 0.05));
        CHECK(near(numField(s.run("audio.measure", {{"input", (dir / "normalized" / "loud.wav").string()}}), "peak_db"), -3, 0.05));
        Result again = s.run("audio.batch", {{"input_dir", (dir / "samples").string()}, {"output_dir", (dir / "normalized").string()}});
        CHECK(again.ok && numField(again, "skipped") == 2);
        Result tp = s.run("audio.batch", {{"input_dir", (dir / "samples").string()}, {"output_dir", (dir / "faster").string()}, {"tempo", 2.0},
                                          {"pitch", -2.0}, {"highpass", 60.0}});
        CHECK(tp.ok && near(numField(s.run("audio.measure", {{"input", (dir / "faster" / "loud.wav").string()}}), "seconds"), 0.25, 0.01));
        CHECK(!s.run("audio.batch", {{"input_dir", (dir / "samples").string()}, {"output_dir", (dir / "samples").string()}}).ok);
        if (audio::ffmpegAvailable()) {
            Result mp3 = s.run("audio.batch", {{"input_dir", (dir / "samples").string()}, {"output_dir", (dir / "mp3").string()}, {"to", std::string("mp3")},
                                               {"normalize", std::string("loudness")}, {"channels", std::int64_t{1}}});
            CHECK(mp3.ok && fs::exists(dir / "mp3" / "loud.mp3"));
            CHECK(s.run("audio.save", {{"doc", hiss}, {"path", (dir / "hiss.flac").string()}}).ok);
            CHECK(s.run("audio.open", {{"path", (dir / "hiss.flac").string()}}).ok);
        } else {
            CHECK(!s.run("audio.save", {{"doc", hiss}, {"path", (dir / "hiss.mp3").string()}}).ok);
        }
    }

    // ---- multitrack ----
    {
        Result pj = s.run("audio.project_new", {{"rate", std::int64_t{22050}}});
        CHECK(pj.ok && str(pj, "project") == "proj1");
        const std::string P = "proj1";
        const std::string music = str(s.run("audio.new", {{"kind", std::string("tone")}, {"seconds", 8.0}, {"frequency", 110.0}}), "doc");
        const std::string talk = str(s.run("audio.new", {{"kind", std::string("tone")}, {"seconds", 3.0}, {"frequency", 330.0}, {"channels", std::int64_t{1}}}), "doc");
        CHECK(numField(s.run("audio.track_add", {{"project", P}, {"name", std::string("music")}}), "track") == 0);
        CHECK(numField(s.run("audio.track_add", {{"project", P}, {"name", std::string("voice")}}), "track") == 1);
        CHECK(s.run("audio.clip_add", {{"project", P}, {"track", std::int64_t{0}}, {"doc", music}, {"fade_in", 1.0}, {"fade_out", 2.0}}).ok);
        Result c1 = s.run("audio.clip_add", {{"project", P}, {"track", std::int64_t{1}}, {"doc", talk}, {"at", 3.0}});
        CHECK(c1.ok && near(numField(c1, "clip_end"), 6.0, 1e-3));
        CHECK(!s.run("audio.clip_add", {{"project", P}, {"track", std::int64_t{5}}, {"doc", talk}}).ok);
        CHECK(s.run("audio.clip_add", {{"project", P}, {"track", std::int64_t{1}}, {"doc", talk}, {"start", 0.0}, {"end", 0.5}}).ok);  // appended at 6 s
        CHECK(s.run("audio.track_set", {{"project", P}, {"track", std::int64_t{0}}, {"duck_under", std::int64_t{1}}, {"duck_db", -15.0}}).ok);
        CHECK(!s.run("audio.track_set", {{"project", P}, {"track", std::int64_t{0}}, {"duck_under", std::int64_t{0}}}).ok);
        CHECK(s.run("audio.clip_set", {{"project", P}, {"track", std::int64_t{1}}, {"clip", std::int64_t{1}}, {"at", 7.0}, {"gain", -3.0}}).ok);
        Result info = s.run("audio.project_info", {{"project", P}, {"preview", true}});
        CHECK(info.ok && info.attachments.size() == 1 && near(numField(info, "seconds"), 8.0, 1e-3));
        CHECK(s.run("audio.render", {{"project", P}, {"output", (dir / "tracks.png").string()}}).ok && fs::exists(dir / "tracks.png"));
        Result mix = s.run("audio.mixdown", {{"project", P}});
        CHECK(mix.ok && near(numField(mix, "seconds"), 8.0, 1e-3));
        {
            auto md = s.workspace().getAs<audio::AudioDoc>(str(mix, "doc"));
            CHECK(md && md->audio.rate == 22050 && md->audio.channelCount() == 2);
        }
        CHECK(s.run("audio.export", {{"project", P}, {"path", (dir / "episode.wav").string()}, {"normalize", std::string("loudness")}, {"target", -16.0}}).ok);
        CHECK(near(numField(s.run("audio.measure", {{"input", (dir / "episode.wav").string()}}), "lufs"), -16, 0.5));
        // trim and split, non-destructive, and the same edits the window's timeline makes
        {
            const auto T = [](std::int64_t v) { return v; };
            CHECK(s.run("audio.clip_trim", {{"project", P}, {"track", T(1)}, {"clip", T(0)}, {"start", 4.0}}).ok);  // 3..6 -> 4..6
            auto pd = s.workspace().getAs<audio::ProjectDoc>(P);
            const audio::Clip& c = pd->project.tracks[1].clips[0];
            CHECK(near(c.at, 4.0, 1e-9) && near(c.offset, 1.0, 1e-9) && near(c.end(), 6.0, 1e-9));
            CHECK(s.run("audio.clip_trim", {{"project", P}, {"track", T(1)}, {"clip", T(0)}, {"start", 1.0}}).ok);  // can only reveal back to 3
            CHECK(near(pd->project.tracks[1].clips[0].at, 3.0, 1e-9) && near(pd->project.tracks[1].clips[0].offset, 0.0, 1e-9));
            CHECK(s.run("audio.clip_trim", {{"project", P}, {"track", T(1)}, {"clip", T(0)}, {"length", 1.5}}).ok);
            CHECK(near(pd->project.tracks[1].clips[0].end(), 4.5, 1e-9));
            CHECK(s.run("audio.clip_trim", {{"project", P}, {"track", T(1)}, {"clip", T(0)}, {"length", 99.0}}).ok);  // back to the full 3 s
            CHECK(near(pd->project.tracks[1].clips[0].end(), 6.0, 1e-9));
            CHECK(!s.run("audio.clip_trim", {{"project", P}, {"track", T(1)}, {"clip", T(0)}}).ok);
            Result sp = s.run("audio.clip_split", {{"project", P}, {"track", T(1)}, {"clip", T(0)}, {"at", 5.0}});
            CHECK(sp.ok && numField(sp, "clip") == 1 && pd->project.tracks[1].clips.size() == 3);
            CHECK(near(pd->project.tracks[1].clips[0].end(), 5.0, 1e-9) && near(pd->project.tracks[1].clips[1].at, 5.0, 1e-9) &&
                  near(pd->project.tracks[1].clips[1].offset, 2.0, 1e-9) && near(pd->project.tracks[1].clips[1].end(), 6.0, 1e-9));
            CHECK(!s.run("audio.clip_split", {{"project", P}, {"track", T(1)}, {"clip", T(0)}, {"at", 9.0}}).ok);
            // the mix is unchanged by a split
            CHECK(near(numField(s.run("audio.mixdown", {{"project", P}}), "seconds"), 8.0, 1e-3));
            // timeline layout and commands
            TimelineLayout layout;
            CHECK(pd->timeline(layout) && layout.docParam == "project" && layout.tracks.size() == 2 && layout.tracks[1].clips.size() == 3);
            CHECK(near(layout.tracks[1].clips[1].headroom, 2.0, 1e-9) && near(layout.tracks[1].clips[1].tailroom, 0.0, 1e-9));
            const std::string mv = pd->timelineCommand({TimelineEditKind::Move, "2", "1", "0", 6.5});
            CHECK(mv == "audio.clip_set track=1 clip=2 at=6.5000 to_track=0");
            cli::Batch b(s);
            CHECK(b.runLine(mv + " project=" + P).ok && pd->project.tracks[0].clips.size() == 2);
            CHECK(b.runLine(pd->timelineCommand({TimelineEditKind::Delete, "1", "1"}) + " project=" + P).ok && pd->project.tracks[1].clips.size() == 1);
            for (int i = 0; i < 7; ++i) CHECK(s.history().undo());  // back to before the trims
            CHECK(pd->project.tracks[1].clips.size() == 2 && near(pd->project.tracks[1].clips[0].at, 3.0, 1e-9) && pd->project.tracks[1].clips[0].length < 0);
        }
        CHECK(s.run("audio.track_set", {{"project", P}, {"track", std::int64_t{1}}, {"mute", true}}).ok);
        CHECK(s.history().undo());
        CHECK(s.run("audio.project_save", {{"project", P}, {"path", (dir / "show.json").string()}}).ok);
        Result re = s.run("audio.project_open", {{"path", (dir / "show.json").string()}});
        CHECK(re.ok && str(re, "project") == "proj2" && numField(re, "tracks") == 2);
        CHECK(s.run("audio.clip_remove", {{"project", P}, {"track", std::int64_t{1}}, {"clip", std::int64_t{1}}}).ok);
        CHECK(s.run("audio.track_remove", {{"project", P}, {"track", std::int64_t{1}}}).ok);
        CHECK(str(s.run("audio.project_info", {{"project", P}}), "tracks") == "1");
        CHECK(s.history().undo() && s.history().undo());
        CHECK(str(s.run("audio.project_info", {{"project", P}}), "tracks") == "2");
        CHECK(s.run("audio.close", {{"doc", P}}).ok && !s.run("audio.project_info", {{"project", P}}).ok);
        CHECK(!s.run("audio.mixdown", {{"project", std::string("proj2")}, {"normalize", std::string("bogus")}}).ok);
    }
    CHECK(s.run("audio.close", {{"doc", std::string("aud1")}}).ok && !s.run("audio.info", {{"doc", std::string("aud1")}}).ok);

    // ---- MCP ----
    {
        Session ms;
        audio::registerCommands(ms);
        mcp::Server server(ms, {"wetwaresuite", "test", "audio"});
        json list = json::parse(*server.handle(R"({"jsonrpc":"2.0","id":1,"method":"tools/list"})"))["result"]["tools"];
        bool sawDenoise = false;
        for (const auto& t : list) {
            if (t["name"] == "audio_measure") CHECK(t["annotations"]["readOnlyHint"] == true);
            if (t["name"] == "audio_export") CHECK(t["annotations"]["readOnlyHint"] == false);
            if (t["name"] == "audio_eq") CHECK(t["inputSchema"]["properties"]["type"]["enum"].size() == 7);
            sawDenoise = sawDenoise || t["name"] == "audio_denoise";
        }
        CHECK(sawDenoise && list.size() >= 45);
        json o = call(server, 2, "audio_open", {{"path", (dir / "tone.wav").string()}});
        CHECK(o["isError"] == false && o["structuredContent"]["doc"] == "aud1");
        json pv = call(server, 3, "audio_preview", {{"doc", "aud1"}, {"spectrogram", true}, {"width", 256}, {"height", 128}});
        CHECK(pv["isError"] == false && pv["content"].size() == 2 && pv["content"][1]["type"] == "image");
        json g = call(server, 4, "audio_normalize", {{"doc", "aud1"}, {"mode", "loudness"}, {"target", -16}, {"preview", true}});
        CHECK(g["isError"] == false && g["content"].size() == 2);
        json ms2 = call(server, 5, "audio_measure", {{"doc", "aud1"}});
        CHECK(ms2["isError"] == false && std::fabs(ms2["structuredContent"]["lufs"].get<double>() + 16) < 0.3);
        json ch = call(server, 6, "audio_gain", {{"doc", "aud1"}, {"db", -3}, {"channels", {0}}, {"start", 0.5}});
        CHECK(ch["isError"] == false);
        CHECK(call(server, 7, "undo", json::object())["isError"] == false);
        json bad = call(server, 8, "audio_eq", {{"doc", "aud1"}, {"type", "wobble"}, {"frequency", 100}});
        CHECK(bad["isError"] == true);
    }

    // ---- recording (test tone; silence with no input) and live playback refresh ----
    {
        Session rs;
        audio::registerCommands(rs);
        Result rec = rs.run("audio.record", {{"seconds", 0.3}, {"device", std::string("tone")}, {"rate", std::int64_t{16000}}});
        CHECK(rec.ok);
        const std::string take = rec.ok ? std::get<std::string>(*rec.field("doc")) : "";
        auto takeDoc = rs.workspace().getAs<audio::AudioDoc>(take);
        CHECK(takeDoc && takeDoc->audio.rate == 16000 && takeDoc->audio.channelCount() == 1 && std::fabs(takeDoc->audio.seconds() - 0.3) < 0.02);
        CHECK(rs.run("audio.record_start", {{"device", std::string("tone")}}).ok);
        CHECK(!rs.run("audio.record_start", {}).ok);  // already going
        for (int k = 0; k < 100 && numField(rs.run("audio.record_status", {}), "seconds") < 0.05; ++k) std::this_thread::sleep_for(std::chrono::milliseconds(10));
        Result stop = rs.run("audio.record_stop", {});
        CHECK(stop.ok && rs.workspace().getAs<audio::AudioDoc>(std::get<std::string>(*stop.field("doc"))));
        CHECK(!rs.run("audio.record_stop", {}).ok);  // nothing recording
        CHECK(rs.run("audio.record_devices", {}).ok);
        // hear an edit while it plays: the gain change is swapped in, the playhead keeps going
        Result tone = rs.run("audio.new", {{"kind", std::string("tone")}, {"seconds", 3.0}});
        const std::string id = std::get<std::string>(*tone.field("doc"));
        CHECK(rs.run("audio.play", {{"doc", id}}).ok);
        CHECK(rs.run("audio.trim", {{"doc", id}, {"start", 0.0}, {"end", 2.0}}).ok);
        Result fresh = rs.run("audio.refresh", {});
        CHECK(fresh.ok && std::get<double>(*fresh.field("length")) < 2.01 && std::get<bool>(*fresh.field("playing")));
        rs.run("audio.stop", {});
        CHECK(rs.run("audio.refresh", {}).message.find("nothing is playing") != std::string::npos);
    }

    // ---- effects buses and sends ----
    {
        Session bs;
        audio::registerCommands(bs);
        const std::string P = std::get<std::string>(*bs.run("audio.project_new", {{"rate", std::int64_t{22050}}}).field("project"));
        const std::string click = std::get<std::string>(*bs.run("audio.new", {{"kind", std::string("tone")}, {"seconds", 0.2}, {"rate", std::int64_t{22050}}}).field("doc"));
        CHECK(bs.run("audio.track_add", {{"project", P}}).ok);
        CHECK(bs.run("audio.clip_add", {{"project", P}, {"track", std::int64_t{0}}, {"doc", click}, {"at", 0.0}}).ok);
        auto mixSeconds = [&] { return numField(bs.run("audio.mixdown", {{"project", P}}), "seconds"); };
        const double dry = mixSeconds();
        Result bus = bs.run("audio.bus_add", {{"project", P}, {"fx", std::string("reverb")}, {"values", std::string("room=0.9 wet=1 dry=0")}});
        CHECK(bus.ok && numField(bus, "bus") == 0);
        CHECK(!bs.run("audio.send", {{"project", P}, {"track", std::int64_t{0}}, {"bus", std::int64_t{3}}}).ok);
        CHECK(bs.run("audio.send", {{"project", P}, {"track", std::int64_t{0}}, {"bus", std::int64_t{0}}, {"level", 0.0}}).ok);
        const double wet = mixSeconds();
        CHECK(wet > dry + 0.3);  // the reverb tail rings past the click
        CHECK(bs.run("audio.fx_add", {{"project", P}, {"bus", std::int64_t{0}}, {"type", std::string("eq")}}).ok);
        Result list = bs.run("audio.fx_list", {{"project", P}});
        CHECK(std::get<std::string>(*list.field("rows")).find("bus0\t1\teq") != std::string::npos);
        CHECK(std::get<std::string>(*bs.run("audio.sends", {{"project", P}}).field("rows")).find("0\treverb\t0\t0\t0\t0") == 0);
        // saved and opened again, the bus and the send come back
        CHECK(bs.run("audio.project_save", {{"project", P}, {"path", (dir / "bus.json").string()}}).ok);
        Result again = bs.run("audio.project_open", {{"path", (dir / "bus.json").string()}});
        CHECK(again.ok);
        auto reopened = bs.workspace().getAs<audio::ProjectDoc>(std::get<std::string>(*again.field("project")));
        CHECK(reopened && reopened->project.buses.size() == 1 && reopened->project.buses[0].fx.size() == 2 &&
              reopened->project.tracks[0].sends.size() == 1);
        CHECK(bs.run("audio.bus_set", {{"project", P}, {"bus", std::int64_t{0}}, {"mute", true}}).ok);
        CHECK(near(mixSeconds(), dry, 0.05));
        CHECK(bs.run("audio.bus_remove", {{"project", P}, {"bus", std::int64_t{0}}}).ok);
        auto pd = bs.workspace().getAs<audio::ProjectDoc>(P);
        CHECK(pd->project.buses.empty() && pd->project.tracks[0].sends.empty());
    }

    fs::remove_all(dir);
    return check::summary("audio");
}
