// MCP server over stdio (newline-delimited JSON-RPC 2.0). Every command in a
// Session's registry becomes an MCP tool, plus undo, redo, history and
// documents. Open documents are also MCP resources with live previews, so
// Claude can do anything a tool's UI can and look at the result.
#pragma once

#include <cstdint>
#include <iosfwd>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "wetwaresuite/core/command.hpp"

namespace wetwaresuite::mcp {

// How tools/list shows the commands. A big registry (the all-in-one suite has over 1,300
// commands) costs a client megabytes of schemas and makes it pick worse, so Auto switches to
// Catalog there: tools/list holds find_tools, describe_tools and run_tool (plus undo, redo,
// history and documents), and every command is still callable by its own name.
enum class ToolMode { Auto, All, Catalog };
inline constexpr std::size_t kCatalogThreshold = 150;  // Auto: catalog above this many commands

struct ServerInfo {
    std::string name;
    std::string version;
    std::string instructions;  // sent to the client on initialize: how to use these tools
    ToolMode mode = ToolMode::All;
};

// Takes --catalog / --all-tools out of `args` (mcp command line), setting `mode`; Auto when neither.
ToolMode takeToolModeArgs(std::vector<std::string>& args);
const char* toolModeUsage();

class Server {
public:
    Server(Session& session, ServerInfo info);
    Server(Session& session, std::string name, std::string version);

    // Handles one JSON-RPC message; returns the response line, or nothing for notifications.
    std::optional<std::string> handle(const std::string& line);

    // Reads messages from `in` until EOF, writing responses to `out`.
    void run(std::istream& in, std::ostream& out);

    // "photo.resize" -> "photo_resize"
    static std::string toolName(const std::string& commandId);

    // true when tools/list shows the catalog tools instead of every command
    bool catalog() const;

private:
    const std::string& toolsJson();                           // cached until the registry changes
    std::string commandFor(const std::string& tool);          // MCP tool name -> command id ("" = none)
    std::string callTool(const std::string& tool, const std::string& argumentsJson);  // a tools/call result, as json text
    std::string catalogJson();
    Session& session_;
    ServerInfo info_;
    std::string tools_;
    std::uint64_t toolsRevision_ = ~std::uint64_t{0};
    std::unordered_map<std::string, std::string> commands_;  // tool name -> command id
    std::uint64_t commandsRevision_ = ~std::uint64_t{0};
};

// The "tools" array of a tools/list answer (every command plus undo, redo, history, documents)
// as compact JSON. toolsListJson writes the text directly (fast: the suite has ~700 tools);
// toolsListJsonReference builds it with nlohmann::json. Both give the same bytes.
std::string toolsListJson(const CommandRegistry& registry);
std::string toolsListJsonReference(const CommandRegistry& registry);

std::string base64(const std::string& bytes);

}  // namespace wetwaresuite::mcp
