// Running helper programs (ffmpeg, LibreOffice, ...) without a shell, so file
// names can never be interpreted as commands.
#pragma once

#include <filesystem>
#include <optional>
#include <utility>
#include <string>
#include <vector>

namespace wetwaresuite {

// Finds an executable on PATH (adds .exe on Windows), then in the suite's own
// tools folder and where package managers usually put programs (Homebrew,
// winget, Chocolatey, Scoop...), since an app started from the desktop often
// gets a shorter PATH than a terminal. Empty if missing.
std::optional<std::filesystem::path> findProgram(const std::string& name);

// Where the suite keeps helper programs it downloads itself (ffmpeg):
// %LOCALAPPDATA%\wetwaresuite\tools, ~/Library/Application Support/wetwaresuite/tools
// or ~/.local/share/wetwaresuite/tools. WETWARESUITE_TOOLS_DIR overrides it.
std::filesystem::path suiteToolsDir();

struct ProcessResult {
    int exitCode = -1;  // -1 when the program couldn't start
    std::string output;  // stdout and stderr together, truncated to the last 64 KiB
};

// Runs args[0] with the remaining args and waits for it.
ProcessResult runProgram(const std::vector<std::string>& args);

}  // namespace wetwaresuite

namespace wetwaresuite {

// A program running in the background whose output is read as it arrives: what
// a terminal, "run this file" and long builds need. Output is stdout and stderr
// together; input is a pipe the program reads as its stdin. The destructor stops
// the program (and on Windows and POSIX, the programs it started) if it still runs.
class ChildProcess {
public:
    ChildProcess();
    ~ChildProcess();
    ChildProcess(const ChildProcess&) = delete;
    ChildProcess& operator=(const ChildProcess&) = delete;

    // Starts args[0] (found on PATH) in `cwd` ("" = this process's folder) with
    // `env` added to the environment. False with a plain-word `error` when it can't.
    bool start(const std::vector<std::string>& args, const std::filesystem::path& cwd, std::string& error,
               const std::vector<std::pair<std::string, std::string>>& env = {});
    // Call before start: the program's error output is thrown away instead of mixed into read()
    // (for programs that speak a protocol on their output, such as language servers).
    void dropErrors(bool on);
    // Output that arrived since the last call; never blocks.
    std::string read();
    // Waits up to `seconds` for more output or the end; returns what arrived.
    std::string readFor(double seconds);
    bool write(const std::string& text);
    void closeInput();
    bool running();         // false once it has ended (or never started)
    int exitCode() const;   // -1 while running or when it never started
    void kill();            // stops it and everything it started
    // Waits up to `seconds` for it to end, collecting output into `output`. True when it ended.
    bool wait(double seconds, std::string& output);
    long long pid() const;

private:
    struct Impl;
    Impl* impl_;
};

}  // namespace wetwaresuite
