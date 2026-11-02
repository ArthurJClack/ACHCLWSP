// Shared pieces of the code.* commands: the open-file document, the project
// folder, background jobs (builds and runs) and small helpers.
#pragma once

#include <chrono>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "wetwaresuite/code/code.hpp"
#include "wetwaresuite/core/process.hpp"
#include "wetwaresuite/sysutil/sysutil.hpp"

namespace wetwaresuite::code {

// An open file. The text always uses "\n"; the file's own line ends and byte
// order mark come back when it is saved.
class CodeDoc : public Document {
public:
    std::string text;
    std::string language = "text";
    std::string eol = "\n";
    bool bom = false;
    std::string saved;  // the text as last opened or saved
    int cursorLine = 1;  // where code.goto last went (1-based), for the GUI

    std::string kind() const override { return "code"; }
    std::string summary() const override;
    bool modified() const { return text != saved; }
};

// a build or run in the background; pump() moves it along
struct Job {
    std::string id;
    std::string description;
    std::vector<Step> steps;
    std::size_t next = 0;  // the step to start next
    std::unique_ptr<ChildProcess> proc;
    std::string output;
    std::size_t dropped = 0;  // bytes cut from the front of a long output
    int exitCode = -1;
    bool done = false;
    bool stopped = false;
    fs::path cwd;
    std::chrono::steady_clock::time_point started;
    double seconds = 0;
};

struct CodeState {
    fs::path project;  // the open folder ("" = none)
    std::map<std::string, std::shared_ptr<Job>> jobs;
    int jobCounter = 0;
};
using StatePtr = std::shared_ptr<CodeState>;

namespace detail {

std::string trim(const std::string& s);
std::vector<std::string> lines(const std::string& text);  // without line ends
std::string join(const std::vector<std::string>& lines);
std::string absText(const fs::path& p);
fs::path folderParam(const CodeState& st, const Params& p, const std::string& key = "folder");
ParamSpec docParam();
ParamSpec opt(const std::string& name, ParamType type, const std::string& what, Value def = {});
Result guard(const std::function<Result()>& fn);

std::shared_ptr<CodeDoc> needDoc(Session& s, const std::string& id);
// a read-only command on an open file
ActionFactory docReader(Session& s, std::function<Result(CodeDoc&, const Params&)> fn);
// an undoable change to an open file's text
ActionFactory docEditor(Session& s, std::function<Result(CodeDoc&, const Params&)> fn);
ActionFactory reader(std::function<Result(const Params&)> fn);

// files under a folder, skipping .git, node_modules, build output and hidden folders
std::vector<fs::path> projectFiles(const fs::path& root, std::size_t limit = 20000);
bool looksBinary(const std::string& bytes);
// text loaded from disk: "\r\n" turned into "\n", BOM removed
void loadText(CodeDoc& doc, const std::string& bytes);
std::string saveText(const CodeDoc& doc);

// jobs
std::shared_ptr<Job> startJob(CodeState& st, Plan plan, std::string& error);
void pump(Job& job);
std::shared_ptr<Job> findJob(CodeState& st, const std::string& id);  // "" = the newest
Result jobResult(Job& job, bool fullOutput);
std::string problemRows(const std::vector<Problem>& problems);

void registerEditCommands(Session& s, StatePtr st);
void registerProjectCommands(Session& s, StatePtr st);
void registerRunCommands(Session& s, StatePtr st);
void registerGitCommands(Session& s, StatePtr st);
void registerLspCommands(Session& s, StatePtr st);

}  // namespace detail
}  // namespace wetwaresuite::code
