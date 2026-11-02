// Live previews, as in Photoshop's adjustment dialogs or Onshape's feature dialogs: while a form
// is open its settings run on the file as they change, so the canvas shows the result before
// Apply. The run is a trial: it's undone when the settings change again, when anything else is
// done, when the form closes or the file changes, and Apply just keeps it. Dragging an arrow in
// the 3D view uses the same trials. Everything still goes through the form's own command.
#include <algorithm>
#include <chrono>

#include "shell.hpp"

namespace wetwaresuite::gui {

namespace {

double nowSeconds() { return static_cast<double>(SDL_GetTicks()) / 1000.0; }

bool docParamOf(const CommandInfo& info) {
    return std::any_of(info.params.begin(), info.params.end(), [](const ParamSpec& p) { return p.name == "doc"; });
}

}  // namespace

std::string Shell::trialKey(const std::string& command, const Params& params) {
    std::string k = command;
    for (const auto& [name, v] : params) k += "\n" + name + "=" + toText(v);
    return k;
}

bool Shell::previewable(const std::string& command) const {
    const CommandInfo* info = find(command);
    if (!info || !info->undoable || info->readOnly || !docParamOf(*info)) return false;
    // things that make, open, keep or send files, and sketches (which open for drawing)
    const std::string verb = command.substr(command.find('.') + 1);
    for (const char* no : {"new", "open", "save", "save_as", "export", "close", "import", "load", "render", "send", "sketch"})
        if (verb == no) return false;
    if (command.rfind("app.", 0) == 0) return false;
    return true;
}

bool Shell::tryRun(const std::string& command, const Params& params) {
    dropTrial();
    auto doc = params.find("doc");
    const std::string docId = doc != params.end() ? toText(doc->second) : currentDoc_;
    const bool wasUnsaved = unsaved_.count(docId) > 0;
    trial_.running = true;
    Result r = run(command, params);
    trial_.running = false;
    const std::size_t depth = session_.history().labels().size();
    const Value* made = r.field("doc");
    if (!r.ok || (made && toText(*made) != docId)) {  // failed, or made another file: not a preview
        if (r.ok) session_.history().undo();
        syncDocs();
        live_.note = r.ok ? "This one makes a new file, so it can't preview." : friendlyError(r.message);
        return false;
    }
    live_.note.clear();
    trial_.applied = true;
    trial_.command = command;
    trial_.doc = docId;
    trial_.key = trialKey(command, params);
    trial_.depth = depth;
    trial_.wasUnsaved = wasUnsaved;
    r.message = sentence(r.message);
    trial_.result = std::move(r);
    return true;
}

void Shell::dropTrial() {
    if (!trial_.applied) return;
    trial_.applied = false;
    // only when it is still the newest step: anything done after it stays
    if (session_.history().labels().size() == trial_.depth && session_.history().undo()) {
        ++changes_;
        if (!trial_.wasUnsaved) unsaved_.erase(trial_.doc);
        canvas_.orbitDirty = true;
    }
    trial_.key.clear();
    syncDocs();
}

bool Shell::keepTrial(const std::string& command, const Params& params) {
    if (!trial_.applied || trial_.key != trialKey(command, params) || session_.history().labels().size() != trial_.depth) {
        dropTrial();
        return false;
    }
    trial_.applied = false;
    return true;
}

void Shell::previewTick() {
    if (trial_.applied && (view3d_.dragging >= 0 || brush3d_.stroking || xform_.active || imgText_.open) && (!form_ || form_->command != trial_.command)) return;  // an arrow's or a brush's own trial
    const bool formFits = form_ && previewable(form_->command) && trial_.applied ? form_->command == trial_.command && currentDoc_ == trial_.doc : true;
    if (trial_.applied && (!form_ || !formFits || !live_.on)) dropTrial();
    if (!form_ || !previewable(form_->command) || (!live_.on && !live_.runNow)) {
        live_.pendingKey.clear();
        return;
    }
    Params params;
    std::string error;
    if (!toParams(*form_, params, error)) {
        dropTrial();  // a required box is empty: show the file as it is
        live_.pendingKey.clear();
        return;
    }
    // only on the file being looked at
    auto doc = params.find("doc");
    if (doc == params.end() || toText(doc->second) != currentDoc_ || !session_.workspace().get(currentDoc_)) return;
    const std::string key = trialKey(form_->command, params);
    if (trial_.applied && key == trial_.key) return;
    const bool slow = live_.slow.count(form_->command) > 0;
    if (!live_.runNow) {
        if (slow) return;
        if (key != live_.pendingKey) {  // wait for the settings to rest a moment
            live_.pendingKey = key;
            live_.changedAt = nowSeconds();
            return;
        }
        if (nowSeconds() - live_.changedAt < 0.25) return;
    }
    live_.runNow = false;
    const ViewCamera before = view3d_.cam;
    const bool keep = canvas_.is3d && view3d_.camOk && view3d_.doc == currentDoc_;
    const auto t0 = std::chrono::steady_clock::now();
    const bool ok = tryRun(form_->command, params);
    const double took = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    if (took > 1.2) live_.slow.insert(form_->command);  // too slow to redo on every change: the Preview button does it
    if (!ok) {
        trial_.key = key;  // don't try the same settings again
        live_.pendingKey = key;
    }
    if (ok && keep) keepView(before);
}

void Shell::previewControls() {
    if (!form_ || !previewable(form_->command)) return;
    ImGui::Checkbox("Live preview", &live_.on);
    ImGui::SetItemTooltip("Show the result on the file while you change the settings; Apply keeps it");
    const bool slow = live_.slow.count(form_->command) > 0;
    if (live_.on && slow) {
        ImGui::SameLine();
        if (ImGui::SmallButton("Preview")) live_.runNow = true;
        ImGui::SetItemTooltip("This one takes a while, so it only previews when you ask");
    }
    const char* note = trial_.applied && trial_.command == form_->command ? "Showing the result: Apply keeps it, Ctrl+Z cancels."
                       : live_.on && !live_.note.empty()                       ? live_.note.c_str()
                                                                               : nullptr;
    if (note) {
        ImGui::PushTextWrapPos(0);
        ImGui::TextDisabled("%s", note);
        ImGui::PopTextWrapPos();
    }
    ImGui::Spacing();
}

}  // namespace wetwaresuite::gui
