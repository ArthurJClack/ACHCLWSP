// Proxies: small copies of big video sources that playback and previews read instead.
#pragma once

#include <filesystem>
#include <string>
#include <vector>

#include "wetwaresuite/cut/cut.hpp"

namespace wetwaresuite::cut {

constexpr int kProxyHeight = 540;  // proxy picture height
constexpr int kProxyAbove = 1080;  // sources taller than this (or wider than 1920) get one

std::filesystem::path proxyFolder();
std::filesystem::path proxyFileFor(const std::string& source);
bool wantsProxy(const Clip& clip);
bool proxyReady(const std::string& source);
// "none", "making", "ready" or "failed: why".
std::string proxyState(const std::string& source);
// Builds the proxy now (blocking). Returns an error, empty when it's there.
std::string makeProxy(const std::string& source);
// Builds it on a background thread; false when it's already there, being made or failed before.
bool makeProxyLater(const std::string& source);
bool useProxies();
void setUseProxies(bool on);
// The file previews should decode for `path`: its proxy when proxies are on and it's ready.
std::string previewSource(const std::string& path);
// Deletes the proxies of these sources; how many there were.
int removeProxies(const std::vector<std::string>& sources);

}  // namespace wetwaresuite::cut
