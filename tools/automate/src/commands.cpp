// automate.* commands: run_script, pipeline, watch (foreground), watch_start /
// watch_stop / watch_status (background threads), list_scripts.
#include <algorithm>
#include <atomic>
#include <chrono>
#include <deque>
#include <fstream>
#include <iostream>
#include <memory>
#include <mutex>
#include <set>
#include <sstream>
#include <thread>

#include <nlohmann/json.hpp>

#include "wetwaresuite/automate/automate.hpp"
#include "wetwaresuite/core/cli.hpp"

namespace wetwaresuite::automate {

using nlohmann::json;

namespace {

constexpr int kMaxDepth = 8;
thread_local int tDepth = 0;  // run_script/pipeline nesting, so a script can't call itself forever

struct DepthGuard {
    DepthGuard() { ++tDepth; }
    ~DepthGuard() { --tDepth; }
};

std::string trim(const std::string& s) {
    auto a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return {};
    auto b = s.find_last_not_of(" \t\r\n");
    return s.substr(a, b - a + 1);
}

std::string lowerExt(const fs::path& p) {
    std::string e = p.extension().string();
    if (!e.empty() && e[0] == '.') e.erase(0, 1);
    std::transform(e.begin(), e.end(), e.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return e;
}

std::string readFile(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    if (!in) throw ParamError("cannot read " + p.string());
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

std::string today(bool withTime) {
    auto t = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
    std::tm tm{};
#ifdef _WIN32
    localtime_s(&tm, &t);
#else
    localtime_r(&t, &tm);
#endif
    char buf[32];
    std::strftime(buf, sizeof buf, withTime ? "%H-%M-%S" : "%Y-%m-%d", &tm);
    return buf;
}

// Variables every script gets unless it's given its own.
void addBuiltins(Vars& vars, const fs::path& scriptPath) {
    std::error_code ec;
    vars.emplace("cwd", fs::current_path(ec).generic_string());
    vars.emplace("date", today(false));
    vars.emplace("time", today(true));
    if (!scriptPath.empty()) {
        fs::path dir = fs::absolute(scriptPath, ec).parent_path();
        vars.emplace("script_dir", dir.generic_string());
    }
}

// The file variables: $file plus .name .stem .ext .dir (and .rel when a base is known).
void addFileVars(Vars& vars, const std::string& var, const fs::path& file, const fs::path& base = {}) {
    vars[var] = file.generic_string();
    vars[var + ".name"] = file.filename().string();
    vars[var + ".stem"] = file.stem().string();
    vars[var + ".ext"] = lowerExt(file);
    std::string dir = file.parent_path().generic_string();
    vars[var + ".dir"] = dir.empty() ? "." : dir;
    if (!base.empty()) vars[var + ".rel"] = file.lexically_relative(base).generic_string();
    if (var != "file") return;
    // short names for the common case
    vars["name"] = vars[var + ".name"];
    vars["stem"] = vars[var + ".stem"];
    vars["ext"] = vars[var + ".ext"];
    vars["dir"] = vars[var + ".dir"];
}

// file=photo.png given by hand: fill in $stem, $name, $file.stem ... without overriding what was given.
void deriveFileVars(Vars& vars) {
    auto it = vars.find("file");
    if (it == vars.end() || it->second.empty() || vars.count("file.stem")) return;
    Vars derived;
    addFileVars(derived, "file", fs::path(it->second));
    for (auto& [k, v] : derived) vars.emplace(k, v);
}

// $names a script uses, and the names it captures itself with "-> name".
void scanVars(const std::string& text, std::set<std::string>& used, std::set<std::string>& captured) {
    std::istringstream in(text);
    for (std::string line; std::getline(in, line);) {
        std::string t = trim(line);
        if (t.empty() || t[0] == '#') continue;
        for (std::size_t i = 0; i < t.size(); ++i) {
            if (t[i] == '\\') {
                ++i;
                continue;
            }
            if (t[i] != '$' || i + 1 >= t.size()) continue;
            std::size_t j = i + 1;
            bool brace = t[j] == '{';
            if (brace) ++j;
            std::size_t k = j;
            while (k < t.size() && (std::isalnum(static_cast<unsigned char>(t[k])) || t[k] == '_')) ++k;
            if (k > j) used.insert(t.substr(j, k - j));
            i = k - 1;
        }
        auto arrow = t.rfind("->");
        if (arrow != std::string::npos) {
            std::string name = trim(t.substr(arrow + 2));
            if (!name.empty() && name.find(' ') == std::string::npos) captured.insert(name);
        }
    }
}

struct Run {
    int ran = 0;
    int failed = 0;
    std::string log;
    std::vector<Attachment> attachments;
    bool stopped = false;
    bool verbose = false;  // false: the first line of each command's output

    void record(const Result& r, const std::string& what) {
        if (r.ok) {
            std::string text = cli::format(r);
            if (!verbose) {
                auto nl = text.find('\n');
                if (nl != std::string::npos) text = text.substr(0, nl) + " ...";
            }
            if (!text.empty()) log += text + "\n";
            for (const auto& a : r.attachments) {
                if (attachments.size() < 8) attachments.push_back(a);
            }
        } else {
            ++failed;
            log += what + ": " + r.message + "\n";
        }
    }
};

bool isCommandLine(const std::string& line) {
    std::string t = trim(line);
    return !t.empty() && t[0] != '#';
}

// Runs one batch line (already substituted) through the batch runner.
Result runLine(cli::Batch& batch, const std::string& line) {
    try {
        return batch.runLine(line);
    } catch (const std::exception& e) {
        return Result::failure(e.what());
    }
}

void runScriptText(Session& session, const std::string& text, Vars vars, bool keepGoing, bool dry, Run& run,
                   const std::string& label) {
    cli::Batch batch(session);
    std::istringstream in(text);
    int number = 0;
    for (std::string raw; std::getline(in, raw);) {
        ++number;
        if (!raw.empty() && raw.back() == '\r') raw.pop_back();
        if (!isCommandLine(raw)) continue;
        std::string line = substitute(raw, vars, true);
        if (dry) {
            run.log += "would run: " + trim(line) + "\n";
            ++run.ran;
            continue;
        }
        Result r = runLine(batch, line);
        ++run.ran;
        run.record(r, label + " line " + std::to_string(number));
        if (!r.ok && !keepGoing) {
            run.stopped = true;
            return;
        }
    }
}

// Missing $vars: used, not given, not captured inside, not built in.
std::vector<std::string> missingVars(const std::string& text, const Vars& vars) {
    std::set<std::string> used, captured;
    scanVars(text, used, captured);
    std::vector<std::string> missing;
    for (const auto& u : used) {
        if (!vars.count(u) && !captured.count(u)) missing.push_back(u);
    }
    return missing;
}

std::string plural(std::size_t n, const std::string& one, const std::string& many) {
    return std::to_string(n) + " " + (n == 1 ? one : many);
}

std::string joinNames(const std::vector<std::string>& v) {
    std::string out;
    for (const auto& s : v) out += (out.empty() ? "" : ", ") + s;
    return out;
}

Result finish(const std::string& what, Run& run, bool dry) {
    std::string head = std::string(dry ? "dry run of " : "ran ") + what + ": " + plural(static_cast<std::size_t>(run.ran), "command", "commands") + ", " +
                       std::to_string(run.failed) + " failed" + (run.stopped ? " (stopped at the first failure)" : "");
    std::string log = trim(run.log);
    std::string text = head + (log.empty() ? "" : "\n" + log);
    Result r = run.failed && !dry ? Result::failure(text) : Result::success(text);
    r.with("ran", static_cast<std::int64_t>(run.ran)).with("failed", static_cast<std::int64_t>(run.failed));
    r.attachments = run.attachments;
    return r;
}

// ---- pipelines ----------------------------------------------------------------

struct Pipe {
    Session& session;
    cli::Batch batch;
    Vars vars;
    bool dry = false;
    bool keepGoing = false;
    fs::path base;  // empty: relative globs, folders and scripts mean the current folder
    Run run;
    std::string currentFileVar = "file";

    Pipe(Session& s) : session(s), batch(s) {}
};

std::string asText(const json& v) {
    if (v.is_string()) return v.get<std::string>();
    if (v.is_null()) return "";
    return v.dump();
}

void capture(Pipe& p, const std::string& name, const Result& r) {
    const Value* doc = r.field("doc");
    std::string text = doc ? toText(*doc) : toText(r.value);
    if (text.empty() && !r.data.empty()) text = toText(r.data.front().second);
    p.vars[name] = text;
    p.vars[name + ".message"] = r.message;
    p.vars[name + ".ok"] = r.ok ? "true" : "false";
    for (const auto& [k, v] : r.data) p.vars[name + "." + k] = toText(v);
}

bool runSteps(Pipe& p, const json& steps, const std::string& where);

const std::set<std::string>& stepKeys() {
    static const std::set<std::string> keys = {"run", "command", "args", "capture", "as", "for_each", "for", "steps", "do", "if",
                                               "unless", "ext", "set", "mkdir", "echo", "script", "pipeline", "vars",
                                               "on_error", "name", "keep_going"};
    return keys;
}

bool runCommandStep(Pipe& p, const json& step, const std::string& where) {
    Result r;
    std::string shown;
    if (step.contains("run")) {
        shown = substitute(asText(step["run"]), p.vars, true);
        if (p.dry) {
            p.run.log += "would run: " + shown + "\n";
        } else {
            r = runLine(p.batch, shown);
        }
    } else {
        const std::string id = substitute(asText(step["command"]), p.vars);
        const CommandInfo* info = p.session.registry().find(id);
        if (!info) {
            r = Result::failure("unknown command: " + id);
        } else {
            std::vector<std::string> tokens;
            if (step.contains("args")) {
                if (!step["args"].is_object()) throw ParamError(where + ": args must be a mapping of parameter: value");
                for (const auto& [k, v] : step["args"].items()) {
                    std::string text = v.is_array() ? [&] {
                        std::string s;
                        for (const auto& e : v) s += (s.empty() ? "" : ",") + asText(e);
                        return s;
                    }()
                                                    : asText(v);
                    tokens.push_back(k + "=" + substitute(text, p.vars));
                }
            }
            shown = id;
            for (const auto& t : tokens) shown += " " + t;
            if (p.dry) {
                p.run.log += "would run: " + shown + "\n";
            } else {
                try {
                    r = p.session.run(id, cli::parseArgs(*info, tokens));
                } catch (const ParamError& e) {
                    r = Result::failure(e.what());
                }
            }
        }
    }
    ++p.run.ran;
    std::string name = step.contains("capture") ? asText(step["capture"]) : step.contains("as") && !step.contains("for_each") && !step.contains("for") ? asText(step["as"]) : "";
    if (p.dry) {
        if (!name.empty()) p.vars[name] = "<" + name + ">";
        return true;
    }
    p.run.record(r, where + " (" + shown + ")");
    if (!name.empty()) capture(p, name, r);
    if (!r.ok) {
        const bool cont = (step.contains("on_error") && asText(step["on_error"]) == "continue") || p.keepGoing;
        if (!cont) p.run.stopped = true;
        return cont;
    }
    return true;
}

bool runStep(Pipe& p, const json& step, const std::string& where) {
    if (p.run.stopped) return false;
    if (step.is_string()) return runCommandStep(p, json{{"run", step}}, where);
    if (!step.is_object()) throw ParamError(where + ": a step is a command line or a mapping");
    for (const auto& [k, v] : step.items()) {
        if (!stepKeys().count(k)) throw ParamError(where + ": unknown key '" + k + "'");
    }
    // on a for_each step, if/unless/ext filter each file; elsewhere they guard the step
    // in a dry run a condition on a capture (which doesn't exist yet) is assumed true
    auto check = [&](const std::string& condition, bool expected) {
        try {
            return evalCondition(condition, p.vars) == expected;
        } catch (const ParamError&) {
            if (!p.dry) throw;
            p.run.log += "would check: " + std::string(expected ? "" : "not ") + substitute(condition, p.vars) + "\n";
            return true;
        }
    };
    auto passes = [&](const std::string& fileVar) {
        if (step.contains("if") && !check(asText(step["if"]), true)) return false;
        if (step.contains("unless") && !check(asText(step["unless"]), false)) return false;
        if (step.contains("ext")) {
            std::string list = asText(step["ext"]);
            if (step["ext"].is_array()) {
                list.clear();
                for (const auto& e : step["ext"]) list += (list.empty() ? "" : ",") + asText(e);
            }
            Vars v = p.vars;
            v["file.ext"] = p.vars.count(fileVar + ".ext") ? p.vars[fileVar + ".ext"] : "";
            if (!evalCondition("ext in " + list, v)) return false;
        }
        return true;
    };
    const bool loop = step.contains("for_each") || step.contains("for");
    if (!loop && !passes(p.currentFileVar)) return true;
    if (step.contains("name")) p.run.log += "== " + substitute(asText(step["name"]), p.vars) + "\n";
    if (step.contains("set")) {
        if (!step["set"].is_object()) throw ParamError(where + ": set must be a mapping of name: value");
        for (const auto& [k, v] : step["set"].items()) p.vars[k] = substitute(asText(v), p.vars);
    }
    if (step.contains("mkdir")) {
        std::string dir = substitute(asText(step["mkdir"]), p.vars);
        fs::path d = fs::path(dir).is_absolute() || p.base.empty() ? fs::path(dir) : p.base / dir;
        if (p.dry) {
            p.run.log += "would create " + d.generic_string() + "\n";
        } else {
            std::error_code ec;
            fs::create_directories(d, ec);
            if (ec) {
                p.run.record(Result::failure("cannot create " + d.string() + ": " + ec.message()), where);
                p.run.stopped = !p.keepGoing;
                return p.keepGoing;
            }
        }
    }
    if (step.contains("echo")) p.run.log += substitute(asText(step["echo"]), p.vars) + "\n";

    // a loop's body: its steps:, or the step's own run:/command: when it has none
    auto loopBody = [&] {
        json body = step.contains("steps") ? step["steps"] : step.contains("do") ? step["do"] : json::array();
        if (!body.is_array()) body = json::array({body});
        if (body.empty() && (step.contains("run") || step.contains("command"))) {
            json single = json::object();
            for (const char* k : {"run", "command", "args", "capture", "on_error"}) {
                if (step.contains(k)) single[k] = step[k];
            }
            body.push_back(single);
        }
        return body;
    };

    if (step.contains("for")) {
        // a list of values: for: [64, 128, 256] (or "64,128,256") sets $item, or the as: name
        std::vector<std::string> values;
        if (step["for"].is_array()) {
            for (const auto& e : step["for"]) values.push_back(substitute(asText(e), p.vars));
        } else {
            std::string item;
            for (char c : substitute(asText(step["for"]), p.vars) + ",") {
                if (c == ',') {
                    std::string t = trim(item);
                    if (!t.empty()) values.push_back(t);
                    item.clear();
                } else {
                    item += c;
                }
            }
        }
        const std::string var = step.contains("as") ? asText(step["as"]) : "item";
        const json body = loopBody();
        std::size_t index = 0;
        for (const auto& v : values) {
            p.vars[var] = v;
            p.vars["index"] = std::to_string(index + 1);
            p.vars["count"] = std::to_string(values.size());
            if (!passes(p.currentFileVar)) continue;
            ++index;
            if (!runSteps(p, body, where + " [" + var + "=" + v + "]")) return false;
        }
        return true;
    }

    if (step.contains("for_each")) {
        std::vector<std::string> patterns;
        if (step["for_each"].is_array()) {
            for (const auto& e : step["for_each"]) patterns.push_back(substitute(asText(e), p.vars));
        } else {
            patterns.push_back(substitute(asText(step["for_each"]), p.vars));
        }
        std::vector<fs::path> files;
        std::set<std::string> seen;
        for (const auto& pat : patterns) {
            for (auto& f : expandGlob(pat, p.base)) {
                if (seen.insert(f.generic_string()).second) files.push_back(f);
            }
        }
        const std::string var = step.contains("as") ? asText(step["as"]) : "file";
        const json body = loopBody();
        p.run.log += "for_each " + joinNames(patterns) + ": " + std::to_string(files.size()) + " files\n";
        const std::string outerVar = p.currentFileVar;
        p.currentFileVar = var;
        std::size_t index = 0;
        for (const auto& f : files) {
            addFileVars(p.vars, var, f);
            p.vars["index"] = std::to_string(index + 1);
            p.vars["count"] = std::to_string(files.size());
            if (!passes(var)) continue;
            ++index;
            if (!runSteps(p, body, where + " [" + f.filename().string() + "]")) {
                p.currentFileVar = outerVar;
                return false;
            }
        }
        p.currentFileVar = outerVar;
        return true;
    }

    if (step.contains("script") || step.contains("pipeline")) {
        const bool isScript = step.contains("script");
        std::string file = substitute(asText(isScript ? step["script"] : step["pipeline"]), p.vars);
        fs::path path = fs::path(file).is_absolute() || p.base.empty() ? fs::path(file) : p.base / file;
        Vars vars = p.vars;
        if (step.contains("vars")) {
            if (!step["vars"].is_object()) throw ParamError(where + ": vars must be a mapping");
            for (const auto& [k, v] : step["vars"].items()) vars[k] = substitute(asText(v), p.vars);
        }
        if (tDepth >= kMaxDepth) throw ParamError("scripts nest more than " + std::to_string(kMaxDepth) + " deep (does one call itself?)");
        DepthGuard guard;
        std::string text = readFile(path);
        if (isScript) {
            vars["script_dir"] = fs::absolute(path).parent_path().generic_string();
            auto missing = missingVars(text, vars);
            if (!missing.empty() && !p.dry) {
                p.run.record(Result::failure(path.string() + " needs " + joinNames(missing)), where);
                p.run.stopped = !p.keepGoing;
                return p.keepGoing;
            }
            Run sub;
            runScriptText(p.session, text, vars, p.keepGoing, p.dry, sub, path.filename().string());
            p.run.ran += sub.ran;
            p.run.failed += sub.failed;
            p.run.log += sub.log;
            for (auto& a : sub.attachments) {
                if (p.run.attachments.size() < 8) p.run.attachments.push_back(a);
            }
            if (sub.failed && !p.keepGoing) {
                p.run.stopped = true;
                return false;
            }
            return true;
        }
        json nested = json::parse(pipelineToJson(text));
        Pipe sub(p.session);
        sub.vars = vars;
        sub.dry = p.dry;
        sub.keepGoing = p.keepGoing;
        sub.vars["pipeline_dir"] = fs::absolute(path).parent_path().generic_string();
        if (nested.is_object() && nested.contains("vars") && nested["vars"].is_object()) {
            for (const auto& [k, v] : nested["vars"].items()) sub.vars.emplace(k, asText(v));
        }
        json body = nested.is_array() ? nested : nested.value("steps", json::array());
        bool ok = runSteps(sub, body, path.filename().string());
        p.run.ran += sub.run.ran;
        p.run.failed += sub.run.failed;
        p.run.log += sub.run.log;
        if (!ok) p.run.stopped = true;
        return ok;
    }

    if (step.contains("run") || step.contains("command")) return runCommandStep(p, step, where);
    return true;
}

bool runSteps(Pipe& p, const json& steps, const std::string& where) {
    if (!steps.is_array()) throw ParamError(where + ": steps must be a list");
    for (std::size_t i = 0; i < steps.size(); ++i) {
        if (!runStep(p, steps[i], where + " step " + std::to_string(i + 1))) return false;
    }
    return true;
}

// The variables a pipeline needs from outside: every $name in its strings, minus what it defines itself (vars:,
// set:, capture:, as:) and the built-ins. A pipeline that uses $file (or $stem ...) outside any for_each is a
// per-file pipeline: it needs file (from automate.watch, or vars=file=...).
std::vector<std::string> pipelineNeeds(const json& doc) {
    std::set<std::string> used, defined;
    bool loops = false;
    if (doc.is_object() && doc.contains("vars") && doc["vars"].is_object()) {
        for (const auto& [k, v] : doc["vars"].items()) defined.insert(k);
    }
    std::function<void(const json&, const std::string&)> walk = [&](const json& j, const std::string& key) {
        if (j.is_string()) {
            std::set<std::string> cap;
            scanVars(j.get<std::string>(), used, cap);
            if (key == "capture" || key == "as") defined.insert(j.get<std::string>());
            defined.insert(cap.begin(), cap.end());
        } else if (j.is_object()) {
            for (const auto& [k, v] : j.items()) {
                if (k == "builder_off") continue;  // blocks switched off in the pipeline builder
                if (k == "for_each") loops = true;
                if (k == "set" && v.is_object()) {
                    for (const auto& [sk, sv] : v.items()) defined.insert(sk);
                }
                walk(v, k);
            }
        } else if (j.is_array()) {
            for (const auto& v : j) walk(v, key);
        }
    };
    walk(doc, "");
    std::vector<std::string> needs;
    bool perFile = false;
    for (const char* f : {"file", "name", "stem", "ext", "dir"}) perFile = perFile || (!loops && used.count(f));
    if (perFile) needs.push_back("file");
    for (const char* b : {"file", "name", "stem", "ext", "dir", "index", "count", "item", "cwd", "date", "time", "pipeline_dir",
                          "script_dir", "watch_dir"}) {
        defined.insert(b);
    }
    for (const auto& u : used) {
        if (!defined.count(u)) needs.push_back(u);
    }
    return needs;
}

Result runPipelineText(Session& session, const std::string& text, const fs::path& path, Vars given, bool dry, bool keepGoing,
                       bool verbose = false) {
    json doc;
    try {
        doc = json::parse(pipelineToJson(text));
    } catch (const ParamError& e) {
        return Result::failure(path.string() + ": " + e.what());
    }
    Pipe p(session);
    p.dry = dry;
    p.keepGoing = keepGoing;
    p.run.verbose = verbose;
    // relative paths mean the current folder, as in batch scripts; $pipeline_dir is the file's own folder
    std::error_code ec;
    p.vars = std::move(given);
    deriveFileVars(p.vars);
    addBuiltins(p.vars, path);
    if (!path.empty()) p.vars.emplace("pipeline_dir", fs::absolute(path, ec).parent_path().generic_string());
    if (doc.is_object() && doc.contains("vars")) {
        if (!doc["vars"].is_object()) return Result::failure("vars must be a mapping of name: value");
        for (const auto& [k, v] : doc["vars"].items()) p.vars.emplace(k, substitute(asText(v), p.vars));  // given vars win
    }
    if (doc.is_object() && doc.contains("keep_going") && doc["keep_going"].is_boolean()) p.keepGoing = p.keepGoing || doc["keep_going"].get<bool>();
    std::vector<std::string> missing;
    for (const auto& n : pipelineNeeds(doc)) {
        if (!p.vars.count(n)) missing.push_back(n);
    }
    if (!missing.empty() && !dry) {
        const std::string what = path.empty() ? "the pipeline" : path.generic_string();
        if (missing[0] == "file") {
            return Result::failure(what + " runs once per file: pass vars=file=<path>, or run it with automate.watch" +
                                   (missing.size() > 1 ? " (it also needs " + joinNames({missing.begin() + 1, missing.end()}) + ")" : ""));
        }
        return Result::failure(what + " needs " + joinNames(missing) + ": pass vars=" + missing[0] + "=...");
    }
    if (dry && !missing.empty()) p.run.log += "(needs vars: " + joinNames(missing) + ")\n";
    json steps = doc.is_array() ? doc : doc.value("steps", json());
    if (!steps.is_array()) return Result::failure("a pipeline needs a steps: list");
    DepthGuard guard;
    try {
        runSteps(p, steps, path.empty() ? "pipeline" : path.filename().string());
    } catch (const ParamError& e) {
        return Result::failure(e.what());
    } catch (const std::exception& e) {
        return Result::failure(e.what());
    }
    std::string name = doc.is_object() && doc.contains("name") ? asText(doc["name"]) : path.empty() ? "pipeline" : path.filename().string();
    Result r = finish(name, p.run, dry);
    return r;
}

// ---- watching -----------------------------------------------------------------

struct WatchConfig {
    fs::path folder;
    std::vector<std::string> patterns;
    std::vector<std::string> exclude;
    bool recursive = false;
    double interval = 2;
    double settle = 1;
    bool existing = false;
    std::string script, pipeline;
    Vars vars;
    fs::path log;
    fs::path state;  // remembers processed files between runs
    bool keepGoing = false;
    bool quiet = false;
};

// A path as shown to people: relative to the current folder when it's inside it.
std::string displayPath(const fs::path& path) {
    std::error_code ec;
    std::string rel = path.lexically_relative(fs::current_path(ec)).generic_string();
    if (ec || rel.empty() || rel.rfind("..", 0) == 0) return path.generic_string();
    return rel;
}

struct Signature {
    std::uintmax_t size = 0;
    std::int64_t tick = 0;
    bool operator==(const Signature&) const = default;
};

class Watcher {
public:
    explicit Watcher(WatchConfig c) : cfg(std::move(c)) { loadState(); }

    WatchConfig cfg;
    std::atomic<bool> stop{false};
    std::thread thread;

    // stats, guarded by mutex
    std::mutex m;
    std::size_t runs = 0, failures = 0, polls = 0;
    std::deque<std::string> recent;
    std::string started = today(false) + " " + today(true);

    void note(const std::string& line, bool foreground) {
        {
            std::lock_guard<std::mutex> lock(m);
            recent.push_back(line);
            while (recent.size() > 30) recent.pop_front();
        }
        if (foreground && !cfg.quiet) std::cerr << line << std::endl;
        if (!cfg.log.empty()) {
            std::ofstream out(cfg.log, std::ios::app);
            out << today(false) << " " << today(true) << " " << line << "\n";
        }
    }

    // The state file: one "size tick path" line per file processed without errors.
    void loadState() {
        if (cfg.state.empty()) return;
        std::ifstream in(cfg.state);
        for (std::string line; std::getline(in, line);) {
            std::istringstream ls(line);
            Signature sig;
            std::string path;
            if (!(ls >> sig.size >> sig.tick)) continue;
            std::getline(ls >> std::ws, path);
            if (!path.empty()) done[path] = sig;
        }
    }

    void finished(const fs::path& file, bool ok) {
        if (cfg.state.empty()) return;
        const std::string key = file.generic_string();
        auto k = known.find(key);
        if (ok && k != known.end()) {
            done[key] = k->second;
        } else {
            done.erase(key);
        }
        fs::path tmp = cfg.state;
        tmp += ".tmp";
        {
            std::ofstream out(tmp, std::ios::trunc);
            for (const auto& [path, sig] : done) out << sig.size << " " << sig.tick << " " << path << "\n";
        }
        std::error_code ec;
        fs::rename(tmp, cfg.state, ec);
        if (ec) {
            fs::remove(cfg.state, ec);
            fs::rename(tmp, cfg.state, ec);
        }
    }

    bool wanted(const fs::path& file) const {
        const std::string name = file.filename().string();
        if (!name.empty() && name[0] == '.') return false;  // hidden and temporary files
        const std::string rel = file.lexically_relative(cfg.folder).generic_string();
        for (const auto& x : cfg.exclude) {
            if (globMatch(x, name) || globMatch(x, rel)) return false;
            if (rel.rfind(x + "/", 0) == 0) return false;
        }
        if (cfg.patterns.empty()) return true;
        for (const auto& p : cfg.patterns) {
            if (globMatch(p, name) || globMatch(p, rel)) return true;
        }
        return false;
    }

    // One scan. Returns files that are new or changed and have stopped changing.
    std::vector<fs::path> poll() {
        std::vector<fs::path> ready;
        std::map<std::string, Signature> now;
        std::error_code ec;
        auto add = [&](const fs::directory_entry& e) {
            std::error_code fec;
            if (!e.is_regular_file(fec) || !wanted(e.path())) return;
            Signature s{e.file_size(fec), static_cast<std::int64_t>(e.last_write_time(fec).time_since_epoch().count())};
            if (!fec) now[e.path().generic_string()] = s;
        };
        if (cfg.recursive) {
            for (fs::recursive_directory_iterator it(cfg.folder, fs::directory_options::skip_permission_denied, ec), end; !ec && it != end;
                 it.increment(ec)) {
                if (it->is_directory(ec) && !it->path().filename().empty() && it->path().filename().string()[0] == '.') {
                    it.disable_recursion_pending();
                    continue;
                }
                add(*it);
            }
        } else {
            for (const auto& e : fs::directory_iterator(cfg.folder, fs::directory_options::skip_permission_denied, ec)) add(e);
        }
        const auto t = std::chrono::steady_clock::now();
        const bool first = polls++ == 0;
        for (const auto& [path, sig] : now) {
            auto k = known.find(path);
            auto d = done.find(path);
            if (d != done.end() && d->second == sig) {  // processed by an earlier run (state=)
                known[path] = sig;
                pending.erase(path);
                continue;
            }
            if (first && !cfg.existing) {
                known[path] = sig;
                continue;
            }
            if (k != known.end() && k->second == sig) {
                pending.erase(path);
                continue;
            }
            auto pend = pending.find(path);
            if (pend == pending.end() || !(pend->second.first == sig)) {
                pending[path] = {sig, t};
                if (cfg.settle > 0) continue;
            }
            if (std::chrono::duration<double>(t - pending[path].second).count() + 1e-9 >= cfg.settle) {
                ready.push_back(path);
                known[path] = sig;
                pending.erase(path);
            }
        }
        for (auto it = known.begin(); it != known.end();) {
            it = now.count(it->first) ? std::next(it) : known.erase(it);
        }
        return ready;
    }

    std::map<std::string, Signature> known;
    std::map<std::string, Signature> done;
    std::map<std::string, std::pair<Signature, std::chrono::steady_clock::time_point>> pending;
};

// Runs the watcher's script or pipeline for one file.
Result runFor(Session& session, const WatchConfig& cfg, const fs::path& file) {
    Vars vars = cfg.vars;
    addFileVars(vars, "file", file, cfg.folder);
    vars["watch_dir"] = cfg.folder.generic_string();
    try {
        if (!cfg.pipeline.empty()) {
            return runPipelineText(session, readFile(cfg.pipeline), cfg.pipeline, vars, false, cfg.keepGoing);
        }
        std::string text = readFile(cfg.script);
        addBuiltins(vars, cfg.script);
        auto missing = missingVars(text, vars);
        if (!missing.empty()) return Result::failure(cfg.script + " needs " + joinNames(missing) + " (pass vars=)");
        Run run;
        DepthGuard guard;
        runScriptText(session, text, vars, cfg.keepGoing, false, run, fs::path(cfg.script).filename().string());
        return finish(fs::path(cfg.script).filename().string(), run, false);
    } catch (const std::exception& e) {
        return Result::failure(e.what());
    }
}

void process(Watcher& w, Session* shared, const Registrar& fresh, const fs::path& file, bool foreground) {
    Result r;
    if (fresh) {
        Session s;
        fresh(s);
        r = runFor(s, w.cfg, file);
    } else {
        r = runFor(*shared, w.cfg, file);
    }
    {
        std::lock_guard<std::mutex> lock(w.m);
        ++w.runs;
        if (!r.ok) ++w.failures;
    }
    w.finished(file, r.ok);
    std::string first = r.message.substr(0, r.message.find('\n'));
    w.note((r.ok ? "ok   " : "FAIL ") + displayPath(file) + ": " + (r.ok ? first : r.message), foreground);
}

void sleepFor(const Watcher& w, double seconds) {
    auto until = std::chrono::steady_clock::now() + std::chrono::duration<double>(seconds);
    while (!w.stop && std::chrono::steady_clock::now() < until) std::this_thread::sleep_for(std::chrono::milliseconds(50));
}

struct State {
    Registrar fresh;
    std::mutex m;
    std::map<std::string, std::unique_ptr<Watcher>> watchers;
    int next = 1;

    ~State() {
        for (auto& [id, w] : watchers) {
            w->stop = true;
            if (w->thread.joinable()) w->thread.join();
        }
    }
};

std::vector<std::string> splitList(const std::string& s) {
    std::vector<std::string> out;
    std::string cur;
    for (char c : s + ",") {
        if (c == ',' || c == ';') {
            std::string t = trim(cur);
            if (!t.empty()) out.push_back(t);
            cur.clear();
        } else {
            cur += c;
        }
    }
    return out;
}

std::vector<ParamSpec> watchParams() {
    return {{"folder", ParamType::String, "folder to watch", true},
            {"script", ParamType::String, "batch script to run for each file ($file, $name, $stem, $ext, $dir are set)", false},
            {"pipeline", ParamType::String, "pipeline (JSON/YAML) to run for each file instead of a script", false},
            {"pattern", ParamType::String, "only files matching these wildcards, e.g. *.png,*.jpg (default: every file)", false},
            {"exclude", ParamType::String, "ignore files or subfolders matching these wildcards, e.g. out,*.tmp", false},
            {"recursive", ParamType::Bool, "watch subfolders too", false, {}, false},
            {"interval", ParamType::Number, "seconds between scans", false, {}, 2.0},
            {"settle", ParamType::Number, "seconds a file must stop changing before it's processed (so half-copied files wait)", false, {}, 1.0},
            {"existing", ParamType::Bool, "also process files already there when watching starts", false, {}, false},
            {"vars", ParamType::String, "extra variables: name=value,other=value", false},
            {"log", ParamType::String, "append a line per processed file to this log file", false},
            {"state", ParamType::String, "remember processed files in this file, so a later run (cron, once=true, a restart) only "
             "processes new or changed ones; implies existing=true", false},
            {"keep_going", ParamType::Bool, "carry on after a failed line inside the script", false, {}, false}};
}

bool watchConfig(const Params& p, WatchConfig& c, std::string& error) {
    c.folder = stringParam(p, "folder");
    std::error_code ec;
    if (!fs::is_directory(c.folder, ec)) {
        error = "no folder at " + c.folder.string();
        return false;
    }
    c.folder = fs::absolute(c.folder, ec).lexically_normal();
    c.script = stringParam(p, "script");
    c.pipeline = stringParam(p, "pipeline");
    if (c.script.empty() == c.pipeline.empty()) {
        error = "give script= or pipeline= (one of them)";
        return false;
    }
    const std::string& file = c.script.empty() ? c.pipeline : c.script;
    if (!fs::is_regular_file(file, ec)) {
        error = "no script at " + file;
        return false;
    }
    if (!c.script.empty()) c.script = fs::absolute(c.script, ec).string();
    if (!c.pipeline.empty()) c.pipeline = fs::absolute(c.pipeline, ec).string();
    c.patterns = splitList(stringParam(p, "pattern"));
    c.exclude = splitList(stringParam(p, "exclude"));
    c.recursive = boolParam(p, "recursive", false);
    c.interval = std::max(0.05, numberParam(p, "interval", 2.0));
    c.settle = std::max(0.0, numberParam(p, "settle", 1.0));
    c.existing = boolParam(p, "existing", false);
    c.keepGoing = boolParam(p, "keep_going", false);
    c.quiet = boolParam(p, "quiet", false);
    c.log = stringParam(p, "log");
    c.state = stringParam(p, "state");
    if (!c.state.empty()) {
        c.state = fs::absolute(c.state, ec).lexically_normal();
        c.existing = true;
    }
    try {
        c.vars = parseVars(stringParam(p, "vars"));
    } catch (const ParamError& e) {
        error = e.what();
        return false;
    }
    // the log and outputs inside the watched folder would trigger runs of their own
    if (!c.log.empty()) {
        auto rel = fs::absolute(c.log, ec).lexically_normal().lexically_relative(c.folder).generic_string();
        if (!rel.empty() && rel.rfind("..", 0) != 0) c.exclude.push_back(rel);
    }
    if (!c.state.empty()) {
        auto rel = c.state.lexically_relative(c.folder).generic_string();
        if (!rel.empty() && rel.rfind("..", 0) != 0) {
            c.exclude.push_back(rel);
            c.exclude.push_back(rel + ".tmp");
        }
    }
    return true;
}

std::string describe(const std::string& id, Watcher& w) {
    std::lock_guard<std::mutex> lock(w.m);
    std::string out = id + ": " + displayPath(w.cfg.folder) + " -> " + displayPath(w.cfg.script.empty() ? w.cfg.pipeline : w.cfg.script) +
                      ", " + plural(w.runs, "run", "runs") + ", " + std::to_string(w.failures) + " failed, " +
                      plural(w.polls, "scan", "scans") + ", since " + w.started + (w.stop ? " (stopped)" : "");
    std::size_t from = w.recent.size() > 5 ? w.recent.size() - 5 : 0;
    for (std::size_t i = from; i < w.recent.size(); ++i) out += "\n    " + w.recent[i];
    return out;
}

}  // namespace

void registerCommands(Session& session, Registrar fresh) {
    auto& reg = session.registry();
    Session* s = &session;
    auto st = std::make_shared<State>();
    st->fresh = std::move(fresh);

    reg.add({"automate.run_script", "Run script",
             "Run a batch script (one command per line, # comments, `-> name` captures, $name reuse) with variables "
             "passed in: vars=input=photos,size=800 fills $input and $size. Also set: $cwd, $date, $time, "
             "$script_dir. Documents the script opens stay open afterwards. Missing variables are reported before "
             "anything runs. There is no shell command, by design: scripts can only run suite commands.",
             {{"path", ParamType::String, "the script file", false},
              {"text", ParamType::String, "the script itself, instead of a file", false},
              {"vars", ParamType::String, "variables: name=value,other=value (\\, for a comma in a value), or a JSON object", false},
              {"keep_going", ParamType::Bool, "carry on after a failed line", false, {}, false},
              {"dry_run", ParamType::Bool, "print each line with variables filled in, without running anything", false, {}, false},
              {"verbose", ParamType::Bool, "show each command's full output (default: its first line)", false, {}, false}},
             false, false},
            [s](const Params& p) {
                return std::make_unique<FunctionAction>([s, p] {
                    const std::string path = stringParam(p, "path"), inline_ = stringParam(p, "text");
                    if (path.empty() == inline_.empty()) return Result::failure("give path= or text= (one of them)");
                    if (tDepth >= kMaxDepth) return Result::failure("scripts nest more than " + std::to_string(kMaxDepth) + " deep");
                    std::string text;
                    Vars vars;
                    try {
                        text = path.empty() ? inline_ : readFile(path);
                        vars = parseVars(stringParam(p, "vars"));
                    } catch (const ParamError& e) {
                        return Result::failure(e.what());
                    }
                    deriveFileVars(vars);
                    addBuiltins(vars, path);
                    const bool dry = boolParam(p, "dry_run");
                    auto missing = missingVars(text, vars);
                    if (!missing.empty() && !dry) {
                        return Result::failure("the script needs " + joinNames(missing) + ": pass vars=" + missing[0] + "=...");
                    }
                    DepthGuard guard;
                    Run run;
                    run.verbose = boolParam(p, "verbose");
                    runScriptText(*s, text, vars, boolParam(p, "keep_going"), dry, run, path.empty() ? "script" : fs::path(path).filename().string());
                    if (dry && !missing.empty()) run.log += "(needs vars: " + joinNames(missing) + ")\n";
                    return finish(path.empty() ? "script" : path, run, dry);
                });
            });

    reg.add({"automate.pipeline", "Run pipeline",
             "Run a pipeline file (JSON, or YAML: steps as a '- ' list). A step is a command line string or a "
             "mapping: run: <command line> (capture: name keeps the result as $name, its fields as $name.width ...), "
             "command: + args: (structured form), for_each: <glob like in/*.svg or in/**/*.png> with steps: (sets "
             "$file, $file.name, $file.stem, $file.ext, $file.dir, $index; as: renames $file), for: [64, 128] (loops over values as $item, or as:), if: <condition: "
             "'$file.ext in png,jpg', '$x == y', '$n > 3', 'exists path', 'not ...'>, ext: png,jpg (shorthand), "
             "set: {name: value}, mkdir: <folder>, echo: <text>, script: <batch file> with vars:, pipeline: <file>, "
             "on_error: continue. Top level: vars: (defaults, overridden by vars=), steps:.",
             {{"path", ParamType::String, "the pipeline file (.json, .yaml or .yml)", false},
              {"text", ParamType::String, "the pipeline itself, instead of a file", false},
              {"vars", ParamType::String, "variables: name=value,other=value, or a JSON object", false},
              {"keep_going", ParamType::Bool, "carry on after failures", false, {}, false},
              {"dry_run", ParamType::Bool, "show what would run (globs are expanded, nothing is run)", false, {}, false},
              {"verbose", ParamType::Bool, "show each command's full output (default: its first line)", false, {}, false}},
             false, false},
            [s](const Params& p) {
                return std::make_unique<FunctionAction>([s, p] {
                    const std::string path = stringParam(p, "path"), inline_ = stringParam(p, "text");
                    if (path.empty() == inline_.empty()) return Result::failure("give path= or text= (one of them)");
                    if (tDepth >= kMaxDepth) return Result::failure("pipelines nest more than " + std::to_string(kMaxDepth) + " deep");
                    std::string text;
                    Vars vars;
                    try {
                        text = path.empty() ? inline_ : readFile(path);
                        vars = parseVars(stringParam(p, "vars"));
                    } catch (const ParamError& e) {
                        return Result::failure(e.what());
                    }
                    const bool dry = boolParam(p, "dry_run");
                    return runPipelineText(*s, text, path, vars, dry, boolParam(p, "keep_going"), boolParam(p, "verbose"));
                });
            });

    reg.add({"automate.pipeline_save", "Save pipeline",
             "Check a pipeline (JSON or YAML text) and save it as a file that automate.pipeline, automate.watch and the "
             "pipeline builder can run. The pipeline builder in the files app saves through this.",
             {{"path", ParamType::String, "where to save it (.json, .yaml or .yml)", true},
              {"text", ParamType::String, "the pipeline", true},
              {"overwrite", ParamType::Bool, "replace a file that's already there", false, {}, false}},
             false, false},
            [](const Params& p) {
                return std::make_unique<FunctionAction>([p] {
                    const fs::path path = stringParam(p, "path");
                    const std::string text = stringParam(p, "text");
                    try {
                        json doc = json::parse(pipelineToJson(text));
                        json steps = doc.is_array() ? doc : doc.value("steps", json());
                        if (!steps.is_array()) return Result::failure("a pipeline needs a steps: list");
                    } catch (const std::exception& e) {
                        return Result::failure(std::string("that isn't a pipeline the runner can read: ") + e.what());
                    }
                    std::error_code ec;
                    if (fs::exists(path, ec) && !boolParam(p, "overwrite")) return Result::failure(path.string() + " is already there: pass overwrite=true");
                    if (path.has_parent_path()) fs::create_directories(path.parent_path(), ec);
                    std::ofstream out(path, std::ios::binary | std::ios::trunc);
                    out << text;
                    if (!out) return Result::failure("can't write " + path.string());
                    Result r = Result::success("saved the pipeline to " + path.string() + "; run it with automate.pipeline path=" + path.generic_string());
                    return r.with("path", path.generic_string());
                });
            });

    {
        auto params = watchParams();
        params.push_back({"duration", ParamType::Number, "stop after this many seconds (0 = run until interrupted with Ctrl+C)", false, {}, 0.0});
        params.push_back({"max_runs", ParamType::Int, "stop after processing this many files (0 = no limit)", false, {}, std::int64_t{0}});
        params.push_back({"once", ParamType::Bool, "process every matching file there now, then stop (for cron / Task Scheduler)", false, {}, false});
        params.push_back({"quiet", ParamType::Bool, "don't print a line per file to stderr", false, {}, false});
        reg.add({"automate.watch", "Watch folder",
                 "Watch a folder by polling and run a script or pipeline for each new or changed file, in the foreground "
                 "(it blocks; progress goes to stderr). For the command line and services. Under MCP use "
                 "automate.watch_start, which runs in the background. Each file gets its own fresh session.",
                 params, false, false},
                [s, st](const Params& p) {
                    return std::make_unique<FunctionAction>([s, st, p] {
                        WatchConfig cfg;
                        std::string error;
                        if (!watchConfig(p, cfg, error)) return Result::failure(error);
                        const bool once = boolParam(p, "once");
                        if (once) {
                            cfg.existing = true;
                            cfg.settle = 0;
                        }
                        Watcher w(cfg);
                        const double duration = numberParam(p, "duration", 0);
                        const auto maxRuns = static_cast<std::size_t>(std::max<std::int64_t>(0, intParam(p, "max_runs", 0)));
                        const auto t0 = std::chrono::steady_clock::now();
                        if (!once) w.note("watching " + displayPath(cfg.folder) + " every " + toText(cfg.interval) + " s", true);
                        while (true) {
                            for (const auto& f : w.poll()) {
                                process(w, s, st->fresh, f, true);
                                if (maxRuns && w.runs >= maxRuns) break;
                            }
                            if (once || (maxRuns && w.runs >= maxRuns)) break;
                            const double elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
                            if (duration > 0 && elapsed >= duration) break;
                            sleepFor(w, duration > 0 ? std::min(cfg.interval, duration - elapsed) : cfg.interval);
                        }
                        std::string msg = "processed " + plural(w.runs, "file", "files") + ", " + std::to_string(w.failures) + " failed";
                        for (const auto& l : w.recent) {
                            if (l.rfind("watching", 0) != 0) msg += "\n  " + l;
                        }
                        Result r = w.failures ? Result::failure(msg) : Result::success(msg);
                        return r.with("runs", static_cast<std::int64_t>(w.runs)).with("failed", static_cast<std::int64_t>(w.failures));
                    });
                });
    }

    reg.add({"automate.watch_start", "Start watching",
             "Start watching a folder in the background: every new or changed file runs the script or pipeline in its "
             "own fresh session, while you carry on. Returns a watch id (w1); see automate.watch_status and stop with "
             "automate.watch_stop. Watches end when the suite process exits.",
             watchParams(), false, false},
            [st](const Params& p) {
                return std::make_unique<FunctionAction>([st, p] {
                    if (!st->fresh) return Result::failure("background watching isn't available in this binary; use automate.watch");
                    WatchConfig cfg;
                    std::string error;
                    if (!watchConfig(p, cfg, error)) return Result::failure(error);
                    std::lock_guard<std::mutex> lock(st->m);
                    std::string id = "w" + std::to_string(st->next++);
                    auto w = std::make_unique<Watcher>(cfg);
                    Watcher* raw = w.get();
                    Registrar fresh = st->fresh;
                    raw->thread = std::thread([raw, fresh] {
                        while (!raw->stop) {
                            for (const auto& f : raw->poll()) {
                                if (raw->stop) break;
                                process(*raw, nullptr, fresh, f, false);
                            }
                            sleepFor(*raw, raw->cfg.interval);
                        }
                    });
                    st->watchers[id] = std::move(w);
                    Result r = Result::success("watching " + displayPath(cfg.folder) + " as " + id + " (every " + toText(cfg.interval) +
                                               " s); stop with automate.watch_stop id=" + id);
                    return r.with("id", id).with("folder", cfg.folder.generic_string());
                });
            });

    reg.add({"automate.watch_stop", "Stop watching", "Stop a background watch (id=w1), or every watch when no id is given.",
             {{"id", ParamType::String, "the watch id from automate.watch_start (default: all)", false}}, false, false},
            [st](const Params& p) {
                return std::make_unique<FunctionAction>([st, p] {
                    const std::string id = stringParam(p, "id");
                    std::vector<std::unique_ptr<Watcher>> stopping;
                    std::string msg;
                    {
                        std::lock_guard<std::mutex> lock(st->m);
                        if (!id.empty() && !st->watchers.count(id)) return Result::failure("no watch called " + id);
                        for (auto it = st->watchers.begin(); it != st->watchers.end();) {
                            if (id.empty() || it->first == id) {
                                it->second->stop = true;
                                msg += "\n  " + it->first + ": " + plural(it->second->runs, "run", "runs");
                                stopping.push_back(std::move(it->second));
                                it = st->watchers.erase(it);
                            } else {
                                ++it;
                            }
                        }
                    }
                    for (auto& w : stopping) {
                        if (w->thread.joinable()) w->thread.join();
                    }
                    Result r = Result::success("stopped " + plural(stopping.size(), "watch", "watches") + msg);
                    return r.with("stopped", static_cast<std::int64_t>(stopping.size()));
                });
            });

    reg.add({"automate.watch_status", "Watch status",
             "List background watches: folder, script, files processed, failures and the latest results.",
             {{"id", ParamType::String, "one watch (default: all)", false}}, false, true},
            [st](const Params& p) {
                return std::make_unique<FunctionAction>([st, p] {
                    const std::string id = stringParam(p, "id");
                    std::lock_guard<std::mutex> lock(st->m);
                    if (!id.empty() && !st->watchers.count(id)) return Result::failure("no watch called " + id);
                    std::string msg = plural(id.empty() ? st->watchers.size() : 1, "watch", "watches");
                    std::int64_t runs = 0, failures = 0;
                    std::string ids;
                    for (auto& [wid, w] : st->watchers) {
                        if (!id.empty() && wid != id) continue;
                        msg += "\n  " + describe(wid, *w);
                        std::lock_guard<std::mutex> wl(w->m);
                        runs += static_cast<std::int64_t>(w->runs);
                        failures += static_cast<std::int64_t>(w->failures);
                        ids += (ids.empty() ? "" : ",") + wid;
                    }
                    Result r = Result::success(msg);
                    return r.with("watches", ids).with("runs", runs).with("failed", failures);
                });
            });

    reg.add({"automate.list_scripts", "List scripts",
             "List the batch scripts (.txt, .batch, .ach) and pipelines (.json, .yaml, .yml) in a folder with their "
             "descriptions (a script's leading # comments, a pipeline's description:) and the variables each one needs.",
             {{"folder", ParamType::String, "where to look (default: $WETWARESUITE_SCRIPTS, else ./scripts)", false},
              {"recursive", ParamType::Bool, "look in subfolders too", false, {}, false},
              {"json", ParamType::Bool, "also return the list as a JSON scripts field", false, {}, false}},
             false, true},
            [](const Params& p) {
                return std::make_unique<FunctionAction>([p] {
                    std::string folder = stringParam(p, "folder");
                    if (folder.empty()) {
                        const char* env = std::getenv("WETWARESUITE_SCRIPTS");
                        folder = env && *env ? env : "scripts";
                    }
                    std::error_code ec;
                    if (!fs::is_directory(folder, ec)) return Result::failure("no folder at " + folder + " (pass folder=)");
                    std::vector<fs::path> files;
                    auto consider = [&](const fs::directory_entry& e) {
                        std::error_code fec;
                        if (!e.is_regular_file(fec)) return;
                        static const std::set<std::string> exts = {"txt", "batch", "ach", "json", "yaml", "yml"};
                        if (exts.count(lowerExt(e.path()))) files.push_back(e.path());
                    };
                    if (boolParam(p, "recursive")) {
                        for (fs::recursive_directory_iterator it(folder, fs::directory_options::skip_permission_denied, ec), end; !ec && it != end; it.increment(ec)) consider(*it);
                    } else {
                        for (const auto& e : fs::directory_iterator(folder, fs::directory_options::skip_permission_denied, ec)) consider(e);
                    }
                    std::sort(files.begin(), files.end());
                    json arr = json::array();
                    std::string msg = std::to_string(files.size()) + " scripts in " + folder;
                    for (const auto& f : files) {
                        std::string text;
                        try {
                            text = readFile(f);
                        } catch (const ParamError&) {
                            continue;
                        }
                        const std::string ext = lowerExt(f);
                        const bool pipeline = ext == "json" || ext == "yaml" || ext == "yml";
                        std::string description, problem;
                        std::set<std::string> used, defined;
                        std::vector<std::string> needs;
                        if (pipeline) {
                            try {
                                json doc = json::parse(pipelineToJson(text));
                                if (doc.is_object()) description = asText(doc.value("description", json(doc.value("name", ""))));
                                needs = pipelineNeeds(doc);
                            } catch (const ParamError& e) {
                                problem = e.what();
                            }
                        } else {
                            std::istringstream in(text);
                            for (std::string l; std::getline(in, l);) {
                                std::string t = trim(l);
                                if (t.empty()) {
                                    if (description.empty()) continue;
                                    break;
                                }
                                if (t[0] != '#') break;
                                t = trim(t.substr(1));
                                if (!t.empty()) description += (description.empty() ? "" : " ") + t;
                            }
                            scanVars(text, used, defined);
                            for (const char* b : {"cwd", "date", "time", "script_dir"}) defined.insert(b);
                            for (const auto& u : used) {
                                if (!defined.count(u)) needs.push_back(u);
                            }
                        }
                        msg += "\n  " + f.lexically_relative(folder).generic_string() + " (" + (pipeline ? "pipeline" : "script") + ")";
                        if (!description.empty()) msg += ": " + description;
                        if (!needs.empty()) msg += "\n      needs: " + joinNames(needs);
                        if (!problem.empty()) msg += "\n      problem: " + problem;
                        arr.push_back({{"path", f.generic_string()}, {"kind", pipeline ? "pipeline" : "script"}, {"description", description},
                                       {"needs", needs}, {"problem", problem}});
                    }
                    Result r = Result::success(msg);
                    r.with("count", static_cast<std::int64_t>(files.size()));
                    if (boolParam(p, "json")) r.with("scripts", arr.dump(-1, ' ', false, json::error_handler_t::replace));
                    return r;
                });
            });
}

}  // namespace wetwaresuite::automate
