// wetwaresuite-<app>: one app's tools, headless. Built once per app from this
// file; CMake defines WW_APP_ID and WW_TOOL_<NAME> for the tools that app
// carries (see apps/app/CMakeLists.txt), so each binary links only those.
#include <set>
#include <string>

#include "app_main.hpp"
#include "apps.hpp"

#if WW_TOOL_TOOLKIT
#include "wetwaresuite/toolkit/detect.hpp"
#endif
#if WW_TOOL_PHOTO
#include "wetwaresuite/photo/photo.hpp"
#endif
#if WW_TOOL_PAINT
#include "wetwaresuite/paint/paint.hpp"
#endif
#if WW_TOOL_VECTOR
#include "wetwaresuite/vector/vector.hpp"
#endif
#if WW_TOOL_TYPE
#include "wetwaresuite/type/type.hpp"
#endif
#if WW_TOOL_BOARD
#include "wetwaresuite/board/board.hpp"
#endif
#if WW_TOOL_MODEL
#include "wetwaresuite/model/model.hpp"
#endif
#if WW_TOOL_SCULPT
#include "wetwaresuite/sculpt/sculpt.hpp"
#endif
#if WW_TOOL_RENDER
#include "wetwaresuite/render/render.hpp"
#endif
#if WW_TOOL_ANIMATE
#include "wetwaresuite/animate/animate.hpp"
#endif
#if WW_TOOL_FORGE
#include "wetwaresuite/forge/forge.hpp"
#endif
#if WW_TOOL_LEVELS
#include "wetwaresuite/levels/levels.hpp"
#endif
#if WW_TOOL_SCENE
#include "wetwaresuite/levels/scene.hpp"
#endif
#if WW_TOOL_CAD
#include "wetwaresuite/cad/cad.hpp"
#endif
#if WW_TOOL_BLUEPRINT
#include "wetwaresuite/blueprint/blueprint.hpp"
#endif
#if WW_TOOL_SLICER
#include "wetwaresuite/slicer/slicer.hpp"
#endif
#if WW_TOOL_FIT
#include "wetwaresuite/fit/fit.hpp"
#endif
#if WW_TOOL_CIRCUIT
#include "wetwaresuite/circuit/circuit.hpp"
#endif
#if WW_TOOL_FLASH
#include "wetwaresuite/flash/flash.hpp"
#endif
#if WW_TOOL_AUDIO
#include "wetwaresuite/audio/commands.hpp"
#endif
#if WW_TOOL_BEATS
#include "wetwaresuite/beats/beats.hpp"
#endif
#if WW_TOOL_CUT
#include "wetwaresuite/cut/cut.hpp"
#endif
#if WW_TOOL_CAPTURE
#include "wetwaresuite/capture/capture.hpp"
#endif
#if WW_TOOL_OFFICE
#include "wetwaresuite/office/office.hpp"
#endif
#if WW_TOOL_PDF
#include "wetwaresuite/pdf/commands.hpp"
#endif
#if WW_TOOL_LAYOUT
#include "wetwaresuite/layout/layout.hpp"
#endif
#if WW_TOOL_NOTES
#include "wetwaresuite/notes/notes.hpp"
#endif
#if WW_TOOL_MAPS
#include "wetwaresuite/maps/maps.hpp"
#endif
#if WW_TOOL_CALC
#include "wetwaresuite/calc/calc.hpp"
#endif
#if WW_TOOL_CODE
#include "wetwaresuite/code/code.hpp"
#endif
#if WW_TOOL_EXPLORER
#include "wetwaresuite/explorer/explorer.hpp"
#endif
#if WW_TOOL_TERMINAL
#include "wetwaresuite/terminal/terminal.hpp"
#endif
#if WW_TOOL_MONITOR
#include "wetwaresuite/monitor/monitor.hpp"
#endif
#if WW_TOOL_CLOCK
#include "wetwaresuite/clock/clock.hpp"
#endif
#if WW_TOOL_CONVERT
#include "wetwaresuite/convert/convert.hpp"
#endif
#if WW_TOOL_LIBRARY
#include "wetwaresuite/library/library.hpp"
#endif
#if WW_TOOL_AUTOMATE
#include "wetwaresuite/automate/automate.hpp"
#endif

#ifndef WW_APP_ID
#error "build this through apps/app/CMakeLists.txt, which sets WW_APP_ID"
#endif

namespace {

using namespace wetwaresuite;

void registerTools(Session& session, const std::set<std::string>& requested) {
    std::set<std::string> only = requested;
    if (only.count("paint")) only.insert("photo");  // paint opens and saves through photo
    if (only.count("beats")) only.insert("audio");
    auto want = [&only](const char* tool) { return only.empty() || only.count(tool) > 0; };
    (void)want;
#if WW_TOOL_TOOLKIT
    if (want("toolkit")) toolkit::registerCommands(session.registry());
#endif
#if WW_TOOL_PHOTO
    if (want("photo")) photo::registerCommands(session);
#endif
#if WW_TOOL_PAINT
    if (want("paint")) paint::registerCommands(session);
#endif
#if WW_TOOL_VECTOR
    if (want("vector")) vector::registerCommands(session);
#endif
#if WW_TOOL_TYPE
    if (want("type")) typeface::registerCommands(session);
#endif
#if WW_TOOL_BOARD
    if (want("board")) board::registerCommands(session);
#endif
#if WW_TOOL_MODEL
    if (want("model")) model::registerCommands(session);
#endif
#if WW_TOOL_SCULPT
    if (want("sculpt")) sculpt::registerCommands(session);
#endif
#if WW_TOOL_RENDER
    if (want("render")) render::registerCommands(session);
#endif
#if WW_TOOL_ANIMATE
    if (want("animate")) animate::registerCommands(session);
#endif
#if WW_TOOL_FORGE
    if (want("forge")) forge::registerCommands(session);
#endif
#if WW_TOOL_LEVELS
    if (want("levels")) levels::registerCommands(session);
#endif
#if WW_TOOL_SCENE
    if (want("scene")) scene::registerCommands(session);
#endif
#if WW_TOOL_CAD
    if (want("cad")) cad::registerCommands(session);
#endif
#if WW_TOOL_BLUEPRINT
    if (want("blueprint")) blueprint::registerCommands(session);
#endif
#if WW_TOOL_SLICER
    if (want("slicer")) slicer::registerCommands(session);
#endif
#if WW_TOOL_FIT
    if (want("fit")) fit::registerCommands(session);
#endif
#if WW_TOOL_CIRCUIT
    if (want("circuit")) circuit::registerCommands(session);
#endif
#if WW_TOOL_FLASH
    if (want("flash")) flash::registerCommands(session);
#endif
#if WW_TOOL_AUDIO
    if (want("audio")) audio::registerCommands(session);
#endif
#if WW_TOOL_BEATS
    if (want("beats")) beats::registerCommands(session);
#endif
#if WW_TOOL_CUT
    if (want("cut")) cut::registerCommands(session);
#endif
#if WW_TOOL_CAPTURE
    if (want("capture")) capture::registerCommands(session);
#endif
#if WW_TOOL_OFFICE
    if (want("office")) office::registerCommands(session);
#endif
#if WW_TOOL_PDF
    if (want("pdf")) pdf::registerCommands(session);
#endif
#if WW_TOOL_LAYOUT
    if (want("layout")) layout::registerCommands(session);
#endif
#if WW_TOOL_NOTES
    if (want("notes")) notes::registerCommands(session);
#endif
#if WW_TOOL_MAPS
    if (want("maps")) maps::registerCommands(session);
#endif
#if WW_TOOL_CALC
    if (want("calc")) calc::registerCommands(session);
#endif
#if WW_TOOL_CODE
    if (want("code")) code::registerCommands(session);
#endif
#if WW_TOOL_EXPLORER
    if (want("explorer")) explorer::registerCommands(session);
#endif
#if WW_TOOL_TERMINAL
    if (want("terminal")) terminal::registerCommands(session);
#endif
#if WW_TOOL_MONITOR
    if (want("monitor")) monitor::registerCommands(session);
#endif
#if WW_TOOL_CLOCK
    if (want("clock")) clock::registerCommands(session);
#endif
#if WW_TOOL_CONVERT
    if (want("convert")) convert::registerCommands(session);
#endif
#if WW_TOOL_LIBRARY
    if (want("library")) library::registerCommands(session);
#endif
#if WW_TOOL_AUTOMATE
    if (want("automate")) automate::registerCommands(session, [only](Session& fresh) { registerTools(fresh, only); });
#endif
}

}  // namespace

int main(int argc, char** argv) {
    using namespace wetwaresuite;
    return apps::appMain(argc, argv, {WW_APP_ID, apps::kVersion, registerTools});
}
