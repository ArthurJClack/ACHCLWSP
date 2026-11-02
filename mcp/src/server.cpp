#include <cmath>
#include "wetwaresuite/mcp/server.hpp"

#include <array>
#include <cstdio>
#include <cstdlib>
#include <algorithm>
#include <cctype>
#include <map>
#include <vector>
#include <istream>
#include <ostream>

#include <nlohmann/json.hpp>

#include "wetwaresuite/core/cli.hpp"

namespace wetwaresuite::mcp {

using nlohmann::json;

namespace {

constexpr std::array kProtocolVersions = {"2025-11-25", "2025-06-18", "2025-03-26", "2024-11-05"};
constexpr const char* kDocScheme = "wetwaresuite://doc/";

json rpcResult(const json& id, json result) {
    return {{"jsonrpc", "2.0"}, {"id", id}, {"result", std::move(result)}};
}

json rpcError(const json& id, int code, const std::string& message) {
    return {{"jsonrpc", "2.0"}, {"id", id}, {"error", {{"code", code}, {"message", message}}}};
}

json textResult(const std::string& text, bool isError) {
    return {{"content", json::array({{{"type", "text"}, {"text", text}}})}, {"isError", isError}};
}

json toJson(const Value& value) {
    if (auto s = std::get_if<std::string>(&value)) return *s;
    if (auto b = std::get_if<bool>(&value)) return *b;
    if (auto i = std::get_if<std::int64_t>(&value)) return *i;
    if (auto d = std::get_if<double>(&value)) return *d;
    if (auto l = std::get_if<NumberList>(&value)) return *l;
    return nullptr;
}

json toolResult(const Result& result) {
    json content = json::array();
    std::string text = cli::format(result);
    content.push_back({{"type", "text"}, {"text", text.empty() ? (result.ok ? "done" : "failed") : text}});
    for (const auto& a : result.attachments) {
        if (a.mime.rfind("image/", 0) == 0) {
            content.push_back({{"type", "image"}, {"data", base64(a.bytes)}, {"mimeType", a.mime}});
        } else {
            content.push_back({{"type", "resource"},
                               {"resource", {{"uri", "wetwaresuite://attachment/" + a.name},
                                             {"mimeType", a.mime},
                                             {"blob", base64(a.bytes)}}}});
        }
    }
    json out = {{"content", content}, {"isError", !result.ok}};
    if (!result.data.empty()) {
        json structured = json::object();
        for (const auto& [key, v] : result.data) structured[key] = toJson(v);
        out["structuredContent"] = structured;
    }
    return out;
}

// Session::run fills a missing doc with the one the tool used last, so calls can chain without it.
bool lastDocDefault(const ParamSpec& p) { return p.required && p.name == "doc"; }
std::string paramDescription(const ParamSpec& p) {
    return lastDocDefault(p) ? p.description + " (default: the one this tool used last)" : p.description;
}

json schemaFor(const ParamSpec& p) {
    json s = {{"type", toString(p.type)}, {"description", paramDescription(p)}};
    if (p.type == ParamType::NumberList) s["items"] = {{"type", "number"}};
    if (!p.choices.empty()) s["enum"] = p.choices;
    if (!std::holds_alternative<std::monostate>(p.defaultValue)) s["default"] = toJson(p.defaultValue);
    return s;
}

json describe(const CommandInfo& info) {
    json properties = json::object();
    json required = json::array();
    for (const auto& p : info.params) {
        properties[p.name] = schemaFor(p);
        if (p.required && !lastDocDefault(p)) required.push_back(p.name);
    }
    return {{"name", Server::toolName(info.id)},
            {"title", info.title},
            {"description", info.description},
            {"inputSchema", {{"type", "object"}, {"properties", properties}, {"required", required}}},
            {"annotations", {{"title", info.title}, {"readOnlyHint", info.readOnly},
                             {"destructiveHint", !info.readOnly && !info.undoable}, {"openWorldHint", false}}}};
}

json noArgsTool(const char* name, const char* description, bool readOnly) {
    return {{"name", name},
            {"description", description},
            {"inputSchema", {{"type", "object"}, {"properties", json::object()}}},
            {"annotations", {{"readOnlyHint", readOnly}, {"openWorldHint", false}}}};
}

// ---- tools/list written straight to text, matching nlohmann's dump() byte for byte:
// object keys in sorted order, no spaces, the same string escapes and number formats.

void putString(std::string& out, const std::string& s) {
    for (unsigned char c : s) {
        if (c >= 0x80) {  // UTF-8: let nlohmann check and copy it
            out += json(s).dump();
            return;
        }
    }
    out += '"';
    for (char ch : s) {
        const auto c = static_cast<unsigned char>(ch);
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\b': out += "\\b"; break;
            case '\f': out += "\\f"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (c < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof buf, "\\u%04x", c);
                    out += buf;
                } else {
                    out += ch;
                }
        }
    }
    out += '"';
}

void putBool(std::string& out, bool b) { out += b ? "true" : "false"; }

void putValue(std::string& out, const Value& value) {
    if (auto s = std::get_if<std::string>(&value)) putString(out, *s);
    else if (auto b = std::get_if<bool>(&value)) putBool(out, *b);
    else if (auto i = std::get_if<std::int64_t>(&value)) out += std::to_string(*i);
    else out += toJson(value).dump();  // doubles and lists: nlohmann's own number format
}

void putTool(std::string& out, const CommandInfo& info) {
    std::map<std::string, const ParamSpec*> props;  // sorted like a json object; a repeated name keeps the last
    for (const auto& p : info.params) props[p.name] = &p;
    out += "{\"annotations\":{\"destructiveHint\":";
    putBool(out, !info.readOnly && !info.undoable);
    out += ",\"openWorldHint\":false,\"readOnlyHint\":";
    putBool(out, info.readOnly);
    out += ",\"title\":";
    putString(out, info.title);
    out += "},\"description\":";
    putString(out, info.description);
    out += ",\"inputSchema\":{\"properties\":{";
    bool first = true;
    for (const auto& [name, p] : props) {
        if (!first) out += ',';
        first = false;
        putString(out, name);
        out += ":{";
        if (!std::holds_alternative<std::monostate>(p->defaultValue)) {
            out += "\"default\":";
            putValue(out, p->defaultValue);
            out += ',';
        }
        out += "\"description\":";
        putString(out, paramDescription(*p));
        if (!p->choices.empty()) {
            out += ",\"enum\":[";
            for (std::size_t i = 0; i < p->choices.size(); ++i) {
                if (i) out += ',';
                putString(out, p->choices[i]);
            }
            out += ']';
        }
        if (p->type == ParamType::NumberList) out += ",\"items\":{\"type\":\"number\"}";
        out += ",\"type\":";
        putString(out, toString(p->type));
        out += '}';
    }
    out += "},\"required\":[";
    first = true;
    for (const auto& p : info.params) {
        if (!p.required || lastDocDefault(p)) continue;
        if (!first) out += ',';
        first = false;
        putString(out, p.name);
    }
    out += "],\"type\":\"object\"},\"name\":";
    putString(out, Server::toolName(info.id));
    out += ",\"title\":";
    putString(out, info.title);
    out += '}';
}

struct NoArgsTool {
    const char* name;
    const char* description;
    bool readOnly;
};
constexpr std::array kNoArgsTools = {NoArgsTool{"undo", "Undo the last change to any open document", false},
                                     NoArgsTool{"redo", "Redo the last undone change", false},
                                     NoArgsTool{"history", "List the undo history, oldest first", true},
                                     NoArgsTool{"documents", "List open documents with their ids, kinds and sizes", true}};

bool toParams(const json& args, Params& out, std::string& error) {
    if (args.is_null()) return true;
    if (!args.is_object()) {
        error = "arguments must be an object";
        return false;
    }
    for (const auto& [key, v] : args.items()) {
        if (v.is_boolean()) out[key] = v.get<bool>();
        else if (v.is_number_integer()) out[key] = v.get<std::int64_t>();
        else if (v.is_number()) {
            // 300.0 from a client means 300; whole numbers satisfy integer params
            double d = v.get<double>();
            if (std::floor(d) == d && std::abs(d) < 9e15) out[key] = static_cast<std::int64_t>(d);
            else out[key] = d;
        }
        else if (v.is_string()) out[key] = v.get<std::string>();
        else if (v.is_array()) {
            NumberList list;
            for (const auto& item : v) {
                if (!item.is_number()) {
                    error = "argument '" + key + "' must be an array of numbers";
                    return false;
                }
                list.push_back(item.get<double>());
            }
            out[key] = std::move(list);
        } else if (v.is_null()) {
            continue;
        } else {
            error = "argument '" + key + "' must be a boolean, number, string or array of numbers";
            return false;
        }
    }
    return true;
}


// ---- catalog mode: find_tools / describe_tools / run_tool ----

constexpr const char* kFindTools = "find_tools";
constexpr const char* kDescribeTools = "describe_tools";
constexpr const char* kRunTool = "run_tool";

json catalogTools() {
    auto tool = [](const char* name, const char* title, const char* description, json properties, json required,
                   bool readOnly) {
        return json{{"name", name},
                    {"title", title},
                    {"description", description},
                    {"inputSchema", {{"type", "object"}, {"properties", std::move(properties)}, {"required", std::move(required)}}},
                    {"annotations", {{"title", title}, {"readOnlyHint", readOnly}, {"openWorldHint", false}}}};
    };
    json tools = json::array();
    tools.push_back(tool(kFindTools, "Find tools",
                         "Search this server's tools by what you want to do (e.g. 'resize image', 'fillet edge', "
                         "'export pdf'), or list a whole family by its name (e.g. 'photo', 'cad'). Gives each "
                         "match's name and what it does; then describe_tools for its parameters and run_tool to use it.",
                         {{"query", {{"type", "string"}, {"description", "words describing the job, or a tool family name"}}},
                          {"limit", {{"type", "integer"}, {"description", "most matches to return (1-200)"}, {"default", 20}}}},
                         json::array({"query"}), true));
    tools.push_back(tool(kDescribeTools, "Describe tools",
                         "The full input schema (every parameter, its type, choices and default) of one or more tools "
                         "found with find_tools.",
                         {{"names", {{"type", "string"}, {"description", "tool names, comma separated, e.g. photo_resize,photo_crop"}}}},
                         json::array({"names"}), true));
    tools.push_back(tool(kRunTool, "Run a tool",
                         "Run any tool of this server by name with its arguments (see describe_tools). Same as calling "
                         "the tool directly; each change is one undo step.",
                         {{"name", {{"type", "string"}, {"description", "tool name, e.g. photo_resize"}}},
                          {"arguments", {{"type", "object"}, {"description", "the tool's arguments"}}}},
                         json::array({"name"}), false));
    return tools;
}

std::string lower(std::string s) {
    for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

std::vector<std::string> words(const std::string& text) {
    std::vector<std::string> out;
    std::string w;
    for (char ch : text + " ") {
        const auto c = static_cast<unsigned char>(ch);
        if (std::isalnum(c)) {
            w += static_cast<char>(std::tolower(c));
            continue;
        }
        if (!w.empty()) {
            // a plural asks for the same thing: "layers" finds "layer"
            if (w.size() > 3 && w.back() == 's' && w[w.size() - 2] != 's') w.pop_back();
            out.push_back(w);
        }
        w.clear();
    }
    return out;
}

std::string firstSentence(const std::string& text) {
    auto end = text.find(". ");
    std::string s = end == std::string::npos ? text : text.substr(0, end + 1);
    if (s.size() > 220) s = s.substr(0, 217) + "...";
    return s;
}

std::string family(const std::string& commandId) { return commandId.substr(0, commandId.find('.')); }

struct Match {
    int score;
    const CommandInfo* info;
};

std::vector<Match> findTools(const CommandRegistry& registry, const std::string& query) {
    const std::vector<std::string> want = words(query);
    std::vector<Match> out;
    if (want.empty()) return out;
    // the name of a family ("photo", "cad") lists all of it
    const bool wholeFamily = want.size() == 1 && [&] {
        bool found = false;
        registry.forEach([&](const CommandInfo& info) { found = found || family(info.id) == want[0]; });
        return found;
    }();
    registry.forEach([&](const CommandInfo& info) {
        if (wholeFamily) {
            if (family(info.id) == want[0]) out.push_back({0, &info});
            return;
        }
        const std::string id = lower(info.id), title = lower(info.title), desc = lower(info.description);
        const auto idWords = words(info.id), titleWords = words(info.title);
        auto has = [](const std::vector<std::string>& ws, const std::string& w) {
            return std::find(ws.begin(), ws.end(), w) != ws.end();
        };
        int score = 0, matched = 0;
        for (const auto& w : want) {
            int best = 0;
            if (has(idWords, w)) best = 8;
            else if (has(titleWords, w)) best = 6;
            else if (id.find(w) != std::string::npos) best = 4;
            else if (title.find(w) != std::string::npos) best = 3;
            else if (desc.find(w) != std::string::npos) best = 1;
            if (best) ++matched;
            score += best;
        }
        if (!matched) return;
        // every word found beats most words found, however strong
        score += matched == static_cast<int>(want.size()) ? 1000 : matched * 100;
        out.push_back({score, &info});
    });
    std::stable_sort(out.begin(), out.end(), [](const Match& a, const Match& b) { return a.score > b.score; });
    return out;
}

// "photo (45), paint (30), ..." in registry order, for the instructions
std::string familySummary(const CommandRegistry& registry) {
    std::vector<std::pair<std::string, int>> counts;
    registry.forEach([&](const CommandInfo& info) {
        const std::string f = family(info.id);
        auto it = std::find_if(counts.begin(), counts.end(), [&](const auto& c) { return c.first == f; });
        if (it == counts.end()) counts.emplace_back(f, 1);
        else ++it->second;
    });
    std::string out;
    for (const auto& [f, n] : counts) out += (out.empty() ? "" : ", ") + f + " (" + std::to_string(n) + ")";
    return out;
}

}  // namespace

std::string base64(const std::string& bytes) {
    static constexpr char table[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve((bytes.size() + 2) / 3 * 4);
    std::size_t i = 0;
    for (; i + 2 < bytes.size(); i += 3) {
        unsigned n = (static_cast<unsigned char>(bytes[i]) << 16) | (static_cast<unsigned char>(bytes[i + 1]) << 8) |
                     static_cast<unsigned char>(bytes[i + 2]);
        out += table[(n >> 18) & 63];
        out += table[(n >> 12) & 63];
        out += table[(n >> 6) & 63];
        out += table[n & 63];
    }
    if (i < bytes.size()) {
        unsigned n = static_cast<unsigned char>(bytes[i]) << 16;
        if (i + 1 < bytes.size()) n |= static_cast<unsigned char>(bytes[i + 1]) << 8;
        out += table[(n >> 18) & 63];
        out += table[(n >> 12) & 63];
        out += i + 1 < bytes.size() ? table[(n >> 6) & 63] : '=';
        out += '=';
    }
    return out;
}

std::string toolsListJson(const CommandRegistry& registry) {
    std::string out = "[";
    out.reserve(registry.size() * 1200);
    registry.forEach([&](const CommandInfo& info) {
        if (out.size() > 1) out += ',';
        putTool(out, info);
    });
    for (const auto& t : kNoArgsTools) {
        if (out.size() > 1) out += ',';
        out += "{\"annotations\":{\"openWorldHint\":false,\"readOnlyHint\":";
        putBool(out, t.readOnly);
        out += "},\"description\":";
        putString(out, t.description);
        out += ",\"inputSchema\":{\"properties\":{},\"type\":\"object\"},\"name\":";
        putString(out, t.name);
        out += '}';
    }
    out += ']';
    return out;
}

std::string toolsListJsonReference(const CommandRegistry& registry) {
    json tools = json::array();
    registry.forEach([&](const CommandInfo& info) { tools.push_back(describe(info)); });
    for (const auto& t : kNoArgsTools) tools.push_back(noArgsTool(t.name, t.description, t.readOnly));
    return tools.dump();
}

ToolMode takeToolModeArgs(std::vector<std::string>& args) {
    ToolMode mode = ToolMode::Auto;
    std::vector<std::string> rest;
    for (auto& a : args) {
        if (a == "--catalog") mode = ToolMode::Catalog;
        else if (a == "--all-tools") mode = ToolMode::All;
        else rest.push_back(std::move(a));
    }
    args = std::move(rest);
    return mode;
}

const char* toolModeUsage() {
    return "  --catalog     list three tools (find_tools, describe_tools, run_tool) instead of every command\n"
           "  --all-tools   list every command as its own tool (the default up to 150 commands)\n";
}

bool Server::catalog() const {
    if (info_.mode == ToolMode::Auto) return session_.registry().size() > kCatalogThreshold;
    return info_.mode == ToolMode::Catalog;
}

std::string Server::catalogJson() {
    json tools = catalogTools();
    for (const auto& t : kNoArgsTools) tools.push_back(noArgsTool(t.name, t.description, t.readOnly));
    return tools.dump();
}

const std::string& Server::toolsJson() {
    const auto& reg = session_.registry();
    if (toolsRevision_ != reg.revision()) {
        tools_ = catalog() ? catalogJson() : toolsListJson(reg);
        toolsRevision_ = reg.revision();
    }
    return tools_;
}

std::string Server::commandFor(const std::string& tool) {
    const auto& reg = session_.registry();
    if (commandsRevision_ != reg.revision()) {
        commands_.clear();
        // in id order, so if two ids map to one tool name the later one wins, as before
        reg.forEach([&](const CommandInfo& info) { commands_[toolName(info.id)] = info.id; });
        commandsRevision_ = reg.revision();
    }
    auto it = commands_.find(tool);
    return it == commands_.end() ? std::string() : it->second;
}

Server::Server(Session& session, ServerInfo info) : session_(session), info_(std::move(info)) {}

Server::Server(Session& session, std::string name, std::string version)
    : Server(session, ServerInfo{std::move(name), std::move(version), {}}) {}

std::string Server::toolName(const std::string& commandId) {
    std::string out = commandId;
    for (auto& c : out) {
        if (c == '.') c = '_';
    }
    return out;
}

std::string Server::callTool(const std::string& tool, const std::string& argumentsJson) {
    const json arguments = json::parse(argumentsJson, nullptr, false);
    if (tool == "undo") {
        bool done = session_.history().undo();
        return textResult(done ? "undone" : "nothing to undo", !done).dump();
    }
    if (tool == "redo") {
        bool done = session_.history().redo();
        return textResult(done ? "redone" : "nothing to redo", !done).dump();
    }
    if (tool == "history") {
        std::string text;
        for (const auto& label : session_.history().labels()) text += label + "\n";
        return textResult(text.empty() ? "history is empty" : text, false).dump();
    }
    if (tool == "documents") {
        std::string text;
        json docs = json::array();
        for (const auto& docId : session_.workspace().ids()) {
            auto doc = session_.workspace().get(docId);
            text += docId + " (" + doc->kind() + "): " + doc->summary() + "\n";
            docs.push_back({{"id", docId}, {"kind", doc->kind()}, {"summary", doc->summary()}, {"path", doc->path}});
        }
        json out = textResult(text.empty() ? "no open documents" : text, false);
        out["structuredContent"] = {{"documents", docs}};
        return out.dump();
    }
    if (tool == kFindTools) {
        const std::string query = arguments.is_object() ? arguments.value("query", "") : "";
        std::int64_t limit = 20;
        if (arguments.is_object() && arguments.contains("limit") && arguments["limit"].is_number())
            limit = std::clamp<std::int64_t>(static_cast<std::int64_t>(arguments["limit"].get<double>()), 1, 200);
        const auto matches = findTools(session_.registry(), query);
        if (matches.empty()) {
            return textResult("no tools match '" + query + "'. Families: " + familySummary(session_.registry()), true).dump();
        }
        std::string text;
        json found = json::array();
        for (std::size_t i = 0; i < matches.size() && static_cast<std::int64_t>(i) < limit; ++i) {
            const CommandInfo& info = *matches[i].info;
            const std::string name = toolName(info.id), what = firstSentence(info.description);
            text += name + ": " + what + "\n";
            found.push_back({{"name", name}, {"title", info.title}, {"description", what}});
        }
        if (static_cast<std::int64_t>(matches.size()) > limit)
            text += "(" + std::to_string(matches.size() - static_cast<std::size_t>(limit)) + " more; raise limit= or narrow the words)\n";
        json out = textResult(text, false);
        out["structuredContent"] = {{"tools", found}, {"total", matches.size()}};
        return out.dump();
    }
    if (tool == kDescribeTools) {
        const std::string names = arguments.is_object() ? arguments.value("names", "") : "";
        json described = json::array();
        std::string missing;
        std::string item;
        for (char c : names + ",") {
            if (c != ',' && c != ' ') {
                item += c;
                continue;
            }
            if (item.empty()) continue;
            const std::string commandId = commandFor(item);
            const CommandInfo* info = commandId.empty() ? nullptr : session_.registry().find(commandId);
            if (info) described.push_back(describe(*info));
            else missing += (missing.empty() ? "" : ", ") + item;
            item.clear();
        }
        if (described.empty()) return textResult("no such tools: " + (missing.empty() ? std::string("(none given)") : missing), true).dump();
        std::string text = described.dump(1);
        if (!missing.empty()) text += "\nnot found: " + missing;
        json out = textResult(text, false);
        out["structuredContent"] = {{"tools", described}};
        return out.dump();
    }

    const std::string commandId = commandFor(tool);
    if (commandId.empty()) return {};

    Params args;
    std::string error;
    if (!toParams(arguments.is_discarded() ? json::object() : arguments, args, error)) return textResult(error, true).dump();
    // a number sent for a string param (e.g. a cad expression field) means its text
    if (const auto* info = session_.registry().find(commandId)) {
        for (const auto& spec : info->params) {
            auto it = args.find(spec.name);
            if (spec.type == ParamType::String && it != args.end() &&
                (std::holds_alternative<std::int64_t>(it->second) || std::holds_alternative<double>(it->second))) {
                std::string text = toText(it->second);
                if (auto d = std::get_if<double>(&it->second)) {
                    char buf[40];
                    std::snprintf(buf, sizeof buf, "%.15g", *d);
                    if (std::strtod(buf, nullptr) != *d) std::snprintf(buf, sizeof buf, "%.17g", *d);
                    text = buf;
                }
                it->second = text;
            }
        }
    }
    Result result;
    try {
        result = session_.run(commandId, args);
    } catch (const std::exception& e) {
        result = Result::failure(std::string("internal error: ") + e.what());
    }
    return toolResult(result).dump();
}

std::optional<std::string> Server::handle(const std::string& line) {
    json msg = json::parse(line, nullptr, false);
    if (msg.is_discarded() || !msg.is_object()) return rpcError(nullptr, -32700, "parse error").dump();

    const bool isRequest = msg.contains("id");
    const json id = isRequest ? msg["id"] : json(nullptr);
    if (!msg.contains("method") || !msg["method"].is_string()) {
        return isRequest ? std::optional(rpcError(id, -32600, "invalid request").dump()) : std::nullopt;
    }
    const std::string method = msg["method"];
    const json params = msg.value("params", json::object());

    if (!isRequest) return std::nullopt;  // notifications/initialized and friends need no answer

    if (method == "initialize") {
        std::string requested = params.value("protocolVersion", "");
        std::string version = kProtocolVersions.front();
        for (const char* v : kProtocolVersions) {
            if (requested == v) version = v;
        }
        json result = {{"protocolVersion", version},
                       {"capabilities",
                        {{"tools", {{"listChanged", false}}}, {"resources", {{"listChanged", false}}}}},
                       {"serverInfo", {{"name", info_.name}, {"version", info_.version}}}};
        std::string instructions = info_.instructions;
        if (catalog()) {
            instructions += std::string(instructions.empty() ? "" : "\n\n") + "This server has " +
                            std::to_string(session_.registry().size()) +
                            " tools, too many to list at once. Use find_tools to search them by job or family, "
                            "describe_tools for a tool's parameters, then run_tool (or call the tool by its name). "
                            "Families: " + familySummary(session_.registry()) + ".";
        }
        if (!instructions.empty()) result["instructions"] = instructions;
        return rpcResult(id, result).dump();
    }
    if (method == "ping") return rpcResult(id, json::object()).dump();

    if (method == "tools/list") {
        // {"id":..,"jsonrpc":"2.0","result":{"tools":[..]}}: what rpcResult(...).dump() gives, without
        // building ~700 tool objects as json values every time
        return "{\"id\":" + id.dump() + ",\"jsonrpc\":\"2.0\",\"result\":{\"tools\":" + toolsJson() + "}}";
    }

    if (method == "tools/call") {
        const std::string tool = params.value("name", "");
        const json arguments = params.value("arguments", json::object());
        if (tool == kRunTool) {
            const std::string inner = arguments.is_object() ? arguments.value("name", "") : "";
            if (inner.empty()) return rpcResult(id, textResult("run_tool needs name= (a tool from find_tools)", true)).dump();
            if (inner == kRunTool) return rpcResult(id, textResult("run_tool can't run itself", true)).dump();
            const json innerArgs = arguments.value("arguments", json::object());
            std::string r = callTool(inner, innerArgs.dump());
            if (r.empty()) return rpcResult(id, textResult("unknown tool: " + inner + " (find_tools lists them)", true)).dump();
            return "{\"id\":" + id.dump() + ",\"jsonrpc\":\"2.0\",\"result\":" + r + "}";
        }
        std::string r = callTool(tool, arguments.dump());
        if (r.empty()) return rpcError(id, -32602, "unknown tool: " + tool).dump();
        return "{\"id\":" + id.dump() + ",\"jsonrpc\":\"2.0\",\"result\":" + r + "}";
    }

    if (method == "resources/list") {
        json resources = json::array();
        for (const auto& docId : session_.workspace().ids()) {
            auto doc = session_.workspace().get(docId);
            resources.push_back({{"uri", kDocScheme + docId},
                                 {"name", docId},
                                 {"description", doc->kind() + ": " + doc->summary()},
                                 {"mimeType", "text/plain"}});
        }
        return rpcResult(id, {{"resources", resources}}).dump();
    }
    if (method == "resources/templates/list") {
        return rpcResult(id, {{"resourceTemplates",
                               json::array({{{"uriTemplate", std::string(kDocScheme) + "{id}"},
                                             {"name", "open document"},
                                             {"description", "summary and preview of an open document"}}})}})
            .dump();
    }
    if (method == "resources/read") {
        const std::string uri = params.value("uri", "");
        const std::string prefix = kDocScheme;
        auto doc = uri.rfind(prefix, 0) == 0 ? session_.workspace().get(uri.substr(prefix.size())) : nullptr;
        if (!doc) return rpcError(id, -32002, "resource not found: " + uri).dump();
        json contents = json::array();
        contents.push_back({{"uri", uri}, {"mimeType", "text/plain"}, {"text", doc->kind() + ": " + doc->summary()}});
        if (auto preview = doc->preview()) {
            contents.push_back({{"uri", uri + "/preview"}, {"mimeType", preview->mime}, {"blob", base64(preview->bytes)}});
        }
        return rpcResult(id, {{"contents", contents}}).dump();
    }

    return rpcError(id, -32601, "method not found: " + method).dump();
}

void Server::run(std::istream& in, std::ostream& out) {
    std::string line;
    while (std::getline(in, line)) {
        if (line.empty() || line == "\r") continue;
        if (auto response = handle(line)) out << *response << '\n' << std::flush;
    }
}

}  // namespace wetwaresuite::mcp
