// Helpers shared by the circuit.* command groups.
#pragma once

#include <map>
#include <cctype>
#include <functional>
#include <cstdio>
#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "wetwaresuite/circuit/circuit.hpp"
#include "wetwaresuite/raster/codec.hpp"

namespace wetwaresuite::circuit::cmd {

namespace fs = std::filesystem;
using Edit = std::function<Result(CircuitDoc&, const Params&)>;

inline bool has(const Params& p, const std::string& key) {
    auto it = p.find(key);
    return it != p.end() && !std::holds_alternative<std::monostate>(it->second);
}

inline std::string num(double v) {
    char b[32];
    std::snprintf(b, sizeof b, "%.3f", v);
    std::string s = b;
    while (s.size() > 1 && s.back() == '0') s.pop_back();
    if (s.back() == '.') s.pop_back();
    if (s == "-0") s = "0";
    return s;
}

inline std::string pt(Pt p) { return num(p.x) + "," + num(p.y); }

inline std::vector<std::string> splitList(const std::string& text, char sep = ',') {
    std::vector<std::string> out;
    std::string item;
    for (char c : text + sep) {
        if (c == sep) {
            const auto a = item.find_first_not_of(" \t"), b = item.find_last_not_of(" \t");
            if (a != std::string::npos) out.push_back(item.substr(a, b - a + 1));
            item.clear();
        } else {
            item += c;
        }
    }
    return out;
}

inline std::string lower(std::string s) {
    for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

// A footprint read from a KiCad board or made in the footprint editor that some part already uses,
// or one from the user footprint library, by name (null if none).
inline std::shared_ptr<const Footprint> importedFootprint(const Circuit& c, const std::string& name) {
    for (const auto& part : c.parts) {
        if (part.customFootprint && part.customFootprint->name == name) return part.customFootprint;
    }
    // then the user's own library (built-in footprints win over a file with the same name)
    if (!name.empty() && !findFootprint(name)) return findUserFootprint(name);
    return nullptr;
}

inline ParamSpec docParam() { return {"doc", ParamType::String, "id of an open circuit, e.g. ckt1", true}; }
inline ParamSpec previewParam() { return {"preview", ParamType::Bool, "attach a PNG of the result (schematic or board)", false, {}, false}; }

inline int snapRot(std::int64_t r) {
    long long v = ((r % 360) + 360) % 360;
    v = (v + 45) / 90 * 90 % 360;
    return static_cast<int>(v);
}

inline Attachment schematicPng(const Circuit& c, const SheetStyle& style = {}, double maxPx = 1400) {
    const pdf::Page page = schematicPage(c, style);
    const double scale = std::clamp(maxPx / std::max(page.width, page.height), 0.5, 3.0);
    return {"image/png", raster::encodePng(pdf::renderPage(page, scale)), "schematic.png"};
}

inline double pcbScale(const Circuit& c, double maxPx = 1200) {
    const double w = std::max(10.0, boardWidth(c.board)), h = std::max(10.0, boardHeight(c.board));
    return std::clamp(maxPx / std::max(w, h), 3.0, 40.0);
}

inline Attachment pcbPng(const Circuit& c, const PcbStyle& style = {}, double maxPx = 1200) {
    const Netlist nl = buildNetlist(c);
    return {"image/png", raster::encodePng(renderPcb(c, nl, pcbScale(c, maxPx), style)), "board.png"};
}

enum class View { Schematic, Pcb };

inline Result& maybePreview(Result& r, const CircuitDoc& doc, const Params& p, View view) {
    if (r.ok && boolParam(p, "preview", false)) r.attach(view == View::Pcb ? pcbPng(doc.circuit) : schematicPng(doc.circuit));
    return r;
}

inline Result edited(const std::string& message, const Params& p) {
    Result r = Result::success(message);
    r.with("doc", stringParam(p, "doc"));
    return r;
}

// Runs `edit` on the document named by "doc": undoable edits snapshot the circuit (one undo step).
inline ActionFactory circuitCommand(Session& session, Edit edit, View view, bool undoable = true) {
    return [&session, edit, view, undoable](const Params& p) -> std::unique_ptr<Action> {
        const std::string id = stringParam(p, "doc");
        auto doc = session.workspace().getAs<CircuitDoc>(id);
        if (!doc) {
            return std::make_unique<FunctionAction>([id] { return Result::failure("no open circuit called '" + id + "' (use circuit.new or circuit.open first)"); });
        }
        auto run = [edit, view](CircuitDoc& d, const Params& q) {
            Result r = edit(d, q);
            return maybePreview(r, d, q, view);
        };
        if (!undoable) {
            return std::make_unique<FunctionAction>([doc, run, p] {
                try {
                    return run(*doc, p);
                } catch (const std::exception& e) {
                    return Result::failure(e.what());
                }
            });
        }
        return std::make_unique<CircuitEdit>(doc, [run, p](CircuitDoc& d) { return run(d, p); });
    };
}

inline bool writeFile(const fs::path& path, const std::string& data, std::string& error) {
    if (auto parent = path.parent_path(); !parent.empty()) {
        std::error_code ec;
        fs::create_directories(parent, ec);
    }
    std::ofstream out(path, std::ios::binary);
    out << data;
    if (!out) {
        error = "can't write " + path.string();
        return false;
    }
    return true;
}

inline std::string readFile(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) throw std::runtime_error("can't read " + path.string());
    std::stringstream s;
    s << in.rdbuf();
    return s.str();
}

inline std::string issuesText(const std::vector<Issue>& issues, int& errors, int& warnings) {
    errors = warnings = 0;
    std::string s;
    for (const auto& i : issues) {
        if (i.severity == "error") ++errors;
        else if (i.severity == "warning") ++warnings;
        s += "\n" + i.severity + " [" + i.code + "] " + i.message;
    }
    return s;
}

void registerFileCommands(Session& session);
void registerSchematicCommands(Session& session);
void registerSchematicMarkCommands(Session& session);
void registerSchematicQueryCommands(Session& session);
void registerBusCommands(Session& session);
void registerPcbCommands(Session& session);
void registerPcbOutputCommands(Session& session);
void registerBoardToolCommands(Session& session);
void registerViewCommands(Session& session);
void registerFootprintEditCommands(Session& session);

}  // namespace wetwaresuite::circuit::cmd
