// Shared command plumbing, file loading/saving per kind, the LibreOffice
// backend, office.convert and office.formats.
#include "wetwaresuite/office/office.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <fstream>
#include <set>
#include <sstream>

#include "wetwaresuite/core/process.hpp"
#include "wetwaresuite/office/chart.hpp"
#include "wetwaresuite/pdf/reader.hpp"
#include "wetwaresuite/raster/codec.hpp"
#include "commands.hpp"
#include "text.hpp"
#include "xml.hpp"

namespace wetwaresuite::office {

// ---- documents -------------------------------------------------------------------------

namespace {

std::string bytesText(std::size_t n) {
    char b[32];
    if (n >= 1024 * 1024) std::snprintf(b, sizeof b, "%.1f MB", n / 1048576.0);
    else if (n >= 1024) std::snprintf(b, sizeof b, "%.0f KB", n / 1024.0);
    else std::snprintf(b, sizeof b, "%zu bytes", n);
    return b;
}

}  // namespace

std::string SheetDoc::summary() const {
    std::string s = std::to_string(data.sheets.size()) + (data.sheets.size() == 1 ? " sheet" : " sheets");
    std::string list;
    for (std::size_t i = 0; i < data.sheets.size() && i < 6; ++i) {
        const Sheet& sh = data.sheets[i];
        int r1, c1, r2, c2;
        list += (list.empty() ? "" : ", ") + sh.name;
        if (sh.bounds(r1, c1, r2, c2)) list += " " + cellName(0, 0) + ":" + cellName(r2, c2);
        else list += " (empty)";
    }
    if (data.sheets.size() > 6) list += ", ...";
    s += ": " + list;
    if (!path.empty()) s += " - " + path;
    return s;
}

bool SheetDoc::grid(CellGrid& out) const {
    painter_.reset();
    painterSheet_ = -1;
    out.docParam = "doc";
    out.sheets.clear();
    out.frozen.clear();
    for (const auto& sh : data.sheets) out.sheets.push_back(sh.name), out.frozen.emplace_back(sh.freezeRows, sh.freezeCols);
    out.active = data.sheets.empty() ? 0 : std::clamp(data.active, 0, static_cast<int>(data.sheets.size()) - 1);
    out.usedRows = out.usedCols = 0;
    if (!data.sheets.empty()) {
        int r1, c1, r2, c2;
        if (data.sheets[static_cast<std::size_t>(out.active)].bounds(r1, c1, r2, c2)) out.usedRows = r2 + 1, out.usedCols = c2 + 1;
    }
    return true;
}

bool SheetDoc::gridCell(int sheet, int row, int col, GridCell& out) const {
    if (sheet < 0 || sheet >= static_cast<int>(data.sheets.size())) return false;
    const Sheet& sh = data.sheets[static_cast<std::size_t>(sheet)];
    bool merged = false;
    for (const auto& m : sh.merges) {
        if (row < m.r1 || row > m.r2 || col < m.c1 || col > m.c2) continue;
        if (row != m.r1 || col != m.c1) {
            out.covered = true;
            return true;
        }
        out.spanRows = m.r2 - m.r1 + 1;
        out.spanCols = m.c2 - m.c1 + 1;
        merged = true;
    }
    const Cell* c = sh.get(row, col);
    if ((!c || c->blank()) && !merged && sh.condFormats.empty()) return false;
    CellStyle st = c ? c->style : CellStyle{};
    if (!sh.condFormats.empty()) {
        if (!painter_ || painterSheet_ != sheet) {
            painter_ = std::make_shared<CellPainter>(data, sheet);
            painterSheet_ = sheet;
        }
        double bar = -1;
        std::uint32_t barColor = 0;
        st = painter_->style(row, col, &bar, &barColor);
        out.bar = static_cast<float>(bar);
        out.barColor = barColor;
    }
    if (!c && !merged && st.plain() && out.bar < 0) return false;
    if (c) {
        out.text = formatValue(c->value, c->format);
        out.input = inputText(*c);
        out.number = c->value.type == CellValue::Number;
        out.error = c->value.isError();
        const std::uint32_t fc = formatColor(c->value, c->format);
        if (fc && !hasColor(st.color)) st.color = fc;
    }
    out.bold = st.bold;
    out.italic = st.italic;
    out.underline = st.underline;
    out.strike = st.strike;
    out.align = st.align;
    out.color = st.color;
    out.fill = st.fill;
    return true;
}

double SheetDoc::gridColumnChars(int sheet, int col) const {
    if (sheet < 0 || sheet >= static_cast<int>(data.sheets.size())) return 10;
    const auto& w = data.sheets[static_cast<std::size_t>(sheet)].colWidths;
    auto it = w.find(col);
    return it == w.end() ? 10 : std::clamp(it->second, 2.0, 80.0);
}

std::vector<std::string> SheetDoc::gridSetCommand(int sheet, int row, int col, const std::string& input) const {
    if (sheet < 0 || sheet >= static_cast<int>(data.sheets.size()) || row < 0 || col < 0 || row >= kMaxRows || col >= kMaxCols) return {};
    const std::string where = "range=" + cellName(row, col), which = "sheet=" + std::to_string(sheet + 1);
    if (input.empty()) return {"office.sheet_clear", where, which, "what=contents"};
    if (input.find_first_of("\t\n") != std::string::npos) return {"office.sheet_set", where, which, "csv=" + input};
    return {"office.sheet_set", where, which, "value=" + input};
}

std::unique_ptr<Attachment> SheetDoc::preview() const {
    if (data.sheets.empty()) return nullptr;
    const int si = std::clamp(data.active, 0, static_cast<int>(data.sheets.size()) - 1);
    Range r;
    r.sheet = si;
    int r1, c1, r2, c2;
    if (data.sheets[static_cast<std::size_t>(si)].bounds(r1, c1, r2, c2)) {
        r.r2 = std::min(r2, 39);
        r.c2 = std::min(c2, 14);
    } else {
        r.r2 = 9;
        r.c2 = 5;
    }
    r.r2 = std::max(r.r2, 4);
    r.c2 = std::max(r.c2, 3);
    return std::make_unique<Attachment>(png(pdf::renderPage(sheetGridPage(data, r, true), 1.5), "sheet.png"));
}

std::string TextDoc::summary() const {
    std::string s = (data.title.empty() ? std::string("untitled") : "\"" + data.title + "\"") + ", " + std::to_string(data.blocks.size()) + " blocks";
    if (!path.empty()) s += " - " + path;
    return s;
}

std::unique_ptr<Attachment> TextDoc::preview() const {
    auto pages = layoutDocument(data);
    if (pages.empty()) return nullptr;
    if (pages.size() == 1) return std::make_unique<Attachment>(png(pdf::renderPage(pages.front(), kSheetScale), "page1.png"));
    // every page (up to kSheetPages), one under another, so the whole document can be scrolled through
    const std::size_t n = std::min<std::size_t>(pages.size(), kSheetPages);
    std::vector<raster::Image> shots;
    int width = 0, height = 0;
    for (std::size_t i = 0; i < n; ++i) {
        shots.push_back(pdf::renderPage(pages[i], kSheetScale));
        width = std::max(width, shots.back().width());
        height += shots.back().height() + (i ? kSheetGap : 0);
    }
    raster::Image sheet(width, height, raster::Color{0xC8, 0xCA, 0xCE, 255});
    int y = 0;
    for (const auto& shot : shots) {
        raster::blendOnto(sheet, shot, 0, y, raster::Blend::Normal, 1.0);
        y += shot.height() + kSheetGap;
    }
    return std::make_unique<Attachment>(png(sheet, "pages.png"));
}

std::string DeckDoc::summary() const {
    std::string s = std::to_string(data.slides.size()) + (data.slides.size() == 1 ? " slide" : " slides") + ", theme " + data.theme.name;
    if (!data.title.empty()) s = "\"" + data.title + "\", " + s;
    if (!path.empty()) s += " - " + path;
    return s;
}

std::unique_ptr<Attachment> DeckDoc::preview() const {
    if (data.slides.empty()) return nullptr;
    if (data.slides.size() == 1) return std::make_unique<Attachment>(png(pdf::renderPage(layoutSlide(data, 0), 800.0 / data.width), "slide1.png"));
    // a slide sorter: every slide as a numbered thumbnail, hidden ones faded
    const std::size_t n = std::min<std::size_t>(data.slides.size(), 48);
    const int cols = 3, tw = 300, gap = 16, label = 20;
    const int th = static_cast<int>(std::lround(tw * data.height / data.width));
    const int rows = static_cast<int>((n + cols - 1) / cols);
    raster::Image sheet(cols * tw + (cols + 1) * gap, rows * (th + label) + (rows + 1) * gap, raster::Color{0xE6, 0xE7, 0xEA, 255});
    for (std::size_t i = 0; i < n; ++i) {
        const int x = gap + static_cast<int>(i % cols) * (tw + gap), y = gap + static_cast<int>(i / cols) * (th + label + gap);
        raster::Image thumb = pdf::renderPage(layoutSlide(data, i), static_cast<double>(tw) / data.width);
        raster::blendOnto(sheet, thumb, x, y, raster::Blend::Normal, data.slides[i].hidden ? 0.35 : 1.0);
        const std::string tag = std::to_string(i + 1) + (data.slides[i].hidden ? " (hidden)" : "") +
                                (data.slides[i].transition.empty() ? "" : "  " + data.slides[i].transition);
        pdf::drawText(sheet, x, y + th + 15, tag, pdf::Font{pdf::Family::Sans, false, false}, 13, raster::Color{0x44, 0x44, 0x48, 255});
    }
    return std::make_unique<Attachment>(png(sheet, "slides.png"));
}

Result CloseAction::apply() {
    doc_ = session_.workspace().take(id_);
    if (!doc_) return Result::failure("no open document called '" + id_ + "'");
    return Result::success("closed " + id_).with("doc", id_);
}

void CloseAction::revert() {
    if (doc_) session_.workspace().put(id_, doc_);
}

bool has(const Params& p, const std::string& key) {
    auto it = p.find(key);
    return it != p.end() && !std::holds_alternative<std::monostate>(it->second);
}

bool hasText(const Params& p, const std::string& key) { return has(p, key) || has(p, key + "_file"); }

std::string textParam(const Params& p, const std::string& key, bool unescape) {
    if (!has(p, key) && has(p, key + "_file")) return readBytes(stringParam(p, key + "_file"));
    const std::string s = stringParam(p, key);
    if (!unescape || s.find('\n') != std::string::npos) return s;
    std::string out;
    for (std::size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '\\' && i + 1 < s.size() && (s[i + 1] == 'n' || s[i + 1] == 't')) {
            out += s[i + 1] == 'n' ? '\n' : '\t';
            ++i;
        } else {
            out += s[i];
        }
    }
    return out;
}

ActionFactory plainCommand(std::function<Result(const Params&)> fn) {
    return [fn](const Params& p) -> std::unique_ptr<Action> {
        return std::make_unique<FunctionAction>([fn, p]() -> Result {
            try {
                return fn(p);
            } catch (const std::exception& e) {
                return Result::failure(e.what());
            }
        });
    };
}

Result created(Session& s, std::shared_ptr<Document> doc, const std::string& verb, const std::string& extra) {
    const std::string prefix = doc->kind();
    const std::string summary = doc->summary();
    const std::string id = s.workspace().add(std::move(doc), prefix);
    Result r = Result::success(verb + " " + id + ": " + summary + extra, id);
    r.with("doc", id);
    return r;
}

// ---- params ----------------------------------------------------------------------------

ParamSpec docSpec(const char* kind, const char* example) {
    return {"doc", ParamType::String, std::string("id of an open ") + kind + ", e.g. " + example + " (optional when only one is open)", false};
}
ParamSpec fileSpec(const char* kind) {
    return {"file", ParamType::String, std::string("instead of an open ") + kind + ": read this file (for one-shot use)", false};
}

void loadInto(SheetDoc& d, const std::string& path) {
    d.data = loadWorkbook(path);
    d.path = path;
}
void loadInto(TextDoc& d, const std::string& path) {
    d.data = loadTextDocument(path);
    d.path = path;
}
void loadInto(DeckDoc& d, const std::string& path) {
    d.data = loadDeck(path);
    d.path = path;
}

ParamSpec previewSpec() { return {"preview", ParamType::Bool, "attach a PNG preview of the result", false, {}, false}; }
ParamSpec str(const std::string& name, const std::string& description, bool required, Value fallback, std::vector<std::string> choices) {
    return {name, ParamType::String, description, required, std::move(choices), std::move(fallback)};
}
ParamSpec integer(const std::string& name, const std::string& description, bool required, Value fallback) {
    return {name, ParamType::Int, description, required, {}, std::move(fallback)};
}
ParamSpec number(const std::string& name, const std::string& description, bool required, Value fallback) {
    return {name, ParamType::Number, description, required, {}, std::move(fallback)};
}
ParamSpec flag(const std::string& name, const std::string& description, Value fallback) {
    return {name, ParamType::Bool, description, false, {}, std::move(fallback)};
}

void maybePreview(Result& r, const Document& doc, const Params& p) {
    if (!r.ok || !boolParam(p, "preview", false)) return;
    if (auto a = doc.preview()) r.attach(*a);
}

Attachment png(const raster::Image& img, const std::string& name) { return {"image/png", raster::encodePng(img), name}; }

Value numValue(double v) {
    const double r = std::round(v);
    if (std::isfinite(v) && std::fabs(r) < 1e15 && std::fabs(v - r) < 1e-9) return static_cast<std::int64_t>(r);
    return v;
}

// ---- files -------------------------------------------------------------------------------

std::string extOf(const std::string& path) {
    std::string e = pathFromUtf8(path).extension().string();
    if (!e.empty() && e[0] == '.') e = e.substr(1);
    return lower(e);
}

std::string readBytes(const std::string& path) {
    std::ifstream in(pathFromUtf8(path), std::ios::binary);
    if (!in) throw std::runtime_error("cannot open " + path);
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

void writeBytes(const std::string& path, const std::string& bytes) {
    fs::path p = pathFromUtf8(path);
    std::error_code ec;
    if (p.has_parent_path()) fs::create_directories(p.parent_path(), ec);
    std::ofstream out(p, std::ios::binary);
    if (!out) throw std::runtime_error("cannot write " + path);
    out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    if (!out) throw std::runtime_error("error writing " + path);
}

std::string displayPath(const std::string& path) { return path; }

namespace {

std::optional<fs::path> soffice() {
    if (auto p = findProgram("soffice")) return p;
    return findProgram("libreoffice");
}

std::string fileUrl(const fs::path& p) {
    std::string s = fs::absolute(p).generic_string();
    std::string out = s.size() && s[0] == '/' ? "file://" : "file:///";
    for (unsigned char c : s) {
        if (std::isalnum(c) || c == '/' || c == '-' || c == '_' || c == '.' || c == ':' || c == '~') out += static_cast<char>(c);
        else {
            char b[4];
            std::snprintf(b, sizeof b, "%%%02X", c);
            out += b;
        }
    }
    return out;
}

fs::path tempDir(const std::string& tag) {
    std::error_code ec;
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    fs::path d = fs::temp_directory_path(ec) / ("wetwaresuite-office-" + tag + "-" + std::to_string(stamp));
    fs::create_directories(d, ec);
    return d;
}

}  // namespace

bool haveLibreOffice() { return soffice().has_value(); }

bool libreOffice(const fs::path& input, const std::string& toExt, std::string& out, std::string& error) {
    auto exe = soffice();
    if (!exe) {
        error = "this needs LibreOffice (soffice) on PATH for ." + toExt + "; install LibreOffice or pick a built-in format (office.formats lists them)";
        return false;
    }
    std::error_code ec;
    fs::path dir = tempDir("lo");
    fs::path profile = fs::temp_directory_path(ec) / "wetwaresuite-office-lo-profile";
    auto r = runProgram({exe->string(), "-env:UserInstallation=" + fileUrl(profile), "--headless", "--norestore", "--nologo", "--convert-to", toExt,
                         "--outdir", dir.string(), input.string()});
    fs::path produced = dir / input.stem();
    produced += "." + toExt;
    if (r.exitCode != 0 || !fs::exists(produced, ec)) {
        fs::remove_all(dir, ec);
        std::string why = trimCopy(r.output);
        if (why.size() > 400) why = why.substr(why.size() - 400);
        error = "LibreOffice could not convert " + input.filename().string() + " to ." + toExt + (why.empty() ? "" : ": " + why);
        return false;
    }
    try {
        out = readBytes(produced.string());
    } catch (const std::exception& e) {
        error = e.what();
        fs::remove_all(dir, ec);
        return false;
    }
    fs::remove_all(dir, ec);
    return true;
}

bool libreOfficeWrite(const std::string& bytes, const std::string& fromExt, const std::string& path, std::string& error) {
    fs::path dir = tempDir("in");
    fs::path in = dir / ("document." + fromExt);
    std::error_code ec;
    try {
        writeBytes(in.string(), bytes);
    } catch (const std::exception& e) {
        error = e.what();
        fs::remove_all(dir, ec);
        return false;
    }
    std::string out;
    bool ok = libreOffice(in, extOf(path), out, error);
    fs::remove_all(dir, ec);
    if (!ok) return false;
    try {
        writeBytes(path, out);
    } catch (const std::exception& e) {
        error = e.what();
        return false;
    }
    return true;
}

// ---- loading and saving per kind -----------------------------------------------------------

namespace {

const std::set<std::string> kSheetNative = {"csv", "tsv", "tab", "xlsx", "xlsm", "ods"};
const std::set<std::string> kSheetLoIn = {"xls", "xlsb", "fods", "numbers", "dbf", "sxc", "xlt", "xltx"};
const std::set<std::string> kSheetOut = {"xlsx", "ods", "csv", "tsv", "pdf", "html"};
const std::set<std::string> kSheetLoOut = {"xls", "fods"};

const std::set<std::string> kDocNative = {"docx", "md", "markdown", "txt", "html", "htm", "odt"};
const std::set<std::string> kDocLoIn = {"doc", "rtf", "docm", "dotx", "dot", "wpd", "fodt", "wps", "sxw", "pages", "lwp"};
const std::set<std::string> kDocOut = {"docx", "odt", "md", "markdown", "html", "htm", "txt", "pdf"};
const std::set<std::string> kDocLoOut = {"rtf", "doc", "fodt", "epub"};

const std::set<std::string> kDeckNative = {"pptx", "md", "markdown"};
const std::set<std::string> kDeckLoIn = {"ppt", "odp", "pps", "ppsx", "pptm", "potx", "fodp", "key", "sxi"};
const std::set<std::string> kDeckOut = {"pptx", "pdf", "md", "markdown"};
const std::set<std::string> kDeckLoOut = {"odp", "ppt", "fodp"};

bool in(const std::set<std::string>& s, const std::string& e) { return s.count(e) > 0; }

std::string loToBytes(const std::string& path, const std::string& ext) {
    std::string out, error;
    if (!libreOffice(pathFromUtf8(path), ext, out, error)) throw std::runtime_error(error);
    return out;
}

std::string mediaPrefixFor(const std::string& path) { return pathFromUtf8(path).stem().string() + "_media/"; }

void writeMedia(const std::string& path, const std::vector<MediaFile>& media) {
    fs::path dir = pathFromUtf8(path).parent_path();
    for (const auto& m : media) writeBytes((dir / pathFromUtf8(m.path)).string(), m.bytes);
}

TextDocument readPlainText(const std::string& text) {
    TextDocument d;
    std::string para;
    auto flush = [&] {
        if (para.empty()) return;
        Block b;
        b.kind = Block::Paragraph;
        Run r;
        r.text = para;
        b.runs.push_back(r);
        d.blocks.push_back(std::move(b));
        para.clear();
    };
    for (const auto& line : splitLines(text)) {
        if (trimCopy(line).empty()) flush();
        else para += (para.empty() ? "" : "\n") + line;
    }
    flush();
    return d;
}

std::string sheetHtml(const Workbook& wb, int only) {
    std::string out = "<!DOCTYPE html>\n<html><head><meta charset=\"utf-8\"><title>Workbook</title><style>"
                      "body{font-family:Arial,Helvetica,sans-serif}table{border-collapse:collapse;margin-bottom:24px}"
                      "td{border:1px solid #ccc;padding:2px 6px}td.n{text-align:right}</style></head><body>\n";
    for (std::size_t s = 0; s < wb.sheets.size(); ++s) {
        if (only >= 0 && static_cast<int>(s) != only) continue;
        const Sheet& sh = wb.sheets[s];
        out += "<h2>" + xml::escape(sh.name) + "</h2>\n<table>\n";
        int r1, c1, r2, c2;
        if (sh.bounds(r1, c1, r2, c2)) {
            for (int r = 0; r <= r2; ++r) {
                out += "<tr>";
                for (int c = 0; c <= c2; ++c) {
                    const Cell* cell = sh.get(r, c);
                    std::string v = cell ? formatValue(cell->value, cell->format) : "";
                    const bool num = cell && cell->value.type == CellValue::Number;
                    out += std::string("<td") + (num ? " class=\"n\"" : "") + ">";
                    if (cell && cell->style.bold) out += "<b>" + xml::escape(v) + "</b>";
                    else out += xml::escape(v);
                    out += "</td>";
                }
                out += "</tr>\n";
            }
        }
        out += "</table>\n";
    }
    return out + "</body></html>\n";
}

}  // namespace

Workbook loadWorkbook(const std::string& path, char delimiter) {
    CsvOptions o;
    o.delimiter = delimiter;
    return loadWorkbook(path, o);
}

Workbook loadWorkbook(const std::string& path, const CsvOptions& csv) {
    const std::string ext = extOf(path);
    Workbook wb;
    if (ext == "csv" || ext == "tsv" || ext == "tab" || ext == "txt") {
        CsvOptions o = csv;
        if (!o.delimiter && (ext == "tsv" || ext == "tab")) o.delimiter = '\t';
        wb = readCsv(readBytes(path), o);
        if (!wb.sheets.empty()) {
            std::string stem = pathFromUtf8(path).stem().string();
            if (!stem.empty()) wb.sheets[0].name = stem.substr(0, 31);
        }
    } else if (ext == "xlsx" || ext == "xlsm") {
        wb = readXlsx(readBytes(path));
    } else if (ext == "ods") {
        wb = readOds(readBytes(path));
    } else if (in(kSheetLoIn, ext)) {
        wb = readXlsx(loToBytes(path, "xlsx"));
    } else {
        throw std::runtime_error("can't open ." + ext + " as a spreadsheet (csv, tsv, xlsx, ods; xls and others through LibreOffice)");
    }
    if (wb.sheets.empty()) wb.sheets.push_back(Sheet{"Sheet1", {}, {}});
    wb.recalc();
    return wb;
}

void saveWorkbook(const Workbook& wb, const std::string& path, int sheet) {
    const std::string ext = extOf(path);
    const int active = std::clamp(sheet >= 0 ? sheet : wb.active, 0, std::max(0, static_cast<int>(wb.sheets.size()) - 1));
    if (ext == "xlsx" || ext == "xlsm") {
        writeBytes(path, writeXlsx(wb));
    } else if (ext == "ods") {
        writeBytes(path, writeOds(wb));
    } else if (ext == "csv" || ext == "tsv" || ext == "tab" || ext == "txt") {
        if (wb.sheets.empty()) throw std::runtime_error("the workbook has no sheets");
        writeBytes(path, writeCsv(wb.sheets[static_cast<std::size_t>(active)], ext == "csv" ? ',' : '\t'));
    } else if (ext == "pdf") {
        std::vector<pdf::Page> pages;
        for (int s = 0; s < static_cast<int>(wb.sheets.size()); ++s) {
            if (sheet >= 0 && s != sheet) continue;
            auto p = layoutSheet(wb, s);
            pages.insert(pages.end(), p.begin(), p.end());
        }
        writeBytes(path, pdf::writePdf(pages, {pathFromUtf8(path).stem().string(), "", ""}));
    } else if (ext == "html" || ext == "htm") {
        writeBytes(path, sheetHtml(wb, sheet));
    } else if (in(kSheetLoOut, ext)) {
        std::string error;
        if (!libreOfficeWrite(writeXlsx(wb), "xlsx", path, error)) throw std::runtime_error(error);
    } else {
        throw std::runtime_error("can't save a spreadsheet as ." + ext + " (xlsx, ods, csv, tsv, pdf, html; xls and fods through LibreOffice)");
    }
}

TextDocument loadTextDocument(const std::string& path) {
    const std::string ext = extOf(path);
    const std::string base = pathFromUtf8(path).parent_path().string();
    TextDocument d;
    if (ext == "docx" || ext == "docm") d = readDocx(readBytes(path));
    else if (ext == "md" || ext == "markdown") d = readMarkdown(readBytes(path), base);
    else if (ext == "html" || ext == "htm") d = readHtml(readBytes(path), base);
    else if (ext == "odt") d = readOdt(readBytes(path));
    else if (ext == "txt" || ext == "text") d = readPlainText(readBytes(path));
    else if (in(kDocLoIn, ext)) d = readDocx(loToBytes(path, "docx"));
    else throw std::runtime_error("can't open ." + ext + " as a document (docx, md, html, odt, txt; doc and rtf through LibreOffice)");
    if (d.title.empty() && ext != "md" && ext != "markdown") {
        for (const auto& b : d.blocks) {
            if (b.kind == Block::Heading) {
                d.title = plainText(b.runs);
                break;
            }
        }
    }
    return d;
}

void saveTextDocument(const TextDocument& doc, const std::string& path) {
    const std::string ext = extOf(path);
    if (ext == "docx") {
        writeBytes(path, writeDocx(doc));
    } else if (ext == "odt") {
        writeBytes(path, writeOdt(doc));
    } else if (ext == "md" || ext == "markdown") {
        std::vector<MediaFile> media;
        std::string text = writeMarkdown(doc, mediaPrefixFor(path), media);
        writeBytes(path, text);
        writeMedia(path, media);
    } else if (ext == "html" || ext == "htm") {
        writeBytes(path, writeHtml(doc));
    } else if (ext == "txt" || ext == "text") {
        writeBytes(path, writeText(doc));
    } else if (ext == "pdf") {
        writeBytes(path, pdf::writePdf(layoutDocument(doc), {doc.title, doc.author, ""}));
    } else if (in(kDocLoOut, ext)) {
        std::string error;
        if (!libreOfficeWrite(writeDocx(doc), "docx", path, error)) throw std::runtime_error(error);
    } else {
        throw std::runtime_error("can't save a document as ." + ext + " (docx, odt, md, html, txt, pdf; rtf and doc through LibreOffice)");
    }
}

Deck loadDeck(const std::string& path) {
    const std::string ext = extOf(path);
    if (ext == "pptx" || ext == "pptm" || ext == "ppsx") return readPptx(readBytes(path));
    if (ext == "md" || ext == "markdown" || ext == "txt") return deckFromOutline(readBytes(path), pathFromUtf8(path).parent_path().string());
    if (in(kDeckLoIn, ext)) return readPptx(loToBytes(path, "pptx"));
    throw std::runtime_error("can't open ." + ext + " as a slide deck (pptx, md outline; odp and ppt through LibreOffice)");
}

void saveDeck(const Deck& deck, const std::string& path) {
    const std::string ext = extOf(path);
    if (ext == "pptx") {
        writeBytes(path, writePptx(deck));
    } else if (ext == "pdf") {
        writeBytes(path, pdf::writePdf(layoutDeck(deck), {deck.title, deck.author, ""}));
    } else if (ext == "md" || ext == "markdown") {
        std::vector<MediaFile> media;
        writeBytes(path, deckToOutline(deck, mediaPrefixFor(path), media));
        writeMedia(path, media);
    } else if (in(kDeckLoOut, ext)) {
        std::string error;
        if (!libreOfficeWrite(writePptx(deck), "pptx", path, error)) throw std::runtime_error(error);
    } else {
        throw std::runtime_error("can't save a deck as ." + ext + " (pptx, pdf, md; odp and ppt through LibreOffice)");
    }
}

// ---- convert -------------------------------------------------------------------------------

namespace {

std::string joinSet(const std::set<std::string>& s) {
    std::string out;
    for (const auto& v : s) out += (out.empty() ? "" : " ") + v;
    return out;
}

enum class Kind { None, Sheet, Doc, Deck };

// A Markdown file written as a slide outline (front matter with theme:/aspect:, or layout comments)?
bool looksLikeOutline(const std::string& path) {
    std::string text;
    try {
        text = readBytes(path);
    } catch (const std::exception&) {
        return false;
    }
    if (text.find("<!-- layout:") != std::string::npos || text.find("<!-- column -->") != std::string::npos) return true;
    if (startsWith(text, "---")) {
        auto end = text.find("\n---", 3);
        std::string front = lower(text.substr(0, end == std::string::npos ? 0 : end));
        return front.find("\ntheme:") != std::string::npos || front.find("\naspect:") != std::string::npos;
    }
    return false;
}

// Which built-in engine reads `from` when the result should be `to`.
Kind builtinKind(const std::string& from, const std::string& to, const std::string& kind, const std::string& input) {
    const bool deckOut = in(kDeckOut, to) || in(kDeckLoOut, to);
    const bool docOut = in(kDocOut, to) || in(kDocLoOut, to);
    const bool sheetOut = in(kSheetOut, to) || in(kSheetLoOut, to);
    const bool md = from == "md" || from == "markdown";
    if (md && (kind == "deck" || (kind == "auto" && (to == "pptx" || in(kDeckLoOut, to) || looksLikeOutline(input)))) && deckOut) return Kind::Deck;
    if (kind == "deck" && !md && !(from == "pptx" || from == "pptm" || from == "ppsx")) return Kind::None;
    if (in(kSheetNative, from) && sheetOut) return Kind::Sheet;
    if (in(kDocNative, from) && docOut) return Kind::Doc;
    if ((from == "pptx" || from == "pptm" || from == "ppsx") && deckOut) return Kind::Deck;
    return Kind::None;
}

}  // namespace

void registerConvertCommands(Session& session) {
    auto& reg = session.registry();
    (void)session;
    reg.add({"office.convert", "Convert Office File",
             "Convert an office file in one step; the output's extension picks the format. Built in (no LibreOffice): "
             "spreadsheets csv/tsv/xlsx/ods -> xlsx/csv/tsv/pdf/html; documents docx/md/html/odt/txt -> docx/odt/md/html/txt/pdf; "
             "decks pptx or a Markdown outline -> pptx/pdf/md; pdf -> txt (text extraction). Everything else (doc, xls, ppt, rtf, "
             "ods/odp output, ...) goes through LibreOffice when it is installed. backend=libreoffice forces LibreOffice.",
             {str("input", "file to convert", true), str("output", "file to write; its extension sets the format", true),
              str("backend", "auto uses the built-in engines when they handle the pair", false, std::string("auto"), {"auto", "builtin", "libreoffice"}),
              integer("sheet", "spreadsheet to csv/tsv: which sheet (1-based; default the active one)"),
              str("kind", "how to read a Markdown input: as a document or a slide outline (auto: an outline when it has theme:/layout markers or the output is a deck format)",
                  false, std::string("auto"), {"auto", "doc", "deck"})},
             false, false},
            plainCommand([](const Params& p) -> Result {
                const std::string input = stringParam(p, "input"), output = stringParam(p, "output");
                const std::string from = extOf(input), to = extOf(output);
                const std::string backend = stringParam(p, "backend", "auto");
                std::error_code ec;
                if (!fs::exists(pathFromUtf8(input), ec)) return Result::failure("no such file: " + input);
                if (to.empty()) return Result::failure("give the output an extension (e.g. report.pdf) so the format is known");
                Kind k = backend == "libreoffice" ? Kind::None : builtinKind(from, to, stringParam(p, "kind", "auto"), input);
                if (backend == "builtin" && (in(kSheetLoOut, to) || in(kDocLoOut, to) || in(kDeckLoOut, to))) {
                    return Result::failure("." + to + " is written through LibreOffice, not built in (office.formats lists the built-in formats)");
                }
                std::string used = "built-in";
                std::string detail;
                if (from == "pdf" && (to == "txt" || to == "text") && backend != "libreoffice") {
                    pdf::PdfFile f = pdf::PdfFile::load(pathFromUtf8(input));
                    std::string text;
                    for (int i = 0; i < f.pageCount(); ++i) text += f.pageText(i) + (i + 1 < f.pageCount() ? "\n\f" : "\n");
                    writeBytes(output, text);
                    detail = std::to_string(f.pageCount()) + " pages of text";
                } else if (k == Kind::Sheet) {
                    Workbook wb = loadWorkbook(input);
                    const int sheet = has(p, "sheet") ? static_cast<int>(intParam(p, "sheet")) - 1 : -1;
                    if (sheet >= static_cast<int>(wb.sheets.size())) return Result::failure("there are only " + std::to_string(wb.sheets.size()) + " sheets");
                    saveWorkbook(wb, output, sheet);
                    detail = std::to_string(wb.sheets.size()) + " sheet(s)";
                    if (in(kSheetLoOut, to)) used = "built-in + LibreOffice";
                } else if (k == Kind::Doc) {
                    TextDocument d = loadTextDocument(input);
                    saveTextDocument(d, output);
                    detail = std::to_string(d.blocks.size()) + " blocks";
                    if (in(kDocLoOut, to)) used = "built-in + LibreOffice";
                } else if (k == Kind::Deck) {
                    Deck d = loadDeck(input);
                    saveDeck(d, output);
                    detail = std::to_string(d.slides.size()) + " slides";
                    if (in(kDeckLoOut, to)) used = "built-in + LibreOffice";
                } else {
                    if (backend == "builtin") return Result::failure("no built-in converter from ." + from + " to ." + to + " (office.formats lists them)");
                    std::string out, error;
                    if (!libreOffice(pathFromUtf8(input), to, out, error)) return Result::failure(error);
                    writeBytes(output, out);
                    used = "LibreOffice";
                }
                auto bytes = static_cast<std::int64_t>(fs::file_size(pathFromUtf8(output), ec));
                return Result::success("converted " + input + " to " + output + " (" + used + (detail.empty() ? "" : ", " + detail) + ", " +
                                           bytesText(static_cast<std::size_t>(bytes)) + ")",
                                       output)
                    .with("output", output)
                    .with("backend", used)
                    .with("bytes", bytes);
            }));

    reg.add({"office.formats", "Office Formats",
             "List the file formats the office tool reads and writes, which are built in and which need LibreOffice, and "
             "whether LibreOffice and pdftoppm are installed.",
             {}, false, true},
            plainCommand([](const Params&) -> Result {
                const bool lo = haveLibreOffice();
                const bool ppm = findProgram("pdftoppm").has_value();
                std::string t;
                t += "spreadsheets (office.sheet_*): read " + joinSet(kSheetNative) + "; write " + joinSet(kSheetOut) + "\n";
                t += "  through LibreOffice: read " + joinSet(kSheetLoIn) + "; write " + joinSet(kSheetLoOut) + "\n";
                t += "documents (office.doc_*): read " + joinSet(kDocNative) + "; write " + joinSet(kDocOut) + "\n";
                t += "  through LibreOffice: read " + joinSet(kDocLoIn) + "; write " + joinSet(kDocLoOut) + "\n";
                t += "slide decks (office.deck_*): read " + joinSet(kDeckNative) + "; write " + joinSet(kDeckOut) + " png\n";
                t += "  through LibreOffice: read " + joinSet(kDeckLoIn) + "; write " + joinSet(kDeckLoOut) + "\n";
                t += "pdf (office.pdf_*): info, text, merge, split, extract, delete pages, rotate, images to pdf (built in); render pages to png (pdftoppm)\n";
                t += std::string("LibreOffice: ") + (lo ? "installed" : "not found (soffice)") + "; pdftoppm: " + (ppm ? "installed" : "not found");
                return Result::success(t).with("libreoffice", lo).with("pdftoppm", ppm);
            }));
}

void registerCommands(Session& session) {
    registerSheetCommands(session);
    registerDocCommands(session);
    registerDeckCommands(session);
    registerPdfCommands(session);
    registerConvertCommands(session);
}

}  // namespace wetwaresuite::office
