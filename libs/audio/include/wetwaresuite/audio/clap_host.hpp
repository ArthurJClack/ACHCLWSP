// Hosting CLAP plugins (https://cleveraudio.org, MIT-licensed headers) as rack effects.
// A plugin effect is an Effect of type "clap" whose `plugin` names the file and the
// plugin inside it ("path#id"); its settings are the plugin's parameters, stored as
// "p<param id>" so they save, undo and automate like built-in ones. Plugins render
// offline (no window, no real-time thread), which every CLAP effect supports.
#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "wetwaresuite/audio/rack.hpp"

namespace wetwaresuite::audio {

struct ClapPluginInfo {
    std::string path;  // the .clap file (a folder bundle on macOS)
    std::string id;    // e.g. "com.u-he.diva"
    std::string name, vendor, version, description;
    std::vector<std::string> features;  // "audio-effect", "instrument", "reverb", ...
    bool effect() const;
    std::string key() const { return path + "#" + id; }
};

struct ClapParamInfo {
    std::uint32_t id = 0;
    std::string name, module;
    double min = 0, max = 1, def = 0;
    bool stepped = false;
};

// The standard CLAP folders for this system, plus WETWARESUITE_CLAP_PATH (separated by ':' or ';').
std::vector<std::filesystem::path> clapSearchPaths();
// Every plugin in those folders (and `extra` files or folders).
std::vector<ClapPluginInfo> scanClapPlugins(const std::vector<std::filesystem::path>& extra = {});
// The plugins inside one .clap file.
bool clapPluginsIn(const std::filesystem::path& file, std::vector<ClapPluginInfo>& out, std::string& error);
// Finds a plugin by file path, "path#id", id or name (searching the standard folders).
bool findClapPlugin(const std::string& what, ClapPluginInfo& out, std::string& error);
// A plugin's parameters (cached after the first look).
bool clapParams(const std::string& key, std::vector<ClapParamInfo>& out, std::string& error);
// An Effect for the plugin with its parameters at their defaults, then `values`
// ("Gain=6 Mix=0.5", by parameter name or p<id>).
bool makeClapEffect(const ClapPluginInfo& plugin, const std::string& values, Effect& out, std::string& error);
// "Gain=6" etc. onto an existing clap effect's settings.
bool setClapValues(Effect& e, const std::string& values, std::string& error);
// A processor that runs the plugin (nullptr with error when it can't load).
std::unique_ptr<Processor> makeClapProcessor(const Effect& e, int rate, int channels, std::string* error = nullptr);
// The range of a clap effect's setting ("p7" or its name); false when it has none.
bool clapParamRange(const Effect& e, const std::string& setting, double& lo, double& hi, std::string& error);
// "clap: Name (Gain 6, Mix 0.5)".
std::string describeClapEffect(const Effect& e);

}  // namespace wetwaresuite::audio
