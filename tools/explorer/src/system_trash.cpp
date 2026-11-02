// The computer's own trash (Recycle Bin on Windows, the Trash on macOS, the freedesktop.org
// trash on Linux desktops), so a file deleted in the suite shows up where people look for it.
// The suite keeps a small record of each one in its own trash folder, so explorer.trash lists it
// and undo or explorer.restore can still put it back.
#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <iterator>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <fstream>
#include <stdexcept>

#include "wetwaresuite/explorer/explorer.hpp"
#include "wetwaresuite/sysutil/sysutil.hpp"

#if defined(_WIN32)
#include <windows.h>
#include <shellapi.h>
#include <cwctype>
#else
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace wetwaresuite::explorer {

namespace {

std::string env(const char* name) {
    const char* v = std::getenv(name);
    return v ? v : "";
}

std::string lowerAscii(std::string s) {
    for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

#if defined(__linux__) || defined(__FreeBSD__) || defined(__OpenBSD__)

// freedesktop.org trash spec: <data home>/Trash/{files,info}, with a .trashinfo per item
fs::path homeTrash() {
    if (!env("XDG_DATA_HOME").empty()) return fs::path(env("XDG_DATA_HOME")) / "Trash";
    if (!env("HOME").empty()) return fs::path(env("HOME")) / ".local" / "share" / "Trash";
    return {};
}

std::string percentEncode(const std::string& s) {
    std::string out;
    for (unsigned char c : s) {
        if (std::isalnum(c) != 0 || c == '/' || c == '-' || c == '_' || c == '.' || c == '~') {
            out += static_cast<char>(c);
        } else {
            char hex[4];
            std::snprintf(hex, sizeof hex, "%%%02X", c);
            out += hex;
        }
    }
    return out;
}

bool sameDevice(const fs::path& a, const fs::path& b) {
    struct stat sa {}, sb {};
    return ::lstat(a.c_str(), &sa) == 0 && ::stat(b.c_str(), &sb) == 0 && sa.st_dev == sb.st_dev;
}

SystemTrashed trashFreedesktop(const fs::path& abs) {
    const fs::path trash = homeTrash();
    if (trash.empty()) throw std::runtime_error("no home folder for the trash");
    std::error_code ec;
    fs::create_directories(trash / "files", ec);
    fs::create_directories(trash / "info", ec);
    // the home trash only takes files on the same drive (the spec's per-drive trash isn't used)
    if (!sameDevice(abs, trash / "files")) throw std::runtime_error("on another drive than the home trash");
    const std::string base = sysutil::nameOf(abs);
    for (int n = 1; n < 10000; ++n) {
        const std::string name = n == 1 ? base : base + "." + std::to_string(n);
        const fs::path info = trash / "info" / (name + ".trashinfo");
        // made with O_EXCL: the name is ours even when another program trashes at the same time
        const int fd = ::open(info.c_str(), O_WRONLY | O_CREAT | O_EXCL, 0600);
        if (fd < 0) continue;
        if (fs::exists(trash / "files" / name, ec) || fs::is_symlink(trash / "files" / name, ec)) {
            ::close(fd);
            fs::remove(info, ec);
            continue;
        }
        char when[32];
        const std::time_t now = std::time(nullptr);
        std::strftime(when, sizeof when, "%Y-%m-%dT%H:%M:%S", std::localtime(&now));
        const std::string text = "[Trash Info]\nPath=" + percentEncode(abs.string()) + "\nDeletionDate=" + when + "\n";
        const bool wrote = ::write(fd, text.data(), text.size()) == static_cast<ssize_t>(text.size());
        ::close(fd);
        if (!wrote || ::rename(abs.c_str(), (trash / "files" / name).c_str()) != 0) {
            fs::remove(info, ec);
            throw std::runtime_error("couldn't move it into " + (trash / "files").string());
        }
        return {trash / "files" / name, info};
    }
    throw std::runtime_error("the trash has too many items called " + base);
}

#elif defined(__APPLE__)

// The Finder's trash: ~/.Trash. Things land with their own name ("name 2.ext" when taken).
SystemTrashed trashMac(const fs::path& abs) {
    if (env("HOME").empty()) throw std::runtime_error("no home folder for the trash");
    const fs::path trash = fs::path(env("HOME")) / ".Trash";
    std::error_code ec;
    fs::create_directories(trash, ec);
    const std::string stem = abs.stem().string(), ext = abs.extension().string();
    for (int n = 1; n < 10000; ++n) {
        const fs::path to = trash / (n == 1 ? abs.filename().string() : stem + " " + std::to_string(n) + ext);
        if (fs::exists(to, ec) || fs::is_symlink(to, ec)) continue;
        fs::rename(abs, to, ec);
        if (ec) throw std::runtime_error("couldn't move it into the Trash: " + ec.message());
        return {to, {}};
    }
    throw std::runtime_error("the Trash has too many items called " + abs.filename().string());
}

#elif defined(_WIN32)

// Recycle Bin: the shell moves it to <drive>\$Recycle.Bin\<user>\$Rxxxxxx.ext and writes a
// matching $Ixxxxxx.ext holding the original path. Finding that pair lets restore undo it.
std::wstring lowerW(std::wstring s) {
    for (auto& c : s) c = static_cast<wchar_t>(towlower(c));
    return s;
}

std::wstring originalOf(const fs::path& iFile) {
    std::ifstream in(iFile, std::ios::binary);
    std::string data((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    if (data.size() < 24) return {};
    std::int64_t version = 0;
    std::memcpy(&version, data.data(), 8);
    std::size_t at = 24, chars = 0;
    if (version == 2) {
        if (data.size() < 28) return {};
        std::uint32_t n = 0;
        std::memcpy(&n, data.data() + 24, 4);
        at = 28;
        chars = n;
    } else {
        chars = 260;
    }
    std::wstring path;
    for (std::size_t i = 0; i < chars && at + 2 * i + 1 < data.size(); ++i) {
        wchar_t c = static_cast<wchar_t>(static_cast<unsigned char>(data[at + 2 * i]) | (static_cast<unsigned char>(data[at + 2 * i + 1]) << 8));
        if (!c) break;
        path += c;
    }
    return path;
}

SystemTrashed trashWindows(const fs::path& abs) {
    std::wstring from = abs.wstring();
    from.push_back(L'\0');  // the list ends with two NULs
    SHFILEOPSTRUCTW op{};
    op.wFunc = FO_DELETE;
    op.pFrom = from.c_str();
    op.fFlags = FOF_ALLOWUNDO | FOF_NOCONFIRMATION | FOF_SILENT | FOF_NOERRORUI;
    if (SHFileOperationW(&op) != 0 || op.fAnyOperationsAborted) throw std::runtime_error("the Recycle Bin refused it");
    std::error_code ec;
    if (fs::exists(abs, ec)) throw std::runtime_error("the Recycle Bin didn't take it (too big, or the drive has none)");
    // look for the $I record the shell just wrote
    const fs::path bin = abs.root_path() / "$Recycle.Bin";
    const std::wstring want = lowerW(abs.wstring());
    const auto recent = fs::file_time_type::clock::now() - std::chrono::minutes(2);
    for (const auto& user : fs::directory_iterator(bin, fs::directory_options::skip_permission_denied, ec)) {
        std::error_code e2;
        for (const auto& item : fs::directory_iterator(user.path(), fs::directory_options::skip_permission_denied, e2)) {
            const std::wstring name = item.path().filename().wstring();
            if (name.rfind(L"$I", 0) != 0) continue;
            std::error_code e3;
            if (fs::last_write_time(item.path(), e3) < recent) continue;
            if (lowerW(originalOf(item.path())) != want) continue;
            return {user.path() / (L"$R" + name.substr(2)), item.path()};
        }
    }
    return {{}, {}};  // in the Recycle Bin, but restoring is up to the person
}

#endif

}  // namespace

bool systemTrashWanted() { return lowerAscii(env("WETWARESUITE_TRASH")) != "suite"; }

SystemTrashed moveToSystemTrashRaw(const fs::path& abs) {
#if defined(__linux__) || defined(__FreeBSD__) || defined(__OpenBSD__)
    return trashFreedesktop(abs);
#elif defined(__APPLE__)
    return trashMac(abs);
#elif defined(_WIN32)
    return trashWindows(abs);
#else
    (void)abs;
    throw std::runtime_error("no system trash on this platform");
#endif
}

}  // namespace wetwaresuite::explorer
