// Hosting a CLAP plugin: scan it, read its parameters, run it in a rack, save and load it.
#include <algorithm>
#include <cmath>
#include <filesystem>

#include "check.hpp"
#include "wetwaresuite/audio/clap_host.hpp"
#include "wetwaresuite/audio/dsp.hpp"
#include "wetwaresuite/audio/project.hpp"

using namespace wetwaresuite;
using namespace wetwaresuite::audio;

int main() {
    const std::string file = TEST_CLAP_PLUGIN;
    std::vector<ClapPluginInfo> list;
    std::string error;
    CHECK(clapPluginsIn(file, list, error));
    CHECK(list.size() == 1 && list[0].id == "org.wetwaresuite.test-gain" && list[0].name == "Test Gain" && list[0].effect());
    CHECK(!clapPluginsIn("/no/such/plugin.clap", list, error) && !error.empty());

    ClapPluginInfo info;
    CHECK(findClapPlugin(file, info, error) && info.id == "org.wetwaresuite.test-gain");
    CHECK(findClapPlugin(info.key(), info, error));
    // found by name once its folder is on the search path
    CHECK(scanClapPlugins({std::filesystem::path(file).parent_path()}).size() >= 1);

    std::vector<ClapParamInfo> params;
    CHECK(clapParams(info.key(), params, error) && params.size() == 1 && params[0].id == 7 && params[0].name == "Gain" && params[0].min == -24);

    Effect e;
    CHECK(makeClapEffect(info, "Gain=-6", e, error));
    CHECK(e.type == "clap" && e.plugin == info.key() && std::fabs(e.params["p7"] + 6) < 1e-9);
    CHECK(!makeClapEffect(info, "Volume=3", e, error) && error.find("Gain") != std::string::npos);
    CHECK(!makeClapEffect(info, "Gain=99", e, error));
    CHECK(makeClapEffect(info, "Gain=-6", e, error));
    CHECK(describeClapEffect(e).find("Gain -6") != std::string::npos);

    // in a rack: -6 dB halves the level, mono and stereo
    for (int ch : {1, 2}) {
        Buffer b = tone(ch, 0.5, 44100, 440, 0.5);
        const float before = b.channels[0][1000];
        applyRack(b, {e});
        CHECK(std::fabs(b.channels[0][1000] - before * 0.501f) < 1e-3f);
        CHECK(b.frames() == tone(ch, 0.5, 44100, 440, 0.5).frames());
    }
    // automation moves the plugin's parameter like a built-in setting
    Buffer b = tone(2, 1.0, 44100, 440, 0.5);
    Automation lane{"fx0.p7", {{0.0, 0.0}, {0.5, 0.0}, {0.51, -24.0}, {1.0, -24.0}}};
    applyRack(b, {e}, {lane});
    float early = 0, late = 0;
    for (std::size_t i = 4410; i < 8820; ++i) early = std::max(early, std::fabs(b.channels[0][i]));
    for (std::size_t i = 30000; i < 40000; ++i) late = std::max(late, std::fabs(b.channels[0][i]));
    CHECK(early > 0.45f && late < 0.04f);

    // saved and loaded with a project
    Project p;
    p.masterFx.push_back(e);
    const auto dir = std::filesystem::temp_directory_path() / "wetwaresuite_test_clap";
    std::filesystem::create_directories(dir);
    saveProject(p, dir / "clap.json");
    Project q = loadProject(dir / "clap.json");
    CHECK(q.masterFx.size() == 1 && q.masterFx[0].plugin == e.plugin && std::fabs(q.masterFx[0].params["p7"] + 6) < 1e-9);
    std::filesystem::remove_all(dir);

    // a missing plugin passes the sound through untouched
    Effect gone = e;
    gone.plugin = "/no/such.clap#x";
    Buffer c = tone(1, 0.1, 44100, 440, 0.5);
    const float v = c.channels[0][100];
    applyRack(c, {gone});
    CHECK(c.channels[0][100] == v);
    return check::summary("audio_clap");
}
