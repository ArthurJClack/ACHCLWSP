// The window's frame, laid out like the tool each app replaces: a menu bar with
// every command, a ribbon of the common ones, file tabs over the canvas, a tool
// rail on the left, Properties / Layers / History tabs on the right and a status
// bar. Every button still runs a command through Session::run.
#include <algorithm>
#include <cctype>
#include <cmath>
#include <filesystem>

#include "shell.hpp"
// after imgui.h
#include "imgui_stdlib.h"

namespace wetwaresuite::gui {

namespace {

std::string fileName(const std::string& path) {
    auto slash = path.find_last_of("/\\");
    return slash == std::string::npos ? path : path.substr(slash + 1);
}

ImGuiKey letterKey(const char* key) {
    if (!key || !key[0] || key[1]) return ImGuiKey_None;
    const char c = key[0];
    if (c >= 'A' && c <= 'Z') return static_cast<ImGuiKey>(ImGuiKey_A + (c - 'A'));
    return ImGuiKey_None;
}

// Plain letters work as shortcuts only while nothing is being typed or dragged.
bool lettersFree() {
    const ImGuiIO& io = ImGui::GetIO();
    return !io.WantTextInput && !io.KeyCtrl && !io.KeyAlt && !io.KeySuper && !ImGui::IsAnyItemActive() &&
           !ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel);
}

struct Shortcut {
    const char* keys;
    const char* what;
};

const Shortcut kShortcuts[] = {
    {"Ctrl+N", "Start something new"},
    {"Ctrl+O", "Open a file"},
    {"Ctrl+S", "Save"},
    {"Ctrl+Shift+S", "Save as"},
    {"Ctrl+W", "Close the file"},
    {"Ctrl+Tab", "Next file (Ctrl+Shift+Tab: previous)"},
    {"Ctrl+Z / Ctrl+Y", "Undo / redo"},
    {"Ctrl+K", "Find any action by typing"},
    {"Ctrl+Enter", "Run the action on the right"},
    {"Ctrl+0", "Fit the picture to the window"},
    {"Ctrl+Space", "Hide or show the panels"},
    {"F1", "This list"},
    {"Ctrl+Q", "Quit"},
    {"Letters", "Pick a tool from the rail: the letter is next to its name (V move, M select, B brush...)"},
    {"Space", "Play or pause sound and video"},
};

}  // namespace

// ---- menu bar ----------------------------------------------------------------------

void Shell::menuBar() {
    if (!ImGui::BeginMenuBar()) return;
    ImGui::PushStyleColor(ImGuiCol_Text, title_);
    ImGui::TextUnformatted(app_.name.c_str());
    ImGui::PopStyleColor();
    ImGui::Dummy({ImGui::GetFontSize() * 0.5f, 0});
    fileMenu();
    editMenu();
    viewMenu();
    for (const auto& tool : tools_) toolMenu(tool);
    helpMenu();
    searchBox();
    ImGui::EndMenuBar();
}

void Shell::fileMenu() {
    if (!ImGui::BeginMenu("File")) return;
    const auto ids = docIds();
    auto doc = session_.workspace().get(currentDoc_);
    const bool onDoc = doc && !showHome_;
    if (ImGui::MenuItem("New...", "Ctrl+N")) openNew_ = true;
    if (ImGui::MenuItem("Open...", "Ctrl+O")) showOpenDialog();
    if (ImGui::BeginMenu("Open recent", !recent_.empty())) {
        for (std::size_t i = 0; i < recent_.size(); ++i) {
            ImGui::PushID(static_cast<int>(i));
            if (ImGui::MenuItem(fileName(recent_[i]).c_str())) openFile(recent_[i]);
            ImGui::SetItemTooltip("%s", recent_[i].c_str());
            ImGui::PopID();
        }
        ImGui::EndMenu();
    }
    ImGui::Separator();
    const std::string save = onDoc ? siblingCommand(docOpener(currentDoc_), "save", commands_) : std::string();
    if (ImGui::MenuItem("Save", "Ctrl+S", false, !save.empty())) saveDoc(false);
    if (ImGui::MenuItem("Save as...", "Ctrl+Shift+S", false, !save.empty())) saveDoc(true);
    // exports: the open file's tool first, then the rest of the app's
    std::vector<const CommandInfo*> exports;
    const std::string owner = onDoc && docTool_.count(currentDoc_) ? docTool_[currentDoc_] : std::string();
    for (int pass = 0; pass < 2; ++pass) {
        for (const auto& c : commands_) {
            const std::string tool = c.id.substr(0, c.id.find('.'));
            if ((pass == 0) != (tool == owner)) continue;
            const std::string verb = c.id.substr(c.id.find('.') + 1);
            if (verb.find("export") != std::string::npos) exports.push_back(&c);
        }
    }
    if (ImGui::BeginMenu("Export", !exports.empty())) {
        std::string lastTool;
        for (const CommandInfo* c : exports) {
            const std::string tool = c->id.substr(0, c->id.find('.'));
            if (!lastTool.empty() && tool != lastTool) ImGui::Separator();
            lastTool = tool;
            if (ImGui::MenuItem(c->title.c_str())) select(c->id);
            ImGui::SetItemTooltip("%s", firstSentence(readableDescription(c->description, commands_)).c_str());
        }
        ImGui::EndMenu();
    }
    if (handoff_ && session_.registry().find("app.targets")) {
        if (ImGui::MenuItem("Send to another app...", nullptr, false, onDoc)) openSend_ = true;
    }
    ImGui::Separator();
    if (ImGui::MenuItem("Close", "Ctrl+W", false, onDoc)) closeDoc(currentDoc_);
    if (ImGui::MenuItem("Quit", "Ctrl+Q") && requestQuit()) quit_ = true;
    ImGui::EndMenu();
}

void Shell::editMenu() {
    if (!ImGui::BeginMenu("Edit")) return;
    const auto labels = session_.history().labels();
    const std::string undoLabel = labels.empty() ? std::string("Undo") : "Undo " + labels.back();
    if (ImGui::MenuItem(undoLabel.c_str(), "Ctrl+Z", false, session_.history().canUndo())) undo();
    if (ImGui::MenuItem("Redo", "Ctrl+Y", false, session_.history().canRedo())) redo();
    if (ImGui::MenuItem("History")) {
        showProps_ = true;
        pickHistory_ = true;
    }
    ImGui::Separator();
    if (ImGui::MenuItem("Find an action...", "Ctrl+K")) focusSearch_ = true;
    ImGui::Separator();
    if (ImGui::MenuItem("Text size and colours...")) openSettings_ = true;
    ImGui::EndMenu();
}

void Shell::viewMenu() {
    if (!ImGui::BeginMenu("View")) return;
    if (ImGui::MenuItem("Start screen", nullptr, showHome_)) showHome_ = true;
    ImGui::Separator();
    if (layout_.ribbon) ImGui::MenuItem("Ribbon", nullptr, &showRibbon_);
    ImGui::MenuItem("Tool rail", nullptr, &showRail_);
    ImGui::MenuItem("Properties", nullptr, &showProps_);
    ImGui::MenuItem("Status bar", nullptr, &showStatus_);
    ImGui::MenuItem("Hide all panels", "Ctrl+Space", &focusMode_);
    ImGui::Separator();
    if (ImGui::MenuItem("Fit to window", "Ctrl+0")) canvas_.fit = true;
    ImGui::Separator();
    ImGui::MenuItem("Show advanced options", nullptr, &pro_);
    ImGui::SetItemTooltip("Extra settings and the rarer actions' details. Everything is free either way.");
    ImGui::EndMenu();
}

void Shell::toolMenu(const std::string& tool) {
    if (!ImGui::BeginMenu(toolTitle(tool).c_str())) return;
    ImGui::PushID(tool.c_str());
    if (hasPanel(tool)) {
        if (ImGui::MenuItem(("Open " + toolTitle(tool)).c_str(), nullptr, !showHome_ && panel_ == tool)) openPanel(tool);
        ImGui::Separator();
    }
    const bool noFiles = docIds().empty();
    auto item = [this, noFiles](const CommandInfo* c) {
        const bool off = noFiles && needsDocument(*c);  // greyed out until a file is open
        if (ImGui::MenuItem(c->title.c_str(), nullptr, form_ && form_->command == c->id, !off)) pick(c->id);
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip | ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("%s", firstSentence(readableDescription(c->description, commands_)).c_str());
    };
    // the ribbon's groups first (Image, Adjust, Select, Filters...): titled sections when they
    // fit on screen, else submenus like Photoshop's Image > Adjustments
    std::vector<const RibbonTab*> tabs;
    std::size_t count = 0;
    for (const auto& tab : ribbonTabs_) {
        if (tab.tool != tool) continue;
        tabs.push_back(&tab);
        for (const auto& g : tab.groups) count += g.commands.size();
    }
    const bool nest = count > 16;
    std::vector<const CommandInfo*> shown;
    auto groupItems = [&](const RibbonGroup& g) {
        for (const auto* c : g.commands) {
            if (std::find(shown.begin(), shown.end(), c) != shown.end()) continue;
            item(c);
            shown.push_back(c);
        }
    };
    for (const RibbonTab* tab : tabs) {
        const bool tabMenu = nest && tabs.size() > 1;  // office: Sheet >, Document >, Slides >
        if (tabMenu && !ImGui::BeginMenu(tab->title.c_str())) {
            for (const auto& g : tab->groups) shown.insert(shown.end(), g.commands.begin(), g.commands.end());
            continue;
        }
        for (const auto& g : tab->groups) {
            if (nest && !tabMenu) {
                if (ImGui::BeginMenu(g.title.c_str())) {
                    groupItems(g);
                    ImGui::EndMenu();
                } else {
                    shown.insert(shown.end(), g.commands.begin(), g.commands.end());
                }
            } else {
                ImGui::SeparatorText(g.title.c_str());
                groupItems(g);
            }
        }
        if (tabMenu) ImGui::EndMenu();
    }
    // then everything else, alphabetical, with families (layer_*, page_*...) in submenus
    auto groups = menuGroups(tool, commands_);
    for (auto& g : groups) {
        g.commands.erase(std::remove_if(g.commands.begin(), g.commands.end(),
                                        [&](const CommandInfo* c) { return std::find(shown.begin(), shown.end(), c) != shown.end(); }),
                         g.commands.end());
        std::sort(g.commands.begin(), g.commands.end(), [](const CommandInfo* x, const CommandInfo* y) { return x->title < y->title; });
    }
    std::size_t rest = 0;
    for (const auto& g : groups) rest += g.commands.size();
    if (rest == 0) {
        ImGui::PopID();
        ImGui::EndMenu();
        return;
    }
    auto everything = [&] {
        for (const auto& g : groups) {
            if (g.commands.empty()) continue;
            if (g.title.empty()) {
                for (const auto* c : g.commands) item(c);
            } else if (ImGui::BeginMenu(g.title.c_str())) {
                for (const auto* c : g.commands) item(c);
                ImGui::EndMenu();
            }
        }
    };
    if (shown.empty()) {
        everything();
    } else {
        ImGui::Separator();
        if (ImGui::BeginMenu(("More " + toolTitle(tool) + " actions").c_str())) {
            everything();
            ImGui::EndMenu();
        }
    }
    ImGui::PopID();
    ImGui::EndMenu();
}

void Shell::helpMenu() {
    if (!ImGui::BeginMenu("Help")) return;
    if (ImGui::MenuItem("Keyboard shortcuts", "F1")) openShortcuts_ = true;
    if (ImGui::MenuItem("Tutorial (opens in your browser)")) {
        const std::string page = app_.id.empty() || app_.id == "suite" ? "README.md" : "docs/tutorials/" + app_.id + ".md";
        SDL_OpenURL(("https://github.com/ArthurJClack/wetwaresuite/blob/main/" + page).c_str());
    }
    ImGui::Separator();
    ImGui::TextDisabled("wetwaresuite: free, open source (GPL-3.0)");
    ImGui::EndMenu();
}

void Shell::searchBox() {
    const float unit = ImGui::GetFontSize();
    const float boxW = std::clamp(ImGui::GetContentRegionAvail().x - unit * 9, unit * 8, unit * 18);
    const float undoW = ImGui::CalcTextSize("Undo").x + ImGui::CalcTextSize("Redo").x + ImGui::GetStyle().FramePadding.x * 4 +
                        ImGui::GetStyle().ItemSpacing.x * 2;
    const float right = ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x;
    ImGui::SetCursorPosX(std::max(ImGui::GetCursorPosX(), right - boxW - undoW - ImGui::GetStyle().ItemSpacing.x));
    ImGui::SetNextItemWidth(boxW);
    if (focusSearch_) {
        ImGui::SetKeyboardFocusHere();
        focusSearch_ = false;
    }
    const bool enter = ImGui::InputTextWithHint("##search", "Find an action  (Ctrl+K)", &query_, ImGuiInputTextFlags_EnterReturnsTrue);
    const bool active = ImGui::IsItemActive();
    searchMin_ = ImGui::GetItemRectMin();
    searchMax_ = ImGui::GetItemRectMax();
    if (active) {
        if (ImGui::IsKeyPressed(ImGuiKey_DownArrow)) ++searchPick_;
        if (ImGui::IsKeyPressed(ImGuiKey_UpArrow)) searchPick_ = std::max(0, searchPick_ - 1);
        if (ImGui::IsKeyPressed(ImGuiKey_Escape)) query_.clear();
    }
    if (enter && !query_.empty()) {
        auto hits = search(query_, commands_, 12);
        if (!hits.empty()) {
            select(hits[static_cast<std::size_t>(std::clamp(searchPick_, 0, static_cast<int>(hits.size()) - 1))].command->id);
            query_.clear();
        }
    }
    searchActive_ = active;
    ImGui::BeginDisabled(!session_.history().canUndo());
    if (ImGui::SmallButton("Undo")) undo();
    ImGui::EndDisabled();
    ImGui::SetItemTooltip("Take back the last change (Ctrl+Z)");
    ImGui::BeginDisabled(!session_.history().canRedo());
    if (ImGui::SmallButton("Redo")) redo();
    ImGui::EndDisabled();
    ImGui::SetItemTooltip("Put back what you took back (Ctrl+Y)");
}

// ---- ribbon ------------------------------------------------------------------------

void Shell::ribbon() {
    if (ribbonTabs_.empty()) return;
    // the tab that fits the open file, the way Office shows sheet buttons for a spreadsheet
    const std::string want = showHome_ ? std::string() : currentDoc_;
    if (want != ribbonFor_) {
        ribbonFor_ = want;
        if (!want.empty()) {
            const int t = ribbonTabFor(ribbonTabs_, docOpener(want));
            if (t >= 0) ribbonPick_ = t;
        }
    }
    if (!expandTool_.empty()) {  // the start screen's "More..." for a tool
        for (std::size_t i = 0; i < ribbonTabs_.size(); ++i) {
            if (ribbonTabs_[i].tool == expandTool_) {
                ribbonPick_ = static_cast<int>(i);
                break;
            }
        }
        expandTool_.clear();
    }
    // circuits: the Board tab while the board is shown
    if (circuitDoc() && !showHome_ && circuit_.doc == currentDoc_) {
        for (std::size_t i = 0; i < ribbonTabs_.size(); ++i) {
            const bool board = ribbonTabs_[i].title == "Board";
            if (ribbonTabs_[i].tool == "circuit" && board == (circuit_.view == "pcb") && ribbonFor_ != "circuit:" + circuit_.view) {
                ribbonPick_ = static_cast<int>(i);
            }
        }
        ribbonFor_ = currentDoc_;
    }

    const bool compact = layout_.compactRibbon;
    const ImGuiStyle& style = ImGui::GetStyle();
    const float unit = ImGui::GetFontSize();
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, {style.FramePadding.x, style.FramePadding.y * 0.6f});
    if (ImGui::BeginTabBar("ribbon", ImGuiTabBarFlags_FittingPolicyScroll)) {
        for (std::size_t i = 0; i < ribbonTabs_.size(); ++i) {
            const RibbonTab& tab = ribbonTabs_[i];
            const ImGuiTabItemFlags flags = ribbonPick_ == static_cast<int>(i) ? ImGuiTabItemFlags_SetSelected : 0;
            ImGui::PushID(static_cast<int>(i));
            if (ImGui::BeginTabItem(tab.title.c_str(), nullptr, flags)) {
                const float rowH = ImGui::GetFrameHeight();
                // room for a scrollbar under the buttons when a narrow window can't fit them all
                const float h = (compact ? rowH : rowH + ImGui::GetTextLineHeight() + style.ItemSpacing.y) + style.ScrollbarSize * 0.5f + style.ItemSpacing.y * 0.5f;
                ImGui::PushStyleVar(ImGuiStyleVar_ScrollbarSize, style.ScrollbarSize * 0.5f);
                ImGui::BeginChild("groups", {0, h}, ImGuiChildFlags_None, ImGuiWindowFlags_HorizontalScrollbar);
                const bool noFiles = docIds().empty();
                for (std::size_t g = 0; g < tab.groups.size(); ++g) {
                    const RibbonGroup& group = tab.groups[g];
                    if (g > 0) {
                        ImGui::SameLine(0, unit * 0.6f);
                        const ImVec2 p = ImGui::GetCursorScreenPos();
                        ImGui::GetWindowDrawList()->AddLine({p.x, p.y + 2}, {p.x, p.y + (compact ? rowH : h - style.ItemSpacing.y) - 2},
                                                            ImGui::GetColorU32(ImGuiCol_Separator));
                        ImGui::Dummy({1, rowH});
                        ImGui::SameLine(0, unit * 0.6f);
                    }
                    ImGui::BeginGroup();
                    const float groupX = ImGui::GetCursorScreenPos().x;
                    ImGui::PushID(static_cast<int>(g));
                    for (std::size_t b = 0; b < group.commands.size(); ++b) {
                        const CommandInfo* c = group.commands[b];
                        if (b > 0) ImGui::SameLine(0, style.ItemSpacing.x * 0.5f);
                        const bool on = form_ && form_->command == c->id;
                        if (on) ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
                        ImGui::PushID(c->id.c_str());
                        const std::string& label = b < group.labels.size() ? group.labels[b] : c->title;
                        // greyed out until there's a file to work on, like Office with no document
                        const bool off = noFiles && needsDocument(*c);
                        ImGui::BeginDisabled(off);
                        if (ImGui::Button(label.c_str())) pick(c->id);
                        ImGui::EndDisabled();
                        ImGui::PopID();
                        if (on) ImGui::PopStyleColor();
                        if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip | ImGuiHoveredFlags_AllowWhenDisabled) && ImGui::BeginTooltip()) {
                            ImGui::PushTextWrapPos(unit * 26);
                            ImGui::Text("%s  (%s)", c->title.c_str(), group.title.c_str());
                            ImGui::TextDisabled("%s", firstSentence(readableDescription(c->description, commands_)).c_str());
                            if (off) ImGui::TextUnformatted("Open a file or start a new one first (Ctrl+O, Ctrl+N).");
                            ImGui::PopTextWrapPos();
                            ImGui::EndTooltip();
                        }
                    }
                    if (!compact) {
                        // the group's name centred under its buttons, like Office
                        const float w = ImGui::GetItemRectMax().x - groupX;
                        const float tw = ImGui::CalcTextSize(group.title.c_str()).x;
                        ImGui::SetCursorScreenPos({groupX + std::max(0.0f, (w - tw) / 2), ImGui::GetCursorScreenPos().y});
                        ImGui::TextDisabled("%s", group.title.c_str());
                    }
                    ImGui::PopID();
                    ImGui::EndGroup();
                }
                ImGui::EndChild();
                ImGui::PopStyleVar();
                ImGui::EndTabItem();
            }
            ImGui::PopID();
        }
        ImGui::EndTabBar();
    }
    ImGui::PopStyleVar();
    ribbonPick_ = -1;
}

// ---- file tabs ---------------------------------------------------------------------

void Shell::docTabs() {
    // what should be showing: the start screen, a tool panel, or a file
    const bool panelShown = !showHome_ && !panel_.empty();
    const std::string want = showHome_ ? std::string() : panelShown ? "panel:" + panel_ : currentDoc_;
    if (want != tabShown_) {
        tabShown_ = want;
        tabForce_ = 2;  // the tab bar shows our pick from the next frame on
    }
    std::vector<std::pair<std::string, std::string>> tabs;  // key, label
    tabs.emplace_back("", "Start");
    for (const auto& [tool, panel] : std::vector<std::pair<std::string, std::string>>{
             {"calc", "Calculator"}, {"code", "Code"}, {"explorer", "Files"}, {"terminal", "Terminal"}, {"monitor", "Monitor"}, {"clock", "Clock"}, {"pipeline", "Pipeline builder"}}) {
        if (hasPanel(tool)) tabs.emplace_back("panel:" + tool, panel);
    }
    for (const auto& id : docIds()) {
        if (!panelForDoc(id).empty()) continue;  // code files and terminals live in their panel
        auto doc = session_.workspace().get(id);
        // a dot marks changes that aren't saved yet, like most editors
        tabs.emplace_back(id, docTitle(id) + (unsaved_.count(id) ? " \xe2\x80\xa2" : ""));
    }
    std::string clicked, closing;
    if (ImGui::BeginTabBar("files", ImGuiTabBarFlags_FittingPolicyScroll | ImGuiTabBarFlags_NoTooltip)) {
        for (const auto& [key, label] : tabs) {
            const bool isDoc = !key.empty() && key.rfind("panel:", 0) != 0;
            bool open = true;
            ImGuiTabItemFlags flags = key == want && tabForce_ > 0 ? ImGuiTabItemFlags_SetSelected : 0;
            if (key.empty()) flags |= ImGuiTabItemFlags_Leading | ImGuiTabItemFlags_NoReorder;
            const std::string shown = label + "###" + (key.empty() ? std::string("start") : key);
            if (ImGui::BeginTabItem(shown.c_str(), isDoc ? &open : nullptr, flags)) {
                if (key != want && tabForce_ == 0) clicked = key.empty() ? "@start" : key;
                ImGui::EndTabItem();
            }
            if (isDoc) {
                if (auto doc = session_.workspace().get(key)) ImGui::SetItemTooltip("%s\n%s", key.c_str(), doc->summary().c_str());
            }
            if (!open) closing = key;
        }
        ImGui::EndTabBar();
    }
    if (tabForce_ > 0) --tabForce_;
    if (!closing.empty()) {
        closeDoc(closing);
        return;
    }
    if (clicked == "@start") {
        showHome_ = true;
    } else if (clicked.rfind("panel:", 0) == 0) {
        openPanel(clicked.substr(6));
    } else if (!clicked.empty()) {
        showDoc(clicked);
    }
    if (!clicked.empty()) tabShown_ = clicked == "@start" ? std::string() : clicked;
}

void Shell::showDoc(const std::string& id) {
    if (currentDoc_ != id) canvas_.fit = true;
    currentDoc_ = id;
    showHome_ = false;
    panel_ = panelForDoc(id);
    canvas_.dirty = true;
}

void Shell::nextTab(int step) {
    std::vector<std::string> order{""};
    for (const auto& id : docIds()) {
        if (panelForDoc(id).empty()) order.push_back(id);
    }
    if (order.size() < 2) return;
    const std::string now = showHome_ || !panel_.empty() ? std::string() : currentDoc_;
    auto it = std::find(order.begin(), order.end(), now);
    std::ptrdiff_t i = it == order.end() ? 0 : it - order.begin();
    const auto n = static_cast<std::ptrdiff_t>(order.size());
    i = ((i + step) % n + n) % n;
    if (order[static_cast<std::size_t>(i)].empty()) showHome_ = true;
    else showDoc(order[static_cast<std::size_t>(i)]);
}

std::string Shell::docOpener(const std::string& id) const {
    auto it = docOpener_.find(id);
    if (it != docOpener_.end()) return it->second;
    auto t = docTool_.find(id);
    return t != docTool_.end() ? t->second + ".open" : std::string();
}

// A file's name, or "Untitled map 1" for one never saved (its id, map1, stays what commands use).
std::string Shell::docTitle(const std::string& id) const {
    auto doc = session_.workspace().get(id);
    if (!doc) return id;
    if (!doc->path.empty()) return fileName(doc->path);
    static const std::map<std::string, std::string> words = {{"vector", "drawing"}, {"cad", "part"},       {"deck", "slides"},
                                                             {"doc", "document"},   {"sheet", "spreadsheet"}, {"audio", "sound"}};
    const std::string kind = doc->kind();
    const auto w = words.find(kind);
    std::size_t digits = id.size();
    while (digits > 0 && std::isdigit(static_cast<unsigned char>(id[digits - 1]))) --digits;
    return "Untitled " + (w != words.end() ? w->second : kind) + (digits < id.size() ? " " + id.substr(digits) : "");
}

// ---- tool rail ---------------------------------------------------------------------

bool Shell::railItem(const char* label, const char* key, const char* tip, bool on) {
    ++railCount_;
    const ImGuiKey k = letterKey(key);
    bool picked = k != ImGuiKey_None && lettersFree() && ImGui::IsKeyPressed(k, false);
    if (!railDraw_) return picked;
    ImGui::PushID(label);
    const float w = ImGui::GetContentRegionAvail().x;
    if (on) {
        ImGui::PushStyleColor(ImGuiCol_Button, accent_);
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, accentHover_);
        ImGui::PushStyleColor(ImGuiCol_Text, accentText_);
    }
    ImGui::PushStyleVar(ImGuiStyleVar_ButtonTextAlign, {0.0f, 0.5f});
    picked = ImGui::Button(label, {w, 0}) || picked;
    ImGui::PopStyleVar();
    if (key && key[0]) {
        // the shortcut letter at the right edge of the button
        const ImVec2 mx = ImGui::GetItemRectMax(), mn = ImGui::GetItemRectMin();
        const ImVec2 ks = ImGui::CalcTextSize(key);
        ImGui::GetWindowDrawList()->AddText({mx.x - ks.x - ImGui::GetStyle().FramePadding.x * 0.7f, mn.y + (mx.y - mn.y - ks.y) / 2},
                                            ImGui::GetColorU32(on ? ImGuiCol_Text : ImGuiCol_TextDisabled), key);
    }
    if (on) ImGui::PopStyleColor(3);
    if (key && key[0]) ImGui::SetItemTooltip("%s  (%s)\n%s", label, key, tip);
    else ImGui::SetItemTooltip("%s\n%s", label, tip);
    ImGui::PopID();
    return picked;
}

void Shell::toolRail() {
    railCount_ = 0;
    railDraw_ = true;
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, {ImGui::GetStyle().ItemSpacing.x, ImGui::GetStyle().ItemSpacing.y * 0.4f});
    railTools();
    ImGui::PopStyleVar();
    railDraw_ = false;
}

// ---- properties --------------------------------------------------------------------

void Shell::propertiesPanel() {
    const bool layers = hasLayers(), effects = hasEffects();
    if (!ImGui::BeginTabBar("props")) return;
    if (ImGui::BeginTabItem("Properties", nullptr, pickProps_ ? ImGuiTabItemFlags_SetSelected : 0)) {
        // the form scrolls; its button stays put under it, like a dialog's OK
        const float buttonH = ImGui::GetFontSize() * 2.2f;
        const std::string shownForm = form_ ? form_->command : std::string();
        if (shownForm != propsForm_) {  // a newly picked action starts at its top
            propsForm_ = shownForm;
            ImGui::SetNextWindowScroll({0, 0});
        }
        ImGui::BeginChild("p", {0, form_ ? -(buttonH + ImGui::GetStyle().ItemSpacing.y) : 0.0f});
        rightPanel();
        ImGui::EndChild();
        if (form_) {
            if (accentButton(runLabel(form_->command).c_str(), {-1, buttonH})) runForm();
            ImGui::SetItemTooltip("Do it (Ctrl+Enter)");
        }
        ImGui::EndTabItem();
    }
    if (layers && ImGui::BeginTabItem("Layers")) {
        ImGui::BeginChild("l");
        layersPanel();
        ImGui::EndChild();
        ImGui::EndTabItem();
    }
    if (effects && ImGui::BeginTabItem("Effects")) {
        ImGui::BeginChild("e");
        soundPanel();
        ImGui::EndChild();
        ImGui::EndTabItem();
    }
    if (ImGui::BeginTabItem("History", nullptr, pickHistory_ ? ImGuiTabItemFlags_SetSelected : 0)) {
        ImGui::BeginChild("h");
        historyPanel();
        ImGui::EndChild();
        ImGui::EndTabItem();
    }
    ImGui::EndTabBar();
    pickProps_ = pickHistory_ = false;
}

// ---- status bar ----------------------------------------------------------------------

void Shell::statusBar() {
    const float unit = ImGui::GetFontSize();
    ImGui::AlignTextToFramePadding();
    if (result_) {
        const bool ok = result_->ok;
        const ImVec4 colour = ok ? (darkLook() ? ImVec4{0.45f, 0.95f, 0.55f, 1} : ImVec4{0.0f, 0.45f, 0.1f, 1})
                                 : (darkLook() ? ImVec4{1.0f, 0.55f, 0.5f, 1} : ImVec4{0.7f, 0.0f, 0.0f, 1});
        std::string line = result_->message;
        if (const auto nl = line.find('\n'); nl != std::string::npos) line = line.substr(0, nl) + " ...";
        if (!resultFor_.empty()) line = resultFor_ + ": " + line;
        ImGui::PushStyleColor(ImGuiCol_Text, colour);
        ImGui::TextUnformatted(line.c_str());
        ImGui::PopStyleColor();
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", result_->message.c_str());
        if (ImGui::IsItemClicked()) {
            showProps_ = true;
            pickProps_ = true;
        }
    } else {
        ImGui::TextDisabled("Ready. Ctrl+K finds any action; F1 lists the shortcuts.");
    }
    auto doc = session_.workspace().get(currentDoc_);
    std::string right;
    if (doc && !showHome_ && panel_.empty()) {
        right = doc->summary();
        if (!canvas_.is3d && canvas_.texture.tex) right += "   " + std::to_string(static_cast<int>(std::lround(canvas_.zoom * 100))) + "%";
    }
    if (!right.empty()) {
        ImGui::SameLine();
        // cut the file's summary short rather than run over the message on the left
        const float room = ImGui::GetContentRegionAvail().x - unit;
        const std::string full = right;
        if (ImGui::CalcTextSize(right.c_str()).x > room) {
            while (!right.empty() && ImGui::CalcTextSize((right + "...").c_str()).x > room) right.pop_back();
            if (!right.empty()) right += "...";
        }
        const float w = ImGui::CalcTextSize(right.c_str()).x;
        ImGui::SetCursorPosX(std::max(ImGui::GetCursorPosX() + unit, ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x - w));
        ImGui::TextDisabled("%s", right.c_str());
        if (right != full) ImGui::SetItemTooltip("%s", full.c_str());
    }
}

// ---- actions -----------------------------------------------------------------------

void Shell::pick(const std::string& commandId) {
    if (hasPanel(commandId.substr(0, commandId.find('.')))) {
        // the system tools' panels have their own buttons for their commands
        showAction(commandId);
        return;
    }
    const CommandInfo* info = find(commandId);
    if (!info) return;
    // nothing to fill in but the open file: do it now, like a ribbon button in Office
    Form f = buildForm(*info, docIds(), currentDoc_);
    bool asks = false;
    for (const auto& field : f.fields) {
        if (field.hidden) continue;
        if (field.kind == FieldKind::Document && !field.text.empty()) continue;
        asks = true;
    }
    if (asks) {
        select(commandId);
        return;
    }
    form_ = std::move(f);
    runForm();
}

bool Shell::runOnDoc(const std::string& commandId, const std::string& doc) {
    const CommandInfo* info = find(commandId);
    if (!info) return false;
    Form f = buildForm(*info, docIds(), doc);
    Params params;
    std::string error;
    if (!toParams(f, params, error)) {
        form_ = std::move(f);
        result_.reset();
        pickProps_ = showProps_ = true;
        return false;
    }
    resultFor_ = info->title;
    Result r = run(commandId, params);
    r.message = r.ok ? sentence(r.message) : friendlyError(r.message);
    const bool ok = r.ok;
    result_ = std::move(r);
    return ok;
}

void Shell::saveDoc(bool askWhere) {
    auto doc = session_.workspace().get(currentDoc_);
    if (!doc || showHome_) return;
    const std::string id = siblingCommand(docOpener(currentDoc_), "save", commands_);
    const CommandInfo* info = id.empty() ? nullptr : find(id);
    if (!info) {
        result_ = Result::failure("This file can't be saved here. Try File > Export.");
        resultFor_ = "Save";
        return;
    }
    Form f = buildForm(*info, docIds(), currentDoc_);
    bool hasPath = false;
    for (auto& field : f.fields) {
        if (field.spec.name != "path") continue;
        hasPath = true;
        if (!doc->path.empty()) setFieldText(field, doc->path);
    }
    if (hasPath && (askWhere || doc->path.empty())) {
        // where to: the system's save window, like any other app
        askSavePath(id, currentDoc_, doc->path);
        return;
    }
    saveDocTo(id, currentDoc_, doc->path);
}

void Shell::saveDocTo(const std::string& command, const std::string& docId, const std::string& path) {
    const CommandInfo* info = find(command);
    if (!info || !session_.workspace().get(docId)) return;
    Form f = buildForm(*info, docIds(), docId);
    for (auto& field : f.fields) {
        if (field.spec.name == "path" && !path.empty()) setFieldText(field, path);
    }
    Params params;
    std::string error;
    if (!toParams(f, params, error)) {  // something else to fill in first
        form_ = std::move(f);
        result_.reset();
        pickProps_ = showProps_ = true;
        return;
    }
    resultFor_ = "Save";
    Result r = run(command, params);
    r.message = r.ok ? sentence(r.message) : friendlyError(r.message);
    result_ = std::move(r);
}

void Shell::closeDoc(const std::string& id) {
    if (unsaved_.count(id) && session_.workspace().get(id)) {
        askClose_ = id;
        askQuit_ = false;
        openAskSave_ = true;
        return;
    }
    closeDocNow(id);
}

bool Shell::requestQuit() {
    for (auto it = unsaved_.begin(); it != unsaved_.end();) it = session_.workspace().get(*it) ? std::next(it) : unsaved_.erase(it);
    if (unsaved_.empty()) return true;
    if (!askQuit_) {  // SDL sends close-requested and quit together: ask once
        askQuit_ = true;
        askClose_.clear();
        openAskSave_ = true;
    }
    return false;
}

void Shell::askSavePopup() {
    if (!ImGui::BeginPopupModal("Save changes?", nullptr, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings)) return;
    const float unit = ImGui::GetFontSize();
    auto name = [this](const std::string& id) { return docTitle(id); };
    if (askQuit_) {
        std::string list;
        for (const auto& id : unsaved_) list += (list.empty() ? "" : ", ") + name(id);
        ImGui::Text("These files have changes that aren't saved: %s", list.c_str());
        ImGui::Spacing();
        if (accentButton("Keep working", {unit * 9, 0}) || ImGui::IsKeyPressed(ImGuiKey_Escape)) {
            askQuit_ = false;
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Quit without saving", {unit * 10, 0})) quit_ = true;
    } else {
        ImGui::Text("Save the changes to %s before closing it?", name(askClose_).c_str());
        ImGui::Spacing();
        if (accentButton("Save", {unit * 6, 0})) {
            closeAfterSave_ = askClose_;
            showDoc(askClose_);
            saveDoc(false);
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Don't save", {unit * 6, 0})) {
            unsaved_.erase(askClose_);
            closeDocNow(askClose_);
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel", {unit * 6, 0}) || ImGui::IsKeyPressed(ImGuiKey_Escape)) ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
}

// A password-protected file: type its password, Open tries again.
void Shell::passwordPopup() {
    if (!ImGui::BeginPopupModal("Password", nullptr, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings)) return;
    PasswordAsk& a = passwordAsk_;
    const float unit = ImGui::GetFontSize();
    ImGui::Text("%s is locked with a password.", fileName(a.path).c_str());
    if (a.focus) {
        ImGui::SetKeyboardFocusHere();
        a.focus = false;
    }
    ImGui::SetNextItemWidth(unit * 18);
    bool go = ImGui::InputTextWithHint("##pw", "password", &a.text, ImGuiInputTextFlags_Password | ImGuiInputTextFlags_EnterReturnsTrue);
    if (!a.error.empty()) ImGui::TextColored({0.95f, 0.45f, 0.4f, 1}, "%s", a.error.c_str());
    ImGui::Spacing();
    go = accentButton("Open", {unit * 6, 0}) || go;
    ImGui::SameLine();
    const bool cancel = ImGui::Button("Cancel", {unit * 6, 0}) || ImGui::IsKeyPressed(ImGuiKey_Escape);
    if (go) {
        Result r = run(a.command, {{"path", a.path}, {"password", a.text}});
        if (r.ok) {
            r.message = sentence(r.message);
            result_ = std::move(r);
            resultFor_ = "Open a file";
            a = {};
            ImGui::CloseCurrentPopup();
        } else {
            a.error = "That password doesn't open it.";
            a.text.clear();
            a.focus = true;
        }
    } else if (cancel) {
        a = {};
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
}

void Shell::closeDocNow(const std::string& id) {
    const std::string close = siblingCommand(docOpener(id), "close", commands_);
    if (close.empty() || !runOnDoc(close, id)) return;
    docOpener_.erase(id);
    unsaved_.erase(id);
    if (form_) {  // a form still aimed at the closed file goes too
        for (const auto& f : form_->fields) {
            if (f.kind == FieldKind::Document && f.text == id) {
                form_.reset();
                break;
            }
        }
    }
    syncDocs();
    if (docIds().empty()) showHome_ = true;
}

// ---- shortcuts and popups ----------------------------------------------------------

void Shell::shortcuts() {
    const ImGuiIO& io = ImGui::GetIO();
    if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_N)) openNew_ = true;
    if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_S)) saveDoc(false);
    if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_S)) saveDoc(true);
    if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_W) && !showHome_ && panel_.empty() && !currentDoc_.empty()) closeDoc(currentDoc_);
    if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_Tab)) nextTab(1);
    if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_Tab)) nextTab(-1);
    if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_Q) && requestQuit()) quit_ = true;
    if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_0)) canvas_.fit = true;
    if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_Space)) focusMode_ = !focusMode_;
    if (ImGui::IsKeyPressed(ImGuiKey_F1, false)) openShortcuts_ = true;
    // tools' letters work with the rail hidden too
    if (!io.WantTextInput && (focusMode_ || !showRail_) && !showHome_ && panel_.empty()) {
        railDraw_ = false;
        railCount_ = 0;
        railTools();
    }
}

void Shell::shortcutsPopup() {
    if (!ImGui::BeginPopupModal("Keyboard shortcuts", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) return;
    if (ImGui::BeginTable("keys", 2, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingFixedFit)) {
        for (const auto& s : kShortcuts) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(s.keys);
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(s.what);
        }
        ImGui::EndTable();
    }
    if (accentButton("Close", {-1, 0}) || ImGui::IsKeyPressed(ImGuiKey_Escape)) ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
}

void Shell::rootPopups() {
    if (openNew_) {
        // only one kind of new file: straight to its settings, not a one-item menu
        std::vector<std::string> kinds;
        for (const auto& tool : tools_) {
            for (const auto& c : commands_) {
                if (c.id.compare(0, tool.size() + 1, tool + ".") == 0 && commandVerb(c.id) == "new") kinds.push_back(c.id);
            }
        }
        if (kinds.size() == 1) {
            openNew_ = false;
            select(kinds.front());
        }
    }
    if (openNew_) {
        ImGui::OpenPopup("new");
        ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Always, {0.5f, 0.5f});
        openNew_ = false;
    }
    newPopup();
    if (openSend_) {
        sendChoices_ = sendChoices(session_.run("app.targets", {{"doc", currentDoc_}}), self_);
        ImGui::OpenPopup("sendto");
        ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Always, {0.5f, 0.5f});
        openSend_ = false;
    }
    sendPopup();
    if (openShortcuts_) {
        ImGui::OpenPopup("Keyboard shortcuts");
        openShortcuts_ = false;
    }
    shortcutsPopup();
    if (openAskSave_) {
        ImGui::OpenPopup("Save changes?");
        ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Always, {0.5f, 0.5f});
        openAskSave_ = false;
    }
    askSavePopup();
    if (passwordAsk_.open) {
        ImGui::OpenPopup("Password");
        ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Always, {0.5f, 0.5f});
        passwordAsk_.open = false;
        passwordAsk_.focus = true;
    }
    passwordPopup();
    if (openSettings_) {
        paletteStale_ = true;  // show what other apps may have saved meanwhile
        openSettings_ = false;
        ImGui::OpenPopup("settings");
        ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Always, {0.5f, 0.5f});
    }
    settingsPopup();
}

}  // namespace wetwaresuite::gui

