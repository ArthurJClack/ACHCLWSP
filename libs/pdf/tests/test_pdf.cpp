#include <cmath>
#include <cstdio>
#include <stdexcept>

#include "wetwaresuite/pdfdoc/fonts.hpp"
#include "wetwaresuite/pdf/crypt.hpp"
#include "wetwaresuite/pdf/ocr.hpp"
#include "wetwaresuite/pdf/page.hpp"
#include "wetwaresuite/pdf/reader.hpp"
#include "wetwaresuite/pdf/sign.hpp"
#include "check.hpp"

using namespace wetwaresuite;
using namespace wetwaresuite::pdf;

namespace {

void testFonts() {
    CHECK(charWidth({Family::Sans}, 'A') == 667);
    CHECK(charWidth({Family::Serif}, 'A') == 722);
    CHECK(charWidth({Family::Mono, true}, 'i') == 600);
    CHECK(charWidth({Family::Sans, true}, 'a') == 556);
    CHECK(std::abs(textWidth({Family::Sans}, "AA", 10) - 13.34) < 1e-9);
    CHECK(toWinAnsi(0x20AC) == 0x80);  // euro
    CHECK(toWinAnsi(0x4E2D) == '?');
    CHECK(winAnsi("caf\xC3\xA9") == "caf\xE9");
    CHECK(utf8Encode(0xE9) == "\xC3\xA9");
    CHECK(fromWinAnsi(0x93) == 0x201C);
    CHECK(baseFontName({Family::Serif, true, true}) == std::string("Times-BoldItalic"));
}

void testZlib() {
    std::string text;
    for (int i = 0; i < 2000; ++i) text += "hello world " + std::to_string(i % 17);
    std::string z = deflate(text);
    CHECK(z.size() < text.size() / 3);
    CHECK(inflate(z) == text);
    bool threw = false;
    try {
        inflate("not zlib at all");
    } catch (const std::runtime_error&) {
        threw = true;
    }
    CHECK(threw);
}

std::vector<Page> samplePages(int n) {
    std::vector<Page> pages;
    for (int i = 0; i < n; ++i) {
        Page p;
        p.text(72, 100, "Page " + std::to_string(i + 1) + " (with parens) caf\xC3\xA9", {Family::Sans, true}, 18);
        p.text(72, 130, "second line", {Family::Serif}, 11);
        p.rect(72, 150, 100, 40, Color{200, 30, 30, 255}, Color{0, 0, 0, 255}, 1);
        p.line(72, 200, 300, 200, {0, 0, 255, 255}, 2);
        pages.push_back(p);
    }
    return pages;
}

void testWriteReadText() {
    std::string bytes = writePdf(samplePages(3), {"Test doc", "Ada", ""});
    CHECK(bytes.rfind("%PDF-1.4", 0) == 0);
    CHECK(bytes.find("/BaseFont /Helvetica-Bold") != std::string::npos);
    PdfFile f(bytes);
    CHECK(f.pageCount() == 3);
    CHECK(f.infoField("Title") == "Test doc");
    CHECK(f.infoField("Author") == "Ada");
    CHECK(std::abs(f.pages()[0].width - 595.28) < 0.01);
    std::string t = f.pageText(1);
    CHECK(t.find("Page 2 (with parens) caf\xC3\xA9") != std::string::npos);
    CHECK(t.find("second line") != std::string::npos);
    CHECK(t.find("\n") != std::string::npos);

    // merge pages from two files in a new order, with rotation
    PdfFile g(writePdf(samplePages(2)));
    std::string merged = assemblePdf({{&f, 2, 0}, {&g, 0, 90}, {&f, 0, 0}});
    PdfFile m(merged);
    CHECK(m.pageCount() == 3);
    CHECK(m.pageText(0).find("Page 3") != std::string::npos);
    CHECK(m.pageText(1).find("Page 1") != std::string::npos);
    CHECK(m.pages()[1].rotate == 90);
    CHECK(m.pages()[0].rotate == 0);
    // re-assembling an assembled file still works
    PdfFile again(assemblePdf({{&m, 1, 90}}));
    CHECK(again.pageCount() == 1 && again.pages()[0].rotate == 180);
}

void testPageList() {
    auto v = parsePageList("1-3,5,8-", 9);
    CHECK((v == std::vector<int>{0, 1, 2, 4, 7, 8}));
    CHECK((parsePageList("3-1", 5) == std::vector<int>{2, 1, 0}));
    CHECK(parsePageList("", 4).size() == 4);
    CHECK((parsePageList("last", 4) == std::vector<int>{3}));
    bool threw = false;
    try {
        parsePageList("7", 4);
    } catch (const std::runtime_error&) {
        threw = true;
    }
    CHECK(threw);
}

void testRender() {
    Page p = samplePages(1)[0];
    raster::Image img = renderPage(p, 1.0);
    CHECK(img.width() == 595 && img.height() == 842);
    // red rectangle and dark text pixels are where they should be
    raster::Color c = img.at(120, 170);
    CHECK(c.r > 180 && c.g < 60);
    int dark = 0;
    for (int y = 82; y < 104; ++y) {
        for (int x = 72; x < 200; ++x) dark += img.at(x, y).r < 100 ? 1 : 0;
    }
    CHECK(dark > 100);
    CHECK(textWidthPx("abc", {}, 20) > 20);
}

void testImages() {
    raster::Image px(4, 3, {10, 200, 30, 255});
    px.set(0, 0, {0, 0, 0, 0});
    auto data = std::make_shared<ImageData>();
    data->pixels = px;
    Page p;
    p.image(10, 10, 40, 30, data);
    std::string bytes = writePdf({p});
    CHECK(bytes.find("/SMask") != std::string::npos);
    PdfFile f(bytes);
    CHECK(f.pageCount() == 1);
    raster::Image img = renderPage(p, 1.0);
    CHECK(img.at(30, 30).g > 150);
}

void testHandWritten() {
    // a classic uncompressed file with an inherited MediaBox and a ToUnicode-less font
    std::string pdf =
        "%PDF-1.3\n1 0 obj << /Type /Catalog /Pages 2 0 R >> endobj\n"
        "2 0 obj << /Type /Pages /Kids [3 0 R] /Count 1 /MediaBox [0 0 300 200] >> endobj\n"
        "3 0 obj << /Type /Page /Parent 2 0 R /Contents 4 0 R /Resources << /Font << /F1 5 0 R >> >> >> endobj\n"
        "4 0 obj << /Length 60 >> stream\nBT /F1 12 Tf 10 100 Td [(Hel) -20 (lo) -500 (there)] TJ ET\nendstream endobj\n"
        "5 0 obj << /Type /Font /Subtype /Type1 /BaseFont /Helvetica >> endobj\n"
        "trailer << /Root 1 0 R >>\n%%EOF\n";
    PdfFile f(pdf);
    CHECK(f.pageCount() == 1);
    CHECK(f.pages()[0].width == 300 && f.pages()[0].height == 200);
    CHECK(f.pageText(0) == "Hello there");
}

void testUnicode() {
    // Greek and Cyrillic aren't in the standard fonts: they're embedded from the built-in font
    const std::string mixed = "Price \xE2\x82\xAC" "5 \xD0\x9F\xD1\x80\xD0\xB8\xD0\xB2\xD0\xB5\xD1\x82 \xCE\xA9mega";  // €, Привет, Ωmega
    Page p;
    p.text(72, 100, mixed, {}, 24);
    p.text(72, 140, "plain latin", {}, 12);
    CHECK(textWidth({}, mixed, 24) > textWidth({}, "Price \xE2\x82\xAC" "5  mega", 24) + 60);
    std::string bytes = writePdf({p}, {"\xD0\x9E\xD1\x82\xD1\x87\xD1\x91\xD1\x82", "", ""});
    CHECK(bytes.find("/Subtype /Type0") != std::string::npos);
    CHECK(bytes.find("/FU ") != std::string::npos);
    CHECK(bytes.size() < 40000);  // a subset, not the whole font
    PdfFile f(bytes);
    const std::string t = f.pageText(0);
    CHECK(t.find(mixed) != std::string::npos);
    CHECK(f.infoField("Title") == "\xD0\x9E\xD1\x82\xD1\x87\xD1\x91\xD1\x82");
    // latin-only files stay as before: no embedded font
    CHECK(writePdf({Page{}}).find("Type0") == std::string::npos);

    // the preview draws the Cyrillic letters too
    Page q;
    q.text(10, 40, "\xD0\x96\xD0\x96\xD0\x96", {}, 40);
    raster::Image img = renderPage(q, 1.0);
    int dark = 0;
    for (int y = 5; y < 45; ++y)
        for (int x = 10; x < 110; ++x) dark += img.at(x, y).r < 100 ? 1 : 0;
    CHECK(dark > 300);
    // characters no font has still become '?' (a private-use code point nothing draws)
    CHECK(textWidth({}, "\xF4\x8F\xBF\xBD", 10) == textWidth({}, "?", 10));
    // Chinese comes from a system font when there is one
    if (wetwaresuite::pdfdoc::unicodeFontIndex(0x4E2D) > 0) CHECK(textWidth({}, "\xE4\xB8\xAD", 10) > textWidth({}, "?", 10));
}


void testCrypto() {
    namespace c = crypto;
    CHECK(c::hex(c::md5("abc")) == "900150983cd24fb0d6963f7d28e17f72");
    CHECK(c::hex(c::sha1("abc")) == "a9993e364706816aba3e25717850c26c9cd0d89d");
    CHECK(c::hex(c::sha256("abc")) == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    CHECK(c::hex(c::sha512("abc")).substr(0, 16) == "ddaf35a193617aba");
    CHECK(c::hex(c::rc4("Key", "Plaintext")) == "bbf316e8d940af0ad3");
    std::string key, pt;
    for (int i = 0; i < 32; ++i) key += static_cast<char>(i);
    for (int i = 0; i < 16; ++i) pt += static_cast<char>(i * 0x11);
    CHECK(c::hex(c::aesEncryptEcb(key.substr(0, 16), pt)) == "69c4e0d86a7b0430d8cdb78070b4c55a");  // FIPS 197
    CHECK(c::hex(c::aesEncryptEcb(key, pt)) == "8ea2b7ca516745bfeafc49904b496089");
    const std::string iv(16, '\x07');
    CHECK(c::aesDecryptCbc(key, iv, c::aesEncryptCbc(key, iv, "a message that spans blocks", true), true) == "a message that spans blocks");
}

void testPasswords() {
    Page pg;
    pg.text(72, 100, "Top secret plans", {}, 14);
    const std::string plain = writePdf({pg}, {"Plans"});
    ProtectOptions o;
    o.userPassword = "open sesame";
    o.ownerPassword = "boss";
    o.print = false;
    const std::string locked = protectPdf(plain, o);
    CHECK(locked.find("Top secret") == std::string::npos);  // the words aren't readable in the file
    bool asked = false, wrong = false;
    try {
        PdfFile f(locked);
    } catch (const PasswordError& e) {
        asked = !e.given;
    }
    try {
        PdfFile f(locked, "nope");
    } catch (const PasswordError& e) {
        wrong = e.given;
    }
    CHECK(asked && wrong);
    const PdfFile user(locked, "open sesame");
    CHECK(user.security() == "AES-256" && !user.openedAsOwner() && user.pageText(0).find("Top secret plans") != std::string::npos);
    CHECK((user.permissions() & 4) == 0);  // printing not allowed
    const PdfFile owner(locked, "boss");
    CHECK(owner.openedAsOwner() && owner.infoField("Title") == "Plans");
}

void testSignatures() {
    const Identity id = makeSelfSignedIdentity("Ann Example", "ann@example.com");
    CHECK(id.keyPem.find("BEGIN PRIVATE KEY") != std::string::npos && id.certPem.find("BEGIN CERTIFICATE") != std::string::npos);
    const std::string sig = rsa::sign(id.keyPem, "hello");
    CHECK(rsa::verify(id.certPem, "hello", sig) && !rsa::verify(id.certPem, "hellp", sig));
    CHECK(loadIdentity(id.keyPem + id.certPem).name == "Ann Example");

    Page pg;
    pg.text(72, 100, "Contract", {}, 14);
    SignOptions o;
    o.identity = id;
    o.reason = "Approved";
    const std::string signedPdf = signPdf(writePdf({pg}), o);
    auto sigs = checkSignatures(signedPdf);
    CHECK(sigs.size() == 1 && sigs[0].intact && sigs[0].coversFile && sigs[0].selfSigned && sigs[0].signer == "Ann Example" && sigs[0].reason == "Approved");
    CHECK(PdfFile(signedPdf).pageText(0).find("Contract") != std::string::npos);

    // a changed byte inside the signed part breaks it; something added after it shows up too
    std::string tampered = signedPdf;
    tampered[7] = tampered[7] == '4' ? '5' : '4';  // %PDF-1.x: inside the signed bytes
    sigs = checkSignatures(tampered);
    CHECK(sigs.size() == 1 && !sigs[0].intact);
    sigs = checkSignatures(signedPdf + "\n% more\n");
    CHECK(sigs.size() == 1 && sigs[0].intact);
    const std::string twice = signPdf(signedPdf, o);
    sigs = checkSignatures(twice);
    CHECK(sigs.size() == 2 && sigs[0].intact && sigs[1].intact);
    int covering = 0;
    for (const auto& s : sigs) covering += s.coversFile ? 1 : 0;
    CHECK(covering == 1);
}

void testOcr() {
    // a "scan": text drawn at about 300 dpi, a little grey and noisy
    raster::Image img(1700, 240, {255, 255, 255, 255});
    drawText(img, 60, 100, "Invoice 20481, due March 3.", Font{Family::Sans}, 44, {30, 30, 30, 255});
    drawText(img, 60, 165, "Please pay the total today!", Font{Family::Serif}, 44, {30, 30, 30, 255});
    for (int i = 0; i < 3000; ++i) img.set((i * 7919) % img.width(), (i * 104729) % img.height(), {200, 200, 200, 255});
    const OcrResult r = recognizeText(img);
    CHECK(r.lines.size() == 2);
    CHECK(r.text == "Invoice 20481, due March 3.\nPlease pay the total today!");
    CHECK(r.confidence > 0.5);
    if (r.lines.size() == 2) CHECK(r.lines[0].words.size() == 5 && r.lines[0].words[0].text == "Invoice" && r.lines[0].words[0].x > 50 && r.lines[0].words[0].x < 70);
    CHECK(recognizeText(raster::Image(64, 64, {255, 255, 255, 255})).lines.empty());
}

}  // namespace

int main() {
    testFonts();
    testZlib();
    testWriteReadText();
    testPageList();
    testRender();
    testImages();
    testHandWritten();
    testUnicode();
    testCrypto();
    testPasswords();
    testSignatures();
    testOcr();
    return check::summary("pdf");
}
