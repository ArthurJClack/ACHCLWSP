// Tags that travel with the file: XMP (embedded in JPEG and PNG, or a .xmp
// sidecar next to anything else) and the EXIF rating/keywords Windows writes.
// Only our four fields are touched; the rest of an existing XMP packet (camera
// data, Lightroom develop settings...) is kept as it was.
#include <algorithm>
#include <array>
#include <cstring>
#include <fstream>
#include <regex>
#include <sstream>
#include <stdexcept>

#include "wetwaresuite/library/library.hpp"

namespace wetwaresuite::library {

namespace {

const char kXmpJpegId[] = "http://ns.adobe.com/xap/1.0/";  // followed by a NUL
const char kXmpPngKey[] = "XML:com.adobe.xmp";
const unsigned char kPngSig[8] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n'};

std::string slurp(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    if (!in) throw std::runtime_error("can't read " + p.string());
    std::ostringstream o;
    o << in.rdbuf();
    return o.str();
}

void spit(const fs::path& p, const std::string& bytes) {
    // write next to it, then swap in, so a crash never leaves half a photo
    fs::path tmp = p;
    tmp += ".wwtmp";
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        if (!out) throw std::runtime_error("can't write " + p.string());
        out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
        if (!out) throw std::runtime_error("can't write " + p.string());
    }
    std::error_code ec;
    fs::rename(tmp, p, ec);
    if (ec) {
        fs::remove(tmp, ec);
        throw std::runtime_error("can't replace " + p.string());
    }
}

std::uint32_t u16be(const std::string& s, std::size_t i) {
    return (static_cast<unsigned char>(s[i]) << 8) | static_cast<unsigned char>(s[i + 1]);
}
std::uint32_t u32be(const std::string& s, std::size_t i) {
    return (u16be(s, i) << 16) | u16be(s, i + 2);
}

std::uint32_t crc32(const std::string& data, std::size_t from, std::size_t n) {
    static const auto table = [] {
        std::array<std::uint32_t, 256> t{};
        for (std::uint32_t i = 0; i < 256; ++i) {
            std::uint32_t c = i;
            for (int k = 0; k < 8; ++k) c = c & 1 ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            t[i] = c;
        }
        return t;
    }();
    std::uint32_t c = 0xFFFFFFFFu;
    for (std::size_t i = from; i < from + n; ++i) c = table[(c ^ static_cast<unsigned char>(data[i])) & 0xFF] ^ (c >> 8);
    return c ^ 0xFFFFFFFFu;
}

void put32(std::string& s, std::uint32_t v) {
    for (int sh = 24; sh >= 0; sh -= 8) s += static_cast<char>((v >> sh) & 0xFF);
}

// ---- JPEG segments ----

struct Segment {
    unsigned marker;
    std::size_t start, length;  // start of the FF byte; length of the whole segment including FF xx LL LL
    std::size_t payload() const { return start + 4; }
};

// The segments before the image data (stops at SOS).
std::vector<Segment> jpegSegments(const std::string& s) {
    std::vector<Segment> out;
    if (s.size() < 4 || static_cast<unsigned char>(s[0]) != 0xFF || static_cast<unsigned char>(s[1]) != 0xD8) return out;
    std::size_t pos = 2;
    while (pos + 4 <= s.size() && static_cast<unsigned char>(s[pos]) == 0xFF) {
        unsigned m = static_cast<unsigned char>(s[pos + 1]);
        if (m == 0xFF) {
            ++pos;
            continue;
        }
        if (m == 0xDA || m == 0xD9) break;
        std::size_t len = u16be(s, pos + 2);
        if (len < 2 || pos + 2 + len > s.size()) break;
        out.push_back({m, pos, 2 + len});
        pos += 2 + len;
    }
    return out;
}

bool isXmpSegment(const std::string& s, const Segment& g) {
    const std::size_t idLen = sizeof(kXmpJpegId);  // includes the NUL
    return g.marker == 0xE1 && g.length >= 4 + idLen && s.compare(g.payload(), idLen, kXmpJpegId, idLen) == 0;
}

std::string jpegXmp(const std::string& s) {
    for (const auto& g : jpegSegments(s)) {
        if (isXmpSegment(s, g)) return s.substr(g.payload() + sizeof(kXmpJpegId), g.length - 4 - sizeof(kXmpJpegId));
    }
    return {};
}

std::string jpegWithXmp(const std::string& s, const std::string& packet) {
    const std::string body = std::string(kXmpJpegId, sizeof(kXmpJpegId)) + packet;
    if (body.size() + 2 > 0xFFFF) throw std::runtime_error("the tags don't fit in the photo's XMP block");
    std::string seg = "\xFF\xE1";
    seg += static_cast<char>(((body.size() + 2) >> 8) & 0xFF);
    seg += static_cast<char>((body.size() + 2) & 0xFF);
    seg += body;
    auto segs = jpegSegments(s);
    if (segs.empty() && s.size() < 4) throw std::runtime_error("not a JPEG");
    for (const auto& g : segs) {
        if (isXmpSegment(s, g)) return s.substr(0, g.start) + seg + s.substr(g.start + g.length);
    }
    // after JFIF/EXIF headers, which readers expect first
    std::size_t at = 2;
    for (const auto& g : segs) {
        if (g.marker == 0xE0 || g.marker == 0xE1) at = g.start + g.length;
        else break;
    }
    return s.substr(0, at) + seg + s.substr(at);
}

// EXIF rating (0x4746) and Windows keywords (0x9C9E, UTF-16LE, ; separated) from IFD0.
void jpegExif(const std::string& s, FileTags& t) {
    for (const auto& g : jpegSegments(s)) {
        if (g.marker != 0xE1 || g.length < 4 + 6 + 8 || s.compare(g.payload(), 6, std::string("Exif\0\0", 6)) != 0) continue;
        const std::string tiff = s.substr(g.payload() + 6, g.length - 4 - 6);
        const bool le = tiff.compare(0, 2, "II") == 0;
        auto r16 = [&](std::size_t i) -> std::uint32_t {
            if (i + 2 > tiff.size()) return 0;
            auto a = static_cast<unsigned char>(tiff[i]), b = static_cast<unsigned char>(tiff[i + 1]);
            return le ? a | (b << 8) : (a << 8) | b;
        };
        auto r32 = [&](std::size_t i) -> std::uint32_t { return le ? r16(i) | (r16(i + 2) << 16) : (r16(i) << 16) | r16(i + 2); };
        std::size_t ifd = r32(4);
        std::uint32_t n = r16(ifd);
        for (std::uint32_t k = 0; k < n && k < 512; ++k) {
            std::size_t e = ifd + 2 + 12 * k;
            if (e + 12 > tiff.size()) break;
            std::uint32_t tag = r16(e), type = r16(e + 2), count = r32(e + 4);
            if (tag == 0x4746 && type == 3) {
                t.rating = std::clamp(static_cast<int>(r16(e + 8)), 0, 5);
            } else if (tag == 0x9C9E && type == 1 && count > 0) {
                std::size_t off = count <= 4 ? e + 8 : r32(e + 8);
                if (off + count > tiff.size()) continue;
                std::string word;
                auto flush = [&] {
                    auto a = word.find_first_not_of(' '), b = word.find_last_not_of(' ');
                    if (a != std::string::npos) t.tags.push_back(word.substr(a, b - a + 1));
                    word.clear();
                };
                for (std::size_t i = 0; i + 1 < count; i += 2) {
                    std::uint32_t c = static_cast<unsigned char>(tiff[off + i]) | (static_cast<unsigned char>(tiff[off + i + 1]) << 8);
                    if (c == 0) break;
                    if (c == ';') flush();
                    else if (c < 0x80) word += static_cast<char>(c);
                    else if (c < 0x800) {
                        word += static_cast<char>(0xC0 | (c >> 6));
                        word += static_cast<char>(0x80 | (c & 0x3F));
                    } else {
                        word += static_cast<char>(0xE0 | (c >> 12));
                        word += static_cast<char>(0x80 | ((c >> 6) & 0x3F));
                        word += static_cast<char>(0x80 | (c & 0x3F));
                    }
                }
                flush();
            }
        }
        if (t.rating || !t.tags.empty()) t.from = "exif";
        return;
    }
}

// ---- PNG chunks ----

struct Chunk {
    std::size_t start, length;  // whole chunk: length, type, data, crc
    std::string type;
    std::size_t data() const { return start + 8; }
    std::size_t dataLen() const { return length - 12; }
};

std::vector<Chunk> pngChunks(const std::string& s) {
    std::vector<Chunk> out;
    if (s.size() < 8 || std::memcmp(s.data(), kPngSig, 8) != 0) return out;
    std::size_t pos = 8;
    while (pos + 12 <= s.size()) {
        std::size_t n = u32be(s, pos);
        if (n > s.size() || pos + 12 + n > s.size()) break;
        out.push_back({pos, 12 + n, s.substr(pos + 4, 4)});
        if (out.back().type == "IEND") break;
        pos += 12 + n;
    }
    return out;
}

bool isXmpChunk(const std::string& s, const Chunk& c) {
    const std::size_t k = sizeof(kXmpPngKey);  // keyword + NUL
    return c.type == "iTXt" && c.dataLen() >= k + 2 && s.compare(c.data(), k, kXmpPngKey, k) == 0 && s[c.data() + k] == 0;
}

std::string pngXmp(const std::string& s) {
    for (const auto& c : pngChunks(s)) {
        if (!isXmpChunk(s, c)) continue;
        // keyword\0 flag method lang\0 translated\0 text
        std::size_t p = c.data() + sizeof(kXmpPngKey) + 2, end = c.data() + c.dataLen();
        for (int nul = 0; nul < 2 && p < end; ++p) {
            if (s[p] == 0) ++nul;
        }
        return s.substr(p, end - p);
    }
    return {};
}

std::string pngWithXmp(const std::string& s, const std::string& packet) {
    auto chunks = pngChunks(s);
    if (chunks.empty() || chunks[0].type != "IHDR") throw std::runtime_error("not a PNG");
    std::string data = std::string(kXmpPngKey, sizeof(kXmpPngKey)) + std::string("\0\0\0\0", 4) + packet;
    std::string chunk;
    put32(chunk, static_cast<std::uint32_t>(data.size()));
    chunk += "iTXt" + data;
    put32(chunk, crc32(chunk, 4, chunk.size() - 4));
    for (const auto& c : chunks) {
        if (isXmpChunk(s, c)) return s.substr(0, c.start) + chunk + s.substr(c.start + c.length);
    }
    const std::size_t at = chunks[0].start + chunks[0].length;
    return s.substr(0, at) + chunk + s.substr(at);
}

// ---- XMP text ----

std::string escape(const std::string& v) {
    std::string o;
    for (char c : v) {
        switch (c) {
            case '&': o += "&amp;"; break;
            case '<': o += "&lt;"; break;
            case '>': o += "&gt;"; break;
            case '"': o += "&quot;"; break;
            default: o += c;
        }
    }
    return o;
}

std::string unescape(std::string v) {
    static const std::pair<const char*, const char*> ents[] = {{"&lt;", "<"}, {"&gt;", ">"}, {"&quot;", "\""}, {"&apos;", "'"}, {"&#xA;", "\n"}, {"&#10;", "\n"}, {"&amp;", "&"}};
    for (const auto& [from, to] : ents) {
        for (std::size_t p = 0; (p = v.find(from, p)) != std::string::npos; p += std::strlen(to)) v.replace(p, std::strlen(from), to);
    }
    return v;
}

// The text between <name ...> and </name>, or nullopt.
std::optional<std::string> element(const std::string& x, const std::string& name) {
    std::size_t a = x.find("<" + name);
    while (a != std::string::npos) {
        char after = a + 1 + name.size() < x.size() ? x[a + 1 + name.size()] : 0;
        if (after == '>' || after == ' ' || after == '\n' || after == '\t' || after == '\r' || after == '/') break;
        a = x.find("<" + name, a + 1);
    }
    if (a == std::string::npos) return std::nullopt;
    std::size_t gt = x.find('>', a);
    if (gt == std::string::npos || x[gt - 1] == '/') return std::nullopt;
    std::size_t b = x.find("</" + name + ">", gt);
    if (b == std::string::npos) return std::nullopt;
    return x.substr(gt + 1, b - gt - 1);
}

std::vector<std::string> listItems(const std::string& x) {
    std::vector<std::string> out;
    static const std::regex li(R"(<rdf:li(?:\s[^>]*)?>([\s\S]*?)</rdf:li>)");
    for (auto it = std::sregex_iterator(x.begin(), x.end(), li); it != std::sregex_iterator(); ++it) out.push_back(unescape((*it)[1].str()));
    return out;
}

std::optional<std::string> simple(const std::string& x, const std::string& name) {
    std::smatch m;
    const std::regex attr("\\b" + name + "=\"([^\"]*)\"");
    if (std::regex_search(x, m, attr)) return unescape(m[1].str());
    if (auto e = element(x, name)) return unescape(*e);
    return std::nullopt;
}

void parseXmp(const std::string& x, FileTags& t) {
    for (const char* name : {"dc:subject"}) {
        if (auto e = element(x, name)) {
            for (auto& v : listItems(*e)) {
                if (!v.empty()) t.tags.push_back(v);
            }
        }
    }
    for (const char* name : {"xmp:Rating", "xap:Rating"}) {
        if (auto v = simple(x, name)) {
            try {
                t.rating = std::clamp(std::stoi(*v), 0, 5);  // -1 means rejected: no stars here
            } catch (const std::exception&) {
            }
            break;
        }
    }
    for (const char* name : {"xmp:Label", "xap:Label"}) {
        if (auto v = simple(x, name)) {
            std::string l = lower(*v);
            if (l == "grey") l = "gray";
            static const std::vector<std::string> ok = {"red", "orange", "yellow", "green", "blue", "purple", "gray"};
            if (std::find(ok.begin(), ok.end(), l) != ok.end()) t.label = l;
            break;
        }
    }
    if (auto e = element(x, "dc:description")) {
        auto items = listItems(*e);
        if (!items.empty()) t.notes = items[0];
    }
}

// Removes every form of `name` (attribute or element) from the packet.
void strip(std::string& x, const std::string& name) {
    x = std::regex_replace(x, std::regex("\\s" + name + "=\"[^\"]*\""), "");
    x = std::regex_replace(x, std::regex("<" + name + "(?:\\s[^>]*)?/>"), "");
    x = std::regex_replace(x, std::regex("[ \\t]*<" + name + "(?:\\s[^>]*)?>[\\s\\S]*?</" + name + ">[ \\t]*\\r?\\n?"), "");
}

std::string fields(const FileTags& t, const std::string& indent) {
    std::string o;
    if (t.rating) o += indent + "<xmp:Rating>" + std::to_string(t.rating) + "</xmp:Rating>\n";
    if (!t.label.empty()) {
        std::string l = t.label;
        l[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(l[0])));  // Bridge and Lightroom write "Red"
        o += indent + "<xmp:Label>" + escape(l) + "</xmp:Label>\n";
    }
    if (!t.tags.empty()) {
        o += indent + "<dc:subject>\n" + indent + " <rdf:Bag>\n";
        for (const auto& tag : t.tags) o += indent + "  <rdf:li>" + escape(tag) + "</rdf:li>\n";
        o += indent + " </rdf:Bag>\n" + indent + "</dc:subject>\n";
    }
    if (!t.notes.empty()) {
        o += indent + "<dc:description>\n" + indent + " <rdf:Alt>\n" + indent + "  <rdf:li xml:lang=\"x-default\">" + escape(t.notes) +
             "</rdf:li>\n" + indent + " </rdf:Alt>\n" + indent + "</dc:description>\n";
    }
    return o;
}

std::string freshPacket(const FileTags& t) {
    return "<?xpacket begin=\"\xEF\xBB\xBF\" id=\"W5M0MpCehiHzreSzNTczkc9d\"?>\n"
           "<x:xmpmeta xmlns:x=\"adobe:ns:meta/\" x:xmptk=\"wetwaresuite\">\n"
           " <rdf:RDF xmlns:rdf=\"http://www.w3.org/1999/02/22-rdf-syntax-ns#\">\n"
           "  <rdf:Description rdf:about=\"\"\n"
           "    xmlns:dc=\"http://purl.org/dc/elements/1.1/\"\n"
           "    xmlns:xmp=\"http://ns.adobe.com/xap/1.0/\">\n" +
           fields(t, "   ") +
           "  </rdf:Description>\n"
           " </rdf:RDF>\n"
           "</x:xmpmeta>\n"
           "<?xpacket end=\"w\"?>";
}

// Our four fields set in an existing packet; everything else stays.
std::string updatedPacket(std::string x, const FileTags& t) {
    for (const char* n : {"xmp:Rating", "xap:Rating", "xmp:Label", "xap:Label", "dc:subject", "dc:description"}) strip(x, n);
    std::size_t a = x.find("<rdf:Description");
    if (a == std::string::npos) {
        std::size_t r = x.find("</rdf:RDF>");
        if (r == std::string::npos) return freshPacket(t);
        x.insert(r, "<rdf:Description rdf:about=\"\"></rdf:Description>\n");
        a = r;
    }
    std::size_t gt = x.find('>', a);
    if (gt == std::string::npos) return freshPacket(t);
    std::string open = x.substr(a, gt - a);
    bool selfClosing = !open.empty() && open.back() == '/';
    if (selfClosing) open.pop_back();
    if (open.find("xmlns:dc=") == std::string::npos) open += " xmlns:dc=\"http://purl.org/dc/elements/1.1/\"";
    if (open.find("xmlns:xmp=") == std::string::npos) open += " xmlns:xmp=\"http://ns.adobe.com/xap/1.0/\"";
    std::string body = "\n" + fields(t, "   ");
    x = x.substr(0, a) + open + ">" + body + (selfClosing ? "  </rdf:Description>" : "") + x.substr(gt + 1);
    return x;
}

bool embeddable(const std::string& ext) { return ext == "jpg" || ext == "jpeg" || ext == "jpe" || ext == "png"; }

std::string embeddedXmp(const std::string& bytes, const std::string& ext) {
    return ext == "png" ? pngXmp(bytes) : jpegXmp(bytes);
}

}  // namespace

bool FileTags::any() const { return !tags.empty() || rating || !label.empty() || !notes.empty(); }

fs::path sidecarPath(const fs::path& file) {
    fs::path a = file;
    a.replace_extension(".xmp");  // photo.xmp: Bridge, Lightroom, Capture One
    fs::path b = file;
    b += ".xmp";  // photo.jpg.xmp: darktable, digiKam
    std::error_code ec;
    if (!fs::exists(a, ec) && fs::exists(b, ec)) return b;
    return a;
}

std::optional<FileTags> readFileTags(const fs::path& path) {
    FileTags t;
    const std::string ext = extOf(path);
    std::error_code ec;
    if (embeddable(ext)) {
        std::string bytes;
        try {
            bytes = slurp(path);
        } catch (const std::exception&) {
            return std::nullopt;
        }
        std::string x = embeddedXmp(bytes, ext);
        if (!x.empty()) {
            parseXmp(x, t);
            if (t.any()) t.from = "the file";
        }
        if (!t.any() && ext != "png") jpegExif(bytes, t);
    }
    if (!t.any() && ext != "xmp") {
        fs::path side = sidecarPath(path);
        if (fs::is_regular_file(side, ec)) {
            try {
                parseXmp(slurp(side), t);
                if (t.any()) t.from = "sidecar " + side.filename().string();
            } catch (const std::exception&) {
            }
        }
    }
    if (!t.any()) return std::nullopt;
    // drop repeats, keep the order
    std::vector<std::string> uniq;
    for (auto& tag : t.tags) {
        bool dup = false;
        for (auto& u : uniq) dup = dup || lower(u) == lower(tag);
        if (!dup) uniq.push_back(tag);
    }
    t.tags = std::move(uniq);
    return t;
}

std::string writeFileTags(const fs::path& path, const FileTags& t, const std::string& where, std::vector<FileBackup>* backup) {
    const std::string ext = extOf(path);
    const bool embed = where == "embed" || (where == "auto" && embeddable(ext));
    if (embed) {
        if (!embeddable(ext)) throw std::runtime_error("tags can be embedded in JPEG and PNG files only; use where=sidecar for ." + ext);
        const std::string bytes = slurp(path);
        const std::string old = embeddedXmp(bytes, ext);
        const std::string packet = old.empty() ? freshPacket(t) : updatedPacket(old, t);
        const std::string out = ext == "png" ? pngWithXmp(bytes, packet) : jpegWithXmp(bytes, packet);
        if (backup) backup->push_back({path, bytes});
        spit(path, out);
        return "the file";
    }
    const fs::path side = sidecarPath(path);
    std::error_code ec;
    std::optional<std::string> before;
    if (fs::is_regular_file(side, ec)) before = slurp(side);
    const std::string packet = before ? updatedPacket(*before, t) : freshPacket(t) + "\n";
    if (backup) backup->push_back({side, before});
    spit(side, packet);
    return "sidecar " + side.filename().string();
}

}  // namespace wetwaresuite::library
