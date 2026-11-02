// Helpers shared by the code.* commands, background jobs and registerCommands.
#include <algorithm>

#include "internal.hpp"

namespace wetwaresuite::code {

std::string CodeDoc::summary() const {
    const Language* lang = languageById(language);
    std::size_t n = static_cast<std::size_t>(std::count(text.begin(), text.end(), '\n')) + 1;
    return (lang ? lang->name : language) + ", " + sysutil::plural(n, "line") + (eol == "\r\n" ? ", CRLF" : "") +
           (modified() ? ", modified" : "") + (path.empty() ? ", not saved yet" : ", " + path);
}

namespace detail {

std::string trim(const std::string& s) { return sysutil::trim(s); }

std::vector<std::string> lines(const std::string& text) { return sysutil::splitLines(text); }

std::string join(const std::vector<std::string>& ls) {
    std::string out;
    for (std::size_t i = 0; i < ls.size(); ++i) {
        if (i) out += '\n';
        out += ls[i];
    }
    return out;
}

std::string absText(const fs::path& p) {
    std::error_code ec;
    fs::path a = fs::absolute(p, ec);
    return sysutil::fromPath(ec ? p : a.lexically_normal());
}

fs::path folderParam(const CodeState& st, const Params& p, const std::string& key) {
    std::string f = stringParam(p, key);
    if (!f.empty()) return sysutil::toPath(f);
    if (!st.project.empty()) return st.project;
    std::error_code ec;
    return fs::current_path(ec);
}

ParamSpec docParam() { return {"doc", ParamType::String, "id of an open file, e.g. code1", true}; }

ParamSpec opt(const std::string& name, ParamType type, const std::string& what, Value def) {
    return {name, type, what, false, {}, std::move(def)};
}

Result guard(const std::function<Result()>& fn) {
    try {
        return fn();
    } catch (const std::exception& e) {
        return Result::failure(e.what());
    }
}

std::shared_ptr<CodeDoc> needDoc(Session& s, const std::string& id) {
    auto doc = s.workspace().getAs<CodeDoc>(id);
    if (!doc) throw std::runtime_error("no open file called '" + id + "' (code.open gives one)");
    return doc;
}

ActionFactory docReader(Session& s, std::function<Result(CodeDoc&, const Params&)> fn) {
    return [&s, fn](const Params& p) -> std::unique_ptr<Action> {
        return std::make_unique<FunctionAction>([&s, fn, p] { return guard([&] { return fn(*needDoc(s, stringParam(p, "doc")), p); }); });
    };
}

namespace {

// keeps the text from before, so undo puts it back
class TextChange : public Action {
public:
    TextChange(Session& s, Params p, std::function<Result(CodeDoc&, const Params&)> fn) : s_(s), p_(std::move(p)), fn_(std::move(fn)) {}
    Result apply() override {
        return guard([&] {
            doc_ = needDoc(s_, stringParam(p_, "doc"));
            before_ = doc_->text;
            Result r = guard([&] { return fn_(*doc_, p_); });
            if (!r.ok) doc_->text = before_;
            return r;
        });
    }
    void revert() override {
        if (doc_) doc_->text = before_;
    }

private:
    Session& s_;
    Params p_;
    std::function<Result(CodeDoc&, const Params&)> fn_;
    std::shared_ptr<CodeDoc> doc_;
    std::string before_;
};

}  // namespace

ActionFactory docEditor(Session& s, std::function<Result(CodeDoc&, const Params&)> fn) {
    return [&s, fn](const Params& p) -> std::unique_ptr<Action> { return std::make_unique<TextChange>(s, p, fn); };
}

ActionFactory reader(std::function<Result(const Params&)> fn) {
    return [fn](const Params& p) -> std::unique_ptr<Action> {
        return std::make_unique<FunctionAction>([fn, p] { return guard([&] { return fn(p); }); });
    };
}

std::vector<fs::path> projectFiles(const fs::path& root, std::size_t limit) {
    static const std::set<std::string> skip = {".git", ".hg", ".svn", "node_modules", "target", "build", "dist", "out",
                                               "__pycache__", ".venv", "venv", ".idea", ".vs", ".vscode", "bin", "obj",
                                               ".cache", ".gradle", ".next", "vendor", "_deps", "CMakeFiles"};
    std::vector<fs::path> out;
    std::error_code ec;
    fs::recursive_directory_iterator it(root, fs::directory_options::skip_permission_denied, ec), end;
    for (; it != end && out.size() < limit; it.increment(ec)) {
        if (ec) break;
        const std::string name = sysutil::nameOf(it->path());
        if (it->is_directory(ec)) {
            if (skip.count(name) || (name.size() > 1 && name[0] == '.')) it.disable_recursion_pending();
            continue;
        }
        if (it->is_regular_file(ec)) out.push_back(it->path());
    }
    std::sort(out.begin(), out.end());
    return out;
}

bool looksBinary(const std::string& bytes) {
    const std::size_t n = std::min<std::size_t>(bytes.size(), 8000);
    for (std::size_t i = 0; i < n; ++i) {
        if (bytes[i] == '\0') return true;
    }
    return false;
}

void loadText(CodeDoc& doc, const std::string& bytes) {
    std::string t = bytes;
    doc.bom = t.size() >= 3 && t.compare(0, 3, "\xEF\xBB\xBF") == 0;
    if (doc.bom) t.erase(0, 3);
    doc.eol = t.find("\r\n") != std::string::npos ? "\r\n" : "\n";
    std::string clean;
    clean.reserve(t.size());
    for (std::size_t i = 0; i < t.size(); ++i) {
        if (t[i] == '\r' && i + 1 < t.size() && t[i + 1] == '\n') continue;
        clean += t[i];
    }
    doc.text = std::move(clean);
    doc.saved = doc.text;
}

std::string saveText(const CodeDoc& doc) {
    std::string out = doc.bom ? "\xEF\xBB\xBF" : "";
    out.reserve(doc.text.size() + doc.text.size() / 20 + 3);
    for (char c : doc.text) {
        if (c == '\n') out += doc.eol;
        else out += c;
    }
    return out;
}

// ---- jobs ------------------------------------------------------------------------------------

namespace {

constexpr std::size_t kMaxOutput = 1 << 20;

void append(Job& job, const std::string& text) {
    job.output += text;
    if (job.output.size() > kMaxOutput) {
        std::size_t cut = job.output.size() - kMaxOutput;
        job.output.erase(0, cut);
        job.dropped += cut;
    }
}

std::string commandLine(const std::vector<std::string>& args) {
    std::string out;
    for (const auto& a : args) {
        bool quote = a.empty() || a.find_first_of(" \t\"") != std::string::npos;
        out += (out.empty() ? "" : " ") + (quote ? "\"" + a + "\"" : a);
    }
    return out;
}

bool startStep(Job& job) {
    const Step& step = job.steps[job.next++];
    append(job, "$ " + commandLine(step.args) + "\n");
    job.proc = std::make_unique<ChildProcess>();
    std::string error;
    if (!job.proc->start(step.args, step.cwd.empty() ? job.cwd : step.cwd, error, {{"TERM", "dumb"}, {"PAGER", "cat"}, {"GIT_PAGER", "cat"}})) {
        append(job, error + "\n");
        job.exitCode = -1;
        job.done = true;
        return false;
    }
    if (step.hasInput) {
        job.proc->write(step.input);
        job.proc->closeInput();
    }
    return true;
}

}  // namespace

std::shared_ptr<Job> startJob(CodeState& st, Plan plan, std::string& error) {
    auto job = std::make_shared<Job>();
    job->id = "job" + std::to_string(++st.jobCounter);
    job->description = plan.description;
    job->steps = std::move(plan.steps);
    job->cwd = job->steps.empty() ? fs::path() : job->steps.front().cwd;
    job->started = std::chrono::steady_clock::now();
    if (job->steps.empty()) {
        error = "nothing to run";
        return nullptr;
    }
    startStep(*job);
    st.jobs[job->id] = job;
    // keep the last 20 jobs
    while (st.jobs.size() > 20) {
        auto oldest = st.jobs.end();
        for (auto it = st.jobs.begin(); it != st.jobs.end(); ++it) {
            if (it->second->done && (oldest == st.jobs.end() || it->second->started < oldest->second->started)) oldest = it;
        }
        if (oldest == st.jobs.end()) break;
        st.jobs.erase(oldest);
    }
    return job;
}

void pump(Job& job) {
    while (!job.done && job.proc) {
        append(job, job.proc->read());
        if (job.proc->running()) return;
        append(job, job.proc->read());
        job.exitCode = job.proc->exitCode();
        if (job.stopped || job.exitCode != 0 || job.next >= job.steps.size()) {
            job.done = true;
            job.seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - job.started).count();
            append(job, job.stopped ? "[stopped]\n" : "[exit code " + std::to_string(job.exitCode) + "]\n");
            return;
        }
        if (!startStep(job)) return;
    }
}

std::shared_ptr<Job> findJob(CodeState& st, const std::string& id) {
    if (id.empty()) {
        std::shared_ptr<Job> newest;
        for (auto& [k, j] : st.jobs) {
            if (!newest || j->started > newest->started) newest = j;
        }
        if (!newest) throw std::runtime_error("nothing has been run yet");
        return newest;
    }
    auto it = st.jobs.find(id);
    if (it == st.jobs.end()) throw std::runtime_error("no job called '" + id + "' (code.jobs lists them)");
    return it->second;
}

std::string problemRows(const std::vector<Problem>& problems) {
    std::string rows;
    for (const auto& pr : problems) {
        rows += pr.file + "\t" + std::to_string(pr.line) + "\t" + std::to_string(pr.col) + "\t" + pr.severity + "\t" + pr.message + "\n";
    }
    return rows;
}

Result jobResult(Job& job, bool fullOutput) {
    pump(job);
    auto problems = parseProblems(job.output, job.cwd);
    std::size_t errors = 0, warnings = 0;
    for (const auto& pr : problems) {
        if (pr.severity == "error") ++errors;
        else if (pr.severity == "warning") ++warnings;
    }
    std::string head = job.id + ": " + job.description;
    if (!job.done) head += " (still running)";
    else if (job.stopped) head += " (stopped)";
    else head += job.exitCode == 0 ? " (finished OK" : " (failed, exit code " + std::to_string(job.exitCode);
    if (job.done && !job.stopped) {
        char buf[32];
        std::snprintf(buf, sizeof buf, ", %.1f s)", job.seconds);
        head += buf;
    }
    if (errors || warnings) head += ", " + sysutil::plural(errors, "error") + ", " + sysutil::plural(warnings, "warning");
    std::string out = job.output;
    if (!fullOutput && out.size() > 12000) out = "...\n" + out.substr(out.size() - 12000);
    Result r = Result::success(head + "\n" + out);
    r.with("job", job.id)
        .with("running", !job.done)
        .with("exit_code", static_cast<std::int64_t>(job.exitCode))
        .with("output", out)
        .with("errors", static_cast<std::int64_t>(errors))
        .with("warnings", static_cast<std::int64_t>(warnings))
        .with("problems", problemRows(problems));
    return r;
}

}  // namespace detail

void registerCommands(Session& session) {
    auto st = std::make_shared<CodeState>();
    detail::registerEditCommands(session, st);
    detail::registerProjectCommands(session, st);
    detail::registerRunCommands(session, st);
    detail::registerGitCommands(session, st);
    detail::registerLspCommands(session, st);
}

}  // namespace wetwaresuite::code
