// The audio tool's effect, restoration, rack, automation, crossfade, marker
// and export commands, driven the way the CLI and MCP drive them.
#include <algorithm>
#include <cmath>
#include <filesystem>

#include <nlohmann/json.hpp>

#include "wetwaresuite/audio/commands.hpp"
#include "wetwaresuite/audio/document.hpp"
#include "wetwaresuite/audio/io.hpp"
#include "wetwaresuite/audio/repair.hpp"
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

Params P(std::initializer_list<std::pair<const std::string, Value>> v) { return Params(v); }
Value S(const char* s) { return std::string(s); }

audio::Buffer& audioOf(Session& s, const char* id) { return s.workspace().getAs<audio::AudioDoc>(id)->audio; }

double level(const audio::Buffer& b, double hz) { return audio::toneLevel(b, hz, audio::rangeOf(b, 0.3, b.seconds() - 0.1)); }

}  // namespace

int main() {
    const fs::path dir = fs::temp_directory_path() / "wetwaresuite_test_audio_fx";
    fs::remove_all(dir);
    fs::create_directories(dir);

    Session s;
    audio::registerCommands(s);
    for (const char* id : {"audio.chorus", "audio.flanger", "audio.phaser", "audio.tremolo", "audio.distort", "audio.deess", "audio.expand",
                           "audio.graphic_eq", "audio.filter", "audio.param_eq", "audio.effect", "audio.fx_types", "audio.declick", "audio.dehum",
                           "audio.spectral", "audio.loudness", "audio.fx_add", "audio.fx_set", "audio.fx_remove", "audio.fx_move", "audio.fx_list",
                           "audio.automate", "audio.automation", "audio.crossfade", "audio.marker_add", "audio.marker_remove", "audio.markers",
                           "audio.export_regions", "audio.export_stems"})
        CHECK(s.registry().find(id) != nullptr);
    CHECK(s.registry().find("audio.loudness")->readOnly && s.registry().find("audio.fx_list")->readOnly && s.registry().find("audio.markers")->readOnly);
    CHECK(s.registry().find("audio.fx_add")->undoable && s.registry().find("audio.marker_add")->undoable && !s.registry().find("audio.export_stems")->undoable);
    // effect settings are typed params
    {
        const CommandInfo* ch = s.registry().find("audio.chorus");
        bool rate = false, mix = false;
        for (const auto& p : ch->params) rate = rate || (p.name == "rate" && p.type == ParamType::Number), mix = mix || p.name == "mix";
        CHECK(rate && mix);
        const CommandInfo* di = s.registry().find("audio.distort");
        bool mode = false;
        for (const auto& p : di->params) mode = mode || (p.name == "mode" && p.choices.size() == 5);
        CHECK(mode);
    }

    // ---- effects on a document ----
    CHECK(s.run("audio.new", P({{"kind", S("tone")}, {"seconds", 2.0}, {"frequency", 200.0}, {"level", -6.0}})).ok);  // aud1
    {
        Result r = s.run("audio.distort", P({{"doc", S("aud1")}, {"drive", 24.0}, {"mode", S("hard")}}));
        CHECK(r.ok && r.message.find("distortion: drive 24 dB, mode hard") != std::string::npos);
        CHECK(level(audioOf(s, "aud1"), 600) > -30);
        CHECK(s.history().undo());
        CHECK(level(audioOf(s, "aud1"), 600) < -80);
        CHECK(!s.run("audio.distort", P({{"doc", S("aud1")}, {"mode", S("crunchy")}})).ok);
        CHECK(!s.run("audio.chorus", P({{"doc", S("aud1")}, {"rate", 50.0}})).ok);
        // only part of it
        CHECK(s.run("audio.tremolo", P({{"doc", S("aud1")}, {"rate", 4.0}, {"depth", 1.0}, {"start", 1.0}})).ok);
        const auto& a = audioOf(s, "aud1");
        CHECK(near(audio::toneLevel(a, 200, audio::rangeOf(a, 0.1, 0.9)), -6, 0.2));
        CHECK(s.history().undo());
    }
    CHECK(s.run("audio.new", P({{"kind", S("tone")}, {"seconds", 2.0}, {"frequency", 1000.0}, {"level", -12.0}})).ok);  // aud2
    {
        CHECK(s.run("audio.param_eq", P({{"doc", S("aud2")}, {"bands", S("peaking 1000 6 2, highpass 50")}})).ok);
        CHECK(near(level(audioOf(s, "aud2"), 1000), -6, 0.6));
        CHECK(!s.run("audio.param_eq", P({{"doc", S("aud2")}, {"bands", S("wobble 1000")}})).ok);
        CHECK(s.run("audio.graphic_eq", P({{"doc", S("aud2")}, {"g1k", -6.0}})).ok);
        CHECK(near(level(audioOf(s, "aud2"), 1000), -12, 1.0));
        CHECK(s.run("audio.effect", P({{"doc", S("aud2")}, {"type", S("utility")}, {"values", S("gain=-3")}})).ok);
        CHECK(near(level(audioOf(s, "aud2"), 1000), -15, 1.0));
        CHECK(!s.run("audio.effect", P({{"doc", S("aud2")}, {"type", S("wobble")}})).ok);
        Result types = s.run("audio.fx_types", {});
        CHECK(types.ok && str(types, "rows").find("compressor\tratio\t1\t20\t3\t") != std::string::npos);
        CHECK(s.run("audio.fx_types", P({{"type", S("reverb")}})).message.find("predelay") != std::string::npos);
    }

    // ---- restoration ----
    {
        CHECK(s.run("audio.new", P({{"kind", S("tone")}, {"seconds", 2.0}, {"frequency", 440.0}, {"level", -6.0}})).ok);  // aud3
        auto& a = audioOf(s, "aud3");
        const audio::Buffer clean = a;
        a.channels[0][10000] += 0.7f;
        a.channels[1][10000] += 0.7f;
        a.channels[0][50000] -= 0.6f;
        a.channels[1][50000] -= 0.6f;
        Result r = s.run("audio.declick", P({{"doc", S("aud3")}}));
        CHECK(r.ok && numField(r, "clicks") == 4);  // 2 clicks x 2 channels
        double err = 0;
        for (std::size_t i = 0; i < a.frames(); ++i) err = std::max(err, static_cast<double>(std::fabs(a.channels[0][i] - clean.channels[0][i])));
        CHECK(err < 0.02);

        CHECK(s.run("audio.new", P({{"kind", S("tone")}, {"seconds", 3.0}, {"frequency", 1000.0}, {"level", -12.0}})).ok);  // aud4
        auto& h = audioOf(s, "aud4");
        const audio::Buffer hum = audio::tone(2, 3, h.rate, 50, 0.2);
        audio::mixInto(h, hum, 0);
        Result d = s.run("audio.dehum", P({{"doc", S("aud4")}}));
        CHECK(d.ok && numField(d, "frequency") == 50 && level(h, 50) < -45 && near(level(h, 1000), -12, 0.4));
        CHECK(!s.run("audio.dehum", P({{"doc", S("aud4")}, {"harmonics", std::int64_t{0}}})).ok);

        CHECK(s.run("audio.new", P({{"kind", S("tone")}, {"seconds", 2.0}, {"frequency", 3000.0}, {"level", -12.0}})).ok);  // aud5
        CHECK(s.run("audio.spectral", P({{"doc", S("aud5")}, {"low", 2500.0}, {"high", 3500.0}, {"gain", -40.0}, {"start", 1.0}})).ok);
        const auto& sp = audioOf(s, "aud5");
        CHECK(near(audio::toneLevel(sp, 3000, audio::rangeOf(sp, 0.1, 0.8)), -12, 0.3) && audio::toneLevel(sp, 3000, audio::rangeOf(sp, 1.2, 1.9)) < -45);
        CHECK(!s.run("audio.spectral", P({{"doc", S("aud5")}, {"low", 3000.0}, {"high", 100.0}})).ok);
    }

    // ---- the loudness meter ----
    {
        CHECK(s.run("audio.new", P({{"kind", S("tone")}, {"seconds", 6.0}, {"frequency", 1000.0}, {"level", -20.0}})).ok);  // aud6
        Result m = s.run("audio.loudness", P({{"doc", S("aud6")}}));
        CHECK(m.ok && std::get_if<NumberList>(m.field("short_term")) && std::get<NumberList>(*m.field("short_term")).size() == 6);
        const double lufs = numField(m, "lufs");
        CHECK(near(std::get<NumberList>(*m.field("short_term"))[3], lufs, 0.3));
        Result t = s.run("audio.loudness", P({{"doc", S("aud6")}, {"target", -14.0}}));
        CHECK(t.ok && !std::get<bool>(*t.field("meets_target")) && t.message.find("too quiet") != std::string::npos);
        CHECK(s.run("audio.normalize", P({{"doc", S("aud6")}, {"mode", S("loudness")}, {"target", -14.0}})).ok);
        t = s.run("audio.loudness", P({{"doc", S("aud6")}, {"target", -14.0}}));
        CHECK(t.ok && std::get<bool>(*t.field("meets_target")));
    }

    // ---- markers, regions, export ----
    {
        CHECK(s.run("audio.marker_add", P({{"doc", S("aud6")}, {"name", S("verse")}, {"at", 0.5}, {"end", 2.5}})).ok);
        CHECK(s.run("audio.marker_add", P({{"doc", S("aud6")}, {"name", S("chorus")}, {"at", 3.0}, {"end", 4.0}})).ok);
        Result bad = s.run("audio.marker_add", P({{"doc", S("aud6")}, {"at", 10.0}}));
        CHECK(!bad.ok);
        Result list = s.run("audio.markers", P({{"doc", S("aud6")}}));
        CHECK(list.ok && numField(list, "markers") == 2 && str(list, "rows").find("chorus\t3\t4") != std::string::npos);
        Result ex = s.run("audio.export_regions", P({{"doc", S("aud6")}, {"folder", (dir / "regions").string()}, {"bits", S("24")}, {"rate", std::int64_t{22050}}}));
        CHECK(ex.ok && numField(ex, "written") == 2);
        const audio::Buffer verse = audio::load(dir / "regions" / "01_verse.wav");
        CHECK(verse.rate == 22050 && near(verse.seconds(), 2.0, 0.001) && fs::file_size(dir / "regions" / "02_chorus.wav") == 44 + 22050 * 2 * 3);
        CHECK(numField(s.run("audio.export_regions", P({{"doc", S("aud6")}, {"folder", (dir / "regions").string()}})), "skipped") == 2);
        CHECK(s.run("audio.marker_remove", P({{"doc", S("aud6")}, {"name", S("verse")}})).ok);
        CHECK(numField(s.run("audio.markers", P({{"doc", S("aud6")}})), "markers") == 1);
        CHECK(s.history().undo());
        CHECK(numField(s.run("audio.markers", P({{"doc", S("aud6")}})), "markers") == 2);
        CHECK(!s.run("audio.markers", P({{"doc", S("nope")}})).ok);
        // markers only (no regions): the stretches between them
        CHECK(s.run("audio.marker_remove", P({{"doc", S("aud6")}, {"all", true}})).ok);
        CHECK(s.run("audio.marker_add", P({{"doc", S("aud6")}, {"name", S("a")}, {"at", 0.0}})).ok);
        CHECK(s.run("audio.marker_add", P({{"doc", S("aud6")}, {"name", S("b")}, {"at", 4.0}})).ok);
        CHECK(numField(s.run("audio.export_regions", P({{"doc", S("aud6")}, {"folder", (dir / "cuts").string()}})), "written") == 2);
        CHECK(near(audio::load(dir / "cuts" / "02_b.wav").seconds(), 2.0, 0.001));
        CHECK(s.run("audio.save", P({{"doc", S("aud6")}, {"path", (dir / "low.wav").string()}, {"rate", std::int64_t{8000}}})).ok);
        CHECK(audio::load(dir / "low.wav").rate == 8000 && audioOf(s, "aud6").rate == 44100);
    }

    // ---- a project: racks, automation, crossfade, stems ----
    {
        Result pr = s.run("audio.project_new", P({{"tracks", std::int64_t{2}}}));
        CHECK(pr.ok && str(pr, "project") == "proj1");
        CHECK(s.run("audio.clip_add", P({{"project", S("proj1")}, {"track", std::int64_t{0}}, {"doc", S("aud2")}, {"at", 0.0}})).ok);
        CHECK(s.run("audio.clip_add", P({{"project", S("proj1")}, {"track", std::int64_t{0}}, {"doc", S("aud4")}, {"at", 1.5}})).ok);
        CHECK(s.run("audio.clip_add", P({{"project", S("proj1")}, {"track", std::int64_t{1}}, {"doc", S("aud1")}, {"at", 0.0}})).ok);
        // effects
        Result add = s.run("audio.fx_add", P({{"project", S("proj1")}, {"track", std::int64_t{1}}, {"type", S("utility")}, {"values", S("gain=-6")}}));
        CHECK(add.ok && numField(add, "fx") == 0);
        CHECK(s.run("audio.fx_add", P({{"project", S("proj1")}, {"track", std::int64_t{1}}, {"type", S("eq")}, {"values", S("bands=lowpass 2000")}})).ok);
        CHECK(!s.run("audio.fx_add", P({{"project", S("proj1")}, {"track", std::int64_t{1}}, {"type", S("compressor")}, {"values", S("ratio=99")}})).ok);
        CHECK(!s.run("audio.fx_add", P({{"project", S("proj1")}, {"type", S("compressor")}})).ok);  // no track or master
        CHECK(s.run("audio.fx_add", P({{"project", S("proj1")}, {"master", true}, {"type", S("limiter")}, {"values", S("ceiling=-3")}})).ok);
        Result list = s.run("audio.fx_list", P({{"project", S("proj1")}}));
        CHECK(list.ok && str(list, "rows").find("1\t0\tutility\t0\tgain\t-6") != std::string::npos && str(list, "rows").find("master\t0\tlimiter") != std::string::npos);
        CHECK(s.run("audio.fx_set", P({{"project", S("proj1")}, {"track", std::int64_t{1}}, {"fx", std::int64_t{0}}, {"values", S("gain=-12")}})).ok);
        CHECK(s.run("audio.fx_set", P({{"project", S("proj1")}, {"track", std::int64_t{1}}, {"fx", std::int64_t{1}}, {"bypass", true}})).ok);
        CHECK(!s.run("audio.fx_set", P({{"project", S("proj1")}, {"track", std::int64_t{1}}, {"fx", std::int64_t{5}}, {"bypass", true}})).ok);
        // automation follows effects that move
        CHECK(s.run("audio.automate", P({{"project", S("proj1")}, {"track", std::int64_t{1}}, {"target", S("fx0.gain")}, {"points", NumberList{0, -12, 2, 0}}})).ok);
        CHECK(!s.run("audio.automate", P({{"project", S("proj1")}, {"track", std::int64_t{1}}, {"target", S("fx0.gain")}, {"points", NumberList{0, -100}}})).ok);
        CHECK(!s.run("audio.automate", P({{"project", S("proj1")}, {"track", std::int64_t{1}}, {"target", S("fx9.gain")}, {"points", NumberList{0, 0}}})).ok);
        CHECK(s.run("audio.fx_move", P({{"project", S("proj1")}, {"track", std::int64_t{1}}, {"fx", std::int64_t{0}}, {"to", std::int64_t{1}}})).ok);
        Result au = s.run("audio.automation", P({{"project", S("proj1")}, {"track", std::int64_t{1}}}));
        CHECK(au.ok && str(au, "rows").find("1\tfx1.gain\t0\t-12") != std::string::npos);
        CHECK(s.run("audio.automate", P({{"project", S("proj1")}, {"track", std::int64_t{1}}, {"target", S("volume")}, {"points", NumberList{0, 0, 1, -6}}})).ok);
        CHECK(s.run("audio.automate", P({{"project", S("proj1")}, {"track", std::int64_t{1}}, {"target", S("volume")}, {"points", NumberList{2, -20}}, {"mode", S("add")}})).ok);
        au = s.run("audio.automation", P({{"project", S("proj1")}, {"track", std::int64_t{1}}}));
        CHECK(str(au, "rows").find("1\tvolume\t2\t-20") != std::string::npos && str(au, "rows").find("1\tvolume\t1\t-6") != std::string::npos);
        CHECK(s.run("audio.automate", P({{"project", S("proj1")}, {"track", std::int64_t{1}}, {"target", S("volume")}, {"mode", S("clear")}})).ok);
        CHECK(s.run("audio.fx_remove", P({{"project", S("proj1")}, {"track", std::int64_t{1}}, {"fx", std::int64_t{1}}})).ok);
        au = s.run("audio.automation", P({{"project", S("proj1")}, {"track", std::int64_t{1}}}));
        CHECK(str(au, "rows").empty());  // the lane went with its effect
        CHECK(s.history().undo());
        CHECK(str(s.run("audio.automation", P({{"project", S("proj1")}, {"track", std::int64_t{1}}})), "rows").find("fx1.gain") != std::string::npos);

        // crossfade: clip 0 (2 s at 0) into clip 1 (at 1.5): 0.5 s overlap
        Result xf = s.run("audio.crossfade", P({{"project", S("proj1")}, {"track", std::int64_t{0}}, {"clip", std::int64_t{0}}}));
        CHECK(xf.ok && near(numField(xf, "seconds"), 0.5, 1e-6));
        auto proj = s.workspace().getAs<audio::ProjectDoc>("proj1");
        CHECK(proj->project.tracks[0].clips[0].fadeOutCurve == audio::FadeCurve::EqualPower && near(proj->project.tracks[0].clips[1].fadeIn, 0.5, 1e-9));
        xf = s.run("audio.crossfade", P({{"project", S("proj1")}, {"track", std::int64_t{0}}, {"clip", std::int64_t{0}}, {"seconds", 1.0}, {"curve", S("linear")}}));
        CHECK(xf.ok && near(numField(xf, "next_at"), 1.0, 1e-9) && proj->project.tracks[0].clips[1].fadeInCurve == audio::FadeCurve::Linear);
        CHECK(!s.run("audio.crossfade", P({{"project", S("proj1")}, {"track", std::int64_t{0}}, {"clip", std::int64_t{1}}})).ok);
        CHECK(s.run("audio.clip_set", P({{"project", S("proj1")}, {"track", std::int64_t{1}}, {"clip", std::int64_t{0}}, {"fade_in", 0.5}, {"fade_in_curve", S("exponential")}})).ok);
        CHECK(proj->project.tracks[1].clips[0].fadeInCurve == audio::FadeCurve::Exponential);

        // the master limiter holds the mix under -3 dBFS
        Result mix = s.run("audio.mixdown", P({{"project", S("proj1")}}));
        CHECK(mix.ok && numField(mix, "peak_db") <= -3 + 1e-3);
        // stems: one per track, the same length, effects included
        Result st = s.run("audio.export_stems", P({{"project", S("proj1")}, {"folder", (dir / "stems").string()}, {"mix", true}}));
        CHECK(st.ok && numField(st, "written") == 3);
        const audio::Buffer s0 = audio::load(dir / "stems" / "01_Track_1.wav"), s1 = audio::load(dir / "stems" / "02_Track_2.wav");
        CHECK(s0.frames() == s1.frames() && near(s0.seconds(), 4.0, 0.01) && fs::exists(dir / "stems" / "mix.wav"));
        // markers on a project, saved with it
        CHECK(s.run("audio.marker_add", P({{"doc", S("proj1")}, {"name", S("intro")}, {"at", 0.0}, {"end", 1.0}})).ok);
        CHECK(s.run("audio.project_save", P({{"project", S("proj1")}, {"path", (dir / "p.json").string()}})).ok);
        Result op = s.run("audio.project_open", P({{"path", (dir / "p.json").string()}}));
        CHECK(op.ok && op.message.find("master fx 0 limiter") != std::string::npos && op.message.find("marker \"intro\"") != std::string::npos);
        CHECK(op.message.find("automation fx1.gain") != std::string::npos);
        Result ex = s.run("audio.export", P({{"project", S("proj1")}, {"path", (dir / "mix22.wav").string()}, {"rate", std::int64_t{22050}}}));
        CHECK(ex.ok && audio::load(dir / "mix22.wav").rate == 22050);
    }

    // ---- CLAP plugins in a project's racks ----
    {
        Session cs;
        audio::registerCommands(cs);
        const std::string plugin = TEST_CLAP_PLUGIN;
        const std::string folder = fs::path(plugin).parent_path().string();
        Result found = cs.run("audio.plugins", {{"folder", folder}});
        CHECK(found.ok && numField(found, "count") >= 1 && str(found, "rows").find("Test Gain\twetwaresuite\t1.0\teffect") != std::string::npos);
        Result prm = cs.run("audio.plugin_params", {{"plugin", plugin}});
        CHECK(prm.ok && str(prm, "rows").find("p7\tGain\t-24\t24\t0\t0") == 0);
        CHECK(!cs.run("audio.plugin_params", {{"plugin", std::string("no such thing")}}).ok);
        const std::string P = str(cs.run("audio.project_new", {}), "project");
        const std::string tone = str(cs.run("audio.new", {{"kind", std::string("tone")}, {"seconds", 0.5}}), "doc");
        CHECK(cs.run("audio.track_add", {{"project", P}}).ok);
        CHECK(cs.run("audio.clip_add", {{"project", P}, {"track", std::int64_t{0}}, {"doc", tone}, {"at", 0.0}}).ok);
        auto peak = [&] { return numField(cs.run("audio.mixdown", {{"project", P}}), "peak_db"); };
        const double loud = peak();
        Result add = cs.run("audio.plugin_add", {{"project", P}, {"track", std::int64_t{0}}, {"plugin", plugin}, {"values", std::string("Gain=-12")}});
        CHECK(add.ok && numField(add, "fx") == 0);
        CHECK(!cs.run("audio.plugin_add", {{"project", P}, {"track", std::int64_t{0}}, {"plugin", plugin}, {"values", std::string("Volume=1")}}).ok);
        CHECK(near(peak(), loud - 12, 0.3));
        Result rows = cs.run("audio.fx_list", {{"project", P}, {"track", std::int64_t{0}}});
        CHECK(str(rows, "rows").find("0\t0\tplugin: Test Gain\t0\tp7\t-12\t-24\t24\t\t\tGain") == 0);
        CHECK(cs.run("audio.fx_set", {{"project", P}, {"track", std::int64_t{0}}, {"fx", std::int64_t{0}}, {"values", std::string("p7=0")}}).ok);
        CHECK(near(peak(), loud, 0.3));
        CHECK(cs.run("audio.automate", {{"project", P}, {"track", std::int64_t{0}}, {"target", std::string("fx0.p7")}, {"points", NumberList{0, -24, 0.5, -24}}}).ok);
        CHECK(!cs.run("audio.automate", {{"project", P}, {"track", std::int64_t{0}}, {"target", std::string("fx0.Gain")}, {"points", NumberList{0, 0}}}).ok);
        CHECK(peak() < loud - 20);
        CHECK(cs.run("audio.project_save", {{"project", P}, {"path", (dir / "clap.json").string()}}).ok);
        Result op = cs.run("audio.project_open", {{"path", (dir / "clap.json").string()}});
        CHECK(op.ok && op.message.find("Gain") != std::string::npos);
    }

    // ---- over MCP ----
    {
        mcp::Server server(s, "wetwaresuite-test", "1");
        json req = {{"jsonrpc", "2.0"}, {"id", 1}, {"method", "tools/call"},
                    {"params", {{"name", "audio_fx_add"}, {"arguments", {{"project", "proj1"}, {"track", 0}, {"type", "reverb"}, {"values", "room=0.5"}}}}}};
        json res = json::parse(*server.handle(req.dump()))["result"];
        CHECK(!res.value("isError", false) && res["structuredContent"]["fx"] == 0);
        req["params"] = {{"name", "audio_chorus"}, {"arguments", {{"doc", "aud2"}, {"rate", 1.5}, {"mix", 0.4}}}};
        res = json::parse(*server.handle(req.dump()))["result"];
        CHECK(!res.value("isError", false));
    }

    fs::remove_all(dir);
    return check::summary("audio_fx");
}
