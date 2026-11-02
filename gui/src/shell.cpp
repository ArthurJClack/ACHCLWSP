// Shell state and the actions every panel shares. Everything that changes a
// document goes through Session::run, so the window, scripts and MCP clients
// all see the same undo history.
#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <cctype>
#include <mutex>
#include <fstream>
#include <set>
#include <sstream>

#include "shell.hpp"
#include "tool_panels.hpp"

namespace wetwaresuite::gui {

Uint32 Shell::wakeEvent = 0;

void Texture::reset() {
    if (tex) SDL_DestroyTexture(tex);
    tex = nullptr;
    width = height = srcWidth = srcHeight = 0;
}

bool Texture::upload(SDL_Renderer* renderer, const std::uint8_t* rgba, int w, int h) {
    srcWidth = w;
    srcHeight = h;
    // big pictures are shown at a lower resolution than the renderer's limit; edits still use full size
    int maxSide = static_cast<int>(SDL_GetNumberProperty(SDL_GetRendererProperties(renderer),
                                                         SDL_PROP_RENDERER_MAX_TEXTURE_SIZE_NUMBER, 4096));
    if (maxSide <= 0) maxSide = 4096;
    int step = 1;
    while ((w + step - 1) / step > maxSide || (h + step - 1) / step > maxSide) ++step;
    const int tw = (w + step - 1) / step, th = (h + step - 1) / step;
    std::vector<std::uint8_t> scaled;
    const std::uint8_t* pixels = rgba;
    if (step > 1) {
        scaled.resize(static_cast<std::size_t>(tw) * th * 4);
        for (int y = 0; y < th; ++y) {
            for (int x = 0; x < tw; ++x) {
                const std::uint8_t* s = rgba + (static_cast<std::size_t>(y * step) * w + x * step) * 4;
                std::copy(s, s + 4, scaled.begin() + (static_cast<std::ptrdiff_t>(y) * tw + x) * 4);
            }
        }
        pixels = scaled.data();
    }
    if (!tex || width != tw || height != th) {
        if (tex) SDL_DestroyTexture(tex);
        tex = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_RGBA32, SDL_TEXTUREACCESS_STATIC, tw, th);
        if (!tex) return false;
        SDL_SetTextureBlendMode(tex, SDL_BLENDMODE_BLEND);
        width = tw;
        height = th;
    }
    return SDL_UpdateTexture(tex, nullptr, pixels, tw * 4);
}

Shell::Shell(Session& session, const AppInfo& app, SDL_Window* window, SDL_Renderer* renderer)
    : session_(session), app_(app), window_(window), renderer_(renderer) {
    const auto all = session_.registry().list();
    tools_ = app_.tools;
    if (tools_.empty()) {
        std::set<std::string> seen;
        for (const auto& c : all) {
            std::string prefix = c.id.substr(0, c.id.find('.'));
            if (seen.insert(prefix).second) tools_.push_back(prefix);
        }
    }
    for (const auto& c : all) {
        std::string prefix = c.id.substr(0, c.id.find('.'));
        if (std::find(tools_.begin(), tools_.end(), prefix) != tools_.end()) commands_.push_back(c);
    }
    // a tool this build doesn't carry gets no empty card
    tools_.erase(std::remove_if(tools_.begin(), tools_.end(), [this](const std::string& t) { return toolCommands(t, commands_).empty(); }),
                 tools_.end());
    handoff_ = hasHandoff(session_);
    if (handoff_) {
        self_ = fieldText(session_.run("app.installed", {}), "self");
        refreshInbox();
    }
    if (const auto* paintStroke = find("paint.stroke")) {
        for (const auto& p : paintStroke->params) {
            if (p.name == "brush" && !p.choices.empty()) {
                if (std::find(p.choices.begin(), p.choices.end(), canvas_.brush) == p.choices.end()) canvas_.brush = p.choices.front();
            }
        }
    }
    loadRecent();
    panels_ = std::make_unique<ToolPanels>();
    hasClock_ = find("clock.due") != nullptr;
    loadPalette();
    layout_ = appLayout(app_.id);
    ribbonTabs_ = ribbonTabs(app_.id, tools_, commands_);
}

Shell::~Shell() {
    if (panels_) panels_->resetTextures();
    canvas_.texture.reset();
}

const CommandInfo* Shell::find(const std::string& id) const {
    for (const auto& c : commands_) {
        if (c.id == id) return &c;
    }
    return nullptr;
}

std::vector<std::string> Shell::docIds() const { return session_.workspace().ids(); }

void Shell::syncDocs() {
    const auto ids = docIds();
    if (!currentDoc_.empty() && std::find(ids.begin(), ids.end(), currentDoc_) == ids.end()) {
        currentDoc_ = ids.empty() ? std::string() : ids.back();
        if (currentDoc_.empty()) showHome_ = true;
    }
    canvas_.dirty = true;
}

void Shell::noteChange(const std::string& id, const Params& params, const Result& r) {
    std::string doc;
    if (auto it = params.find("doc"); it != params.end()) {
        if (auto s = std::get_if<std::string>(&it->second)) doc = *s;
    }
    if (doc.empty()) {
        if (const Value* v = r.field("doc")) {
            if (auto s = std::get_if<std::string>(v)) doc = *s;
        }
    }
    if (doc.empty()) return;
    const std::string verb = commandVerb(id);
    if (verb == "save" || verb == "save_as" || verb == "new" || verb == "open" || verb == "load" || verb == "import") {
        unsaved_.erase(doc);
        if (closeAfterSave_ == doc && (verb == "save" || verb == "save_as")) {
            closeAfterSave_.clear();
            closeDocNow(doc);
        }
        return;
    }
    const CommandInfo* info = find(id);
    if (verb.empty() && info && !info->readOnly && session_.workspace().get(doc)) unsaved_.insert(doc);
}

Result Shell::run(const std::string& id, const Params& params) {
    if (trial_.applied && !trial_.running) dropTrial();  // doing something else: the preview goes
    Result r = session_.run(id, params);
    if (r.ok) noteChange(id, params, r);
    if (r.ok) {  // names of things just made, so the next form can offer them
        for (const auto& [key, v] : r.data) {
            auto s = std::get_if<std::string>(&v);
            if (!s || s->empty() || !std::isdigit(static_cast<unsigned char>(s->back())) || session_.workspace().get(*s)) continue;
            madeNames_.push_back(*s);
        }
        if (madeNames_.size() > 64) madeNames_.erase(madeNames_.begin(), madeNames_.end() - 64);
    }
    if (r.ok && id == "cad.sketch") {  // a new sketch opens for drawing, as in Onshape
        if (auto name = std::get_if<std::string>(&r.value); name && !name->empty()) {
            auto it = params.find("doc");
            if (it != params.end())
                if (auto doc = std::get_if<std::string>(&it->second)) enterSketch(*doc, *name);
        }
    }
    if (r.ok) {
        const CommandInfo* info = find(id);
        if (!info || !info->readOnly) ++changes_;
        // a command that made or picked a document: show it
        // most tools answer with doc=; audio projects with project=, songs with song=
        const Value* doc = r.field("doc");
        if (!doc) doc = r.field("project");
        if (!doc) doc = r.field("song");
        if (doc) {
            if (auto s = std::get_if<std::string>(doc); s && session_.workspace().get(*s)) {
                if (*s != currentDoc_) canvas_.fit = true;
                currentDoc_ = *s;
                if (!docTool_.count(*s)) docTool_[*s] = id.substr(0, id.find('.'));
                // what made it decides its ribbon tab and its save and close commands
                const std::string verb = id.substr(id.find('.') + 1);
                const bool opener = verb == "open" || verb == "new" || verb == "load" || openerStem(id) != "";
                if (opener || !docOpener_.count(*s)) docOpener_[*s] = id;
                showHome_ = false;
                panel_ = panelForDoc(*s);  // code and terminals open in their panel
            }
        }
        auto path = params.find("path");
        if (path != params.end() && id.size() > 5 && (id.compare(id.size() - 5, 5, ".open") == 0 || id.compare(id.size() - 5, 5, "_open") == 0)) {
            if (auto p = std::get_if<std::string>(&path->second)) {
                recent_ = pushRecent(recent_, *p);
                saveRecent();
            }
        }
    }
    syncDocs();
    return r;
}

void Shell::select(const std::string& commandId) {
    const CommandInfo* info = find(commandId);
    if (!info) return;
    form_ = buildForm(*info, docIds(), currentDoc_);
    prefillMadeNames(*form_, madeNames_);
    fillPicked(false);
    fillAudioRange(false);
    result_.reset();
    pickProps_ = true;
    showProps_ = true;
}

void Shell::fillPicked(bool replace) {
    if (!form_ || canvas_.vecSel.empty()) return;
    for (auto& f : form_->fields) {
        if (f.spec.name != "shape" && f.spec.name != "shapes") continue;
        if (replace || f.text.empty()) setFieldText(f, canvas_.vecSel);
    }
}

void Shell::fillAudioRange(bool replace) {
    if (!form_ || !audioSel_.any() || audioSel_.doc != currentDoc_) return;
    if (form_->command.rfind("audio.", 0) != 0) return;
    char a[32], b[32];
    std::snprintf(a, sizeof a, "%.3f", audioSel_.a);
    std::snprintf(b, sizeof b, "%.3f", audioSel_.b);
    for (auto& f : form_->fields) {
        if (f.spec.name == "start" && (replace || !f.set || f.text.empty() || f.text == "0")) setFieldText(f, a);
        if (f.spec.name == "end" && (replace || !f.set || f.text.empty() || f.text == "0")) setFieldText(f, b);
    }
}

void Shell::runForm() {
    if (!form_) return;
    Params params;
    std::string error;
    resultFor_ = form_->title;
    if (!toParams(*form_, params, error)) {
        result_ = Result::failure(error);
        return;
    }
    // long jobs (exports) run in the background so the window keeps working, with a progress bar
    const CommandInfo* info = find(form_->command);
    const bool background = info && std::any_of(info->params.begin(), info->params.end(), [](const ParamSpec& p) {
        return p.name == "background" && p.type == ParamType::Bool;
    });
    if (background) params["background"] = true;
    // a live preview already shows exactly this: keep it instead of doing it again, and close the
    // form like a dialog's OK (so it doesn't preview a second one)
    const bool kept = keepTrial(form_->command, params), previewing = live_.on && previewable(form_->command);
    Result r = kept ? trial_.result : run(form_->command, params);
    if (background && r.ok) {
        if (const Value* job = r.field("job")) {
            exportJob_ = ExportJob{form_->command.substr(0, form_->command.find('.')), toText(*job), {}, 0, 0};
            if (const Value* out = r.field("output")) exportJob_.output = toText(*out);
        }
    }
    r.message = r.ok ? sentence(r.message) : friendlyError(r.message);
    const bool done = r.ok && (kept || previewing || oneShotCommand(form_->command) || form_->command == "cad.sketch");  // a sketch opens for drawing
    result_ = std::move(r);
    scrollToResult_ = true;
    if (done) {  // like a New or Save dialog's OK: the panel goes back to the file
        form_.reset();
        return;
    }
    // documents may have changed: refresh document pickers to what's open now
    const auto ids = docIds();
    for (auto& f : form_->fields) {
        if (f.kind == FieldKind::Document && (f.text.empty() || std::find(ids.begin(), ids.end(), f.text) == ids.end())) {
            f.text = currentDoc_;
            f.set = !f.text.empty();
        }
    }
}

void Shell::pollExport() {
    ExportJob& job = exportJob_;
    if (job.id.empty()) return;
    const std::uint64_t now = SDL_GetTicks();
    if (now - job.polledAt < 150) return;
    job.polledAt = now;
    Result r = session_.run(job.tool + ".export_status", {{"job", job.id}});
    const Value* done = r.field("done");
    if (r.ok && !(done && std::get_if<bool>(done) && *std::get_if<bool>(done))) {
        if (const Value* p = r.field("progress"); p && std::get_if<double>(p)) job.progress = *std::get_if<double>(p);
        return;
    }
    // finished (or gone): its result replaces the "exporting..." message
    r.message = r.ok ? sentence(r.message) : friendlyError(r.message);
    resultFor_ = "Export";
    result_ = std::move(r);
    scrollToResult_ = true;
    job = ExportJob{};
}

void Shell::exportProgress() {
    ExportJob& job = exportJob_;
    if (job.id.empty()) return;
    const std::string name = job.output.empty() ? std::string("the video") : std::filesystem::path(job.output).filename().string();
    char overlay[64];
    std::snprintf(overlay, sizeof overlay, "%d%%", static_cast<int>(job.progress * 100));
    ImGui::TextUnformatted(("Exporting " + name).c_str());
    ImGui::SameLine();
    const float cancelW = ImGui::CalcTextSize("Cancel").x + ImGui::GetStyle().FramePadding.x * 2;
    ImGui::ProgressBar(static_cast<float>(job.progress), {std::max(60.0f, ImGui::GetContentRegionAvail().x - cancelW - ImGui::GetStyle().ItemSpacing.x), 0},
                       overlay);
    ImGui::SameLine();
    if (ImGui::Button("Cancel##export")) session_.run(job.tool + ".export_cancel", {{"job", job.id}});
    ImGui::SetItemTooltip("Stop the export (the half-made file is removed)");
}

void Shell::undo() {
    if (trial_.applied) {  // Ctrl+Z during a live preview cancels it
        dropTrial();
        form_.reset();
        result_ = Result::success("Preview cancelled.");
        resultFor_ = "Undo";
        return;
    }
    beforeHistoryStep();
    ++changes_;
    if (session_.history().undo()) {
        if (!currentDoc_.empty()) unsaved_.insert(currentDoc_);
        result_ = Result::success("Undone.");
        resultFor_ = "Undo";
    }
    syncDocs();
}

void Shell::redo() {
    dropTrial();
    beforeHistoryStep();
    ++changes_;
    if (session_.history().redo()) {
        if (!currentDoc_.empty()) unsaved_.insert(currentDoc_);
        result_ = Result::success("Redone.");
        resultFor_ = "Redo";
    }
    syncDocs();
}

void Shell::openFile(const std::string& path) {
    if (!app_.launcher.empty()) return launcherOpen(path);  // the launcher hands every file to its own app
    if (openFolder(path)) return;  // a folder opens in the file manager (or as a code project)
    auto candidates = openCandidates(path, tools_, commands_);
    resultFor_ = "Open a file";
    // the suite's own file table knows best (notes.md is a document, not slides)
    if (handoff_ && session_.registry().find("app.open_with")) {
        const Result route = session_.run("app.open_with", {{"path", path}});
        const std::string command = fieldText(route, "command");
        if (fieldText(route, "app") == self_ && !command.empty()) {
            auto it = std::find(candidates.begin(), candidates.end(), command);
            if (it != candidates.end()) std::rotate(candidates.begin(), it, it + 1);
        }
    }
    if (candidates.empty() && find("toolkit.inspect")) {
        // the files app: say what the file is and get it ready to convert
        Result r = run("toolkit.inspect", {{"path", path}});
        if (r.ok && find("convert.file")) {
            select("convert.file");
            for (auto& f : form_->fields)
                if (f.spec.name == "input") setFieldText(f, path);
        }
        r.message = r.ok ? sentence(r.message) : friendlyError(r.message);
        result_ = std::move(r);
        resultFor_ = "Open a file";
        suggestOtherApp(path, true);
        return;
    }
    if (candidates.empty()) {
        result_ = Result::failure("This app can't open files. Try \"New\" instead.");
        suggestOtherApp(path, false);
        if (suggestion_.show) result_ = Result::failure(suggestion_.text);
        return;
    }
    std::string firstError;
    for (const auto& id : candidates) {
        Result r = run(id, {{"path", path}});
        if (r.ok) {
            r.message = sentence(r.message);
            result_ = std::move(r);
            suggestOtherApp(path, true);
            return;
        }
        if (firstError.empty()) firstError = r.message;
        // a locked PDF: ask for its password and try again
        const CommandInfo* info = find(id);
        if (r.message.find("password-protected") != std::string::npos && info &&
            std::any_of(info->params.begin(), info->params.end(), [](const ParamSpec& ps) { return ps.name == "password"; })) {
            passwordAsk_ = {true, path, id, {}, {}};
            return;
        }
    }
    result_ = Result::failure("Couldn't open " + path + ": " + friendlyError(firstError));
    suggestOtherApp(path, false);
    if (suggestion_.show) result_ = Result::failure(suggestion_.text);  // the banner says what to do instead
}

// ---- native file dialogs --------------------------------------------------------

namespace {

// Answers outlive the window on purpose: a dialog may still be open when it closes.
struct DialogQueue {
    std::mutex mutex;
    std::vector<std::pair<DialogTarget, std::string>> results;
    std::string error;
};

DialogQueue& dialogQueue() {
    static DialogQueue q;
    return q;
}

struct DialogRequest {
    DialogTarget target;
};

// SDL may call this on another thread: only queue the answer and wake the loop.
void SDLCALL onDialog(void* userdata, const char* const* files, int) {
    auto* req = static_cast<DialogRequest*>(userdata);
    {
        auto& q = dialogQueue();
        std::lock_guard<std::mutex> lock(q.mutex);
        if (!files) q.error = std::string("Couldn't show a file window (") + SDL_GetError() + "). Type the path into the box instead.";
        else if (req->target.kind == DialogTarget::AddMedia)
            for (int i = 0; files[i]; ++i) q.results.emplace_back(req->target, files[i]);
        else if (files[0]) q.results.emplace_back(req->target, files[0]);
    }
    SDL_Event wake{};
    wake.type = Shell::wakeEvent;
    SDL_PushEvent(&wake);
    delete req;
}

}  // namespace

void Shell::showOpenDialog() {
    auto* req = new DialogRequest{{DialogTarget::OpenDocument, {}, 0}};
    SDL_ShowOpenFileDialog(onDialog, req, window_, nullptr, 0, nullptr, false);
}

void Shell::askSavePath(const std::string& command, const std::string& doc, const std::string& start) {
    auto* req = new DialogRequest{{DialogTarget::SaveDocument, command, 0, doc}};
    SDL_ShowSaveFileDialog(onDialog, req, window_, nullptr, 0, start.empty() ? nullptr : start.c_str());
}

void Shell::showAddMediaDialog() {
    auto* req = new DialogRequest{{DialogTarget::AddMedia, currentDoc_, 0}};
    static const SDL_DialogFileFilter filters[] = {
        {"Video, pictures and sound", "mp4;mov;mkv;webm;avi;m4v;png;jpg;jpeg;webp;gif;bmp;wav;mp3;flac;ogg;m4a;aac;opus;srt;vtt"},
        {"All files", "*"}};
    SDL_ShowOpenFileDialog(onDialog, req, window_, filters, 2, nullptr, true);
}

void Shell::dropFile(const std::string& path) {
    // onto the timeline that's open, when it takes the file (videos, stills, sound, captions)
    auto doc = session_.workspace().get(currentDoc_);
    TimelineLayout layout;
    if (!showHome_ && doc && doc->timeline(layout) && layout.tools) {
        TimelineEdit e;
        e.kind = TimelineEditKind::AddFile;
        e.name = path;
        e.track = timeline_.doc == currentDoc_ ? timeline_.selTrack : std::string();
        if (!doc->timelineCommand(e).empty()) {
            runTimelineEdit(layout, e);
            return;
        }
    }
    openFile(path);
}

void Shell::askFolder(const std::string& purpose) {
    auto* req = new DialogRequest{{DialogTarget::PanelFolder, purpose, 0}};
    SDL_ShowOpenFolderDialog(onDialog, req, window_, nullptr, false);
}

void Shell::showFieldDialog(std::size_t index) {
    if (!form_ || index >= form_->fields.size()) return;
    auto* req = new DialogRequest{{DialogTarget::FormField, form_->command, index}};
    const Field& f = form_->fields[index];
    const char* start = f.text.empty() ? nullptr : f.text.c_str();
    if (f.kind == FieldKind::SaveFile) SDL_ShowSaveFileDialog(onDialog, req, window_, nullptr, 0, start);
    else if (f.kind == FieldKind::Folder) SDL_ShowOpenFolderDialog(onDialog, req, window_, start, false);
    else SDL_ShowOpenFileDialog(onDialog, req, window_, nullptr, 0, start, false);
}

void Shell::drainDialogs() {
    std::vector<std::pair<DialogTarget, std::string>> results;
    std::string error;
    {
        auto& q = dialogQueue();
        std::lock_guard<std::mutex> lock(q.mutex);
        results.swap(q.results);
        error.swap(q.error);
    }
    if (!error.empty()) {
        result_ = Result::failure(error);
        resultFor_ = "File window";
    }
    for (const auto& [target, path] : results) {
        if (target.kind == DialogTarget::OpenDocument) {
            openFile(path);
        } else if (target.kind == DialogTarget::SaveDocument) {
            saveDocTo(target.command, target.doc, path);
        } else if (target.kind == DialogTarget::AddMedia) {
            if (target.command == currentDoc_) dropFile(path);
        } else if (target.kind == DialogTarget::PanelFolder) {
            folderChosen(target.command, path);
        } else if (form_ && form_->command == target.command && target.field < form_->fields.size()) {
            setFieldText(form_->fields[target.field], path);
        }
    }
}

// ---- recent files ------------------------------------------------------------------

void Shell::loadRecent() {
    std::string safe;
    for (char c : app_.name) safe += std::isalnum(static_cast<unsigned char>(c)) ? c : '-';
    char* pref = SDL_GetPrefPath("wetwaresuite", safe.empty() ? "gui" : safe.c_str());
    if (!pref) return;
    recentFile_ = std::string(pref) + "recent.txt";
    SDL_free(pref);
    std::ifstream in(recentFile_, std::ios::binary);
    std::stringstream buf;
    buf << in.rdbuf();
    recent_ = parseRecent(buf.str());
}

void Shell::saveRecent() {
    if (recentFile_.empty()) return;
    std::ofstream out(recentFile_, std::ios::binary | std::ios::trunc);
    out << formatRecent(recent_);
}

}  // namespace wetwaresuite::gui
