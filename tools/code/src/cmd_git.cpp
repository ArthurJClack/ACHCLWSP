// code.git_* commands: status, diff, stage, commit, log, branches, merge, and fetch/pull/push
// through the git program when it is installed. Only fetch, pull and push talk to a server, and
// they never stop to ask for a password: git uses the credentials it already has (a credential
// helper, an ssh key) or fails with a message saying so.
#include "internal.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>

namespace wetwaresuite::code::detail {

namespace {

using std::int64_t;

ProcessResult git(const fs::path& dir, std::vector<std::string> args) {
    if (!findProgram("git")) throw std::runtime_error("git is not installed (get it from git-scm.com or your package manager)");
    args.insert(args.begin(), {"git", "-C", sysutil::fromPath(dir), "-c", "color.ui=never", "-c", "core.quotepath=off"});
    return runProgram(args);
}

// For commands that reach a server: no terminal prompts (a window or an MCP client has no
// terminal to answer them in), ssh in batch mode.
ProcessResult gitRemote(const fs::path& dir, std::vector<std::string> args) {
    static const bool once = [] {
#ifdef _WIN32
        _putenv_s("GIT_TERMINAL_PROMPT", "0");
        if (!std::getenv("GIT_SSH_COMMAND")) _putenv_s("GIT_SSH_COMMAND", "ssh -o BatchMode=yes");
#else
        setenv("GIT_TERMINAL_PROMPT", "0", 1);
        setenv("GIT_SSH_COMMAND", "ssh -o BatchMode=yes", 0);
#endif
        return true;
    }();
    (void)once;
    ProcessResult r = git(dir, std::move(args));
    if (r.exitCode != 0) {
        const std::string out = sysutil::trim(r.output);
        if (out.find("terminal prompts disabled") != std::string::npos || out.find("could not read Username") != std::string::npos ||
            out.find("Permission denied (publickey") != std::string::npos || out.find("Authentication failed") != std::string::npos)
            throw std::runtime_error("the server wants a login git doesn't have yet. Sign in once from a terminal (or set up a credential helper or an ssh key), then try again.\n" + out);
        if (out.find("Could not resolve host") != std::string::npos || out.find("unable to access") != std::string::npos)
            throw std::runtime_error("couldn't reach the server (offline?)\n" + out);
    }
    return r;
}

std::vector<std::string> conflictedFiles(const fs::path& dir) {
    std::vector<std::string> out;
    for (const auto& l : lines(git(dir, {"diff", "--name-only", "--diff-filter=U"}).output)) {
        if (!sysutil::trim(l).empty()) out.push_back(sysutil::trim(l));
    }
    return out;
}

std::string currentBranch(const fs::path& dir) { return sysutil::trim(git(dir, {"rev-parse", "--abbrev-ref", "HEAD"}).output); }

// a merge or pull that stopped on conflicts: which files, and what to do next
Result conflictResult(const fs::path& dir, const std::string& what) {
    const auto files = conflictedFiles(dir);
    std::string text = what + " stopped: " + std::to_string(files.size()) + " file(s) changed on both sides. Fix the parts between <<<<<<< and >>>>>>> in:";
    std::string rows;
    for (const auto& f : files) {
        text += "\n  " + f;
        rows += f + "\n";
    }
    text += "\nthen code.git_commit all=true to finish, or code.git_merge abort=true to go back.";
    Result r = Result::success(text);
    return r.with("conflicts", static_cast<int64_t>(files.size())).with("rows", rows);
}

ProcessResult gitOk(const fs::path& dir, std::vector<std::string> args) {
    ProcessResult r = git(dir, std::move(args));
    if (r.exitCode != 0) {
        std::string out = sysutil::trim(r.output);
        if (out.find("not a git repository") != std::string::npos) throw std::runtime_error(sysutil::fromPath(dir) + " is not a git repository (code.git_init makes one)");
        throw std::runtime_error(out.empty() ? "git failed" : out);
    }
    return r;
}

std::vector<std::string> pathList(const Params& p) {
    std::vector<std::string> out;
    for (auto& a : splitArgs(stringParam(p, "paths", "."))) out.push_back(a);
    if (out.empty()) out.push_back(".");
    return out;
}

std::string stateName(char c) {
    switch (c) {
        case 'M': return "modified";
        case 'A': return "added";
        case 'D': return "deleted";
        case 'R': return "renamed";
        case 'C': return "copied";
        case 'U': return "conflict";
        case '?': return "new";
        case '!': return "ignored";
        default: return "";
    }
}

ParamSpec folderOpt() { return opt("folder", ParamType::String, "the repository folder (default: the open project)"); }

void registerReadCommands(Session& s, StatePtr st) {
    auto& reg = s.registry();
    reg.add({"code.git_status", "Changes",
             "The branch and every changed file: staged (ready to commit) and not staged, new files, conflicts.",
             {folderOpt()}, false, true},
            reader([st](const Params& p) {
                const fs::path dir = folderParam(*st, p);
                ProcessResult r = gitOk(dir, {"status", "--porcelain=v1", "-b", "-uall"});
                std::string branch, text, rows;
                bool fresh = false;
                std::size_t staged = 0, unstaged = 0;
                for (const auto& line : lines(r.output)) {
                    if (line.size() < 3) continue;
                    if (line.rfind("## ", 0) == 0) {
                        branch = line.substr(3);
                        for (const char* pre : {"No commits yet on ", "Initial commit on "}) {
                            if (branch.rfind(pre, 0) == 0) {
                                branch = branch.substr(std::string(pre).size());
                                fresh = true;
                            }
                        }
                        continue;
                    }
                    const char x = line[0], y = line[1];
                    std::string path = line.substr(3);
                    if (x == '?') {
                        ++unstaged;
                        text += "  new (not staged): " + path + "\n";
                        rows += "?\t?\t" + path + "\n";
                        continue;
                    }
                    if (x != ' ') {
                        ++staged;
                        text += "  " + stateName(x) + " (staged): " + path + "\n";
                    }
                    if (y != ' ') {
                        ++unstaged;
                        text += "  " + stateName(y) + " (not staged): " + path + "\n";
                    }
                    rows += std::string(1, x) + "\t" + std::string(1, y) + "\t" + path + "\n";
                }
                std::string head = "branch " + (branch.empty() ? "?" : branch) + (fresh ? " (no commits yet)" : "") + "\n";
                if (!staged && !unstaged) head += "nothing changed\n";
                return Result::success(head + text)
                    .with("branch", branch.substr(0, branch.find("...")))
                    .with("staged", static_cast<int64_t>(staged))
                    .with("unstaged", static_cast<int64_t>(unstaged))
                    .with("rows", rows);
            }));

    reg.add({"code.git_diff", "Diff",
             "What changed, as a unified diff: of everything or one file, not yet staged (default) or staged.",
             {folderOpt(), opt("file", ParamType::String, "only this file"), opt("staged", ParamType::Bool, "the staged changes", false),
              opt("commit", ParamType::String, "show this commit instead (e.g. HEAD~1, a hash)")},
             false, true},
            reader([st](const Params& p) {
                const fs::path dir = folderParam(*st, p);
                std::vector<std::string> args;
                if (!stringParam(p, "commit").empty()) args = {"show", "--stat", "-p", stringParam(p, "commit")};
                else args = {"diff"};
                if (boolParam(p, "staged", false) && args[0] == "diff") args.push_back("--cached");
                if (!stringParam(p, "file").empty()) {
                    args.push_back("--");
                    args.push_back(stringParam(p, "file"));
                }
                ProcessResult r = gitOk(dir, args);
                std::size_t added = 0, removed = 0;
                for (const auto& l : lines(r.output)) {
                    if (l.rfind("+++", 0) == 0 || l.rfind("---", 0) == 0) continue;
                    if (!l.empty() && l[0] == '+') ++added;
                    if (!l.empty() && l[0] == '-') ++removed;
                }
                return Result::success(r.output.empty() ? "no differences" : r.output)
                    .with("diff", r.output)
                    .with("added", static_cast<int64_t>(added))
                    .with("removed", static_cast<int64_t>(removed));
            }));

    reg.add({"code.git_log", "History", "Recent commits: short hash, author, when, and message.",
             {folderOpt(), opt("count", ParamType::Int, "how many", int64_t{20}), opt("file", ParamType::String, "only commits touching this file")},
             false, true},
            reader([st](const Params& p) {
                const fs::path dir = folderParam(*st, p);
                std::vector<std::string> args = {"log", "-n", std::to_string(std::max<int64_t>(1, intParam(p, "count", 20))), "--format=%h%x09%an%x09%ar%x09%s"};
                if (!stringParam(p, "file").empty()) args.insert(args.end(), {"--", stringParam(p, "file")});
                ProcessResult r = git(dir, args);
                if (r.exitCode != 0) {
                    if (r.output.find("does not have any commits") != std::string::npos) return Result::success("no commits yet").with("count", int64_t{0}).with("rows", "");
                    throw std::runtime_error(sysutil::trim(r.output));
                }
                std::string text;
                std::size_t n = 0;
                for (const auto& l : lines(r.output)) {
                    std::vector<std::string> f;
                    std::size_t start = 0;
                    while (true) {
                        std::size_t tab = l.find('\t', start);
                        f.push_back(l.substr(start, tab == std::string::npos ? std::string::npos : tab - start));
                        if (tab == std::string::npos) break;
                        start = tab + 1;
                    }
                    if (f.size() < 4) continue;
                    ++n;
                    text += f[0] + "  " + f[3] + "  (" + f[1] + ", " + f[2] + ")\n";
                }
                return Result::success(n ? text : "no commits yet").with("count", static_cast<int64_t>(n)).with("rows", r.output);
            }));

    reg.add({"code.git_branches", "Branches", "The branches, the current one marked with *.", {folderOpt()}, false, true},
            reader([st](const Params& p) {
                ProcessResult r = gitOk(folderParam(*st, p), {"branch", "--format=%(HEAD)%(refname:short)"});
                std::string text, current;
                std::size_t n = 0;
                for (const auto& l : lines(r.output)) {
                    if (l.size() < 2) continue;
                    ++n;
                    if (l[0] == '*') current = l.substr(1);
                    text += (l[0] == '*' ? "* " : "  ") + l.substr(1) + "\n";
                }
                return Result::success(n ? text : "no branches yet (make a commit first)").with("current", current).with("count", static_cast<int64_t>(n));
            }));
}

void registerWriteCommands(Session& s, StatePtr st) {
    auto& reg = s.registry();
    reg.add({"code.git_stage", "Stage", "Mark files to go in the next commit (paths=\".\" = everything).",
             {folderOpt(), opt("paths", ParamType::String, "files, separated by spaces (quotes for names with spaces)", std::string("."))}, false},
            reader([st](const Params& p) {
                auto paths = pathList(p);
                std::vector<std::string> args = {"add", "-A", "--"};
                args.insert(args.end(), paths.begin(), paths.end());
                gitOk(folderParam(*st, p), args);
                return Result::success("staged " + stringParam(p, "paths", "."));
            }));

    reg.add({"code.git_unstage", "Unstage", "Take files back out of the next commit (their changes stay).",
             {folderOpt(), opt("paths", ParamType::String, "files, separated by spaces", std::string("."))}, false},
            reader([st](const Params& p) {
                const fs::path dir = folderParam(*st, p);
                auto paths = pathList(p);
                std::vector<std::string> args = {"reset", "-q", "--"};
                ProcessResult head = git(dir, {"rev-parse", "--verify", "-q", "HEAD"});
                if (head.exitCode != 0) args = {"rm", "-q", "--cached", "-r", "--"};  // no commits yet
                args.insert(args.end(), paths.begin(), paths.end());
                gitOk(dir, args);
                return Result::success("unstaged " + stringParam(p, "paths", "."));
            }));

    reg.add({"code.git_commit", "Commit",
             "Save the staged changes as a commit with a message (all=true stages every changed file first).",
             {folderOpt(), {"message", ParamType::String, "what the change does", true}, opt("all", ParamType::Bool, "stage every change first", false)},
             false},
            reader([st](const Params& p) {
                const fs::path dir = folderParam(*st, p);
                const std::string msg = sysutil::trim(stringParam(p, "message"));
                if (msg.empty()) throw std::runtime_error("write a commit message");
                if (boolParam(p, "all", false)) gitOk(dir, {"add", "-A"});
                ProcessResult r = git(dir, {"commit", "-q", "-m", msg});
                if (r.exitCode != 0) {
                    std::string out = sysutil::trim(r.output);
                    if (out.find("user.email") != std::string::npos || out.find("Author identity") != std::string::npos)
                        throw std::runtime_error("git needs your name and email first: git config --global user.name \"Your Name\" and git config --global user.email you@example.com");
                    if (out.find("nothing to commit") != std::string::npos || out.find("no changes added") != std::string::npos)
                        throw std::runtime_error("nothing staged to commit (code.git_stage, or all=true)");
                    throw std::runtime_error(out);
                }
                ProcessResult h = git(dir, {"log", "-1", "--format=%h"});
                const std::string hash = sysutil::trim(h.output);
                return Result::success("committed " + hash + ": " + msg).with("commit", hash);
            }));

    reg.add({"code.git_switch", "Switch branch", "Switch to another branch, or make a new one (create=true).",
             {folderOpt(), {"branch", ParamType::String, "branch name", true}, opt("create", ParamType::Bool, "make a new branch here", false)}, false},
            reader([st](const Params& p) {
                const std::string b = stringParam(p, "branch");
                if (b.empty()) throw std::runtime_error("give a branch name");
                gitOk(folderParam(*st, p), boolParam(p, "create", false) ? std::vector<std::string>{"checkout", "-q", "-b", b} : std::vector<std::string>{"checkout", "-q", b});
                return Result::success("on branch " + b).with("branch", b);
            }));

    reg.add({"code.git_merge", "Merge",
             "Bring another branch's commits into the current one. Stops on conflicts and lists the files (status shows them "
             "too); fix them, then code.git_commit all=true. abort=true goes back to before the merge.",
             {folderOpt(), opt("branch", ParamType::String, "the branch to merge in (e.g. main, or origin/main after a fetch)"),
              opt("abort", ParamType::Bool, "give up on a merge that stopped on conflicts", false)},
             false},
            reader([st](const Params& p) {
                const fs::path dir = folderParam(*st, p);
                if (boolParam(p, "abort", false)) {
                    gitOk(dir, {"merge", "--abort"});
                    return Result::success("merge abandoned: back to how it was");
                }
                const std::string b = stringParam(p, "branch");
                if (b.empty()) throw std::runtime_error("give the branch to merge in (branch=)");
                const std::string before = sysutil::trim(git(dir, {"rev-parse", "HEAD"}).output);
                ProcessResult r = git(dir, {"merge", "--no-edit", b});
                if (r.exitCode != 0) {
                    if (!conflictedFiles(dir).empty()) return conflictResult(dir, "merging " + b);
                    throw std::runtime_error(sysutil::trim(r.output));
                }
                const std::string after = sysutil::trim(git(dir, {"rev-parse", "HEAD"}).output);
                if (after == before) return Result::success("already up to date with " + b).with("conflicts", int64_t{0});
                const bool ff = r.output.find("Fast-forward") != std::string::npos;
                return Result::success(std::string(ff ? "fast-forwarded to " : "merged ") + b + " (" + sysutil::trim(git(dir, {"log", "-1", "--format=%h"}).output) + ")")
                    .with("conflicts", int64_t{0})
                    .with("commit", sysutil::trim(git(dir, {"log", "-1", "--format=%h"}).output));
            }));

    reg.add({"code.git_remotes", "Remotes",
             "The servers this repository syncs with (name and address). add= with url= adds one (e.g. add=origin "
             "url=https://github.com/you/project.git), remove= takes one away.",
             {folderOpt(), opt("add", ParamType::String, "name of a remote to add"), opt("url", ParamType::String, "its address"),
              opt("remove", ParamType::String, "name of a remote to remove")},
             false},
            reader([st](const Params& p) {
                const fs::path dir = folderParam(*st, p);
                std::string note;
                if (!stringParam(p, "add").empty()) {
                    if (stringParam(p, "url").empty()) throw std::runtime_error("add= needs url=");
                    gitOk(dir, {"remote", "add", stringParam(p, "add"), stringParam(p, "url")});
                    note = "added " + stringParam(p, "add") + "\n";
                }
                if (!stringParam(p, "remove").empty()) {
                    gitOk(dir, {"remote", "remove", stringParam(p, "remove")});
                    note += "removed " + stringParam(p, "remove") + "\n";
                }
                std::string text, rows;
                int64_t n = 0;
                for (const auto& l : lines(gitOk(dir, {"remote", "-v"}).output)) {
                    if (l.find("(fetch)") == std::string::npos) continue;
                    const auto tab = l.find('\t');
                    if (tab == std::string::npos) continue;
                    std::string url = l.substr(tab + 1);
                    url = sysutil::trim(url.substr(0, url.rfind(" (")));
                    text += "  " + l.substr(0, tab) + "  " + url + "\n";
                    rows += l.substr(0, tab) + "\t" + url + "\n";
                    ++n;
                }
                return Result::success(note + (n ? text : "no remotes (add=origin url=... adds one)\n")).with("count", n).with("rows", rows);
            }));

    reg.add({"code.git_fetch", "Fetch",
             "Download new commits from a remote (all of them by default) without changing your files. Then git_status "
             "shows how far ahead or behind you are, and git_merge branch=origin/main brings them in.",
             {folderOpt(), opt("remote", ParamType::String, "which remote (default: all)")}, false},
            reader([st](const Params& p) {
                const fs::path dir = folderParam(*st, p);
                const std::string remote = stringParam(p, "remote");
                ProcessResult r = gitRemote(dir, remote.empty() ? std::vector<std::string>{"fetch", "--all", "--prune"} : std::vector<std::string>{"fetch", "--prune", remote});
                if (r.exitCode != 0) throw std::runtime_error(sysutil::trim(r.output));
                ProcessResult ab = git(dir, {"rev-list", "--left-right", "--count", "HEAD...@{upstream}"});
                std::string text = "fetched " + (remote.empty() ? std::string("every remote") : remote);
                Result res = Result::success("");
                if (ab.exitCode == 0) {
                    long long ahead = 0, behind = 0;
                    std::sscanf(ab.output.c_str(), "%lld %lld", &ahead, &behind);
                    text += ": " + std::to_string(ahead) + " commit(s) to push, " + std::to_string(behind) + " to pull";
                    res.with("ahead", static_cast<int64_t>(ahead)).with("behind", static_cast<int64_t>(behind));
                }
                res.message = text;
                return res;
            }));

    reg.add({"code.git_pull", "Pull",
             "Fetch and merge the current branch's upstream (or remote= and branch=). rebase=true replays your commits on "
             "top instead of merging. Stops on conflicts and lists the files, like git_merge.",
             {folderOpt(), opt("remote", ParamType::String, "remote (default: the branch's upstream)"), opt("branch", ParamType::String, "branch on the remote"),
              opt("rebase", ParamType::Bool, "rebase instead of merge", false)},
             false},
            reader([st](const Params& p) {
                const fs::path dir = folderParam(*st, p);
                std::vector<std::string> args{"pull", "--no-edit", boolParam(p, "rebase", false) ? "--rebase" : "--no-rebase"};
                if (!stringParam(p, "remote").empty()) args.push_back(stringParam(p, "remote"));
                if (!stringParam(p, "branch").empty()) {
                    if (stringParam(p, "remote").empty()) args.push_back("origin");
                    args.push_back(stringParam(p, "branch"));
                }
                const std::string before = sysutil::trim(git(dir, {"rev-parse", "HEAD"}).output);
                ProcessResult r = gitRemote(dir, args);
                if (r.exitCode != 0) {
                    if (!conflictedFiles(dir).empty()) return conflictResult(dir, "pulling");
                    std::string out = sysutil::trim(r.output);
                    if (out.find("no tracking information") != std::string::npos)
                        throw std::runtime_error("this branch has no upstream yet: give remote= and branch= (or push once with code.git_push)");
                    throw std::runtime_error(out);
                }
                const std::string after = sysutil::trim(git(dir, {"rev-parse", "HEAD"}).output);
                if (before == after) return Result::success("already up to date").with("conflicts", int64_t{0}).with("commits", int64_t{0});
                const auto count = sysutil::trim(git(dir, {"rev-list", "--count", before + ".." + after}).output);
                return Result::success("pulled " + count + " commit(s), now at " + sysutil::trim(git(dir, {"log", "-1", "--format=%h %s"}).output))
                    .with("conflicts", int64_t{0})
                    .with("commits", static_cast<int64_t>(std::atoll(count.c_str())));
            }));

    reg.add({"code.git_push", "Push",
             "Send the current branch's commits to a remote (default origin). The first push of a branch sets it as the "
             "upstream so later pulls and pushes need no names. force=true overwrites the remote branch only if nobody "
             "else pushed since your last fetch (--force-with-lease).",
             {folderOpt(), opt("remote", ParamType::String, "remote (default: the upstream's, or origin)"), opt("branch", ParamType::String, "branch to push (default: the current one)"),
              opt("force", ParamType::Bool, "replace the remote branch (safely)", false)},
             false},
            reader([st](const Params& p) {
                const fs::path dir = folderParam(*st, p);
                const std::string branch = stringParam(p, "branch").empty() ? currentBranch(dir) : stringParam(p, "branch");
                if (branch.empty() || branch == "HEAD") throw std::runtime_error("not on a branch (code.git_switch to one first)");
                std::string remote = stringParam(p, "remote");
                const bool hasUpstream = git(dir, {"rev-parse", "--abbrev-ref", branch + "@{upstream}"}).exitCode == 0;
                if (remote.empty()) {
                    remote = sysutil::trim(git(dir, {"config", "branch." + branch + ".remote"}).output);
                    if (remote.empty()) remote = "origin";
                }
                std::vector<std::string> args{"push", "--porcelain"};
                if (!hasUpstream) args.push_back("-u");
                if (boolParam(p, "force", false)) args.push_back("--force-with-lease");
                args.push_back(remote);
                args.push_back(branch);
                ProcessResult r = gitRemote(dir, args);
                if (r.exitCode != 0) {
                    const std::string out = sysutil::trim(r.output);
                    if (out.find("rejected") != std::string::npos && (out.find("fetch first") != std::string::npos || out.find("non-fast-forward") != std::string::npos))
                        throw std::runtime_error("the remote has commits you don't have yet: code.git_pull first, then push\n" + out);
                    if (out.find("does not appear to be a git repository") != std::string::npos || out.find("No such remote") != std::string::npos)
                        throw std::runtime_error("no remote called " + remote + " (code.git_remotes add=" + remote + " url=... adds it)");
                    throw std::runtime_error(out);
                }
                const bool upToDate = r.output.find("[up to date]") != std::string::npos;
                return Result::success(upToDate ? branch + " is already up to date on " + remote
                                                : "pushed " + branch + " to " + remote + (hasUpstream ? "" : " (now its upstream)"))
                    .with("remote", remote)
                    .with("branch", branch);
            }));

    reg.add({"code.git_init", "Start a repository", "Make a folder a git repository so changes can be tracked.", {folderOpt()}, false},
            reader([st](const Params& p) {
                const fs::path dir = folderParam(*st, p);
                gitOk(dir, {"init", "-q"});
                return Result::success("git repository started in " + sysutil::fromPath(dir));
            }));
}

}  // namespace

void registerGitCommands(Session& s, StatePtr st) {
    registerReadCommands(s, st);
    registerWriteCommands(s, st);
}

}  // namespace wetwaresuite::code::detail
