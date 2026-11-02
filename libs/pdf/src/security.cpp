// The PDF standard security handler (ISO 32000-2, 7.6.4): opening RC4 and AES files with the
// user or owner password, and writing AES-256 protected copies.
#include <algorithm>
#include <cstring>
#include <stdexcept>

#include "wetwaresuite/pdf/crypt.hpp"
#include "wetwaresuite/pdf/reader.hpp"
#include "internal.hpp"

namespace wetwaresuite::pdf {

namespace {

namespace cr = crypto;

const char kPad[] = "\x28\xBF\x4E\x5E\x4E\x75\x8A\x41\x64\x00\x4E\x56\xFF\xFA\x01\x08\x2E\x2E\x00\xB6\xD0\x68\x3E\x80\x2F\x0C\xA9\xFE\x64\x53\x69\x7A";
const std::string kPadding(kPad, 32);

std::string padPassword(const std::string& pw) { return (pw.substr(0, 32) + kPadding).substr(0, 32); }

std::string le32(std::int32_t v) {
    std::string s;
    for (int i = 0; i < 4; ++i) s += static_cast<char>((static_cast<std::uint32_t>(v) >> (8 * i)) & 0xFF);
    return s;
}

std::string xorKey(const std::string& key, int i) {
    std::string k = key;
    for (char& c : k) c = static_cast<char>(static_cast<unsigned char>(c) ^ i);
    return k;
}

// Algorithm 2.B: the hardened hash of revision 6 (revision 5 is a single SHA-256)
std::string hash2B(const std::string& pw, const std::string& salt, const std::string& udata, int revision) {
    std::string k = cr::sha256(pw + salt + udata);
    if (revision < 6) return k;
    int i = 0;
    unsigned last = 0;
    do {
        std::string k1;
        const std::string unit = pw + k + udata;
        k1.reserve(unit.size() * 64);
        for (int r = 0; r < 64; ++r) k1 += unit;
        const std::string e = cr::aesEncryptCbc(k.substr(0, 16), k.substr(16, 16), k1, false);
        unsigned sum = 0;
        for (int b = 0; b < 16; ++b) sum += static_cast<unsigned char>(e[static_cast<std::size_t>(b)]);
        k = sum % 3 == 0 ? cr::sha256(e) : sum % 3 == 1 ? cr::sha384(e) : cr::sha512(e);
        last = static_cast<unsigned char>(e.back());
        ++i;
    } while (i < 64 || i < static_cast<int>(last) + 32);
    return k.substr(0, 32);
}

enum class Method { None, Rc4, Aes128, Aes256 };

struct Handler {
    int revision = 0, version = 0;
    std::string key;
    Method strings = Method::None, streams = Method::None;
    bool encryptMetadata = true;

    std::string objectKey(int num, int gen, Method m) const {
        if (m == Method::Aes256) return key;
        std::string k = key;
        k += static_cast<char>(num & 0xFF), k += static_cast<char>((num >> 8) & 0xFF), k += static_cast<char>((num >> 16) & 0xFF);
        k += static_cast<char>(gen & 0xFF), k += static_cast<char>((gen >> 8) & 0xFF);
        if (m == Method::Aes128) k += "sAlT";
        return cr::md5(k).substr(0, std::min<std::size_t>(key.size() + 5, 16));
    }
    std::string decrypt(const std::string& data, int num, int gen, Method m) const {
        if (m == Method::None) return data;
        const std::string k = objectKey(num, gen, m);
        if (m == Method::Rc4) return cr::rc4(k, data);
        if (data.size() < 32) return data.size() == 16 ? std::string() : data;  // an empty string is just the IV
        return cr::aesDecryptCbc(k, data.substr(0, 16), data.substr(16), true);
    }
    std::string encrypt(const std::string& data, int num, int gen, Method m) const {
        if (m == Method::None) return data;
        const std::string k = objectKey(num, gen, m);
        if (m == Method::Rc4) return cr::rc4(k, data);
        const std::string iv = cr::randomBytes(16);
        return iv + cr::aesEncryptCbc(k, iv, data, true);
    }
};

double numberOf(const Object* o, double fallback) { return o && o->kind == Object::Number ? o->number : fallback; }
std::string stringOf(const Object* o) { return o && o->kind == Object::String ? o->str : std::string(); }

Method methodFor(const Object& enc, const std::string& filterKey, const PdfFile* file, const std::map<int, Object>& objects) {
    auto resolved = [&](const Object* o) -> const Object* {
        for (int hops = 0; o && o->kind == Object::Ref && hops < 8; ++hops) {
            auto it = objects.find(static_cast<int>(o->number));
            o = it == objects.end() ? nullptr : &it->second;
        }
        return o;
    };
    (void)file;
    const Object* name = enc.get(filterKey);
    if (!name || name->kind != Object::Name) return Method::Rc4;  // V4 without StmF/StrF: Identity per spec, but RC4 files exist without them
    if (name->str == "Identity") return Method::None;
    const Object* cf = resolved(enc.get("CF"));
    const Object* f = cf ? resolved(cf->get(name->str)) : nullptr;
    const Object* cfm = f ? f->get("CFM") : nullptr;
    if (!cfm || cfm->kind != Object::Name || cfm->str == "None") return Method::None;
    if (cfm->str == "V2") return Method::Rc4;
    if (cfm->str == "AESV2") return Method::Aes128;
    if (cfm->str == "AESV3") return Method::Aes256;
    throw std::runtime_error("this PDF uses an encryption method that isn't supported (" + cfm->str + ")");
}

void cryptStrings(Object& o, const Handler& h, int num, int gen, bool encrypting) {
    switch (o.kind) {
        case Object::String:
            o.str = encrypting ? h.encrypt(o.str, num, gen, h.strings) : h.decrypt(o.str, num, gen, h.strings);
            break;
        case Object::Array:
        case Object::Dict:
        case Object::Stream: {
            // a signature's Contents is left as it is (it holds the signature over the file's bytes)
            const bool sig = (o.kind == Object::Dict) && o.get("ByteRange");
            for (std::size_t i = 0; i < o.items.size(); ++i) {
                if (sig && i < o.keys.size() && o.keys[i] == "Contents") continue;
                cryptStrings(o.items[i], h, num, gen, encrypting);
            }
            break;
        }
        default: break;
    }
}

bool isXrefOrObjStm(const Object& o, bool objStmToo) {
    const Object* t = o.get("Type");
    return t && t->kind == Object::Name && (t->str == "XRef" || (objStmToo && t->str == "ObjStm"));
}

// ---- writing ----
std::string nameText(const std::string& n) {
    std::string s = "/";
    for (unsigned char c : n) {
        if (c < 33 || c > 126 || std::strchr("#()<>[]{}/%", c)) {
            static const char* hx = "0123456789ABCDEF";
            s += '#', s += hx[c >> 4], s += hx[c & 15];
        } else {
            s += static_cast<char>(c);
        }
    }
    return s;
}

std::string numText(double v) {
    if (v == static_cast<double>(static_cast<long long>(v))) return std::to_string(static_cast<long long>(v));
    char b[64];
    std::snprintf(b, sizeof b, "%.6f", v);
    std::string s = b;
    while (!s.empty() && s.back() == '0') s.pop_back();
    if (!s.empty() && s.back() == '.') s.pop_back();
    return s;
}

}  // namespace

std::string detail::writeObject(const Object& o) {
    switch (o.kind) {
        case Object::Null: return "null";
        case Object::Bool: return o.boolean ? "true" : "false";
        case Object::Number: return numText(o.number);
        case Object::String: return "<" + cr::hex(o.str) + ">";
        case Object::Name: return nameText(o.str);
        case Object::Ref: return std::to_string(static_cast<int>(o.number)) + " 0 R";  // everything is written as generation 0
        case Object::Array: {
            std::string s = "[";
            for (std::size_t i = 0; i < o.items.size(); ++i) s += (i ? " " : "") + detail::writeObject(o.items[i]);
            return s + "]";
        }
        case Object::Dict:
        case Object::Stream: {
            std::string s = "<<";
            for (std::size_t i = 0; i < o.keys.size(); ++i) {
                if (o.kind == Object::Stream && o.keys[i] == "Length") continue;
                s += nameText(o.keys[i]) + " " + detail::writeObject(o.items[i]) + " ";
            }
            if (o.kind == Object::Stream) return s + "/Length " + std::to_string(o.str.size()) + " >>\nstream\n" + o.str + "\nendstream";
            return s + ">>";
        }
    }
    return "null";
}

namespace {

}  // namespace

void PdfFile::unlock(const std::string& password) {
    const Object* encRef = trailer_.get("Encrypt");
    if (!encRef) {  // only an older trailer had it: treat the file as plain
        encrypted_ = false;
        return;
    }
    int encNum = encRef->kind == Object::Ref ? static_cast<int>(encRef->number) : -1;
    const Object enc = encRef->kind == Object::Ref ? object(encNum) : *encRef;
    const Object* filter = enc.get("Filter");
    if (!filter || !filter->isName("Standard")) throw std::runtime_error("this PDF is locked with a certificate or plug-in, not a password; it can't be opened here");
    Handler h;
    h.version = static_cast<int>(numberOf(enc.get("V"), 0));
    h.revision = static_cast<int>(numberOf(enc.get("R"), 2));
    const Object* em = enc.get("EncryptMetadata");
    h.encryptMetadata = !(em && em->kind == Object::Bool && !em->boolean);
    const std::string O = stringOf(enc.get("O")), U = stringOf(enc.get("U"));
    const std::int32_t P = static_cast<std::int32_t>(static_cast<long long>(numberOf(enc.get("P"), -4)));
    permissions_ = P;
    std::string id0;
    if (const Object* id = trailer_.get("ID"); id && id->kind == Object::Array && !id->items.empty()) id0 = id->items[0].str;

    bool ok = false;
    if (h.revision >= 5) {
        if (O.size() < 48 || U.size() < 48) throw std::runtime_error("this PDF's password data is damaged");
        const std::string pw = password.substr(0, 127);
        const std::string u48 = U.substr(0, 48);
        if (hash2B(pw, O.substr(32, 8), u48, h.revision) == O.substr(0, 32)) {
            h.key = cr::aesDecryptCbc(hash2B(pw, O.substr(40, 8), u48, h.revision), std::string(16, '\0'), stringOf(enc.get("OE")), false);
            ok = owner_ = true;
        } else if (hash2B(pw, U.substr(32, 8), "", h.revision) == U.substr(0, 32)) {
            h.key = cr::aesDecryptCbc(hash2B(pw, U.substr(40, 8), "", h.revision), std::string(16, '\0'), stringOf(enc.get("UE")), false);
            ok = true;
        }
        h.strings = h.streams = Method::Aes256;
        if (h.version >= 4) {
            h.strings = methodFor(enc, "StrF", this, objects_);
            h.streams = methodFor(enc, "StmF", this, objects_);
        }
        security_ = "AES-256";
    } else {
        const int n = h.revision == 2 ? 5 : std::clamp(static_cast<int>(numberOf(enc.get("Length"), 40)) / 8, 5, 16);
        auto fileKey = [&](const std::string& pw) {
            std::string k = cr::md5(padPassword(pw) + O + le32(P) + id0 + (h.revision >= 4 && !h.encryptMetadata ? std::string("\xFF\xFF\xFF\xFF") : std::string()));
            if (h.revision >= 3)
                for (int i = 0; i < 50; ++i) k = cr::md5(k.substr(0, static_cast<std::size_t>(n)));
            return k.substr(0, static_cast<std::size_t>(n));
        };
        auto userFits = [&](const std::string& key) {
            if (h.revision == 2) return cr::rc4(key, kPadding) == U;
            std::string x = cr::rc4(key, cr::md5(kPadding + id0));
            for (int i = 1; i <= 19; ++i) x = cr::rc4(xorKey(key, i), x);
            return U.size() >= 16 && x == U.substr(0, 16);
        };
        // the owner password unlocks the user password out of O
        std::string ok2 = cr::md5(padPassword(password));
        if (h.revision >= 3)
            for (int i = 0; i < 50; ++i) ok2 = cr::md5(ok2);
        const std::string ownerKey = ok2.substr(0, static_cast<std::size_t>(n));
        std::string userPw;
        if (h.revision == 2) {
            userPw = cr::rc4(ownerKey, O);
        } else {
            userPw = O;
            for (int i = 19; i >= 0; --i) userPw = cr::rc4(xorKey(ownerKey, i), userPw);
        }
        if (std::string k = fileKey(userPw); userFits(k)) {
            h.key = k;
            ok = owner_ = true;
        } else if (std::string k2 = fileKey(password); userFits(k2)) {
            h.key = k2;
            ok = true;
        }
        h.strings = h.streams = Method::Rc4;
        if (h.version >= 4) {
            h.strings = methodFor(enc, "StrF", this, objects_);
            h.streams = methodFor(enc, "StmF", this, objects_);
        }
        const bool aes = h.strings == Method::Aes128 || h.streams == Method::Aes128;
        security_ = aes ? "AES-128" : "RC4 " + std::to_string(n * 8) + "-bit";
    }
    if (!ok) {
        if (password.empty()) throw PasswordError("this PDF is password-protected; give its password", false);
        throw PasswordError("that password doesn't open this PDF", true);
    }
    // an empty user password: anyone may open it, but only the owner may do what the flags forbid
    if (!owner_ && P == -1) owner_ = true;

    for (auto& [num, o] : objects_) {
        if (num == encNum) continue;
        if (o.kind == Object::Stream && isXrefOrObjStm(o, false)) continue;
        cryptStrings(o, h, num, o.gen, false);
        if (o.kind == Object::Stream) {
            const Object* t = o.get("Type");
            if (!h.encryptMetadata && t && t->isName("Metadata")) continue;
            // a stream with its own /Crypt filter naming Identity stays as it is
            const Object* f = o.get("Filter");
            if (f && (f->isName("Crypt") || (f->kind == Object::Array && !f->items.empty() && f->items[0].isName("Crypt")))) continue;
            o.str = h.decrypt(o.str, num, o.gen, h.streams);
        }
    }
    objects_.erase(encNum);
    trailer_.erase("Encrypt");
    encrypted_ = false;
}

std::string protectPdf(const std::string& pdfBytes, const ProtectOptions& options) {
    const PdfFile file(pdfBytes);
    const std::string user = options.userPassword.substr(0, 127);
    std::string owner = options.ownerPassword.substr(0, 127);
    if (owner.empty()) owner = user.empty() ? cr::hex(cr::randomBytes(16)) : user;

    Handler h;
    h.revision = 6;
    h.version = 5;
    h.key = cr::randomBytes(32);
    h.strings = h.streams = Method::Aes256;
    // permission flags: bits 1-2 zero, 7-8 and 13-32 one
    std::uint32_t p = 0xFFFFF0C0u;
    if (options.print) p |= 4u | 2048u;
    if (options.edit) p |= 8u | 1024u;
    if (options.copy) p |= 16u | 512u;
    if (options.annotate) p |= 32u | 256u;
    const std::int32_t P = static_cast<std::int32_t>(p);

    const std::string uv = cr::randomBytes(8), uk = cr::randomBytes(8);
    const std::string U = hash2B(user, uv, "", 6) + uv + uk;
    const std::string UE = cr::aesEncryptCbc(hash2B(user, uk, "", 6), std::string(16, '\0'), h.key, false);
    const std::string ov = cr::randomBytes(8), ok = cr::randomBytes(8);
    const std::string O = hash2B(owner, ov, U, 6) + ov + ok;
    const std::string OE = cr::aesEncryptCbc(hash2B(owner, ok, U, 6), std::string(16, '\0'), h.key, false);
    const std::string perms = cr::aesEncryptEcb(h.key, le32(P) + std::string("\xFF\xFF\xFF\xFF", 4) + "Tadb" + cr::randomBytes(4));

    // every object again, its strings and streams encrypted, written plainly with a classic xref table
    std::vector<std::pair<int, std::string>> bodies;
    int maxNum = 0;
    for (const auto& [num, src] : file.objects()) {
        if (src.kind == Object::Stream && isXrefOrObjStm(src, true)) continue;
        Object o = src;
        const int gen = 0;
        cryptStrings(o, h, num, gen, true);
        if (o.kind == Object::Stream) o.str = h.encrypt(o.str, num, gen, h.streams);
        bodies.emplace_back(num, detail::writeObject(o));
        maxNum = std::max(maxNum, num);
    }
    const int encNum = maxNum + 1;
    std::string enc = "<</Filter /Standard /V 5 /R 6 /Length 256 /CF <</StdCF <</AuthEvent /DocOpen /CFM /AESV3 /Length 32>>>> /StmF /StdCF /StrF /StdCF";
    enc += " /O <" + cr::hex(O) + "> /U <" + cr::hex(U) + "> /OE <" + cr::hex(OE) + "> /UE <" + cr::hex(UE) + "> /P " + std::to_string(P) +
           " /Perms <" + cr::hex(perms) + "> /EncryptMetadata true>>";
    bodies.emplace_back(encNum, enc);

    std::string out = "%PDF-2.0\n%\xE2\xE3\xCF\xD3\n";
    std::vector<std::size_t> offsets(static_cast<std::size_t>(encNum) + 1, 0);
    for (const auto& [num, body] : bodies) {
        offsets[static_cast<std::size_t>(num)] = out.size();
        out += std::to_string(num) + " 0 obj\n" + body + "\nendobj\n";
    }
    const std::size_t xref = out.size();
    out += "xref\n0 " + std::to_string(encNum + 1) + "\n0000000000 65535 f \n";
    for (int i = 1; i <= encNum; ++i) {
        char line[32];
        const std::size_t off = offsets[static_cast<std::size_t>(i)];
        std::snprintf(line, sizeof line, off ? "%010zu 00000 n \n" : "%010zu 65535 f \n", off);
        out += line;
    }
    std::string id;
    if (const Object* ids = file.trailer().get("ID"); ids && ids->kind == Object::Array && !ids->items.empty()) id = ids->items[0].str;
    if (id.empty()) id = cr::randomBytes(16);
    out += "trailer\n<</Size " + std::to_string(encNum + 1);
    if (const Object* root = file.trailer().get("Root")) out += " /Root " + detail::writeObject(*root);
    if (const Object* info = file.trailer().get("Info")) out += " /Info " + detail::writeObject(*info);
    out += " /Encrypt " + std::to_string(encNum) + " 0 R /ID [<" + cr::hex(id) + "> <" + cr::hex(id) + ">]>>\nstartxref\n" + std::to_string(xref) + "\n%%EOF\n";
    return out;
}

}  // namespace wetwaresuite::pdf
