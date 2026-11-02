// cut.proxies: small copies of big (4K and up) video sources for smooth playback
// and scrubbing, made with ffmpeg and kept in the cache folder. Exports always
// read the originals.
#include <set>

#include "internal.hpp"
#include "proxy.hpp"

namespace wetwaresuite::cut {

namespace {

void bigSources(const Timeline& tl, std::set<std::string>& out, std::vector<std::pair<std::string, std::string>>& sizes) {
    for (const auto& t : tl.tracks) {
        for (const auto& c : t.clips) {
            if (c.kind == "nest" && c.nested) {
                bigSources(*c.nested, out, sizes);
            } else if (wantsProxy(c) && out.insert(c.source).second) {
                sizes.emplace_back(c.source, std::to_string(c.sourceWidth) + "x" + std::to_string(c.sourceHeight));
            }
        }
    }
}

}  // namespace

void registerProxyCommands(Session& session) {
    auto& reg = session.registry();
    reg.add({"cut.proxies", "Proxies",
             "Small copies of big video sources (taller than 1080 or wider than 1920 px) that playback, scrubbing and "
             "previews read instead, so 4K plays smoothly on a slow PC. Exports always use the originals. "
             "action=status lists the big sources and their proxies; make builds the missing ones (wait=false: in the "
             "background, the window does this by itself); on/off picks whether previews use them; clear deletes this "
             "timeline's proxies. rows: source, size, state (none, making, ready, failed: why), proxy file.",
             {docParam(),
              {"action", ParamType::String, "what to do", false, {"status", "make", "on", "off", "clear"}, std::string("status")},
              {"wait", ParamType::Bool, "make: wait until they're done (false: build them in the background)", false, {}, true}},
             false, true},
            cutCommand(
                session,
                [](Timeline& tl, const Params& p) {
                    const std::string action = stringParam(p, "action", "status");
                    std::set<std::string> sources;
                    std::vector<std::pair<std::string, std::string>> sizes;
                    bigSources(tl, sources, sizes);
                    std::string note;
                    if (action == "on" || action == "off") {
                        setUseProxies(action == "on");
                        note = action == "on" ? "previews use proxies where they're ready" : "previews read the original files";
                    } else if (action == "clear") {
                        const int n = removeProxies({sources.begin(), sources.end()});
                        note = "deleted " + std::to_string(n) + " prox" + (n == 1 ? "y" : "ies");
                    } else if (action == "make") {
                        int started = 0;
                        for (const auto& s : sources) {
                            if (!boolParam(p, "wait", true)) {
                                started += makeProxyLater(s) ? 1 : 0;
                            } else if (std::string err = makeProxy(s); !err.empty()) {
                                return Result::failure("couldn't make a proxy of " + s + ": " + err);
                            }
                        }
                        note = boolParam(p, "wait", true) ? "made the proxies" : "making " + std::to_string(started) + " in the background";
                    } else if (action != "status") {
                        return Result::failure("action is status, make, on, off or clear");
                    }
                    std::string rows, text;
                    int ready = 0;
                    for (const auto& [src, size] : sizes) {
                        const std::string state = proxyState(src);
                        ready += state == "ready" ? 1 : 0;
                        rows += src + "\t" + size + "\t" + state + "\t" + (state == "ready" ? proxyFileFor(src).string() : "") + "\n";
                        text += "\n  " + std::filesystem::path(src).filename().string() + " (" + size + "): " + state;
                    }
                    std::string head = sizes.empty() ? "no clips big enough to need proxies (they're for sources over 1080p)"
                                                     : std::to_string(ready) + " of " + std::to_string(sizes.size()) + " big source(s) have proxies";
                    head += useProxies() ? "; previews use them" : "; proxies are off";
                    Result r = Result::success((note.empty() ? "" : note + ". ") + head + text);
                    return r.with("rows", rows)
                        .with("count", static_cast<std::int64_t>(sizes.size()))
                        .with("ready", static_cast<std::int64_t>(ready))
                        .with("on", useProxies())
                        .with("folder", proxyFolder().string());
                },
                false));
}

}  // namespace wetwaresuite::cut
