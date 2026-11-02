#include "wetwaresuite/audio/clap_host.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <sstream>

#include <clap/clap.h>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <dlfcn.h>
#endif

namespace wetwaresuite::audio {

namespace fs = std::filesystem;

namespace {

std::string lowerText(std::string s) {
    for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

std::string numText(double v) {
    std::ostringstream o;
    o.precision(4);
    o << v;
    return o.str();
}

// ---- loading .clap files: each is opened once and kept for the life of the program ----

struct Library {
    void* handle = nullptr;
    const clap_plugin_entry_t* entry = nullptr;
    const clap_plugin_factory_t* factory = nullptr;
};

std::mutex& libMutex() {
    static std::mutex m;
    return m;
}

struct Libraries {
    std::map<std::string, Library> open;
    ~Libraries() {
        for (auto& [path, lib] : open)
            if (lib.entry) lib.entry->deinit();
        // the libraries themselves stay mapped: unloading a plugin's code at exit gains nothing
    }
};

Libraries& libraries() {
    static Libraries l;
    return l;
}

fs::path binaryOf(const fs::path& file) {
#if defined(__APPLE__)
    if (fs::is_directory(file)) return file / "Contents" / "MacOS" / file.stem();
#endif
    return file;
}

const Library* loadLibrary(const fs::path& file, std::string& error) {
    std::lock_guard lock(libMutex());
    const std::string key = file.string();
    auto& open = libraries().open;
    if (auto it = open.find(key); it != open.end()) return it->second.factory ? &it->second : (error = "couldn't load " + key, nullptr);
    Library lib;
    const fs::path bin = binaryOf(file);
#if defined(_WIN32)
    HMODULE h = LoadLibraryW(bin.wstring().c_str());
    lib.handle = h;
    if (h) lib.entry = reinterpret_cast<const clap_plugin_entry_t*>(GetProcAddress(h, "clap_entry"));
#else
    lib.handle = dlopen(bin.string().c_str(), RTLD_NOW | RTLD_LOCAL);
    if (lib.handle) lib.entry = static_cast<const clap_plugin_entry_t*>(dlsym(lib.handle, "clap_entry"));
#endif
    if (!lib.handle) {
        error = "couldn't load " + key + " (not a plugin for this system?)";
    } else if (!lib.entry) {
        error = key + " isn't a CLAP plugin (no clap_entry)";
    } else if (!clap_version_is_compatible(lib.entry->clap_version)) {
        error = key + " needs a newer CLAP version";
        lib.entry = nullptr;
    } else if (!lib.entry->init(file.string().c_str())) {
        error = key + " failed to start";
        lib.entry = nullptr;
    } else {
        lib.factory = static_cast<const clap_plugin_factory_t*>(lib.entry->get_factory(CLAP_PLUGIN_FACTORY_ID));
        if (!lib.factory) error = key + " has no plugins in it";
    }
    auto& slot = open[key] = lib;
    return slot.factory ? &slot : nullptr;
}

// ---- the host side the plugin talks to: a quiet offline host ----

const void* hostExtension(const clap_host_t*, const char*) { return nullptr; }
void hostNoop(const clap_host_t*) {}

const clap_host_t* theHost() {
    static const clap_host_t host = {CLAP_VERSION_INIT, nullptr, "wetwaresuite", "wetwaresuite", "https://github.com/ArthurJClack/wetwaresuite",
                                     "0.3", &hostExtension, &hostNoop, &hostNoop, &hostNoop};
    return &host;
}

struct EventList {
    std::vector<clap_event_param_value_t> events;
    clap_input_events_t in{this, &EventList::size, &EventList::get};
    static uint32_t size(const clap_input_events_t* l) { return static_cast<uint32_t>(static_cast<const EventList*>(l->ctx)->events.size()); }
    static const clap_event_header_t* get(const clap_input_events_t* l, uint32_t i) {
        const auto* self = static_cast<const EventList*>(l->ctx);
        return i < self->events.size() ? &self->events[i].header : nullptr;
    }
    void param(clap_id id, double value) {
        clap_event_param_value_t e{};
        e.header.size = sizeof e;
        e.header.time = 0;
        e.header.space_id = CLAP_CORE_EVENT_SPACE_ID;
        e.header.type = CLAP_EVENT_PARAM_VALUE;
        e.header.flags = 0;
        e.param_id = id;
        e.cookie = nullptr;
        e.note_id = -1;
        e.port_index = -1;
        e.channel = -1;
        e.key = -1;
        e.value = value;
        events.push_back(e);
    }
};

bool dropOutput(const clap_output_events_t*, const clap_event_header_t*) { return true; }
const clap_output_events_t kNoOutput{nullptr, &dropOutput};

// One live plugin instance.
struct Instance {
    const clap_plugin_t* plugin = nullptr;
    const clap_plugin_params_t* params = nullptr;
    const clap_plugin_audio_ports_t* ports = nullptr;
    bool active = false, processing = false;

    bool create(const Library& lib, const std::string& id, std::string& error) {
        plugin = lib.factory->create_plugin(lib.factory, theHost(), id.c_str());
        if (!plugin) return error = "couldn't create plugin " + id, false;
        if (!plugin->init(plugin)) {
            plugin->destroy(plugin);
            plugin = nullptr;
            return error = "plugin " + id + " failed to start", false;
        }
        params = static_cast<const clap_plugin_params_t*>(plugin->get_extension(plugin, CLAP_EXT_PARAMS));
        ports = static_cast<const clap_plugin_audio_ports_t*>(plugin->get_extension(plugin, CLAP_EXT_AUDIO_PORTS));
        return true;
    }
    ~Instance() {
        if (!plugin) return;
        if (processing) plugin->stop_processing(plugin);
        if (active) plugin->deactivate(plugin);
        plugin->destroy(plugin);
    }
    std::vector<ClapParamInfo> paramList() const {
        std::vector<ClapParamInfo> out;
        if (!params) return out;
        const uint32_t n = params->count(plugin);
        for (uint32_t i = 0; i < n; ++i) {
            clap_param_info_t info{};
            if (!params->get_info(plugin, i, &info)) continue;
            if (info.flags & (CLAP_PARAM_IS_READONLY | CLAP_PARAM_IS_HIDDEN)) continue;
            out.push_back({info.id, info.name, info.module, info.min_value, info.max_value, info.default_value, (info.flags & CLAP_PARAM_IS_STEPPED) != 0});
        }
        return out;
    }
};

bool splitKey(const std::string& key, std::string& path, std::string& id) {
    const auto hash = key.rfind('#');
    if (hash == std::string::npos) return false;
    path = key.substr(0, hash);
    id = key.substr(hash + 1);
    return !path.empty() && !id.empty();
}

std::map<std::string, std::vector<ClapParamInfo>>& paramCache() {
    static std::map<std::string, std::vector<ClapParamInfo>> c;
    return c;
}

std::string paramKey(std::uint32_t id) { return "p" + std::to_string(id); }

class ClapProcessor : public Processor {
public:
    ClapProcessor(std::unique_ptr<Instance> inst, int rate, int channels) : inst_(std::move(inst)), channels_(std::max(1, channels)) {
        const clap_plugin_t* p = inst_->plugin;
        // the plugin's audio ports: our sound goes in and comes out of the first of each
        auto portChannels = [&](bool input) {
            std::vector<uint32_t> v;
            if (inst_->ports) {
                const uint32_t n = inst_->ports->count(p, input);
                for (uint32_t i = 0; i < n; ++i) {
                    clap_audio_port_info_t info{};
                    v.push_back(inst_->ports->get(p, i, input, &info) ? std::max<uint32_t>(1, info.channel_count) : 2);
                }
            } else {
                v.push_back(2);
            }
            return v;
        };
        setup(inBufs_, inData_, inPtrs_, portChannels(true));
        setup(outBufs_, outData_, outPtrs_, portChannels(false));
        inst_->active = p->activate(p, rate, 1, kMaxBlock);
        if (inst_->active) inst_->processing = p->start_processing(p);
    }
    bool ok() const { return inst_->active; }

    void process(float* const* ch, int count, std::size_t n) override {
        if (!inst_->active) return;
        const clap_plugin_t* p = inst_->plugin;
        for (std::size_t at = 0; at < n; at += kMaxBlock) {
            const auto len = static_cast<uint32_t>(std::min<std::size_t>(kMaxBlock, n - at));
            // settings that changed go in as parameter events at the block start
            EventList events;
            if (dirty_) {
                for (const auto& [name, value] : v_) {
                    if (name.size() < 2 || name[0] != 'p') continue;
                    auto it = sent_.find(name);
                    if (it != sent_.end() && it->second == value) continue;
                    events.param(static_cast<clap_id>(std::strtoul(name.c_str() + 1, nullptr, 10)), value);
                    sent_[name] = value;
                }
                dirty_ = false;
            }
            for (auto& port : inData_)
                for (auto& c : port) std::fill(c.begin(), c.begin() + len, 0.0f);
            if (!inData_.empty()) {
                auto& main = inData_[0];
                for (std::size_t c = 0; c < main.size(); ++c) {
                    const float* src = ch[std::min<std::size_t>(c, static_cast<std::size_t>(count) - 1)] + at;  // mono feeds every input channel
                    std::copy(src, src + len, main[c].begin());
                }
            }
            clap_process_t proc{};
            proc.steady_time = steady_;
            proc.frames_count = len;
            proc.transport = nullptr;
            proc.audio_inputs = inBufs_.data();
            proc.audio_inputs_count = static_cast<uint32_t>(inBufs_.size());
            proc.audio_outputs = outBufs_.data();
            proc.audio_outputs_count = static_cast<uint32_t>(outBufs_.size());
            proc.in_events = &events.in;
            proc.out_events = &kNoOutput;
            const clap_process_status st = inst_->processing ? p->process(p, &proc) : CLAP_PROCESS_ERROR;
            steady_ += len;
            if (st == CLAP_PROCESS_ERROR || outData_.empty()) continue;  // the sound passes through untouched
            const auto& out = outData_[0];
            for (int c = 0; c < count; ++c) {
                float* dst = ch[c] + at;
                if (out.size() == 1 || (count == 1 && out.size() >= 2)) {
                    // mono plugin into many channels, or a stereo plugin back to mono (average)
                    for (uint32_t i = 0; i < len; ++i) {
                        float v = 0;
                        for (const auto& oc : out) v += oc[i];
                        dst[i] = count == 1 ? v / static_cast<float>(out.size()) : out[0][i];
                    }
                } else {
                    const auto& oc = out[std::min<std::size_t>(static_cast<std::size_t>(c), out.size() - 1)];
                    std::copy(oc.begin(), oc.begin() + len, dst);
                }
            }
        }
    }

private:
    static constexpr uint32_t kMaxBlock = 1024;
    void setup(std::vector<clap_audio_buffer_t>& bufs, std::vector<std::vector<std::vector<float>>>& data, std::vector<std::vector<float*>>& ptrs,
               const std::vector<uint32_t>& channels) {
        data.assign(channels.size(), {});
        ptrs.assign(channels.size(), {});
        for (std::size_t i = 0; i < channels.size(); ++i) {
            data[i].assign(channels[i], std::vector<float>(kMaxBlock, 0.0f));
            for (auto& c : data[i]) ptrs[i].push_back(c.data());
        }
        for (std::size_t i = 0; i < channels.size(); ++i) {
            clap_audio_buffer_t b{};
            b.data32 = ptrs[i].data();
            b.data64 = nullptr;
            b.channel_count = channels[i];
            b.latency = 0;
            b.constant_mask = 0;
            bufs.push_back(b);
        }
    }
    std::unique_ptr<Instance> inst_;
    int channels_;
    int64_t steady_ = 0;
    std::map<std::string, double> sent_;
    std::vector<clap_audio_buffer_t> inBufs_, outBufs_;
    std::vector<std::vector<std::vector<float>>> inData_, outData_;
    std::vector<std::vector<float*>> inPtrs_, outPtrs_;
};

}  // namespace

bool ClapPluginInfo::effect() const {
    return std::find(features.begin(), features.end(), CLAP_PLUGIN_FEATURE_AUDIO_EFFECT) != features.end() ||
           std::find(features.begin(), features.end(), CLAP_PLUGIN_FEATURE_ANALYZER) != features.end();
}

std::vector<fs::path> clapSearchPaths() {
    std::vector<fs::path> out;
    auto env = [](const char* name) -> std::string {
        const char* v = std::getenv(name);
        return v ? v : "";
    };
    const std::string extra = env("WETWARESUITE_CLAP_PATH") + (env("CLAP_PATH").empty() ? "" : ";" + env("CLAP_PATH"));
    std::string part;
    for (char c : extra + ";") {
#if defined(_WIN32)
        const bool sep = c == ';';
#else
        const bool sep = c == ';' || c == ':';
#endif
        if (!sep) part += c;
        else if (!part.empty()) out.emplace_back(part), part.clear();
    }
#if defined(_WIN32)
    if (!env("COMMONPROGRAMFILES").empty()) out.push_back(fs::path(env("COMMONPROGRAMFILES")) / "CLAP");
    if (!env("LOCALAPPDATA").empty()) out.push_back(fs::path(env("LOCALAPPDATA")) / "Programs" / "Common" / "CLAP");
#elif defined(__APPLE__)
    if (!env("HOME").empty()) out.push_back(fs::path(env("HOME")) / "Library" / "Audio" / "Plug-Ins" / "CLAP");
    out.push_back("/Library/Audio/Plug-Ins/CLAP");
#else
    if (!env("HOME").empty()) out.push_back(fs::path(env("HOME")) / ".clap");
    out.push_back("/usr/lib/clap");
    out.push_back("/usr/local/lib/clap");
#endif
    return out;
}

bool clapPluginsIn(const fs::path& file, std::vector<ClapPluginInfo>& out, std::string& error) {
    const Library* lib = loadLibrary(file, error);
    if (!lib) return false;
    const uint32_t n = lib->factory->get_plugin_count(lib->factory);
    for (uint32_t i = 0; i < n; ++i) {
        const clap_plugin_descriptor_t* d = lib->factory->get_plugin_descriptor(lib->factory, i);
        if (!d || !d->id) continue;
        ClapPluginInfo info;
        info.path = file.string();
        info.id = d->id;
        info.name = d->name ? d->name : d->id;
        info.vendor = d->vendor ? d->vendor : "";
        info.version = d->version ? d->version : "";
        info.description = d->description ? d->description : "";
        for (const char* const* f = d->features; f && *f; ++f) info.features.emplace_back(*f);
        out.push_back(std::move(info));
    }
    return true;
}

std::vector<ClapPluginInfo> scanClapPlugins(const std::vector<fs::path>& extra) {
    std::vector<ClapPluginInfo> out;
    std::vector<fs::path> roots = extra;
    for (auto& p : clapSearchPaths()) roots.push_back(p);
    std::vector<std::string> seen;
    auto take = [&](const fs::path& f) {
        const std::string key = fs::weakly_canonical(f).string();
        if (std::find(seen.begin(), seen.end(), key) != seen.end()) return;
        seen.push_back(key);
        std::string error;
        clapPluginsIn(f, out, error);  // a broken file is skipped
    };
    for (const auto& root : roots) {
        std::error_code ec;
        if (lowerText(root.extension().string()) == ".clap" && fs::exists(root, ec)) {
            take(root);
            continue;
        }
        if (!fs::is_directory(root, ec)) continue;
        for (auto it = fs::recursive_directory_iterator(root, fs::directory_options::skip_permission_denied, ec); it != fs::recursive_directory_iterator();
             it.increment(ec)) {
            if (ec) break;
            if (lowerText(it->path().extension().string()) != ".clap") continue;
            take(it->path());
            if (it->is_directory(ec)) it.disable_recursion_pending();  // a macOS bundle
        }
    }
    return out;
}

bool findClapPlugin(const std::string& what, ClapPluginInfo& out, std::string& error) {
    std::string path, id;
    if (splitKey(what, path, id)) {
        std::vector<ClapPluginInfo> list;
        if (!clapPluginsIn(path, list, error)) return false;
        for (const auto& p : list)
            if (p.id == id) return out = p, true;
        error = path + " has no plugin " + id;
        return false;
    }
    std::error_code ec;
    if (fs::exists(what, ec)) {
        std::vector<ClapPluginInfo> list;
        if (!clapPluginsIn(what, list, error)) return false;
        if (list.empty()) return error = what + " has no plugins in it", false;
        for (const auto& p : list)
            if (p.effect()) return out = p, true;
        out = list.front();
        return true;
    }
    const std::string want = lowerText(what);
    for (const auto& p : scanClapPlugins())
        if (lowerText(p.id) == want || lowerText(p.name) == want) return out = p, true;
    error = "no CLAP plugin called \"" + what + "\" (audio.clap_scan lists them; or give its file path)";
    return false;
}

bool clapParams(const std::string& key, std::vector<ClapParamInfo>& out, std::string& error) {
    {
        std::lock_guard lock(libMutex());
        if (auto it = paramCache().find(key); it != paramCache().end()) return out = it->second, true;
    }
    std::string path, id;
    if (!splitKey(key, path, id)) return error = "not a plugin reference: " + key, false;
    const Library* lib = loadLibrary(path, error);
    if (!lib) return false;
    Instance inst;
    if (!inst.create(*lib, id, error)) return false;
    out = inst.paramList();
    std::lock_guard lock(libMutex());
    paramCache()[key] = out;
    return true;
}

bool setClapValues(Effect& e, const std::string& values, std::string& error) {
    std::vector<ClapParamInfo> params;
    if (!clapParams(e.plugin, params, error)) return false;
    std::string text = values;
    for (auto& c : text)
        if (c == ',' || c == ';') c = ' ';
    // names can hold spaces ("Dry Wet=0.5"): split on '=' and take the last word run before it
    std::size_t pos = 0;
    while (pos < text.size()) {
        const auto eq = text.find('=', pos);
        if (eq == std::string::npos) break;
        std::string name = text.substr(pos, eq - pos);
        auto valEnd = text.find(' ', eq + 1);
        if (valEnd == std::string::npos) valEnd = text.size();
        const std::string val = text.substr(eq + 1, valEnd - eq - 1);
        pos = valEnd + 1;
        while (!name.empty() && name.front() == ' ') name.erase(0, 1);
        while (!name.empty() && name.back() == ' ') name.pop_back();
        const ClapParamInfo* hit = nullptr;
        for (const auto& p : params)
            if (lowerText(p.name) == lowerText(name) || paramKey(p.id) == name) hit = &p;
        if (!hit) {
            std::string names;
            for (const auto& p : params) names += (names.empty() ? "" : ", ") + p.name;
            error = "the plugin has no setting \"" + name + "\" (it has: " + (names.empty() ? "none" : names) + ")";
            return false;
        }
        char* end = nullptr;
        const double v = std::strtod(val.c_str(), &end);
        if (end == val.c_str()) return error = "\"" + val + "\" isn't a number for " + hit->name, false;
        if (v < hit->min - 1e-9 || v > hit->max + 1e-9) {
            error = hit->name + " goes from " + numText(hit->min) + " to " + numText(hit->max);
            return false;
        }
        e.params[paramKey(hit->id)] = v;
    }
    return true;
}

bool makeClapEffect(const ClapPluginInfo& plugin, const std::string& values, Effect& out, std::string& error) {
    Effect e;
    e.type = "clap";
    e.plugin = plugin.key();
    std::vector<ClapParamInfo> params;
    if (!clapParams(e.plugin, params, error)) return false;
    for (const auto& p : params) e.params[paramKey(p.id)] = p.def;
    if (!values.empty() && !setClapValues(e, values, error)) return false;
    out = std::move(e);
    return true;
}

std::unique_ptr<Processor> makeClapProcessor(const Effect& e, int rate, int channels, std::string* error) {
    std::string err, path, id;
    if (!splitKey(e.plugin, path, id)) {
        if (error) *error = "the effect names no plugin";
        return nullptr;
    }
    const Library* lib = loadLibrary(path, err);
    auto inst = std::make_unique<Instance>();
    if (!lib || !inst->create(*lib, id, err)) {
        if (error) *error = err;
        return nullptr;
    }
    auto proc = std::make_unique<ClapProcessor>(std::move(inst), rate, channels);
    if (!proc->ok()) {
        if (error) *error = "plugin " + id + " couldn't start at " + std::to_string(rate) + " Hz";
        return nullptr;
    }
    for (const auto& [k, v] : e.params) proc->set(k, v);
    return proc;
}

bool clapParamRange(const Effect& e, const std::string& setting, double& lo, double& hi, std::string& error) {
    std::vector<ClapParamInfo> params;
    if (!clapParams(e.plugin, params, error)) return false;
    for (const auto& p : params)
        if (paramKey(p.id) == setting || lowerText(p.name) == lowerText(setting)) {
            lo = p.min, hi = p.max;
            return true;
        }
    std::string names;
    for (const auto& p : params) names += (names.empty() ? "" : ", ") + paramKey(p.id) + " (" + p.name + ")";
    error = "the plugin has no setting \"" + setting + "\" (it has: " + (names.empty() ? "none" : names) + ")";
    return false;
}

std::string describeClapEffect(const Effect& e) {
    std::string path, id, error;
    splitKey(e.plugin, path, id);
    std::string s = "clap: " + (id.empty() ? std::string("(no plugin)") : id);
    std::vector<ClapParamInfo> params;
    if (clapParams(e.plugin, params, error)) {
        bool first = true;
        for (const auto& p : params) {
            s += (first ? " (" : ", ") + p.name + " " + numText(e.get(paramKey(p.id)));
            first = false;
        }
        if (!first) s += ")";
    } else {
        s += " (can't load: " + error + ")";
    }
    if (e.bypass) s += " (bypassed)";
    return s;
}

}  // namespace wetwaresuite::audio
