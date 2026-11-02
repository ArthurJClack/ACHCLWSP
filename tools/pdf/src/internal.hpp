// The pdf tool's document: an open PDF (or new pages) plus the edits made to
// it. Edits stay separate until pdf.save writes a new file, so every one can
// be moved, changed or deleted, and undo restores the whole document.
#pragma once

#include <array>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "wetwaresuite/core/command.hpp"
#include "wetwaresuite/pdf/page.hpp"
#include "wetwaresuite/pdf/reader.hpp"
#include "wetwaresuite/pdf/view.hpp"
#include "wetwaresuite/raster/image.hpp"

namespace wetwaresuite::pdf::tool {

namespace fs = std::filesystem;
using raster::Color;

// Something added to a page. Coordinates are points from the top-left corner of the
// page as its file shows it (before any turn added here).
struct Mark {
    int id = 0;
    std::string kind;  // text, rect, ellipse, line, arrow, ink, highlight, underline, strikeout, note, image, whiteout, redact, signature
    double x = 0, y = 0, w = 0, h = 0;
    std::vector<double> points;                  // ink, line, arrow: x1,y1,x2,y2,...
    std::vector<std::array<double, 4>> boxes;    // highlight/underline/strikeout: x, y, w, h per piece
    std::vector<std::vector<double>> strokes;    // ink and drawn signatures: several strokes
    std::string text;                            // text, note, highlight comment, signature name
    double size = 12;
    Font font;
    Color color{0, 0, 0, 255};
    std::optional<Color> fill;
    double width = 1.5;
    double opacity = 1;
    std::shared_ptr<const ImageData> image;
    std::string author;

    bool isAnnotation() const;  // written as a PDF annotation (else drawn into the page)
};

struct PageEntry {
    int file = 0;   // index into PdfDoc::files; -1: a blank page
    int index = 0;  // page in that file
    double width = 595.28, height = 841.89;  // blank pages
    int rotate = 0;  // turn added here, clockwise
    std::vector<Mark> marks;
    std::shared_ptr<const std::string> content;  // the page's content after text was taken out
    std::set<int> dropAnnots;  // the file's own annotations deleted here (object numbers)
};

struct FieldInfo {
    std::string name;   // full name, parts joined with '.'
    std::string type;   // text, checkbox, radio, choice, button, signature
    std::string value;
    std::vector<std::string> options;  // choices, or the on-states of a checkbox/radio
    bool readOnly = false, multiline = false;
    int fieldObject = 0;  // the object holding /V
    struct Widget {
        int object = 0;
        int page = -1;  // in the file
        double rect[4] = {0, 0, 0, 0};  // user space
    };
    std::vector<Widget> widgets;
};

struct DocData {
    std::vector<std::shared_ptr<const PdfFile>> files;  // [0] is the opened file (null for a new document)
    std::vector<PageEntry> pages;
    std::map<std::string, std::string> values;  // form fields filled in here
    std::map<std::string, std::string> info;    // Title, Author, Subject, Keywords set here
    int nextMark = 1;
    // password protection for saved copies (opened with a password: kept, so saving doesn't unlock it)
    bool protect = false;
    std::string userPassword, ownerPassword;
    bool allowPrint = true, allowCopy = true, allowEdit = true;
    // a digital signature added when it's saved (it has to be the last thing written)
    bool digital = false;
    std::string identityPath, reason, location, contact;
    int signPage = 0;
};

class PdfDoc : public Document {
public:
    using Data = DocData;
    static constexpr const char* kKind = "pdf";
    Data data;
    std::string kind() const override { return kKind; }
    std::string summary() const override;
    std::unique_ptr<Attachment> preview() const override;

    // cached per file: form fields
    const std::vector<FieldInfo>& fields() const;

private:
    mutable std::vector<FieldInfo> fields_;
    mutable const PdfFile* fieldsFor_ = nullptr;
};

// ---- geometry ----------------------------------------------------------------------

struct PageGeom {
    double width = 0, height = 0;  // as shown now (with the turn added here)
    double baseWidth = 0, baseHeight = 0;  // as the file shows it
    int turn = 0;                   // turn added here
    Matrix userToBase;              // file user space -> base page points (identity-ish for blank pages)
};
PageGeom geometry(const DocData& d, int page);
// page points as shown now <-> base page points
void toBase(const PageGeom& g, double x, double y, double& bx, double& by);
void fromBase(const PageGeom& g, double bx, double by, double& x, double& y);

// ---- drawing -----------------------------------------------------------------------

// The page with its edits, `scale` pixels per point, as shown now.
raster::Image renderPage(const DocData& d, int page, double scale);
void drawMark(raster::Image& img, const Mark& m, double scale);
// Text lines of a text mark (wrapped to its width when it has one).
std::vector<std::string> markLines(const Mark& m);
double lineHeight(const Mark& m);

// ---- text ---------------------------------------------------------------------------

std::vector<TextChar> charsOf(const DocData& d, int page);  // in base page points

// ---- forms ---------------------------------------------------------------------------

std::vector<FieldInfo> readFields(const PdfFile& f);

// ---- saving --------------------------------------------------------------------------

// The finished file; `pages` (0-based, in order) limits it to those pages.
std::string saveBytes(const DocData& d, bool flatten, const std::vector<int>* pages = nullptr);

// ---- helpers -------------------------------------------------------------------------

fs::path pathFromUtf8(const std::string& s);
std::string readBytes(const std::string& path);
void writeBytes(const std::string& path, const std::string& bytes);
// The signing identity's PEM file: identity= if given, else the suite's own (made on first use).
std::filesystem::path defaultIdentityPath();
// The finished file's bytes: password protection and the digital signature put on.
std::string finishBytes(const DocData& d, std::string bytes);
std::optional<Color> parseColor(const std::string& text);
std::string colorText(Color c);
std::string fmt(double v);  // short number text
std::string utf8Lower(std::string s);

}  // namespace wetwaresuite::pdf::tool
