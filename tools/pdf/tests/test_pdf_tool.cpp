#include <chrono>
#include <filesystem>
#include <fstream>
#include <random>
#include <cstdlib>
#include <sstream>

#include <nlohmann/json.hpp>

#include "wetwaresuite/mcp/server.hpp"
#include "wetwaresuite/pdf/commands.hpp"
#include "wetwaresuite/pdf/page.hpp"
#include "wetwaresuite/pdf/reader.hpp"
#include "wetwaresuite/pdf/view.hpp"
#include "check.hpp"

using namespace wetwaresuite;
namespace fs = std::filesystem;
using nlohmann::json;

namespace {

std::int64_t intField(const Result& r, const char* key) {
    const Value* v = r.field(key);
    return v && std::holds_alternative<std::int64_t>(*v) ? std::get<std::int64_t>(*v) : -1;
}

std::string strField(const Result& r, const char* key) {
    const Value* v = r.field(key);
    return v ? toText(*v) : "";
}

void writeBytes(const fs::path& p, const std::string& bytes) {
    std::ofstream out(p, std::ios::binary);
    out << bytes;
}

bool contains(const std::string& hay, const std::string& needle) { return hay.find(needle) != std::string::npos; }

std::string pageText(const fs::path& p, int page = 0) {
    auto f = pdf::PdfFile::load(p);
    std::string all;
    for (const auto& c : pdf::pageChars(f, page)) all += c.text;
    return all;
}

// a small form: one text field and one checkbox, hand-written with a real xref table
std::string formPdf() {
    std::vector<std::string> objs = {
        "<< /Type /Catalog /Pages 2 0 R /AcroForm << /Fields [4 0 R 5 0 R] /DA (/Helv 0 Tf 0 g) >> >>",
        "<< /Type /Pages /Kids [3 0 R] /Count 1 >>",
        "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 300 200] /Annots [4 0 R 5 0 R] >>",
        "<< /Type /Annot /Subtype /Widget /FT /Tx /T (Name) /Rect [20 140 220 160] /DA (/Helv 12 Tf 0 g) /P 3 0 R /F 4 >>",
        "<< /Type /Annot /Subtype /Widget /FT /Btn /T (Agree) /Rect [20 100 34 114] /V /Off /AS /Off /P 3 0 R /F 4 "
        "/AP << /N << /Yes 6 0 R /Off 7 0 R >> >> >>",
        "<< /Type /XObject /Subtype /Form /BBox [0 0 14 14] /Length 13 >>\nstream\n0 0 14 14 re f\nendstream",
        "<< /Type /XObject /Subtype /Form /BBox [0 0 14 14] /Length 0 >>\nstream\n\nendstream",
    };
    std::string out = "%PDF-1.7\n";
    std::vector<size_t> offsets;
    for (size_t i = 0; i < objs.size(); ++i) {
        offsets.push_back(out.size());
        out += std::to_string(i + 1) + " 0 obj\n" + objs[i] + "\nendobj\n";
    }
    const size_t xref = out.size();
    out += "xref\n0 " + std::to_string(objs.size() + 1) + "\n0000000000 65535 f \n";
    for (size_t o : offsets) {
        char line[32];
        std::snprintf(line, sizeof line, "%010zu 00000 n \n", o);
        out += line;
    }
    out += "trailer\n<< /Size " + std::to_string(objs.size() + 1) + " /Root 1 0 R >>\nstartxref\n" + std::to_string(xref) + "\n%%EOF\n";
    return out;
}

}  // namespace

int main() {
    std::mt19937_64 rng(static_cast<std::uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count()));
    const fs::path dir = fs::temp_directory_path() / ("wetwaresuite_pdf_" + std::to_string(rng() % 1000000000));
    fs::remove_all(dir);
    fs::create_directories(dir);

    {
        std::vector<pdf::Page> pages(2);
        pages[0].text(72, 100, "Hello World from wetwaresuite", pdf::Font{}, 14);
        pages[0].text(72, 140, "Secret number 4242 here", pdf::Font{}, 12);
        pages[1].text(72, 100, "Second page text", pdf::Font{}, 12);
        writeBytes(dir / "in.pdf", pdf::writePdf(pages, {"Test doc", "Ann", ""}));
    }
    writeBytes(dir / "form.pdf", formPdf());

    Session s;
    pdf::registerCommands(s);
    auto run = [&](const std::string& id, Params p) { return s.run(id, p); };
    auto str = [](const char* v) { return Value(std::string(v)); };

    // ---- open, read, find ----
    Result open = run("pdf.open", {{"path", (dir / "in.pdf").string()}});
    CHECK(open.ok && strField(open, "doc") == "pdf1" && intField(open, "pages") == 2);
    Result text = run("pdf.text", {});
    CHECK(text.ok && contains(text.message, "Hello World") && contains(text.message, "Second page"));
    Result found = run("pdf.find", {{"text", str("secret")}});
    CHECK(found.ok && intField(found, "total") == 1);
    Result info = run("pdf.info", {});
    CHECK(info.ok && contains(info.message, "Test doc"));

    // ---- view ----
    Result view = run("pdf.view", {{"page", std::int64_t{1}}, {"dpi", 72.0}});
    CHECK(view.ok && view.attachments.size() == 1 && view.attachments[0].mime == "image/png");
    CHECK(intField(view, "pixels_wide") > 500 || std::abs(std::get<double>(*view.field("width")) - 595.28) < 1);

    // ---- edits ----
    Result rep = run("pdf.replace_text", {{"find", str("Hello World")}, {"replace", str("Howdy Planet")}});
    CHECK(rep.ok);
    Result hl = run("pdf.highlight", {{"page", std::int64_t{1}}, {"text", str("Secret")}});
    CHECK(hl.ok);
    CHECK(run("pdf.note", {{"page", std::int64_t{1}}, {"x", 300.0}, {"y", 300.0}, {"text", str("check this")}}).ok);
    CHECK(run("pdf.draw", {{"page", std::int64_t{1}}, {"points", NumberList{100, 400, 150, 420, 200, 400}}}).ok);
    CHECK(run("pdf.shape", {{"page", std::int64_t{1}}, {"kind", str("rect")}, {"x", 60.0}, {"y", 500.0}, {"w", 100.0}, {"h", 40.0}}).ok);
    CHECK(run("pdf.add_text", {{"page", std::int64_t{2}}, {"x", 72.0}, {"y", 200.0}, {"text", str("Added later")}}).ok);
    CHECK(run("pdf.sign", {{"page", std::int64_t{2}}, {"x", 72.0}, {"y", 300.0}, {"name", str("Ann Example")}}).ok);
    Result red = run("pdf.redact", {{"text", str("4242")}});
    CHECK(red.ok);
    Result marks = run("pdf.marks", {});
    CHECK(marks.ok && intField(marks, "total") >= 7);
    CHECK(run("pdf.view", {{"page", std::int64_t{1}}, {"dpi", 50.0}}).ok);

    // undo takes the last edit back
    const auto before = intField(run("pdf.marks", {}), "total");
    CHECK(s.history().undo());
    CHECK(intField(run("pdf.marks", {}), "total") == before - 1);
    CHECK(s.history().redo());

    // ---- save and reopen ----
    Result save = run("pdf.save", {{"path", (dir / "out.pdf").string()}});
    CHECK(save.ok && fs::exists(dir / "out.pdf"));
    {
        const std::string t1 = pageText(dir / "out.pdf", 0);
        CHECK(contains(t1, "Howdy Planet"));
        CHECK(!contains(t1, "Hello World"));
        CHECK(!contains(t1, "4242"));  // really gone, not just covered
        CHECK(contains(t1, "Secret number"));
        const std::string t2 = pageText(dir / "out.pdf", 1);
        CHECK(contains(t2, "Added later") && contains(t2, "Second page"));
    }
    Result reopen = run("pdf.open", {{"path", (dir / "out.pdf").string()}});
    CHECK(reopen.ok && intField(reopen, "pages") == 2);
    Result annots = run("pdf.annotations", {{"doc", strField(reopen, "doc")}});
    CHECK(annots.ok && intField(annots, "total") >= 4);  // highlight, note, ink, square

    // flatten burns the comments in
    CHECK(run("pdf.save", {{"doc", str("pdf1")}, {"path", (dir / "flat.pdf").string()}, {"flatten", true}}).ok);
    {
        auto f = pdf::PdfFile::load(dir / "flat.pdf");
        auto img = pdf::renderPdfPage(f, 0, 0.5);
        CHECK(img.width() > 0);
    }

    // ---- pages ----
    CHECK(run("pdf.page_insert", {{"doc", str("pdf1")}, {"after", std::int64_t{2}}, {"count", std::int64_t{1}}}).ok);
    CHECK(intField(run("pdf.info", {{"doc", str("pdf1")}}), "pages") == 3);
    CHECK(run("pdf.page_rotate", {{"doc", str("pdf1")}, {"pages", str("1")}, {"angle", std::int64_t{90}}}).ok);
    CHECK(run("pdf.page_move", {{"doc", str("pdf1")}, {"page", std::int64_t{2}}, {"to", std::int64_t{1}}}).ok);
    CHECK(run("pdf.page_delete", {{"doc", str("pdf1")}, {"pages", str("3")}}).ok);
    CHECK(run("pdf.save", {{"doc", str("pdf1")}, {"path", (dir / "pages.pdf").string()}}).ok);
    {
        auto f = pdf::PdfFile::load(dir / "pages.pdf");
        CHECK(f.pageCount() == 2);
        CHECK(contains(pageText(dir / "pages.pdf", 0), "Second page"));
        auto box = pdf::pageBox(f, 1);
        CHECK(box.rotate == 90);
    }
    CHECK(run("pdf.save", {{"doc", str("pdf1")}, {"path", (dir / "extract.pdf").string()}, {"pages", str("2")}}).ok);
    CHECK(pdf::PdfFile::load(dir / "extract.pdf").pageCount() == 1);

    // ---- new document ----
    Result made = run("pdf.new", {{"pages", std::int64_t{1}}, {"size", str("letter")}});
    CHECK(made.ok);
    CHECK(run("pdf.add_text", {{"x", 72.0}, {"y", 72.0}, {"text", str("Fresh page \xC3\xA9t\xC3\xA9")}}).ok);
    CHECK(run("pdf.save", {{"path", (dir / "new.pdf").string()}}).ok);
    CHECK(contains(pageText(dir / "new.pdf"), "Fresh page"));

    // ---- forms ----
    Result form = run("pdf.open", {{"path", (dir / "form.pdf").string()}});
    CHECK(form.ok && intField(form, "fields") == 2);
    Result fields = run("pdf.fields", {});
    CHECK(fields.ok && contains(fields.message, "Name") && contains(fields.message, "Agree"));
    CHECK(contains(strField(fields, "widgets"), "Name\t1\t20\t40\t200\t20"));
    CHECK(run("pdf.fill", {{"values", str("Name=Ann Example; Agree=yes")}}).ok);
    CHECK(!run("pdf.fill", {{"name", str("Nope")}, {"value", str("x")}}).ok);
    CHECK(run("pdf.save", {{"path", (dir / "filled.pdf").string()}}).ok);
    {
        Result again = run("pdf.open", {{"path", (dir / "filled.pdf").string()}});
        CHECK(again.ok);
        Result f2 = run("pdf.fields", {});
        CHECK(contains(f2.message, "Ann Example") && contains(f2.message, "Yes"));
    }

    // ---- passwords ----
    {
        CHECK(run("pdf.open", {{"path", (dir / "in.pdf").string()}}).ok);
        CHECK(run("pdf.protect", {{"password", str("open sesame")}, {"copy", false}}).ok);
        CHECK(run("pdf.save", {{"path", (dir / "locked.pdf").string()}}).ok);
        Result noPass = run("pdf.open", {{"path", (dir / "locked.pdf").string()}});
        CHECK(!noPass.ok && contains(noPass.message, "password"));
        CHECK(!run("pdf.open", {{"path", (dir / "locked.pdf").string()}, {"password", str("wrong")}}).ok);
        Result unlocked = run("pdf.open", {{"path", (dir / "locked.pdf").string()}, {"password", str("open sesame")}});
        CHECK(unlocked.ok);
        CHECK(contains(run("pdf.text", {}).message, "Hello World"));
        // saving a file opened with its password keeps it locked; remove=true takes it off
        CHECK(run("pdf.save", {{"path", (dir / "still.pdf").string()}}).ok);
        CHECK(!run("pdf.open", {{"path", (dir / "still.pdf").string()}}).ok);
        CHECK(run("pdf.open", {{"path", (dir / "still.pdf").string()}, {"password", str("open sesame")}}).ok);
        CHECK(run("pdf.protect", {{"remove", true}}).ok);
        CHECK(run("pdf.save", {{"path", (dir / "open.pdf").string()}}).ok);
        CHECK(contains(pageText(dir / "open.pdf"), "Hello World"));
    }

    // ---- digital signatures (a throwaway identity, not the user's) ----
    {
        const std::string idPath = (dir / "identity.pem").string();
#ifdef _WIN32
        _putenv_s("WETWARESUITE_IDENTITY", idPath.c_str());
#else
        setenv("WETWARESUITE_IDENTITY", idPath.c_str(), 1);
#endif
        CHECK(run("pdf.open", {{"path", (dir / "in.pdf").string()}}).ok);
        CHECK(!run("pdf.sign_digital", {}).ok);  // no identity yet and no name to make one
        Result sd = run("pdf.sign_digital", {{"name", str("Test Signer")}, {"reason", str("Approved")}});
        CHECK(sd.ok && strField(sd, "signer") == "Test Signer");
        CHECK(fs::exists(idPath));
        CHECK(!run("pdf.protect", {{"password", str("x")}}).ok || !run("pdf.save", {{"path", (dir / "both.pdf").string()}}).ok);
        run("pdf.protect", {{"remove", true}});
        CHECK(run("pdf.save", {{"path", (dir / "signed.pdf").string()}}).ok);
        Result sigs = run("pdf.signatures", {{"path", (dir / "signed.pdf").string()}});
        CHECK(sigs.ok && intField(sigs, "signatures") == 1 && contains(sigs.message, "Test Signer"));
        const Value* valid = sigs.field("valid");
        CHECK(valid && std::holds_alternative<bool>(*valid) && std::get<bool>(*valid));
        Result none = run("pdf.signatures", {{"path", (dir / "in.pdf").string()}});
        CHECK(none.ok && intField(none, "signatures") == 0);
        Result who = run("pdf.identity", {});
        CHECK(who.ok && contains(who.message, "Test Signer"));
    }

    // ---- reading scanned text ----
    {
        auto scan = std::make_shared<pdf::ImageData>();
        scan->pixels = raster::Image(1240, 300, {255, 255, 255, 255});
        pdf::drawText(scan->pixels, 80, 140, "Scanned receipt total 42", pdf::Font{}, 46, {20, 20, 20, 255});
        std::vector<pdf::Page> pages(1);
        pages[0].width = 595.28;
        pages[0].height = 144;
        pages[0].image(0, 0, 595.28, 144, scan);
        writeBytes(dir / "scan.pdf", pdf::writePdf(pages));
        CHECK(run("pdf.open", {{"path", (dir / "scan.pdf").string()}}).ok);
        CHECK(!contains(run("pdf.text", {}).message, "receipt"));
        Result ocr = run("pdf.ocr", {});
        CHECK(ocr.ok && intField(ocr, "pages") == 1);
        CHECK(contains(run("pdf.find", {{"text", str("receipt")}}).message, "receipt") || intField(run("pdf.find", {{"text", str("receipt")}}), "count") > 0);
        CHECK(run("pdf.save", {{"path", (dir / "searchable.pdf").string()}}).ok);
        CHECK(contains(pageText(dir / "searchable.pdf"), "receipt"));
    }

    // ---- over MCP ----
    {
        Session ms;
        pdf::registerCommands(ms);
        mcp::Server server(ms, {"wetwaresuite", "test", "pdf"});
        json list = json::parse(*server.handle(R"({"jsonrpc":"2.0","id":1,"method":"tools/list"})"))["result"]["tools"];
        bool sawRedact = false;
        for (const auto& t : list) sawRedact = sawRedact || contains(t["name"].get<std::string>(), "redact");
        CHECK(sawRedact);
        json req = {{"jsonrpc", "2.0"}, {"id", 2}, {"method", "tools/call"},
                    {"params", {{"name", mcp::Server::toolName("pdf.open")}, {"arguments", {{"path", (dir / "in.pdf").string()}}}}}};
        json res = json::parse(*server.handle(req.dump()))["result"];
        CHECK(!res.value("isError", false));
    }

    fs::remove_all(dir);
    return check::summary("pdf_tool");
}

