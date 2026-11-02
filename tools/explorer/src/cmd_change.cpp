// explorer.* commands that change files: copy, move, rename, delete (to the
// trash), new folder and file, rename many at once, and the trash itself.
// Every one can be undone; undo moves what it made to the trash.
#include <regex>
#include <set>

#include "internal.hpp"

namespace wetwaresuite::explorer::detail {

namespace {

std::string names(const std::vector<fs::path>& ps) {
    std::string out;
    for (std::size_t i = 0; i < ps.size() && i < 5; ++i) out += (i ? ", " : "") + sysutil::nameOf(ps[i]);
    if (ps.size() > 5) out += " and " + std::to_string(ps.size() - 5) + " more";
    return out;
}

void undoByTrash(UndoList& undo, const fs::path& made) {
    undo.push_back([made] {
        std::error_code ec;
        if (fs::exists(made, ec)) moveToTrash(made);
    });
}

fs::path destFolder(const Params& p) {
    fs::path to = pathParam(p, "to");
    std::error_code ec;
    if (!fs::is_directory(to, ec)) {
        if (!boolParam(p, "make_folder", true) || fs::exists(to, ec)) throw std::runtime_error("not a folder: " + stringParam(p, "to"));
        fs::create_directories(to, ec);
        if (ec) throw std::runtime_error("can't make folder " + stringParam(p, "to"));
    }
    return to;
}

void registerCopyMove(Session& s) {
    auto& reg = s.registry();
    reg.add({"explorer.copy", "Copy",
             "Copy files or folders into a folder. A name that is taken gets \" (2)\" added unless overwrite=true. Undo moves "
             "the copies to the trash.",
             {pathsParam(), req("to", ParamType::String, "the folder to copy into (made if missing)"),
              opt("overwrite", ParamType::Bool, "replace files with the same name (the old ones go to the trash)", false)},
             true},
            changer([](const Params& p, UndoList& undo) {
                auto from = splitPaths(stringParam(p, "paths"));
                const fs::path to = destFolder(p);
                const bool overwrite = boolParam(p, "overwrite", false);
                std::vector<fs::path> made;
                for (const auto& f : from) {
                    fs::path target = to / f.filename();
                    std::error_code ec;
                    if (fs::equivalent(f, target, ec) || (fs::exists(target, ec) && !overwrite)) target = freeName(target);
                    else if (fs::exists(target, ec)) {
                        std::string id = moveToTrash(target);
                        undo.push_back([id, target] { restoreFromTrash(id, target); });
                    }
                    copyAny(f, target, false);
                    undoByTrash(undo, target);
                    made.push_back(target);
                }
                std::string rows;
                for (const auto& m : made) rows += sysutil::fromPath(m) + "\n";
                return Result::success("copied " + names(from) + " to " + sysutil::fromPath(to)).with("count", static_cast<int64_t>(made.size())).with("rows", rows);
            }));

    reg.add({"explorer.move", "Move", "Move files or folders into another folder. Undo moves them back.",
             {pathsParam(), req("to", ParamType::String, "the folder to move into (made if missing)")}, true},
            changer([](const Params& p, UndoList& undo) {
                auto from = splitPaths(stringParam(p, "paths"));
                const fs::path to = destFolder(p);
                std::string rows;
                for (const auto& f : from) {
                    const fs::path target = to / f.filename();
                    moveAny(f, target);
                    undo.push_back([f, target] { moveAny(target, f); });
                    rows += sysutil::fromPath(target) + "\n";
                }
                return Result::success("moved " + names(from) + " to " + sysutil::fromPath(to)).with("count", static_cast<int64_t>(from.size())).with("rows", rows);
            }));

    reg.add({"explorer.rename", "Rename", "Give a file or folder a new name (in the same folder).",
             {req("path", ParamType::String, "file or folder"), req("name", ParamType::String, "the new name")}, true},
            changer([](const Params& p, UndoList& undo) {
                const fs::path from = pathParam(p, "path");
                const std::string name = sysutil::trim(stringParam(p, "name"));
                if (name.empty() || name.find_first_of("/\\") != std::string::npos || name == "." || name == "..")
                    throw std::runtime_error("a name can't be empty or have / or \\ in it");
                const fs::path to = from.parent_path() / sysutil::toPath(name);
                moveAny(from, to);
                undo.push_back([from, to] { moveAny(to, from); });
                return Result::success("renamed to " + name).with("path", sysutil::fromPath(to));
            }));

    reg.add({"explorer.delete", "Delete",
             "Move files or folders to the trash: the computer's own (Recycle Bin, macOS Trash, the desktop trash on Linux) "
             "when it can take them, else the suite's. explorer.trash lists both, undo or explorer.restore brings them back. "
             "system=false always uses the suite's trash (WETWARESUITE_TRASH=suite does that everywhere).",
             {pathsParam(), opt("system", ParamType::Bool, "use the computer's trash when it can take them", true)}, true},
            changer([](const Params& p, UndoList& undo) {
                auto paths = splitPaths(stringParam(p, "paths"));
                std::string rows;
                int64_t inSystem = 0;
                for (const auto& f : paths) {
                    std::error_code ec;
                    const fs::path abs = fs::absolute(f, ec).lexically_normal();
                    bool sys = false;
                    const std::string id = boolParam(p, "system", true) ? moveToSystemTrash(abs, &sys) : moveToTrash(abs);
                    inSystem += sys ? 1 : 0;
                    undo.push_back([id, abs] { restoreFromTrash(id, abs); });
                    rows += id + "\t" + sysutil::fromPath(abs) + "\t" + (sys ? "system" : "suite") + "\n";
                }
                const int64_t n = static_cast<int64_t>(paths.size());
                const std::string where = inSystem == n ? "the computer's trash" : inSystem == 0 ? "the suite's trash" : "the trash (some to the computer's, some to the suite's)";
                return Result::success("moved " + names(paths) + " to " + where).with("count", n).with("system", inSystem).with("rows", rows);
            }));

    reg.add({"explorer.new_folder", "New folder", "Make a folder (and any missing folders above it).",
             {req("path", ParamType::String, "the new folder")}, true},
            changer([](const Params& p, UndoList& undo) {
                const fs::path dir = pathParam(p, "path");
                std::error_code ec;
                if (fs::exists(dir, ec)) throw std::runtime_error(stringParam(p, "path") + " already exists");
                fs::path top = dir;  // the highest folder that didn't exist: undo takes that one away
                while (top.has_parent_path() && top.parent_path() != top && !fs::exists(top.parent_path(), ec)) top = top.parent_path();
                fs::create_directories(dir, ec);
                if (ec) throw std::runtime_error("can't make " + stringParam(p, "path") + ": " + ec.message());
                undoByTrash(undo, top);
                return Result::success("made " + sysutil::fromPath(dir)).with("path", sysutil::fromPath(dir));
            }));

    reg.add({"explorer.new_file", "New file", "Make a new file, empty or with some text in it.",
             {req("path", ParamType::String, "the new file"), opt("text", ParamType::String, "what to put in it"),
              opt("overwrite", ParamType::Bool, "replace a file that is there (it goes to the trash)", false)},
             true},
            changer([](const Params& p, UndoList& undo) {
                const fs::path file = pathParam(p, "path");
                std::error_code ec;
                if (fs::exists(file, ec)) {
                    if (!boolParam(p, "overwrite", false)) throw std::runtime_error(stringParam(p, "path") + " already exists");
                    const fs::path abs = fs::absolute(file, ec);
                    std::string id = moveToTrash(abs);
                    undo.push_back([id, abs] { restoreFromTrash(id, abs); });
                }
                if (file.has_parent_path()) fs::create_directories(file.parent_path(), ec);
                sysutil::writeFile(file, stringParam(p, "text"));
                undoByTrash(undo, fs::absolute(file, ec));
                return Result::success("made " + sysutil::fromPath(file)).with("path", sysutil::fromPath(file));
            }));
}

void registerBatchAndTrash(Session& s) {
    auto& reg = s.registry();
    reg.add({"explorer.batch_rename", "Rename many",
             "Rename every file in a folder whose name matches: replace text (or a regular expression, with $1 for groups) "
             "in the names, and/or number them. dry_run=true shows the new names first.",
             {opt("folder", ParamType::String, "the folder (default: home)"), opt("files", ParamType::String, "only names like these, e.g. *.jpg"),
              opt("find", ParamType::String, "text in the name to replace"), opt("replace", ParamType::String, "what to put instead", std::string()),
              opt("regex", ParamType::Bool, "find is a regular expression", false),
              opt("number", ParamType::String, "rename to this with a counter, e.g. \"holiday {n}\" (keeps the extension; {n:3} pads to 001)"),
              opt("start", ParamType::Int, "first number", int64_t{1}), opt("dry_run", ParamType::Bool, "only show the new names", false)},
             true},
            changer([](const Params& p, UndoList& undo) {
                const fs::path dir = pathParam(p, "folder");
                const std::string files = stringParam(p, "files"), find = stringParam(p, "find"), numberPat = stringParam(p, "number");
                if (find.empty() && numberPat.empty()) throw std::runtime_error("give find= (with replace=) or number=");
                std::regex re;
                if (!find.empty()) {
                    std::string pat = find;
                    if (!boolParam(p, "regex", false)) pat = std::regex_replace(find, std::regex(R"([.^$|()\[\]{}*+?\\])"), R"(\$&)");
                    re = std::regex(pat, std::regex::ECMAScript);
                }
                std::string with = stringParam(p, "replace");
                if (!boolParam(p, "regex", false)) with = std::regex_replace(with, std::regex(R"(\$)"), "$$$$");
                std::vector<std::pair<fs::path, fs::path>> plan;
                int64_t counter = intParam(p, "start", 1);
                for (const auto& e : listFolder(dir, false)) {
                    if (e.dir || (!files.empty() && !wildcard(files, e.name))) continue;
                    std::string name = e.name;
                    if (!find.empty()) name = std::regex_replace(name, re, with);
                    if (!numberPat.empty()) {
                        std::string ext = sysutil::fromPath(sysutil::toPath(e.name).extension());
                        std::smatch m;
                        std::string out = numberPat;
                        static const std::regex slot(R"(\{n(?::(\d+))?\})");
                        if (std::regex_search(out, m, slot)) {
                            std::string num = std::to_string(counter);
                            const std::size_t width = m[1].matched ? std::stoul(m[1].str()) : 0;
                            if (num.size() < width) num = std::string(width - num.size(), '0') + num;
                            out = m.prefix().str() + num + m.suffix().str();
                        } else {
                            out += " " + std::to_string(counter);
                        }
                        ++counter;
                        // "beach {n}.jpg" already has the extension: don't add it twice
                        const bool hasExt = !ext.empty() && out.size() >= ext.size() &&
                                            sysutil::lowerAscii(out.substr(out.size() - ext.size())) == sysutil::lowerAscii(ext);
                        name = hasExt ? out : out + ext;
                    }
                    if (name == e.name) continue;
                    if (name.empty() || name.find_first_of("/\\") != std::string::npos) throw std::runtime_error("'" + e.name + "' would get a bad name: '" + name + "'");
                    plan.emplace_back(sysutil::toPath(e.path), dir / sysutil::toPath(name));
                }
                std::set<fs::path> targets;
                for (const auto& [a, b] : plan) {
                    if (!targets.insert(b).second) throw std::runtime_error("two files would both be called " + sysutil::nameOf(b));
                }
                std::string text, rows;
                for (const auto& [a, b] : plan) {
                    text += sysutil::nameOf(a) + " -> " + sysutil::nameOf(b) + "\n";
                    rows += sysutil::fromPath(a) + "\t" + sysutil::fromPath(b) + "\n";
                }
                if (plan.empty()) return Result::success("no names would change").with("count", int64_t{0});
                if (boolParam(p, "dry_run", false)) return Result::success("would rename " + sysutil::plural(plan.size(), "file") + "\n" + text).with("count", static_cast<int64_t>(plan.size())).with("rows", rows);
                // two passes through temporary names, so a -> b, b -> c works
                std::vector<std::pair<fs::path, fs::path>> temps;
                for (std::size_t i = 0; i < plan.size(); ++i) {
                    fs::path tmp = dir / sysutil::toPath(".ww-rename-" + std::to_string(i) + "-" + sysutil::nameOf(plan[i].first));
                    moveAny(plan[i].first, tmp);
                    undo.push_back([tmp, orig = plan[i].first] { moveAny(tmp, orig); });
                    temps.emplace_back(tmp, plan[i].second);
                }
                for (const auto& [tmp, target] : temps) {
                    moveAny(tmp, target);
                    undo.push_back([tmp, target] { moveAny(target, tmp); });
                }
                return Result::success("renamed " + sysutil::plural(plan.size(), "file") + "\n" + text).with("count", static_cast<int64_t>(plan.size())).with("rows", rows);
            }));

    reg.add({"explorer.trash", "Trash", "What the suite has deleted: id, where it was, when, its size, and whether it sits in the computer's trash or the suite's.", {}, false, true},
            reader([](const Params&) {
                auto items = trashItems();
                std::string text, rows;
                std::uint64_t total = 0;
                for (const auto& t : items) {
                    total += t.size;
                    text += t.id + "  " + t.original + (t.dir ? "/" : "") + "  (" + sysutil::humanSize(t.size) + ", deleted " + formatTime(t.deleted) +
                            (t.system.empty() ? "" : ", in the computer's trash") + ")\n";
                    rows += t.id + "\t" + t.original + "\t" + std::to_string(t.deleted) + "\t" + std::to_string(t.size) + "\t" + (t.dir ? "dir" : "file") + "\t" +
                            (t.system.empty() ? "suite" : "system") + "\n";
                }
                if (items.empty()) text = "the trash is empty\n";
                return Result::success(text)
                    .with("count", static_cast<int64_t>(items.size()))
                    .with("bytes", static_cast<int64_t>(total))
                    .with("folder", sysutil::fromPath(trashFolder()))
                    .with("rows", rows);
            }));

    reg.add({"explorer.restore", "Restore", "Put something from the trash back where it was (or to=).",
             {req("id", ParamType::String, "trash id (explorer.trash lists them)"), opt("to", ParamType::String, "restore here instead")}, true},
            changer([](const Params& p, UndoList& undo) {
                const fs::path to = stringParam(p, "to").empty() ? fs::path() : pathParam(p, "to");
                const fs::path where = restoreFromTrash(stringParam(p, "id"), to);
                undoByTrash(undo, where);
                return Result::success("restored " + sysutil::fromPath(where)).with("path", sysutil::fromPath(where));
            }));

    reg.add({"explorer.empty_trash", "Empty trash",
             "Delete what the suite put in the trash for good (cannot be undone), including what it put in the computer's trash: everything, or what was deleted more than older_days ago.",
             {opt("older_days", ParamType::Number, "only items deleted longer ago than this", 0.0)}, false},
            reader([](const Params& p) {
                std::size_t n = emptyTrash(numberParam(p, "older_days", 0));
                return Result::success("deleted " + sysutil::plural(n, "item") + " for good").with("count", static_cast<int64_t>(n));
            }));
}

}  // namespace

void registerChangeCommands(Session& s) {
    registerCopyMove(s);
    registerBatchAndTrash(s);
}

}  // namespace wetwaresuite::explorer::detail
