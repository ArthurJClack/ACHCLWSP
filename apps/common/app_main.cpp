#include "app_main.hpp"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <vector>

#include "app_commands.hpp"
#include "apps.hpp"
#include "gui_launch.hpp"
#include "launch.hpp"
#include "wetwaresuite/core/cli.hpp"
#include "wetwaresuite/mcp/server.hpp"
#include "wetwaresuite/playback/player.hpp"
#include "wetwaresuite/mcp/transport.hpp"

namespace wetwaresuite::apps {

namespace {

std::string toolOf(const std::string& id) { return id.substr(0, id.find('.')); }

std::string joined(const std::vector<std::string>& v) {
    std::string out;
    for (const auto& t : v) out += (out.empty() ? "" : ", ") + t;
    return out;
}

void usage(const std::string& bin, const App& app) {
    std::cout << bin << ": " << app.summary << " (tools: " << joined(app.tools) << ")\n\n"
              << "usage:\n"
              << "  " << bin << "                          open the window (also: " << bin << " <files>, open <files>, gui)\n"
              << "  " << bin << " list [tool]              list commands (optionally for one tool)\n"
              << "  " << bin << " help <command>           show a command's parameters\n"
              << "  " << bin << " run <command> key=value  run one command\n"
              << "  " << bin << " <command> key=value      same as run\n"
              << "  " << bin << " batch [file] [--keep-going]  run a script of commands (stdin if no file)\n"
              << "  " << bin << " mcp [--tools a,b] [--http]  serve this app to Claude over MCP (stdin/stdout, or HTTP)\n"
              << "  " << bin << " app.receive              open the newest file another app sent here\n"
              << "  " << bin << " app.palette_set primary=#rrggbb  window colours for every app (also secondary=, tertiary=)\n"
              << "  " << bin << " --version\n";
}

int list(const Session& session, const std::string& tool) {
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

int help(const Session& session, const std::string& bin, const std::string& id) {
    const auto* info = session.registry().find(id);
    if (!info) {
        std::cerr << "unknown command: " << id << " (try `" << bin << " list`; other apps have other commands, see `"
                  << bin << " app.installed`)\n";
        return 2;
    }
    std::cout << info->id << ": " << info->title << "\n" << info->description << "\n\nusage: " << cli::usage(*info) << "\n";
    for (const auto& p : info->params) {
        std::cout << "  " << p.name << " (" << toString(p.type) << (p.required ? ", required" : "") << ")  " << p.description;
        if (!p.choices.empty()) {
            std::cout << " [";
            for (std::size_t i = 0; i < p.choices.size(); ++i) std::cout << (i ? "|" : "") << p.choices[i];
            std::cout << "]";
        }
        std::string d = toText(p.defaultValue);
        if (!d.empty()) std::cout << " (default " << d << ")";
        std::cout << "\n";
    }
    return 0;
}

int serve(const MainSpec& spec, const App& app, const std::vector<std::string>& args) {
    const std::string bin = binaryName(spec.app);
    // --tools picks what to serve; everything else picks the transport (see mcp/transport.hpp)
    std::set<std::string> keep;
    std::vector<std::string> rest;
    for (std::size_t i = 1; i < args.size(); ++i) {
        if (args[i] != "--tools" || i + 1 >= args.size()) {
            rest.push_back(args[i]);
            continue;
        }
        std::string item;
        for (char c : args[++i] + ",") {
            if (c != ',') {
                item += c;
                continue;
            }
            if (!item.empty()) keep.insert(item);
            item.clear();
        }
    }
    for (const auto& t : keep) {
        if (std::find(app.tools.begin(), app.tools.end(), t) == app.tools.end()) {
            std::cerr << bin << " has no tool called " << t << "; tools: " << joined(app.tools) << "\n";
            return 2;
        }
    }
    const mcp::ToolMode mode = mcp::takeToolModeArgs(rest);
    mcp::TransportOptions transport;
    std::string error;
    if (!mcp::parseTransportArgs(rest, transport, error)) {
        std::cerr << "error: " << error << "\nusage: " << bin << " mcp [--tools a,b] [--catalog|--all-tools] [--http ...]  (tools: "
                  << joined(app.tools) << ")\n" << mcp::toolModeUsage() << mcp::transportUsage();
        return 2;
    }
    Session session;
    spec.registerTools(session, keep);
    registerCommands(session, spec.app);
    mcp::Server server(session, {bin, spec.version, appInstructions(spec.app) + handoffInstructions(spec.app), mode});
    return mcp::serve(server, transport, std::cin, std::cout, std::cerr);
}

}  // namespace

int appMain(int argc, char** argv, const MainSpec& spec) {
    std::vector<std::string> args(argv + 1, argv + argc);
    if (argc > 0) setSelfPath(argv[0]);
    const App* app = find(spec.app);
    if (!app) {
        std::cerr << "unknown app " << spec.app << "\n";
        return 2;
    }
    const std::string bin = binaryName(spec.app);
    if (!args.empty() && (args[0] == "--help" || args[0] == "-h")) {
        usage(bin, *app);
        return 0;
    }
    try {
        // no arguments, `open`/`gui`, or files dragged onto the program: the window
        Session session;
        spec.registerTools(session, {});
        registerCommands(session, spec.app);
        auto isFile = [](const std::string& a) {
            std::error_code ec;
            return std::filesystem::is_regular_file(a, ec);
        };
        auto isCommand = [&session](const std::string& a) { return session.registry().find(a) != nullptr; };
        Launch launch = decideLaunch(args, isFile, isCommand);
        if (launch.kind == LaunchKind::Gui) {
            if (!launch.error.empty()) {
                std::cerr << bin << ": " << launch.error << "\n";
                return 2;
            }
            gui::RunOptions options{launch.screenshot, launch.files, launch.action};
            return runGui(session, guiInfo(spec.app), options, [&] { usage(bin, *app); });
        }
        const std::string cmd = args[0];
        if (cmd == "--version") {
            std::cout << bin << " " << spec.version << "\n";
            return 0;
        }
        if (cmd == "mcp") return serve(spec, *app, args);
        if (cmd == "list") {
            bool known = args.size() < 2 || args[1] == "app" ||
                         std::find(app->tools.begin(), app->tools.end(), args[1]) != app->tools.end();
            if (!known) {
                const App* other = appOfTool(args[1]);
                std::cerr << bin << " has no tool called " << args[1] << "; tools: " << joined(app->tools) << ", app"
                          << (other ? " (" + args[1] + " is in " + binaryName(other->id) + ")" : std::string()) << "\n";
                return 2;
            }
            return list(session, args.size() > 1 ? args[1] : "");
        }
        if (cmd == "help") {
            if (args.size() == 2) return help(session, bin, args[1]);
            usage(bin, *app);
            return args.size() == 1 ? 0 : 2;
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
        std::size_t start = cmd == "run" ? 1 : 0;
        if (start >= args.size()) {
            usage(bin, *app);
            return 2;
        }
        const std::string id = args[start];
        const auto* info = session.registry().find(id);
        if (!info) {
            const App* other = appOfTool(toolOf(id));
            std::cerr << "unknown command: " << id << " (try `" << bin << " list`"
                      << (other && other->id != spec.app ? "; " + toolOf(id) + " is in " + binaryName(other->id) : std::string())
                      << ")\n";
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

}  // namespace wetwaresuite::apps
