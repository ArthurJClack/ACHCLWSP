// Configurations, like SolidWorks' configurations or Onshape's configuration table: named sets of
// parameter values kept in the part. cad.config_use sets the parameters, so the same feature tree
// makes every size; cad.config_export writes one file per configuration.
#include <algorithm>
#include <cctype>
#include <filesystem>
#include <stdexcept>

#include "build.hpp"
#include "cmdutil.hpp"

namespace wetwaresuite::cad {

using namespace cmd;

namespace {

// "a=1; b=w/2" or "a=1,b=2" (commas only split when there are no semicolons, so min(a,b) works with ;)
std::vector<Parameter> parseValues(const std::string& text) {
    const char sep = text.find(';') != std::string::npos ? ';' : ',';
    std::vector<Parameter> out;
    std::string item;
    auto flush = [&] {
        const auto eq = item.find('=');
        auto trim = [](std::string s) {
            while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front()))) s.erase(s.begin());
            while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) s.pop_back();
            return s;
        };
        if (!trim(item).empty()) {
            if (eq == std::string::npos) throw std::invalid_argument("'" + trim(item) + "' isn't name=value");
            out.push_back({trim(item.substr(0, eq)), trim(item.substr(eq + 1))});
        }
        item.clear();
    };
    for (char c : text) {
        if (c == sep) flush();
        else item += c;
    }
    flush();
    return out;
}

Configuration* findConfig(CadDoc& doc, const std::string& name) {
    for (auto& c : doc.configs)
        if (c.name == name) return &c;
    return nullptr;
}

std::string describe(const Configuration& c) {
    std::string out;
    for (const auto& v : c.values) out += (out.empty() ? "" : "; ") + v.name + "=" + v.expr;
    return out;
}

void apply(CadDoc& doc, const Configuration& c) {
    for (const auto& v : c.values) {
        bool found = false;
        for (auto& p : doc.params)
            if (p.name == v.name) p.expr = v.expr, found = true;
        if (!found) doc.params.push_back({v.name, v.expr});
    }
    doc.activeConfig = c.name;
}

}  // namespace

void registerConfigCommands(Session& session) {
    auto& reg = session.registry();

    reg.add({"cad.config_add", "Add Configuration",
             "Add (or replace) a configuration: a name and parameter values, e.g. name=M4 values=\"hole_d=4; length=30\". "
             "Parameters it names that don't exist yet are created when it's used. cad.config_use switches to it.",
             {docParam(), str("name", "configuration name, e.g. M4 or large", true),
              str("values", "name=value pairs, separated by ; (or , when no value has a comma)", true),
              boolean("use", "switch to it straight away", false)},
             true},
            cadCommand(session, [](CadDoc& doc, const Params& p) {
                const std::string name = stringParam(p, "name");
                if (name.empty()) return Result::failure("give the configuration a name");
                std::vector<Parameter> values;
                try {
                    values = parseValues(stringParam(p, "values"));
                } catch (const std::invalid_argument& e) {
                    return Result::failure(e.what());
                }
                if (values.empty()) return Result::failure("values is empty: give name=value pairs, e.g. hole_d=4; length=30");
                for (const auto& v : values)
                    if (!validParamName(v.name)) return Result::failure("'" + v.name + "' isn't a valid parameter name");
                Configuration* c = findConfig(doc, name);
                const bool replaced = c != nullptr;
                if (!c) {
                    doc.configs.push_back({name, {}});
                    c = &doc.configs.back();
                }
                c->values = values;
                if (boolParam(p, "use", false)) apply(doc, *c);
                Result r = Result::success((replaced ? "replaced configuration " : "added configuration ") + name + ": " + describe(*c), name);
                r.with("doc", stringParam(p, "doc")).with("name", name).with("count", static_cast<std::int64_t>(doc.configs.size()));
                return r;
            }));

    reg.add({"cad.config_use", "Use Configuration",
             "Switch the part to a configuration: its parameter values are set and the part rebuilds. If a value breaks "
             "a feature, nothing changes and the reason is given.",
             {docParam(), str("name", "configuration name (cad.config_list lists them)", true)}, true},
            cadCommand(session, [](CadDoc& doc, const Params& p) {
                const std::string name = stringParam(p, "name");
                const Configuration* c = findConfig(doc, name);
                if (!c) return Result::failure("no configuration called '" + name + "' (cad.config_list lists them)");
                apply(doc, *c);
                Result r = Result::success("using " + name + ": " + describe(*c), name);
                r.with("doc", stringParam(p, "doc")).with("name", name);
                return r;
            }));

    reg.add({"cad.config_delete", "Delete Configuration", "Remove a configuration (the parameters keep their values).",
             {docParam(), str("name", "configuration name", true)}, true},
            cadCommand(session, [](CadDoc& doc, const Params& p) {
                const std::string name = stringParam(p, "name");
                for (auto it = doc.configs.begin(); it != doc.configs.end(); ++it) {
                    if (it->name != name) continue;
                    doc.configs.erase(it);
                    if (doc.activeConfig == name) doc.activeConfig.clear();
                    return Result::success("deleted configuration " + name, name).with("doc", stringParam(p, "doc"));
                }
                return Result::failure("no configuration called '" + name + "'");
            }));

    reg.add({"cad.config_list", "List Configurations", "List the part's configurations with their values, and which is in use.",
             {docParam()}, false, true},
            cadCommand(session, [](CadDoc& doc, const Params& p) {
                std::string text, names;
                for (const auto& c : doc.configs) {
                    text += "\n  " + c.name + (c.name == doc.activeConfig ? " (in use)" : "") + ": " + describe(c);
                    names += (names.empty() ? "" : ",") + c.name;
                }
                Result r = Result::success(doc.configs.empty() ? "no configurations (cad.config_add makes one)"
                                                               : std::to_string(doc.configs.size()) + " configurations" + text,
                                           names);
                r.with("doc", stringParam(p, "doc")).with("names", names).with("active", doc.activeConfig);
                return r;
            }, false));

    reg.add({"cad.config_export", "Export Configurations",
             "Write one file per configuration: path has {name} where the configuration's name goes, e.g. "
             "bracket-{name}.stl or parts/{name}.step. The part is put back as it was afterwards.",
             {docParam(), str("path", "output path with {name} in it; the extension picks the format (stl, step, 3mf, obj, ...)", true),
              str("names", "only these configurations, comma-separated (default: all)")},
             false},
            [&session](const Params& p) -> std::unique_ptr<Action> {
                return std::make_unique<FunctionAction>([&session, p] {
                    const std::string id = stringParam(p, "doc");
                    auto doc = session.workspace().getAs<CadDoc>(id);
                    if (!doc) return Result::failure("no open cad document called '" + id + "'");
                    const std::string pattern = stringParam(p, "path");
                    if (pattern.find("{name}") == std::string::npos) return Result::failure("path needs {name} in it, e.g. bracket-{name}.stl");
                    if (doc->configs.empty()) return Result::failure("the part has no configurations (cad.config_add makes one)");
                    std::vector<std::string> only = splitList(stringParam(p, "names"));
                    const auto params = doc->params;
                    const auto active = doc->activeConfig;
                    std::string written, failed;
                    std::int64_t count = 0;
                    for (const auto& c : doc->configs) {
                        if (!only.empty() && std::find(only.begin(), only.end(), c.name) == only.end()) continue;
                        std::string path = pattern;
                        path.replace(path.find("{name}"), 6, c.name);
                        apply(*doc, c);
                        Result r;
                        try {
                            doc->regenerate();
                            r = session.run("cad.save", {{"doc", id}, {"path", path}});
                        } catch (const std::exception& e) {
                            r = Result::failure(e.what());
                        }
                        if (r.ok) {
                            written += "\n  " + path;
                            ++count;
                        } else {
                            failed += "\n  " + c.name + ": " + r.message;
                        }
                        doc->params = params;
                    }
                    doc->activeConfig = active;
                    try {
                        doc->regenerate();
                    } catch (const std::exception&) {
                    }
                    if (count == 0) return Result::failure("nothing written" + failed);
                    Result r = Result::success("wrote " + std::to_string(count) + " files" + written + (failed.empty() ? "" : "\nfailed:" + failed));
                    r.with("doc", id).with("count", count);
                    return r;
                });
            });
}

}  // namespace wetwaresuite::cad
