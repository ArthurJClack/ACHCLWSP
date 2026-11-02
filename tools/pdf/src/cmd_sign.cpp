// Password protection and digital signatures on save, and the commands for signing identities
// and checking the signatures in a file.
#include <algorithm>
#include <fstream>
#include <sstream>

#include "commands.hpp"
#include "wetwaresuite/core/process.hpp"
#include "wetwaresuite/pdf/sign.hpp"

namespace wetwaresuite::pdf::tool {

namespace {

std::string pathToUtf8(const std::filesystem::path& p) {
    const std::u8string u = p.u8string();
    return std::string(u.begin(), u.end());
}

std::string readText(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) throw std::runtime_error("can't read " + path.string());
    std::stringstream s;
    s << in.rdbuf();
    return s.str();
}

std::string describe(const SignatureInfo& s) {
    std::string t = s.field + ": ";
    if (s.intact && s.coversFile) t += "valid, the document hasn't changed since";
    else if (s.intact) t += "valid for the version it signed; the file was added to after that";
    else t += "NOT valid: " + s.problem;
    t += s.signer.empty() ? "" : "; signed by " + s.signer;
    if (!s.time.empty()) t += " on " + s.time;
    if (!s.reason.empty()) t += "; reason: " + s.reason;
    if (!s.location.empty()) t += "; at " + s.location;
    if (s.selfSigned) t += " (self-signed: the name is the signer's own claim, not checked by an authority)";
    return t;
}

}  // namespace

std::filesystem::path defaultIdentityPath() {
    if (const char* env = std::getenv("WETWARESUITE_IDENTITY"); env && *env) return pathFromUtf8(env);
    return suiteToolsDir().parent_path() / "identity.pem";
}

std::string finishBytes(const DocData& d, std::string bytes) {
    if (d.digital && d.protect)
        throw std::runtime_error("a file can't be both password-protected and digitally signed here; take one off (pdf.protect remove=true)");
    if (d.protect) {
        ProtectOptions o;
        o.userPassword = d.userPassword;
        o.ownerPassword = d.ownerPassword;
        o.print = d.allowPrint, o.copy = d.allowCopy, o.edit = d.allowEdit, o.annotate = d.allowEdit;
        bytes = protectPdf(bytes, o);
    }
    if (d.digital) {
        SignOptions o;
        o.identity = loadIdentity(readText(d.identityPath.empty() ? defaultIdentityPath() : pathFromUtf8(d.identityPath)));
        o.reason = d.reason, o.location = d.location, o.contact = d.contact, o.page = d.signPage;
        bytes = signPdf(bytes, o);
    }
    return bytes;
}

void registerSignCommands(Session& session) {
    auto& reg = session.registry();

    reg.add({"pdf.identity", "Signing Identity",
             "The key and certificate digital signatures are made with. new=true makes a fresh one (name=, email=, organisation=; "
             "self-signed, kept in the suite's settings folder or at path=). export= writes the certificate (public, safe to "
             "share) so others can trust your signatures. With neither, says who you sign as.",
             {flag("new", "make a new identity (replaces the old one)", false), str("name", "your name as signatures show it", false),
              str("email", "email in the certificate", false), str("organisation", "organisation in the certificate", false),
              str("path", "where the identity (.pem, private!) lives (default: the suite's own)", false),
              str("export", "write the certificate (.pem or .crt) here", false)},
             false, false},
            plainCommand([](const Params& p) -> Result {
                const std::filesystem::path path = has(p, "path") ? pathFromUtf8(stringParam(p, "path")) : defaultIdentityPath();
                if (boolParam(p, "new", false)) {
                    const std::string name = stringParam(p, "name", "");
                    if (name.empty()) return Result::failure("give name= (how your signatures will be labelled)");
                    const Identity id = makeSelfSignedIdentity(name, stringParam(p, "email", ""), stringParam(p, "organisation", ""));
                    std::error_code ec;
                    std::filesystem::create_directories(path.parent_path(), ec);
                    writeBytes(pathToUtf8(path), id.keyPem + id.certPem);
                    std::filesystem::permissions(path, std::filesystem::perms::owner_read | std::filesystem::perms::owner_write,
                                                 std::filesystem::perm_options::replace, ec);
                    return Result::success("made a signing identity for " + name + " (2048-bit RSA, self-signed, 10 years) at " + pathToUtf8(path), pathToUtf8(path))
                        .with("path", pathToUtf8(path))
                        .with("name", name);
                }
                std::error_code ec;
                if (!std::filesystem::exists(path, ec)) return Result::failure("no signing identity yet; make one with new=true name=\"Your Name\"");
                const Identity id = loadIdentity(readText(path));
                if (has(p, "export")) {
                    const std::string out = stringParam(p, "export");
                    writeBytes(out, id.certPem);
                    return Result::success("wrote " + id.name + "'s certificate to " + out, out).with("path", out);
                }
                return Result::success("you sign as " + id.name + " (" + pathToUtf8(path) + ")", id.name).with("name", id.name).with("path", pathToUtf8(path));
            }));

    reg.add({"pdf.sign_digital", "Sign Digitally",
             "Put a certificate-based digital signature on the document when it's saved: readers show who signed and whether "
             "anything changed afterwards. Uses your signing identity (pdf.identity; made now from name= if there's none) or "
             "identity=<key+certificate .pem>. Pair it with pdf.sign for a visible signature. off=true takes it off again.",
             {docSpec(), str("reason", "why you're signing, e.g. Approved", false), str("location", "where", false), str("contact", "how to reach you", false),
              str("identity", "a .pem with your private key and certificate (default: your signing identity)", false),
              str("name", "your name, to make a signing identity if you don't have one", false), pageSpec(), flag("off", "don't sign after all", false)},
             true, false},
            docCommand(session,
                       [](PdfDoc& d, const Params& p, const std::string& id) -> Result {
                           if (boolParam(p, "off", false)) {
                               d.data.digital = false;
                               return Result::success(id + " will save without a digital signature");
                           }
                           if (d.data.protect) return Result::failure("take the password off first (pdf.protect remove=true): signed files can't be password-protected here");
                           const std::string idPath = stringParam(p, "identity", "");
                           std::error_code ec;
                           const std::filesystem::path path = idPath.empty() ? defaultIdentityPath() : pathFromUtf8(idPath);
                           if (!std::filesystem::exists(path, ec)) {
                               if (!idPath.empty()) return Result::failure("can't find " + idPath);
                               const std::string name = stringParam(p, "name", "");
                               if (name.empty()) return Result::failure("you don't have a signing identity yet: give name=\"Your Name\" to make one, or identity=<file.pem>");
                               const Identity made = makeSelfSignedIdentity(name);
                               std::filesystem::create_directories(path.parent_path(), ec);
                               writeBytes(pathToUtf8(path), made.keyPem + made.certPem);
                               std::filesystem::permissions(path, std::filesystem::perms::owner_read | std::filesystem::perms::owner_write,
                                                            std::filesystem::perm_options::replace, ec);
                           }
                           const Identity who = loadIdentity(readText(path));  // checks the key fits the certificate now, not at save time
                           d.data.digital = true;
                           d.data.identityPath = idPath;
                           d.data.reason = stringParam(p, "reason", "");
                           d.data.location = stringParam(p, "location", "");
                           d.data.contact = stringParam(p, "contact", "");
                           d.data.signPage = has(p, "page") ? pageParam(d.data, p) : 0;
                           return Result::success(id + " will be digitally signed by " + who.name + " when it's saved").with("signer", who.name);
                       },
                       true));

    reg.add({"pdf.signatures", "Check Signatures",
             "List the digital signatures in a PDF file and check each one: who signed, when, and whether the document changed since.",
             {str("path", "the PDF file (default: the open document's file)", false), docSpec()}, false, true},
            [&session](const Params& p) -> std::unique_ptr<Action> {
                std::string path = stringParam(p, "path", "");
                if (path.empty()) {
                    std::string id, error;
                    auto doc = findPdfDoc(session, p, id, error);
                    if (!doc) return std::make_unique<FunctionAction>([error] { return Result::failure(error); });
                    path = doc->path;
                }
                return std::make_unique<FunctionAction>([path] {
                    if (path.empty()) return Result::failure("save the document first, or give path=");
                    const auto sigs = checkSignatures(readText(pathFromUtf8(path)));
                    if (sigs.empty()) return Result::success(path + " has no digital signatures").with("signatures", std::int64_t{0});
                    std::string t = path + ": " + std::to_string(sigs.size()) + " signature" + (sigs.size() == 1 ? "" : "s");
                    bool allGood = true;
                    for (const auto& s : sigs) {
                        t += "\n  " + describe(s);
                        allGood = allGood && s.intact;
                    }
                    return Result::success(t).with("signatures", static_cast<std::int64_t>(sigs.size())).with("valid", allGood).with("latest_covers_file", sigs.back().coversFile);
                });
            });
}

}  // namespace wetwaresuite::pdf::tool
