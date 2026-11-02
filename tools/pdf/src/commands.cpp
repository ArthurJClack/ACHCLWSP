// pdf.* commands: the shared helpers, and opening, saving, viewing, reading
// and searching documents, form fields and document info.
#include "commands.hpp"

#include <algorithm>
#include <cmath>
#include <map>
#include <mutex>

#include "wetwaresuite/pdf/commands.hpp"
#include "wetwaresuite/raster/codec.hpp"

namespace wetwaresuite::pdf::tool {

// ---- plumbing -------------------------------------------------------------------------

Result SnapshotEdit::apply() {
    before_ = std::make_unique<DocData>(doc_->data);
    Result r;
    try {
        r = fn_(*doc_);
    } catch (const std::exception& e) {
        r = Result::failure(e.what());
    }
    if (!r.ok) revert();
    return r;
}

void SnapshotEdit::revert() {
    if (before_) doc_->data = *before_;
}

bool has(const Params& p, const std::string& key) {
    auto it = p.find(key);
    return it != p.end() && !std::holds_alternative<std::monostate>(it->second) &&
           !(std::holds_alternative<std::string>(it->second) && std::get<std::string>(it->second).empty());
}

namespace {

// the PDF used last in each session: commands without doc= work on it
std::mutex currentMutex;
std::map<const Session*, std::string> currentDocs;

void setCurrent(const Session& s, const std::string& id) {
    std::lock_guard lock(currentMutex);
    currentDocs[&s] = id;
}

std::shared_ptr<PdfDoc> findDoc(Session& s, const Params& p, std::string& id, std::string& error) {
    if (has(p, "doc")) {
        id = stringParam(p, "doc");
        auto doc = s.workspace().getAs<PdfDoc>(id);
        if (!doc) error = "no open PDF called '" + id + "' (pdf.open or pdf.new makes one)";
        else setCurrent(s, id);
        return doc;
    }
    {
        std::lock_guard lock(currentMutex);
        auto it = currentDocs.find(&s);
        if (it != currentDocs.end()) {
            if (auto doc = s.workspace().getAs<PdfDoc>(it->second)) {
                id = it->second;
                return doc;
            }
        }
    }
    std::vector<std::string> found;
    for (const auto& i : s.workspace().ids()) {
        if (s.workspace().getAs<PdfDoc>(i)) found.push_back(i);
    }
    if (found.size() == 1) {
        id = found.front();
        return s.workspace().getAs<PdfDoc>(id);
    }
    error = found.empty() ? "no PDF is open (pdf.open path=... opens one)" : "several PDFs are open; say which with doc=<id>";
    return nullptr;
}

class CloseAction : public Action {
public:
    CloseAction(Session& s, std::string id) : s_(s), id_(std::move(id)) {}
    Result apply() override {
        doc_ = s_.workspace().take(id_);
        if (!doc_) return Result::failure("no open document called '" + id_ + "'");
        return Result::success("closed " + id_).with("doc", id_);
    }
    void revert() override {
        if (doc_) s_.workspace().put(id_, doc_);
    }

private:
    Session& s_;
    std::string id_;
    std::shared_ptr<Document> doc_;
};

}  // namespace

std::shared_ptr<PdfDoc> findPdfDoc(Session& s, const Params& p, std::string& id, std::string& error) { return findDoc(s, p, id, error); }

ActionFactory docCommand(Session& session, DocFn fn, bool undoable) {
    return [&session, fn, undoable](const Params& p) -> std::unique_ptr<Action> {
        std::string id, error;
        auto doc = findDoc(session, p, id, error);
        if (!doc) return std::make_unique<FunctionAction>([error] { return Result::failure(error); });
        if (!undoable) {
            return std::make_unique<FunctionAction>([doc, fn, p, id]() -> Result {
                try {
                    return fn(*doc, p, id);
                } catch (const std::exception& e) {
                    return Result::failure(e.what());
                }
            });
        }
        return std::make_unique<SnapshotEdit>(doc, [fn, p, id](PdfDoc& d) { return fn(d, p, id); });
    };
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

ParamSpec docSpec() { return {"doc", ParamType::String, "id of an open PDF, e.g. pdf1 (default: the one used last)", false}; }
ParamSpec pageSpec(bool required) { return {"page", ParamType::Int, "page number, 1 = first", required, {}, std::int64_t{1}}; }
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
ParamSpec list(const std::string& name, const std::string& description, bool required) {
    return {name, ParamType::NumberList, description, required, {}, {}};
}

int pageParam(const DocData& d, const Params& p, const std::string& key) {
    const auto n = intParam(p, key, 1);
    if (n < 1 || n > static_cast<std::int64_t>(d.pages.size()))
        throw std::runtime_error("page " + std::to_string(n) + " doesn't exist (the document has " + std::to_string(d.pages.size()) + ")");
    return static_cast<int>(n - 1);
}

std::vector<int> pagesParam(const DocData& d, const Params& p, const std::string& key) {
    const std::string spec = stringParam(p, key, "all");
    return parsePageList(spec.empty() ? "all" : spec, static_cast<int>(d.pages.size()));
}

std::optional<Color> optColorParam(const Params& p, const std::string& key) {
    if (!has(p, key)) return std::nullopt;
    auto c = parseColor(stringParam(p, key));
    if (!c) throw std::runtime_error("'" + stringParam(p, key) + "' isn't a colour (try red, #ff8800 or rgb(0,0,0))");
    return c;
}

Color colorParam(const Params& p, const std::string& key, Color fallback) { return optColorParam(p, key).value_or(fallback); }

Font fontParam(const Params& p) {
    Font f;
    const std::string fam = utf8Lower(stringParam(p, "font", "sans"));
    if (fam == "serif" || fam == "times") f.family = Family::Serif;
    else if (fam == "mono" || fam == "courier") f.family = Family::Mono;
    f.bold = boolParam(p, "bold", false);
    f.italic = boolParam(p, "italic", false);
    return f;
}

std::array<double, 4> boxToBase(const PageGeom& g, double x, double y, double w, double h) {
    double xs[2], ys[2];
    toBase(g, x, y, xs[0], ys[0]);
    toBase(g, x + w, y + h, xs[1], ys[1]);
    return {std::min(xs[0], xs[1]), std::min(ys[0], ys[1]), std::abs(xs[1] - xs[0]), std::abs(ys[1] - ys[0])};
}

std::array<double, 4> boxFromBase(const PageGeom& g, double x, double y, double w, double h) {
    double xs[2], ys[2];
    fromBase(g, x, y, xs[0], ys[0]);
    fromBase(g, x + w, y + h, xs[1], ys[1]);
    return {std::min(xs[0], xs[1]), std::min(ys[0], ys[1]), std::abs(xs[1] - xs[0]), std::abs(ys[1] - ys[0])};
}

Mark& addMark(DocData& d, int page, Mark m) {
    m.id = d.nextMark++;
    auto& marks = d.pages.at(static_cast<std::size_t>(page)).marks;
    marks.push_back(std::move(m));
    return marks.back();
}

Mark* findMark(DocData& d, int id, int* page) {
    for (std::size_t i = 0; i < d.pages.size(); ++i) {
        for (auto& m : d.pages[i].marks) {
            if (m.id == id) {
                if (page) *page = static_cast<int>(i);
                return &m;
            }
        }
    }
    return nullptr;
}

Result markAdded(const DocData& d, int page, const Mark& m, const std::string& what) {
    (void)d;
    return Result::success(what + " on page " + std::to_string(page + 1) + " (edit " + std::to_string(m.id) + ")", static_cast<std::int64_t>(m.id))
        .with("mark", static_cast<std::int64_t>(m.id))
        .with("page", static_cast<std::int64_t>(page + 1));
}

std::vector<int> charsInBox(const std::vector<TextChar>& chars, const std::array<double, 4>& b) {
    std::vector<int> out;
    for (std::size_t i = 0; i < chars.size(); ++i) {
        const TextChar& c = chars[i];
        const double cx = (c.x0 + c.x1) / 2, cy = (c.y0 + c.y1) / 2;
        if (cx >= b[0] && cx <= b[0] + b[2] && cy >= b[1] && cy <= b[1] + b[3]) out.push_back(static_cast<int>(i));
    }
    return out;
}

std::vector<std::array<double, 4>> lineBoxes(const std::vector<TextChar>& chars, const std::vector<int>& which) {
    std::vector<std::array<double, 4>> out;  // x0, y0, x1, y1 while building
    for (int i : which) {
        const TextChar& c = chars[static_cast<std::size_t>(i)];
        if (c.text == " " && out.empty()) continue;
        bool merged = false;
        for (auto& b : out) {
            const double h = std::min(b[3] - b[1], c.y1 - c.y0);
            const double overlap = std::min(b[3], c.y1) - std::max(b[1], c.y0);
            if (overlap > h * 0.5 && c.x0 <= b[2] + c.size * 2 && c.x1 >= b[0] - c.size * 2) {
                b = {std::min(b[0], c.x0), std::min(b[1], c.y0), std::max(b[2], c.x1), std::max(b[3], c.y1)};
                merged = true;
                break;
            }
        }
        if (!merged) out.push_back({c.x0, c.y0, c.x1, c.y1});
    }
    for (auto& b : out) b = {b[0], b[1], b[2] - b[0], b[3] - b[1]};
    return out;
}

int dropGlyphs(DocData& d, int page, const std::vector<TextChar>& chars, const std::vector<int>& which) {
    PageEntry& e = d.pages.at(static_cast<std::size_t>(page));
    if (e.file < 0) return 0;
    std::vector<std::pair<int, int>> glyphs;
    int skipped = 0;
    for (int i : which) {
        const TextChar& c = chars[static_cast<std::size_t>(i)];
        if (c.op < 0) ++skipped;
        else glyphs.emplace_back(c.op, c.glyph);
    }
    if (glyphs.empty()) return skipped;
    const PdfFile& f = *d.files[static_cast<std::size_t>(e.file)];
    e.content = std::make_shared<const std::string>(removeGlyphs(f, e.index, glyphs, e.content ? e.content.get() : nullptr));
    return skipped;
}

std::shared_ptr<ImageData> loadPicture(const std::string& path) { return loadImageData(readBytes(path)); }

// ---- file commands ------------------------------------------------------------------------

namespace {

Result created(Session& s, std::shared_ptr<PdfDoc> doc, const std::string& verb) {
    const std::string summary = doc->summary();
    const std::string id = s.workspace().add(doc, "pdf");
    setCurrent(s, id);
    return Result::success(verb + " " + id + ": " + summary, id).with("doc", id).with("pages", static_cast<std::int64_t>(doc->data.pages.size()));
}

std::string pageSizes(const DocData& d) {
    std::string out;
    for (std::size_t i = 0; i < d.pages.size();) {
        const PageGeom g = geometry(d, static_cast<int>(i));
        std::size_t j = i;
        while (j + 1 < d.pages.size()) {
            const PageGeom h = geometry(d, static_cast<int>(j + 1));
            if (std::abs(h.width - g.width) > 0.5 || std::abs(h.height - g.height) > 0.5) break;
            ++j;
        }
        out += "\n  page" + std::string(j > i ? "s " : " ") + std::to_string(i + 1) + (j > i ? "-" + std::to_string(j + 1) : "") + ": " + fmt(g.width) +
               " x " + fmt(g.height) + " pt";
        i = j + 1;
        if (out.size() > 2000) {
            out += "\n  ...";
            break;
        }
    }
    return out;
}

}  // namespace

void registerFileCommands(Session& session) {
    auto& reg = session.registry();

    reg.add({"pdf.open", "Open PDF",
             "Open any PDF to view, mark up, fill in, sign or edit. Returns a doc id like pdf1. Nothing changes on disk until pdf.save.",
             {str("path", "the PDF file", true), str("password", "for a password-protected PDF: its open (user) or owner password", false)},
             false, true},
            plainCommand([&session](const Params& p) -> Result {
                const std::string path = stringParam(p, "path");
                const std::string password = stringParam(p, "password", "");
                std::shared_ptr<const PdfFile> file;
                try {
                    file = std::make_shared<const PdfFile>(PdfFile::load(pathFromUtf8(path), password));
                } catch (const PasswordError& e) {
                    return Result::failure(path + (e.given ? ": that password doesn't open it" : " is password-protected; open it with password=..."));
                }
                if (file->encrypted()) return Result::failure(path + " is password-protected and couldn't be unlocked");
                if (file->pageCount() == 0) return Result::failure(path + " has no pages");
                auto doc = std::make_shared<PdfDoc>();
                doc->path = path;
                if (file->wasEncrypted()) {  // saving keeps it locked with the same password (pdf.protect remove=true takes it off)
                    doc->data.protect = true;
                    // opened with the owner password: keep it as the open password too, unless the file
                    // opens with no password at all (then it only had restrictions)
                    bool opensWithout = !password.empty() && file->openedAsOwner();
                    if (opensWithout) {
                        try {
                            (void)PdfFile::load(pathFromUtf8(path), "");
                        } catch (const PasswordError&) {
                            opensWithout = false;
                        }
                    }
                    doc->data.userPassword = opensWithout ? std::string() : password;
                    doc->data.ownerPassword = password;
                    const int perms = file->permissions();
                    doc->data.allowPrint = (perms & 4) != 0;
                    doc->data.allowCopy = (perms & 16) != 0;
                    doc->data.allowEdit = (perms & 8) != 0;
                }
                doc->data.files.push_back(file);
                for (int i = 0; i < file->pageCount(); ++i) {
                    PageEntry e;
                    e.file = 0;
                    e.index = i;
                    doc->data.pages.push_back(std::move(e));
                }
                Result r = created(session, doc, file->wasEncrypted() ? "opened (" + file->security() + ", unlocked" + (file->openedAsOwner() ? " as owner" : "") + ")" : "opened");
                r.with("fields", static_cast<std::int64_t>(doc->fields().size()));
                if (file->wasEncrypted()) r.with("security", file->security()).with("owner", file->openedAsOwner());
                return r;
            }));

    reg.add({"pdf.new", "New PDF", "Start a PDF of blank pages to write, draw and place pictures on.",
             {integer("pages", "how many pages", false, std::int64_t{1}), str("size", "paper size", false, std::string("a4"), {"a4", "letter", "legal", "a3", "a5"}),
              flag("landscape", "turn the paper sideways", false)},
             false, true},
            plainCommand([&session](const Params& p) -> Result {
                const std::string size = stringParam(p, "size", "a4");
                double w = 595.28, h = 841.89;
                if (size == "letter") w = 612, h = 792;
                else if (size == "legal") w = 612, h = 1008;
                else if (size == "a3") w = 841.89, h = 1190.55;
                else if (size == "a5") w = 419.53, h = 595.28;
                if (boolParam(p, "landscape", false)) std::swap(w, h);
                const auto n = intParam(p, "pages", 1);
                if (n < 1 || n > 2000) return Result::failure("pages must be 1 to 2000");
                auto doc = std::make_shared<PdfDoc>();
                doc->data.files.push_back(nullptr);
                for (std::int64_t i = 0; i < n; ++i) {
                    PageEntry e;
                    e.file = -1;
                    e.width = w;
                    e.height = h;
                    doc->data.pages.push_back(e);
                }
                return created(session, doc, "made");
            }));

    reg.add({"pdf.save", "Save PDF",
             "Write the PDF with every edit. Highlights, notes, drawings and shapes stay editable comments in other PDF apps unless "
             "flatten=true burns them into the page. pages= saves only some pages (extract), e.g. 2-4.",
             {docSpec(), str("path", "where to write (default: over the opened file)", false), flag("flatten", "draw comments into the pages", false),
              str("pages", "only these pages, e.g. 1-3,7", false)},
             false, false},
            docCommand(session,
                       [](PdfDoc& d, const Params& p, const std::string& id) -> Result {
                           const std::string path = has(p, "path") ? stringParam(p, "path") : d.path;
                           if (path.empty()) return Result::failure("say where to save with path=...");
                           std::vector<int> only;
                           if (has(p, "pages")) only = pagesParam(d.data, p);
                           const std::string bytes = finishBytes(d.data, saveBytes(d.data, boolParam(p, "flatten", false), has(p, "pages") ? &only : nullptr));
                           writeBytes(path, bytes);
                           if (!has(p, "pages")) d.path = path;
                           const std::size_t n = has(p, "pages") ? only.size() : d.data.pages.size();
                           return Result::success("saved " + id + " to " + path + " (" + std::to_string(n) + " page" + (n == 1 ? "" : "s") + ", " +
                                                      std::to_string(bytes.size() / 1024) + " KB" + (d.data.protect ? ", password-protected" : "") +
                                                      (d.data.digital ? ", digitally signed" : "") + ")",
                                                  path)
                               .with("path", path)
                               .with("bytes", static_cast<std::int64_t>(bytes.size()));
                       },
                       false));

    reg.add({"pdf.protect", "Password Protect",
             "Lock the saved PDF with a password (AES-256, opens in every PDF reader): password= is needed to open it; owner= is "
             "needed to print, copy or change it when print/copy/edit=false (owner alone: anyone can open it, only you can change "
             "it). remove=true saves it unlocked again. Takes effect on the next pdf.save.",
             {docSpec(), str("password", "the password to open it (empty: opens without one)", false), str("owner", "the owner password (default: the same)", false),
              flag("print", "allow printing", true), flag("copy", "allow copying text", true), flag("edit", "allow changes and comments", true),
              flag("remove", "take the password off", false)},
             true, false},
            docCommand(session,
                       [](PdfDoc& d, const Params& p, const std::string& id) -> Result {
                           if (boolParam(p, "remove", false)) {
                               d.data.protect = false;
                               d.data.userPassword.clear();
                               d.data.ownerPassword.clear();
                               return Result::success(id + " will save without a password");
                           }
                           const std::string user = stringParam(p, "password", ""), owner = stringParam(p, "owner", "");
                           if (user.empty() && owner.empty()) return Result::failure("give password= (to open it) and/or owner= (to change it)");
                           d.data.protect = true;
                           d.data.userPassword = user;
                           d.data.ownerPassword = owner.empty() ? user : owner;
                           d.data.allowPrint = boolParam(p, "print", true);
                           d.data.allowCopy = boolParam(p, "copy", true);
                           d.data.allowEdit = boolParam(p, "edit", true);
                           std::string t = id + " will save " + (user.empty() ? "openable by anyone" : "locked with a password");
                           if (!d.data.allowPrint || !d.data.allowCopy || !d.data.allowEdit)
                               t += std::string(", owner password needed to") + (!d.data.allowPrint ? " print" : "") + (!d.data.allowCopy ? " copy" : "") +
                                    (!d.data.allowEdit ? " edit" : "");
                           return Result::success(t + " (AES-256)");
                       },
                       true));

    reg.add({"pdf.close", "Close PDF", "Close an open PDF without saving (undo opens it again).", {docSpec()}, true, false},
            [&session](const Params& p) -> std::unique_ptr<Action> {
                std::string id, error;
                auto doc = findDoc(session, p, id, error);
                if (!doc) return std::make_unique<FunctionAction>([error] { return Result::failure(error); });
                return std::make_unique<CloseAction>(session, id);
            });

    reg.add({"pdf.info", "PDF Info", "Pages and their sizes, title and author, form fields and the edits made so far.", {docSpec()}, false, true},
            docCommand(session,
                       [](PdfDoc& d, const Params&, const std::string& id) -> Result {
                           std::string t = id + ": " + d.summary() + pageSizes(d.data);
                           const PdfFile* f = d.data.files.empty() ? nullptr : d.data.files[0].get();
                           for (const char* key : {"Title", "Author", "Subject", "Keywords"}) {
                               auto it = d.data.info.find(key);
                               std::string v = it != d.data.info.end() ? it->second : f ? f->infoField(key) : "";
                               if (!v.empty()) t += "\n" + utf8Lower(key) + ": " + v;
                           }
                           if (!d.fields().empty()) t += "\n" + std::to_string(d.fields().size()) + " form field(s): pdf.fields lists them";
                           Result r = Result::success(t, static_cast<std::int64_t>(d.data.pages.size()));
                           r.with("pages", static_cast<std::int64_t>(d.data.pages.size())).with("fields", static_cast<std::int64_t>(d.fields().size()));
                           if (!d.data.pages.empty()) {
                               const PageGeom g = geometry(d.data, 0);
                               r.with("width", g.width).with("height", g.height);
                           }
                           return r;
                       },
                       false));

    reg.add({"pdf.view", "View Page",
             "A picture of one page with every edit, as PNG (attached, and written to output= when given). Coordinates in other pdf "
             "commands are points from the page's top-left corner; width/height in the answer give the page size in points.",
             {docSpec(), pageSpec(), number("dpi", "picture resolution", false, 110.0), str("output", "also write the PNG here", false)}, false, true},
            docCommand(session,
                       [](PdfDoc& d, const Params& p, const std::string&) -> Result {
                           const int page = pageParam(d.data, p);
                           const double dpi = std::clamp(numberParam(p, "dpi", 110), 10.0, 600.0);
                           const PageGeom g = geometry(d.data, page);
                           const raster::Image img = renderPage(d.data, page, dpi / 72.0);
                           Attachment a{"image/png", raster::encodePng(img), "page-" + std::to_string(page + 1) + ".png"};
                           if (has(p, "output")) writeBytes(stringParam(p, "output"), a.bytes);
                           Result r = Result::success("page " + std::to_string(page + 1) + " of " + std::to_string(d.data.pages.size()) + " (" + fmt(g.width) +
                                                      " x " + fmt(g.height) + " pt)");
                           r.with("page", static_cast<std::int64_t>(page + 1))
                               .with("pages", static_cast<std::int64_t>(d.data.pages.size()))
                               .with("width", g.width)
                               .with("height", g.height)
                               .with("pixels_width", static_cast<std::int64_t>(img.width()))
                               .with("pixels_height", static_cast<std::int64_t>(img.height()));
                           r.attach(std::move(a));
                           return r;
                       },
                       false));

    reg.add({"pdf.text", "Read Text", "The text of the pages (all, or pages=\"2-4\"), line by line. Scanned pages without a text layer come back empty (pdf.ocr reads them).",
             {docSpec(), str("pages", "pages to read, e.g. 1-3,5 or last", false, std::string("all")), flag("boxes", "add each line's x, y, w, h", false)},
             false, true},
            docCommand(session,
                       [](PdfDoc& d, const Params& p, const std::string&) -> Result {
                           std::string out, rows;
                           for (int i : pagesParam(d.data, p)) {
                               const PageGeom g = geometry(d.data, i);
                               const auto chars = charsOf(d.data, i);
                               out += (out.empty() ? "" : "\n\n") + std::string("--- page ") + std::to_string(i + 1) + " ---";
                               for (const auto& l : textLines(chars)) {
                                   const auto b = boxFromBase(g, l.x0, l.y0, l.x1 - l.x0, l.y1 - l.y0);
                                   out += "\n" + l.text;
                                   if (boolParam(p, "boxes", false)) out += "    [" + fmt(b[0]) + ", " + fmt(b[1]) + ", " + fmt(b[2]) + ", " + fmt(b[3]) + "]";
                                   rows += std::to_string(i + 1) + "\t" + fmt(b[0]) + "\t" + fmt(b[1]) + "\t" + fmt(b[2]) + "\t" + fmt(b[3]) + "\t" + l.text + "\n";
                               }
                           }
                           return Result::success(out, out).with("rows", rows);
                       },
                       false));

    reg.add({"pdf.find", "Find Text", "Where some text appears: page, x, y, w, h of each match (rows), for pdf.highlight or pdf.redact.",
             {docSpec(), str("text", "what to look for", true), flag("match_case", "Aa must match", false), integer("page", "only this page (0 = all)", false, std::int64_t{0})},
             false, true},
            docCommand(session,
                       [](PdfDoc& d, const Params& p, const std::string&) -> Result {
                           const std::string q = stringParam(p, "text");
                           const auto onePage = intParam(p, "page", 0);
                           std::string rows, out;
                           int count = 0;
                           for (std::size_t i = 0; i < d.data.pages.size(); ++i) {
                               if (onePage > 0 && static_cast<std::int64_t>(i) != onePage - 1) continue;
                               const PageGeom g = geometry(d.data, static_cast<int>(i));
                               for (const auto& m : findText(charsOf(d.data, static_cast<int>(i)), q, boolParam(p, "match_case", false))) {
                                   for (const auto& b : m.boxes) {
                                       const auto s = boxFromBase(g, b[0], b[1], b[2], b[3]);
                                       rows += std::to_string(i + 1) + "\t" + fmt(s[0]) + "\t" + fmt(s[1]) + "\t" + fmt(s[2]) + "\t" + fmt(s[3]) + "\t" + m.text + "\n";
                                       if (count < 40) out += "\n  page " + std::to_string(i + 1) + " at " + fmt(s[0]) + ", " + fmt(s[1]) + " (" + fmt(s[2]) + " x " + fmt(s[3]) + ")";
                                   }
                                   ++count;
                               }
                           }
                           return Result::success(std::to_string(count) + " match" + (count == 1 ? "" : "es") + " for \"" + q + "\"" + out,
                                                  static_cast<std::int64_t>(count))
                               .with("total", static_cast<std::int64_t>(count))
                               .with("rows", rows);
                       },
                       false));

    reg.add({"pdf.fields", "Form Fields", "The form's fields: name, type, value, choices and page (rows), and where their boxes are (widgets: name, page, x, y, w, h). Fill them with pdf.fill.", {docSpec()}, false, true},
            docCommand(session,
                       [](PdfDoc& d, const Params&, const std::string&) -> Result {
                           const auto& fields = d.fields();
                           if (fields.empty()) return Result::success("this PDF has no form fields", std::int64_t{0}).with("rows", std::string());
                           std::string out, rows;
                           for (const auto& f : fields) {
                               auto v = d.data.values.find(f.name);
                               const std::string value = v != d.data.values.end() ? v->second : f.value;
                               std::string opts;
                               for (const auto& o : f.options) opts += (opts.empty() ? "" : "|") + o;
                               const int page = f.widgets.empty() ? -1 : f.widgets[0].page;
                               out += "\n" + f.name + " (" + f.type + (f.readOnly ? ", read-only" : "") + ")" + (value.empty() ? "" : " = " + value) +
                                      (opts.empty() ? "" : "  [" + opts + "]");
                               rows += f.name + "\t" + f.type + "\t" + value + "\t" + opts + "\t" + std::to_string(page + 1) + "\n";
                           }
                           // where each box sits on the pages as shown now: name, page, x, y, w, h
                           std::string widgets;
                           for (std::size_t i = 0; i < d.data.pages.size(); ++i) {
                               const PageEntry& e = d.data.pages[i];
                               if (e.file != 0) continue;
                               const PageGeom g = geometry(d.data, static_cast<int>(i));
                               for (const auto& f : fields) {
                                   for (const auto& w : f.widgets) {
                                       if (w.page != e.index) continue;
                                       double xs[2], ys[2];
                                       g.userToBase.apply(w.rect[0], w.rect[1], xs[0], ys[0]);
                                       g.userToBase.apply(w.rect[2], w.rect[3], xs[1], ys[1]);
                                       const auto b = boxFromBase(g, std::min(xs[0], xs[1]), std::min(ys[0], ys[1]), std::abs(xs[1] - xs[0]), std::abs(ys[1] - ys[0]));
                                       widgets += f.name + "\t" + std::to_string(i + 1) + "\t" + fmt(b[0]) + "\t" + fmt(b[1]) + "\t" + fmt(b[2]) + "\t" + fmt(b[3]) + "\n";
                                   }
                               }
                           }
                           return Result::success(std::to_string(fields.size()) + " field(s):" + out, static_cast<std::int64_t>(fields.size()))
                               .with("rows", rows)
                               .with("widgets", widgets);
                       },
                       false));

    reg.add({"pdf.fill", "Fill Form",
             "Fill in form fields: name= and value=, or several at once as values=\"Name=Ann; Agree=Yes\". Checkboxes take Yes/No "
             "(or their on-state name), radio buttons and lists one of their choices.",
             {docSpec(), str("name", "the field (pdf.fields lists them)", false), str("value", "what to put in it", false),
              str("values", "several: name=value pairs separated by ; or new lines", false)},
             true, false},
            docCommand(session,
                       [](PdfDoc& d, const Params& p, const std::string&) -> Result {
                           std::vector<std::pair<std::string, std::string>> pairs;
                           if (has(p, "name")) pairs.emplace_back(stringParam(p, "name"), stringParam(p, "value"));
                           if (has(p, "values")) {
                               std::string all = stringParam(p, "values");
                               std::replace(all.begin(), all.end(), '\n', ';');
                               std::size_t pos = 0;
                               while (pos <= all.size()) {
                                   std::size_t end = all.find(';', pos);
                                   std::string item = all.substr(pos, end == std::string::npos ? std::string::npos : end - pos);
                                   auto eq = item.find('=');
                                   if (eq != std::string::npos) {
                                       auto trim = [](std::string s) {
                                           while (!s.empty() && s.front() == ' ') s.erase(0, 1);
                                           while (!s.empty() && s.back() == ' ') s.pop_back();
                                           return s;
                                       };
                                       pairs.emplace_back(trim(item.substr(0, eq)), trim(item.substr(eq + 1)));
                                   }
                                   if (end == std::string::npos) break;
                                   pos = end + 1;
                               }
                           }
                           if (pairs.empty()) return Result::failure("give name= and value=, or values=\"name=value; ...\"");
                           std::string out;
                           for (auto [name, value] : pairs) {
                               const FieldInfo* fi = nullptr;
                               for (const auto& f : d.fields()) {
                                   if (f.name == name) fi = &f;
                               }
                               if (!fi) {
                                   for (const auto& f : d.fields()) {
                                       if (utf8Lower(f.name) == utf8Lower(name)) fi = &f;
                                   }
                               }
                               if (!fi) return Result::failure("no field called '" + name + "' (pdf.fields lists them)");
                               if (fi->readOnly) return Result::failure("'" + fi->name + "' is read-only");
                               if (fi->type == "button" || fi->type == "signature")
                                   return Result::failure("'" + fi->name + "' is a " + fi->type + " field; use pdf.sign for signatures");
                               if (fi->type == "checkbox" || fi->type == "radio") {
                                   const std::string l = utf8Lower(value);
                                   if (l == "no" || l == "off" || l == "false" || l == "0" || l.empty()) value = "Off";
                                   else if (std::find(fi->options.begin(), fi->options.end(), value) == fi->options.end()) {
                                       std::string match;
                                       for (const auto& o : fi->options) {
                                           if (utf8Lower(o) == l) match = o;
                                       }
                                       if (match.empty() && fi->type == "checkbox" && !fi->options.empty() &&
                                           (l == "yes" || l == "on" || l == "true" || l == "1" || l == "x"))
                                           match = fi->options[0];
                                       if (match.empty()) {
                                           std::string opts;
                                           for (const auto& o : fi->options) opts += (opts.empty() ? "" : ", ") + o;
                                           return Result::failure("'" + fi->name + "' takes one of: " + opts + " (or Off)");
                                       }
                                       value = match;
                                   }
                               }
                               d.data.values[fi->name] = value;
                               out += (out.empty() ? "" : ", ") + fi->name + " = " + value;
                           }
                           return Result::success("filled " + out, static_cast<std::int64_t>(pairs.size())).with("count", static_cast<std::int64_t>(pairs.size()));
                       },
                       true));

    reg.add({"pdf.metadata", "Document Info", "Set the title, author, subject or keywords saved in the file (blank ones stay as they are).",
             {docSpec(), str("title", "title", false), str("author", "author", false), str("subject", "subject", false), str("keywords", "keywords", false)},
             true, false},
            docCommand(session,
                       [](PdfDoc& d, const Params& p, const std::string&) -> Result {
                           std::string out;
                           for (const auto& [key, name] : std::vector<std::pair<std::string, std::string>>{
                                    {"title", "Title"}, {"author", "Author"}, {"subject", "Subject"}, {"keywords", "Keywords"}}) {
                               if (!has(p, key)) continue;
                               d.data.info[name] = stringParam(p, key);
                               out += (out.empty() ? "" : ", ") + key + " = " + stringParam(p, key);
                           }
                           if (out.empty()) return Result::failure("give title=, author=, subject= or keywords=");
                           return Result::success("set " + out);
                       },
                       true));
}

}  // namespace wetwaresuite::pdf::tool

namespace wetwaresuite::pdf {

void registerCommands(Session& session) {
    tool::registerFileCommands(session);
    tool::registerEditCommands(session);
    tool::registerCoverCommands(session);
    tool::registerMarkCommands(session);
    tool::registerPageCommands(session);
    tool::registerSignCommands(session);
    tool::registerOcrCommands(session);
}

}  // namespace wetwaresuite::pdf
