#include "wetwaresuite/core/process.hpp"

#include <cctype>
#include <cerrno>
#include <cstdlib>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#else
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

#include <chrono>
#include <cstring>
extern char** environ;
#endif
#include <chrono>
#include <thread>

namespace wetwaresuite {

namespace {

constexpr std::size_t kMaxOutput = 64 * 1024;

void keepTail(std::string& out) {
    if (out.size() > kMaxOutput) out.erase(0, out.size() - kMaxOutput);
}

#ifdef _WIN32
std::wstring widen(const std::string& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(static_cast<std::size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
    return w;
}

// Quotes one argument the way CommandLineToArgvW parses it.
std::wstring quoteArg(const std::wstring& arg) {
    if (!arg.empty() && arg.find_first_of(L" \t\n\v\"") == std::wstring::npos) return arg;
    std::wstring out = L"\"";
    for (std::size_t i = 0;; ++i) {
        std::size_t slashes = 0;
        while (i < arg.size() && arg[i] == L'\\') {
            ++i;
            ++slashes;
        }
        if (i == arg.size()) {
            out.append(slashes * 2, L'\\');
            break;
        }
        if (arg[i] == L'"') {
            out.append(slashes * 2 + 1, L'\\');
            out += L'"';
        } else {
            out.append(slashes, L'\\');
            out += arg[i];
        }
    }
    out += L'"';
    return out;
}
#endif

}  // namespace

namespace {

std::string envText(const char* name) {
    const char* v = std::getenv(name);
    return v ? std::string(v) : std::string();
}

std::optional<std::filesystem::path> programIn(const std::filesystem::path& dir, const std::string& name) {
    namespace fs = std::filesystem;
#ifdef _WIN32
    static const std::vector<std::string> suffixes = {".exe", ".com", ".bat", ""};
#else
    static const std::vector<std::string> suffixes = {""};
#endif
    for (const auto& suffix : suffixes) {
        fs::path candidate = dir / (name + suffix);
        std::error_code ec;
        if (fs::is_regular_file(candidate, ec)) {
#ifndef _WIN32
            if (access(candidate.c_str(), X_OK) != 0) continue;
#endif
            return candidate;
        }
    }
    return std::nullopt;
}

// Folders outside PATH where programs are often installed.
std::vector<std::filesystem::path> usualProgramDirs() {
    namespace fs = std::filesystem;
    std::vector<fs::path> dirs = {suiteToolsDir()};
#ifdef _WIN32
    const std::string local = envText("LOCALAPPDATA"), user = envText("USERPROFILE");
    if (!local.empty()) {
        dirs.push_back(fs::path(local) / "Microsoft" / "WinGet" / "Links");
        // winget unpacks zips like Gyan.FFmpeg_.../ffmpeg-7.1-full_build/bin
        std::error_code ec;
        for (fs::directory_iterator it(fs::path(local) / "Microsoft" / "WinGet" / "Packages", ec), end; !ec && it != end; it.increment(ec)) {
            std::error_code ec2;
            for (fs::directory_iterator sub(it->path(), ec2), send; !ec2 && sub != send; sub.increment(ec2)) dirs.push_back(sub->path() / "bin");
        }
    }
    if (!user.empty()) dirs.push_back(fs::path(user) / "scoop" / "shims");
    const std::string programData = envText("ProgramData"), programFiles = envText("ProgramFiles");
    dirs.push_back(fs::path(programData.empty() ? "C:\\ProgramData" : programData) / "chocolatey" / "bin");
    dirs.push_back(fs::path(programFiles.empty() ? "C:\\Program Files" : programFiles) / "ffmpeg" / "bin");
    dirs.push_back("C:\\ffmpeg\\bin");
#else
#ifdef __APPLE__
    dirs.insert(dirs.end(), {"/opt/homebrew/bin", "/usr/local/bin", "/opt/local/bin"});
#else
    dirs.insert(dirs.end(), {"/usr/local/bin", "/usr/bin", "/snap/bin"});
#endif
    if (const std::string home = envText("HOME"); !home.empty()) dirs.push_back(fs::path(home) / ".local" / "bin");
#endif
    return dirs;
}

}  // namespace

std::filesystem::path suiteToolsDir() {
    namespace fs = std::filesystem;
    if (const std::string dir = envText("WETWARESUITE_TOOLS_DIR"); !dir.empty()) return dir;
#ifdef _WIN32
    std::string base = envText("LOCALAPPDATA");
    if (base.empty()) base = envText("APPDATA");
    return (base.empty() ? fs::temp_directory_path() : fs::path(base)) / "wetwaresuite" / "tools";
#elif defined(__APPLE__)
    const std::string home = envText("HOME");
    return (home.empty() ? fs::temp_directory_path() : fs::path(home) / "Library" / "Application Support") / "wetwaresuite" / "tools";
#else
    if (const std::string xdg = envText("XDG_DATA_HOME"); !xdg.empty()) return fs::path(xdg) / "wetwaresuite" / "tools";
    const std::string home = envText("HOME");
    return (home.empty() ? fs::temp_directory_path() : fs::path(home) / ".local" / "share") / "wetwaresuite" / "tools";
#endif
}

std::optional<std::filesystem::path> findProgram(const std::string& name) {
    namespace fs = std::filesystem;
    if (name.find('/') != std::string::npos || name.find('\\') != std::string::npos) {
        std::error_code ec;
        if (fs::exists(name, ec)) return fs::path(name);
        return std::nullopt;
    }
#ifdef _WIN32
    const char sep = ';';
#else
    const char sep = ':';
#endif
    const std::string paths = envText("PATH");
    std::size_t start = 0;
    while (!paths.empty() && start <= paths.size()) {
        auto end = paths.find(sep, start);
        std::string dir = paths.substr(start, end == std::string::npos ? std::string::npos : end - start);
        if (!dir.empty()) {
            if (auto found = programIn(dir, name)) return found;
        }
        if (end == std::string::npos) break;
        start = end + 1;
    }
    for (const auto& dir : usualProgramDirs()) {
        if (auto found = programIn(dir, name)) return found;
    }
    return std::nullopt;
}

ProcessResult runProgram(const std::vector<std::string>& args) {
    ProcessResult result;
    if (args.empty()) return result;
#ifdef _WIN32
    SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
    HANDLE readPipe = nullptr, writePipe = nullptr;
    if (!CreatePipe(&readPipe, &writePipe, &sa, 0)) return result;
    SetHandleInformation(readPipe, HANDLE_FLAG_INHERIT, 0);

    std::wstring cmd;
    for (const auto& a : args) cmd += (cmd.empty() ? L"" : L" ") + quoteArg(widen(a));
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = writePipe;
    si.hStdError = writePipe;
    si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    PROCESS_INFORMATION pi{};
    BOOL ok = CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
    CloseHandle(writePipe);
    if (!ok) {
        CloseHandle(readPipe);
        return result;
    }
    char buf[4096];
    DWORD n = 0;
    while (ReadFile(readPipe, buf, sizeof buf, &n, nullptr) && n > 0) {
        result.output.append(buf, n);
        keepTail(result.output);
    }
    CloseHandle(readPipe);
    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD code = 0;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    result.exitCode = static_cast<int>(code);
#else
    int fds[2];
    if (pipe(fds) != 0) return result;
    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_addclose(&actions, fds[0]);
    posix_spawn_file_actions_adddup2(&actions, fds[1], STDOUT_FILENO);
    posix_spawn_file_actions_adddup2(&actions, fds[1], STDERR_FILENO);
    posix_spawn_file_actions_addclose(&actions, fds[1]);
    posix_spawn_file_actions_addopen(&actions, STDIN_FILENO, "/dev/null", 0, 0);

    std::vector<char*> argv;
    for (const auto& a : args) argv.push_back(const_cast<char*>(a.c_str()));
    argv.push_back(nullptr);
    pid_t pid = 0;
    int rc = posix_spawnp(&pid, argv[0], &actions, nullptr, argv.data(), environ);
    posix_spawn_file_actions_destroy(&actions);
    close(fds[1]);
    if (rc != 0) {
        close(fds[0]);
        return result;
    }
    char buf[4096];
    ssize_t n;
    while ((n = read(fds[0], buf, sizeof buf)) > 0) {
        result.output.append(buf, static_cast<std::size_t>(n));
        keepTail(result.output);
    }
    close(fds[0]);
    int status = 0;
    while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {
    }
    result.exitCode = WIFEXITED(status) ? WEXITSTATUS(status) : 128 + (WIFSIGNALED(status) ? WTERMSIG(status) : 0);
#endif
    return result;
}

// ---- ChildProcess ------------------------------------------------------------------

namespace {

double secondsSince(std::chrono::steady_clock::time_point t) {
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - t).count();
}

}  // namespace

#ifdef _WIN32

struct ChildProcess::Impl {
    HANDLE proc = nullptr;
    HANDLE job = nullptr;
    HANDLE out = nullptr;
    HANDLE in = nullptr;
    DWORD pid = 0;
    int code = -1;
    bool ended = true;
    bool dropErrors = false;
};

ChildProcess::ChildProcess() : impl_(new Impl) {}

ChildProcess::~ChildProcess() {
    kill();
    if (impl_->out) CloseHandle(impl_->out);
    if (impl_->in) CloseHandle(impl_->in);
    if (impl_->proc) CloseHandle(impl_->proc);
    if (impl_->job) CloseHandle(impl_->job);
    delete impl_;
}

bool ChildProcess::start(const std::vector<std::string>& args, const std::filesystem::path& cwd, std::string& error,
                         const std::vector<std::pair<std::string, std::string>>& env) {
    if (args.empty()) {
        error = "nothing to run";
        return false;
    }
    if (!impl_->ended) {
        error = "it's already running";
        return false;
    }
    auto program = findProgram(args[0]);
    if (!program) {
        error = "couldn't find " + args[0] + " (is it installed and on PATH?)";
        return false;
    }
    std::vector<std::string> full = args;
    // backslashes: cmd.exe reads switches out of its own path, so C:/Windows/.../cmd.exe breaks every command
    full[0] = std::filesystem::path(*program).make_preferred().string();
    std::string ext = program->extension().string();
    for (auto& c : ext) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (ext == ".bat" || ext == ".cmd") full.insert(full.begin(), {"cmd.exe", "/d", "/c"});

    SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
    HANDLE outRead = nullptr, outWrite = nullptr, inRead = nullptr, inWrite = nullptr;
    if (!CreatePipe(&outRead, &outWrite, &sa, 0)) {
        error = "couldn't make a pipe";
        return false;
    }
    if (!CreatePipe(&inRead, &inWrite, &sa, 0)) {
        CloseHandle(outRead);
        CloseHandle(outWrite);
        error = "couldn't make a pipe";
        return false;
    }
    SetHandleInformation(outRead, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(inWrite, HANDLE_FLAG_INHERIT, 0);

    std::wstring cmd;
    for (const auto& a : full) cmd += (cmd.empty() ? L"" : L" ") + quoteArg(widen(a));
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = outWrite;
    si.hStdError = outWrite;
    HANDLE nul = nullptr;
    if (impl_->dropErrors) {
        nul = CreateFileW(L"NUL", GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, OPEN_EXISTING, 0, nullptr);
        if (nul != INVALID_HANDLE_VALUE) si.hStdError = nul;
        else nul = nullptr;
    }
    si.hStdInput = inRead;
    PROCESS_INFORMATION pi{};
    // the extra variables are set here for the child to inherit, then put back
    std::vector<std::pair<std::wstring, std::wstring>> saved;
    for (const auto& [k, v] : env) {
        std::wstring key = widen(k);
        wchar_t old[32768];
        DWORD n = GetEnvironmentVariableW(key.c_str(), old, 32768);
        saved.emplace_back(key, n > 0 && n < 32768 ? std::wstring(old, n) : std::wstring(1, L'\0'));
        SetEnvironmentVariableW(key.c_str(), widen(v).c_str());
    }
    // backslashes: cmd.exe started in a folder written with / fails every command ("The syntax of the command is incorrect")
    std::wstring dir = cwd.empty() ? std::wstring() : std::filesystem::path(cwd).make_preferred().wstring();
    BOOL ok = CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW | CREATE_SUSPENDED, nullptr,
                             dir.empty() ? nullptr : dir.c_str(), &si, &pi);
    const DWORD startError = GetLastError();
    for (const auto& [k, v] : saved) SetEnvironmentVariableW(k.c_str(), v.size() == 1 && v[0] == L'\0' ? nullptr : v.c_str());
    CloseHandle(outWrite);
    CloseHandle(inRead);
    if (nul) CloseHandle(nul);
    if (!ok) {
        CloseHandle(outRead);
        CloseHandle(inWrite);
        error = "couldn't start " + args[0] + " (Windows error " + std::to_string(startError) + ")";
        return false;
    }
    if (impl_->job) CloseHandle(impl_->job);
    impl_->job = CreateJobObjectW(nullptr, nullptr);
    if (impl_->job) {
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION li{};
        li.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        SetInformationJobObject(impl_->job, JobObjectExtendedLimitInformation, &li, sizeof(li));
        AssignProcessToJobObject(impl_->job, pi.hProcess);
    }
    ResumeThread(pi.hThread);
    CloseHandle(pi.hThread);
    if (impl_->proc) CloseHandle(impl_->proc);
    if (impl_->out) CloseHandle(impl_->out);
    if (impl_->in) CloseHandle(impl_->in);
    impl_->proc = pi.hProcess;
    impl_->pid = pi.dwProcessId;
    impl_->out = outRead;
    impl_->in = inWrite;
    impl_->code = -1;
    impl_->ended = false;
    return true;
}

std::string ChildProcess::read() {
    std::string got;
    if (!impl_->out) return got;
    char buf[4096];
    for (;;) {
        DWORD avail = 0;
        if (!PeekNamedPipe(impl_->out, nullptr, 0, nullptr, &avail, nullptr)) {
            CloseHandle(impl_->out);  // the other end is closed: everything has been read
            impl_->out = nullptr;
            break;
        }
        if (avail == 0) break;
        DWORD n = 0;
        if (!ReadFile(impl_->out, buf, avail < sizeof buf ? avail : static_cast<DWORD>(sizeof buf), &n, nullptr) || n == 0) break;
        got.append(buf, n);
    }
    return got;
}

std::string ChildProcess::readFor(double seconds) {
    const auto start = std::chrono::steady_clock::now();
    std::string got = read();
    while (got.empty() && impl_->out && secondsSince(start) < seconds) {
        Sleep(5);
        got = read();
        if (got.empty() && !running()) {
            got = read();
            break;
        }
    }
    if (got.empty() && !impl_->out) Sleep(10);  // nothing more can arrive: just let the time pass
    return got;
}

bool ChildProcess::write(const std::string& text) {
    if (!impl_->in) return false;
    DWORD n = 0;
    return WriteFile(impl_->in, text.data(), static_cast<DWORD>(text.size()), &n, nullptr) && n == text.size();
}

void ChildProcess::closeInput() {
    if (impl_->in) CloseHandle(impl_->in);
    impl_->in = nullptr;
}

bool ChildProcess::running() {
    if (impl_->ended || !impl_->proc) return false;
    if (WaitForSingleObject(impl_->proc, 0) != WAIT_OBJECT_0) return true;
    DWORD code = 0;
    GetExitCodeProcess(impl_->proc, &code);
    impl_->code = static_cast<int>(code);
    impl_->ended = true;
    return false;
}

int ChildProcess::exitCode() const { return impl_->ended ? impl_->code : -1; }

void ChildProcess::kill() {
    if (!running()) return;
    if (impl_->job) TerminateJobObject(impl_->job, 1);
    else TerminateProcess(impl_->proc, 1);
    WaitForSingleObject(impl_->proc, 2000);
    running();
    if (!impl_->ended) {
        impl_->ended = true;
        impl_->code = 1;
    }
}

void ChildProcess::dropErrors(bool on) { impl_->dropErrors = on; }

long long ChildProcess::pid() const { return static_cast<long long>(impl_->pid); }

#else

struct ChildProcess::Impl {
    pid_t pid = -1;
    int out = -1;
    int in = -1;
    int code = -1;
    bool ended = true;
    bool dropErrors = false;
};

namespace {

void closeOnExec(int fd) { fcntl(fd, F_SETFD, fcntl(fd, F_GETFD) | FD_CLOEXEC); }

bool makePipe(int fds[2]) {
    if (pipe(fds) != 0) return false;
    closeOnExec(fds[0]);
    closeOnExec(fds[1]);
    return true;
}

}  // namespace

ChildProcess::ChildProcess() : impl_(new Impl) {}

ChildProcess::~ChildProcess() {
    kill();
    if (impl_->out >= 0) close(impl_->out);
    if (impl_->in >= 0) close(impl_->in);
    delete impl_;
}

bool ChildProcess::start(const std::vector<std::string>& args, const std::filesystem::path& cwd, std::string& error,
                         const std::vector<std::pair<std::string, std::string>>& env) {
    if (args.empty()) {
        error = "nothing to run";
        return false;
    }
    if (!impl_->ended) {
        error = "it's already running";
        return false;
    }
    auto program = findProgram(args[0]);
    if (!program) {
        error = "couldn't find " + args[0] + " (is it installed and on PATH?)";
        return false;
    }
    static const bool ignorePipe = [] {
        signal(SIGPIPE, SIG_IGN);  // writing to a program that has ended must not end us
        return true;
    }();
    (void)ignorePipe;
    int outp[2], inp[2], status[2];
    if (!makePipe(outp)) {
        error = "couldn't make a pipe";
        return false;
    }
    if (!makePipe(inp)) {
        close(outp[0]);
        close(outp[1]);
        error = "couldn't make a pipe";
        return false;
    }
    if (!makePipe(status)) {
        for (int fd : {outp[0], outp[1], inp[0], inp[1]}) close(fd);
        error = "couldn't make a pipe";
        return false;
    }
    const std::string path = program->string();
    const std::string dir = cwd.string();
    const bool dropErrors = impl_->dropErrors;
    std::vector<char*> argv;
    for (const auto& a : args) argv.push_back(const_cast<char*>(a.c_str()));
    argv.push_back(nullptr);
    pid_t pid = fork();
    if (pid == 0) {
        setpgid(0, 0);  // its own group, so kill() reaches everything it starts
        int err = 0;
        if (!dir.empty() && chdir(dir.c_str()) != 0) err = errno;
        if (!err) {
            dup2(inp[0], STDIN_FILENO);
            dup2(outp[1], STDOUT_FILENO);
            const int nul = dropErrors ? open("/dev/null", O_WRONLY) : -1;
            dup2(nul >= 0 ? nul : outp[1], STDERR_FILENO);
            signal(SIGPIPE, SIG_DFL);
            for (const auto& [k, v] : env) setenv(k.c_str(), v.c_str(), 1);
            execv(path.c_str(), argv.data());
            err = errno;
        }
        ssize_t w = ::write(status[1], &err, sizeof err);
        (void)w;
        _exit(127);
    }
    close(outp[1]);
    close(inp[0]);
    close(status[1]);
    if (pid < 0) {
        close(outp[0]);
        close(inp[1]);
        close(status[0]);
        error = "couldn't start " + args[0];
        return false;
    }
    int err = 0;
    ssize_t n;
    while ((n = ::read(status[0], &err, sizeof err)) < 0 && errno == EINTR) {
    }
    close(status[0]);
    if (n == static_cast<ssize_t>(sizeof err)) {
        int st = 0;
        waitpid(pid, &st, 0);
        close(outp[0]);
        close(inp[1]);
        error = "couldn't start " + args[0] + ": " + std::strerror(err);
        return false;
    }
    fcntl(outp[0], F_SETFL, fcntl(outp[0], F_GETFL) | O_NONBLOCK);
    if (impl_->out >= 0) close(impl_->out);
    if (impl_->in >= 0) close(impl_->in);
    impl_->pid = pid;
    impl_->out = outp[0];
    impl_->in = inp[1];
    impl_->code = -1;
    impl_->ended = false;
    return true;
}

std::string ChildProcess::read() {
    std::string got;
    if (impl_->out < 0) return got;
    char buf[4096];
    for (;;) {
        ssize_t n = ::read(impl_->out, buf, sizeof buf);
        if (n > 0) {
            got.append(buf, static_cast<std::size_t>(n));
            continue;
        }
        if (n < 0 && errno == EINTR) continue;
        if (n == 0) {  // every writer has closed it: all output is in
            close(impl_->out);
            impl_->out = -1;
        }
        break;
    }
    return got;
}

std::string ChildProcess::readFor(double seconds) {
    std::string got = read();
    if (!got.empty()) return got;
    if (impl_->out < 0) {  // nothing more can arrive: just let the time pass
        std::this_thread::sleep_for(std::chrono::duration<double>(seconds < 0.01 ? (seconds > 0 ? seconds : 0) : 0.01));
        return got;
    }
    pollfd p{impl_->out, POLLIN, 0};
    int ms = static_cast<int>(seconds * 1000);
    if (poll(&p, 1, ms < 0 ? 0 : ms) > 0) got = read();
    return got;
}

bool ChildProcess::write(const std::string& text) {
    if (impl_->in < 0) return false;
    std::size_t done = 0;
    while (done < text.size()) {
        ssize_t n = ::write(impl_->in, text.data() + done, text.size() - done);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return false;
        done += static_cast<std::size_t>(n);
    }
    return true;
}

void ChildProcess::closeInput() {
    if (impl_->in >= 0) close(impl_->in);
    impl_->in = -1;
}

bool ChildProcess::running() {
    if (impl_->ended || impl_->pid <= 0) return false;
    int status = 0;
    pid_t r = waitpid(impl_->pid, &status, WNOHANG);
    if (r == 0) return true;
    if (r == impl_->pid) impl_->code = WIFEXITED(status) ? WEXITSTATUS(status) : 128 + (WIFSIGNALED(status) ? WTERMSIG(status) : 0);
    impl_->ended = true;
    return false;
}

int ChildProcess::exitCode() const { return impl_->ended ? impl_->code : -1; }

void ChildProcess::kill() {
    if (!running()) return;
    ::kill(-impl_->pid, SIGKILL);
    ::kill(impl_->pid, SIGKILL);
    int status = 0;
    while (waitpid(impl_->pid, &status, 0) < 0 && errno == EINTR) {
    }
    impl_->code = 128 + SIGKILL;
    impl_->ended = true;
}

void ChildProcess::dropErrors(bool on) { impl_->dropErrors = on; }

long long ChildProcess::pid() const { return static_cast<long long>(impl_->pid); }

#endif

bool ChildProcess::wait(double seconds, std::string& output) {
    const auto start = std::chrono::steady_clock::now();
    for (;;) {
        double left = seconds - secondsSince(start);
        output += readFor(left < 0.05 ? (left > 0 ? left : 0) : 0.05);
        if (!running()) {
            // what it wrote just before it ended (programs it started may still hold the pipe: don't wait for them)
            output += readFor(0.05);
            output += read();
            return true;
        }
        if (secondsSince(start) >= seconds) return false;
    }
}

}  // namespace wetwaresuite
