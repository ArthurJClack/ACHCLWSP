// The code panel: a project tree, tabs, the editor, find and replace, go to
// line, an outline, search in files, run and build with the toolchains already
// installed, clickable errors and a git panel. Everything runs code.* commands.
#include <algorithm>
#include <climits>

#include "tool_panels.hpp"
// after imgui.h
#include "imgui_stdlib.h"

namespace wetwaresuite::gui {

namespace {

std::string commentFor(const std::string& lang) {
    static const char* hash[] = {"python", "ruby", "shell", "fish", "perl", "r", "julia", "elixir", "powershell", "makefile",
                                 "cmake", "dockerfile", "yaml", "toml", "ini"};
    for (const char* h : hash) {
        if (lang == h) return "#";
    }
    if (lang == "sql" || lang == "lua" || lang == "haskell") return "--";
    if (lang == "batch") return "REM";
    if (lang == "html" || lang == "xml" || lang == "markdown" || lang == "json" || lang == "css" || lang == "text") return "";
    return "//";
}

std::string quoted(const std::string& path) { return path.find(' ') == std::string::npos ? path : "\"" + path + "\""; }

bool samePath(const std::string& a, const std::string& b) {
    if (a == b) return true;
    std::error_code ec;
    return !a.empty() && !b.empty() && std::filesystem::equivalent(tp::pathOf(a), tp::pathOf(b), ec);
}

}  // namespace

void Shell::folderChosen(const std::string& purpose, const std::string& path) {
    if (purpose == "code") {
        Result r = run("code.project", {{"folder", path}});
        panels_->code.treeStale = panels_->code.gitStale = true;
        if (!r.ok) panels_->code.status = friendlyError(r.message);
        openPanel("code");
    } else if (purpose == "explorer") {
        openPanel("explorer");
        explorerGo(path);
    } else if (purpose == "terminal") {
        openPanel("terminal");
        terminalNew(path);
    } else if (purpose.rfind("pipeline", 0) == 0) {
        pipelineChosen(purpose, path);
    }
}

void Shell::codeCommit() {
    CodeEditor& ed = panels_->code.editor;
    if (!ed.dirty || ed.doc.empty()) return;
    ed.dirty = false;
    Result r = run("code.set_text", {{"doc", ed.doc}, {"text", editorText(ed)}});
    if (!r.ok) panels_->code.status = friendlyError(r.message);
    ed.seen = changes_;
}

void Shell::codeSync() {
    CodePanel& c = panels_->code;
    CodeEditor& ed = c.editor;
    if (c.filesSeen != changes_) {
        c.files = tp::rows(query("code.files", {}));
        Result p = query("code.project", {});
        const std::string folder = p.ok ? tp::text(p, "folder") : std::string();
        if (folder != c.project) {
            c.project = folder;
            c.treeStale = c.gitStale = true;
        }
        c.filesSeen = changes_;
        c.gitStale = true;
    }
    // which file the editor shows: the current document when it is code
    std::string want = ed.doc;
    if (panelForDoc(currentDoc_) == "code") want = currentDoc_;
    bool known = false;
    for (const auto& f : c.files) known = known || f[0] == want;
    if (!known) want = c.files.empty() ? std::string() : c.files.back()[0];
    if (want != ed.doc) {
        codeCommit();
        ed = CodeEditor();
        ed.doc = want;
        c.selectTab = want;
        c.findInfo.clear();
        if (!want.empty()) currentDoc_ = want;
    }
    if (ed.doc.empty()) return;
    if (ed.seen != changes_ && !ed.dirty) {
        Result r = query("code.read", {{"doc", ed.doc}, {"raw", true}});
        if (r.ok) setEditorText(ed, tp::text(r, "text"));
        for (const auto& f : c.files) {
            if (f[0] == ed.doc && f.size() > 2) ed.language = f[2];
        }
        ed.comment = commentFor(ed.language);
        ed.seen = changes_;
    }
    const double now = tp::seconds();
    if (ed.spansStale && now - ed.lastHighlight > (ed.lines.size() > 4000 ? 1.0 : 0.15)) {
        ed.lastHighlight = now;
        Result h = query("code.highlight", {{"text", editorText(ed)}, {"language", ed.language.empty() ? std::string("text") : ed.language}});
        ed.spans.clear();
        for (double v : tp::numbers(h, "spans")) ed.spans.push_back(static_cast<int>(v));
        ed.spanStart.assign(ed.lines.size(), static_cast<int>(ed.spans.size()));
        for (std::size_t s = ed.spans.size(); s >= 4; s -= 4) {
            const int line = ed.spans[s - 4];
            if (line >= 0 && static_cast<std::size_t>(line) < ed.spanStart.size()) ed.spanStart[static_cast<std::size_t>(line)] = static_cast<int>(s - 4);
        }
        ed.spansStale = false;
    }
}

void Shell::codeOpenAt(const std::string& path, int line) {
    CodePanel& c = panels_->code;
    std::string id;
    for (const auto& f : c.files) {
        if (f.size() > 1 && samePath(f[1], path)) id = f[0];
    }
    if (id.empty()) {
        std::string full = path;
        if (!c.project.empty() && !tp::pathOf(path).is_absolute()) full = tp::joinPath(c.project, path);
        Result r = run("code.open", {{"path", full}});
        if (!r.ok) {
            c.status = friendlyError(r.message);
            return;
        }
        id = tp::text(r, "doc");
    }
    currentDoc_ = id;
    showHome_ = false;
    panel_ = "code";
    codeSync();
    if (line > 0) {
        editorSelect(c.editor, {line - 1, 0}, {line - 1, 0});
        c.editor.focusNext = true;
    }
}

void Shell::codeRun(const std::string& what) {
    CodePanel& c = panels_->code;
    codeCommit();
    Result r;
    if (what == "run") {
        if (c.editor.doc.empty()) {
            c.status = "Open a file to run first.";
            return;
        }
        r = run("code.run", {{"doc", c.editor.doc}, {"wait", false}});
    } else {
        r = run("code.build", {{"action", what}, {"wait", false}});
    }
    c.output.clear();
    c.problems.clear();
    c.bottom = 10;  // show the output tab
    if (!r.ok) {
        c.output = friendlyError(r.message);
        c.jobRunning = false;
        return;
    }
    c.job = tp::text(r, "job");
    c.output = tp::text(r, "output");
    c.jobRunning = tp::number(r, "running") != 0;
    c.status = r.message;
    c.lastPoll = 0;
}

void Shell::codePoll() {
    CodePanel& c = panels_->code;
    if (!c.jobRunning || c.job.empty()) return;
    const double now = tp::seconds();
    if (now - c.lastPoll < 0.2) return;
    c.lastPoll = now;
    Result r = query("code.job_output", {{"job", c.job}, {"full", true}});
    if (!r.ok) {
        c.jobRunning = false;
        return;
    }
    c.output = tp::text(r, "output");
    c.problems = tp::rows(r, "problems");
    c.jobRunning = tp::number(r, "running") != 0;
    if (!c.jobRunning) {
        const int code = static_cast<int>(tp::number(r, "exit_code"));
        const int errors = static_cast<int>(tp::number(r, "errors")), warnings = static_cast<int>(tp::number(r, "warnings"));
        c.status = code == 0 ? "Finished." : "Finished with exit code " + std::to_string(code) + ".";
        if (errors || warnings) c.status += " " + std::to_string(errors) + " error(s), " + std::to_string(warnings) + " warning(s).";
        if (errors) c.bottom = 11;  // show the problems tab
    }
}

void Shell::codeFind(int step) {
    CodePanel& c = panels_->code;
    CodeEditor& ed = c.editor;
    if (c.findText.empty() || ed.doc.empty()) {
        ed.found.clear();
        c.findInfo.clear();
        return;
    }
    codeCommit();
    Result r = query("code.find", {{"doc", ed.doc}, {"find", c.findText}, {"regex", c.regex}, {"match_case", c.matchCase}, {"whole_word", c.wholeWord}});
    ed.found.clear();
    if (!r.ok) {
        c.findInfo = friendlyError(r.message);
        return;
    }
    for (const auto& row : tp::rows(r)) {
        if (row.size() < 3) continue;
        ed.found.push_back({std::stoi(row[0]) - 1, std::stoi(row[1])});
        ed.foundLen = std::stoi(row[2]);
    }
    if (ed.found.empty()) {
        c.findInfo = "Not found";
        return;
    }
    if (step != 0) {
        // the next (or previous) match after the cursor
        const TextPos from = ed.cursor;
        int pick = step > 0 ? 0 : static_cast<int>(ed.found.size()) - 1;
        for (int i = 0; i < static_cast<int>(ed.found.size()); ++i) {
            const TextPos& f = ed.found[static_cast<std::size_t>(i)];
            if (step > 0 && from < TextPos{f.line, f.col + 1}) {
                pick = i;
                break;
            }
            if (step < 0 && f < ed.anchor) pick = i;
        }
        const TextPos& f = ed.found[static_cast<std::size_t>(pick)];
        editorSelect(ed, f, {f.line, f.col + ed.foundLen});
        c.findIndex = pick;
    }
    c.findInfo = std::to_string(c.findIndex + 1) + " of " + std::to_string(ed.found.size());
}

void Shell::codeToolbar() {
    CodePanel& c = panels_->code;
    if (ImGui::Button("Open folder...")) ImGui::OpenPopup("codefolder");
    ImGui::SetItemTooltip("Open a project folder: its files show on the left, and Build uses it");
    tp::flow("Open file...");
    if (ImGui::Button("Open file...")) showOpenDialog();
    tp::flow("New");
    if (ImGui::Button("New")) ImGui::OpenPopup("codenew");
    tp::flow("Save");
    ImGui::BeginDisabled(c.editor.doc.empty());
    if (ImGui::Button("Save")) {
        codeCommit();
        std::string path;
        for (const auto& f : c.files) {
            if (f[0] == c.editor.doc && f.size() > 1) path = f[1];
        }
        if (path.empty()) {
            ImGui::OpenPopup("codesaveas");
        } else {
            Result r = run("code.save", {{"doc", c.editor.doc}});
            c.status = r.ok ? sentence(r.message) : friendlyError(r.message);
        }
    }
    ImGui::SetItemTooltip("Save the file (Ctrl+S)");
    tp::flow("Run");
    if (accentButton("Run")) codeRun("run");
    ImGui::SetItemTooltip("Run this file with the compiler or interpreter installed for it (F5)");
    ImGui::EndDisabled();
    tp::flow("Build");
    ImGui::BeginDisabled(c.project.empty());
    if (ImGui::Button("Build")) codeRun("build");
    ImGui::SetItemTooltip("Build the project folder with its own build system (Ctrl+B)");
    if (pro_) {
        tp::flow("Test");
        if (ImGui::Button("Test")) codeRun("test");
        tp::flow("Clean");
        if (ImGui::Button("Clean")) codeRun("clean");
    }
    ImGui::EndDisabled();
    if (c.jobRunning) {
        tp::flow("Stop");
        if (ImGui::Button("Stop")) {
            run("code.job_stop", {{"job", c.job}});
            c.lastPoll = 0;
        }
    }
    tp::flow("Find");
    ImGui::BeginDisabled(c.editor.doc.empty());
    if (ImGui::Button("Find")) {
        c.findOpen = true;
        c.focusFind = true;
    }
    ImGui::SetItemTooltip("Find and replace in this file (Ctrl+F)");
    tp::flow("Go to line");
    if (ImGui::Button("Go to line")) {
        c.gotoOpen = true;
        c.gotoLine = c.editor.cursor.line + 1;
    }
    ImGui::SetItemTooltip("Jump to a line number (Ctrl+G)");
    tp::flow("View");
    if (ImGui::Button("View")) ImGui::OpenPopup("codeview");
    ImGui::SetItemTooltip("Wrap long lines, fold blocks");
    if (ImGui::BeginPopup("codeview")) {
        ImGui::Checkbox("Wrap long lines (Alt+Z)", &c.editor.wrap);
        if (ImGui::Checkbox("Language server", &c.lsp)) {
            c.lspProblems.clear();
            c.lspPoll = 0;
            if (!c.lsp) run("code.lsp", {{"action", std::string("stop")}});
        }
        ImGui::SetItemTooltip("Completion (Ctrl+Space), go to definition (F12 or Ctrl+click), notes when the mouse rests on a name, and problems as you "
                              "type, from the language server installed for the file's language (clangd, pyright, rust-analyzer, gopls, ...)");
        if (ImGui::Selectable("Fold every block")) editorFoldAll(c.editor, true);
        if (ImGui::Selectable("Open every block")) editorFoldAll(c.editor, false);
        ImGui::Separator();
        ImGui::TextDisabled("Ctrl+Shift+[ / ] folds or opens the block at the caret.");
        ImGui::TextDisabled("Alt+click adds a caret, Ctrl+D the next match, Ctrl+Alt+Up/Down a line.");
        ImGui::EndPopup();
    }
    ImGui::EndDisabled();
    if (pro_ && !c.editor.doc.empty()) {
        ImGui::SameLine();
        if (c.languages.empty()) c.languages = tp::rows(query("code.languages", {}));
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 8);
        std::string shownLang = c.editor.language;
        for (const auto& l : c.languages) {
            if (l.size() > 1 && l[0] == c.editor.language) shownLang = l[1];
        }
        if (ImGui::BeginCombo("##lang", shownLang.empty() ? "Plain text" : shownLang.c_str())) {
            for (const auto& l : c.languages) {
                if (l.size() > 1 && ImGui::Selectable(l[1].c_str(), l[0] == c.editor.language)) {
                    run("code.set_language", {{"doc", c.editor.doc}, {"language", l[0]}});
                    c.filesSeen = ~0ull;
                    c.editor.seen = ~0ull;
                }
            }
            ImGui::EndCombo();
        }
        tp::flow("Format");
        if (ImGui::Button("Format")) {
            codeCommit();
            Result r = run("code.format", {{"doc", c.editor.doc}});
            c.status = r.ok ? sentence(r.message) : friendlyError(r.message);
        }
        ImGui::SetItemTooltip("Tidy the file with the formatter installed for its language");
    }

    // popups
    if (ImGui::BeginPopup("codefolder")) {
        ImGui::TextUnformatted("Project folder");
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 22);
        if (c.folderInput.empty()) c.folderInput = c.project;
        bool go = ImGui::InputText("##folder", &c.folderInput, ImGuiInputTextFlags_EnterReturnsTrue);
        if (ImGui::Button("Browse...")) {
            askFolder("code");
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (accentButton("Open") || go) {
            folderChosen("code", c.folderInput);
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
    if (ImGui::BeginPopup("codenew")) {
        ImGui::TextUnformatted("File name (the ending picks the language)");
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 16);
        bool go = ImGui::InputText("##name", &c.newName, ImGuiInputTextFlags_EnterReturnsTrue);
        if (accentButton("Create") || go) {
            Result r = run("code.new", {{"name", c.newName}});
            if (!r.ok) c.status = friendlyError(r.message);
            c.saveAs = c.project.empty() ? std::string() : tp::joinPath(c.project, c.newName);
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
    if (ImGui::BeginPopup("codesaveas")) {
        ImGui::TextUnformatted("Save as");
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 22);
        bool go = ImGui::InputTextWithHint("##saveas", "full path of the new file", &c.saveAs, ImGuiInputTextFlags_EnterReturnsTrue);
        if ((accentButton("Save") || go) && !c.saveAs.empty()) {
            Result r = run("code.save", {{"doc", c.editor.doc}, {"path", c.saveAs}});
            c.status = r.ok ? sentence(r.message) : friendlyError(r.message);
            c.treeStale = true;
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
    if (c.gotoOpen) {
        ImGui::OpenPopup("codegoto");
        c.gotoOpen = false;
    }
    if (ImGui::BeginPopup("codegoto")) {
        ImGui::TextUnformatted("Go to line");
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 8);
        if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
        bool go = ImGui::InputInt("##line", &c.gotoLine, 1, 10, ImGuiInputTextFlags_EnterReturnsTrue);
        ImGui::SameLine();
        if (accentButton("Go") || go) {
            query("code.goto", {{"doc", c.editor.doc}, {"line", static_cast<std::int64_t>(std::max(1, c.gotoLine))}});
            editorSelect(c.editor, {c.gotoLine - 1, 0}, {c.gotoLine - 1, 0});
            c.editor.focusNext = true;
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
}

void Shell::codeTree() {
    CodePanel& c = panels_->code;
    if (c.project.empty()) {
        tp::muted("No folder open. Open a project folder to see its files here.");
        if (ImGui::Button("Open folder...")) askFolder("code");
        for (const auto& f : c.files) {
            if (f[0] == c.editor.doc && f.size() > 1 && !f[1].empty() && ImGui::Button("Open this file's folder")) folderChosen("code", tp::parentOf(f[1]));
        }
        return;
    }
    if (c.treeStale) {
        c.tree = tp::rows(query("code.tree", {{"depth", std::int64_t{8}}}));
        c.treeStale = false;
    }
    ImGui::TextDisabled("%s", tp::baseName(c.project).c_str());
    ImGui::SameLine();
    if (ImGui::SmallButton("Refresh")) c.treeStale = true;
    const float unit = ImGui::GetFontSize();
    int hideBelow = INT_MAX;
    for (std::size_t i = 0; i < c.tree.size(); ++i) {
        const auto& row = c.tree[i];
        if (row.size() < 3) continue;
        const int depth = std::stoi(row[0]);
        if (depth > hideBelow) continue;
        hideBelow = INT_MAX;
        const std::string& path = row[2];
        ImGui::PushID(static_cast<int>(i));
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + unit * 0.8f * static_cast<float>(depth));
        if (row[1] == "dir") {
            const bool open = c.openFolders.count(path) > 0;
            ImGui::SetNextItemOpen(open);
            const bool nowOpen = ImGui::TreeNodeEx(tp::baseName(path).c_str(), ImGuiTreeNodeFlags_NoTreePushOnOpen | ImGuiTreeNodeFlags_SpanAvailWidth);
            if (nowOpen != open) {
                if (nowOpen) c.openFolders.insert(path);
                else c.openFolders.erase(path);
            }
            if (!nowOpen) hideBelow = depth;
        } else {
            bool current = false;
            for (const auto& f : c.files) {
                if (f[0] == c.editor.doc && f.size() > 1 && f[1] == path) current = true;
            }
            if (ImGui::Selectable(tp::baseName(path).c_str(), current)) codeOpenAt(path, 0);
            ImGui::SetItemTooltip("%s", path.c_str());
        }
        ImGui::PopID();
    }
}

void Shell::codeOutline() {
    CodePanel& c = panels_->code;
    if (c.editor.doc.empty()) {
        tp::muted("Open a file to see its functions and classes.");
        return;
    }
    if (c.outlineFor != c.editor.doc || c.outlineSeen != c.editor.seen) {
        c.outline = tp::rows(query("code.outline", {{"doc", c.editor.doc}}));
        c.outlineFor = c.editor.doc;
        c.outlineSeen = c.editor.seen;
    }
    if (c.outline.empty()) tp::muted("Nothing to list in this file.");
    const float unit = ImGui::GetFontSize();
    for (std::size_t i = 0; i < c.outline.size(); ++i) {
        const auto& row = c.outline[i];
        if (row.size() < 4) continue;
        ImGui::PushID(static_cast<int>(i));
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + unit * 0.8f * static_cast<float>(std::stoi(row[2])));
        const int line = std::stoi(row[0]);
        if (ImGui::Selectable((row[3] + "##o").c_str(), c.editor.cursor.line + 1 == line)) {
            editorSelect(c.editor, {line - 1, 0}, {line - 1, 0});
            c.editor.focusNext = true;
        }
        ImGui::SameLine();
        ImGui::TextDisabled("%s %d", row[1].c_str(), line);
        ImGui::PopID();
    }
}

void Shell::codeSearch() {
    CodePanel& c = panels_->code;
    ImGui::SetNextItemWidth(-1);
    bool go = ImGui::InputTextWithHint("##search", "text to find in every file", &c.searchText, ImGuiInputTextFlags_EnterReturnsTrue);
    if (pro_) {
        ImGui::SetNextItemWidth(-1);
        ImGui::InputTextWithHint("##replace", "replace with (optional)", &c.searchReplace);
    }
    if (ImGui::Button("Search") || go) {
        codeCommit();
        Params p{{"find", c.searchText}, {"limit", std::int64_t{500}}};
        if (c.project.empty()) {
            for (const auto& f : c.files) {
                if (f[0] == c.editor.doc && f.size() > 1 && !f[1].empty()) p["folder"] = tp::parentOf(f[1]);
            }
        }
        Result r = query("code.search", p);
        c.searchHits = tp::rows(r);
        c.searchInfo = r.ok ? r.message.substr(0, r.message.find('\n')) : friendlyError(r.message);
    }
    if (pro_) {
        ImGui::SameLine();
        ImGui::BeginDisabled(c.searchText.empty());
        if (ImGui::Button("Replace in all files")) {
            codeCommit();
            Result r = run("code.replace_in_files", {{"find", c.searchText}, {"replace", c.searchReplace}});
            c.searchInfo = r.ok ? sentence(r.message.substr(0, r.message.find('\n'))) + " Undo puts them back." : friendlyError(r.message);
            c.searchHits.clear();
        }
        ImGui::EndDisabled();
    }
    tp::muted(c.searchInfo);
    for (std::size_t i = 0; i < c.searchHits.size(); ++i) {
        const auto& h = c.searchHits[i];
        if (h.size() < 5) continue;
        ImGui::PushID(static_cast<int>(i));
        std::string name = h[0];
        if (!c.project.empty() && name.rfind(c.project, 0) == 0) name = name.substr(std::min(name.size(), c.project.size() + 1));
        if (ImGui::Selectable((name + ":" + h[1]).c_str())) codeOpenAt(h[0], std::stoi(h[1]));
        std::string snippet = h[4];
        snippet.erase(0, snippet.find_first_not_of(" \t"));
        ImGui::SetItemTooltip("%s", snippet.c_str());
        ImGui::TextDisabled("  %s", snippet.substr(0, 60).c_str());
        ImGui::PopID();
    }
}

void Shell::codeGit() {
    CodePanel& c = panels_->code;
    if (c.project.empty()) {
        tp::muted("Open a project folder to see its changes.");
        return;
    }
    if (c.gitStale) {
        Result st = query("code.git_status", {});
        c.isRepo = st.ok;
        c.gitFiles = tp::rows(st);
        c.branch = tp::text(st, "branch");
        c.gitLog = c.isRepo ? tp::rows(query("code.git_log", {{"count", std::int64_t{15}}})) : tp::Rows{};
        c.branches.clear();
        if (c.isRepo && pro_) {
            Result b = query("code.git_branches", {});
            std::string text = b.message;
            for (const auto& row : tp::splitRows(text)) {
                std::string name = row[0];
                name.erase(0, name.find_first_not_of(" *"));
                if (!name.empty() && name.find(' ') == std::string::npos) c.branches.push_back(name);
            }
        }
        c.gitStale = false;
    }
    if (!c.isRepo) {
        tp::muted("This folder's changes are not tracked yet.");
        if (ImGui::Button("Start tracking changes (git init)")) {
            Result r = run("code.git_init", {});
            c.gitInfo = r.ok ? sentence(r.message) : friendlyError(r.message);
            c.gitStale = true;
        }
        tp::muted(c.gitInfo);
        return;
    }
    ImGui::Text("Branch: %s", c.branch.c_str());
    ImGui::SameLine();
    if (ImGui::SmallButton("Refresh")) c.gitStale = true;
    int staged = 0;
    for (const auto& f : c.gitFiles) {
        if (f.size() > 2 && f[0] != " " && f[0] != "?") ++staged;
    }
    if (c.gitFiles.empty()) tp::muted("Nothing changed since the last commit.");
    for (std::size_t i = 0; i < c.gitFiles.size(); ++i) {
        const auto& f = c.gitFiles[i];
        if (f.size() < 3) continue;
        ImGui::PushID(static_cast<int>(i));
        bool on = f[0] != " " && f[0] != "?";
        if (ImGui::Checkbox("##stage", &on)) {
            run(on ? "code.git_stage" : "code.git_unstage", {{"paths", quoted(f[2])}});
            c.gitStale = true;
        }
        ImGui::SetItemTooltip(on ? "In the next commit (click to take it out)" : "Click to put it in the next commit");
        ImGui::SameLine();
        const std::string state = f[0] == "?" ? "new" : (f[1] == "D" || f[0] == "D") ? "deleted" : (f[0] == "A") ? "added" : "changed";
        if (ImGui::Selectable((tp::baseName(f[2]) + "  (" + state + ")").c_str())) {
            Result d = query("code.git_diff", {{"file", f[2]}, {"staged", on}});
            c.output = d.ok ? tp::text(d, "diff") : friendlyError(d.message);
            if (c.output.empty()) c.output = d.message;
            c.bottom = 10;
        }
        ImGui::SetItemTooltip("%s - click to see what changed", f[2].c_str());
        ImGui::PopID();
    }
    ImGui::Spacing();
    ImGui::InputTextMultiline("##msg", &c.gitMessage, {-1, ImGui::GetFontSize() * 3});
    ImGui::BeginDisabled(c.gitMessage.empty() || c.gitFiles.empty());
    if (accentButton(staged ? "Commit" : "Commit all changes")) {
        Result r = run("code.git_commit", {{"message", c.gitMessage}, {"all", staged == 0}});
        c.gitInfo = r.ok ? sentence(r.message) : friendlyError(r.message);
        if (r.ok) c.gitMessage.clear();
        c.gitStale = true;
    }
    ImGui::EndDisabled();
    if (c.gitMessage.empty()) tp::muted("Write what you changed, then commit.");
    tp::muted(c.gitInfo);
    if (pro_ && !c.branches.empty()) {
        ImGui::SetNextItemWidth(-1);
        if (ImGui::BeginCombo("##branch", c.branch.c_str())) {
            for (const auto& b : c.branches) {
                if (ImGui::Selectable(b.c_str(), b == c.branch)) {
                    Result r = run("code.git_switch", {{"branch", b}});
                    c.gitInfo = r.ok ? sentence(r.message) : friendlyError(r.message);
                    c.gitStale = true;
                    c.filesSeen = ~0ull;
                    c.treeStale = true;
                }
            }
            ImGui::EndCombo();
        }
        ImGui::SetNextItemWidth(-1);
        ImGui::InputTextWithHint("##newbranch", "new branch name", &c.newBranch);
        if (ImGui::Button("Create branch") && !c.newBranch.empty()) {
            Result r = run("code.git_switch", {{"branch", c.newBranch}, {"create", true}});
            c.gitInfo = r.ok ? sentence(r.message) : friendlyError(r.message);
            c.newBranch.clear();
            c.gitStale = true;
        }
    }
    if (!c.gitLog.empty() && ImGui::CollapsingHeader("Recent commits")) {
        for (std::size_t i = 0; i < c.gitLog.size(); ++i) {
            const auto& l = c.gitLog[i];
            if (l.size() < 4) continue;
            ImGui::PushID(static_cast<int>(i));
            if (ImGui::Selectable(l[3].c_str())) {
                Result d = query("code.git_diff", {{"commit", l[0]}});
                c.output = d.ok ? tp::text(d, "diff") : friendlyError(d.message);
                c.bottom = 10;
            }
            ImGui::SetItemTooltip("%s  %s, %s", l[0].c_str(), l[1].c_str(), l[2].c_str());
            ImGui::PopID();
        }
    }
}

void Shell::codeSidebar() {
    CodePanel& c = panels_->code;
    // section buttons that wrap when the sidebar is narrow
    if (c.pickSide >= 0) {
        c.side = c.pickSide;
        c.pickSide = -1;
    }
    const char* names[] = {"Files", "Outline", "Find", pro_ ? "Git" : "Changes"};
    for (int i = 0; i < 4; ++i) {
        tp::flow(names[i], i == 0);
        const bool on = c.side == i;
        if (on) ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_Header));
        if (ImGui::Button(names[i])) c.side = i;
        if (on) ImGui::PopStyleColor();
    }
    ImGui::Separator();
    ImGui::BeginChild("sidebody");
    if (c.side == 0) codeTree();
    else if (c.side == 1) codeOutline();
    else if (c.side == 2) codeSearch();
    else codeGit();
    ImGui::EndChild();
}

void Shell::codeTabs() {
    CodePanel& c = panels_->code;
    if (!ImGui::BeginTabBar("codetabs", ImGuiTabBarFlags_FittingPolicyScroll | ImGuiTabBarFlags_Reorderable)) return;
    std::string close;
    for (const auto& f : c.files) {
        const std::string& id = f[0];
        std::string label = f.size() > 1 && !f[1].empty() ? tp::baseName(f[1]) : "untitled (" + id + ")";
        const bool modified = (f.size() > 3 && f[3] == "modified") || (id == c.editor.doc && c.editor.dirty);
        if (modified) label += " *";
        bool open = true;
        ImGuiTabItemFlags flags = c.selectTab == id ? ImGuiTabItemFlags_SetSelected : 0;
        if (modified) flags |= ImGuiTabItemFlags_UnsavedDocument;
        if (ImGui::BeginTabItem((label + "###" + id).c_str(), &open, flags)) {
            if (c.selectTab.empty() && id != c.editor.doc) currentDoc_ = id;
            ImGui::EndTabItem();
        }
        if (f.size() > 1) ImGui::SetItemTooltip("%s", f[1].c_str());
        if (!open) close = id;
    }
    c.selectTab.clear();
    ImGui::EndTabBar();
    if (!close.empty()) {
        if (close == c.editor.doc) codeCommit();
        Result r = run("code.close", {{"doc", close}});
        c.status = r.ok ? sentence(r.message) + " Undo opens it again." : friendlyError(r.message);
    }
}

void Shell::codeFindBar() {
    CodePanel& c = panels_->code;
    if (!c.findOpen) return;
    const float unit = ImGui::GetFontSize();
    ImGui::SetNextItemWidth(unit * 14);
    if (c.focusFind) {
        ImGui::SetKeyboardFocusHere();
        c.focusFind = false;
        const std::string sel = editorSelection(c.editor);
        if (!sel.empty() && sel.find('\n') == std::string::npos) c.findText = sel;
    }
    if (ImGui::InputTextWithHint("##find", "find", &c.findText, ImGuiInputTextFlags_EnterReturnsTrue)) {
        codeFind(ImGui::GetIO().KeyShift ? -1 : 1);
        ImGui::SetKeyboardFocusHere(-1);
    }
    ImGui::SameLine();
    if (ImGui::Button("<")) codeFind(-1);
    ImGui::SetItemTooltip("Previous match");
    ImGui::SameLine();
    if (ImGui::Button(">")) codeFind(1);
    ImGui::SetItemTooltip("Next match (Enter)");
    ImGui::SameLine();
    if (ImGui::Checkbox("Aa", &c.matchCase)) codeFind(0);
    ImGui::SetItemTooltip("Upper and lower case must match");
    ImGui::SameLine();
    if (ImGui::Checkbox("Word", &c.wholeWord)) codeFind(0);
    ImGui::SetItemTooltip("Whole words only");
    if (pro_) {
        ImGui::SameLine();
        if (ImGui::Checkbox(".*", &c.regex)) codeFind(0);
        ImGui::SetItemTooltip("Regular expression");
    }
    ImGui::SameLine();
    ImGui::TextDisabled("%s", c.findInfo.c_str());
    ImGui::SetNextItemWidth(unit * 14);
    ImGui::InputTextWithHint("##replacewith", "replace with", &c.replaceText);
    ImGui::SameLine();
    if (ImGui::Button("Replace")) {
        // the selected match is swapped like typing over it, then on to the next one
        const std::string sel = editorSelection(c.editor);
        if (!sel.empty() && !c.editor.found.empty()) {
            const TextPos start = c.editor.anchor < c.editor.cursor ? c.editor.anchor : c.editor.cursor;
            codeCommit();
            Result r = run("code.replace", {{"doc", c.editor.doc}, {"find", c.findText}, {"replace", c.replaceText}, {"regex", c.regex},
                                            {"match_case", c.matchCase}, {"whole_word", c.wholeWord}, {"line", std::int64_t{start.line + 1}}});
            if (!r.ok) c.findInfo = friendlyError(r.message);
            codeSync();
            editorSelect(c.editor, start, start);
        }
        codeFind(1);
    }
    ImGui::SetItemTooltip("Replace the matches on this line, then find the next one");
    ImGui::SameLine();
    if (ImGui::Button("Replace all")) {
        codeCommit();
        Result r = run("code.replace", {{"doc", c.editor.doc}, {"find", c.findText}, {"replace", c.replaceText}, {"regex", c.regex},
                                        {"match_case", c.matchCase}, {"whole_word", c.wholeWord}});
        c.findInfo = r.ok ? sentence(r.message) : friendlyError(r.message);
        c.editor.found.clear();
    }
    ImGui::SameLine();
    if (ImGui::Button("Close")) {
        c.findOpen = false;
        c.editor.found.clear();
        c.editor.focusNext = true;
    }
}

void Shell::codeOutput() {
    CodePanel& c = panels_->code;
    const int want = c.bottom >= 10 ? c.bottom - 10 : -1;  // 10 + tab: a switch asked for
    if (c.bottom >= 10) c.bottom -= 10;
    if (ImGui::BeginTabBar("codebottom")) {
        if (ImGui::BeginTabItem("Output", nullptr, want == 0 ? ImGuiTabItemFlags_SetSelected : 0)) {
            c.bottom = 0;
            ImGui::EndTabItem();
        }
        const std::size_t live = c.lsp && c.lspProblemsFor == c.editor.doc ? c.lspProblems.size() : 0;
        const std::string problems = "Problems (" + std::to_string(c.problems.size() + live) + ")###problems";
        if (ImGui::BeginTabItem(problems.c_str(), nullptr, want == 1 ? ImGuiTabItemFlags_SetSelected : 0)) {
            c.bottom = 1;
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }
    ImGui::SameLine();
    ImGui::TextDisabled("%s", c.jobRunning ? "running..." : c.status.c_str());
    if (c.bottom == 0) {
        const float inputH = c.jobRunning ? ImGui::GetFrameHeightWithSpacing() : 0.0f;
        ImGui::BeginChild("out", {0, -inputH}, ImGuiChildFlags_Borders, ImGuiWindowFlags_HorizontalScrollbar);
        ImGui::PushFont(monoFont, 0.0f);
        if (c.output.empty()) ImGui::TextDisabled("Run or build to see the output here.");
        else ImGui::TextUnformatted(c.output.c_str(), c.output.c_str() + c.output.size());
        ImGui::PopFont();
        if (c.jobRunning) ImGui::SetScrollHereY(1.0f);
        ImGui::EndChild();
        if (c.jobRunning) {
            ImGui::SetNextItemWidth(-ImGui::GetFontSize() * 4);
            if (ImGui::InputTextWithHint("##jobin", "type an answer for the program and press Enter", &c.jobInput, ImGuiInputTextFlags_EnterReturnsTrue)) {
                run("code.job_input", {{"job", c.job}, {"text", c.jobInput}});
                c.jobInput.clear();
                ImGui::SetKeyboardFocusHere(-1);
            }
            ImGui::SameLine();
            if (ImGui::Button("Stop##job")) run("code.job_stop", {{"job", c.job}});
        }
        return;
    }
    ImGui::BeginChild("problems", {0, 0}, ImGuiChildFlags_Borders);
    const bool live = c.lsp && c.lspProblemsFor == c.editor.doc;
    if (c.problems.empty() && (!live || c.lspProblems.empty())) ImGui::TextDisabled("No errors or warnings.");
    // the language server's, for the file in the editor, as it is now
    for (std::size_t i = 0; live && i < c.lspProblems.size(); ++i) {
        const auto& p = c.lspProblems[i];
        if (p.size() < 4) continue;
        ImGui::PushID(static_cast<int>(100000 + i));
        const bool error = p[2] == "error";
        ImGui::PushStyleColor(ImGuiCol_Text, error ? ImVec4{0.95f, 0.45f, 0.42f, 1} : ImVec4{0.92f, 0.72f, 0.25f, 1});
        ImGui::TextUnformatted(p[2].c_str());
        ImGui::PopStyleColor();
        ImGui::SameLine();
        if (ImGui::Selectable(("line " + p[0] + ":" + p[1] + "  " + p[3]).c_str())) {
            const int line = std::stoi(p[0]) - 1, col = std::stoi(p[1]) - 1;
            editorSelect(c.editor, {line, col}, {line, col});
            c.editor.focusNext = true;
        }
        ImGui::PopID();
    }
    for (std::size_t i = 0; i < c.problems.size(); ++i) {
        const auto& p = c.problems[i];
        if (p.size() < 5) continue;
        ImGui::PushID(static_cast<int>(i));
        const bool error = p[3] == "error";
        ImGui::PushStyleColor(ImGuiCol_Text, error ? ImVec4{0.95f, 0.45f, 0.42f, 1} : ImVec4{0.92f, 0.72f, 0.25f, 1});
        ImGui::TextUnformatted(error ? "error" : p[3].c_str());
        ImGui::PopStyleColor();
        ImGui::SameLine();
        const std::string where = tp::baseName(p[0]) + ":" + p[1] + (p[2] != "0" ? ":" + p[2] : "");
        if (ImGui::Selectable((where + "  " + p[4]).c_str())) codeOpenAt(p[0], std::stoi(p[1]));
        ImGui::SetItemTooltip("%s", p[0].c_str());
        ImGui::PopID();
    }
    ImGui::EndChild();
}

void Shell::codePanel() {
    CodePanel& c = panels_->code;
    codeSync();
    CodeEditor& ed = c.editor;
    // problem marks for the file in the editor
    ed.marks.clear();
    std::string edPath;
    for (const auto& f : c.files) {
        if (f[0] == ed.doc && f.size() > 1) edPath = f[1];
    }
    for (const auto& p : c.problems) {
        if (p.size() > 3 && !edPath.empty() && (samePath(p[0], edPath) || tp::baseName(p[0]) == tp::baseName(edPath))) {
            ed.marks.emplace_back(std::stoi(p[1]) - 1, p[3] == "error");
        }
    }
    if (c.lsp && c.lspProblemsFor == ed.doc) {
        for (const auto& p : c.lspProblems) {
            if (p.size() > 2 && (p[2] == "error" || p[2] == "warning")) ed.marks.emplace_back(std::stoi(p[0]) - 1, p[2] == "error");
        }
    }
    // shortcuts
    if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_F) && !ed.doc.empty()) {
        c.findOpen = true;
        c.focusFind = true;
    }
    if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_G) && !ed.doc.empty()) {
        c.gotoOpen = true;
        c.gotoLine = ed.cursor.line + 1;
    }
    if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_S) && !ed.doc.empty()) {
        codeCommit();
        Result r = run("code.save", {{"doc", ed.doc}});
        c.status = r.ok ? sentence(r.message) : friendlyError(r.message);
    }
    if (ImGui::IsKeyPressed(ImGuiKey_F5, false) && !ed.doc.empty()) codeRun("run");
    if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_B) && !c.project.empty()) codeRun("build");
    if (ImGui::IsKeyPressed(ImGuiKey_F3, false)) codeFind(ImGui::GetIO().KeyShift ? -1 : 1);

    codeToolbar();
    const float unit = ImGui::GetFontSize();
    const float sideW = std::clamp(ImGui::GetContentRegionAvail().x * 0.26f, unit * 11, unit * 17);
    ImGui::BeginChild("codeside", {sideW, 0}, ImGuiChildFlags_Borders);
    codeSidebar();
    ImGui::EndChild();
    ImGui::SameLine();
    ImGui::BeginChild("codemain", {0, 0}, ImGuiChildFlags_None);
    const float outputH = std::max(unit * 6, ImGui::GetContentRegionAvail().y * 0.28f);
    if (ed.doc.empty()) {
        ImGui::BeginChild("welcome", {0, -outputH}, ImGuiChildFlags_Borders);
        tp::title("Write and run code", 1.5f);
        tp::muted("Open a file or a whole project folder, or start a new file. Run uses the compilers and interpreters already on "
                  "this computer; nothing is downloaded.");
        if (accentButton("Open folder...", {unit * 10, unit * 2.2f})) askFolder("code");
        ImGui::SameLine();
        if (ImGui::Button("Open file...", {unit * 10, unit * 2.2f})) showOpenDialog();
        if (pro_) {
            if (c.toolchains.empty()) c.toolchains = tp::rows(query("code.toolchains", {}));
            ImGui::Spacing();
            ImGui::TextUnformatted("Installed toolchains");
            for (const auto& t : c.toolchains) {
                if (t.size() > 1) ImGui::BulletText("%s  (%s)", t[0].c_str(), t[1].c_str());
            }
        }
        ImGui::EndChild();
    } else {
        codeTabs();
        codeFindBar();
        ed.menuOpen = c.compOpen;
        if (drawEditor(ed, monoFont, dark_, {0, -outputH})) c.outlineSeen = ~0ull;
        codeLanguageServer();
        if (ed.wantUndo) {
            ed.wantUndo = false;
            undo();
        }
        if (ed.wantRedo) {
            ed.wantRedo = false;
            redo();
        }
    }
    std::string lspNote;
    if (!ed.doc.empty() && c.lsp && c.lspProblemsFor == ed.doc) lspNote = "   " + std::to_string(c.lspProblems.size()) + " problem(s)";
    if (!ed.doc.empty() && !c.lsp && c.lspHintFor == ed.language && !c.lspHint.empty()) lspNote = "   " + c.lspHint;
    ImGui::TextDisabled("%s", ed.doc.empty() ? "" : ("Line " + std::to_string(ed.cursor.line + 1) + ", column " + std::to_string(ed.cursor.col + 1) +
                                                     (ed.extra.empty() ? "" : "   " + std::to_string(ed.extra.size() + 1) + " carets") +
                                                     (ed.language.empty() ? "" : "   " + ed.language) + lspNote + (c.project.empty() ? "" : "   " + c.project)).c_str());
    codeOutput();
    ImGui::EndChild();
}

void Shell::codeLanguageServer() {
    CodePanel& c = panels_->code;
    CodeEditor& ed = c.editor;
    const double now = tp::seconds();
    std::string edPath;
    for (const auto& f : c.files) {
        if (f[0] == ed.doc && f.size() > 1) edPath = f[1];
    }
    // off: say once per language when a server is there to turn on
    if (!c.lsp) {
        if (c.lspHintFor != ed.language) {
            c.lspHintFor = ed.language;
            c.lspHint.clear();
            for (const auto& r : tp::rows(query("code.lsp", {}))) {
                if (r.size() > 2 && r[0] == ed.language && r[2] != "not installed") c.lspHint = r[1] + " is installed: View > Language server";
            }
        }
        c.compOpen = false;
        ed.wantComplete = ed.wantDefinition = false;
        return;
    }
    if (edPath.empty()) {
        if (ed.wantComplete || ed.wantDefinition) c.status = "Save the file first: the language server reads it from disk.";
        ed.wantComplete = ed.wantDefinition = false;
        c.compOpen = false;
        return;
    }
    auto spot = [&](TextPos p) {
        return Params{{"doc", ed.doc}, {"line", static_cast<std::int64_t>(p.line + 1)}, {"col", static_cast<std::int64_t>(p.col + 1)}};
    };
    // live problems, a moment after typing stops
    if (now - ed.lastEdit > 0.8 && now - c.lspPoll > 1.5) {
        c.lspPoll = now;
        codeCommit();
        Params q{{"doc", ed.doc}, {"wait", 0.0}};
        Result r = query("code.diagnostics", q);
        if (r.ok) {
            c.lspProblems = tp::rows(r, "rows");
            c.lspProblemsFor = ed.doc;
            c.lspWaiting = tp::text(r, "fresh") != "true";
        } else {
            c.status = friendlyError(r.message);
            c.lsp = false;  // no server for this file: off, with the reason in the status line
        }
    }
    // keep drawing (and so asking) while the server is still looking, or typing just stopped
    if (c.lsp && (c.lspWaiting || now - ed.lastEdit < 2.5)) animating_ = true;
    // completion list
    auto refilter = [&]() {
        const std::string& line = ed.lines[static_cast<std::size_t>(ed.cursor.line)];
        if (ed.cursor.line != c.compLine || ed.cursor.col < c.compFrom) {
            c.compOpen = false;
            return;
        }
        std::string typed = line.substr(static_cast<std::size_t>(c.compFrom), static_cast<std::size_t>(ed.cursor.col - c.compFrom));
        for (auto& ch : typed) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
        c.comps.clear();
        for (const auto& it : c.compsAll) {
            std::string l = it[0];
            for (auto& ch : l) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
            if (l.rfind(typed, 0) == 0) c.comps.push_back(it);
        }
        c.compSel = std::clamp(c.compSel, 0, std::max(0, static_cast<int>(c.comps.size()) - 1));
        if (c.comps.empty()) c.compOpen = false;
    };
    if (ed.wantComplete) {
        ed.wantComplete = false;
        codeCommit();
        Result r = query("code.complete", spot(ed.cursor));
        if (r.ok) {
            c.compsAll = tp::rows(r, "rows");
            c.compFrom = static_cast<int>(tp::number(r, "from_col")) - 1;
            c.compLine = ed.cursor.line;
            c.compSel = 0;
            c.compOpen = !c.compsAll.empty();
            refilter();
            if (c.compsAll.empty()) c.status = "Nothing to complete here.";
        } else {
            c.status = friendlyError(r.message);
        }
    }
    if (c.compOpen) refilter();
    if (c.compOpen) {
        auto accept = [&](int i) {
            const auto& it = c.comps[static_cast<std::size_t>(i)];
            editorReplaceAtCaret(ed, c.compFrom, it.size() > 3 && !it[3].empty() ? it[3] : it[0]);
            c.compOpen = false;
            ed.focusNext = true;
        };
        if (ImGui::IsKeyPressed(ImGuiKey_DownArrow)) c.compSel = (c.compSel + 1) % static_cast<int>(c.comps.size());
        if (ImGui::IsKeyPressed(ImGuiKey_UpArrow)) c.compSel = (c.compSel + static_cast<int>(c.comps.size()) - 1) % static_cast<int>(c.comps.size());
        if (ImGui::IsKeyPressed(ImGuiKey_Escape, false)) c.compOpen = false;
        if (c.compOpen && (ImGui::IsKeyPressed(ImGuiKey_Enter, false) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter, false) || ImGui::IsKeyPressed(ImGuiKey_Tab, false))) {
            accept(c.compSel);
        }
        if (c.compOpen) {
            const float unit = ImGui::GetFontSize();
            ImGui::SetNextWindowPos({ed.caretScreen.x, ed.caretScreen.y + ed.lineHeight});
            ImGui::SetNextWindowSizeConstraints({unit * 22, 0}, {unit * 36, unit * 16});
            ImGui::Begin("##complete", nullptr, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_AlwaysAutoResize |
                                                    ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoSavedSettings |
                                                    ImGuiWindowFlags_Tooltip);
            for (int i = 0; i < static_cast<int>(c.comps.size()) && i < 60; ++i) {
                const auto& it = c.comps[static_cast<std::size_t>(i)];
                ImGui::PushID(i);
                if (ImGui::Selectable(it[0].c_str(), i == c.compSel)) accept(i);
                if (i == c.compSel) ImGui::SetScrollHereY();
                if (it.size() > 2 && !(it[1].empty() && it[2].empty())) {
                    ImGui::SameLine(unit * 12);
                    ImGui::TextDisabled("%s", (it[1] + (it[2].empty() ? "" : "  " + it[2])).substr(0, 60).c_str());
                }
                ImGui::PopID();
                if (!c.compOpen) break;
            }
            ImGui::End();
        }
    }
    // go to definition
    if (ed.wantDefinition) {
        ed.wantDefinition = false;
        codeCommit();
        Result r = query("code.definition", spot(ed.cursor));
        if (!r.ok) {
            c.status = friendlyError(r.message);
        } else if (tp::number(r, "count") < 1) {
            c.status = "No definition found there.";
        } else {
            const int line = static_cast<int>(tp::number(r, "line")), col = static_cast<int>(tp::number(r, "col"));
            codeOpenAt(tp::text(r, "path"), line);
            editorSelect(c.editor, {line - 1, col - 1}, {line - 1, col - 1});
            c.editor.focusNext = true;
        }
    }
    // a note when the mouse rests on a name
    if (!ed.mouseOnText || ed.mousePos != c.hoverAt) {
        c.hoverAt = ed.mouseOnText ? ed.mousePos : TextPos{-1, -1};
        c.hoverSince = now;
        c.hoverAsked = false;
        c.hoverText.clear();
    } else if (!c.hoverAsked && now - c.hoverSince <= 0.6) {
        animating_ = true;  // keep drawing so the note can come up while the mouse rests
    } else if (!c.hoverAsked && !c.compOpen) {
        c.hoverAsked = true;
        codeCommit();
        Result r = query("code.hover", spot(c.hoverAt));
        c.hoverText = r.ok && tp::text(r, "found") == "true" ? tp::text(r, "text") : std::string();
    }
    if (!c.hoverText.empty() && ed.mouseOnText) {
        if (now - c.hoverSince < 1.5) animating_ = true;  // a tooltip takes a frame or two to size itself
        ImGui::BeginTooltip();
        ImGui::PushTextWrapPos(ImGui::GetFontSize() * 32);
        ImGui::TextUnformatted(c.hoverText.substr(0, 1500).c_str());
        ImGui::PopTextWrapPos();
        ImGui::EndTooltip();
    }
}

}  // namespace wetwaresuite::gui
