// The tool panels' shared parts: which tools have one, the "Open" list on the
// left, polling for timers, alarms and running programs, and small helpers.
#include "tool_panels.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>

#include "imgui_internal.h"
#include "wetwaresuite/raster/codec.hpp"

namespace wetwaresuite::gui {

ImFont* Shell::monoFont = nullptr;

namespace tp {

std::string text(const Result& r, const std::string& key) {
    const Value* v = r.field(key);
    return v ? toText(*v) : std::string();
}

double number(const Result& r, const std::string& key, double fallback) {
    const Value* v = r.field(key);
    if (!v) return fallback;
    if (auto d = std::get_if<double>(v)) return *d;
    if (auto i = std::get_if<std::int64_t>(v)) return static_cast<double>(*i);
    if (auto b = std::get_if<bool>(v)) return *b ? 1 : 0;
    if (auto s = std::get_if<std::string>(v)) {
        try {
            return std::stod(*s);
        } catch (...) {
            return fallback;
        }
    }
    return fallback;
}

std::vector<double> numbers(const Result& r, const std::string& key) {
    const Value* v = r.field(key);
    if (!v) return {};
    if (auto l = std::get_if<NumberList>(v)) return *l;
    return {};
}

Rows splitRows(const std::string& text) {
    Rows out;
    std::size_t start = 0;
    while (start < text.size()) {
        std::size_t end = text.find('\n', start);
        if (end == std::string::npos) end = text.size();
        std::string line = text.substr(start, end - start);
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (!line.empty()) {
            std::vector<std::string> cells;
            std::size_t a = 0;
            while (true) {
                std::size_t b = line.find('\t', a);
                cells.push_back(line.substr(a, b == std::string::npos ? std::string::npos : b - a));
                if (b == std::string::npos) break;
                a = b + 1;
            }
            out.push_back(std::move(cells));
        }
        start = end + 1;
    }
    return out;
}

Rows rows(const Result& r, const std::string& key) { return splitRows(text(r, key)); }

std::string baseName(const std::string& path) {
    std::string p = path;
    while (p.size() > 1 && (p.back() == '/' || p.back() == '\\')) p.pop_back();
    auto slash = p.find_last_of("/\\");
    return slash == std::string::npos ? p : p.substr(slash + 1);
}

std::string parentOf(const std::string& path) {
    std::string p = path;
    while (p.size() > 1 && (p.back() == '/' || p.back() == '\\')) p.pop_back();
    auto slash = p.find_last_of("/\\");
    if (slash == std::string::npos) return p;
    if (slash == 0) return p.substr(0, 1);
    if (slash == 2 && p[1] == ':') return p.substr(0, 3);  // C:\ stays C:\.
    return p.substr(0, slash);
}

std::string joinPath(const std::string& folder, const std::string& name) {
    if (folder.empty()) return name;
    const char last = folder.back();
    if (last == '/' || last == '\\') return folder + name;
#ifdef _WIN32
    return folder + "\\" + name;
#else
    return folder + "/" + name;
#endif
}

std::string lowerAscii(std::string s) {
    for (char& c : s) {
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    }
    return s;
}

std::string humanSize(double bytes) {
    const char* units[] = {"bytes", "KB", "MB", "GB", "TB", "PB"};
    int u = 0;
    while (bytes >= 1024 && u < 5) {
        bytes /= 1024;
        ++u;
    }
    char buf[32];
    if (u == 0) std::snprintf(buf, sizeof buf, "%.0f %s", bytes, units[u]);
    else std::snprintf(buf, sizeof buf, "%.1f %s", bytes, units[u]);
    return buf;
}

std::filesystem::path pathOf(const std::string& utf8) { return std::filesystem::path(std::u8string(utf8.begin(), utf8.end())); }

double seconds() {
    using namespace std::chrono;
    return duration<double>(steady_clock::now().time_since_epoch()).count();
}

void muted(const std::string& t) {
    if (t.empty()) return;
    ImGui::PushTextWrapPos(0.0f);
    ImGui::TextDisabled("%s", t.c_str());
    ImGui::PopTextWrapPos();
}

void title(const std::string& t, float scale) {
    ImGui::PushFont(nullptr, ImGui::GetStyle().FontSizeBase * scale);
    ImGui::TextUnformatted(t.c_str());
    ImGui::PopFont();
}

void errorText(const std::string& t) {
    if (t.empty()) return;
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4{0.95f, 0.45f, 0.40f, 1});
    ImGui::PushTextWrapPos(0.0f);
    ImGui::TextUnformatted(t.c_str());
    ImGui::PopTextWrapPos();
    ImGui::PopStyleColor();
}

bool attachmentTexture(SDL_Renderer* renderer, Texture& tex, const Result& r) {
    for (const auto& a : r.attachments) {
        if (a.mime != "image/png" && a.mime != "image/jpeg") continue;
        try {
            raster::Image img = raster::decode(a.bytes);
            return tex.upload(renderer, img.data(), img.width(), img.height());
        } catch (...) {
            return false;
        }
    }
    return false;
}

void fitImage(const Texture& tex, float w, float h) {
    if (!tex.tex || tex.srcWidth <= 0 || tex.srcHeight <= 0) return;
    const float s = std::min({w / static_cast<float>(tex.srcWidth), h / static_cast<float>(tex.srcHeight), 1.0f});
    ImGui::Image(ImTextureRef(tex.id()), {static_cast<float>(tex.srcWidth) * s, static_cast<float>(tex.srcHeight) * s});
}

void flow(const char* label, bool first) {
    if (first) return;
    const ImGuiStyle& st = ImGui::GetStyle();
    const char* end = ImGui::FindRenderedTextEnd(label);
    const float w = ImGui::CalcTextSize(label, end).x + st.FramePadding.x * 2;
    const float right = ImGui::GetCurrentWindow()->WorkRect.Max.x;
    ImGui::SameLine();
    if (ImGui::GetCursorScreenPos().x + w > right) ImGui::NewLine();
}

}  // namespace tp

namespace {

struct PanelName {
    const char* tool;
    const char* simple;    // Simple mode label
    const char* advanced;  // Advanced mode label
};

const PanelName kPanels[] = {
    {"calc", "Calculator", "Calculator"},   {"code", "Code editor", "Code (IDE)"},
    {"explorer", "Files", "File manager"},  {"terminal", "Terminal", "Terminal"},
    {"monitor", "This computer", "System monitor"}, {"clock", "Clock and timers", "Clock"},
    {"pipeline", "Pipeline builder", "Pipeline builder"},  // automate's; shown when automate is in the app
};

}  // namespace

bool Shell::hasPanel(const std::string& tool) const {
    if (tool == "pipeline") return std::find(tools_.begin(), tools_.end(), "automate") != tools_.end();
    for (const auto& p : kPanels) {
        if (tool == p.tool) return std::find(tools_.begin(), tools_.end(), tool) != tools_.end();
    }
    return false;
}

std::string Shell::panelForDoc(const std::string& id) const {
    auto doc = session_.workspace().get(id);
    if (!doc) return {};
    const std::string kind = doc->kind();
    if (kind == "code" && hasPanel("code")) return "code";
    if (kind == "terminal" && hasPanel("terminal")) return "terminal";
    return {};
}

void Shell::openPanel(const std::string& tool) {
    if (!hasPanel(tool)) return;
    panel_ = tool;
    showHome_ = false;
    if (tool == "terminal" && panels_->terminal.tabs.empty()) {
        // reuse open terminals, else start one at home
        for (const auto& id : docIds()) {
            if (panelForDoc(id) == "terminal") panels_->terminal.tabs.push_back({id});
        }
        if (panels_->terminal.tabs.empty()) terminalNew("");
        else panels_->terminal.current = panels_->terminal.tabs.back().doc;
    }
    if (tool == "terminal") panels_->terminal.focusInput = true;
    if (tool == "calc") panels_->calc.focusInput = true;
}

void Shell::openPanelFor(const std::string& commandId) {
    const std::string tool = commandId.substr(0, commandId.find('.'));
    if (!hasPanel(tool)) return;
    openPanel(tool);
    // the part of the panel the command belongs to
    const std::string verb = commandId.substr(commandId.find('.') + 1);
    auto starts = [&](const char* p) { return verb.rfind(p, 0) == 0; };
    if (tool == "calc") {
        if (verb == "graph" || verb == "sample" || verb == "table") panels_->calc.pickTab = 1;
        else if (verb == "convert" || verb == "units") panels_->calc.pickTab = 2;
        else if (verb == "stats" || verb == "regression") panels_->calc.pickTab = pro_ ? 3 : 0;
        else if (verb == "solve") panels_->calc.pickTab = pro_ ? 4 : 0;
        else if (verb == "base" || verb == "factor") panels_->calc.pickTab = pro_ ? 5 : 0;
        else panels_->calc.pickTab = 0;
    } else if (tool == "clock") {
        int t = 0;
        if (verb == "world" || verb == "zones") t = 1;
        else if (starts("timer")) t = 2;
        else if (verb == "stopwatch") t = 3;
        else if (starts("alarm")) t = 4;
        else if (verb == "calendar") t = 5;
        else if (starts("date") || verb == "convert") t = 6;
        panels_->clock.pickTab = t;
    } else if (tool == "monitor") {
        panels_->monitor.pickTab = (starts("process") || verb == "kill") ? 1 : 0;
    } else if (tool == "explorer" && (verb == "trash" || verb == "restore" || verb == "empty_trash")) {
        explorerRefresh();
        panels_->explorer.trashView = true;
        panels_->explorer.trash = tp::rows(query("explorer.trash", {}));
    } else if (tool == "code" && starts("git")) {
        panels_->code.pickSide = 3;
    }
}

bool Shell::openFolder(const std::string& path) {
    std::error_code ec;
    if (path.empty() || !std::filesystem::is_directory(tp::pathOf(path), ec)) return false;
    if (hasPanel("explorer")) {
        openPanel("explorer");
        explorerGo(path);
        return true;
    }
    if (hasPanel("code")) {
        Result r = run("code.project", {{"folder", path}});
        panels_->code.treeStale = true;
        openPanel("code");
        result_ = r;
        resultFor_ = "Open folder";
        return true;
    }
    return false;
}

Result Shell::query(const std::string& id, const Params& params) {
    if (!find(id)) return Result::failure("this app has no " + id);
    return session_.run(id, params);
}

void Shell::toolPanelView() {
    if (panel_ == "calc") calcPanel();
    else if (panel_ == "code") codePanel();
    else if (panel_ == "explorer") explorerPanel();
    else if (panel_ == "terminal") terminalPanel();
    else if (panel_ == "monitor") monitorPanel();
    else if (panel_ == "clock") clockPanel();
    else if (panel_ == "pipeline") pipelinePanel();
    else homeView();
}

bool Shell::settling() {
    std::string key = std::to_string(changes_) + "|" + currentDoc_ + "|" + panel_ + (showHome_ ? "|home" : "|doc") + "|" +
                      std::to_string(docIds().size()) + (form_ ? "|" + form_->command : std::string()) + "|" +
                      std::to_string(ImGui::GetActiveID()) + "|" + std::to_string(ImGui::GetIO().WantTextInput);
    if (key == settledKey_) return false;
    settledKey_ = std::move(key);
    return true;
}

int Shell::tickMs() const {
    if (transport_.playing && !showHome_) return 15;  // the playhead (and a timeline's picture) moves
    if (canvas_.game.playing && !showHome_) return 20;
    if (dope_.playing && !showHome_) return 15;  // a model's animation is playing (dopesheet.cpp)  // a sprite animation is playing (game_view.cpp)
    if (video_ && video_->pending() && !showHome_) return 15;  // a timeline frame is being drawn
    if (!exportJob_.id.empty()) return 100;           // the export's progress bar
    if (stripsPending_ && !showHome_) return 150;     // thumbnails still coming in
    if (!panels_) return 0;
    const bool visible = !showHome_;
    int ms = 0;
    auto want = [&](int t) { ms = ms == 0 ? t : std::min(ms, t); };
    const ClockPanel& c = panels_->clock;
    if (hasClock_) {
        bool waiting = !c.alarms.empty();
        for (const auto& t : c.timers) waiting = waiting || (t.size() > 3 && t[3] == "running");
        if (waiting) want(1000);
        if (visible && panel_ == "clock") want(c.swRunning ? 100 : 1000);
    }
    if (visible && panel_ == "monitor") want(1000);
    for (const auto& t : panels_->terminal.tabs) {
        if (t.running) want(100);
    }
    if (panels_->code.jobRunning) want(200);
    if (panels_->code.editor.dirty) want(250);
    return ms;
}

void Shell::backgroundTicks() {
    const double now = tp::seconds();
    ClockPanel& c = panels_->clock;
    if (hasClock_) {
        if (!c.alarmsLoaded) {
            c.alarms = tp::rows(query("clock.alarms", {}));
            c.alarmsLoaded = true;
        }
        bool waiting = !c.alarms.empty();
        for (const auto& t : c.timers) waiting = waiting || (t.size() > 3 && t[3] == "running");
        if (waiting && now - c.lastDue >= 1.0) {
            c.lastDue = now;
            Result due = query("clock.due", {});
            for (const auto& row : tp::rows(due)) {
                if (row.size() < 2) continue;
                const std::string label = row.size() > 2 && !row[2].empty() ? row[2] : row[1];
                c.due.push_back(row[0] == "timer" ? "Time's up: " + label : "Alarm: " + label);
            }
            if (!tp::rows(due).empty()) {
                SDL_FlashWindow(window_, SDL_FLASH_UNTIL_FOCUSED);
                c.timers = tp::rows(query("clock.timers", {}));
                c.alarms = tp::rows(query("clock.alarms", {}));
            }
        }
    }
    terminalPoll(false);
    codePoll();
    CodeEditor& ed = panels_->code.editor;
    if (ed.dirty && now - ed.lastEdit > 0.6) codeCommit();
}

void Shell::beforeHistoryStep() {
    if (panels_ && panels_->code.editor.dirty) codeCommit();
}

void Shell::dueBanner() {
    auto& due = panels_->clock.due;
    if (due.empty()) return;
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos({vp->WorkPos.x + vp->WorkSize.x * 0.5f, vp->WorkPos.y + ImGui::GetFontSize() * 4}, ImGuiCond_Always, {0.5f, 0});
    ImGui::SetNextWindowBgAlpha(0.97f);
    ImGui::Begin("##due", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings |
                     ImGuiWindowFlags_NoFocusOnAppearing);
    for (const auto& line : due) tp::title(line, 1.4f);
    if (accentButton("OK", {ImGui::GetFontSize() * 8, 0})) due.clear();
    if (hasPanel("clock")) {
        ImGui::SameLine();
        if (ImGui::Button("Show the clock")) {
            due.clear();
            openPanel("clock");
        }
    }
    ImGui::End();
}

}  // namespace wetwaresuite::gui
