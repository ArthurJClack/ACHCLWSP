// Footprint editor pieces: footprints generated from datasheet numbers, and the user's own
// footprint library (a folder of KiCad .kicad_mod files, so KiCad can share it).
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <map>
#include <mutex>
#include <sstream>

#include "wetwaresuite/circuit/circuit.hpp"
#include "wetwaresuite/core/process.hpp"

namespace wetwaresuite::circuit {

namespace fs = std::filesystem;

namespace {

void box(Footprint& f, double x0, double y0, double x1, double y1) {
    f.silk.push_back({{x0, y0}, {x1, y0}});
    f.silk.push_back({{x1, y0}, {x1, y1}});
    f.silk.push_back({{x1, y1}, {x0, y1}});
    f.silk.push_back({{x0, y1}, {x0, y0}});
}

// pin 1 marker: a short tick outside the body, next to pad 1
void pinOneMark(Footprint& f) {
    if (f.pads.empty()) return;
    const PadDef& p = f.pads.front();
    const double x = p.pos.x - p.w / 2 - 0.4;
    f.silk.push_back({{x, p.pos.y - 0.3}, {x, p.pos.y + 0.3}});
}

std::string safeName(std::string s) {
    for (auto& ch : s)
        if (ch == '/' || ch == '\\' || ch == ':' || ch == '*' || ch == '?' || ch == '"' || ch == '<' || ch == '>' || ch == '|') ch = '_';
    return s;
}

std::string mm(double v) {
    char b[32];
    std::snprintf(b, sizeof b, "%g", std::round(v * 1000) / 1000);
    return b;
}

std::string lowered(std::string s) {
    for (auto& ch : s) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    return s;
}

std::string rowName(int i) {  // BGA rows: A, B, ..., skipping I, O, Q, S, X, Z as JEDEC does
    static const std::string letters = "ABCDEFGHJKLMNPRTUVWY";
    std::string s;
    for (++i; i > 0; i = (i - 1) / static_cast<int>(letters.size())) s.insert(s.begin(), letters[static_cast<std::size_t>((i - 1) % letters.size())]);
    return s;
}

}  // namespace

Footprint makeFootprint(const FootprintSpec& sp) {
    Footprint f;
    f.name = sp.name;
    const std::string kind = lowered(sp.kind);
    const int n = std::max(1, sp.pins);
    const double p = sp.pitch > 0 ? sp.pitch : 1.27;
    const PadShape firstShape = sp.drill > 0 ? PadShape::Rect : sp.shape;
    auto pad = [&](int i, Pt pos, double w, double h) {
        f.pads.push_back({std::to_string(i + 1), pos, w, h, i == 0 ? firstShape : sp.shape, sp.drill});
    };
    if (kind == "smd2") {  // a two-pad chip: pitch is pad centre to pad centre
        pad(0, {-p / 2, 0}, sp.padW, sp.padH);
        pad(1, {p / 2, 0}, sp.padW, sp.padH);
        f.pads[0].shape = sp.shape;
        const double gx = p / 2 - sp.padW / 2 - 0.15;
        if (gx > 0.1) {
            f.silk.push_back({{-gx, -sp.padH / 2}, {gx, -sp.padH / 2}});
            f.silk.push_back({{-gx, sp.padH / 2}, {gx, sp.padH / 2}});
        }
        f.description = "2 pads, " + mm(p) + " mm apart";
    } else if (kind == "row") {  // single row (headers, connectors)
        for (int i = 0; i < n; ++i) pad(i, {0, (i - (n - 1) / 2.0) * p}, sp.padW, sp.padH);
        const double hx = std::max(sp.padW, sp.padH) / 2 + 0.3, hy = n * p / 2;
        box(f, -hx, -hy, hx, hy);
        f.description = std::to_string(n) + " pads in a row, " + mm(p) + " mm pitch";
    } else if (kind == "dual") {  // SOIC/DIP style: 1..n/2 down the left, the rest up the right
        const int half = (n + 1) / 2;
        for (int i = 0; i < n; ++i) {
            const bool left = i < half;
            const int row = left ? i : n - 1 - i;
            pad(i, {left ? -sp.span / 2 : sp.span / 2, (row - (half - 1) / 2.0) * p}, sp.padH, sp.padW);
        }
        const double bx = std::max(0.3, sp.span / 2 - sp.padH / 2 - 0.25), by = half * p / 2 + 0.1;
        box(f, -bx, -by, bx, by);
        pinOneMark(f);
        f.description = std::to_string(n) + " pads in two rows " + mm(sp.span) + " mm apart, " + mm(p) + " mm pitch";
    } else if (kind == "quad") {  // QFP/QFN style, counter-clockwise from the top of the left side
        const int side = std::max(1, n / 4);
        for (int i = 0; i < side * 4; ++i) {
            const int s = i / side, k = i % side;
            const double off = (k - (side - 1) / 2.0) * p;
            if (s == 0) pad(i, {-sp.span / 2, off}, sp.padH, sp.padW);
            else if (s == 1) pad(i, {off, sp.span / 2}, sp.padW, sp.padH);
            else if (s == 2) pad(i, {sp.span / 2, -off}, sp.padH, sp.padW);
            else pad(i, {-off, -sp.span / 2}, sp.padW, sp.padH);
        }
        if (sp.exposed > 0) f.pads.push_back({std::to_string(side * 4 + 1), {0, 0}, sp.exposed, sp.exposed, PadShape::Rect, 0});
        // corner marks outside the pad ring, stopping short of the end pads
        const double b = sp.span / 2 + sp.padH / 2 + 0.2;
        const double c = std::max(0.2, b - ((side - 1) / 2.0 * p + sp.padW / 2) - 0.25);
        f.silk.push_back({{-b + c, -b}, {-b, -b + c}});
        for (int sx : {-1, 1})
            for (int sy : {-1, 1}) {
                if (sx < 0 && sy < 0) continue;
                f.silk.push_back({{sx * b, sy * b}, {sx * (b - c), sy * b}});
                f.silk.push_back({{sx * b, sy * b}, {sx * b, sy * (b - c)}});
            }
        pinOneMark(f);
        f.description = std::to_string(side * 4) + " pads on four sides, " + mm(p) + " mm pitch";
    } else if (kind == "grid") {  // BGA: balls named A1, A2, ... B1, ...
        const int cols = sp.columns > 0 ? sp.columns : static_cast<int>(std::ceil(std::sqrt(static_cast<double>(n))));
        const int rows = (n + cols - 1) / cols;
        const double d = sp.padW > 0 ? sp.padW : p / 2;
        for (int i = 0; i < n; ++i) {
            const int r = i / cols, k = i % cols;
            f.pads.push_back({rowName(r) + std::to_string(k + 1), {(k - (cols - 1) / 2.0) * p, (r - (rows - 1) / 2.0) * p}, d, d, PadShape::Circle, 0});
        }
        const double hx = cols * p / 2, hy = rows * p / 2;
        box(f, -hx, -hy, hx, hy);
        f.silk.push_back({{-hx - 0.5, -hy}, {-hx, -hy - 0.5}});
        f.description = std::to_string(n) + " balls, " + std::to_string(rows) + " x " + std::to_string(cols) + ", " + mm(p) + " mm pitch";
    } else if (kind == "blank" || kind.empty()) {
        f.description = "drawn by hand";
    } else {
        throw std::runtime_error("unknown footprint kind '" + sp.kind + "' (smd2, row, dual, quad, grid or blank)");
    }
    finishFootprint(f);
    return f;
}

fs::path userFootprintDir() {
    if (const char* env = std::getenv("WETWARESUITE_FOOTPRINTS"); env && *env) {
        const std::string s = env;
        return fs::path(std::u8string(s.begin(), s.end()));
    }
    return suiteToolsDir().parent_path() / "footprints";
}

namespace {
std::mutex cacheMutex;
std::map<std::string, std::pair<fs::file_time_type, std::shared_ptr<const Footprint>>> cache;
}  // namespace

std::shared_ptr<const Footprint> findUserFootprint(const std::string& nameIn) {
    std::string name = nameIn;
    if (auto colon = name.find(':'); colon != std::string::npos) name = name.substr(colon + 1);
    if (name.empty()) return nullptr;
    std::error_code ec;
    const fs::path dir = userFootprintDir();
    fs::path file = dir / (safeName(name) + ".kicad_mod");
    if (!fs::is_regular_file(file, ec)) {
        // case-insensitive match
        file.clear();
        for (const auto& e : fs::directory_iterator(dir, ec)) {
            if (e.path().extension() == ".kicad_mod" && lowered(e.path().stem().string()) == lowered(safeName(name))) file = e.path();
        }
        if (file.empty()) return nullptr;
    }
    const auto stamp = fs::last_write_time(file, ec);
    std::lock_guard lock(cacheMutex);
    auto& slot = cache[file.string()];
    if (slot.second && slot.first == stamp) return slot.second;
    std::ifstream in(file, std::ios::binary);
    std::stringstream ss;
    ss << in.rdbuf();
    try {
        auto fp = fromKicadMod(ss.str());
        if (fp && fp->name != file.stem().string()) {
            Footprint copy = *fp;
            copy.name = file.stem().string();
            fp = std::make_shared<const Footprint>(std::move(copy));
        }
        slot = {stamp, fp};
        return fp;
    } catch (const std::exception&) {
        return nullptr;
    }
}

std::vector<std::string> userFootprints() {
    std::vector<std::string> out;
    std::error_code ec;
    for (const auto& e : fs::directory_iterator(userFootprintDir(), ec))
        if (e.path().extension() == ".kicad_mod") out.push_back(e.path().stem().string());
    std::sort(out.begin(), out.end());
    return out;
}

fs::path saveUserFootprint(const Footprint& fp) {
    const fs::path dir = userFootprintDir();
    std::error_code ec;
    fs::create_directories(dir, ec);
    const fs::path file = dir / (safeName(fp.name) + ".kicad_mod");
    std::ofstream out(file, std::ios::binary);
    if (!out) throw std::runtime_error("can't write " + file.string());
    out << toKicadMod(fp);
    if (!out) throw std::runtime_error("can't write " + file.string());
    return file;
}

}  // namespace wetwaresuite::circuit
