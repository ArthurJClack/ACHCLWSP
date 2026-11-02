#include "wetwaresuite/gui/pipeline.hpp"

#include <nlohmann/json.hpp>

namespace wetwaresuite::gui {

using nlohmann::ordered_json;

namespace {

ordered_json blockJson(const PipeBlock& b) {
    ordered_json s;
    s["command"] = b.command;
    ordered_json args = ordered_json::object();
    for (const auto& [k, v] : b.args) {
        if (!k.empty() && !v.empty()) args[k] = v;
    }
    if (!args.empty()) s["args"] = args;
    if (!b.ext.empty()) s["ext"] = b.ext;
    if (b.keepGoing) s["on_error"] = "continue";
    return s;
}

bool readBlock(const ordered_json& s, PipeBlock& b, std::string& error) {
    if (!s.is_object() || !s.contains("command") || !s["command"].is_string()) {
        error = "the builder shows command steps only; open this pipeline as text instead";
        return false;
    }
    for (const auto& [k, v] : s.items()) {
        if (k != "command" && k != "args" && k != "ext" && k != "on_error" && k != "builder_off") {
            error = "the builder can't show a step with " + k + ": open this pipeline as text instead";
            return false;
        }
    }
    b.command = s["command"].get<std::string>();
    if (s.contains("args") && s["args"].is_object()) {
        for (const auto& [k, v] : s["args"].items()) b.args.emplace_back(k, v.is_string() ? v.get<std::string>() : v.dump());
    }
    if (s.contains("ext") && s["ext"].is_string()) b.ext = s["ext"].get<std::string>();
    b.keepGoing = s.contains("on_error") && s["on_error"] == "continue";
    return true;
}

}  // namespace

std::string pipelineJson(const PipeModel& m) {
    ordered_json doc;
    if (!m.name.empty()) doc["name"] = m.name;
    if (!m.description.empty()) doc["description"] = m.description;
    ordered_json steps = ordered_json::array();
    for (const auto& b : m.blocks) {
        if (!b.off && !b.command.empty()) steps.push_back(blockJson(b));
    }
    // switched-off blocks are kept in the file, where the pipeline runner ignores them
    ordered_json off = ordered_json::array();
    for (std::size_t i = 0; i < m.blocks.size(); ++i) {
        if (m.blocks[i].off && !m.blocks[i].command.empty()) {
            ordered_json s = blockJson(m.blocks[i]);
            s["at"] = i;
            off.push_back(s);
        }
    }
    if (m.forEach) {
        ordered_json loop;
        loop["for_each"] = m.glob;
        loop["steps"] = steps;
        doc["steps"] = ordered_json::array({loop});
    } else {
        doc["steps"] = steps;
    }
    if (!off.empty()) doc["builder_off"] = off;
    return doc.dump(2) + "\n";
}

bool parsePipelineJson(const std::string& text, PipeModel& out, std::string& error) {
    ordered_json doc;
    try {
        doc = ordered_json::parse(text);
    } catch (const std::exception&) {
        error = "the builder opens JSON pipelines; open YAML ones as text";
        return false;
    }
    PipeModel m;
    ordered_json steps = doc;
    if (doc.is_object()) {
        if (doc.contains("name") && doc["name"].is_string()) m.name = doc["name"].get<std::string>();
        if (doc.contains("description") && doc["description"].is_string()) m.description = doc["description"].get<std::string>();
        if (doc.contains("vars")) {
            error = "the builder can't show vars: yet; open this pipeline as text instead";
            return false;
        }
        steps = doc.contains("steps") ? doc["steps"] : ordered_json::array();
    }
    if (!steps.is_array()) {
        error = "steps must be a list";
        return false;
    }
    if (steps.size() == 1 && steps[0].is_object() && steps[0].contains("for_each")) {
        const auto& loop = steps[0];
        for (const auto& [k, v] : loop.items()) {
            if (k != "for_each" && k != "steps") {
                error = "the builder can't show a for_each with " + k + ": open this pipeline as text instead";
                return false;
            }
        }
        m.forEach = true;
        m.glob = loop["for_each"].is_string() ? loop["for_each"].get<std::string>() : "";
        steps = loop.contains("steps") ? loop["steps"] : ordered_json::array();
    }
    for (const auto& s : steps) {
        PipeBlock b;
        if (!readBlock(s, b, error)) return false;
        m.blocks.push_back(std::move(b));
    }
    if (doc.is_object() && doc.contains("builder_off") && doc["builder_off"].is_array()) {
        for (const auto& s : doc["builder_off"]) {
            ordered_json clean = s;
            std::size_t at = clean.contains("at") && clean["at"].is_number_unsigned() ? clean["at"].get<std::size_t>() : m.blocks.size();
            clean.erase("at");
            PipeBlock b;
            if (!readBlock(clean, b, error)) return false;
            b.off = true;
            m.blocks.insert(m.blocks.begin() + static_cast<std::ptrdiff_t>(std::min(at, m.blocks.size())), std::move(b));
        }
    }
    out = std::move(m);
    return true;
}

int moveBlock(PipeModel& m, int from, int to) {
    const int n = static_cast<int>(m.blocks.size());
    if (from < 0 || from >= n) return from;
    to = std::max(0, std::min(to, n - 1));
    if (from == to) return from;
    PipeBlock b = std::move(m.blocks[static_cast<std::size_t>(from)]);
    m.blocks.erase(m.blocks.begin() + from);
    m.blocks.insert(m.blocks.begin() + to, std::move(b));
    return to;
}

}  // namespace wetwaresuite::gui
