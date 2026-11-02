// PDF export: pages with bleed and crop marks, CMYK colours as DeviceCMYK,
// standard or embedded fonts, images (JPEG passed through), vector art, and
// N-up imposition for cards and labels.
#include <algorithm>
#include <map>
#include <set>

#include "wetwaresuite/pdfdoc/writer.hpp"
#include "wetwaresuite/vector/vector.hpp"
#include "internal.hpp"

namespace wetwaresuite::layout {

static std::string pn(double v) { return pdfdoc::num(v); }

namespace {

std::string cm(const Affine& m) {
    return pn(m.a) + " " + pn(m.b) + " " + pn(m.c) + " " + pn(m.d) + " " + pn(m.e) + " " + pn(m.f) + " cm\n";
}

std::string rectPath(double x, double y, double w, double h) {
    return pn(x) + " " + pn(y) + " " + pn(w) + " " + pn(h) + " re\n";
}

// Bezier ellipse / rounded rectangle paths in the current (frame-local mm) space.
std::string ellipsePath(double cx, double cy, double rx, double ry) {
    const double k = 0.5522847498;
    auto p = [](double x, double y) { return pn(x) + " " + pn(y); };
    std::string s = p(cx + rx, cy) + " m\n";
    s += p(cx + rx, cy + ry * k) + " " + p(cx + rx * k, cy + ry) + " " + p(cx, cy + ry) + " c\n";
    s += p(cx - rx * k, cy + ry) + " " + p(cx - rx, cy + ry * k) + " " + p(cx - rx, cy) + " c\n";
    s += p(cx - rx, cy - ry * k) + " " + p(cx - rx * k, cy - ry) + " " + p(cx, cy - ry) + " c\n";
    s += p(cx + rx * k, cy - ry) + " " + p(cx + rx, cy - ry * k) + " " + p(cx + rx, cy) + " c\nh\n";
    return s;
}

std::string roundRectPath(double x, double y, double w, double h, double r) {
    r = std::clamp(r, 0.0, std::min(std::abs(w), std::abs(h)) / 2);
    if (r <= 0) return rectPath(x, y, w, h);
    const double k = 0.5522847498 * r;
    auto p = [](double px, double py) { return pn(px) + " " + pn(py); };
    std::string s = p(x + r, y) + " m\n" + p(x + w - r, y) + " l\n";
    s += p(x + w - r + k, y) + " " + p(x + w, y + r - k) + " " + p(x + w, y + r) + " c\n" + p(x + w, y + h - r) + " l\n";
    s += p(x + w, y + h - r + k) + " " + p(x + w - r + k, y + h) + " " + p(x + w - r, y + h) + " c\n" + p(x + r, y + h) + " l\n";
    s += p(x + r - k, y + h) + " " + p(x, y + h - r + k) + " " + p(x, y + h - r) + " c\n" + p(x, y + r) + " l\n";
    s += p(x, y + r - k) + " " + p(x + r - k, y) + " " + p(x + r, y) + " c\nh\n";
    return s;
}

class Builder {
public:
    Builder(const LayoutDoc& doc, bool embedFonts) : doc_(doc), embed_(embedFonts) { resources_ = w_.reserve(); }

    pdfdoc::Writer& writer() { return w_; }
    int resources() const { return resources_; }

    // Colour operator for a paint ("rg"/"k" for fill, "RG"/"K" for stroke); empty when none.
    std::string colour(const std::string& spec, bool stroke, double opacity) {
        auto p = resolvePaint(doc_, spec);
        if (!p || p->none) return {};
        std::string s;
        const double alpha = std::clamp(opacity * p->rgb.a / 255.0, 0.0, 1.0);
        if (alpha < 0.999) s += "/" + alphaState(alpha) + " gs\n";
        if (p->cmyk) {
            usedCmyk = true;
            s += pn(p->c) + " " + pn(p->m) + " " + pn(p->y) + " " + pn(p->k) + (stroke ? " K\n" : " k\n");
        } else {
            usedRgb = true;
            s += pn(p->rgb.r / 255.0) + " " + pn(p->rgb.g / 255.0) + " " + pn(p->rgb.b / 255.0) + (stroke ? " RG\n" : " rg\n");
        }
        return s;
    }
    std::string colour(Color c, bool stroke, double opacity) {
        std::string s;
        const double alpha = std::clamp(opacity * c.a / 255.0, 0.0, 1.0);
        if (alpha < 0.999) s += "/" + alphaState(alpha) + " gs\n";
        usedRgb = true;
        return s + pn(c.r / 255.0) + " " + pn(c.g / 255.0) + " " + pn(c.b / 255.0) + (stroke ? " RG\n" : " rg\n");
    }

    std::string alphaState(double a) {
        const std::string key = num(a, 3);
        auto it = alphas_.find(key);
        if (it != alphas_.end()) return it->second;
        const std::string name = "GS" + std::to_string(alphas_.size() + 1);
        gsObjects_[name] = w_.add("<< /Type /ExtGState /ca " + key + " /CA " + key + " >>");
        alphas_[key] = name;
        return name;
    }

    std::string font(const Face& face) {
        auto it = fonts_.find(face.key);
        if (it != fonts_.end()) return it->second;
        const std::string name = "F" + std::to_string(fonts_.size() + 1);
        int id = 0;
        if (face.ttf) {
            id = pdfdoc::addTrueTypeFont(w_, *face.ttf);
            report.embedded.push_back(face.label);
        } else if (embed_) {
            auto tt = pdfdoc::systemFont(face.family, face.bold, face.italic);
            if (tt && tt->embeddable()) {
                const pdfdoc::Standard std = face.font;
                id = pdfdoc::addTrueTypeFont(w_, *tt, [std](int code) { return pdfdoc::standardWidth(std, code); });
                report.embedded.push_back(face.label + " (as " + tt->postscriptName() + ")");
            } else {
                report.warnings.push_back("no system font to embed for " + face.label + "; it stays a standard font");
            }
        }
        if (!id) id = pdfdoc::addStandardFont(w_, face.font);
        report.fonts.push_back(face.label);
        fontObjects_[name] = id;
        fonts_[face.key] = name;
        return name;
    }

    // Image XObject name, or empty when the file can't be read.
    std::string image(const std::string& path) {
        auto it = images_.find(path);
        if (it != images_.end()) return it->second;
        int id = 0;
        const std::string ext = lower(std::filesystem::path(path).extension().string());
        if (ext == ".jpg" || ext == ".jpeg") {
            try {
                id = pdfdoc::addJpeg(w_, readFile(path));
            } catch (const std::exception&) {
            }
        }
        if (!id) {
            if (auto img = loadImage(path)) id = pdfdoc::addImage(w_, *img);
        }
        std::string name;
        if (id) {
            name = "Im" + std::to_string(imageObjects_.size() + 1);
            imageObjects_[name] = id;
        }
        images_[path] = name;
        return name;
    }

    void finishResources() {
        std::string r = "<< /ProcSet [/PDF /Text /ImageB /ImageC]";
        auto dict = [&](const char* key, const std::map<std::string, int>& m) {
            if (m.empty()) return;
            r += std::string(" /") + key + " <<";
            for (const auto& [n, id] : m) r += " /" + n + " " + std::to_string(id) + " 0 R";
            r += " >>";
        };
        uni_.finish();
        for (const auto& [name, id] : uni_.resources()) fontObjects_[name] = id;
        dict("Font", fontObjects_);
        dict("XObject", imageObjects_);
        dict("ExtGState", gsObjects_);
        w_.set(resources_, r + " >>");
    }

    // ---- page content in page mm (y down) ----

    std::string text(const Frame& f, const FrameText& ft) {
        std::string s;
        for (const auto& line : ft.lines) {
            for (const auto& run : line.runs) {
                const std::string col = colour(run.color, false, f.opacity);
                if (col.empty()) continue;
                s += "q\n" + col + "BT\n/" + font(*run.face) + " " + pn(run.size) + " Tf\n";
                if (run.tracking != 0) s += pn(run.tracking) + " Tc\n";
                if (run.wordSpace != 0) s += pn(run.wordSpace) + " Tw\n";
                s += "1 0 0 -1 " + pn(run.x) + " " + pn(line.baseline) + " Tm\n" + uni_.show(run.text, font(*run.face), run.size) + "\nET\n";
                if (run.underline) s += rectPath(run.x, line.baseline + run.size * 0.1, run.width, std::max(run.size * 0.05, 0.1)) + "f\n";
                s += "Q\n";
            }
        }
        return s;
    }

    std::string vectorArt(const Frame& f, const Box& b) {
        auto v = loadVector(f.path);
        if (!v) return {};
        const double sx = b.w / std::max(1, v->width), sy = b.h / std::max(1, v->height);
        std::string s = "1 j 1 J\n";
        for (const auto& shape : v->shapes) {
            if (!shape.visible || shape.opacity <= 0) continue;
            const vector::Outline o = vector::outline(shape, 4.0);
            std::string path;
            for (const auto& c : o) {
                if (c.points.size() < 2) continue;
                for (std::size_t i = 0; i < c.points.size(); ++i) {
                    path += pn(b.x + c.points[i].x * sx) + " " + pn(b.y + c.points[i].y * sy) + (i ? " l\n" : " m\n");
                }
                if (c.closed) path += "h\n";
            }
            if (path.empty()) continue;
            const double op = f.opacity * shape.opacity;
            if (shape.fill && shape.fill->a > 0 && shape.kind != vector::ShapeKind::Line) s += "q\n" + colour(*shape.fill, false, op) + path + "f*\nQ\n";
            if (shape.stroke && shape.stroke->a > 0 && shape.strokeWidth > 0) {
                s += "q\n" + colour(*shape.stroke, true, op) + pn(shape.strokeWidth * std::sqrt(sx * sy)) + " w\n" + path + "S\nQ\n";
            }
        }
        return s;
    }

    std::string frame(const Frame& f, const FrameText* ft) {
        std::string s = "q\n" + cm(frameToPage(f));
        const double sw = f.strokeWidth * kPtToMm;
        std::string shape;
        switch (f.type) {
            case FrameType::Line: {
                const std::string col = colour(f.stroke.empty() ? "black" : f.stroke, true, f.opacity);
                if (!col.empty()) s += col + pn(sw) + " w\n0 0 m\n" + pn(f.w) + " " + pn(f.h) + " l\nS\n";
                return s + "Q\n";
            }
            case FrameType::Ellipse: shape = ellipsePath(f.w / 2, f.h / 2, std::abs(f.w) / 2, std::abs(f.h) / 2); break;
            case FrameType::Rect: shape = roundRectPath(0, 0, f.w, f.h, f.radius); break;
            default: shape = rectPath(0, 0, f.w, f.h); break;
        }
        const std::string fillCol = colour(f.fill, false, f.opacity);
        if (!fillCol.empty()) s += "q\n" + fillCol + shape + "f\nQ\n";
        if (f.type == FrameType::Image || f.type == FrameType::Vector) {
            const PictureInfo info = pictureInfo(f.path);
            if (info.ok) {
                const Box b = pictureBox(f, info);
                s += "q\n" + rectPath(0, 0, f.w, f.h) + "W n\n";
                if (info.vector) {
                    s += vectorArt(f, b);
                } else {
                    const std::string im = image(f.path);
                    if (!im.empty()) {
                        if (f.opacity < 0.999) s += "/" + alphaState(f.opacity) + " gs\n";
                        s += pn(b.w) + " 0 0 " + pn(-b.h) + " " + pn(b.x) + " " + pn(b.y + b.h) + " cm\n/" + im + " Do\n";
                    }
                }
                s += "Q\n";
            }
        }
        if (f.type == FrameType::Text && ft) s += text(f, *ft);
        const std::string strokeCol = colour(f.stroke, true, f.opacity);
        if (!strokeCol.empty() && sw > 0) s += "q\n" + strokeCol + pn(sw) + " w\n" + shape + "S\nQ\n";
        return s + "Q\n";
    }

    std::string page(int index, const std::map<std::string, StoryLayout>& layouts) {
        const Page& pg = doc_.pages[static_cast<std::size_t>(index)];
        std::string s;
        if (const Master* master = doc_.findMaster(pg.master)) {
            for (const auto& mf : master->frames) {
                const Frame f = placedMasterFrame(doc_, mf, index);
                if (f.type == FrameType::Text) {
                    auto it = doc_.stories.find(f.story);
                    if (it == doc_.stories.end()) continue;
                    StoryLayout sl = layoutStory(doc_, fieldsFilled(doc_, it->second, index), {&f}, index);
                    s += frame(f, &sl.frames[f.id]);
                } else {
                    s += frame(f, nullptr);
                }
            }
        }
        for (const auto& f : pg.frames) {
            const FrameText* ft = nullptr;
            if (f.type == FrameType::Text) {
                auto it = layouts.find(f.story);
                if (it != layouts.end()) {
                    auto t = it->second.frames.find(f.id);
                    if (t != it->second.frames.end()) ft = &t->second;
                }
            }
            s += frame(f, ft);
        }
        return s;
    }

    // Registration-colour hairline from a to b (sheet mm).
    static std::string mark(double x0, double y0, double x1, double y1) {
        return pn(x0) + " " + pn(y0) + " m\n" + pn(x1) + " " + pn(y1) + " l\nS\n";
    }

    int addPage(int pages, double mediaW, double mediaH, const std::string& content, const std::string& boxes) {
        const double k = kMmToPt;
        const int contents = w_.addStream("", content, compress);
        return w_.add("<< /Type /Page /Parent " + std::to_string(pages) + " 0 R /MediaBox [0 0 " + pn(mediaW * k) + " " +
                      pn(mediaH * k) + "]" + boxes + " /Resources " + std::to_string(resources_) + " 0 R /Contents " +
                      std::to_string(contents) + " 0 R >>");
    }

    std::string finish(int pages, const std::vector<int>& kids) {
        std::string list;
        for (int id : kids) list += (list.empty() ? "" : " ") + std::to_string(id) + " 0 R";
        w_.set(pages, "<< /Type /Pages /Kids [" + list + "] /Count " + std::to_string(kids.size()) + " >>");
        finishResources();
        const int root = w_.add("<< /Type /Catalog /Pages " + std::to_string(pages) + " 0 R >>");
        const int info = w_.add("<< /Title " + pdfdoc::infoText(doc_.title.empty() ? "layout" : doc_.title) +
                                " /Creator (wetwaresuite layout) /Producer (wetwaresuite) >>");
        report.pages = static_cast<int>(kids.size());
        if (usedRgb && usedCmyk) report.warnings.push_back("mixes RGB and CMYK colours");
        return w_.finish(root, info, "1.4");
    }

    PdfReport report;
    bool usedRgb = false, usedCmyk = false;
    bool compress = true;

private:
    const LayoutDoc& doc_;
    bool embed_ = false;
    pdfdoc::Writer w_;
    pdfdoc::UnicodeFallback uni_{w_};  // Greek, Cyrillic, ... in the built-in font
    int resources_ = 0;
    std::map<std::string, std::string> fonts_, images_, alphas_;
    std::map<std::string, int> fontObjects_, imageObjects_, gsObjects_;
};

std::string boxes(double x0, double y0, double x1, double y1, const char* name) {
    const double k = kMmToPt;
    return std::string(" /") + name + " [" + pn(x0 * k) + " " + pn(y0 * k) + " " + pn(x1 * k) + " " + pn(y1 * k) + "]";
}

std::vector<int> pageList(const LayoutDoc& doc, const std::vector<int>& pages) {
    std::vector<int> out;
    for (int p : pages) {
        if (p >= 0 && p < static_cast<int>(doc.pages.size())) out.push_back(p);
    }
    if (pages.empty()) {
        for (std::size_t i = 0; i < doc.pages.size(); ++i) out.push_back(static_cast<int>(i));
    }
    return out;
}

}  // namespace

std::string toPdf(const LayoutDoc& doc, const PdfOptions& options, PdfReport* report) {
    Builder b(doc, options.embedFonts);
    b.compress = options.compress;
    const int pagesId = b.writer().reserve();
    const auto layouts = layoutAll(doc);
    const double bleed = options.bleed ? doc.bleed : 0;
    const double gap = std::max(doc.bleed, 3.0), len = 5;
    const double off = options.marks ? gap + len + 2 : bleed;
    std::vector<int> kids;
    for (int index : pageList(doc, options.pages)) {
        const Page& pg = doc.pages[static_cast<std::size_t>(index)];
        const double mw = pg.width + 2 * off, mh = pg.height + 2 * off;
        // sheet mm, y down from the media's top-left corner
        std::string s = pn(kMmToPt) + " 0 0 " + pn(-kMmToPt) + " 0 " + pn(mh * kMmToPt) + " cm\n";
        s += "q\n1 0 0 1 " + pn(off) + " " + pn(off) + " cm\n";
        s += rectPath(-bleed, -bleed, pg.width + 2 * bleed, pg.height + 2 * bleed) + "W n\n";
        s += b.page(index, layouts) + "Q\n";
        if (options.marks) {
            s += "q\n1 1 1 1 K\n" + pn(0.25 * kPtToMm) + " w\n";
            const double x0 = off, y0 = off, x1 = off + pg.width, y1 = off + pg.height;
            for (double x : {x0, x1}) {
                s += Builder::mark(x, y0 - gap, x, y0 - gap - len) + Builder::mark(x, y1 + gap, x, y1 + gap + len);
            }
            for (double y : {y0, y1}) {
                s += Builder::mark(x0 - gap, y, x0 - gap - len, y) + Builder::mark(x1 + gap, y, x1 + gap + len, y);
            }
            s += "Q\n";
            auto face = resolveFace(doc, "helvetica", false, false);
            const std::string label = (doc.title.empty() ? std::string("layout") : doc.title) + "  page " + std::to_string(index + 1) +
                                      " of " + std::to_string(doc.pages.size());
            s += "q\n0 0 0 1 k\nBT\n/" + b.font(*face) + " " + pn(6 * kPtToMm) + " Tf\n1 0 0 -1 " + pn(x0) + " " +
                 pn(mh - 2) + " Tm\n" + pdfdoc::text(label) + " Tj\nET\nQ\n";
            b.usedCmyk = true;
        }
        const double t0x = off, t0y = off;  // trim box, PDF y up
        std::string bx = boxes(t0x, t0y, t0x + pg.width, t0y + pg.height, "TrimBox");
        bx += boxes(t0x - bleed, t0y - bleed, t0x + pg.width + bleed, t0y + pg.height + bleed, "BleedBox");
        kids.push_back(b.addPage(pagesId, mw, mh, s, bx));
    }
    std::string out = b.finish(pagesId, kids);
    if (report) *report = b.report;
    return out;
}

std::string imposePdf(const LayoutDoc& doc, const ImposeOptions& o, PdfReport* report) {
    const std::vector<int> pages = pageList(doc, o.pages);
    if (pages.empty()) throw std::runtime_error("no pages to impose");
    const Page& first = doc.pages[static_cast<std::size_t>(pages[0])];
    const double cw = first.width, ch = first.height;
    const double margin = o.marks ? 10 : 5;
    const double gap = std::max(0.0, o.gap);
    int cols = o.cols > 0 ? o.cols : static_cast<int>(std::floor((o.sheetW - 2 * margin + gap) / (cw + gap)));
    int rows = o.rows > 0 ? o.rows : static_cast<int>(std::floor((o.sheetH - 2 * margin + gap) / (ch + gap)));
    if (cols < 1 || rows < 1) throw std::runtime_error("a " + num(cw, 1) + " x " + num(ch, 1) + " mm page doesn't fit on the sheet");
    const double gridW = cols * cw + (cols - 1) * gap, gridH = rows * ch + (rows - 1) * gap;
    if (gridW > o.sheetW + 1e-6 || gridH > o.sheetH + 1e-6) throw std::runtime_error("that grid is bigger than the sheet");
    const double gx = (o.sheetW - gridW) / 2, gy = (o.sheetH - gridH) / 2;
    const double bleed = gap >= 2 * doc.bleed && doc.bleed > 0 ? doc.bleed : 0;
    const int perSheet = cols * rows;

    Builder b(doc, o.embedFonts);
    const int pagesId = b.writer().reserve();
    const auto layouts = layoutAll(doc);
    std::vector<int> kids;
    // which page goes in each slot, sheet by sheet
    std::vector<std::vector<int>> sheets;
    if (o.repeat) {
        for (int p : pages) sheets.push_back(std::vector<int>(static_cast<std::size_t>(perSheet), p));
    } else {
        for (std::size_t i = 0; i < pages.size(); i += static_cast<std::size_t>(perSheet)) {
            std::vector<int> slots;
            for (std::size_t k = i; k < pages.size() && k < i + static_cast<std::size_t>(perSheet); ++k) slots.push_back(pages[k]);
            sheets.push_back(slots);
        }
    }
    std::map<int, std::string> contentCache;
    for (const auto& slots : sheets) {
        std::string s = pn(kMmToPt) + " 0 0 " + pn(-kMmToPt) + " 0 " + pn(o.sheetH * kMmToPt) + " cm\n";
        for (std::size_t i = 0; i < slots.size(); ++i) {
            const int col = static_cast<int>(i) % cols, row = static_cast<int>(i) / cols;
            const double x = gx + col * (cw + gap), y = gy + row * (ch + gap);
            auto it = contentCache.find(slots[i]);
            if (it == contentCache.end()) it = contentCache.emplace(slots[i], b.page(slots[i], layouts)).first;
            s += "q\n1 0 0 1 " + pn(x) + " " + pn(y) + " cm\n" + rectPath(-bleed, -bleed, cw + 2 * bleed, ch + 2 * bleed) + "W n\n" +
                 it->second + "Q\n";
        }
        if (o.marks) {
            const double mgap = std::max(2.0, bleed + 1), len = std::max(2.0, std::min(6.0, std::min(gx, gy) - mgap - 0.5));
            s += "q\n1 1 1 1 K\n" + pn(0.25 * kPtToMm) + " w\n";
            std::set<long long> xs, ys;
            for (int c = 0; c < cols; ++c) {
                xs.insert(std::llround((gx + c * (cw + gap)) * 1000));
                xs.insert(std::llround((gx + c * (cw + gap) + cw) * 1000));
            }
            for (int r = 0; r < rows; ++r) {
                ys.insert(std::llround((gy + r * (ch + gap)) * 1000));
                ys.insert(std::llround((gy + r * (ch + gap) + ch) * 1000));
            }
            for (long long xi : xs) {
                const double x = xi / 1000.0;
                s += Builder::mark(x, gy - mgap, x, gy - mgap - len) + Builder::mark(x, gy + gridH + mgap, x, gy + gridH + mgap + len);
            }
            for (long long yi : ys) {
                const double y = yi / 1000.0;
                s += Builder::mark(gx - mgap, y, gx - mgap - len, y) + Builder::mark(gx + gridW + mgap, y, gx + gridW + mgap + len, y);
            }
            s += "Q\n";
            b.usedCmyk = true;
        }
        kids.push_back(b.addPage(pagesId, o.sheetW, o.sheetH, s, ""));
    }
    std::string out = b.finish(pagesId, kids);
    if (report) {
        *report = b.report;
        report->cols = cols;
        report->rows = rows;
    }
    return out;
}

}  // namespace wetwaresuite::layout
