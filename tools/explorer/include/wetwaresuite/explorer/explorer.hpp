// explorer: a file manager you can drive headless. Browse folders, copy, move,
// rename and delete (to the suite's own trash, so every change can be undone),
// search by name, content, size and date, preview files, compare folders, find
// duplicates, check sums, and make or unpack zip, tar and tar.gz archives.
// Nothing here needs another program, except opening a file in its own app.
#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include "wetwaresuite/core/command.hpp"

namespace wetwaresuite::explorer {

namespace fs = std::filesystem;

struct Entry {
    std::string name;
    std::string path;  // UTF-8, forward slashes
    bool dir = false;
    bool link = false;
    bool hidden = false;
    std::uint64_t size = 0;    // bytes; folders: 0
    std::int64_t modified = 0; // seconds since 1970 (UTC)
    std::string type;          // "folder", "PNG image", ...
};

// Folders first, then by `sort`: name, size, date or type. Hidden entries only when asked.
std::vector<Entry> listFolder(const fs::path& dir, bool hidden = false, const std::string& sort = "name", bool descending = false);
std::int64_t modifiedTime(const fs::path& p);  // seconds since 1970, 0 when unknown
std::string formatTime(std::int64_t unixSeconds);  // "2026-10-16 14:05" local time

// Home, Desktop, Documents, Downloads, Pictures, Music, Videos, the suite's data
// folder, the temporary folder and every drive or mount.
std::vector<std::pair<std::string, fs::path>> places();

// Opens a file or folder in the program the system uses for it.
bool launch(const fs::path& p, std::string& error);

// ---- checksums ------------------------------------------------------------------------

std::uint32_t crc32(const void* data, std::size_t size, std::uint32_t crc = 0);
std::string md5Hex(const std::string& bytes);
std::string sha1Hex(const std::string& bytes);
std::string sha256Hex(const std::string& bytes);
// algorithm: crc32, md5, sha1, sha256. Reads in pieces, so big files are fine.
std::string checksumFile(const fs::path& p, const std::string& algorithm);

// ---- archives ---------------------------------------------------------------------------

struct ArchiveItem {
    std::string name;  // path inside the archive, forward slashes; folders end in '/'
    std::uint64_t size = 0;
    std::uint64_t packed = 0;
    bool dir = false;
    std::int64_t modified = 0;
};

// "zip", "tar", "tar.gz" or "gz" from the name and first bytes, "" when it isn't one.
std::string archiveKind(const fs::path& p);
std::vector<ArchiveItem> archiveList(const fs::path& archive);
// Packs files and folders (folders with everything in them), stored relative to
// their parent folder. kind: zip, tar or tar.gz. level 0 stores without compressing.
// Returns how many files went in.
std::size_t archiveCreate(const fs::path& output, const std::vector<fs::path>& inputs, const std::string& kind, int level = 6);
// Unpacks into `dest` (made if needed); `only` limits it to these names or folders.
// Names that would land outside `dest` (../, absolute) are refused. Returns the
// files and folders written, in order.
std::vector<fs::path> archiveExtract(const fs::path& archive, const fs::path& dest, const std::vector<std::string>& only = {},
                                     bool overwrite = false);

// ---- trash -------------------------------------------------------------------------------

struct TrashItem {
    std::string id;
    std::string original;  // where it was
    std::int64_t deleted = 0;
    std::uint64_t size = 0;
    bool dir = false;
    std::string system;  // in the computer's own trash: where (empty: in the suite's trash folder)
};
fs::path trashFolder();  // <data folder>/trash
// Moves a file or folder into the trash and returns its id.
std::string moveToTrash(const fs::path& p);
// The computer's own trash (Recycle Bin, macOS Trash, freedesktop trash): where the item went and
// its record there (Linux .trashinfo, Windows $I file). The item is empty when the Recycle Bin took
// it but couldn't be found again (restore then has to happen from the Recycle Bin).
struct SystemTrashed {
    fs::path item, record;
};
SystemTrashed moveToSystemTrashRaw(const fs::path& abs);
bool systemTrashWanted();  // false when WETWARESUITE_TRASH=suite
// Moves into the computer's trash, falling back to the suite's trash when it can't take it (another
// drive, no desktop). Returns an id like moveToTrash: explorer.trash lists it, restore puts it back.
std::string moveToSystemTrash(const fs::path& p, bool* usedSystem = nullptr);
// Puts it back where it was (or at `to`); returns where it went.
fs::path restoreFromTrash(const std::string& id, const fs::path& to = {});
std::vector<TrashItem> trashItems();
// Deletes for good: everything, or items deleted more than `olderThanDays` ago. Returns how many.
std::size_t emptyTrash(double olderThanDays = 0);

// Copies a file or a folder with everything in it. Throws when the target exists and !overwrite.
void copyAny(const fs::path& from, const fs::path& to, bool overwrite);
// Renames, or copies and deletes when it has to cross drives.
void moveAny(const fs::path& from, const fs::path& to);
// "name (2).txt" when "name.txt" is taken
fs::path freeName(const fs::path& wanted);
// total bytes and files under a folder (or a file's size)
std::uint64_t sizeOf(const fs::path& p, std::uint64_t* files = nullptr);

void registerCommands(Session& session);

}  // namespace wetwaresuite::explorer
