// The panels: the frame's layout (the parts themselves are in chrome.cpp), the
// start screen or the open document in the middle, the action's form and result.
#include <algorithm>
#include <cmath>
#include <filesystem>

#include "shell.hpp"
#include "wetwaresuite/core/cli.hpp"
// after imgui.h
#include "imgui_stdlib.h"

namespace wetwaresuite::gui {

namespace {

ImVec4 mix(ImVec4 a, ImVec4 b, float t) {
    return {a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.z + (b.z - a.z) * t, a.w + (b.w - a.w) * t};
}

float luminance(ImVec4 c) { return 0.2126f * c.x + 0.7152f * c.y + 0.0722f * c.z; }

void wrappedMuted(const std::string& text) {
    ImGui::PushTextWrapPos(0.0f);
    ImGui::TextDisabled("%s", text.c_str());
    ImGui::PopTextWrapPos();
}

void heading(const std::string& text, float scale = 1.35f) {
    ImGui::PushFont(nullptr, ImGui::GetStyle().FontSizeBase * scale);
    ImGui::TextUnformatted(text.c_str());
    ImGui::PopFont();
}

std::string fileName(const std::string& path) {
    auto slash = path.find_last_of("/\\");
    return slash == std::string::npos ? path : path.substr(slash + 1);
}

}  // namespace

namespace {

ImVec4 im(const Rgba& c) { return {c.r, c.g, c.b, c.a}; }

// Every colour from a picked palette (see palette.hpp for what each one drives).
void applyTheme(ImGuiStyle& s, const Theme& th) {
    if (th.dark) ImGui::StyleColorsDark(&s);
    else ImGui::StyleColorsLight(&s);
    ImVec4* c = s.Colors;
    c[ImGuiCol_Text] = im(th.text);
    c[ImGuiCol_TextDisabled] = im(th.textMuted);
    c[ImGuiCol_WindowBg] = c[ImGuiCol_ChildBg] = im(th.background);
    c[ImGuiCol_PopupBg] = im(th.popup);
    c[ImGuiCol_Border] = c[ImGuiCol_Separator] = im(th.border);
    c[ImGuiCol_SeparatorHovered] = c[ImGuiCol_SeparatorActive] = im(th.focus);
    c[ImGuiCol_FrameBg] = im(th.frame);
    c[ImGuiCol_FrameBgHovered] = im(th.frameHover);
    c[ImGuiCol_FrameBgActive] = im(th.frameActive);
    c[ImGuiCol_Button] = im(th.button);
    c[ImGuiCol_ButtonHovered] = im(th.buttonHover);
    c[ImGuiCol_ButtonActive] = im(th.buttonActive);
    c[ImGuiCol_Header] = im(th.header);
    c[ImGuiCol_HeaderHovered] = im(th.headerHover);
    c[ImGuiCol_HeaderActive] = im(th.headerActive);
    c[ImGuiCol_CheckMark] = c[ImGuiCol_SliderGrab] = im(th.highlight);
    c[ImGuiCol_SliderGrabActive] = im(th.accent);
    c[ImGuiCol_ScrollbarBg] = im(th.background);
    c[ImGuiCol_ScrollbarGrab] = im(th.button);
    c[ImGuiCol_ScrollbarGrabHovered] = im(th.buttonHover);
    c[ImGuiCol_ScrollbarGrabActive] = im(th.buttonActive);
    c[ImGuiCol_TextLink] = im(th.link);
    c[ImGuiCol_TextSelectedBg] = im(th.selection);
    c[ImGuiCol_NavCursor] = im(th.focus);
    c[ImGuiCol_ResizeGrip] = c[ImGuiCol_ResizeGripHovered] = c[ImGuiCol_ResizeGripActive] = im(th.border);
    // tabs and floating editors' title bars follow the palette too, not ImGui's blue
    c[ImGuiCol_Tab] = c[ImGuiCol_TabDimmed] = im(th.button);
    c[ImGuiCol_TabHovered] = im(th.buttonHover);
    c[ImGuiCol_TabSelected] = c[ImGuiCol_TabDimmedSelected] = im(th.header);
    c[ImGuiCol_TabSelectedOverline] = c[ImGuiCol_TabDimmedSelectedOverline] = im(th.accent);
    c[ImGuiCol_TitleBg] = c[ImGuiCol_TitleBgCollapsed] = im(th.button);
    c[ImGuiCol_TitleBgActive] = im(th.header);
}

}  // namespace

void Shell::applyStyle(float dpiScale) {
    dpi_ = dpiScale > 0 ? dpiScale : 1.0f;
    ImGuiStyle s;
    if (custom_) {
        theme_ = deriveTheme(palette_);
        applyTheme(s, theme_);
        accent_ = im(theme_.accent);
        accentHover_ = im(theme_.accentHover);
        accentActive_ = im(theme_.accentActive);
        accentText_ = im(theme_.accentText);
        title_ = im(theme_.title);
    } else {
        // the app's own look: its accent on the standard dark or light surfaces
        if (dark_) ImGui::StyleColorsDark(&s);
        else ImGui::StyleColorsLight(&s);
        const Rgba a = fromRgb24(app_.accent);
        const ImVec4 accent{a.r, a.g, a.b, 1.0f};
        ImVec4* c = s.Colors;
        const ImVec4 bg = dark_ ? ImVec4{0.08f, 0.09f, 0.10f, 1} : ImVec4{0.98f, 0.98f, 0.98f, 1};
        const ImVec4 fg = dark_ ? ImVec4{1, 1, 1, 1} : ImVec4{0, 0, 0, 1};
        c[ImGuiCol_Text] = fg;
        c[ImGuiCol_TextDisabled] = dark_ ? ImVec4{0.74f, 0.76f, 0.80f, 1} : ImVec4{0.30f, 0.31f, 0.33f, 1};
        c[ImGuiCol_WindowBg] = c[ImGuiCol_ChildBg] = bg;
        c[ImGuiCol_PopupBg] = dark_ ? ImVec4{0.12f, 0.13f, 0.15f, 1} : ImVec4{1, 1, 1, 1};
        c[ImGuiCol_Border] = dark_ ? ImVec4{0.42f, 0.44f, 0.48f, 1} : ImVec4{0.45f, 0.45f, 0.48f, 1};
        c[ImGuiCol_FrameBg] = dark_ ? ImVec4{0.16f, 0.17f, 0.19f, 1} : ImVec4{0.92f, 0.92f, 0.93f, 1};
        c[ImGuiCol_FrameBgHovered] = mix(c[ImGuiCol_FrameBg], accent, 0.25f);
        c[ImGuiCol_FrameBgActive] = mix(c[ImGuiCol_FrameBg], accent, 0.40f);
        c[ImGuiCol_Button] = mix(c[ImGuiCol_FrameBg], accent, dark_ ? 0.30f : 0.18f);
        c[ImGuiCol_ButtonHovered] = mix(c[ImGuiCol_FrameBg], accent, 0.55f);
        c[ImGuiCol_ButtonActive] = mix(c[ImGuiCol_FrameBg], accent, 0.80f);
        c[ImGuiCol_Header] = mix(bg, accent, 0.35f);
        c[ImGuiCol_HeaderHovered] = mix(bg, accent, 0.50f);
        c[ImGuiCol_HeaderActive] = mix(bg, accent, 0.70f);
        c[ImGuiCol_CheckMark] = c[ImGuiCol_SliderGrab] = dark_ ? mix(accent, {1, 1, 1, 1}, 0.25f) : accent;
        c[ImGuiCol_SliderGrabActive] = accent;
        c[ImGuiCol_NavCursor] = accent;
        // tabs and floating editors' title bars in the app's colour, not ImGui's blue
        c[ImGuiCol_Tab] = c[ImGuiCol_TabDimmed] = c[ImGuiCol_Button];
        c[ImGuiCol_TabHovered] = c[ImGuiCol_ButtonHovered];
        c[ImGuiCol_TabSelected] = c[ImGuiCol_TabDimmedSelected] = mix(bg, accent, dark_ ? 0.55f : 0.35f);
        c[ImGuiCol_TabSelectedOverline] = c[ImGuiCol_TabDimmedSelectedOverline] = accent;
        c[ImGuiCol_TitleBg] = c[ImGuiCol_TitleBgCollapsed] = mix(bg, accent, 0.25f);
        c[ImGuiCol_TitleBgActive] = mix(bg, accent, 0.50f);
        accent_ = accent;
        accentHover_ = mix(accent, {1, 1, 1, 1}, 0.2f);
        accentActive_ = mix(accent, {0, 0, 0, 1}, 0.2f);
        accentText_ = luminance(accent) > 0.55f ? ImVec4{0, 0, 0, 1} : ImVec4{1, 1, 1, 1};
        title_ = accent;
    }
    s.FrameRounding = s.GrabRounding = s.ChildRounding = s.PopupRounding = 6;
    s.FrameBorderSize = 1;
    s.ChildBorderSize = 1;
    s.WindowBorderSize = 0;
    s.FramePadding = {10, 7};
    s.ItemSpacing = {10, 9};
    s.ScrollbarSize = 18;
    s.GrabMinSize = 18;
    s.ScaleAllSizes(dpi_);
    s.FontSizeBase = 20.0f;
    s.FontScaleMain = textScale_;
    s.FontScaleDpi = dpi_;
    ImGui::GetStyle() = s;
}

bool Shell::accentButton(const char* label, ImVec2 size) {
    ImGui::PushStyleColor(ImGuiCol_Button, accent_);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, accentHover_);
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, accentActive_);
    ImGui::PushStyleColor(ImGuiCol_Text, accentText_);
    bool pressed = ImGui::Button(label, size);
    ImGui::PopStyleColor(4);
    return pressed;
}

void Shell::frame() {
    animating_ = false;  // set again by whatever draws something moving
    drainDialogs();
    backgroundTicks();
    pollExport();
    previewTick();
    if (paletteStale_ && loadPalette()) applyStyle(dpi_);
    paletteStale_ = false;
    if (palettePending_ && !ImGui::IsMouseDown(ImGuiMouseButton_Left)) savePaletteNow();
    ImGuiIO& io = ImGui::GetIO();
    if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_K)) focusSearch_ = true;
    if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_O)) showOpenDialog();
    if (!io.WantTextInput) {
        if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_Z)) undo();
        if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_Y) || ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_Z)) redo();
    }
    if (form_ && ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_Enter)) runForm();
    if (!app_.launcher.empty()) return launcherFrame();

    shortcuts();

    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->WorkPos);
    ImGui::SetNextWindowSize(vp->WorkSize);
    ImGui::Begin("##root", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                     ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_MenuBar);
    menuBar();
    rootPopups();
    const bool panels = !focusMode_;
    if (panels && layout_.ribbon && showRibbon_) ribbon();
    const ImGuiStyle& style = ImGui::GetStyle();
    const float unit = ImGui::GetFontSize();
    const float statusH = panels && showStatus_ ? ImGui::GetFrameHeight() + style.ItemSpacing.y : 0.0f;
    const bool panelShown = !showHome_ && !panel_.empty();  // a system tool's panel fills the middle
    const bool docShown = !showHome_ && panel_.empty() && !currentDoc_.empty();
    // the right side: Properties / Layers / History, beside an open file. The start screen and
    // the system panels (which have their own sidebars) only get it once an action is picked
    const bool rightShown = panels && showProps_ && ((docShown && !gameActive()) || form_);
    const bool railShown = panels && showRail_ && docShown && railCount_ > 0;
    if (railShown != railWas_) canvas_.fit = true;  // the canvas got wider or narrower: fit the picture again
    railWas_ = railShown;
    ImGui::BeginChild("body", {0, -statusH}, ImGuiChildFlags_None, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    const float avail = ImGui::GetContentRegionAvail().x;
    if (railShown) {
        float railW = unit * 4;
        for (const char* l : {"Anchors", "Identify", "Measure", "Ellipse", "Select"}) railW = std::max(railW, ImGui::CalcTextSize(l).x);
        railW += unit * 1.2f + style.FramePadding.x * 2 + style.WindowPadding.x * 2;
        ImGui::BeginChild("rail", {railW, 0}, ImGuiChildFlags_Borders);
        toolRail();
        ImGui::EndChild();
        ImGui::SameLine();
    } else if (docShown && !focusMode_ && showRail_) {
        // count the rail's tools for the next frame without drawing them
        railCount_ = 0;
        railTools();
    }
    const float rightW = rightShown ? std::clamp(avail * 0.24f, unit * 13, unit * 20) : 0.0f;
    const float centerW = rightShown ? std::max(unit * 8, ImGui::GetContentRegionAvail().x - rightW - style.ItemSpacing.x) : 0.0f;
    ImGui::BeginChild("center", {centerW, 0}, ImGuiChildFlags_Borders, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    docTabs();
    if (panelShown) toolPanelView();
    else if (showHome_ || currentDoc_.empty()) homeView();
    else documentView();
    ImGui::EndChild();
    if (rightShown) {
        ImGui::SameLine();
        ImGui::BeginChild("right", {0, 0}, ImGuiChildFlags_Borders);
        propertiesPanel();
        ImGui::EndChild();
    }
    ImGui::EndChild();
    if (statusH > 0) statusBar();
    ImGui::End();
    searchPopup();
    dueBanner();
}

void Shell::searchPopup() {
    const float minX = searchMin_.x, maxX = searchMax_.x, maxY = searchMax_.y;
    if (query_.empty() || (!searchActive_ && !searchHovered_)) {
        searchHovered_ = false;
        searchPick_ = 0;
        return;
    }
    auto hits = search(query_, commands_, 12);
    searchPick_ = std::clamp(searchPick_, 0, std::max(0, static_cast<int>(hits.size()) - 1));
    ImGui::SetNextWindowPos({minX, maxY + 2});
    ImGui::SetNextWindowSizeConstraints({maxX - minX, 0}, {maxX - minX, ImGui::GetFontSize() * 30});
    ImGui::Begin("##searchresults", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoFocusOnAppearing |
                     ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoNav);
    if (hits.empty()) ImGui::TextDisabled("No action matches \"%s\". Try another word.", query_.c_str());
    for (std::size_t i = 0; i < hits.size(); ++i) {
        const CommandInfo& c = *hits[i].command;
        ImGui::PushID(static_cast<int>(i));
        if (ImGui::Selectable(c.title.c_str(), static_cast<int>(i) == searchPick_)) {
            select(c.id);
            query_.clear();
        }
        ImGui::SameLine();
        ImGui::TextDisabled("%s", c.id.c_str());
        ImGui::PopID();
    }
    searchHovered_ = ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows | ImGuiHoveredFlags_AllowWhenBlockedByActiveItem);
    ImGui::End();
}

void Shell::newPopup() {
    if (!ImGui::BeginPopup("new")) return;
    ImGui::TextDisabled("Start something new:");
    bool any = false;
    // every kind of new file: image.new, office.doc_new, office.sheet_new...
    for (const auto& tool : tools_) {
        for (const auto& c : commands_) {
            if (c.id.compare(0, tool.size() + 1, tool + ".") != 0 || commandVerb(c.id) != "new") continue;
            any = true;
            if (ImGui::Selectable((c.title + "  (" + toolTitle(tool) + ")").c_str())) select(c.id);
        }
    }
    if (!any) ImGui::TextUnformatted("Nothing to create in this app.");
    ImGui::EndPopup();
}

void Shell::settingsPopup() {
    if (!ImGui::BeginPopup("settings")) return;
    ImGui::TextUnformatted("Text size");
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 12);
    if (ImGui::SliderFloat("##textsize", &textScale_, 0.75f, 2.0f, "%.2fx")) applyStyle(dpi_);
    coloursSettings();
    updatesSettings();
    ImGui::EndPopup();
}

void Shell::homeView() {
    const float unit = ImGui::GetFontSize();
    ImGui::BeginChild("home", {0, 0}, ImGuiChildFlags_None);
    inboxBannerView();
    suggestionBannerView();
    heading("What would you like to do?", 1.6f);
    ImGui::Spacing();
    const ImVec2 big{unit * 11, unit * 3};
    if (accentButton("Open a file", big)) showOpenDialog();
    ImGui::SameLine();
    if (accentButton("Start something new", big)) openNew_ = true;
    ImGui::SameLine();
    if (ImGui::Button("Find an action", big)) focusSearch_ = true;

    if (!recent_.empty()) {
        ImGui::Spacing();
        heading("Recent files", 1.15f);
        for (std::size_t i = 0; i < recent_.size() && i < 8; ++i) {
            ImGui::PushID(static_cast<int>(i));
            const std::string path = recent_[i];
            if (ImGui::Selectable(fileName(path).c_str())) openFile(path);
            ImGui::SetItemTooltip("%s", path.c_str());
            ImGui::SameLine();
            ImGui::TextDisabled("%s", std::filesystem::path(path).parent_path().string().c_str());
            ImGui::PopID();
        }
    }

    ImGui::Spacing();
    heading(tools_.size() == 1 ? "What this app does" : "Tools in this app", 1.15f);
    const float cardW = unit * 15, cardH = unit * 9.5f;
    const int columns = std::max(1, static_cast<int>((ImGui::GetContentRegionAvail().x + ImGui::GetStyle().ItemSpacing.x) /
                                                     (cardW + ImGui::GetStyle().ItemSpacing.x)));
    for (std::size_t i = 0; i < tools_.size(); ++i) {
        const std::string& tool = tools_[i];
        if (i % static_cast<std::size_t>(columns)) ImGui::SameLine();
        ImGui::PushID(tool.c_str());
        ImGui::BeginChild("card", {cardW, cardH}, ImGuiChildFlags_Borders, ImGuiWindowFlags_NoScrollbar);
        ImGui::PushStyleColor(ImGuiCol_Text, title_);
        heading(toolTitle(tool), 1.2f);
        ImGui::PopStyleColor();
        ImGui::PushTextWrapPos(0.0f);
        ImGui::TextUnformatted(toolBlurb(tool).c_str());
        ImGui::PopTextWrapPos();
        // quick actions, wrapped onto new lines when the card is full
        std::vector<std::pair<std::string, std::string>> buttons;
        if (hasPanel(tool)) buttons.emplace_back("Open", "panel:" + tool);
        for (const auto& id : beginnerCommands(tool, commands_, 3)) {
            if (const CommandInfo* c = find(id)) buttons.emplace_back(c->title, id);
        }
        buttons.emplace_back("More...", std::string());
        const float right = ImGui::GetCursorScreenPos().x + ImGui::GetContentRegionAvail().x;
        for (std::size_t b = 0; b < buttons.size(); ++b) {
            const float w = ImGui::CalcTextSize(buttons[b].first.c_str()).x + ImGui::GetStyle().FramePadding.x * 2;
            if (b > 0) {
                ImGui::SameLine();
                if (ImGui::GetCursorScreenPos().x + w > right) ImGui::NewLine();
            }
            if (ImGui::SmallButton(buttons[b].first.c_str())) {
                if (buttons[b].second.empty()) expandTool_ = tool;
                else if (buttons[b].second.rfind("panel:", 0) == 0) openPanel(tool);
                else select(buttons[b].second);
            }
        }
        ImGui::EndChild();
        ImGui::PopID();
    }
    ImGui::EndChild();
}

void Shell::documentView() {
    auto doc = session_.workspace().get(currentDoc_);
    if (!doc) {
        showHome_ = true;
        return;
    }
    suggestionBannerView();
    if (doc->kind() == "pdf" && find("pdf.view")) {
        pdfView();
        return;
    }
    transportBar();
    if (CellGrid layout; doc->grid(layout) && !layout.sheets.empty()) {
        gridView(layout);
        return;
    }
    canvasToolbar();
    if (doc->kind() == "model" && find("model.keys")) {
        // the 3D view on top, the dope sheet below (dopesheet.cpp)
        const float avail = ImGui::GetContentRegionAvail().y;
        const float ds = std::min(dopeSheetHeight(), avail * 0.45f);
        ImGui::BeginChild("preview", {0, std::max(64.0f, avail - ds - ImGui::GetStyle().ItemSpacing.y)});
        canvasView();
        ImGui::EndChild();
        ImGui::BeginChild("dopesheet", {0, 0});
        dopeSheetView();
        ImGui::EndChild();
        return;
    }
    TimelineLayout layout;
    if (doc->timeline(layout)) {
        // the preview on top, the timeline below
        const float want = timelineHeight(layout);
        const float avail = ImGui::GetContentRegionAvail().y;
        const float tl = std::min(want, avail * 0.6f);
        ImGui::BeginChild("preview", {0, std::max(64.0f, avail - tl - ImGui::GetStyle().ItemSpacing.y)});
        if (doc->kind() == "song" && find("beats.notes"))
            pianoRollView();  // songs: the pattern's notes over the arrangement (sound_view.cpp)
        else
            canvasView();
        ImGui::EndChild();
        ImGui::BeginChild("timeline", {0, 0});
        timelineView(layout);
        ImGui::EndChild();
        return;
    }
    canvasView();
}

void Shell::rightPanel() {
    formPanel();
    // a form shows its own result; brush strokes, undo and saves report on the status bar
    if (result_ && form_ && resultFor_ == form_->title) {
        ImGui::Spacing();
        ImGui::Separator();
        const bool ok = result_->ok;
        const ImVec4 colour = ok ? (darkLook() ? ImVec4{0.45f, 0.95f, 0.55f, 1} : ImVec4{0.0f, 0.45f, 0.1f, 1})
                                 : (darkLook() ? ImVec4{1.0f, 0.55f, 0.5f, 1} : ImVec4{0.7f, 0.0f, 0.0f, 1});
        ImGui::PushStyleColor(ImGuiCol_Text, colour);
        heading(ok ? "Done" : "That didn't work", 1.1f);
        ImGui::PushTextWrapPos(0.0f);
        ImGui::TextUnformatted(result_->message.c_str());
        ImGui::PopTextWrapPos();
        ImGui::PopStyleColor();
        for (const auto& line : resultLines(*result_)) wrappedMuted(line);
        exportProgress();
        launchOfferView();
        if (scrollToResult_) {
            ImGui::SetScrollHereY(1.0f);
            scrollToResult_ = false;
        }
        if (!result_->attachments.empty()) wrappedMuted(std::to_string(result_->attachments.size()) + " file(s) made in memory.");
    }
}

void Shell::formPanel() {
    if (!form_) {
        if (clipPanel()) return;
        // nothing picked: what's open, and where the actions are
        auto doc = session_.workspace().get(currentDoc_);
        if (doc && !showHome_ && panel_.empty()) {
            heading(docTitle(currentDoc_), 1.1f);
            wrappedMuted(doc->summary());
            if (!doc->path.empty() && doc->summary().find(doc->path) == std::string::npos) wrappedMuted(doc->path);
            if (handoff_ && session_.registry().find("app.targets") && ImGui::Button("Send to another app...")) openSend_ = true;
            ImGui::Spacing();
        }
        wrappedMuted(layout_.ribbon ? "Pick a button on the ribbon or an action from the menus, and its settings show up here."
                                    : "Pick an action from the menus, and its settings show up here.");
        wrappedMuted("Ctrl+K finds any action by typing.");
        return;
    }
    heading(form_->title, 1.25f);
    {
        std::string lead, rest;
        splitDescription(readableDescription(form_->description, commands_), lead, rest);
        wrappedMuted(lead);
        if (!rest.empty()) {
            ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
            const bool open = ImGui::TreeNodeEx("More about this", ImGuiTreeNodeFlags_NoTreePushOnOpen | ImGuiTreeNodeFlags_SpanAvailWidth);
            ImGui::PopStyleColor();
            if (open) wrappedMuted(rest);
        }
    }
    ImGui::Spacing();
    previewControls();
    bool needsDoc = false;
    for (std::size_t i = 0; i < form_->fields.size(); ++i) {
        Field& f = form_->fields[i];
        if (f.hidden) continue;
        needsDoc = needsDoc || (f.kind == FieldKind::Document && f.required);
        field(f, i);
    }
    if (needsDoc && docIds().empty()) wrappedMuted("Open a file or start a new one first: this action works on an open file.");
}

void Shell::field(Field& f, std::size_t index) {
    ImGui::PushID(static_cast<int>(index));
    const bool hasDefault = !std::holds_alternative<std::monostate>(f.spec.defaultValue);
    if (f.kind != FieldKind::Bool) {
        ImGui::TextUnformatted(f.label.c_str());
        if (!f.required) {
            ImGui::SameLine();
            ImGui::TextDisabled("(optional)");
        }
        if (pro_) ImGui::SetItemTooltip("%s", f.help.c_str());
    }
    ImGui::SetNextItemWidth(-1);
    switch (f.kind) {
        case FieldKind::Bool:
            if (ImGui::Checkbox(f.label.c_str(), &f.flag)) f.set = true;
            if (pro_) ImGui::SetItemTooltip("%s", f.help.c_str());
            break;
        case FieldKind::Choice: {
            const char* shown = f.text.empty() ? "(automatic)" : f.text.c_str();
            if (ImGui::BeginCombo("##v", shown)) {
                if (!f.required && !hasDefault && ImGui::Selectable("(automatic)", f.text.empty())) f.text.clear();
                for (const auto& choice : f.spec.choices) {
                    if (ImGui::Selectable(choice.c_str(), choice == f.text)) setFieldText(f, choice);
                }
                ImGui::EndCombo();
            }
            break;
        }
        case FieldKind::Document: {
            const auto ids = docIds();
            if (ImGui::BeginCombo("##v", f.text.empty() ? "(no file open)" : f.text.c_str())) {
                for (const auto& id : ids) {
                    auto doc = session_.workspace().get(id);
                    std::string label = id + (doc ? "  " + doc->summary() : std::string());
                    if (ImGui::Selectable(label.c_str(), id == f.text)) setFieldText(f, id);
                }
                ImGui::EndCombo();
            }
            break;
        }
        case FieldKind::Color: {
            Rgba c = parseHex(f.text).value_or(Rgba{0, 0, 0, f.text.empty() ? 0.0f : 1.0f});
            float v[4] = {c.r, c.g, c.b, c.a};
            if (ImGui::ColorEdit4("##pick", v, ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_AlphaBar | ImGuiColorEditFlags_AlphaPreviewHalf)) {
                setFieldText(f, toHex({v[0], v[1], v[2], v[3]}));
            }
            ImGui::SameLine();
            ImGui::SetNextItemWidth(-1);
            if (ImGui::InputTextWithHint("##v", f.required ? "#rrggbb or a name like red" : "(automatic)", &f.text)) f.set = !f.text.empty();
            break;
        }
        case FieldKind::OpenFile:
        case FieldKind::SaveFile:
        case FieldKind::Folder: {
            const float browseW = ImGui::CalcTextSize("Browse...").x + ImGui::GetStyle().FramePadding.x * 2;
            ImGui::SetNextItemWidth(-browseW - ImGui::GetStyle().ItemSpacing.x);
            const char* hint = f.kind == FieldKind::Folder ? "a folder" : f.kind == FieldKind::SaveFile ? "where to save" : "a file";
            if (ImGui::InputTextWithHint("##v", hint, &f.text)) f.set = !f.text.empty();
            ImGui::SameLine();
            if (ImGui::Button("Browse...")) showFieldDialog(index);
            break;
        }
        case FieldKind::Integer:
        case FieldKind::Number: {
            if (!f.required && !hasDefault) {
                bool automatic = !f.set;
                if (ImGui::Checkbox("Automatic", &automatic)) {
                    f.set = !automatic;
                    if (f.set && f.min) f.number = std::max(f.number, *f.min);
                }
                if (!f.set) break;
                ImGui::SetNextItemWidth(-1);
            }
            if (f.min && f.max) {
                if (f.kind == FieldKind::Integer) {
                    int v = static_cast<int>(std::lround(f.number));
                    if (ImGui::SliderInt("##v", &v, static_cast<int>(*f.min), static_cast<int>(*f.max))) {
                        f.number = v;
                        f.set = true;
                    }
                } else {
                    if (ImGui::SliderScalar("##v", ImGuiDataType_Double, &f.number, &*f.min, &*f.max, "%.2f")) f.set = true;
                }
            } else if (f.kind == FieldKind::Integer) {
                int v = static_cast<int>(std::lround(f.number));
                if (ImGui::InputInt("##v", &v)) {
                    f.number = v;
                    f.set = true;
                }
            } else {
                if (ImGui::InputDouble("##v", &f.number, 1.0, 10.0, "%g")) f.set = true;
            }
            break;
        }
        case FieldKind::NumberList:
            if (ImGui::InputTextWithHint("##v", "numbers, e.g. 10, 20, 30", &f.text)) f.set = !f.text.empty();
            break;
        case FieldKind::Text: {
            const std::string& n = f.spec.name;
            const bool multi = n == "text" || n == "content" || n == "markdown" || n == "script" || n == "body";
            bool changed = multi ? ImGui::InputTextMultiline("##v", &f.text, {-1, ImGui::GetFontSize() * 5})
                                 : ImGui::InputTextWithHint("##v", f.required ? "" : "(automatic)", &f.text);
            if (changed) f.set = !f.text.empty();
            break;
        }
    }
    if (!pro_ && !f.help.empty() && f.kind != FieldKind::Document) wrappedMuted(f.help);
    ImGui::Spacing();
    ImGui::PopID();
}

void Shell::historyPanel() {
    const auto labels = session_.history().labels();
    if (labels.empty()) {
        wrappedMuted("Nothing changed yet. Every change shows up here, and Undo takes it back.");
        return;
    }
    for (std::size_t i = 0; i < labels.size(); ++i) {
        const bool last = i + 1 == labels.size();
        std::string line = std::to_string(i + 1) + ". " + labels[i];
        if (last) ImGui::TextUnformatted((line + "   <- now").c_str());
        else ImGui::TextDisabled("%s", line.c_str());
    }
    if (session_.history().canRedo()) wrappedMuted("Some steps were undone: Redo brings them back.");
}

}  // namespace wetwaresuite::gui

namespace wetwaresuite::gui {

bool Shell::clipPanel() {
    TimelineState& st = timeline_;
    auto doc = session_.workspace().get(currentDoc_);
    TimelineLayout layout;
    if (!doc || st.doc != currentDoc_ || st.selClip.empty() || !doc->timeline(layout)) return false;
    const std::vector<ClipProperty> props = doc->clipProperties(st.selClip);
    if (props.empty()) return false;
    std::string label = st.selClip;
    for (const auto& t : layout.tracks)
        for (const auto& c : t.clips)
            if (c.id == st.selClip) label = c.label.empty() ? c.id : c.label;
    heading(label, 1.25f);
    wrappedMuted("Drag a slider to change this clip (Ctrl+click to type a value). Each change is one step in the history.");
    std::string group;
    for (const auto& p : props) {
        ImGui::PushID(p.name.c_str());
        if (p.group != group) {
            group = p.group;
            ImGui::Spacing();
            ImGui::TextDisabled("%s", group.c_str());
        }
        auto apply = [&](double value) {
            TimelineEdit e{TimelineEditKind::SetProperty, st.selClip};
            e.name = p.name;
            e.at = value;
            runTimelineEdit(layout, e);
        };
        if (p.toggle) {
            bool on = p.value != 0;
            if (ImGui::Checkbox(p.label.c_str(), &on)) apply(on ? 1 : 0);
        } else {
            auto it = clipEdits_.find(p.name);
            float v = it != clipEdits_.end() ? it->second : static_cast<float>(p.value);
            ImGui::TextUnformatted(p.label.c_str());
            ImGui::SetNextItemWidth(-ImGui::GetFrameHeight() - ImGui::GetStyle().ItemSpacing.x);
            // wide positive ranges (speed 0.25..4x, scale) get a log scale, so 1x sits in the middle
            const ImGuiSliderFlags flags = p.min > 0 && p.max / p.min >= 10 ? ImGuiSliderFlags_Logarithmic : ImGuiSliderFlags_None;
            if (ImGui::SliderFloat("##v", &v, static_cast<float>(p.min), static_cast<float>(p.max), p.format.c_str(), flags)) clipEdits_[p.name] = v;
            if (ImGui::IsItemDeactivated()) {
                if (ImGui::IsItemDeactivatedAfterEdit() && std::fabs(v - p.value) > 1e-6) apply(v);
                clipEdits_.erase(p.name);
            }
            ImGui::SameLine();
            // back to where a new clip starts
            const double neutral = p.name == "speed" || p.name == "opacity" || p.name == "scale" || p.name == "contrast" || p.name == "saturation" ||
                                           p.name == "volume"
                                       ? 1.0
                                       : 0.0;
            ImGui::BeginDisabled(std::fabs(p.value - neutral) < 1e-6);
            if (ImGui::Button("R", {ImGui::GetFrameHeight(), 0})) apply(neutral);
            ImGui::EndDisabled();
            ImGui::SetItemTooltip("Reset %s", p.label.c_str());
        }
        ImGui::PopID();
    }
    ImGui::Spacing();
    // the command the sliders run, opened as a full form for everything else
    TimelineEdit probe{TimelineEditKind::SetProperty, st.selClip};
    probe.name = props.front().name;
    probe.at = props.front().value;
    const std::vector<std::string> line = cli::split(doc->timelineCommand(probe));
    const std::string setter = line.empty() ? std::string() : line.front();
    if (!setter.empty() && find(setter) && ImGui::Button("All settings...", {-1, 0})) {
        select(setter);
        if (form_) {
            for (auto& f : form_->fields) {
                if (f.spec.name == "clip") {
                    f.text = st.selClip;
                    f.set = true;
                }
            }
        }
    }
    ImGui::SetItemTooltip("Every setting of this clip: position, crop, colour wheels, keys, title text...");
    ImGui::Spacing();
    ImGui::Separator();
    heading("Pick an action", 1.0f);
    wrappedMuted("Or choose one on the left, or type what you want to do in the search box at the top.");
    return true;
}

}  // namespace wetwaresuite::gui
