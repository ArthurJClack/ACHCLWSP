// The pipeline builder (files app, automate): pick blocks from every command in
// the app, fill in their settings, drag them into order, optionally run them for
// every file in a folder, then try it, run it or save it as a pipeline file.
// It builds the same JSON automate.pipeline runs, so a saved pipeline also runs
// from the command line, a watch folder or Claude.
#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <sstream>

#include "tool_panels.hpp"
// after imgui.h
#include "imgui_stdlib.h"

namespace wetwaresuite::gui {

namespace {

std::string lowerText(std::string s) {
    for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

std::string niceName(std::string s) {
    for (auto& c : s) {
        if (c == '_') c = ' ';
    }
    if (!s.empty()) s[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(s[0])));
    return s;
}

void bigText(const std::string& text, float scale) {
    ImGui::PushFont(nullptr, ImGui::GetStyle().FontSizeBase * scale);
    ImGui::TextUnformatted(text.c_str());
    ImGui::PopFont();
}

std::string* argSlot(PipeBlock& b, const std::string& name) {
    for (auto& [k, v] : b.args) {
        if (k == name) return &v;
    }
    b.args.emplace_back(name, "");
    return &b.args.back().second;
}

std::string summary(const PipeBlock& b) {
    std::string s;
    for (const auto& [k, v] : b.args) {
        if (v.empty()) continue;
        if (!s.empty()) s += "  ";
        s += k + "=" + v;
        if (s.size() > 70) {
            s = s.substr(0, 67) + "...";
            break;
        }
    }
    if (!b.ext.empty()) s += (s.empty() ? "" : "  ") + std::string("only .") + b.ext;
    return s.empty() ? std::string("(default settings)") : s;
}

bool fileLike(const ParamSpec& p) {
    const std::string n = p.name;
    for (const char* w : {"path", "input", "file", "output", "folder", "root", "to", "source"}) {
        if (n == w || n.find(w) != std::string::npos) return true;
    }
    return false;
}

}  // namespace

void Shell::pipelinePanel() {
    PipelinePanel& pp = panels_->pipeline;
    PipeModel& m = pp.model;
    const float unit = ImGui::GetFontSize();

    // ---- what it runs on ----
    ImGui::TextUnformatted("Name");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(unit * 14);
    ImGui::InputTextWithHint("##name", "my pipeline", &m.name);
    ImGui::SameLine(0, unit * 1.5f);
    ImGui::Checkbox("Run for every file in", &m.forEach);
    ImGui::SameLine();
    ImGui::BeginDisabled(!m.forEach);
    ImGui::SetNextItemWidth(unit * 16);
    ImGui::InputTextWithHint("##glob", "photos/*.jpg", &m.glob);
    ImGui::SameLine();
    if (ImGui::Button("Folder...")) askFolder("pipeline-glob");
    ImGui::EndDisabled();
    ImGui::SetItemTooltip("* matches any name, ** any folder depth: photos/**/*.png");
    if (m.forEach) ImGui::TextDisabled("In settings, $file is the file, $stem its name without extension, $ext its extension and $dir its folder.");
    else ImGui::TextDisabled("Tick \"Run for every file\" to repeat the blocks for each file in a folder.");
    ImGui::Separator();

    const float bottom = unit * 9.5f;
    const float leftW = std::max(unit * 18, ImGui::GetContentRegionAvail().x * 0.42f);

    // ---- the blocks ----
    ImGui::BeginChild("blocks", {leftW, -bottom}, ImGuiChildFlags_Borders);
    if (m.blocks.empty()) {
        ImGui::PushTextWrapPos(0);
        ImGui::TextDisabled("No blocks yet. Add one below: each block is one action, like \"Convert file\" or \"Resize\". "
                            "They run top to bottom; drag them to change the order.");
        ImGui::PopTextWrapPos();
    }
    int dropFrom = -1, dropTo = -1, remove = -1;
    for (int i = 0; i < static_cast<int>(m.blocks.size()); ++i) {
        PipeBlock& b = m.blocks[static_cast<std::size_t>(i)];
        ImGui::PushID(i);
        const CommandInfo* info = find(b.command);
        const std::string title = std::to_string(i + 1) + ".  " + (info ? info->title : b.command) + (b.off ? "  (off)" : "");
        const float h = unit * 2.9f;
        const ImVec2 at = ImGui::GetCursorScreenPos();
        if (b.off) ImGui::BeginDisabled();
        if (ImGui::Selectable("##block", pp.selected == i, ImGuiSelectableFlags_AllowOverlap, {0, h})) pp.selected = i;
        if (b.off) ImGui::EndDisabled();
        if (ImGui::BeginDragDropSource()) {
            ImGui::SetDragDropPayload("PIPE_BLOCK", &i, sizeof i);
            ImGui::TextUnformatted(title.c_str());
            ImGui::EndDragDropSource();
        }
        if (ImGui::BeginDragDropTarget()) {
            if (const ImGuiPayload* pl = ImGui::AcceptDragDropPayload("PIPE_BLOCK")) {
                dropFrom = *static_cast<const int*>(pl->Data);
                dropTo = i;
            }
            ImGui::EndDragDropTarget();
        }
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const ImVec2 end{at.x + ImGui::GetContentRegionAvail().x, at.y + h};
        dl->AddRect(at, end, ImGui::GetColorU32(pp.selected == i ? accent_ : ImGui::GetStyleColorVec4(ImGuiCol_Border)), 6, 0,
                    pp.selected == i ? 2.0f : 1.0f);
        dl->AddRectFilled(at, {at.x + unit * 0.35f, end.y}, ImGui::GetColorU32(accent_), 6, ImDrawFlags_RoundCornersLeft);
        dl->AddText({at.x + unit * 0.8f, at.y + unit * 0.35f}, ImGui::GetColorU32(ImGuiCol_Text), title.c_str());
        dl->AddText({at.x + unit * 0.8f, at.y + unit * 1.55f}, ImGui::GetColorU32(ImGuiCol_TextDisabled), summary(b).c_str());
        // move, switch off, remove
        const float bw = unit * 1.7f;
        ImGui::SetCursorScreenPos({end.x - bw * 4 - unit * 0.6f, at.y + (h - ImGui::GetFrameHeight()) / 2});
        ImGui::BeginDisabled(i == 0);
        if (ImGui::ArrowButton("up", ImGuiDir_Up)) dropFrom = i, dropTo = i - 1;
        ImGui::EndDisabled();
        ImGui::SetItemTooltip("Move up");
        ImGui::SameLine(0, 2);
        ImGui::BeginDisabled(i + 1 == static_cast<int>(m.blocks.size()));
        if (ImGui::ArrowButton("down", ImGuiDir_Down)) dropFrom = i, dropTo = i + 1;
        ImGui::EndDisabled();
        ImGui::SetItemTooltip("Move down");
        ImGui::SameLine(0, 2);
        if (ImGui::SmallButton(b.off ? "on" : "off")) b.off = !b.off;
        ImGui::SetItemTooltip(b.off ? "Switch this block back on" : "Skip this block without removing it");
        ImGui::SameLine(0, 2);
        if (ImGui::SmallButton("x")) remove = i;
        ImGui::SetItemTooltip("Remove");
        ImGui::SetCursorScreenPos({at.x, end.y + unit * 0.3f});
        ImGui::Dummy({1, 1});
        ImGui::PopID();
    }
    if (dropFrom >= 0 && dropTo >= 0) {
        const int now = moveBlock(m, dropFrom, dropTo);
        if (pp.selected == dropFrom) pp.selected = now;
    }
    if (remove >= 0) {
        m.blocks.erase(m.blocks.begin() + remove);
        if (pp.selected >= static_cast<int>(m.blocks.size())) pp.selected = static_cast<int>(m.blocks.size()) - 1;
    }
    if (ImGui::Button("+ Add block", {-1, 0})) {
        pp.search.clear();
        pp.focusSearch = true;
        ImGui::OpenPopup("add");
    }
    if (ImGui::BeginPopup("add")) {
        if (pp.focusSearch) {
            ImGui::SetKeyboardFocusHere();
            pp.focusSearch = false;
        }
        ImGui::SetNextItemWidth(unit * 22);
        ImGui::InputTextWithHint("##find", "what should it do? (resize, convert, tag...)", &pp.search);
        ImGui::BeginChild("list", {unit * 22, unit * 18});
        const std::string q = lowerText(pp.search);
        std::string chosen;
        session_.registry().forEach([&](const CommandInfo& c) {
            if (c.id.rfind("app.", 0) == 0 || c.id.rfind("automate.watch", 0) == 0) return;
            if (!q.empty() && lowerText(c.title + " " + c.id + " " + c.description).find(q) == std::string::npos) return;
            if (ImGui::Selectable((c.title + "##" + c.id).c_str())) chosen = c.id;
            ImGui::SetItemTooltip("%s\n%s", c.id.c_str(), c.description.substr(0, 300).c_str());
            ImGui::SameLine(unit * 13);
            ImGui::TextDisabled("%s", c.id.c_str());
        });
        ImGui::EndChild();
        if (!chosen.empty()) {
            PipeBlock b;
            b.command = chosen;
            // a file goes in the first file setting by default when the pipeline loops over files
            if (const CommandInfo* c = find(chosen); c && m.forEach) {
                for (const auto& p : c->params) {
                    if (p.type == ParamType::String && (p.name == "path" || p.name == "input" || p.name == "file")) {
                        b.args.emplace_back(p.name, "$file");
                        break;
                    }
                }
            }
            m.blocks.push_back(std::move(b));
            pp.selected = static_cast<int>(m.blocks.size()) - 1;
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
    ImGui::EndChild();

    // ---- the selected block's settings ----
    ImGui::SameLine();
    ImGui::BeginChild("settings", {0, -bottom}, ImGuiChildFlags_Borders);
    if (pp.selected >= 0 && pp.selected < static_cast<int>(m.blocks.size())) {
        PipeBlock& b = m.blocks[static_cast<std::size_t>(pp.selected)];
        const CommandInfo* info = find(b.command);
        bigText(info ? info->title : b.command, 1.15f);
        ImGui::TextDisabled("%s", b.command.c_str());
        if (info) {
            ImGui::PushTextWrapPos(0);
            const std::string first = info->description.substr(0, info->description.find(". ") == std::string::npos ? info->description.size() : info->description.find(". ") + 1);
            ImGui::TextUnformatted(first.c_str());
            ImGui::PopTextWrapPos();
            ImGui::Spacing();
            for (const auto& p : info->params) {
                if (p.name == "doc" || p.name == "preview") continue;  // the pipeline's own session picks the document
                ImGui::PushID(p.name.c_str());
                std::string* v = argSlot(b, p.name);
                ImGui::TextUnformatted((niceName(p.name) + (p.required ? "" : "  ")).c_str());
                if (p.required) {
                    ImGui::SameLine();
                    ImGui::TextDisabled("(needed)");
                }
                const std::string def = std::holds_alternative<std::monostate>(p.defaultValue) ? std::string() : toText(p.defaultValue);
                if (p.type == ParamType::Bool || !p.choices.empty()) {
                    std::vector<std::string> opts = p.type == ParamType::Bool ? std::vector<std::string>{"true", "false"} : p.choices;
                    const std::string shown = v->empty() ? (def.empty() ? std::string("(default)") : "(default: " + def + ")") : *v;
                    ImGui::SetNextItemWidth(-1);
                    if (ImGui::BeginCombo("##v", shown.c_str())) {
                        if (ImGui::Selectable("(default)", v->empty())) v->clear();
                        for (const auto& o : opts) {
                            if (ImGui::Selectable(o.c_str(), *v == o)) *v = o;
                        }
                        ImGui::EndCombo();
                    }
                } else {
                    const bool file = fileLike(p);
                    const float extra = file ? unit * (m.forEach ? 7.2f : 4.6f) : 0;
                    ImGui::SetNextItemWidth(-1 - extra);
                    ImGui::InputTextWithHint("##v", def.empty() ? "" : def.c_str(), v);
                    if (file) {
                        ImGui::SameLine(0, 4);
                        if (ImGui::Button("Browse")) askFile("pipeline-arg:" + std::to_string(pp.selected) + ":" + p.name);
                        if (m.forEach) {
                            ImGui::SameLine(0, 4);
                            if (ImGui::Button("$file")) *v = "$file";
                            ImGui::SetItemTooltip("The file this round is for. Output names can use $dir/$stem.png");
                        }
                    }
                }
                if (!p.description.empty()) {
                    ImGui::PushTextWrapPos(0);
                    ImGui::TextDisabled("%s", p.description.c_str());
                    ImGui::PopTextWrapPos();
                }
                ImGui::Spacing();
                ImGui::PopID();
            }
        } else {
            ImGui::TextColored({0.9f, 0.4f, 0.4f, 1}, "This app has no %s command.", b.command.c_str());
        }
        ImGui::Separator();
        if (m.forEach) {
            ImGui::TextUnformatted("Only for files ending in");
            ImGui::SetNextItemWidth(-1);
            ImGui::InputTextWithHint("##ext", "png,jpg (empty = every file)", &b.ext);
        }
        ImGui::Checkbox("Keep going if this block fails", &b.keepGoing);
    } else {
        ImGui::TextDisabled("Click a block to change its settings.");
    }
    ImGui::EndChild();

    // ---- try, run, save ----
    const std::string json = pipelineJson(m);
    const bool any = std::any_of(m.blocks.begin(), m.blocks.end(), [](const PipeBlock& b) { return !b.off; });
    auto runIt = [&](bool dry) {
        Result r = run("automate.pipeline", {{"text", json}, {"dry_run", dry}});
        pp.output = r.message;
        pp.outputOk = r.ok;
    };
    ImGui::BeginDisabled(!any);
    if (ImGui::Button("Try it (nothing changes)")) runIt(true);
    ImGui::SetItemTooltip("Shows every command it would run, with the files filled in");
    ImGui::SameLine();
    ImGui::PushStyleColor(ImGuiCol_Button, accent_);
    ImGui::PushStyleColor(ImGuiCol_Text, accentText_);
    if (ImGui::Button("Run")) runIt(false);
    ImGui::PopStyleColor(2);
    ImGui::EndDisabled();
    ImGui::SameLine(0, unit * 1.5f);
    ImGui::SetNextItemWidth(unit * 14);
    ImGui::InputText("##path", &pp.path);
    ImGui::SameLine();
    if (ImGui::Button("Save")) {
        Result r = run("automate.pipeline_save", {{"path", pp.path}, {"text", json}, {"overwrite", true}});
        pp.output = r.message;
        pp.outputOk = r.ok;
    }
    ImGui::SetItemTooltip("Saves a pipeline file that also runs from the command line, a watch folder or Claude");
    ImGui::SameLine();
    if (ImGui::Button("Open...")) askFile("pipeline-open");
    ImGui::SameLine();
    if (ImGui::Button("New")) {
        m = PipeModel{};
        pp.selected = -1;
        pp.output.clear();
    }
    ImGui::SameLine();
    if (ImGui::Button("Copy as text")) ImGui::SetClipboardText(json.c_str());
    ImGui::SetItemTooltip("The pipeline as JSON, for a file, a script or Claude");
    if (!pp.outputOk) ImGui::PushStyleColor(ImGuiCol_Text, ImVec4{0.95f, 0.45f, 0.45f, 1});
    ImGui::InputTextMultiline("##out", &pp.output, {-1, -1}, ImGuiInputTextFlags_ReadOnly);
    if (!pp.outputOk) ImGui::PopStyleColor();
}

void Shell::pipelineOpen(const std::string& path) {
    PipelinePanel& pp = panels_->pipeline;
    std::ifstream in(tp::pathOf(path), std::ios::binary);
    std::ostringstream text;
    text << in.rdbuf();
    std::string err;
    PipeModel loaded;
    if (!in) {
        pp.output = "can't read " + path;
        pp.outputOk = false;
    } else if (parsePipelineJson(text.str(), loaded, err)) {
        pp.model = std::move(loaded);
        pp.selected = pp.model.blocks.empty() ? -1 : 0;
        pp.path = path;
        pp.output = "opened " + path;
        pp.outputOk = true;
    } else {
        pp.output = err;
        pp.outputOk = false;
    }
}

void Shell::pipelineChosen(const std::string& purpose, const std::string& path) {
    PipelinePanel& pp = panels_->pipeline;
    openPanel("pipeline");
    if (purpose == "pipeline-glob") {
        pp.model.glob = path + "/*";
        pp.model.forEach = true;
    } else if (purpose == "pipeline-open") {
        pipelineOpen(path);
    } else if (purpose.rfind("pipeline-arg:", 0) == 0) {
        const std::string rest = purpose.substr(13);
        const auto colon = rest.find(':');
        const int block = std::atoi(rest.substr(0, colon).c_str());
        if (colon != std::string::npos && block >= 0 && block < static_cast<int>(pp.model.blocks.size())) {
            *argSlot(pp.model.blocks[static_cast<std::size_t>(block)], rest.substr(colon + 1)) = path;
        }
    }
}

}  // namespace wetwaresuite::gui
