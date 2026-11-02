// wetwaresuite: every tool in one headless binary. Run single commands, batch
// scripts, or serve everything to Claude over MCP.
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <set>
#include <string>
#include <vector>

#include "wetwaresuite/core/cli.hpp"
#include "wetwaresuite/mcp/server.hpp"
#include "wetwaresuite/playback/player.hpp"
#include "app_commands.hpp"
#include "apps.hpp"
#include "gui_launch.hpp"
#include "launch.hpp"
#include "wetwaresuite/mcp/transport.hpp"
#include "suite.hpp"

namespace {

using wetwaresuite::apps::kVersion;

constexpr const char* kInstructions =
    "wetwaresuite is a creative suite you drive with tools. Workflow: open or create a document (photo_open, photo_new, "
    "paint_new, vector_new), which returns a doc id like img1; pass that id as `doc` to edit it; call photo_preview "
    "(or pass preview=true on most edits) to see the result as an image; then save with photo_save or vector_save. "
    "Every edit is undoable with undo/redo, and `documents` lists what's open. Coordinates are pixels from the "
    "top-left. Colours accept #rrggbb, #rrggbbaa, rgb(r,g,b) or names. Strokes take points as a flat [x1,y1,x2,y2,...] "
    "list. convert_file changes file formats; convert_formats shows what's available. toolkit_inspect identifies "
    "any file. blueprint_* tools are the exception to pixel coordinates: technical drawings and floor plans in real "
    "units (mm, m, ft...) with y pointing up, drawn to a scale such as 1:50 (blueprint_new returns bp1; "
    "blueprint_list and blueprint_measure read exact values; blueprint_export writes PDF/SVG/PNG to scale). 3D work is in mm with z up: cad_* builds parametric parts "
    "(cad_new returns cad1; numeric fields take expressions over cad_param_set parameters; cad_save exports STL/3MF), "
    "slicer_* turns a model into G-code (slicer_slice_file is the one-shot), fit_* designs car parts and mounts that fit "
    "a phone 3D scan (fit_open fixes metres and Y-up and returns fit1; fit_plane/fit_cylinder/fit_circle fit clean shapes to "
    "noisy patches; fit_plate, fit_block, fit_clamp, fit_phone_mount, fit_gauge_pod build parts with a gap; fit_pick turns a "
    "u,v spot on a preview into a point; fit_check shows the gap), forge_* prepares game assets (meshes, "
    "LODs, sprite sheets, textures). Media and office: audio_* edits sound in seconds (audio_open returns aud1; "
    "audio_preview shows a waveform or spectrogram; audio_measure gives LUFS), cut_* edits video on a timeline "
    "(cut_new returns cut1; cut_preview shows a frame; cut_render exports), capture_screenshot without output returns "
    "an image of the screen, office_* handles spreadsheets with formulas, documents, slide decks and PDF tools, "
    "layout_* builds print layouts exported to PDF, library_* indexes and searches asset folders, and automate_* runs "
    "batch scripts, pipelines and folder watches. 3D art and games: sculpt_* sculpts meshes with brush strokes along "
    "point paths (sculpt_new returns sculpt1; strokes accept space=screen points on a preview), render_* builds "
    "scenes and path-traces them (render_new returns scene1; render_render without output returns the image), "
    "animate_* keyframes 2D layers, cels, bone rigs and 3D meshes and exports GIF, sprite sheets or video, "
    "levels_* edits tilemaps and generates caves and dungeons from a seed (Tiled TMX/TMJ, LDtk), and beats_* makes "
    "music from step patterns, notes and chords (beats_generate makes a starter song; beats_render writes audio). "
    "Making and organising: circuit_* draws schematics in mm and lays out PCBs (circuit_new returns ckt1; circuit_erc "
    "and circuit_drc check them; circuit_gerber writes fab files), flash_* finds boards on serial ports and flashes "
    "Arduino, ESP32 and STM32 firmware (flash_ports first; sim: boards let you try it without hardware), maps_* "
    "handles GeoJSON/KML/GPX/Shapefile data in lon/lat and renders maps (maps_new returns map1), board_* makes "
    "whiteboards and diagrams (board_from_mermaid turns Mermaid text into a laid-out diagram), notes_* works on an "
    "Obsidian-style folder of Markdown notes (notes_search, notes_backlinks, notes_tasks), and type_* designs fonts "
    "and exports TTF/WOFF (type_new returns font1; type_proof shows a specimen). System: calc_eval works out "
    "expressions with exact fractions, units (\"5 km to miles\") and hex/bitwise maths; code_* edits source files "
    "(code_open returns code1; code_edit replaces exact text), searches folders and runs or builds with the installed "
    "compilers; explorer_* manages files with undo through its own trash and makes or unpacks zip/tar; terminal_* "
    "runs shell commands (terminal_open returns term1; terminal_exec for one-offs); monitor_* reads processor, memory, "
    "disks and processes; clock_* gives world time, timers, alarms and date maths.";

void usage() {
    std::cout << "wetwaresuite " << kVersion << ": the whole suite, headless\n\n"
              << "usage:\n"
                 "  wetwaresuite                          open the launcher: every app starts in its own window\n"
                 "  wetwaresuite <files>                  open each file in its own app (also: open <files>)\n"
                 "  wetwaresuite gui --all                every tool in one window\n"
                 "  wetwaresuite list [tool]              list commands (optionally for one tool, e.g. photo)\n"
                 "  wetwaresuite help <command>           show a command's parameters\n"
                 "  wetwaresuite run <command> key=value  run one command, e.g. run convert.file input=a.png output=a.jpg\n"
                 "  wetwaresuite <command> key=value      same as run\n"
                 "  wetwaresuite batch [file] [--keep-going]  run a script of commands (stdin if no file)\n"
                 "  wetwaresuite mcp [--tools a,b] [--catalog|--all-tools]  serve all (or some) tools to Claude over MCP\n"
                 "  wetwaresuite mcp --http [--port N] [--host H] [--token T]  serve MCP over HTTP instead\n"
                 "  wetwaresuite --version\n";
}

std::string toolOf(const std::string& id) { return id.substr(0, id.find('.')); }

std::string joinedTools() {
    std::string out;
    for (const auto& t : wetwaresuite::suite::toolNames()) out += (out.empty() ? "" : ", ") + t;
    return out;
}

int list(const wetwaresuite::Session& session, const std::string& tool) {
    std::string current;
    for (const auto& info : session.registry().list()) {
        if (!tool.empty() && toolOf(info.id) != tool) continue;
        if (toolOf(info.id) != current) {
            current = toolOf(info.id);
            std::cout << (current.empty() ? "" : "\n") << current << "\n";
        }
        std::cout << "  " << info.id << "  " << info.title << (info.readOnly ? "  (read-only)" : info.undoable ? "" : "  (no undo)") << "\n";
    }
    return 0;
}

int help(const wetwaresuite::Session& session, const std::string& id) {
    const auto* info = session.registry().find(id);
    if (!info) {
        std::cerr << "unknown command: " << id << " (try `wetwaresuite list`)\n";
        return 2;
    }
    std::cout << info->id << ": " << info->title << "\n" << info->description << "\n\nusage: " << wetwaresuite::cli::usage(*info) << "\n";
    for (const auto& p : info->params) {
        std::cout << "  " << p.name << " (" << wetwaresuite::toString(p.type) << (p.required ? ", required" : "") << ")  "
                  << p.description;
        if (!p.choices.empty()) {
            std::cout << " [";
            for (std::size_t i = 0; i < p.choices.size(); ++i) std::cout << (i ? "|" : "") << p.choices[i];
            std::cout << "]";
        }
        std::string d = wetwaresuite::toText(p.defaultValue);
        if (!d.empty()) std::cout << " (default " << d << ")";
        std::cout << "\n";
    }
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    using namespace wetwaresuite;
    std::vector<std::string> args(argv + 1, argv + argc);
    if (!args.empty() && (args[0] == "--help" || args[0] == "-h")) {
        usage();
        return 0;
    }

    if (!args.empty() && args[0] == "--version") {  // no need to register ~700 commands for this
        std::cout << "wetwaresuite " << kVersion << "\n";
        return 0;
    }

    if (argc > 0) apps::setSelfPath(argv[0]);
    // `mcp --tools a,b` builds its own smaller session below, so it skips the full one
    const auto toolsFlag = std::find(args.begin(), args.end(), "--tools");
    const bool narrowMcp = !args.empty() && args[0] == "mcp" && toolsFlag != args.end() && toolsFlag + 1 != args.end();
    Session session;
    if (!narrowMcp) {
        suite::registerAll(session);
        apps::registerCommands(session, "suite");
    }

    // no arguments, `open`/`gui`, or files dragged onto the program: the window
    auto isFile = [](const std::string& a) {
        std::error_code ec;
        return std::filesystem::is_regular_file(a, ec);
    };
    auto isCommand = [&session](const std::string& a) { return session.registry().find(a) != nullptr; };
    apps::Launch launch = apps::decideLaunch(args, isFile, isCommand);
    if (launch.kind == apps::LaunchKind::Gui) {
        if (!launch.error.empty()) {
            std::cerr << "wetwaresuite: " << launch.error << "\n";
            return 2;
        }
        gui::RunOptions options{launch.screenshot, launch.files, launch.action};
        // the launcher (or files straight to their own apps), unless asked for every
        // tool in one window or no app is installed next to this program
        if (!launch.all && launch.action.empty()) {
            int code = apps::runLauncher(session, options, usage);
            if (code >= 0) return code;
        }
        return apps::runGui(session, apps::guiInfo("suite"), options, usage);
    }
    const std::string cmd = args[0];
    const std::string instructions = std::string(kInstructions) + apps::handoffInstructions("suite");

    try {
        if (cmd == "list") {
            if (args.size() > 1 && args[1] != "app" && !suite::toolNames().count(args[1])) {
                std::cerr << "no tool called " << args[1] << "; tools: " << joinedTools() << "\n";
                return 2;
            }
            return list(session, args.size() > 1 ? args[1] : "");
        }
        if (cmd == "help") {
            if (args.size() == 2) return help(session, args[1]);
            usage();
            return args.size() == 1 ? 0 : 2;
        }
        if (cmd == "mcp") {
            // --tools picks what to serve; everything else picks the transport (see mcp/transport.hpp)
            std::vector<std::string> rest;
            std::string toolList;
            bool hasTools = false;
            for (std::size_t i = 1; i < args.size(); ++i) {
                if (args[i] == "--tools" && i + 1 < args.size()) {
                    toolList = args[++i];
                    hasTools = true;
                } else {
                    rest.push_back(args[i]);
                }
            }
            const mcp::ToolMode mode = mcp::takeToolModeArgs(rest);
            mcp::TransportOptions transport;
            std::string error;
            if (!mcp::parseTransportArgs(rest, transport, error)) {
                std::cerr << "error: " << error << "\nusage: wetwaresuite mcp [--tools a,b] [--catalog|--all-tools] [--http ...]  (tools: "
                          << joinedTools() << ")\n" << mcp::toolModeUsage() << mcp::transportUsage();
                return 2;
            }
            if (hasTools) {
                std::set<std::string> keep;
                std::string item;
                for (char c : toolList + ",") {
                    if (c == ',') {
                        if (!item.empty()) keep.insert(item);
                        item.clear();
                    } else {
                        item += c;
                    }
                }
                for (const auto& t : keep) {
                    if (!suite::toolNames().count(t)) {
                        std::cerr << "no tool called " << t << "; tools: " << joinedTools() << "\n";
                        return 2;
                    }
                }
                if (keep.empty()) {
                    std::cerr << "--tools needs a list, e.g. --tools photo,paint\n";
                    return 2;
                }
                Session filtered;
                suite::registerAll(filtered, keep);
                apps::registerCommands(filtered, "suite");
                mcp::Server server(filtered, {"wetwaresuite", kVersion, instructions, mode});
                return mcp::serve(server, transport, std::cin, std::cout, std::cerr);
            }
            mcp::Server server(session, {"wetwaresuite", kVersion, instructions, mode});
            return mcp::serve(server, transport, std::cin, std::cout, std::cerr);
        }
        if (cmd == "batch") {
            bool keepGoing = false;
            std::string file;
            for (std::size_t i = 1; i < args.size(); ++i) {
                if (args[i] == "--keep-going") keepGoing = true;
                else file = args[i];
            }
            cli::Batch batch(session);
            if (file.empty() || file == "-") {
                const int rc = batch.run(std::cin, std::cout, std::cerr, keepGoing) == 0 ? 0 : 1;
                playback::finishBeforeExit();
                return rc;
            }
            std::ifstream in(file);
            if (!in) {
                std::cerr << "cannot open " << file << "\n";
                return 2;
            }
            const int rc = batch.run(in, std::cout, std::cerr, keepGoing) == 0 ? 0 : 1;
            playback::finishBeforeExit();  // let a played sound finish
            return rc;
        }

        // run <command> ... or <command> ...
        std::size_t start = cmd == "run" ? 1 : 0;
        if (start >= args.size()) {
            usage();
            return 2;
        }
        const std::string id = args[start];
        const auto* info = session.registry().find(id);
        if (!info) {
            std::cerr << "unknown command: " << id << " (try `wetwaresuite list`)\n";
            return 2;
        }
        Params params = cli::parseArgs(*info, {args.begin() + static_cast<std::ptrdiff_t>(start) + 1, args.end()});
        Result r = session.run(id, params);
        std::string text = cli::format(r);
        (r.ok ? std::cout : std::cerr) << text << (text.empty() ? "" : "\n");
        if (r.ok) playback::finishBeforeExit();  // `audio.play path=a.wav` is heard, not cut off
        return r.ok ? 0 : 1;
    } catch (const std::exception& e) {
        std::cerr << "error: " << e.what() << "\n";
        return 1;
    }
}
