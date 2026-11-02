// A tiny language server for the tests: answers initialize, completion, hover, definition,
// references and shutdown, and reports a warning on every line with TODO in it.
#include <iostream>
#include <nlohmann/json.hpp>
#include <string>

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#endif

using json = nlohmann::json;

namespace {

bool readMessage(json& out) {
    std::string line;
    std::size_t length = 0;
    while (std::getline(std::cin, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty()) break;
        if (line.rfind("Content-Length:", 0) == 0) length = std::stoul(line.substr(15));
    }
    if (!std::cin || length == 0) return false;
    std::string body(length, '\0');
    std::cin.read(body.data(), static_cast<std::streamsize>(length));
    out = json::parse(body, nullptr, false);
    return !out.is_discarded();
}

void send(const json& m) {
    const std::string body = m.dump();
    std::cout << "Content-Length: " << body.size() << "\r\n\r\n" << body << std::flush;
}

void diagnose(const std::string& uri, const std::string& text) {
    json list = json::array();
    std::size_t start = 0;
    for (int line = 0;; ++line) {
        const std::size_t nl = text.find('\n', start);
        const std::string l = text.substr(start, nl == std::string::npos ? std::string::npos : nl - start);
        if (auto at = l.find("TODO"); at != std::string::npos)
            list.push_back({{"range", {{"start", {{"line", line}, {"character", at}}}, {"end", {{"line", line}, {"character", at + 4}}}}}, {"severity", 2}, {"message", "something left to do"}});
        if (nl == std::string::npos) break;
        start = nl + 1;
    }
    send({{"jsonrpc", "2.0"}, {"method", "textDocument/publishDiagnostics"}, {"params", {{"uri", uri}, {"diagnostics", list}}}});
}

}  // namespace

int main() {
#ifdef _WIN32
    _setmode(_fileno(stdin), _O_BINARY);
    _setmode(_fileno(stdout), _O_BINARY);
#endif
    std::cerr << "fake server starting (this goes to stderr and must not reach the client)\n";
    json m;
    while (readMessage(m)) {
        const std::string method = m.value("method", std::string());
        const bool isRequest = m.contains("id");
        json result = nullptr;
        if (method == "initialize") {
            result = {{"capabilities", {{"completionProvider", json::object()}, {"hoverProvider", true}}}};
            // ask the client something, as real servers do
            send({{"jsonrpc", "2.0"}, {"id", "cfg1"}, {"method", "workspace/configuration"}, {"params", {{"items", json::array({json::object()})}}}});
        } else if (method == "textDocument/didOpen") {
            diagnose(m["params"]["textDocument"]["uri"], m["params"]["textDocument"]["text"]);
        } else if (method == "textDocument/didChange") {
            diagnose(m["params"]["textDocument"]["uri"], m["params"]["contentChanges"][0]["text"]);
        } else if (method == "textDocument/completion") {
            result = {{"isIncomplete", false},
                      {"items", json::array({{{"label", "print"}, {"kind", 3}, {"detail", "builtin"}, {"sortText", "b"}},
                                             {{"label", "printf"}, {"kind", 3}, {"detail", "C style"}, {"sortText", "a"}},
                                             {{"label", "zebra"}, {"kind", 6}}})}};
        } else if (method == "textDocument/hover") {
            const auto& pos = m["params"]["position"];
            result = {{"contents", {{"kind", "plaintext"}, {"value", "hover at " + std::to_string(pos["line"].get<int>()) + ":" + std::to_string(pos["character"].get<int>())}}}};
        } else if (method == "textDocument/definition") {
            result = {{"uri", m["params"]["textDocument"]["uri"]}, {"range", {{"start", {{"line", 0}, {"character", 4}}}, {"end", {{"line", 0}, {"character", 7}}}}}};
        } else if (method == "textDocument/references") {
            const json uri = m["params"]["textDocument"]["uri"];
            result = json::array({{{"uri", uri}, {"range", {{"start", {{"line", 0}, {"character", 4}}}, {"end", {{"line", 0}, {"character", 7}}}}}},
                                  {{"uri", uri}, {"range", {{"start", {{"line", 2}, {"character", 0}}}, {"end", {{"line", 2}, {"character", 3}}}}}}});
        } else if (method == "exit") {
            return 0;
        }
        if (isRequest && !method.empty()) send({{"jsonrpc", "2.0"}, {"id", m["id"]}, {"result", result}});
    }
    return 0;
}
