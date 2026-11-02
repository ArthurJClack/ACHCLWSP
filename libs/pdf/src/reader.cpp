#include "wetwaresuite/pdf/reader.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <functional>
#include <set>
#include <sstream>
#include <stdexcept>

#include "wetwaresuite/pdf/fonts.hpp"
#include "wetwaresuite/pdf/page.hpp"
#include "internal.hpp"
#include "lexer.hpp"

namespace wetwaresuite::pdf {

using detail::hexVal;
using detail::isDelim;
using detail::isRegular;
using detail::isWs;
using detail::Lexer;

// ---- Object ------------------------------------------------------------------

const Object* Object::get(const std::string& key) const {
    for (std::size_t i = 0; i < keys.size(); ++i) {
        if (keys[i] == key) return &items[i];
    }
    return nullptr;
}

void Object::set(const std::string& key, Object value) {
    for (std::size_t i = 0; i < keys.size(); ++i) {
        if (keys[i] == key) {
            items[i] = std::move(value);
            return;
        }
    }
    keys.push_back(key);
    items.push_back(std::move(value));
}

void Object::erase(const std::string& key) {
    for (std::size_t i = 0; i < keys.size(); ++i) {
        if (keys[i] == key) {
            keys.erase(keys.begin() + static_cast<std::ptrdiff_t>(i));
            items.erase(items.begin() + static_cast<std::ptrdiff_t>(i));
            return;
        }
    }
}

Object Object::makeNumber(double v) {
    Object o;
    o.kind = Number;
    o.number = v;
    return o;
}

Object Object::makeName(const std::string& n) {
    Object o;
    o.kind = Name;
    o.str = n;
    return o;
}

Object Object::makeRef(int num, int gen) {
    Object o;
    o.kind = Ref;
    o.number = num;
    o.gen = gen;
    return o;
}

// ---- helpers -----------------------------------------------------------------

namespace {

std::string readAll(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) throw std::runtime_error("cannot open " + path.string());
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

int toInt(const Object* o) { return o && o->kind == Object::Number ? static_cast<int>(o->number) : 0; }

// PNG predictors (DecodeParms /Predictor >= 10) as used by xref and object streams.
std::string unpredict(const std::string& data, int columns, int colors, int bpc) {
    const int bpp = std::max(1, colors * bpc / 8);
    const std::size_t row = static_cast<std::size_t>((columns * colors * bpc + 7) / 8);
    std::string out;
    std::string prev(row, '\0');
    for (std::size_t i = 0; i + 1 + row <= data.size(); i += row + 1) {
        const int type = static_cast<unsigned char>(data[i]);
        std::string cur = data.substr(i + 1, row);
        for (std::size_t x = 0; x < row; ++x) {
            const int a = x >= static_cast<std::size_t>(bpp) ? static_cast<unsigned char>(cur[x - static_cast<std::size_t>(bpp)]) : 0;
            const int b = static_cast<unsigned char>(prev[x]);
            const int c = x >= static_cast<std::size_t>(bpp) ? static_cast<unsigned char>(prev[x - static_cast<std::size_t>(bpp)]) : 0;
            int v = static_cast<unsigned char>(cur[x]);
            switch (type) {
                case 1: v += a; break;
                case 2: v += b; break;
                case 3: v += (a + b) / 2; break;
                case 4: {
                    const int p = a + b - c, pa = std::abs(p - a), pb = std::abs(p - b), pc = std::abs(p - c);
                    v += pa <= pb && pa <= pc ? a : pb <= pc ? b : c;
                    break;
                }
                default: break;
            }
            cur[x] = static_cast<char>(v & 0xFF);
        }
        out += cur;
        prev = cur;
    }
    return out;
}

std::string lzwDecode(const std::string& in, bool earlyChange) {
    std::string out;
    std::vector<std::string> table;
    auto reset = [&] {
        table.clear();
        for (int i = 0; i < 256; ++i) table.emplace_back(1, static_cast<char>(i));
        table.emplace_back();  // 256 clear
        table.emplace_back();  // 257 end
    };
    reset();
    int bits = 9;
    std::uint32_t buf = 0;
    int have = 0;
    std::string prev;
    bool first = true;
    for (char ch : in) {
        buf = (buf << 8) | static_cast<unsigned char>(ch);
        have += 8;
        while (have >= bits) {
            const int code = static_cast<int>((buf >> (have - bits)) & ((1u << bits) - 1));
            have -= bits;
            if (code == 256) {
                reset();
                bits = 9;
                first = true;
                continue;
            }
            if (code == 257) return out;
            std::string entry;
            if (code < static_cast<int>(table.size()) && (code < 256 || code > 257)) entry = table[static_cast<std::size_t>(code)];
            else if (!first) entry = prev + prev[0];
            else return out;
            out += entry;
            if (!first) table.push_back(prev + entry[0]);
            prev = entry;
            first = false;
            const int next = static_cast<int>(table.size()) + (earlyChange ? 1 : 0);
            if (next >= 4096) continue;
            if (next >= (1 << bits)) ++bits;
            if (bits > 12) bits = 12;
        }
    }
    return out;
}

std::string runLengthDecode(const std::string& in) {
    std::string out;
    for (std::size_t i = 0; i < in.size();) {
        const int len = static_cast<unsigned char>(in[i++]);
        if (len == 128) break;
        if (len < 128) {
            const std::size_t n = std::min<std::size_t>(static_cast<std::size_t>(len) + 1, in.size() - i);
            out.append(in, i, n);
            i += n;
        } else if (i < in.size()) {
            out.append(static_cast<std::size_t>(257 - len), in[i++]);
        }
    }
    return out;
}

std::string asciiHex(const std::string& in) {
    std::string out;
    int hi = -1;
    for (char c : in) {
        if (c == '>') break;
        int v = hexVal(c);
        if (v < 0) continue;
        if (hi < 0) {
            hi = v;
        } else {
            out += static_cast<char>(hi * 16 + v);
            hi = -1;
        }
    }
    if (hi >= 0) out += static_cast<char>(hi * 16);
    return out;
}

std::string ascii85(const std::string& in) {
    std::string out;
    std::uint32_t tuple = 0;
    int count = 0;
    std::size_t i = 0;
    if (in.compare(0, 2, "<~") == 0) i = 2;
    for (; i < in.size(); ++i) {
        char c = in[i];
        if (c == '~') break;
        if (isWs(c)) continue;
        if (c == 'z' && count == 0) {
            out.append(4, '\0');
            continue;
        }
        if (c < '!' || c > 'u') continue;
        tuple = tuple * 85 + static_cast<std::uint32_t>(c - '!');
        if (++count == 5) {
            for (int k = 3; k >= 0; --k) out += static_cast<char>((tuple >> (k * 8)) & 0xFF);
            tuple = 0;
            count = 0;
        }
    }
    if (count > 1) {
        for (int k = count; k < 5; ++k) tuple = tuple * 85 + 84;
        for (int k = 0; k < count - 1; ++k) out += static_cast<char>((tuple >> ((3 - k) * 8)) & 0xFF);
    }
    return out;
}

}  // namespace

// ---- file --------------------------------------------------------------------

PdfFile::PdfFile(std::string bytes, const std::string& password) : bytes_(std::move(bytes)) {
    const std::size_t head = bytes_.find("%PDF-");
    if (head == std::string::npos || head > 1024) throw std::runtime_error("not a PDF file");
    std::size_t e = head + 5;
    while (e < bytes_.size() && (std::isdigit(static_cast<unsigned char>(bytes_[e])) || bytes_[e] == '.')) ++e;
    version_ = bytes_.substr(head + 5, e - head - 5);
    scan();
    if (encrypted_) unlock(password);
    loadObjectStreams();
    buildPages();
}

PdfFile PdfFile::load(const std::filesystem::path& path, const std::string& password) { return PdfFile(readAll(path), password); }

const Object& PdfFile::object(int num) const {
    static const Object null;
    auto it = objects_.find(num);
    return it == objects_.end() ? null : it->second;
}

const Object& PdfFile::resolve(const Object& o) const {
    const Object* cur = &o;
    for (int hops = 0; cur->kind == Object::Ref && hops < 32; ++hops) cur = &object(static_cast<int>(cur->number));
    if (cur->kind == Object::Ref) {
        static const Object null;
        return null;
    }
    return *cur;
}

const Object& PdfFile::catalog() const {
    const Object* root = trailer_.get("Root");
    static const Object null;
    return root ? resolve(*root) : null;
}

void PdfFile::scan() {
    const std::string& s = bytes_;
    std::vector<Object> trailers;
    std::size_t i = 0;
    auto digitAt = [&](std::size_t p) { return p < s.size() && s[p] >= '0' && s[p] <= '9'; };
    while (i < s.size()) {
        const char c = s[i];
        if (c == 't' && s.compare(i, 7, "trailer") == 0 && (i == 0 || isWs(s[i - 1]))) {
            Lexer lx(s, i + 7);
            try {
                Object t = lx.parse();
                if (t.kind == Object::Dict) trailers.push_back(std::move(t));
            } catch (const std::exception&) {
            }
            i = std::max(lx.pos, i + 7);
            continue;
        }
        if (!digitAt(i) || (i > 0 && !isWs(s[i - 1]) && !isDelim(s[i - 1]))) {
            ++i;
            continue;
        }
        // "N G obj"
        std::size_t p = i;
        int num = 0, gen = 0;
        while (digitAt(p)) num = num * 10 + (s[p++] - '0');
        if (p >= s.size() || !isWs(s[p])) {
            i = p;
            continue;
        }
        while (p < s.size() && isWs(s[p])) ++p;
        if (!digitAt(p)) {
            i = p;
            continue;
        }
        while (digitAt(p)) gen = gen * 10 + (s[p++] - '0');
        while (p < s.size() && isWs(s[p])) ++p;
        if (s.compare(p, 3, "obj") != 0 || (p + 3 < s.size() && isRegular(s[p + 3]))) {
            i = i + 1;
            continue;
        }
        Lexer lx(s, p + 3);
        Object o;
        try {
            o = lx.parse();
        } catch (const std::exception&) {
            i = p + 3;
            continue;
        }
        if (o.kind == Object::Dict && lx.peekKeyword("stream")) {
            std::size_t d = lx.pos + 6;
            if (d < s.size() && s[d] == '\r') ++d;
            if (d < s.size() && s[d] == '\n') ++d;
            std::size_t end = std::string::npos;
            const Object* len = o.get("Length");
            if (len && len->kind == Object::Number && len->number >= 0) {
                std::size_t L = static_cast<std::size_t>(len->number);
                if (d + L <= s.size()) {
                    Lexer chk(s, d + L);
                    if (chk.peekKeyword("endstream")) end = d + L;
                }
            }
            std::size_t es = s.find("endstream", end == std::string::npos ? d : end);
            if (end == std::string::npos) {
                if (es == std::string::npos) es = s.size();
                end = es;
                while (end > d && (s[end - 1] == '\n' || s[end - 1] == '\r')) --end;
            }
            o.kind = Object::Stream;
            o.str = s.substr(d, end - d);
            lx.pos = es == std::string::npos ? s.size() : es + 9;
            if (o.get("Type") && o.get("Type")->isName("XRef")) trailers.push_back(o);
        }
        o.gen = gen;  // the per-object encryption key needs it
        objects_[num] = std::move(o);
        i = std::max(lx.pos, p + 3);
    }
    // the newest trailer that names a catalog wins
    for (auto it = trailers.rbegin(); it != trailers.rend(); ++it) {
        if (it->get("Root")) {
            trailer_ = *it;
            trailer_.kind = Object::Dict;
            trailer_.str.clear();
            break;
        }
    }
    for (const auto& t : trailers) {
        if (t.get("Encrypt")) encrypted_ = true;
    }
}

void PdfFile::loadObjectStreams() {
    std::vector<int> streams;
    for (const auto& [num, o] : objects_) {
        if (o.kind == Object::Stream && o.get("Type") && o.get("Type")->isName("ObjStm")) streams.push_back(num);
    }
    for (int num : streams) {
        const Object os = objects_[num];
        std::string data;
        try {
            data = streamData(os);
        } catch (const std::exception&) {
            continue;
        }
        const int n = toInt(os.get("N")), first = toInt(os.get("First"));
        Lexer lx(data, 0);
        std::vector<std::pair<int, int>> index;
        for (int k = 0; k < n; ++k) {
            Object a = lx.parse(), b = lx.parse();
            if (a.kind != Object::Number || b.kind != Object::Number) break;
            index.emplace_back(static_cast<int>(a.number), static_cast<int>(b.number));
        }
        for (const auto& [objNum, off] : index) {
            if (objects_.count(objNum)) continue;
            const std::size_t at = static_cast<std::size_t>(first) + static_cast<std::size_t>(off);
            if (at >= data.size()) continue;
            Lexer ol(data, at);
            try {
                objects_[objNum] = ol.parse();
            } catch (const std::exception&) {
            }
        }
    }
    if (!trailer_.get("Root")) {
        for (const auto& [num, o] : objects_) {
            if (o.kind == Object::Dict && o.get("Type") && o.get("Type")->isName("Catalog")) {
                trailer_.kind = Object::Dict;
                trailer_.set("Root", Object::makeRef(num));
                break;
            }
        }
    }
}

std::string PdfFile::streamData(const Object& stream) const {
    if (stream.kind != Object::Stream) return {};
    std::string data = stream.str;
    const Object* f = stream.get("Filter");
    std::vector<std::string> filters;
    std::vector<const Object*> parms;
    const Object* dp = stream.get("DecodeParms");
    if (f) {
        const Object& fr = resolve(*f);
        if (fr.kind == Object::Name) {
            filters.push_back(fr.str);
            parms.push_back(dp ? &resolve(*dp) : nullptr);
        } else if (fr.kind == Object::Array) {
            for (std::size_t k = 0; k < fr.items.size(); ++k) {
                filters.push_back(resolve(fr.items[k]).str);
                const Object* p = nullptr;
                if (dp) {
                    const Object& dpr = resolve(*dp);
                    if (dpr.kind == Object::Array && k < dpr.items.size()) p = &resolve(dpr.items[k]);
                }
                parms.push_back(p);
            }
        }
    }
    for (std::size_t k = 0; k < filters.size(); ++k) {
        const std::string& name = filters[k];
        if (name == "FlateDecode" || name == "Fl") {
            data = inflate(data);
            const Object* p = parms[k];
            if (p && p->kind == Object::Dict && toInt(p->get("Predictor")) >= 10) {
                const int cols = p->get("Columns") ? toInt(p->get("Columns")) : 1;
                const int colors = p->get("Colors") ? toInt(p->get("Colors")) : 1;
                const int bpc = p->get("BitsPerComponent") ? toInt(p->get("BitsPerComponent")) : 8;
                data = unpredict(data, cols, colors, bpc);
            }
        } else if (name == "LZWDecode" || name == "LZW") {
            const Object* p = parms[k];
            const Object* ec = p && p->kind == Object::Dict ? p->get("EarlyChange") : nullptr;
            data = lzwDecode(data, !ec || toInt(ec) != 0);
            if (p && p->kind == Object::Dict && toInt(p->get("Predictor")) >= 10) {
                const int cols = p->get("Columns") ? toInt(p->get("Columns")) : 1;
                const int colors = p->get("Colors") ? toInt(p->get("Colors")) : 1;
                const int bpc = p->get("BitsPerComponent") ? toInt(p->get("BitsPerComponent")) : 8;
                data = unpredict(data, cols, colors, bpc);
            }
        } else if (name == "RunLengthDecode" || name == "RL") {
            data = runLengthDecode(data);
        } else if (name == "ASCIIHexDecode" || name == "AHx") {
            data = asciiHex(data);
        } else if (name == "ASCII85Decode" || name == "A85") {
            data = ascii85(data);
        } else {
            break;  // image codecs etc.: leave encoded
        }
    }
    return data;
}

void PdfFile::buildPages() {
    const Object* root = trailer_.get("Root");
    if (!root) throw std::runtime_error("PDF has no document catalog");
    const Object& catalog = resolve(*root);
    const Object* pagesRef = catalog.get("Pages");
    if (!pagesRef) throw std::runtime_error("PDF has no page tree");
    std::set<int> seen;
    std::function<void(const Object&, int, Object)> walk = [&](const Object& nodeRef, int depth, Object inherited) {
        if (depth > 64) return;
        int num = nodeRef.kind == Object::Ref ? static_cast<int>(nodeRef.number) : -1;
        if (num >= 0 && !seen.insert(num).second) return;
        const Object& node = resolve(nodeRef);
        if (node.kind != Object::Dict && node.kind != Object::Stream) return;
        for (const char* key : {"Resources", "MediaBox", "CropBox", "Rotate"}) {
            if (const Object* v = node.get(key)) inherited.set(key, *v);
        }
        const Object* type = node.get("Type");
        const Object* kids = node.get("Kids");
        if ((type && type->isName("Pages")) || (!type && kids)) {
            if (!kids) return;
            const Object& k = resolve(*kids);
            for (const auto& kid : k.items) walk(kid, depth + 1, inherited);
            return;
        }
        PageInfo info;
        info.object = num;
        info.dict = node;
        info.dict.kind = Object::Dict;
        for (std::size_t i = 0; i < inherited.keys.size(); ++i) {
            if (!info.dict.get(inherited.keys[i])) info.dict.set(inherited.keys[i], inherited.items[i]);
        }
        if (const Object* mb = info.dict.get("MediaBox")) {
            const Object& box = resolve(*mb);
            if (box.kind == Object::Array && box.items.size() == 4) {
                double v[4];
                for (int j = 0; j < 4; ++j) v[j] = resolve(box.items[static_cast<std::size_t>(j)]).number;
                info.width = std::abs(v[2] - v[0]);
                info.height = std::abs(v[3] - v[1]);
            }
        }
        if (info.width <= 0) info.width = 612;
        if (info.height <= 0) info.height = 792;
        if (const Object* r = info.dict.get("Rotate")) info.rotate = ((static_cast<int>(resolve(*r).number) % 360) + 360) % 360;
        pages_.push_back(std::move(info));
    };
    Object none;
    none.kind = Object::Dict;
    walk(*pagesRef, 0, none);
}

std::string decodeTextString(const std::string& b) {
    std::string out;
    if (b.size() >= 2 && static_cast<unsigned char>(b[0]) == 0xFE && static_cast<unsigned char>(b[1]) == 0xFF) {
        for (std::size_t i = 2; i + 1 < b.size(); i += 2) {
            unsigned u = (static_cast<unsigned char>(b[i]) << 8) | static_cast<unsigned char>(b[i + 1]);
            if (u >= 0xD800 && u < 0xDC00 && i + 3 < b.size()) {
                unsigned lo = (static_cast<unsigned char>(b[i + 2]) << 8) | static_cast<unsigned char>(b[i + 3]);
                u = 0x10000 + ((u - 0xD800) << 10) + (lo - 0xDC00);
                i += 2;
            }
            out += utf8Encode(u);
        }
        return out;
    }
    if (b.size() >= 3 && b.compare(0, 3, "\xEF\xBB\xBF") == 0) return b.substr(3);
    for (char c : b) out += utf8Encode(fromWinAnsi(static_cast<unsigned char>(c)));
    return out;
}

std::string PdfFile::infoField(const std::string& key) const {
    const Object* info = trailer_.get("Info");
    if (!info) return {};
    const Object& d = resolve(*info);
    const Object* v = d.get(key);
    if (!v) return {};
    const Object& r = resolve(*v);
    return r.kind == Object::String ? decodeTextString(r.str) : r.kind == Object::Name ? r.str : std::string();
}

// ---- text extraction ---------------------------------------------------------

namespace {

struct FontMap {
    int codeBytes = 1;
    bool hasMap = false;
    bool identity = false;  // Type0 without ToUnicode: nothing we can decode
    std::map<std::uint32_t, std::string> map;
    // glyph advances in 1/1000 em, to tell where a run of text ends
    int firstChar = 0;
    std::vector<double> widths;
    int standard = -1;  // standard-14 font index when the font names one
    double width(std::uint32_t code) const {
        if (code >= static_cast<std::uint32_t>(firstChar) && code - static_cast<std::uint32_t>(firstChar) < widths.size()) {
            double w = widths[code - static_cast<std::uint32_t>(firstChar)];
            if (w > 0) return w;
        }
        if (standard >= 0 && code < 256) {
            static const Family fam[3] = {Family::Sans, Family::Serif, Family::Mono};
            Font f{fam[standard / 4], (standard % 4) == 1 || (standard % 4) == 3, (standard % 4) >= 2};
            return charWidth(f, static_cast<unsigned char>(code));
        }
        return codeBytes == 2 ? 1000 : 500;
    }
};

std::string utf16ToUtf8(const std::string& b) {
    std::string out;
    for (std::size_t i = 0; i + 1 < b.size(); i += 2) {
        unsigned u = (static_cast<unsigned char>(b[i]) << 8) | static_cast<unsigned char>(b[i + 1]);
        if (u >= 0xD800 && u < 0xDC00 && i + 3 < b.size()) {
            unsigned lo = (static_cast<unsigned char>(b[i + 2]) << 8) | static_cast<unsigned char>(b[i + 3]);
            u = 0x10000 + ((u - 0xD800) << 10) + (lo - 0xDC00);
            i += 2;
        }
        out += utf8Encode(u);
    }
    if (b.size() == 1) out += utf8Encode(static_cast<unsigned char>(b[0]));
    return out;
}

std::uint32_t codeOf(const std::string& b) {
    std::uint32_t v = 0;
    for (char c : b) v = (v << 8) | static_cast<unsigned char>(c);
    return v;
}

void parseCMap(const std::string& data, FontMap& fm) {
    Lexer lx(data, 0);
    std::vector<Object> operands;
    while (!lx.eof()) {
        std::string op;
        Object o = lx.parse(&op);
        if (lx.eof() && op.empty() && o.kind == Object::Null) break;
        if (op.empty()) {
            operands.push_back(std::move(o));
            continue;
        }
        if (op == "endcodespacerange") {
            if (!operands.empty() && operands[0].kind == Object::String) fm.codeBytes = std::max<int>(1, static_cast<int>(operands[0].str.size()));
        } else if (op == "endbfchar") {
            for (std::size_t i = 0; i + 1 < operands.size(); i += 2) {
                if (operands[i].kind == Object::String && operands[i + 1].kind == Object::String) {
                    fm.map[codeOf(operands[i].str)] = utf16ToUtf8(operands[i + 1].str);
                }
            }
            fm.hasMap = true;
        } else if (op == "endbfrange") {
            for (std::size_t i = 0; i + 2 < operands.size(); i += 3) {
                if (operands[i].kind != Object::String || operands[i + 1].kind != Object::String) continue;
                std::uint32_t lo = codeOf(operands[i].str), hi = codeOf(operands[i + 1].str);
                if (hi < lo || hi - lo > 65535) continue;
                const Object& dst = operands[i + 2];
                if (dst.kind == Object::String) {
                    std::string base = dst.str;
                    for (std::uint32_t c = lo; c <= hi; ++c) {
                        fm.map[c] = utf16ToUtf8(base);
                        if (!base.empty()) {  // increment the last UTF-16 unit
                            std::size_t k = base.size();
                            while (k-- > 0) {
                                unsigned char v = static_cast<unsigned char>(base[k]);
                                base[k] = static_cast<char>(v + 1);
                                if (v != 0xFF) break;
                            }
                        }
                    }
                } else if (dst.kind == Object::Array) {
                    for (std::uint32_t c = lo; c <= hi && c - lo < dst.items.size(); ++c) {
                        if (dst.items[c - lo].kind == Object::String) fm.map[c] = utf16ToUtf8(dst.items[c - lo].str);
                    }
                }
            }
            fm.hasMap = true;
        }
        operands.clear();
    }
}

class TextExtractor {
public:
    explicit TextExtractor(const PdfFile& f) : file_(f) {}

    std::string run(const Object& page) {
        const Object* res = page.get("Resources");
        Object resources = res ? file_.resolve(*res) : Object{};
        std::string content;
        if (const Object* c = page.get("Contents")) {
            const Object& cr = file_.resolve(*c);
            if (cr.kind == Object::Array) {
                for (const auto& part : cr.items) content += file_.streamData(file_.resolve(part)) + "\n";
            } else {
                content = file_.streamData(cr);
            }
        }
        runContent(content, resources, 0);
        // tidy: trim trailing spaces on lines, collapse 3+ newlines
        std::string out, line;
        int blank = 0;
        std::istringstream in(out_);
        while (std::getline(in, line)) {
            while (!line.empty() && line.back() == ' ') line.pop_back();
            if (line.empty()) {
                if (++blank > 1) continue;
            } else {
                blank = 0;
            }
            out += line + "\n";
        }
        while (!out.empty() && (out.back() == '\n')) out.pop_back();
        while (!out.empty() && out.front() == '\n') out.erase(0, 1);
        return out;
    }

private:
    const PdfFile& file_;
    std::string out_;
    std::map<const Object*, FontMap> fontCache_;
    double lastY_ = 1e9, lastX_ = 0;
    double fontSize_ = 10;
    bool started_ = false;

    const FontMap& fontFor(const Object& resources, const std::string& name) {
        static const FontMap simple;
        const Object* fonts = resources.get("Font");
        if (!fonts) return simple;
        const Object& fd = file_.resolve(*fonts);
        const Object* fr = fd.get(name);
        if (!fr) return simple;
        const Object& font = file_.resolve(*fr);
        auto it = fontCache_.find(&font);
        if (it != fontCache_.end()) return it->second;
        FontMap fm;
        const Object* sub = font.get("Subtype");
        if (sub && sub->isName("Type0")) {
            fm.codeBytes = 2;
            fm.identity = true;
        }
        if (const Object* fc = font.get("FirstChar")) fm.firstChar = static_cast<int>(file_.resolve(*fc).number);
        if (const Object* w = font.get("Widths")) {
            const Object& arr = file_.resolve(*w);
            for (const auto& it : arr.items) fm.widths.push_back(file_.resolve(it).number);
        }
        if (const Object* bf = font.get("BaseFont")) {
            std::string name = file_.resolve(*bf).str;
            auto plus = name.find('+');
            if (plus != std::string::npos) name = name.substr(plus + 1);
            for (int i = 0; i < 12; ++i) {
                static const Family fam[3] = {Family::Sans, Family::Serif, Family::Mono};
                Font f{fam[i / 4], (i % 4) == 1 || (i % 4) == 3, (i % 4) >= 2};
                if (name == baseFontName(f)) fm.standard = fontIndex(f);
            }
            if (fm.standard < 0 && (name == "Arial" || name == "ArialMT")) fm.standard = 0;
        }
        if (const Object* tu = font.get("ToUnicode")) {
            const Object& cm = file_.resolve(*tu);
            if (cm.kind == Object::Stream) {
                try {
                    parseCMap(file_.streamData(cm), fm);
                } catch (const std::exception&) {
                }
            }
        }
        return fontCache_[&font] = fm;
    }

    // Advance of a shown string in text space units (before the text matrix).
    double advance(const std::string& bytes, const FontMap& fm) const {
        const std::size_t n = static_cast<std::size_t>(fm.codeBytes);
        double w = 0;
        for (std::size_t i = 0; i + n <= bytes.size(); i += n) w += fm.width(codeOf(bytes.substr(i, n)));
        return w / 1000 * fontSize_;
    }

    void emit(const std::string& bytes, const FontMap& fm) {
        if (fm.hasMap) {
            const std::size_t n = static_cast<std::size_t>(fm.codeBytes);
            for (std::size_t i = 0; i + n <= bytes.size(); i += n) {
                auto it = fm.map.find(codeOf(bytes.substr(i, n)));
                if (it != fm.map.end()) out_ += it->second;
            }
        } else if (!fm.identity) {
            for (char c : bytes) {
                unsigned char b = static_cast<unsigned char>(c);
                if (b >= 32 || b == '\t') out_ += utf8Encode(fromWinAnsi(b));
            }
        }
    }

    void moveTo(double x, double y) {
        if (started_) {
            const double dy = std::abs(y - lastY_);
            if (dy > fontSize_ * 0.5) {
                out_ += dy > fontSize_ * 1.9 ? "\n\n" : "\n";
            } else if ((x > lastX_ + fontSize_ * 0.15 || x < lastX_ - fontSize_ * 2) && !out_.empty() && out_.back() != ' ' && out_.back() != '\n') {
                out_ += ' ';
            }
        }
        lastY_ = y;
        lastX_ = x;
        started_ = true;
    }

    void runContent(const std::string& content, const Object& resources, int depth) {
        if (depth > 8) return;
        Lexer lx(content, 0);
        std::vector<Object> ops;
        const FontMap* font = nullptr;
        double tm[6] = {1, 0, 0, 1, 0, 0}, lm[6] = {1, 0, 0, 1, 0, 0};
        double leading = 0;
        auto numAt = [&](std::size_t i) { return i < ops.size() && ops[i].kind == Object::Number ? ops[i].number : 0.0; };
        auto lineMove = [&](double tx, double ty) {
            lm[4] += tx * lm[0] + ty * lm[2];
            lm[5] += tx * lm[1] + ty * lm[3];
            std::copy(lm, lm + 6, tm);
            moveTo(tm[4], tm[5]);
        };
        while (!lx.eof()) {
            std::string op;
            std::size_t before = lx.pos;
            Object o;
            try {
                o = lx.parse(&op);
            } catch (const std::exception&) {
                break;
            }
            if (lx.pos == before) ++lx.pos;
            if (op.empty()) {
                if (o.kind != Object::Null || lx.eof()) ops.push_back(std::move(o));
                continue;
            }
            if (op == "BI") {  // inline image: skip to EI
                std::size_t id = content.find("ID", lx.pos);
                std::size_t ei = id == std::string::npos ? std::string::npos : content.find("EI", id + 2);
                while (ei != std::string::npos && ei + 2 < content.size() && isRegular(content[ei + 2])) ei = content.find("EI", ei + 2);
                lx.pos = ei == std::string::npos ? content.size() : ei + 2;
            } else if (op == "BT") {
                std::fill(tm, tm + 6, 0.0);
                tm[0] = tm[3] = 1;
                std::copy(tm, tm + 6, lm);
            } else if (op == "Tf") {
                if (ops.size() >= 2 && ops[0].kind == Object::Name) font = &fontFor(resources, ops[0].str);
                fontSize_ = std::max(1.0, std::abs(numAt(1)));
            } else if (op == "TL") {
                leading = numAt(0);
            } else if (op == "Td") {
                lineMove(numAt(0), numAt(1));
            } else if (op == "TD") {
                leading = -numAt(1);
                lineMove(numAt(0), numAt(1));
            } else if (op == "T*") {
                lineMove(0, -leading);
            } else if (op == "Tm") {
                for (std::size_t k = 0; k < 6; ++k) lm[k] = tm[k] = numAt(k);
                moveTo(tm[4], tm[5]);
                fontSize_ = std::max(1.0, std::abs(fontSize_));
            } else if (op == "Tj" || op == "'" || op == "\"") {
                if (op != "Tj") lineMove(0, -leading);
                const Object* s = ops.empty() ? nullptr : &ops.back();
                if (s && s->kind == Object::String) {
                    emit(s->str, font ? *font : FontMap{});
                    lastX_ += advance(s->str, font ? *font : FontMap{}) * std::hypot(tm[0], tm[1]);
                }
            } else if (op == "TJ") {
                if (!ops.empty() && ops[0].kind == Object::Array) {
                    const double scale = std::hypot(tm[0], tm[1]);
                    for (const auto& it : ops[0].items) {
                        if (it.kind == Object::String) {
                            emit(it.str, font ? *font : FontMap{});
                            lastX_ += advance(it.str, font ? *font : FontMap{}) * scale;
                        } else if (it.kind == Object::Number) {
                            if (it.number < -200 && !out_.empty() && out_.back() != ' ') out_ += ' ';
                            lastX_ -= it.number / 1000 * fontSize_ * scale;
                        }
                    }
                }
            } else if (op == "Do") {
                if (!ops.empty() && ops[0].kind == Object::Name) {
                    const Object* xo = resources.get("XObject");
                    if (xo) {
                        const Object& xd = file_.resolve(*xo);
                        if (const Object* x = xd.get(ops[0].str)) {
                            const Object& form = file_.resolve(*x);
                            if (form.kind == Object::Stream && form.get("Subtype") && form.get("Subtype")->isName("Form")) {
                                const Object* fr = form.get("Resources");
                                Object formRes = fr ? file_.resolve(*fr) : resources;
                                try {
                                    runContent(file_.streamData(form), formRes, depth + 1);
                                } catch (const std::exception&) {
                                }
                            }
                        }
                    }
                }
            }
            ops.clear();
        }
    }
};

}  // namespace

std::string PdfFile::pageText(int index) const {
    if (index < 0 || index >= pageCount()) return {};
    TextExtractor x(*this);
    return x.run(pages_[static_cast<std::size_t>(index)].dict);
}

// ---- writing selected pages --------------------------------------------------

namespace {

std::string numText(double v) {
    if (std::abs(v - std::round(v)) < 1e-9 && std::abs(v) < 1e15) return std::to_string(static_cast<long long>(std::llround(v)));
    char buf[64];
    std::snprintf(buf, sizeof buf, "%.6f", v);
    std::string s = buf;
    while (!s.empty() && s.back() == '0') s.pop_back();
    if (!s.empty() && s.back() == '.') s.pop_back();
    return s;
}

std::string nameText(const std::string& n) {
    std::string out = "/";
    for (char c : n) {
        unsigned char b = static_cast<unsigned char>(c);
        if (b < 33 || b > 126 || c == '#' || isDelim(c)) {
            char buf[4];
            std::snprintf(buf, sizeof buf, "#%02X", b);
            out += buf;
        } else {
            out += c;
        }
    }
    return out;
}

struct Assembler {
    struct Key {
        std::size_t file;
        int num;
        bool operator<(const Key& o) const { return file != o.file ? file < o.file : num < o.num; }
    };
    std::vector<const PdfFile*> files;
    std::vector<std::set<int>> pageObjects;  // every page object number per file
    std::map<Key, int> selectedPage;         // first new number of a selected page
    std::map<Key, int> mapped;
    std::vector<std::string> out;  // new object i+1
    std::vector<Key> queue;

    int alloc() {
        out.emplace_back();
        return static_cast<int>(out.size());
    }

    int refFor(std::size_t f, int num) {
        Key k{f, num};
        auto sp = selectedPage.find(k);
        if (sp != selectedPage.end()) return sp->second;
        if (pageObjects[f].count(num)) return 0;  // a page that isn't coming along
        auto it = mapped.find(k);
        if (it != mapped.end()) return it->second;
        if (files[f]->object(num).kind == Object::Null) return 0;
        int id = alloc();
        mapped[k] = id;
        queue.push_back(k);
        return id;
    }

    std::string write(std::size_t f, const Object& o) {
        switch (o.kind) {
            case Object::Null: return "null";
            case Object::Bool: return o.boolean ? "true" : "false";
            case Object::Number: return numText(o.number);
            case Object::String: {
                std::string s = "<";
                static const char* hx = "0123456789ABCDEF";
                for (char c : o.str) {
                    s += hx[(static_cast<unsigned char>(c) >> 4) & 15];
                    s += hx[static_cast<unsigned char>(c) & 15];
                }
                return s + ">";
            }
            case Object::Name: return nameText(o.str);
            case Object::Array: {
                std::string s = "[";
                for (std::size_t i = 0; i < o.items.size(); ++i) s += (i ? " " : "") + write(f, o.items[i]);
                return s + "]";
            }
            case Object::Ref: {
                int id = refFor(f, static_cast<int>(o.number));
                return id ? std::to_string(id) + " 0 R" : "null";
            }
            case Object::Dict:
            case Object::Stream: {
                std::string s = "<<";
                for (std::size_t i = 0; i < o.keys.size(); ++i) {
                    if (o.kind == Object::Stream && o.keys[i] == "Length") continue;
                    s += nameText(o.keys[i]) + " " + write(f, o.items[i]) + " ";
                }
                if (o.kind == Object::Stream) {
                    s += "/Length " + std::to_string(o.str.size()) + " >>\nstream\n" + o.str + "\nendstream";
                    return s;
                }
                return s + ">>";
            }
        }
        return "null";
    }
};

}  // namespace

std::string assemblePdf(const std::vector<PageSource>& pages) {
    Assembler a;
    std::map<const PdfFile*, std::size_t> fileIndex;
    for (const auto& p : pages) {
        if (!p.file) throw std::runtime_error("missing PDF");
        if (p.file->encrypted()) throw std::runtime_error("encrypted PDFs can't be merged or split");
        if (p.index < 0 || p.index >= p.file->pageCount()) throw std::runtime_error("page out of range");
        if (!fileIndex.count(p.file)) {
            fileIndex[p.file] = a.files.size();
            a.files.push_back(p.file);
            std::set<int> nums;
            for (const auto& pi : p.file->pages()) nums.insert(pi.object);
            a.pageObjects.push_back(std::move(nums));
        }
    }
    const int catalog = a.alloc();
    const int pagesId = a.alloc();
    std::vector<int> pageIds;
    for (std::size_t i = 0; i < pages.size(); ++i) pageIds.push_back(a.alloc());
    for (std::size_t i = 0; i < pages.size(); ++i) {
        Assembler::Key k{fileIndex[pages[i].file], pages[i].file->pages()[static_cast<std::size_t>(pages[i].index)].object};
        if (!a.selectedPage.count(k)) a.selectedPage[k] = pageIds[i];
    }
    for (std::size_t i = 0; i < pages.size(); ++i) {
        const std::size_t f = fileIndex[pages[i].file];
        const auto& info = pages[i].file->pages()[static_cast<std::size_t>(pages[i].index)];
        Object d = info.dict;
        d.erase("Parent");
        d.erase("StructParents");
        d.erase("B");
        const int rot = (((info.rotate + pages[i].rotate) % 360) + 360) % 360;
        if (rot) d.set("Rotate", Object::makeNumber(rot));
        else d.erase("Rotate");
        std::string body = a.write(f, d);
        body.insert(2, "/Parent " + std::to_string(pagesId) + " 0 R ");
        a.out[static_cast<std::size_t>(pageIds[i] - 1)] = body;
    }
    while (!a.queue.empty()) {
        Assembler::Key k = a.queue.back();
        a.queue.pop_back();
        const int id = a.mapped[k];
        a.out[static_cast<std::size_t>(id - 1)] = a.write(k.file, a.files[k.file]->object(k.num));
    }
    std::string kids;
    for (int id : pageIds) kids += (kids.empty() ? "" : " ") + std::to_string(id) + " 0 R";
    a.out[static_cast<std::size_t>(pagesId - 1)] = "<< /Type /Pages /Kids [" + kids + "] /Count " + std::to_string(pageIds.size()) + " >>";
    a.out[static_cast<std::size_t>(catalog - 1)] = "<< /Type /Catalog /Pages " + std::to_string(pagesId) + " 0 R >>";
    const int infoId = a.alloc();
    a.out[static_cast<std::size_t>(infoId - 1)] = "<< /Producer (wetwaresuite) >>";

    std::string out = "%PDF-1.7\n%\xE2\xE3\xCF\xD3\n";
    std::vector<std::size_t> offsets;
    for (std::size_t i = 0; i < a.out.size(); ++i) {
        offsets.push_back(out.size());
        out += std::to_string(i + 1) + " 0 obj\n" + (a.out[i].empty() ? "null" : a.out[i]) + "\nendobj\n";
    }
    const std::size_t xref = out.size();
    out += "xref\n0 " + std::to_string(a.out.size() + 1) + "\n0000000000 65535 f \n";
    for (auto off : offsets) {
        char buf[32];
        std::snprintf(buf, sizeof buf, "%010zu 00000 n \n", off);
        out += buf;
    }
    out += "trailer\n<< /Size " + std::to_string(a.out.size() + 1) + " /Root " + std::to_string(catalog) + " 0 R /Info " +
           std::to_string(infoId) + " 0 R >>\nstartxref\n" + std::to_string(xref) + "\n%%EOF\n";
    return out;
}

std::vector<int> parsePageList(const std::string& spec, int count) {
    std::vector<int> out;
    std::string s;
    for (char c : spec) {
        if (c != ' ') s += c;
    }
    if (s.empty() || s == "all") {
        for (int i = 0; i < count; ++i) out.push_back(i);
        return out;
    }
    std::stringstream ss(s);
    std::string part;
    auto parseNum = [&](const std::string& t, int fallback) {
        if (t.empty()) return fallback;
        if (t == "last" || t == "end") return count;
        for (char c : t) {
            if (!std::isdigit(static_cast<unsigned char>(c))) throw std::runtime_error("bad page list '" + spec + "' (use e.g. 1-3,5,8-)");
        }
        return std::atoi(t.c_str());
    };
    while (std::getline(ss, part, ',')) {
        if (part.empty()) continue;
        auto dash = part.find('-');
        int a = 0, b = 0;
        if (dash == std::string::npos) {
            a = b = parseNum(part, 0);
        } else {
            a = parseNum(part.substr(0, dash), 1);
            b = parseNum(part.substr(dash + 1), count);
        }
        if (a < 1 || b < 1 || a > count || b > count) {
            throw std::runtime_error("page " + std::to_string(a < 1 || a > count ? a : b) + " is out of range (the PDF has " +
                                     std::to_string(count) + " pages)");
        }
        if (a <= b) {
            for (int i = a; i <= b; ++i) out.push_back(i - 1);
        } else {
            for (int i = a; i >= b; --i) out.push_back(i - 1);
        }
    }
    return out;
}

}  // namespace wetwaresuite::pdf
