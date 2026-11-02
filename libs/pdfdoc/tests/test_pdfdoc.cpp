#include <cmath>
#include "wetwaresuite/pdfdoc/fonts.hpp"
#include "wetwaresuite/pdfdoc/writer.hpp"
#include "check.hpp"

using namespace wetwaresuite;
using namespace wetwaresuite::pdfdoc;

int main() {
    // encoding
    CHECK(winAnsi('A') == 'A');
    CHECK(winAnsi(0x20AC) == 128);   // euro
    CHECK(winAnsi(0x2014) == 151);   // em dash
    CHECK(winAnsi(0xE9) == 0xE9);    // é
    CHECK(winAnsi(0x4E2D) == -1);
    CHECK(unicodeOf(147) == 0x201C);
    CHECK(toWinAnsi("caf\xC3\xA9 \xE2\x80\x94") == "caf\xE9 \x97");
    CHECK(utf8Decode(utf8Encode(0x2019)).at(0) == 0x2019);

    // standard fonts
    CHECK(standardFamily("Arial") == std::optional<std::string>("helvetica"));
    CHECK(standardFamily("serif") == std::optional<std::string>("times"));
    CHECK(standardFamily("Mono") == std::optional<std::string>("courier"));
    CHECK(!standardFamily("Comic Sans"));
    CHECK(std::string(baseFontName(standardFont("times", true, true))) == "Times-BoldItalic");
    CHECK(std::string(baseFontName(standardFont("helvetica", false, true))) == "Helvetica-Oblique");
    CHECK(standardWidth(Standard::Helvetica, 'W') == 944);
    CHECK(standardWidth(Standard::TimesRoman, 'a') == 444);
    CHECK(standardWidth(Standard::CourierBold, 'i') == 600);
    CHECK(standardWidth(Standard::HelveticaBold, 'b') > standardWidth(Standard::Helvetica, 'b'));
    CHECK(standardMetrics(Standard::TimesRoman).ascent == 683);

    // writer
    Writer w;
    int pages = w.reserve();
    int font = addStandardFont(w, Standard::Helvetica);
    int content = w.addStream("", "BT /F1 12 Tf 72 720 Td " + text("Hello \xE2\x80\x94 (world)") + " Tj ET");
    int page = w.add("<< /Type /Page /Parent " + std::to_string(pages) + " 0 R /MediaBox [0 0 595 842] /Resources << /Font << /F1 " +
                     std::to_string(font) + " 0 R >> >> /Contents " + std::to_string(content) + " 0 R >>");
    w.set(pages, "<< /Type /Pages /Kids [" + std::to_string(page) + " 0 R] /Count 1 >>");
    int root = w.add("<< /Type /Catalog /Pages " + std::to_string(pages) + " 0 R >>");
    std::string pdf = w.finish(root, 0);
    CHECK(pdf.rfind("%PDF-1.4", 0) == 0);
    CHECK(pdf.find("/FlateDecode") != std::string::npos);
    CHECK(pdf.find("startxref") != std::string::npos);
    CHECK(pdf.find("/BaseFont /Helvetica") != std::string::npos);
    CHECK(literal("a(b)\\") == "(a\\(b\\)\\\\)");
    CHECK(num(1.5) == "1.5" && num(2.0) == "2" && num(-0.00001) == "0");
    std::string z = deflate(std::string(1000, 'x'));
    CHECK(z.size() < 100 && static_cast<unsigned char>(z[0]) == 0x78);

    // images: an alpha channel adds a soft mask
    raster::Image img(4, 3, {255, 0, 0, 128});
    Writer wi;
    addImage(wi, img);
    std::string ipdf = wi.finish(1, 0);
    CHECK(ipdf.find("/SMask") != std::string::npos);
    CHECK(addJpeg(wi, "not a jpeg") == 0);

    // system fonts are optional; when present they must work
    if (auto tt = systemFont("helvetica", false, false)) {
        CHECK(tt->hasGlyph('A'));
        CHECK(tt->advance('A') > 400 && tt->advance('A') < 900);
        CHECK(!tt->outline('O').empty());
        CHECK(tt->outline('O').size() >= 2);  // outer and inner contour
        CHECK(!tt->postscriptName().empty());
        if (tt->embeddable()) {
            Writer wf;
            addTrueTypeFont(wf, *tt);
            std::string fpdf = wf.finish(1, 0);
            CHECK(fpdf.find("/FontFile2") != std::string::npos);
            CHECK(fpdf.find("/Subtype /TrueType") != std::string::npos);
        }
    }
    // the built-in font: subsets keep only the glyphs asked for, with the same numbers
    {
        auto sans = builtinSans();
        const int zhe = sans->glyphIndex(0x0416), omega = sans->glyphIndex(0x03A9), a = sans->glyphIndex('a');
        CHECK(zhe > 0 && omega > 0 && a > 0 && sans->glyphIndex(0x4E2D) == 0);
        const std::string sub = sans->subset({zhe, omega});
        CHECK(sub.size() < sans->bytes().size() / 4);
        auto small = TrueType::fromBytes(sub, "subset");
        CHECK(small->glyphIndex(0x0416) == zhe && !small->outline(0x0416).empty() && small->outline('a').empty());
        CHECK(small->glyphCount() == sans->glyphCount());
        CHECK(small->glyphAdvance(zhe) == sans->glyphAdvance(zhe));
    }
    {
        Writer wu;
        int f = addUnicodeFont(wu, *builtinSans(), {0x0416, 0x03A9, 'A'});
        CHECK(f > 0);
        std::string upd = wu.finish(1, 0);
        CHECK(upd.find("/Subtype /Type0") != std::string::npos && upd.find("/Identity-H") != std::string::npos);
        CHECK(upd.find("/ToUnicode") != std::string::npos && upd.find("/CIDToGIDMap /Identity") != std::string::npos);
        CHECK(upd.find("+Roboto") != std::string::npos);
        CHECK(upd.size() < 30000);
        const std::string hex = glyphText(*builtinSans(), "\xD0\x96" "A");
        CHECK(hex.size() == 10 && hex.front() == '<' && hex.back() == '>');
    }
    // compact subsets: renumbered glyphs, per-glyph tables shrink
    {
        const auto& f = *builtinSans();
        const int ga = f.glyphIndex('A'), gz = f.glyphIndex(0x0416);
        std::vector<int> oldOf;
        const std::string sub = f.subsetCompact({ga, gz}, oldOf);
        CHECK(oldOf.size() >= 3 && oldOf[0] == 0);
        auto t = TrueType::fromBytes(sub, "compact");
        CHECK(t->glyphCount() == static_cast<int>(oldOf.size()));
        for (std::size_t n = 0; n < oldOf.size(); ++n) CHECK(std::abs(t->glyphAdvance(static_cast<int>(n)) - f.glyphAdvance(oldOf[n])) < 0.01);
        CHECK(sub.size() < f.subset({ga, gz}).size());
    }
    // fallback fonts: the built-in one first, system fonts only for what it lacks
    CHECK(unicodeFontIndex(0x03A9) == 0 && needsUnicode(0x03A9));  // Greek: built in
    CHECK(!needsUnicode('A'));
    CHECK(unicodeFontAt(0) == builtinSans() && unicodeFontAt(9999) == nullptr);
    if (unicodeFontIndex(0x4E2D) > 0) {  // a system font with Chinese (most machines have one)
        CHECK(needsUnicode(0x4E2D) && unicodeFontFor(0x4E2D)->embeddable());
        Writer wc;
        UnicodeFallback uni(wc);
        const std::string ops = uni.show("A \xCE\xA9 \xE4\xB8\xAD\xE6\x96\x87", "F1", 12);
        CHECK(ops.find("/FU 12 Tf") != std::string::npos && ops.find("/FU" + std::to_string(unicodeFontIndex(0x4E2D)) + " 12 Tf") != std::string::npos);
        CHECK(uni.resources().size() == 2);
        uni.finish();
        const std::string pdf = wc.finish(1, 0);
        CHECK(pdf.find("/CIDToGIDMap /Identity") != std::string::npos);  // Roboto: small, kept as is
        CHECK(pdf.find("/CIDToGIDMap " ) != std::string::npos && pdf.size() < 60000);  // the CJK font: a compact subset
    }
    return check::summary("pdfdoc");
}
