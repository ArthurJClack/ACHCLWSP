#include "suite.hpp"

#include "wetwaresuite/animate/animate.hpp"
#include "wetwaresuite/audio/commands.hpp"
#include "wetwaresuite/beats/beats.hpp"
#include "wetwaresuite/board/board.hpp"
#include "wetwaresuite/automate/automate.hpp"
#include "wetwaresuite/blueprint/blueprint.hpp"
#include "wetwaresuite/calc/calc.hpp"
#include "wetwaresuite/cad/cad.hpp"
#include "wetwaresuite/capture/capture.hpp"
#include "wetwaresuite/circuit/circuit.hpp"
#include "wetwaresuite/clock/clock.hpp"
#include "wetwaresuite/code/code.hpp"
#include "wetwaresuite/convert/convert.hpp"
#include "wetwaresuite/cut/cut.hpp"
#include "wetwaresuite/explorer/explorer.hpp"
#include "wetwaresuite/flash/flash.hpp"
#include "wetwaresuite/forge/forge.hpp"
#include "wetwaresuite/layout/layout.hpp"
#include "wetwaresuite/levels/levels.hpp"
#include "wetwaresuite/levels/scene.hpp"
#include "wetwaresuite/maps/maps.hpp"
#include "wetwaresuite/monitor/monitor.hpp"
#include "wetwaresuite/library/library.hpp"
#include "wetwaresuite/notes/notes.hpp"
#include "wetwaresuite/office/office.hpp"
#include "wetwaresuite/pdf/commands.hpp"
#include "wetwaresuite/paint/paint.hpp"
#include "wetwaresuite/photo/photo.hpp"
#include "wetwaresuite/model/model.hpp"
#include "wetwaresuite/sculpt/sculpt.hpp"
#include "wetwaresuite/fit/fit.hpp"
#include "wetwaresuite/render/render.hpp"
#include "wetwaresuite/slicer/slicer.hpp"
#include "wetwaresuite/terminal/terminal.hpp"
#include "wetwaresuite/toolkit/detect.hpp"
#include "wetwaresuite/type/type.hpp"
#include "wetwaresuite/vector/vector.hpp"

namespace wetwaresuite::suite {

const std::set<std::string>& toolNames() {
    static const std::set<std::string> names = {
        "toolkit",
        "photo",
        "paint",
        "vector",
        "blueprint",
        "convert",
        "forge",
        "slicer",
        "cad",
        "fit",
        "layout",
        "audio",
        "cut",
        "capture",
        "library",
        "automate",
        "office",
        "pdf",
        "levels",
        "scene",
        "model",
        "sculpt",
        "beats",
        "animate",
        "render",
        "circuit",
        "flash",
        "board",
        "notes",
        "maps",
        "type",
        "calc",
        "code",
        "explorer",
        "terminal",
        "monitor",
        "clock",
    };
    return names;
}

void registerAll(Session& session, const std::set<std::string>& only) {
    std::set<std::string> tools = only;
    if (tools.count("paint")) tools.insert("photo");
    if (tools.count("convert")) tools.insert("vector");
    if (tools.count("render")) tools.insert("forge");
    if (tools.count("forge")) tools.insert("photo");
    if (tools.count("sculpt")) tools.insert("photo");
    if (tools.count("beats")) tools.insert("audio");
    auto want = [&](const char* tool) { return only.empty() || tools.count(tool) > 0; };
    if (want("toolkit")) toolkit::registerCommands(session.registry());
    if (want("photo")) photo::registerCommands(session);
    if (want("paint")) paint::registerCommands(session);
    if (want("vector")) vector::registerCommands(session);
    if (want("blueprint")) blueprint::registerCommands(session);
    if (want("convert")) convert::registerCommands(session);
    if (want("forge")) forge::registerCommands(session);
    if (want("slicer")) slicer::registerCommands(session);
    if (want("cad")) cad::registerCommands(session);
    if (want("fit")) fit::registerCommands(session);
    if (want("layout")) layout::registerCommands(session);
    if (want("audio")) audio::registerCommands(session);
    if (want("cut")) cut::registerCommands(session);
    if (want("capture")) capture::registerCommands(session);
    if (want("library")) library::registerCommands(session);
    if (want("automate")) automate::registerCommands(session, [only](Session& fresh) { registerAll(fresh, only); });
    if (want("office")) office::registerCommands(session);
    if (want("pdf")) pdf::registerCommands(session);
    if (want("levels")) levels::registerCommands(session);
    if (want("scene")) scene::registerCommands(session);
    if (want("model")) model::registerCommands(session);
    if (want("sculpt")) sculpt::registerCommands(session);
    if (want("beats")) beats::registerCommands(session);
    if (want("animate")) animate::registerCommands(session);
    if (want("render")) render::registerCommands(session);
    if (want("circuit")) circuit::registerCommands(session);
    if (want("flash")) flash::registerCommands(session);
    if (want("board")) board::registerCommands(session);
    if (want("notes")) notes::registerCommands(session);
    if (want("maps")) maps::registerCommands(session);
    if (want("type")) typeface::registerCommands(session);
    if (want("calc")) calc::registerCommands(session);
    if (want("code")) code::registerCommands(session);
    if (want("explorer")) explorer::registerCommands(session);
    if (want("terminal")) terminal::registerCommands(session);
    if (want("monitor")) monitor::registerCommands(session);
    if (want("clock")) clock::registerCommands(session);
}

}  // namespace wetwaresuite::suite
