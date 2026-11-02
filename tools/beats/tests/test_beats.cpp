#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>

#include <nlohmann/json.hpp>

#include "wetwaresuite/audio/analysis.hpp"
#include "wetwaresuite/audio/document.hpp"
#include "wetwaresuite/audio/dsp.hpp"
#include "wetwaresuite/audio/io.hpp"
#include "wetwaresuite/beats/beats.hpp"
#include "wetwaresuite/core/process.hpp"
#include "wetwaresuite/mcp/server.hpp"
#include "wetwaresuite/raster/codec.hpp"
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

std::string S(const char* s) { return s; }

double rmsOf(const audio::Buffer& b, double from, double to) {
    const auto r = audio::rangeOf(b, from, to);
    double sum = 0;
    for (std::size_t i = r.begin; i < r.end; ++i) sum += b.channels[0][i] * b.channels[0][i];
    return r.size() ? std::sqrt(sum / static_cast<double>(r.size())) : 0;
}

}  // namespace

int main() {
    const fs::path dir = fs::temp_directory_path() / "wetwaresuite_test_beats";
    fs::remove_all(dir);
    fs::create_directories(dir);

    // ---- model helpers ----
    using namespace beats;
    CHECK(parseNote("C4") == 60 && parseNote("A4") == 69 && parseNote("C#4") == 61 && parseNote("Db4") == 61 && parseNote("Bb2") == 46);
    CHECK(parseNote("B3") == 59 && parseNote("c-1") == 0 && parseNote("64") == 64 && parseNote("H2") == -1 && parseNote("200") == -1);
    CHECK(noteName(61) == "C#4" && noteName(36) == "C2");
    CHECK(near(noteFrequency(69), 440, 1e-9));
    {
        std::vector<Note> n;
        std::string err;
        CHECK(parseNotes("C4 0 1 100, E4 1 0.5; 67:2:1", n, err) && n.size() == 3 && n[1].pitch == 64 && near(n[1].length, 0.5, 1e-9) && n[2].velocity == 100);
        CHECK(!parseNotes("C4 0", n, err) && !parseNotes("Q4 0 1", n, err) && !parseNotes("C4 0 1 300", n, err));
    }
    CHECK((chordNotes("Am", 3) == std::vector<int>{57, 60, 64}));
    CHECK((chordNotes("Cmaj7", 4) == std::vector<int>{60, 64, 67, 71}));
    CHECK(chordNotes("C/E", 4).front() == 52 && chordNotes("Xyz").empty());
    {
        std::string clean, err;
        CHECK(cleanSteps("x... x... | X.o.", clean, err) && clean == "x...x...X.o.");
        CHECK(!cleanSteps("x..y", clean, err));
    }
    {
        int root = 0;
        bool minor = false;
        CHECK(parseKey("A minor", root, minor) && root == 9 && minor);
        CHECK(parseKey("F#", root, minor) && root == 6 && !minor);
        CHECK(parseKey("Ebm", root, minor) && root == 3 && minor && !parseKey("H dorian", root, minor));
    }
    {
        // synth note: the attack/release shape and the filter work
        SynthSettings st;
        preset("pluck", st);
        audio::Buffer b = synthNote(st, 44100, 220, 0.5, 1.0);
        CHECK(near(b.seconds(), 0.5 + st.release, 0.01));
        CHECK(rmsOf(b, 0.0, 0.05) > rmsOf(b, 0.4, 0.5) * 3);  // plucks decay
        for (const auto& v : drumVoices()) CHECK(!drumHit(v, 22050).empty() && drumMidi(v) > 0 && drumVoiceForMidi(drumMidi(v)) == v);
    }

    Session s;
    beats::registerCommands(s);
    for (const auto& info : s.registry().list()) CHECK(info.id.rfind("beats.", 0) == 0);
    CHECK(s.registry().list().size() >= 24);
    CHECK(s.registry().find("beats.info")->readOnly && s.registry().find("beats.new")->readOnly && !s.registry().find("beats.render")->readOnly);
    CHECK(s.registry().find("beats.pattern_steps")->undoable && !s.registry().find("beats.save")->undoable);

    // ---- build a song by hand ----
    Result made = s.run("beats.new", {{"bpm", 120.0}, {"name", S("demo")}});
    CHECK(made.ok && str(made, "song") == "song1");
    const std::string g = "song1";
    CHECK(!s.run("beats.new", {{"time_signature", S("4/5")}}).ok);
    Result dr = s.run("beats.track_add", {{"song", g}, {"kind", S("drum")}});
    CHECK(dr.ok && str(dr, "track") == "0" && str(dr, "name") == "Drums");
    Result bs = s.run("beats.track_add", {{"song", g}, {"kind", S("synth")}, {"preset", S("bass")}, {"pan", -0.2}});
    CHECK(bs.ok && str(bs, "name") == "Bass");
    CHECK(!s.run("beats.track_add", {{"song", g}, {"preset", S("kazoo")}}).ok);
    CHECK(s.run("beats.pattern_add", {{"song", g}, {"name", S("beat")}, {"steps", S("kick: x...x...x...x...; snare: ....x.......x...; hat: ..x.")}}).ok);
    CHECK(!s.run("beats.pattern_add", {{"song", g}, {"name", S("beat")}}).ok);  // duplicate
    CHECK(!s.run("beats.pattern_add", {{"song", g}, {"steps", S("kick x...")}}).ok);
    Result bl = s.run("beats.pattern_add", {{"song", g}, {"name", S("bassline")}, {"notes", S("C2 0 0.5, C2 1 0.5, Eb2 2 0.5, G1 3 0.5, C2 4 2")}});
    CHECK(bl.ok && numField(bl, "bars") == 2 && numField(bl, "notes") == 5);
    CHECK(s.run("beats.arrange_place", {{"song", g}, {"track", S("Drums")}, {"pattern", S("beat")}, {"bar", std::int64_t{0}}, {"repeat", std::int64_t{4}}}).ok);
    Result pl = s.run("beats.arrange_place", {{"song", g}, {"track", S("1")}, {"pattern", S("bassline")}, {"bar", std::int64_t{0}}, {"repeat", std::int64_t{2}}});
    CHECK(pl.ok && numField(pl, "bars") == 4);
    CHECK(!s.run("beats.arrange_place", {{"song", g}, {"track", S("7")}, {"pattern", S("beat")}}).ok);
    CHECK(!s.run("beats.arrange_place", {{"song", g}, {"track", S("0")}, {"pattern", S("nope")}}).ok);
    {
        auto doc = s.workspace().getAs<SongDoc>(g);
        CHECK(doc && doc->song.bars() == 4 && near(doc->song.seconds(), 8.0, 1e-9));
        const auto ev = trackEvents(doc->song, 0);
        int kicks = 0, hats = 0;
        for (const auto& e : ev) kicks += e.voice == "kick", hats += e.voice == "hat";
        CHECK(kicks == 16 && hats == 16);  // the short hat lane repeats
    }
    // steps edits and undo
    CHECK(s.run("beats.pattern_steps", {{"song", g}, {"pattern", S("beat")}, {"lane", S("clap")}, {"steps", S("....x.......x...")}}).ok);
    Result info = s.run("beats.info", {{"song", g}});
    CHECK(info.ok && info.message.find("clap: ....x.......x...") != std::string::npos);
    CHECK(s.history().undo());
    CHECK(s.run("beats.info", {{"song", g}}).message.find("clap:") == std::string::npos);
    CHECK(!s.run("beats.pattern_steps", {{"song", g}, {"pattern", S("beat")}, {"lane", S("kick")}, {"steps", S("x..z")}}).ok);

    // swing pushes odd steps late
    CHECK(s.run("beats.set", {{"song", g}, {"swing", 0.5}}).ok);
    {
        auto doc = s.workspace().getAs<SongDoc>(g);
        const auto ev = trackEvents(doc->song, 0);
        bool swung = false;
        for (const auto& e : ev) swung = swung || (e.voice == "hat" && near(e.beat, 0.5, 1e-9));  // hat at step 2 is even: not moved
        CHECK(swung);
    }
    CHECK(s.history().undo());

    // ---- render ----
    Result rr = s.run("beats.render", {{"song", g}, {"path", (dir / "demo.wav").string()}, {"preview", true}});
    CHECK(rr.ok && rr.attachments.size() == 1 && rr.attachments[0].mime == "image/png");
    CHECK(near(numField(rr, "seconds"), 8.0, 0.8) && numField(rr, "peak_db") <= -0.9 && numField(rr, "peak_db") > -20);
    {
        audio::Buffer b = audio::load(dir / "demo.wav");
        CHECK(b.channelCount() == 2 && b.rate == 44100);
        // first kick at 0 s is loud; the gap between beat 0's kick tail and... check sound exists at each beat
        CHECK(rmsOf(b, 0.0, 0.1) > 0.05 && rmsOf(b, 2.0, 2.1) > 0.05);
    }
    // mute / solo
    CHECK(s.run("beats.track_set", {{"song", g}, {"track", S("Drums")}, {"mute", true}}).ok);
    {
        auto doc = s.workspace().getAs<SongDoc>(g);
        audio::Buffer b = render(doc->song);
        CHECK(rmsOf(b, 0.0, 0.1) > 0.01);  // bass still plays at 0
        // bass rests at beat 0.5..1 (0.25 s..0.5 s): drums muted, so quiet in that gap after the release
        CHECK(rmsOf(b, 0.4, 0.49) < rmsOf(b, 0.05, 0.2));
    }
    CHECK(s.run("beats.track_set", {{"song", g}, {"track", S("Drums")}, {"mute", false}, {"solo", true}}).ok);
    {
        auto doc = s.workspace().getAs<SongDoc>(g);
        RenderOptions o;
        o.bars = 1;
        audio::Buffer solo = render(doc->song, o);
        o.onlyTrack = 0;
        audio::Buffer only = render(doc->song, o);
        CHECK(near(rmsOf(solo, 0, 2), rmsOf(only, 0, 2), 1e-4));
    }
    CHECK(s.run("beats.track_set", {{"song", g}, {"track", S("Drums")}, {"solo", false}}).ok);
    // effects
    CHECK(s.run("beats.effect", {{"song", g}, {"track", S("Bass")}, {"type", S("distortion")}, {"amount", 0.5}}).ok);
    CHECK(s.run("beats.effect", {{"song", g}, {"track", S("Bass")}, {"type", S("delay")}, {"amount", 0.3}, {"time", 0.75}}).ok);
    CHECK(s.run("beats.effect", {{"song", g}, {"track", S("Drums")}, {"type", S("reverb")}, {"amount", 0.2}}).ok);
    CHECK(s.run("beats.effect", {{"song", g}, {"track", S("Drums")}, {"type", S("compress")}, {"amount", 0.4}}).ok);
    CHECK(!s.run("beats.effect", {{"song", g}, {"track", S("Drums")}, {"type", S("reverb")}, {"amount", 2.0}}).ok);
    CHECK(s.run("beats.info", {{"song", g}}).message.find("reverb 0.2") != std::string::npos);
    // bounce into an audio document
    Result bo = s.run("beats.bounce", {{"song", g}, {"bars", std::int64_t{2}}});
    CHECK(bo.ok && str(bo, "doc") == "aud1" && s.workspace().getAs<audio::AudioDoc>("aud1"));
    CHECK(near(numField(bo, "seconds"), 4.0, 3.1) && numField(bo, "seconds") >= 4.0);
    CHECK(!s.run("beats.bounce", {{"song", g}, {"start_bar", std::int64_t{50}}}).ok);

    // ---- sampler and sample slots ----
    {
        audio::Buffer tone = audio::tone(1, 1.0, 44100, 261.63, 0.5);
        audio::save(tone, dir / "c4.wav");
        Result sm = s.run("beats.track_add", {{"song", g}, {"kind", S("sampler")}, {"sample", (dir / "c4.wav").string()}, {"root", S("C4")}});
        CHECK(sm.ok && str(sm, "track") == "2");
        CHECK(!s.run("beats.track_add", {{"song", g}, {"kind", S("sampler")}, {"sample", (dir / "none.wav").string()}}).ok);
        CHECK(s.run("beats.pattern_add", {{"song", g}, {"name", S("tune")}, {"notes", S("C4 0 1, C5 1 1")}, {"track", S("Sampler")}, {"bar", std::int64_t{0}}}).ok);
        auto doc = s.workspace().getAs<SongDoc>(g);
        RenderOptions o;
        o.onlyTrack = 2;
        o.bars = 1;
        audio::Buffer b = render(doc->song, o);
        // zero crossings: an octave up has twice as many
        auto crossings = [&](double a, double z) {
            const auto r = audio::rangeOf(b, a, z);
            int n = 0;
            for (std::size_t i = r.begin + 1; i < r.end; ++i) n += (b.channels[0][i - 1] < 0) != (b.channels[0][i] < 0);
            return n;
        };
        const int low = crossings(0.1, 0.4), high = crossings(0.6, 0.9);
        CHECK(low > 100 && std::abs(high - 2 * low) < low / 10);
        CHECK(s.run("beats.track_set", {{"song", g}, {"track", S("Drums")}, {"slot", S("perc")}, {"sample", (dir / "c4.wav").string()}}).ok);
        CHECK(!s.run("beats.track_set", {{"song", g}, {"track", S("Drums")}, {"sample", (dir / "c4.wav").string()}}).ok);
        CHECK(!s.run("beats.track_set", {{"song", g}, {"track", S("Bass")}, {"sample", (dir / "c4.wav").string()}}).ok);
        CHECK(s.run("beats.pattern_steps", {{"song", g}, {"pattern", S("beat")}, {"lane", S("perc")}, {"steps", S("x...............")}}).ok);
        bool sawPerc = false;
        for (const auto& e : trackEvents(doc->song, 0)) sawPerc = sawPerc || e.voice == "perc";
        CHECK(sawPerc);
    }

    // ---- save / open ----
    {
        auto renamed = [](std::string t, const std::string& from, const std::string& to) {  // undo the rename in a saved file
            for (auto at = t.find(from); at != std::string::npos; at = t.find(from, at + to.size())) t.replace(at, from.size(), to);
            return t;
        };
        // songs saved before the rename ("achclwss-beats") still load
        auto a = s.workspace().getAs<SongDoc>("song1");
        const std::string fresh = beats::toJson(a->song);
        CHECK(fresh.find("\"wetwaresuite-beats\"") != std::string::npos);
        CHECK(beats::fromJson(renamed(fresh, "wetwaresuite-beats", "achclwss-beats")).tracks.size() == a->song.tracks.size());
    }
    CHECK(s.run("beats.save", {{"song", g}, {"path", (dir / "demo.beats.json").string()}}).ok);
    Result op = s.run("beats.open", {{"path", (dir / "demo.beats.json").string()}, {"preview", true}});
    CHECK(op.ok && str(op, "song") == "song2" && numField(op, "tracks") == 3 && numField(op, "bars") == 4 && op.attachments.size() == 1);
    {
        auto a = s.workspace().getAs<SongDoc>("song1");
        auto b = s.workspace().getAs<SongDoc>("song2");
        CHECK(toJson(a->song, dir / "demo.beats.json") == toJson(b->song, dir / "demo.beats.json"));
        CHECK(b->song.tracks[2].sample.audio && b->song.tracks[0].slots.count("perc") && b->song.tracks[0].slots["perc"].audio);
        CHECK(b->song.tracks[0].fx.reverb > 0.19);
    }
    CHECK(!s.run("beats.open", {{"path", (dir / "nope.beats.json").string()}}).ok);

    // ---- MIDI round trip ----
    CHECK(s.run("beats.midi_export", {{"song", g}, {"path", (dir / "demo.mid").string()}}).ok);
    {
        std::ifstream in(dir / "demo.mid", std::ios::binary);
        std::ostringstream ss;
        ss << in.rdbuf();
        CHECK(ss.str().compare(0, 4, "MThd") == 0);
    }
    Result mi = s.run("beats.midi_import", {{"path", (dir / "demo.mid").string()}});
    CHECK(mi.ok && str(mi, "song") == "song3" && numField(mi, "tracks") == 3 && numField(mi, "bars") == 4);
    {
        auto a = s.workspace().getAs<SongDoc>("song1");
        auto m = s.workspace().getAs<SongDoc>("song3");
        CHECK(near(m->song.bpm, 120, 0.01) && m->song.beatsPerBar == 4);
        CHECK(m->song.tracks[0].kind == TrackKind::Drum && m->song.tracks[0].name == "Drums");
        // same number of notes and the same bass notes
        CHECK(trackEvents(m->song, 1).size() == trackEvents(a->song, 1).size());
        const auto ea = trackEvents(a->song, 1), em = trackEvents(m->song, 1);
        bool same = ea.size() == em.size();
        for (std::size_t i = 0; same && i < ea.size(); ++i) same = ea[i].pitch == em[i].pitch && near(ea[i].beat, em[i].beat, 1e-3) && near(ea[i].length, em[i].length, 1e-3);
        CHECK(same);
        // drums come back as kit voices (the perc slot has no GM number: it goes out as a rim and returns as one)
        int kicks = 0;
        for (const auto& e : trackEvents(m->song, 0)) kicks += e.voice == "kick";
        CHECK(kicks == 16);
    }
    CHECK(!s.run("beats.midi_import", {{"path", (dir / "demo.wav").string()}}).ok);
    // one-shot use straight from files
    CHECK(s.run("beats.render", {{"input", (dir / "demo.mid").string()}, {"path", (dir / "frommidi.wav").string()}, {"bars", std::int64_t{1}}}).ok);
    CHECK(fs::exists(dir / "frommidi.wav"));
    CHECK(s.run("beats.info", {{"input", (dir / "demo.beats.json").string()}}).message.find("Sampler") != std::string::npos);
    CHECK(!s.run("beats.info", {}).ok && !s.run("beats.info", {{"input", (dir / "none.beats.json").string()}}).ok);
    {
        Session one;
        beats::registerCommands(one);
        CHECK(one.run("beats.generate", {{"genre", S("dnb")}, {"save", (dir / "gen.beats.json").string()}}).ok);
        CHECK(one.run("beats.draw", {{"input", (dir / "gen.beats.json").string()}, {"path", (dir / "gen.png").string()}}).ok);
        CHECK(raster::load(dir / "gen.png").width() == 1200);
    }

    // ---- generate ----
    Result gen = s.run("beats.generate", {{"genre", S("house")}, {"key", S("A minor")}, {"seed", std::int64_t{7}}, {"bars", std::int64_t{8}}});
    CHECK(gen.ok && str(gen, "song") == "song4" && numField(gen, "tracks") == 4 && numField(gen, "bars") == 8);
    CHECK(gen.message.find("A minor") != std::string::npos);
    {
        Result pl = s.run("beats.play", {{"song", S("song4")}, {"bars", std::int64_t{1}}, {"end", 0.3}, {"wait", true}});
        CHECK(pl.ok && str(pl, "source") == "song4" && !std::get<bool>(*pl.field("playing")));
        CHECK(s.run("beats.play", {{"song", S("song4")}, {"loop", true}}).ok);
        CHECK(s.run("beats.stop", {}).ok && !std::get<bool>(*s.run("beats.playhead", {}).field("playing")));
        CHECK(!s.run("beats.play", {{"song", S("nope")}}).ok);
    }
    Result gen2 = s.run("beats.generate", {{"genre", S("house")}, {"key", S("A minor")}, {"seed", std::int64_t{7}}, {"bars", std::int64_t{8}}});
    {
        auto a = s.workspace().getAs<SongDoc>("song4");
        auto b = s.workspace().getAs<SongDoc>(str(gen2, "song"));
        CHECK(a && b && toJson(a->song) == toJson(b->song));  // same seed, same song
        // every bass note is in A natural minor
        bool inKey = true;
        for (const auto& e : trackEvents(a->song, 1)) {
            const int pc = e.pitch % 12;
            inKey = inKey && (pc == 9 || pc == 11 || pc == 0 || pc == 2 || pc == 4 || pc == 5 || pc == 7);
        }
        CHECK(inKey);
    }
    CHECK(s.history().undo());  // undo the second generate: the song is closed
    CHECK(!s.workspace().getAs<SongDoc>(str(gen2, "song")));
    CHECK(s.history().redo() && s.workspace().getAs<SongDoc>(str(gen2, "song")));
    for (const auto& genre : genreNames()) {
        Result r = s.run("beats.generate", {{"genre", genre}, {"key", S("Eb major")}, {"seed", std::int64_t{3}}, {"bars", std::int64_t{4}}});
        CHECK(r.ok && numField(r, "bars") == 4);
        auto d = s.workspace().getAs<SongDoc>(str(r, "song"));
        RenderOptions o;
        o.bars = 1;
        audio::Buffer b = render(d->song, o);
        CHECK(audio::measure(b, audio::all(b), false).peakDb > -30);
    }
    CHECK(!s.run("beats.generate", {{"genre", S("polka")}}).ok);
    CHECK(!s.run("beats.generate", {{"key", S("Q minor")}}).ok);
    CHECK(s.run("beats.generate", {{"song", g}, {"genre", S("lofi")}, {"drums", false}, {"melody", false}}).ok);
    CHECK(s.run("beats.info", {{"song", g}}).message.find("Chords") != std::string::npos);

    // ---- chords, transposing, clearing, removing ----
    Result ch = s.run("beats.pattern_add", {{"song", g}, {"name", S("prog")}, {"chords", S("Am F C G")}});
    CHECK(ch.ok && numField(ch, "bars") == 4 && numField(ch, "notes") == 12);
    CHECK(s.run("beats.pattern_notes", {{"song", g}, {"pattern", S("prog")}, {"transpose", std::int64_t{2}}}).ok);
    CHECK(s.run("beats.pattern_notes", {{"song", g}, {"pattern", S("prog")}, {"notes", S("A5 0 4")}, {"mode", S("replace")}}).ok);
    CHECK(s.run("beats.info", {{"song", g}}).message.find("1 notes: A5 0 4 100") != std::string::npos);
    CHECK(s.run("beats.pattern_set", {{"song", g}, {"pattern", S("bassline")}, {"name", S("bass2")}}).ok);
    CHECK(s.run("beats.info", {{"song", g}}).message.find("bass2") != std::string::npos);
    CHECK(s.run("beats.pattern_clear", {{"song", g}, {"pattern", S("beat")}, {"lane", S("perc")}}).ok);
    CHECK(s.run("beats.arrange_remove", {{"song", g}, {"track", S("Drums")}, {"bar", std::int64_t{1}}}).ok);
    CHECK(!s.run("beats.arrange_remove", {{"song", g}, {"track", S("Drums")}, {"bar", std::int64_t{1}}}).ok);
    CHECK(s.run("beats.track_remove", {{"song", g}, {"track", S("Drums")}}).ok);
    {
        auto doc = s.workspace().getAs<SongDoc>(g);
        for (const auto& p : doc->song.arrangement) CHECK(p.track >= 0 && p.track < static_cast<int>(doc->song.tracks.size()));
    }
    CHECK(s.run("beats.pattern_remove", {{"song", g}, {"pattern", S("bass2")}}).ok);
    Result pv = s.run("beats.preview", {{"song", g}, {"view", S("pattern")}, {"pattern", S("beat")}, {"width", std::int64_t{600}}});
    CHECK(pv.ok && pv.attachments.size() == 1 && raster::decode(pv.attachments[0].bytes).width() == 600);
    CHECK(s.run("beats.preview", {{"song", g}, {"view", S("waveform")}, {"bars", std::int64_t{1}}}).ok);
    CHECK(s.run("beats.presets", {}).ok);
    CHECK(s.run("beats.close", {{"song", S("song2")}}).ok && !s.run("beats.info", {{"song", S("song2")}}).ok);

    // ---- mp3 / ogg through ffmpeg ----
    if (findProgram("ffmpeg")) {
        CHECK(s.run("beats.render", {{"song", S("song1")}, {"path", (dir / "demo.mp3").string()}, {"bars", std::int64_t{1}}}).ok);
        CHECK(fs::exists(dir / "demo.mp3") && audio::load(dir / "demo.mp3").seconds() > 1.5);
        CHECK(s.run("beats.render", {{"song", S("song1")}, {"path", (dir / "demo.ogg").string()}, {"bars", std::int64_t{1}}}).ok);
        CHECK(fs::exists(dir / "demo.ogg"));
    } else {
        CHECK(!s.run("beats.render", {{"song", S("song1")}, {"path", (dir / "demo.mp3").string()}}).ok);
    }

    // ---- MCP ----
    {
        Session ms;
        beats::registerCommands(ms);
        mcp::Server server(ms, {"wetwaresuite", "test", "beats"});
        json list = json::parse(*server.handle(R"({"jsonrpc":"2.0","id":1,"method":"tools/list"})"))["result"]["tools"];
        bool sawGenerate = false;
        for (const auto& t : list) {
            if (t["name"] == "beats_info") CHECK(t["annotations"]["readOnlyHint"] == true);
            if (t["name"] == "beats_render") CHECK(t["annotations"]["readOnlyHint"] == false);
            if (t["name"] == "beats_generate") {
                sawGenerate = true;
                CHECK(t["inputSchema"]["properties"]["genre"]["enum"].size() == genreNames().size());
            }
        }
        CHECK(sawGenerate && list.size() >= 24);
        json gj = call(server, 2, "beats_generate", {{"genre", "trap"}, {"key", "F minor"}, {"seed", 2}, {"preview", true}});
        CHECK(gj["isError"] == false && gj["structuredContent"]["song"] == "song1" && gj["content"].size() == 2 && gj["content"][1]["type"] == "image");
        json st = call(server, 3, "beats_pattern_steps", {{"song", "song1"}, {"pattern", "trap beat"}, {"lane", "clap"}, {"steps", "....x.......x..."}, {"preview", true}});
        CHECK(st["isError"] == false && st["content"].size() == 2);
        CHECK(call(server, 4, "undo", json::object())["isError"] == false);
        json bad = call(server, 5, "beats_pattern_notes", {{"song", "song1"}, {"pattern", "melody"}, {"notes", "Z9 0 1"}});
        CHECK(bad["isError"] == true);
        json rj = call(server, 6, "beats_render", {{"song", "song1"}, {"path", (dir / "mcp.wav").string()}, {"bars", 2}});
        CHECK(rj["isError"] == false && fs::exists(dir / "mcp.wav"));
    }

    // ---- MIDI input: the virtual keyboard into a pattern, live and step by step ----
    {
        Session m;
        beats::registerCommands(m);
        const std::string song = str(m.run("beats.new", {{"bpm", 120.0}}), "song");
        CHECK(m.run("beats.pattern_add", {{"song", song}, {"name", S("keys")}}).ok);
        CHECK(str(m.run("beats.midi_devices", {}), "device_0") == "keyboard");
        CHECK(!m.run("beats.midi_open", {{"device", S("no such synth")}}).ok);
        CHECK(m.run("beats.midi_open", {{"device", S("keyboard")}}).ok);
        // live at 120 bpm: a beat is 0.5 s. C4 at 1.0 s for 0.5 s, E4 at 1.5 s for 0.24 s (snapped to a half beat)
        auto key = [&](const char* pitch, bool on, double t) { return m.run("beats.midi_key", {{"pitch", S(pitch)}, {"on", on}, {"time", t}}).ok; };
        CHECK(key("C4", true, 1.0) && key("C4", false, 1.5) && key("E4", true, 1.5) && key("E4", false, 1.74));
        Result took = m.run("beats.midi_take", {{"song", song}, {"pattern", S("keys")}, {"since", 0.0}, {"at", 0.0}});
        CHECK(took.ok && numField(took, "count") == 2);
        Result notes = m.run("beats.notes", {{"song", song}, {"pattern", S("keys")}});
        const std::string rows = str(notes, "rows");
        CHECK(rows.find("60\tC4\t2\t1\t") != std::string::npos && rows.find("64\tE4\t3\t0.5\t") != std::string::npos);
        // step entry: a chord (two keys 20 ms apart) then a single note, half a beat each
        CHECK(key("C4", true, 5.0) && key("G4", true, 5.02) && key("C4", false, 5.3) && key("G4", false, 5.3) && key("A4", true, 6.0) && key("A4", false, 9.0));
        took = m.run("beats.midi_take", {{"song", song}, {"pattern", S("keys")}, {"mode", S("step")}, {"at", 4.0}, {"step", 0.5}});
        CHECK(took.ok);
        const std::string rows2 = str(m.run("beats.notes", {{"song", song}, {"pattern", S("keys")}}), "rows");
        CHECK(rows2.find("60\tC4\t4\t0.5") != std::string::npos && rows2.find("67\tG4\t4\t0.5") != std::string::npos &&
              rows2.find("69\tA4\t4.5\t0.5") != std::string::npos);
        CHECK(!m.run("beats.midi_take", {{"song", song}, {"pattern", S("keys")}}).ok);  // nothing new played
        CHECK(m.run("beats.midi_read", {}).ok);
        CHECK(m.run("beats.midi_close", {}).ok);
        CHECK(!m.run("beats.midi_key", {{"pitch", S("C4")}}).ok);  // closed
    }

    fs::remove_all(dir);
    return check::summary("beats");
}
