// The effects rack of each project track and the master (audio.fx_*), and
// automation lanes that move the volume, pan or any effect setting over time.
#include <algorithm>
#include <cmath>

#include "common.hpp"
#include "wetwaresuite/audio/clap_host.hpp"
#include "wetwaresuite/audio/rack.hpp"

namespace wetwaresuite::audio::tool {

namespace {

struct RackRef {
    Rack* rack = nullptr;
    std::vector<Automation>* lanes = nullptr;
    Track* track = nullptr;  // null for the master
    std::string name;        // "track 1 \"Voice\"" or "the master"
};

std::vector<ParamSpec> ownerParams() {
    return {projectParam(), {"track", ParamType::Int, "track number, from 0 (or master=true, or bus=)", false},
            {"master", ParamType::Bool, "the master (everything mixed) instead of a track", false, {}, false},
            {"bus", ParamType::Int, "an effects bus (audio.bus_add) instead of a track", false}};
}

ParamSpec fxParam() { return {"fx", ParamType::Int, "effect number in the rack, from 0", true}; }

bool rackArg(ProjectDoc& d, const Params& p, RackRef& out, std::string& error) {
    if (boolParam(p, "master", false)) {
        out = {&d.project.masterFx, &d.project.masterAutomation, nullptr, "the master"};
        return true;
    }
    if (p.count("bus")) {
        const auto b = intParam(p, "bus", -1);
        if (b < 0 || b >= static_cast<std::int64_t>(d.project.buses.size())) {
            error = "no bus " + std::to_string(b) + " (the project has " + std::to_string(d.project.buses.size()) + "; audio.bus_add makes one)";
            return false;
        }
        Bus& bus = d.project.buses[static_cast<std::size_t>(b)];
        out = {&bus.fx, &bus.automation, nullptr, "bus " + std::to_string(b) + (bus.name.empty() ? "" : " \"" + bus.name + "\"")};
        return true;
    }
    if (!p.count("track")) {
        error = "give track=<number>, bus=<number> or master=true";
        return false;
    }
    const auto t = intParam(p, "track", -1);
    if (t < 0 || t >= static_cast<std::int64_t>(d.project.tracks.size())) {
        error = "no track " + std::to_string(t) + " (the project has " + std::to_string(d.project.tracks.size()) + ", numbered from 0)";
        return false;
    }
    Track& tr = d.project.tracks[static_cast<std::size_t>(t)];
    out = {&tr.fx, &tr.automation, &tr, "track " + std::to_string(t) + (tr.name.empty() ? "" : " \"" + tr.name + "\"")};
    return true;
}

bool fxArg(const RackRef& r, const Params& p, std::size_t& out, std::string& error) {
    const auto i = intParam(p, "fx", -1);
    if (i < 0 || i >= static_cast<std::int64_t>(r.rack->size())) {
        error = "no effect " + std::to_string(i) + " on " + r.name + " (it has " + std::to_string(r.rack->size()) + ", numbered from 0)";
        return false;
    }
    out = static_cast<std::size_t>(i);
    return true;
}

std::string rackText(const RackRef& r) {
    if (r.rack->empty()) return "\n  (no effects)";
    std::string s;
    for (std::size_t i = 0; i < r.rack->size(); ++i) s += "\n  " + std::to_string(i) + " " + describeEffect((*r.rack)[i]);
    return s;
}

// owner, fx, type, bypass, setting, value, min, max, unit, choices
std::string rackRows(const std::string& owner, const Rack& rack) {
    std::string rows;
    for (std::size_t i = 0; i < rack.size(); ++i) {
        if (rack[i].type == "clap") {
            // plugin settings: the setting column is the id key ("p7", what fx_set and automation take), the 11th the name
            std::vector<ClapParamInfo> params;
            std::string error;
            ClapPluginInfo info;
            std::string name = rack[i].plugin.substr(rack[i].plugin.rfind('#') + 1);
            if (findClapPlugin(rack[i].plugin, info, error)) name = info.name;
            if (!clapParams(rack[i].plugin, params, error)) continue;
            for (const auto& prm : params)
                rows += owner + "\t" + std::to_string(i) + "\tplugin: " + name + "\t" + (rack[i].bypass ? "1" : "0") + "\tp" + std::to_string(prm.id) + "\t" +
                        num(rack[i].get("p" + std::to_string(prm.id)), 8) + "\t" + num(prm.min, 8) + "\t" + num(prm.max, 8) + "\t\t\t" + prm.name + "\n";
            continue;
        }
        const EffectType* t = findEffectType(rack[i].type);
        if (!t) continue;
        for (const auto& prm : t->params) {
            std::string choices;
            for (const auto& c : prm.choices) choices += (choices.empty() ? "" : "|") + c;
            rows += owner + "\t" + std::to_string(i) + "\t" + t->name + "\t" + (rack[i].bypass ? "1" : "0") + "\t" + prm.name + "\t" +
                    num(rack[i].get(prm.name), 8) + "\t" + num(prm.min, 8) + "\t" + num(prm.max, 8) + "\t" + prm.unit + "\t" + choices + "\n";
        }
    }
    return rows;
}

std::vector<Automation> lanesOf(const RackRef& r) { return r.track ? trackLanes(*r.track) : *r.lanes; }

std::string lanesText(const std::vector<Automation>& lanes) {
    if (lanes.empty()) return "\n  (no automation)";
    std::string s;
    for (const auto& l : lanes) {
        s += "\n  " + l.target + ":";
        for (std::size_t i = 0; i < l.points.size() && i < 12; ++i) s += " " + num(l.points[i].time) + "s=" + num(l.points[i].value);
        if (l.points.size() > 12) s += " ... (" + std::to_string(l.points.size()) + " points)";
    }
    return s;
}

Result rackResult(const std::string& message, const ProjectDoc& d, const Params& p) {
    Result r = Result::success(message);
    r.with("project", stringParam(p, "project")).with("seconds", d.project.seconds());
    return maybePreview(r, d, p);
}

}  // namespace

void registerRackCommands(Session& session) {
    auto& reg = session.registry();

    reg.add({"audio.fx_add",
             "Add effect",
             "Add an effect to a track's rack (track=0) or the master's (master=true): eq, graphic_eq, filter, compressor, "
             "limiter, gate, expander, deesser, reverb, delay, chorus, flanger, phaser, tremolo, distortion or utility, "
             "with settings as text (values=\"threshold=-20 ratio=4\"; the EQ also takes values=\"bands=highpass 80, "
             "peaking 3000 -3 2\"). Effects run in order before the track's volume and pan; the sound changes only when "
             "it's played or mixed, so it can be changed or removed any time. Returns the effect's number.",
             concat(ownerParams(), {{"type", ParamType::String, "effect", true, effectTypeNames()},
                                    {"values", ParamType::String, "settings as name=value pairs (audio.fx_types lists them)", false},
                                    {"at", ParamType::Int, "position in the rack (default: the end)", false}, previewParam()}),
             true},
            projectCommand(session, [](ProjectDoc& d, const Params& p) {
                RackRef r;
                Effect e;
                std::string error;
                if (!rackArg(d, p, r, error) || !makeEffect(stringParam(p, "type"), stringParam(p, "values"), e, error)) return failed(error);
                if (r.rack->size() >= 32) return failed("a rack holds up to 32 effects");
                auto at = static_cast<std::size_t>(std::clamp<std::int64_t>(intParam(p, "at", static_cast<std::int64_t>(r.rack->size())), 0,
                                                                            static_cast<std::int64_t>(r.rack->size())));
                r.rack->insert(r.rack->begin() + static_cast<std::ptrdiff_t>(at), e);
                if (at + 1 < r.rack->size()) renumberLanes(*r.lanes, -1, static_cast<int>(at));
                return rackResult("added effect " + std::to_string(at) + " to " + r.name + ": " + describeEffect(e), d, p)
                    .with("fx", static_cast<std::int64_t>(at));
            }));

    reg.add({"audio.plugins",
             "Find plugins",
             "List the CLAP plugins on this computer (the standard plugin folders, WETWARESUITE_CLAP_PATH, and folder= "
             "if given). rows: key (what audio.plugin_add takes), id, name, vendor, version, kind (effect or instrument), "
             "features. Only effects can go in a rack; plugins run while mixing, without their own window.",
             {{"folder", ParamType::String, "another folder or .clap file to look in", false}},
             false, true},
            [](const Params& p) {
                const std::string folder = stringParam(p, "folder");
                return std::make_unique<FunctionAction>([folder] {
                    std::vector<std::filesystem::path> extra;
                    if (!folder.empty()) extra.emplace_back(folder);
                    const auto list = scanClapPlugins(extra);
                    std::string text, rows;
                    for (const auto& c : list) {
                        std::string features;
                        for (const auto& f : c.features) features += (features.empty() ? "" : ",") + f;
                        text += "\n  " + c.name + (c.vendor.empty() ? "" : " (" + c.vendor + ")") + (c.effect() ? "" : " [instrument]") + "  " + c.key();
                        rows += c.key() + "\t" + c.id + "\t" + c.name + "\t" + c.vendor + "\t" + c.version + "\t" + (c.effect() ? "effect" : "instrument") + "\t" + features + "\n";
                    }
                    std::string where;
                    for (const auto& d : clapSearchPaths()) where += "\n  " + d.string();
                    Result r = Result::success(list.empty() ? "no CLAP plugins found; looked in:" + where
                                                            : std::to_string(list.size()) + " plugin(s):" + text);
                    r.with("count", static_cast<std::int64_t>(list.size())).with("rows", rows);
                    return r;
                });
            });

    reg.add({"audio.plugin_params",
             "Plugin settings",
             "The settings of a CLAP plugin. rows: key (p<id>, for values= and automation targets like fx0.p7), name, min, max, default, stepped (0/1).",
             {{"plugin", ParamType::String, "a .clap file, key from audio.plugins, plugin id or name", true}},
             false, true},
            [](const Params& p) {
                const std::string what = stringParam(p, "plugin");
                return std::make_unique<FunctionAction>([what] {
                    ClapPluginInfo info;
                    std::vector<ClapParamInfo> params;
                    std::string error;
                    if (!findClapPlugin(what, info, error) || !clapParams(info.key(), params, error)) return Result::failure(error);
                    std::string text, rows;
                    for (const auto& c : params) {
                        text += "\n  p" + std::to_string(c.id) + " " + c.name + ": " + num(c.min) + " .. " + num(c.max) + " (default " + num(c.def) + ")";
                        rows += "p" + std::to_string(c.id) + "\t" + c.name + "\t" + num(c.min, 8) + "\t" + num(c.max, 8) + "\t" + num(c.def, 8) + "\t" +
                                (c.stepped ? "1" : "0") + "\n";
                    }
                    Result r = Result::success(info.name + ": " + std::to_string(params.size()) + " setting(s)" + text);
                    r.with("plugin", info.key()).with("name", info.name).with("count", static_cast<std::int64_t>(params.size())).with("rows", rows);
                    return r;
                });
            });

    reg.add({"audio.plugin_add",
             "Add plugin",
             "Put a CLAP effect plugin in a track's, bus's or the master's rack, like audio.fx_add: plugin= is a .clap "
             "file, a key from audio.plugins, a plugin id or its name; values=\"Gain=-6 Mix=0.5\" sets settings by name "
             "or p<id>. Settings save with the project, change with audio.fx_set and automate as fx<n>.p<id>. A project "
             "opened where the plugin is missing plays that slot as a pass-through.",
             concat(ownerParams(), {{"plugin", ParamType::String, "a .clap file, key from audio.plugins, plugin id or name", true},
                                    {"values", ParamType::String, "settings as name=value pairs (audio.plugin_params lists them)", false},
                                    {"at", ParamType::Int, "position in the rack (default: the end)", false}, previewParam()}),
             true},
            projectCommand(session, [](ProjectDoc& d, const Params& p) {
                RackRef r;
                Effect e;
                ClapPluginInfo info;
                std::string error;
                if (!rackArg(d, p, r, error) || !findClapPlugin(stringParam(p, "plugin"), info, error)) return failed(error);
                if (!info.effect()) return failed(info.name + " is an instrument, and only effect plugins can go in a rack");
                if (!makeClapEffect(info, stringParam(p, "values"), e, error)) return failed(error);
                if (r.rack->size() >= 32) return failed("a rack holds up to 32 effects");
                auto at = static_cast<std::size_t>(std::clamp<std::int64_t>(intParam(p, "at", static_cast<std::int64_t>(r.rack->size())), 0,
                                                                            static_cast<std::int64_t>(r.rack->size())));
                r.rack->insert(r.rack->begin() + static_cast<std::ptrdiff_t>(at), e);
                if (at + 1 < r.rack->size()) renumberLanes(*r.lanes, -1, static_cast<int>(at));
                return rackResult("added effect " + std::to_string(at) + " to " + r.name + ": " + describeEffect(e), d, p)
                    .with("fx", static_cast<std::int64_t>(at))
                    .with("plugin", info.key());
            }));

    reg.add({"audio.fx_set",
             "Effect settings",
             "Change settings of an effect in a rack (values=\"ratio=6 attack=3\"), or bypass it (bypass=true: kept but not heard).",
             concat(ownerParams(), {fxParam(), {"values", ParamType::String, "settings to change, name=value pairs", false},
                                    {"bypass", ParamType::Bool, "switch the effect off without removing it", false}, previewParam()}),
             true},
            projectCommand(session, [](ProjectDoc& d, const Params& p) {
                RackRef r;
                std::size_t i = 0;
                std::string error;
                if (!rackArg(d, p, r, error) || !fxArg(r, p, i, error)) return failed(error);
                Effect& e = (*r.rack)[i];
                if (!p.count("values") && !p.count("bypass")) return failed("give values= and/or bypass=");
                if (e.type == "clap") {
                    if (p.count("values") && !setClapValues(e, stringParam(p, "values"), error)) return failed(error);
                } else {
                    std::map<std::string, double> changed;
                    if (!parseEffectValues(e.type, stringParam(p, "values"), changed, error)) return failed(error);
                    for (const auto& [k, v] : changed) e.params[k] = v;
                }
                if (p.count("bypass")) e.bypass = boolParam(p, "bypass");
                return rackResult(r.name + " effect " + std::to_string(i) + " now " + describeEffect(e), d, p);
            }));

    reg.add({"audio.fx_remove",
             "Remove effect",
             "Take an effect out of a rack (later effects move up one number; their automation follows).",
             concat(ownerParams(), {fxParam()}),
             true},
            projectCommand(session, [](ProjectDoc& d, const Params& p) {
                RackRef r;
                std::size_t i = 0;
                std::string error;
                if (!rackArg(d, p, r, error) || !fxArg(r, p, i, error)) return failed(error);
                const std::string what = (*r.rack)[i].type;
                r.rack->erase(r.rack->begin() + static_cast<std::ptrdiff_t>(i));
                renumberLanes(*r.lanes, static_cast<int>(i), -1);
                return rackResult("removed " + what + " from " + r.name, d, p);
            }));

    reg.add({"audio.fx_move",
             "Move effect",
             "Change the order of a rack: move effect fx to position `to` (order matters: EQ before or after the compressor sounds different).",
             concat(ownerParams(), {fxParam(), {"to", ParamType::Int, "new position, from 0", true}}),
             true},
            projectCommand(session, [](ProjectDoc& d, const Params& p) {
                RackRef r;
                std::size_t i = 0;
                std::string error;
                if (!rackArg(d, p, r, error) || !fxArg(r, p, i, error)) return failed(error);
                const auto to = intParam(p, "to");
                if (to < 0 || to >= static_cast<std::int64_t>(r.rack->size())) return failed("to must be 0.." + std::to_string(r.rack->size() - 1));
                Effect e = (*r.rack)[i];
                r.rack->erase(r.rack->begin() + static_cast<std::ptrdiff_t>(i));
                r.rack->insert(r.rack->begin() + to, e);
                renumberLanes(*r.lanes, -1, -1, static_cast<int>(i), static_cast<int>(to));
                return rackResult(r.name + " rack:" + rackText(r), d, p);
            }));

    reg.add({"audio.fx_list",
             "List effects",
             "Show the effects racks: one track's, the master's, or (with neither) every rack in the project. rows has one "
             "line per setting: owner (track number or master), fx, type, bypassed (0/1), setting, value, min, max, unit, choices.",
             ownerParams(),
             false, true},
            projectCommand(
                session,
                [](ProjectDoc& d, const Params& p) {
                    std::string text, rows;
                    auto one = [&](const RackRef& r, const std::string& owner) {
                        text += "\n" + r.name + ":" + rackText(r);
                        rows += rackRows(owner, *r.rack);
                    };
                    if (p.count("track") || p.count("bus") || boolParam(p, "master", false)) {
                        RackRef r;
                        std::string error;
                        if (!rackArg(d, p, r, error)) return failed(error);
                        one(r, r.track ? std::to_string(intParam(p, "track")) : p.count("bus") ? "bus" + std::to_string(intParam(p, "bus")) : "master");
                    } else {
                        for (std::size_t t = 0; t < d.project.tracks.size(); ++t) {
                            Track& tr = d.project.tracks[t];
                            one({&tr.fx, &tr.automation, &tr, "track " + std::to_string(t) + (tr.name.empty() ? "" : " \"" + tr.name + "\"")}, std::to_string(t));
                        }
                        for (std::size_t b = 0; b < d.project.buses.size(); ++b) {
                            Bus& bus = d.project.buses[b];
                            one({&bus.fx, &bus.automation, nullptr, "bus " + std::to_string(b) + (bus.name.empty() ? "" : " \"" + bus.name + "\"")},
                                "bus" + std::to_string(b));
                        }
                        one({&d.project.masterFx, &d.project.masterAutomation, nullptr, "the master"}, "master");
                    }
                    return Result::success("effects" + text).with("rows", rows);
                },
                false));

    reg.add({"audio.automate",
             "Automate",
             "Draw automation: target=volume (dB), pan (-1..1) or an effect setting fx<number>.<setting> (fx0.cutoff, "
             "fx1.wet), as time,value pairs in seconds: points=0,200,4,8000 sweeps from 200 to 8000 over 4 s. The value "
             "moves in straight lines between points and holds before the first and after the last. mode=add merges "
             "points into the lane (replacing any at the same time), mode=clear removes the lane.",
             concat(ownerParams(), {{"target", ParamType::String, "volume, pan or fx<i>.<setting>", true},
                                    {"points", ParamType::NumberList, "time,value pairs", false},
                                    {"mode", ParamType::String, "what to do with the lane", false, {"replace", "add", "clear"}, std::string("replace")},
                                    previewParam()}),
             true},
            projectCommand(session, [](ProjectDoc& d, const Params& p) {
                RackRef r;
                std::string error;
                if (!rackArg(d, p, r, error)) return failed(error);
                const std::string target = stringParam(p, "target"), mode = stringParam(p, "mode", "replace");
                if (!validTarget(target, *r.rack, error)) return failed(error);
                std::vector<AutomationPoint> pts;
                if (mode != "clear") {
                    const NumberList v = listParam(p, "points");
                    if (v.empty() || v.size() % 2) return failed("points takes time,value pairs, e.g. points=0,0,2,-12");
                    double lo = 0, hi = 0;
                    targetRange(target, *r.rack, lo, hi);
                    for (std::size_t i = 0; i + 1 < v.size(); i += 2) {
                        if (v[i] < 0) return failed("automation times can't be negative");
                        if (v[i + 1] < lo - 1e-9 || v[i + 1] > hi + 1e-9) return failed(target + " values must be " + num(lo) + ".." + num(hi));
                        pts.push_back({v[i], v[i + 1]});
                    }
                    if (mode == "add") {
                        const auto lanes = lanesOf(r);
                        if (const Automation* old = findLane(lanes, target)) {
                            for (const auto& o : old->points) {
                                const bool replaced = std::any_of(pts.begin(), pts.end(), [&](const AutomationPoint& n) { return std::fabs(n.time - o.time) < 1e-6; });
                                if (!replaced) pts.push_back(o);
                            }
                        }
                    }
                }
                if (r.track) setTrackLane(*r.track, target, pts);
                else setLane(*r.lanes, target, pts);
                return rackResult(r.name + " automation:" + lanesText(lanesOf(r)), d, p).with("points", static_cast<std::int64_t>(pts.size()));
            }));

    reg.add({"audio.automation",
             "List automation",
             "Show the automation lanes of a track or the master (or of everything). rows: owner, target, time, value.",
             ownerParams(),
             false, true},
            projectCommand(
                session,
                [](ProjectDoc& d, const Params& p) {
                    std::string text, rows;
                    auto one = [&](const RackRef& r, const std::string& owner) {
                        const auto lanes = lanesOf(r);
                        text += "\n" + r.name + ":" + lanesText(lanes);
                        for (const auto& l : lanes)
                            for (const auto& pt : l.points) rows += owner + "\t" + l.target + "\t" + num(pt.time, 8) + "\t" + num(pt.value, 8) + "\n";
                    };
                    if (p.count("track") || boolParam(p, "master", false)) {
                        RackRef r;
                        std::string error;
                        if (!rackArg(d, p, r, error)) return failed(error);
                        one(r, r.track ? std::to_string(intParam(p, "track")) : "master");
                    } else {
                        for (std::size_t t = 0; t < d.project.tracks.size(); ++t) {
                            Track& tr = d.project.tracks[t];
                            one({&tr.fx, &tr.automation, &tr, "track " + std::to_string(t)}, std::to_string(t));
                        }
                        one({&d.project.masterFx, &d.project.masterAutomation, nullptr, "the master"}, "master");
                    }
                    return Result::success("automation" + text).with("rows", rows);
                },
                false));

    // ---- effects buses and sends ----
    reg.add({"audio.bus_add", "Add bus",
             "Add a shared effects bus (e.g. one reverb several tracks feed with audio.send). It starts with the effect "
             "given (fx=reverb) or empty; its rack takes the usual audio.fx_* commands with bus=. Returns its number.",
             {projectParam(), {"name", ParamType::String, "bus name", false}, {"fx", ParamType::String, "an effect to start with", false, effectTypeNames()},
              {"values", ParamType::String, "that effect's settings", false}, {"gain", ParamType::Number, "bus level in dB", false, {}, 0.0}, previewParam()},
             true},
            projectCommand(session, [](ProjectDoc& d, const Params& p) {
                if (d.project.buses.size() >= 16) return failed("a project holds up to 16 buses");
                Bus b;
                b.name = stringParam(p, "name", p.count("fx") ? stringParam(p, "fx") : "Bus " + std::to_string(d.project.buses.size() + 1));
                b.gainDb = numberParam(p, "gain", 0.0);
                if (p.count("fx")) {
                    Effect e;
                    std::string error;
                    if (!makeEffect(stringParam(p, "fx"), stringParam(p, "values"), e, error)) return failed(error);
                    if (e.type == "reverb" && !p.count("values")) e.params["dry"] = 0.0, e.params["wet"] = 1.0;  // a send bus is all wet
                    b.fx.push_back(e);
                }
                d.project.buses.push_back(b);
                const auto index = static_cast<std::int64_t>(d.project.buses.size() - 1);
                return rackResult("added bus " + std::to_string(index) + " \"" + b.name + "\"", d, p).with("bus", index);
            }));
    reg.add({"audio.bus_set", "Bus settings", "Rename a bus, change its level or mute it.",
             {projectParam(), {"bus", ParamType::Int, "bus number, from 0", true}, {"name", ParamType::String, "new name", false},
              {"gain", ParamType::Number, "level in dB", false}, {"mute", ParamType::Bool, "silence it", false}, previewParam()},
             true},
            projectCommand(session, [](ProjectDoc& d, const Params& p) {
                const auto i = intParam(p, "bus", -1);
                if (i < 0 || i >= static_cast<std::int64_t>(d.project.buses.size())) return failed("no bus " + std::to_string(i));
                Bus& b = d.project.buses[static_cast<std::size_t>(i)];
                if (p.count("name")) b.name = stringParam(p, "name");
                if (p.count("gain")) b.gainDb = std::clamp(numberParam(p, "gain"), -90.0, 24.0);
                if (p.count("mute")) b.mute = boolParam(p, "mute");
                return rackResult("bus " + std::to_string(i) + " \"" + b.name + "\": " + dbText(b.gainDb) + (b.mute ? ", muted" : ""), d, p);
            }));
    reg.add({"audio.bus_remove", "Remove bus", "Delete a bus; sends to it go too, and later buses move up one number.",
             {projectParam(), {"bus", ParamType::Int, "bus number, from 0", true}}, true},
            projectCommand(session, [](ProjectDoc& d, const Params& p) {
                const auto i = intParam(p, "bus", -1);
                if (i < 0 || i >= static_cast<std::int64_t>(d.project.buses.size())) return failed("no bus " + std::to_string(i));
                d.project.buses.erase(d.project.buses.begin() + i);
                for (auto& t : d.project.tracks) {
                    t.sends.erase(std::remove_if(t.sends.begin(), t.sends.end(), [&](const Send& snd) { return snd.bus == i; }), t.sends.end());
                    for (auto& snd : t.sends)
                        if (snd.bus > i) --snd.bus;
                }
                return rackResult("removed bus " + std::to_string(i), d, p);
            }));
    reg.add({"audio.send", "Send to bus",
             "Send part of a track (after its volume and pan) to a bus at level dB (0 = as loud as the track, -12 a "
             "little). off=true (or level -90 and below) removes the send.",
             {projectParam(), {"track", ParamType::Int, "track number, from 0", true}, {"bus", ParamType::Int, "bus number, from 0", true},
              {"level", ParamType::Number, "send level in dB", false, {}, -6.0}, {"off", ParamType::Bool, "remove the send", false, {}, false}, previewParam()},
             true},
            projectCommand(session, [](ProjectDoc& d, const Params& p) {
                const auto t = intParam(p, "track", -1), b = intParam(p, "bus", -1);
                if (t < 0 || t >= static_cast<std::int64_t>(d.project.tracks.size())) return failed("no track " + std::to_string(t));
                if (b < 0 || b >= static_cast<std::int64_t>(d.project.buses.size())) return failed("no bus " + std::to_string(b) + " (audio.bus_add makes one)");
                Track& tr = d.project.tracks[static_cast<std::size_t>(t)];
                const double level = numberParam(p, "level", -6.0);
                auto it = std::find_if(tr.sends.begin(), tr.sends.end(), [&](const Send& snd) { return snd.bus == b; });
                if (boolParam(p, "off", false) || level <= -90) {
                    if (it != tr.sends.end()) tr.sends.erase(it);
                    return rackResult("track " + std::to_string(t) + " no longer sends to bus " + std::to_string(b), d, p);
                }
                if (it == tr.sends.end()) tr.sends.push_back({static_cast<int>(b), level});
                else it->levelDb = level;
                return rackResult("track " + std::to_string(t) + " sends " + dbText(std::min(level, 24.0)) + " to bus " + std::to_string(b), d, p);
            }));
    reg.add({"audio.sends", "List sends", "Every bus and the tracks sending to it. rows: bus, name, gain dB, muted, track, send level dB (one row per send, or one with track -1 for a bus nobody feeds).",
             {projectParam()}, false, true},
            projectCommand(
                session,
                [](ProjectDoc& d, const Params&) {
                    std::string rows, text;
                    for (std::size_t b = 0; b < d.project.buses.size(); ++b) {
                        const Bus& bus = d.project.buses[b];
                        const std::string head = std::to_string(b) + "\t" + bus.name + "\t" + num(bus.gainDb, 6) + "\t" + (bus.mute ? "1" : "0") + "\t";
                        text += "\nbus " + std::to_string(b) + " \"" + bus.name + "\":";
                        bool any = false;
                        for (std::size_t t = 0; t < d.project.tracks.size(); ++t)
                            for (const auto& snd : d.project.tracks[t].sends)
                                if (snd.bus == static_cast<int>(b)) {
                                    rows += head + std::to_string(t) + "\t" + num(snd.levelDb, 6) + "\n";
                                    text += " track " + std::to_string(t) + " at " + dbText(snd.levelDb) + ";";
                                    any = true;
                                }
                        if (!any) rows += head + "-1\t0\n", text += " (nothing sent yet)";
                    }
                    return Result::success(d.project.buses.empty() ? "no buses yet (audio.bus_add)" : "buses" + text)
                        .with("rows", rows)
                        .with("buses", static_cast<std::int64_t>(d.project.buses.size()));
                },
                false));
}

}  // namespace wetwaresuite::audio::tool
