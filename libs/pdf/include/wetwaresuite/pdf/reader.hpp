// Reading existing PDF files: objects (including compressed object streams),
// the page tree, text extraction, and writing a new file from pages picked
// out of one or more files (merge, split, extract, reorder, rotate).
#pragma once

#include <filesystem>
#include <map>
#include <stdexcept>
#include <string>
#include <vector>

namespace wetwaresuite::pdf {

struct Object {
    enum Kind { Null, Bool, Number, String, Name, Array, Dict, Ref, Stream };
    Kind kind = Null;
    bool boolean = false;
    double number = 0;     // Number; object number for Ref
    int gen = 0;           // Ref generation
    std::string str;       // String bytes, Name (without '/'), raw (still encoded) Stream data
    std::vector<Object> items;  // Array items, Dict/Stream values
    std::vector<std::string> keys;  // Dict/Stream keys, parallel to items

    const Object* get(const std::string& key) const;  // Dict/Stream entry or nullptr
    void set(const std::string& key, Object value);
    void erase(const std::string& key);
    bool isName(const std::string& n) const { return kind == Name && str == n; }

    static Object makeNumber(double v);
    static Object makeName(const std::string& n);
    static Object makeRef(int num, int gen = 0);
};

// A password-protected PDF opened without the right password. `given` says whether one was tried.
class PasswordError : public std::runtime_error {
public:
    PasswordError(const std::string& message, bool given) : std::runtime_error(message), given(given) {}
    bool given;
};

class PdfFile {
public:
    // Throws std::runtime_error when it isn't a PDF, PasswordError when it's locked and the password
    // (user or owner; files that open without one need none) doesn't fit. Encrypted files are
    // decrypted while loading, so everything below sees plain objects.
    explicit PdfFile(std::string bytes, const std::string& password = "");
    static PdfFile load(const std::filesystem::path& path, const std::string& password = "");

    struct PageInfo {
        int object = 0;
        double width = 0, height = 0;  // MediaBox size in points
        int rotate = 0;
        Object dict;  // the page dictionary with inherited Resources/MediaBox/CropBox/Rotate filled in
    };

    const std::vector<PageInfo>& pages() const { return pages_; }
    int pageCount() const { return static_cast<int>(pages_.size()); }
    const std::string& version() const { return version_; }
    bool encrypted() const { return encrypted_; }  // still locked (only when decrypting failed in a way that wasn't thrown)
    bool wasEncrypted() const { return !security_.empty(); }
    const std::string& security() const { return security_; }  // "AES-256", "RC4 128-bit"... or empty
    bool openedAsOwner() const { return owner_; }  // the owner password fit (or the file has no user password)
    int permissions() const { return permissions_; }  // the /P flags of a protected file (-1: all allowed)
    std::string infoField(const std::string& key) const;  // "Title", "Author", ... as UTF-8

    const Object& object(int num) const;  // a null object when missing
    const Object& resolve(const Object& o) const;
    std::string streamData(const Object& stream) const;  // decoded (Flate, ASCIIHex, ASCII85; others left as-is)

    std::string pageText(int index) const;  // 0-based

    // the raw structure, for editing: every object by number, the trailer and the catalog
    const std::map<int, Object>& objects() const { return objects_; }
    const Object& trailer() const { return trailer_; }
    const Object& catalog() const;
    int maxObject() const { return objects_.empty() ? 0 : objects_.rbegin()->first; }

private:
    std::string bytes_;
    std::map<int, Object> objects_;
    std::vector<PageInfo> pages_;
    Object trailer_;
    std::string version_;
    bool encrypted_ = false;
    std::string security_;
    bool owner_ = false;
    int permissions_ = -1;

    void scan();
    void unlock(const std::string& password);
    void loadObjectStreams();
    void buildPages();
};

struct PageSource {
    const PdfFile* file = nullptr;
    int index = 0;   // 0-based page in that file
    int rotate = 0;  // extra clockwise rotation in degrees (multiple of 90)
};

// A new PDF made of the given pages, in order. Throws for encrypted inputs.
std::string assemblePdf(const std::vector<PageSource>& pages);

// "1-3,5,8-" style page list (1-based, inclusive) -> 0-based indices. Throws on bad syntax or out of range.
std::vector<int> parsePageList(const std::string& spec, int pageCount);

// Password-protects a PDF (AES-256, the PDF 2.0 standard security handler): it opens only with a
// password when user is set, and anything the flags forbid needs the owner password.
struct ProtectOptions {
    std::string userPassword, ownerPassword;  // owner empty: same as user (or random when user is empty too)
    bool print = true, copy = true, edit = true, annotate = true;
};
std::string protectPdf(const std::string& pdfBytes, const ProtectOptions& options);

// Decodes a PDF text string (UTF-16BE with BOM or PDFDocEncoding) to UTF-8.
std::string decodeTextString(const std::string& bytes);

}  // namespace wetwaresuite::pdf
