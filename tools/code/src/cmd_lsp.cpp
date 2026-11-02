// Language servers (LSP): completion, hover, go to definition, references and problems
// from the server installed for a language (clangd, pyright, rust-analyzer, gopls,
// typescript-language-server, ...). The suite starts the server the first time a file of
// that language asks, keeps it running, and sends it the open file's text as it changes.
// Nothing is downloaded: with no server installed the commands say which one to get.
#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <set>
#include <nlohmann/json.hpp>
#include <sstream>

#include "internal.hpp"

namespace wetwaresuite::code::detail {

namespace {

using json = nlohmann::json;
using std::int64_t;

struct ServerChoice {
    std::string language;                   // our language id
    std::vector<std::vector<std::string>> commands;  // tried in order
    std::string get;                        // how to install one
};

const std::vector<ServerChoice>& serverTable() {
    static const std::vector<ServerChoice> t = {
        {"c", {{"clangd"}}, "clangd (LLVM; most package managers have it)"},
        {"cpp", {{"clangd"}}, "clangd (LLVM; most package managers have it)"},
        {"python", {{"pyright-langserver", "--stdio"}, {"pylsp"}, {"jedi-language-server"}}, "pyright (pip install pyright) or python-lsp-server (pip install python-lsp-server)"},
        {"rust", {{"rust-analyzer"}}, "rust-analyzer (rustup component add rust-analyzer)"},
        {"go", {{"gopls"}}, "gopls (go install golang.org/x/tools/gopls@latest)"},
        {"javascript", {{"typescript-language-server", "--stdio"}}, "typescript-language-server (npm install -g typescript typescript-language-server)"},
        {"typescript", {{"typescript-language-server", "--stdio"}}, "typescript-language-server (npm install -g typescript typescript-language-server)"},
        {"java", {{"jdtls"}}, "Eclipse JDT language server (jdtls)"},
        {"csharp", {{"csharp-ls"}, {"OmniSharp", "-lsp"}}, "csharp-ls (dotnet tool install -g csharp-ls)"},
        {"lua", {{"lua-language-server"}}, "lua-language-server"},
        {"zig", {{"zls"}}, "zls"},
        {"shell", {{"bash-language-server", "start"}}, "bash-language-server (npm install -g bash-language-server)"},
        {"html", {{"vscode-html-language-server", "--stdio"}}, "vscode-langservers-extracted (npm)"},
        {"css", {{"vscode-css-language-server", "--stdio"}}, "vscode-langservers-extracted (npm)"},
        {"json", {{"vscode-json-language-server", "--stdio"}}, "vscode-langservers-extracted (npm)"},
        {"haskell", {{"haskell-language-server-wrapper", "--lsp"}}, "haskell-language-server"},
        {"ruby", {{"solargraph", "stdio"}, {"ruby-lsp"}}, "solargraph (gem install solargraph)"},
        {"php", {{"intelephense", "--stdio"}}, "intelephense (npm install -g intelephense)"},
        {"kotlin", {{"kotlin-language-server"}}, "kotlin-language-server"},
        {"dart", {{"dart", "language-server"}}, "the Dart SDK"},
        {"elixir", {{"elixir-ls"}}, "elixir-ls"},
        {"yaml", {{"yaml-language-server", "--stdio"}}, "yaml-language-server (npm)"},
    };
    return t;
}

std::string upperId(const std::string& id) {
    std::string out;
    for (char c : id) out += static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return out;
}

// WETWARESUITE_LSP_<LANG>="program args" picks the server (e.g. WETWARESUITE_LSP_PYTHON="pylsp").
std::vector<std::string> serverFor(const std::string& language, std::string& get) {
    if (const char* forced = std::getenv(("WETWARESUITE_LSP_" + upperId(language)).c_str()); forced && *forced) {
        auto args = splitArgs(forced);
        if (!args.empty()) return args;
    }
    for (const auto& s : serverTable()) {
        if (s.language != language) continue;
        get = s.get;
        for (const auto& c : s.commands) {
            if (findProgram(c[0])) return c;
        }
    }
    return {};
}

std::string languageIdFor(const std::string& id) {
    if (id == "shell") return "shellscript";
    return id;
}

std::string uriFor(const fs::path& p) {
    std::string s = fs::absolute(p).lexically_normal().generic_string();
    std::string out = "file://";
    if (!s.empty() && s[0] != '/') out += '/';  // C:/... -> file:///C:/...
    for (unsigned char c : s) {
        if (std::isalnum(c) || c == '/' || c == '-' || c == '_' || c == '.' || c == '~' || c == ':') {
            out += static_cast<char>(c);
        } else {
            char hex[4];
            std::snprintf(hex, sizeof hex, "%%%02X", c);
            out += hex;
        }
    }
    return out;
}

fs::path pathFromUri(const std::string& uri) {
    if (uri.rfind("file://", 0) != 0) return {};
    std::string s = uri.substr(7), out;
    for (std::size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '%' && i + 2 < s.size()) {
            out += static_cast<char>(std::strtol(s.substr(i + 1, 2).c_str(), nullptr, 16));
            i += 2;
        } else {
            out += s[i];
        }
    }
    if (out.size() > 2 && out[0] == '/' && out[2] == ':') out.erase(0, 1);  // /C:/x -> C:/x
    return sysutil::toPath(out);
}

// LSP columns count UTF-16 units; ours count bytes.
int utf16Col(const std::string& line, int byteCol) {
    int u = 0;
    for (int i = 0; i < byteCol && i < static_cast<int>(line.size());) {
        const auto c = static_cast<unsigned char>(line[static_cast<std::size_t>(i)]);
        const int len = c < 0x80 ? 1 : c < 0xE0 ? 2 : c < 0xF0 ? 3 : 4;
        u += len == 4 ? 2 : 1;
        i += len;
    }
    return u;
}

int byteCol(const std::string& line, int u16) {
    int u = 0, i = 0;
    while (i < static_cast<int>(line.size()) && u < u16) {
        const auto c = static_cast<unsigned char>(line[static_cast<std::size_t>(i)]);
        const int len = c < 0x80 ? 1 : c < 0xE0 ? 2 : c < 0xF0 ? 3 : 4;
        u += len == 4 ? 2 : 1;
        i += len;
    }
    return std::min(i, static_cast<int>(line.size()));
}

std::string lineOf(const std::string& text, int line) {
    std::size_t start = 0;
    for (int i = 0; i < line; ++i) {
        start = text.find('\n', start);
        if (start == std::string::npos) return {};
        ++start;
    }
    const std::size_t end = text.find('\n', start);
    return text.substr(start, end == std::string::npos ? std::string::npos : end - start);
}

std::string fileLine(const fs::path& p, int line) {
    try {
        std::string t = sysutil::readFile(p);
        std::string l = lineOf(t, line);
        if (!l.empty() && l.back() == '\r') l.pop_back();
        return l;
    } catch (const std::exception&) {
        return {};
    }
}

class LspClient {
public:
    std::string language, program;
    fs::path root;
    std::map<std::string, json> diagnostics;  // uri -> the latest list
    std::map<std::string, int> diagnosticsArrived;  // uri -> how many times
    std::map<std::string, int> changedAt;           // uri -> diagnosticsArrived when its text last went out
    std::map<std::string, int> versions;
    std::map<std::string, std::string> sent;  // uri -> text the server has
    json capabilities;

    bool start(const std::vector<std::string>& cmd, const fs::path& folder, std::string& err) {
        root = folder;
        program = cmd[0];
        proc_.dropErrors(true);
        if (!proc_.start(cmd, folder, err)) return false;
        json init = {{"processId", nullptr},
                     {"rootUri", uriFor(folder)},
                     {"rootPath", sysutil::fromPath(folder)},
                     {"workspaceFolders", json::array({{{"uri", uriFor(folder)}, {"name", sysutil::nameOf(folder)}}})},
                     {"clientInfo", {{"name", "wetwaresuite"}}},
                     {"capabilities",
                      {{"textDocument",
                        {{"synchronization", {{"didSave", true}}},
                         {"completion", {{"completionItem", {{"snippetSupport", false}, {"documentationFormat", json::array({"plaintext", "markdown"})}}}}},
                         {"hover", {{"contentFormat", json::array({"plaintext", "markdown"})}}},
                         {"definition", json::object()},
                         {"references", json::object()},
                         {"publishDiagnostics", {{"relatedInformation", false}}}}},
                       {"workspace", {{"workspaceFolders", true}, {"configuration", true}}}}}};
        json r = request("initialize", init, 30, err);
        if (!err.empty()) {
            proc_.kill();
            err = program + " didn't start talking: " + err;
            return false;
        }
        capabilities = r.value("capabilities", json::object());
        notify("initialized", json::object());
        return true;
    }

    bool running() { return proc_.running(); }

    void stop() {
        if (!proc_.running()) return;
        std::string err;
        request("shutdown", nullptr, 2, err);
        notify("exit", nullptr);
        std::string out;
        if (!proc_.wait(1.0, out)) proc_.kill();
    }

    void notify(const std::string& method, const json& params) {
        json m = {{"jsonrpc", "2.0"}, {"method", method}};
        if (!params.is_null()) m["params"] = params;
        send(m);
    }

    json request(const std::string& method, const json& params, double timeout, std::string& err) {
        const int id = nextId_++;
        json m = {{"jsonrpc", "2.0"}, {"id", id}, {"method", method}, {"params", params}};
        send(m);
        const auto until = std::chrono::steady_clock::now() + std::chrono::duration<double>(timeout);
        while (true) {
            pump(0.05);
            if (auto it = responses_.find(id); it != responses_.end()) {
                json r = std::move(it->second);
                responses_.erase(it);
                if (r.contains("error")) {
                    err = r["error"].value("message", std::string("the server refused it"));
                    return nullptr;
                }
                return r.value("result", json());
            }
            if (!proc_.running()) {
                err = program + " stopped";
                return nullptr;
            }
            if (std::chrono::steady_clock::now() > until) {
                err = program + " took too long to answer (it may still be reading the project; try again in a moment)";
                notify("$/cancelRequest", {{"id", id}});
                return nullptr;
            }
        }
    }

    // Reads whatever the server sent: answers, diagnostics, and its own questions (answered empty).
    void pump(double seconds) {
        buf_ += seconds > 0 ? proc_.readFor(seconds) : proc_.read();
        while (true) {
            const std::size_t head = buf_.find("Content-Length:");
            if (head == std::string::npos) {
                if (buf_.size() > 64) buf_.erase(0, buf_.size() - 64);
                return;
            }
            if (head > 0) buf_.erase(0, head);
            const std::size_t end = buf_.find("\r\n\r\n");
            if (end == std::string::npos) return;
            const std::size_t length = static_cast<std::size_t>(std::strtoull(buf_.c_str() + 15, nullptr, 10));
            if (buf_.size() < end + 4 + length) return;
            const std::string body = buf_.substr(end + 4, length);
            buf_.erase(0, end + 4 + length);
            json m = json::parse(body, nullptr, false);
            if (m.is_discarded()) continue;
            handle(m);
        }
    }

    // The server gets the file's current text: opened the first time, then whole-text changes.
    void sync(const std::string& uri, const std::string& lang, const std::string& text) {
        auto it = sent.find(uri);
        if (it == sent.end()) {
            versions[uri] = 1;
            notify("textDocument/didOpen", {{"textDocument", {{"uri", uri}, {"languageId", languageIdFor(lang)}, {"version", 1}, {"text", text}}}});
        } else if (it->second != text) {
            const int v = ++versions[uri];
            notify("textDocument/didChange", {{"textDocument", {{"uri", uri}, {"version", v}}}, {"contentChanges", json::array({{{"text", text}}})}});
        } else {
            return;
        }
        sent[uri] = text;
        changedAt[uri] = diagnosticsArrived[uri];
    }

    bool freshDiagnostics(const std::string& uri) { return diagnosticsArrived[uri] > changedAt[uri]; }

private:
    void send(const json& m) {
        const std::string body = m.dump();
        proc_.write("Content-Length: " + std::to_string(body.size()) + "\r\n\r\n" + body);
    }

    void handle(const json& m) {
        if (m.contains("id") && !m.contains("method")) {
            if (m["id"].is_number_integer()) responses_[m["id"].get<int>()] = m;
            return;
        }
        const std::string method = m.value("method", std::string());
        if (method == "textDocument/publishDiagnostics" && m.contains("params")) {
            const std::string uri = m["params"].value("uri", std::string());
            diagnostics[uri] = m["params"].value("diagnostics", json::array());
            ++diagnosticsArrived[uri];
            return;
        }
        if (m.contains("id")) {
            // the server asks something (configuration, progress tokens, registering features): say ok
            json result = nullptr;
            if (method == "workspace/configuration" && m.contains("params") && m["params"].contains("items")) {
                result = json::array();
                for (std::size_t i = 0; i < m["params"]["items"].size(); ++i) result.push_back(nullptr);
            } else if (method == "workspace/workspaceFolders") {
                result = json::array({{{"uri", uriFor(root)}, {"name", sysutil::nameOf(root)}}});
            }
            send({{"jsonrpc", "2.0"}, {"id", m["id"]}, {"result", result}});
        }
    }

    ChildProcess proc_;
    std::string buf_;
    int nextId_ = 1;
    std::map<int, json> responses_;
};

struct LspState {
    std::mutex lock;
    std::map<std::string, std::unique_ptr<LspClient>> clients;  // language + "|" + root
};

LspState& lsp() {
    static auto* s = new LspState;  // servers outlive any one session; ended with code.lsp action=stop or at exit
    return *s;
}

// The folder the server should treat as the project: the open project if the file is in it,
// else the nearest folder above with a .git, compile_commands.json, Cargo.toml, go.mod,
// package.json or pyproject.toml, else the file's own folder.
fs::path rootFor(const CodeState& st, const fs::path& file) {
    std::error_code ec;
    const fs::path abs = fs::absolute(file, ec).lexically_normal();
    if (!st.project.empty()) {
        const auto rel = abs.lexically_relative(fs::absolute(st.project, ec).lexically_normal());
        if (!rel.empty() && rel.native()[0] != '.') return st.project;
    }
    for (fs::path d = abs.parent_path(); !d.empty(); d = d.parent_path()) {
        for (const char* mark : {".git", "compile_commands.json", "Cargo.toml", "go.mod", "package.json", "pyproject.toml", "setup.py", "CMakeLists.txt"}) {
            if (fs::exists(d / mark, ec)) return d;
        }
        if (d == d.parent_path()) break;
    }
    return abs.parent_path();
}

struct Opened {
    LspClient* client = nullptr;
    std::string uri;
};

// Starts (or reuses) the server for this file and gives it the current text. Call with the lock held.
Opened openFor(CodeState& st, CodeDoc& doc) {
    if (doc.path.empty()) throw std::runtime_error("save the file first: language servers read files from disk");
    std::string get;
    const auto cmd = serverFor(doc.language, get);
    if (cmd.empty()) {
        if (get.empty()) throw std::runtime_error("no language server is known for " + doc.language + " files");
        throw std::runtime_error("no language server for " + doc.language + " is installed; get " + get +
                                 ", or name one with WETWARESUITE_LSP_" + upperId(doc.language) + "=\"program args\"");
    }
    const fs::path file = sysutil::toPath(doc.path);
    const fs::path root = rootFor(st, file);
    const std::string key = doc.language + "|" + sysutil::fromPath(root);
    auto& slot = lsp().clients[key];
    if (slot && !slot->running()) slot.reset();
    if (!slot) {
        auto c = std::make_unique<LspClient>();
        c->language = doc.language;
        std::string err;
        if (!c->start(cmd, root, err)) throw std::runtime_error(err);
        slot = std::move(c);
    }
    Opened o{slot.get(), uriFor(file)};
    o.client->sync(o.uri, doc.language, doc.text);
    o.client->pump(0);
    return o;
}

struct Spot {
    int line = 0;  // 0-based
    int col = 0;   // bytes
};

Spot spotParam(const CodeDoc& doc, const Params& p) {
    Spot s;
    s.line = static_cast<int>(intParam(p, "line", 1)) - 1;
    const std::string l = lineOf(doc.text, s.line);
    s.col = std::clamp(static_cast<int>(intParam(p, "col", static_cast<int64_t>(l.size()) + 1)) - 1, 0, static_cast<int>(l.size()));
    return s;
}

json position(const CodeDoc& doc, const Spot& s, const std::string& uri) {
    return {{"textDocument", {{"uri", uri}}}, {"position", {{"line", s.line}, {"character", utf16Col(lineOf(doc.text, s.line), s.col)}}}};
}

std::vector<ParamSpec> spotParams() {
    return {docParam(), opt("line", ParamType::Int, "line, 1 = the first", int64_t{1}), opt("col", ParamType::Int, "column in bytes, 1 = before the first character (default: the line's end)")};
}

std::string plainText(const json& v) {
    if (v.is_string()) return v.get<std::string>();
    if (v.is_object()) return v.value("value", std::string());
    if (v.is_array()) {
        std::string out;
        for (const auto& x : v) {
            const std::string t = plainText(x);
            if (!t.empty()) out += (out.empty() ? "" : "\n\n") + t;
        }
        return out;
    }
    return {};
}

// location(s) -> rows: path, line (1-based), col (1-based bytes), the line's text
std::string locationRows(const json& r, int& count, std::string& first) {
    std::vector<json> locs;
    if (r.is_array()) {
        for (const auto& x : r) locs.push_back(x);
    } else if (r.is_object()) {
        locs.push_back(r);
    }
    std::string rows;
    count = 0;
    for (const auto& l : locs) {
        const std::string uri = l.contains("targetUri") ? l.value("targetUri", std::string()) : l.value("uri", std::string());
        const json range = l.contains("targetSelectionRange") ? l["targetSelectionRange"] : l.value("range", json::object());
        const fs::path path = pathFromUri(uri);
        if (path.empty()) continue;
        const int line = range.contains("start") ? range["start"].value("line", 0) : 0;
        const std::string text = fileLine(path, line);
        const int col = byteCol(text, range.contains("start") ? range["start"].value("character", 0) : 0);
        rows += sysutil::fromPath(path) + "\t" + std::to_string(line + 1) + "\t" + std::to_string(col + 1) + "\t" + trim(text) + "\n";
        if (count == 0) first = sysutil::fromPath(path) + ":" + std::to_string(line + 1) + ":" + std::to_string(col + 1);
        ++count;
    }
    return rows;
}

const char* kindName(int k) {
    static const char* names[] = {"",          "text",     "method",   "function", "constructor", "field",    "variable", "class",     "interface",
                                  "module",    "property", "unit",     "value",    "enum",        "keyword",  "snippet",  "color",     "file",
                                  "reference", "folder",   "enum member", "constant", "struct",  "event",    "operator", "type parameter"};
    return k >= 0 && k <= 25 ? names[k] : "";
}

void registerLsp(Session& s, StatePtr st) {
    auto& reg = s.registry();

    reg.add({"code.lsp", "Language servers",
             "Which language server each language uses, which are installed and which are running. action=stop ends "
             "the running ones (they start again when needed). Pick a server with WETWARESUITE_LSP_<LANGUAGE>=\"program args\".",
             {opt("action", ParamType::String, "status or stop", std::string("status"))}, false, true},
            reader([](const Params& p) {
                std::lock_guard<std::mutex> g(lsp().lock);
                if (stringParam(p, "action", "status") == "stop") {
                    const auto n = lsp().clients.size();
                    for (auto& [k, c] : lsp().clients) c->stop();
                    lsp().clients.clear();
                    return Result::success("stopped " + std::to_string(n) + " language server(s)");
                }
                std::string text, rows;
                int64_t installed = 0;
                std::set<std::string> seen;
                for (const auto& sc : serverTable()) {
                    if (!seen.insert(sc.language).second) continue;
                    std::string get;
                    const auto cmd = serverFor(sc.language, get);
                    int running = 0;
                    for (auto& [k, c] : lsp().clients) running += c->language == sc.language && c->running() ? 1 : 0;
                    installed += cmd.empty() ? 0 : 1;
                    const std::string state = cmd.empty() ? "not installed" : running ? "running" : "installed";
                    text += "  " + sc.language + ": " + (cmd.empty() ? "not installed (get " + get + ")" : cmd[0] + (running ? " (running)" : "")) + "\n";
                    rows += sc.language + "\t" + (cmd.empty() ? std::string() : cmd[0]) + "\t" + state + "\n";
                }
                return Result::success(std::to_string(installed) + " language server(s) installed\n" + text).with("installed", installed).with("rows", rows);
            }));

    reg.add({"code.complete", "Complete",
             "What can go at a spot in an open file, from its language server: rows of label, kind, detail and the "
             "text to insert. from_col is where the word being typed starts (replace from there to col).",
             [] {
                 auto v = spotParams();
                 v.push_back(opt("limit", ParamType::Int, "at most this many", int64_t{100}));
                 return v;
             }(),
             false, true},
            docReader(s, [st](CodeDoc& doc, const Params& p) {
                std::lock_guard<std::mutex> g(lsp().lock);
                const Opened o = openFor(*st, doc);
                const Spot sp = spotParam(doc, p);
                std::string err;
                json r = o.client->request("textDocument/completion", position(doc, sp, o.uri), 15, err);
                if (!err.empty()) throw std::runtime_error(err);
                const json items = r.is_array() ? r : r.value("items", json::array());
                // the word being typed: letters, digits and _ before the spot
                const std::string line = lineOf(doc.text, sp.line);
                int from = sp.col;
                while (from > 0 && (std::isalnum(static_cast<unsigned char>(line[static_cast<std::size_t>(from - 1)])) || line[static_cast<std::size_t>(from - 1)] == '_')) --from;
                const std::string typed = line.substr(static_cast<std::size_t>(from), static_cast<std::size_t>(sp.col - from));
                struct Item {
                    std::string label, kind, detail, insert, sort;
                };
                std::vector<Item> list;
                for (const auto& it : items) {
                    Item i;
                    i.label = trim(it.value("label", std::string()));
                    i.kind = kindName(it.value("kind", 0));
                    i.detail = trim(it.value("detail", std::string()));
                    i.insert = it.value("insertText", i.label);
                    if (it.contains("textEdit") && it["textEdit"].is_object()) i.insert = it["textEdit"].value("newText", i.insert);
                    i.sort = it.value("sortText", i.label);
                    if (!typed.empty()) {
                        // servers send everything that could fit; keep what starts like the typed word (any case)
                        std::string a = it.value("filterText", i.label), b = typed;
                        for (auto& c : a) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                        for (auto& c : b) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                        if (a.rfind(b, 0) != 0) continue;
                    }
                    for (auto* f : {&i.label, &i.detail, &i.insert}) {
                        for (auto& c : *f) {
                            if (c == '\t' || c == '\n' || c == '\r') c = ' ';
                        }
                    }
                    list.push_back(std::move(i));
                }
                std::stable_sort(list.begin(), list.end(), [](const Item& a, const Item& b) { return a.sort < b.sort; });
                const auto limit = static_cast<std::size_t>(std::clamp<int64_t>(intParam(p, "limit", 100), 1, 2000));
                std::string text, rows;
                for (std::size_t i = 0; i < list.size() && i < limit; ++i) {
                    rows += list[i].label + "\t" + list[i].kind + "\t" + list[i].detail + "\t" + list[i].insert + "\n";
                    if (i < 20) text += "\n  " + list[i].label + (list[i].kind.empty() ? "" : "  (" + list[i].kind + ")") + (list[i].detail.empty() ? "" : "  " + list[i].detail);
                }
                Result res = Result::success(std::to_string(list.size()) + " completion(s)" + text);
                return res.with("count", static_cast<int64_t>(list.size())).with("rows", rows).with("from_col", static_cast<int64_t>(from + 1)).with("server", o.client->program);
            }));

    reg.add({"code.hover", "Hover info", "The language server's note on the symbol at a spot: its type, signature and documentation.", spotParams(), false, true},
            docReader(s, [st](CodeDoc& doc, const Params& p) {
                std::lock_guard<std::mutex> g(lsp().lock);
                const Opened o = openFor(*st, doc);
                std::string err;
                json r = o.client->request("textDocument/hover", position(doc, spotParam(doc, p), o.uri), 10, err);
                if (!err.empty()) throw std::runtime_error(err);
                const std::string text = r.is_object() ? trim(plainText(r.value("contents", json()))) : std::string();
                if (text.empty()) return Result::success("nothing known about that spot").with("found", false);
                return Result::success(text).with("found", true).with("text", text);
            }));

    reg.add({"code.definition", "Go to definition",
             "Where the symbol at a spot is defined, from the language server: path, line and col (rows when there are several).", spotParams(), false, true},
            docReader(s, [st](CodeDoc& doc, const Params& p) {
                std::lock_guard<std::mutex> g(lsp().lock);
                const Opened o = openFor(*st, doc);
                std::string err;
                json r = o.client->request("textDocument/definition", position(doc, spotParam(doc, p), o.uri), 15, err);
                if (!err.empty()) throw std::runtime_error(err);
                int count = 0;
                std::string first;
                const std::string rows = locationRows(r, count, first);
                if (!count) return Result::success("no definition found there").with("count", int64_t{0});
                std::istringstream row(rows.substr(0, rows.find('\n')));
                std::string path, line, col;
                std::getline(row, path, '\t');
                std::getline(row, line, '\t');
                std::getline(row, col, '\t');
                return Result::success("defined at " + first + (count > 1 ? " (and " + std::to_string(count - 1) + " more)" : ""))
                    .with("count", static_cast<int64_t>(count))
                    .with("path", path)
                    .with("line", static_cast<int64_t>(std::atoll(line.c_str())))
                    .with("col", static_cast<int64_t>(std::atoll(col.c_str())))
                    .with("rows", rows);
            }));

    reg.add({"code.references", "Find references", "Every place the symbol at a spot is used, from the language server: rows of path, line, col and the line's text.",
             spotParams(), false, true},
            docReader(s, [st](CodeDoc& doc, const Params& p) {
                std::lock_guard<std::mutex> g(lsp().lock);
                const Opened o = openFor(*st, doc);
                std::string err;
                json params = position(doc, spotParam(doc, p), o.uri);
                params["context"] = {{"includeDeclaration", true}};
                json r = o.client->request("textDocument/references", params, 20, err);
                if (!err.empty()) throw std::runtime_error(err);
                int count = 0;
                std::string first;
                const std::string rows = locationRows(r, count, first);
                std::string text = std::to_string(count) + " reference(s)";
                std::istringstream in(rows);
                std::string l;
                for (int i = 0; i < 30 && std::getline(in, l); ++i) {
                    std::replace(l.begin(), l.end(), '\t', ' ');
                    text += "\n  " + l;
                }
                return Result::success(text).with("count", static_cast<int64_t>(count)).with("rows", rows);
            }));

    reg.add({"code.diagnostics", "Problems (language server)",
             "Errors and warnings the language server finds in an open file as it is now, without building: rows of "
             "line, col, severity (error, warning, info, hint) and message. wait= seconds for the server to look again "
             "after a change.",
             {docParam(), opt("wait", ParamType::Number, "seconds to wait for fresh results", 3.0)}, false, true},
            docReader(s, [st](CodeDoc& doc, const Params& p) {
                std::lock_guard<std::mutex> g(lsp().lock);
                const Opened o = openFor(*st, doc);
                const double wait = std::clamp(numberParam(p, "wait", 3.0), 0.0, 60.0);
                const auto until = std::chrono::steady_clock::now() + std::chrono::duration<double>(wait);
                // fresh: the server has sent results since it got this text
                while (!o.client->freshDiagnostics(o.uri) && std::chrono::steady_clock::now() < until) o.client->pump(0.05);
                const bool fresh = o.client->freshDiagnostics(o.uri);
                const json list = o.client->diagnostics.count(o.uri) ? o.client->diagnostics[o.uri] : json::array();
                static const char* sev[] = {"", "error", "warning", "info", "hint"};
                std::string rows, text;
                int64_t errors = 0, warnings = 0;
                for (const auto& d : list) {
                    const int line = d.contains("range") ? d["range"]["start"].value("line", 0) : 0;
                    const int col = byteCol(lineOf(doc.text, line), d.contains("range") ? d["range"]["start"].value("character", 0) : 0);
                    const int sv = std::clamp(d.value("severity", 1), 1, 4);
                    std::string msg = d.value("message", std::string());
                    for (auto& c : msg) {
                        if (c == '\n' || c == '\t' || c == '\r') c = ' ';
                    }
                    errors += sv == 1 ? 1 : 0;
                    warnings += sv == 2 ? 1 : 0;
                    rows += std::to_string(line + 1) + "\t" + std::to_string(col + 1) + "\t" + sev[sv] + "\t" + msg + "\n";
                    text += "\n  " + std::to_string(line + 1) + ":" + std::to_string(col + 1) + " " + sev[sv] + ": " + msg;
                }
                return Result::success((list.empty() ? std::string("no problems found") : std::to_string(list.size()) + " problem(s)") + text)
                    .with("count", static_cast<int64_t>(list.size()))
                    .with("errors", errors)
                    .with("warnings", warnings)
                    .with("rows", rows)
                    .with("fresh", fresh)
                    .with("server", o.client->program);
            }));
}

}  // namespace

void registerLspCommands(Session& s, StatePtr st) { registerLsp(s, st); }

}  // namespace wetwaresuite::code::detail
