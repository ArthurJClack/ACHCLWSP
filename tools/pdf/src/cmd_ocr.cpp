// Reading the text of scanned pages: each page is drawn at scanner resolution, read with the
// built-in recognizer and given an invisible text layer, so it can be searched and copied.
#include <algorithm>
#include <cmath>

#include "commands.hpp"
#include "wetwaresuite/pdf/ocr.hpp"

namespace wetwaresuite::pdf::tool {

void registerOcrCommands(Session& session) {
    auto& reg = session.registry();
    reg.add({"pdf.ocr", "Read Scanned Text",
             "Read the text of scanned pages (OCR) and add it as an invisible layer over the picture, so the PDF can be searched, "
             "selected and copied like any other. Pages that already have text are skipped unless force=true. Works offline on "
             "printed text in ordinary fonts; returns what it read.",
             {docSpec(), str("pages", "only these pages, e.g. 1-3 (default: all)", false), flag("force", "also pages that already have text", false),
              number("dpi", "resolution to read at (default 300; more helps tiny print)", false, 300.0)},
             true, false},
            docCommand(session,
                       [](PdfDoc& d, const Params& p, const std::string& id) -> Result {
                           std::vector<int> pages;
                           if (has(p, "pages")) pages = pagesParam(d.data, p);
                           else
                               for (int i = 0; i < static_cast<int>(d.data.pages.size()); ++i) pages.push_back(i);
                           const bool force = boolParam(p, "force", false);
                           const double dpi = std::clamp(numberParam(p, "dpi", 300), 100.0, 600.0);
                           std::string text;
                           int read = 0, skipped = 0, words = 0;
                           double confidence = 0;
                           for (int page : pages) {
                               PageEntry& e = d.data.pages[static_cast<std::size_t>(page)];
                               if (e.file < 0) continue;
                               // pages with a text layer of their own (or one read before) don't need it
                               e.marks.erase(std::remove_if(e.marks.begin(), e.marks.end(), [](const Mark& m) { return m.kind == "ocr"; }), e.marks.end());
                               if (!force && !charsOf(d.data, page).empty()) {
                                   ++skipped;
                                   continue;
                               }
                               const PdfFile& f = *d.data.files[static_cast<std::size_t>(e.file)];
                               RenderOptions opt;
                               opt.annotations = false;
                               double scale = dpi / 72.0;
                               const PageGeom g = geometry(d.data, page);
                               scale = std::min(scale, 7000.0 / std::max(g.baseWidth, g.baseHeight));
                               raster::Image img = renderPdfPage(f, e.index, scale, opt);
                               OcrResult r = recognizeText(img);
                               if (r.textHeight > 0 && r.textHeight < 22 && scale * 1.6 * std::max(g.baseWidth, g.baseHeight) < 9000) {
                                   scale *= 1.6;  // small print: look closer
                                   img = renderPdfPage(f, e.index, scale, opt);
                                   r = recognizeText(img);
                               }
                               Mark m;
                               m.kind = "ocr";
                               for (const auto& line : r.lines) {
                                   for (const auto& w : line.words) {
                                       m.boxes.push_back({w.x / scale, w.y / scale, w.w / scale, w.h / scale});
                                       m.points.push_back(w.baseline / scale);
                                       m.points.push_back(std::max(1.0, r.textHeight / scale / 0.72));
                                       m.text += (m.text.empty() ? "" : "\n") + w.text;
                                       ++words;
                                   }
                               }
                               if (!m.boxes.empty()) {
                                   m.x = 0, m.y = 0, m.w = g.baseWidth, m.h = g.baseHeight;
                                   addMark(d.data, page, m);
                               }
                               confidence += r.confidence;
                               ++read;
                               text += (text.empty() ? "" : "\n\n") + std::string("--- page ") + std::to_string(page + 1) + " ---\n" + r.text;
                           }
                           if (!read && skipped) return Result::success(id + ": every page already has text (force=true reads them anyway)");
                           std::string head = id + ": read " + std::to_string(words) + " words on " + std::to_string(read) + " page" + (read == 1 ? "" : "s");
                           if (skipped) head += " (" + std::to_string(skipped) + " already had text)";
                           const double conf = read ? confidence / read : 0;
                           head += conf < 0.45 ? "; it reads poorly here, so check it (a sharper scan or dpi=400 helps)" : "";
                           return Result::success(head + "\n" + text, text)
                               .with("words", static_cast<std::int64_t>(words))
                               .with("pages", static_cast<std::int64_t>(read))
                               .with("confidence", std::round(conf * 100) / 100);
                       },
                       true));
}

}  // namespace wetwaresuite::pdf::tool
