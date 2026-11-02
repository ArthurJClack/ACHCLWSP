// Proxies: small, quick-to-decode copies of big video sources (4K and up) that
// playback, scrubbing and previews read instead of the original. Exports always
// read the originals. They live in the user's cache folder, named after the
// source's path, size and change time, so an edited source gets a fresh one.
#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <mutex>
#include <thread>

#include "internal.hpp"
#include "media.hpp"
#include "proxy.hpp"

namespace wetwaresuite::cut {

namespace fs = std::filesystem;

namespace {

struct ProxyJobs {
    std::mutex lock;
    std::map<std::string, std::string> state;  // source -> "making", "failed: why"
    bool use = true;
};

ProxyJobs& jobs() {
    static auto* j = new ProxyJobs;  // never destroyed: background builds may outlive main
    return *j;
}

std::string env(const char* name) {
    const char* v = std::getenv(name);
    return v ? v : "";
}

std::uint64_t fnv(const std::string& s) {
    std::uint64_t h = 1469598103934665603ull;
    for (unsigned char c : s) h = (h ^ c) * 1099511628211ull;
    return h;
}

}  // namespace

fs::path proxyFolder() {
    if (!env("WETWARESUITE_PROXY_DIR").empty()) return env("WETWARESUITE_PROXY_DIR");
#if defined(_WIN32)
    if (!env("LOCALAPPDATA").empty()) return fs::path(env("LOCALAPPDATA")) / "wetwaresuite" / "cache" / "proxies";
#elif defined(__APPLE__)
    if (!env("HOME").empty()) return fs::path(env("HOME")) / "Library" / "Caches" / "wetwaresuite" / "proxies";
#else
    if (!env("XDG_CACHE_HOME").empty()) return fs::path(env("XDG_CACHE_HOME")) / "wetwaresuite" / "proxies";
    if (!env("HOME").empty()) return fs::path(env("HOME")) / ".cache" / "wetwaresuite" / "proxies";
#endif
    return fs::temp_directory_path() / "wetwaresuite-proxies";
}

fs::path proxyFileFor(const std::string& source) {
    std::error_code ec;
    const auto size = fs::file_size(source, ec);
    const auto when = fs::last_write_time(source, ec).time_since_epoch().count();
    const std::string key = fs::absolute(source, ec).string() + "|" + std::to_string(size) + "|" + std::to_string(when);
    char hex[17];
    std::snprintf(hex, sizeof hex, "%016llx", static_cast<unsigned long long>(fnv(key)));
    std::string stem = fs::path(source).stem().string().substr(0, 40);
    for (auto& c : stem)
        if (!std::isalnum(static_cast<unsigned char>(c)) && c != '-' && c != '_') c = '_';
    return proxyFolder() / (stem + "-" + hex + "-" + std::to_string(kProxyHeight) + "p.mp4");
}

bool wantsProxy(const Clip& c) {
    return c.kind == "video" && !c.source.empty() && (c.sourceHeight > kProxyAbove || c.sourceWidth > kProxyAbove * 16 / 9);
}

bool proxyReady(const std::string& source) {
    std::error_code ec;
    const fs::path p = proxyFileFor(source);
    return fs::is_regular_file(p, ec) && fs::file_size(p, ec) > 0;
}

std::string proxyState(const std::string& source) {
    {
        std::lock_guard<std::mutex> g(jobs().lock);
        auto it = jobs().state.find(source);
        if (it != jobs().state.end()) return it->second;
    }
    return proxyReady(source) ? "ready" : "none";
}

std::string makeProxy(const std::string& source) {
    if (!ffmpegPath()) return missingFfmpeg();
    std::error_code ec;
    if (!fs::is_regular_file(source, ec)) return "the source is missing: " + source;
    const fs::path out = proxyFileFor(source);
    if (proxyReady(source)) return {};
    fs::create_directories(out.parent_path(), ec);
    fs::path part = out;
    part += ".part.mp4";
    // small, all short GOPs (quick to seek), no sound: the mixer reads sound from the original
    const std::string err = runFfmpeg({"ffmpeg", "-hide_banner", "-nostdin", "-loglevel", "error", "-y", "-i", source, "-map", "0:v:0", "-an", "-sn", "-dn",
                                       "-vf", "scale=-2:" + std::to_string(kProxyHeight) + ":flags=fast_bilinear", "-c:v", "libx264", "-preset", "ultrafast",
                                       "-tune", "fastdecode", "-crf", "26", "-g", "12", "-pix_fmt", "yuv420p", part.string()});
    if (!err.empty()) {
        fs::remove(part, ec);
        return err;
    }
    fs::rename(part, out, ec);
    if (ec) {
        fs::remove(part, ec);
        return "couldn't save the proxy in " + out.parent_path().string();
    }
    return {};
}

bool makeProxyLater(const std::string& source) {
    {
        std::lock_guard<std::mutex> g(jobs().lock);
        auto it = jobs().state.find(source);
        if (it != jobs().state.end() && (it->second == "making" || it->second.rfind("failed", 0) == 0)) return false;
        if (proxyReady(source)) return false;
        jobs().state[source] = "making";
    }
    std::thread([source] {
        std::string err;
        try {
            err = makeProxy(source);
        } catch (const std::exception& e) {
            err = e.what();
        }
        std::lock_guard<std::mutex> g(jobs().lock);
        if (err.empty()) jobs().state.erase(source);
        else jobs().state[source] = "failed: " + err;
    }).detach();
    return true;
}

bool useProxies() {
    std::lock_guard<std::mutex> g(jobs().lock);
    return jobs().use;
}

void setUseProxies(bool on) {
    std::lock_guard<std::mutex> g(jobs().lock);
    jobs().use = on;
}

std::string previewSource(const std::string& path) {
    if (!useProxies()) return path;
    // asked for every frame: remember the answer, and look again for a missing one at most once a second
    struct Seen {
        std::string proxy;
        std::chrono::steady_clock::time_point at;
    };
    static std::mutex m;
    static std::map<std::string, Seen> seen;
    const auto now = std::chrono::steady_clock::now();
    {
        std::lock_guard<std::mutex> g(m);
        auto it = seen.find(path);
        if (it != seen.end() && (!it->second.proxy.empty() || now - it->second.at < std::chrono::seconds(1))) {
            if (it->second.proxy.empty()) return path;
            std::error_code ec;
            if (fs::is_regular_file(it->second.proxy, ec)) return it->second.proxy;
        }
    }
    const fs::path p = proxyFileFor(path);
    std::error_code ec;
    const bool ready = fs::is_regular_file(p, ec) && fs::file_size(p, ec) > 0;
    std::lock_guard<std::mutex> g(m);
    if (seen.size() > 256) seen.clear();
    seen[path] = {ready ? p.string() : std::string(), now};
    return ready ? p.string() : path;
}

int removeProxies(const std::vector<std::string>& sources) {
    int n = 0;
    std::error_code ec;
    for (const auto& s : sources) n += fs::remove(proxyFileFor(s), ec) ? 1 : 0;
    std::lock_guard<std::mutex> g(jobs().lock);
    for (const auto& s : sources) jobs().state.erase(s);
    return n;
}

}  // namespace wetwaresuite::cut
