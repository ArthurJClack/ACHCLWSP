// PDF digital signatures from scratch: big-number RSA (Montgomery arithmetic), a little DER for
// keys, certificates and CMS, and the incremental update that carries the signature.
#include "wetwaresuite/pdf/sign.hpp"

#include <algorithm>
#include <cstring>
#include <ctime>
#include <map>
#include <tuple>
#include <stdexcept>

#include "internal.hpp"
#include "wetwaresuite/pdf/crypt.hpp"
#include "wetwaresuite/pdf/reader.hpp"

namespace wetwaresuite::pdf {

namespace {

namespace cr = crypto;
using u32 = std::uint32_t;
using u64 = std::uint64_t;

// ---- big numbers: little-endian 32-bit limbs, no leading zero limbs ----
using Big = std::vector<u32>;

void trim(Big& a) {
    while (!a.empty() && a.back() == 0) a.pop_back();
}
Big fromBytes(const std::string& be) {
    Big a((be.size() + 3) / 4, 0);
    for (std::size_t i = 0; i < be.size(); ++i) {
        const std::size_t bit = (be.size() - 1 - i) * 8;
        a[bit / 32] |= u32(static_cast<unsigned char>(be[i])) << (bit % 32);
    }
    trim(a);
    return a;
}
std::string toBytes(const Big& a, std::size_t len = 0) {
    std::string out;
    for (std::size_t i = a.size(); i-- > 0;)
        for (int b = 3; b >= 0; --b) out += static_cast<char>((a[i] >> (8 * b)) & 0xFF);
    std::size_t z = 0;
    while (z < out.size() && out[z] == 0) ++z;
    out.erase(0, z);
    if (len > out.size()) out.insert(0, len - out.size(), '\0');
    return out;
}
int cmp(const Big& a, const Big& b) {
    if (a.size() != b.size()) return a.size() < b.size() ? -1 : 1;
    for (std::size_t i = a.size(); i-- > 0;)
        if (a[i] != b[i]) return a[i] < b[i] ? -1 : 1;
    return 0;
}
Big sub(Big a, const Big& b) {  // a >= b
    u64 borrow = 0;
    for (std::size_t i = 0; i < a.size(); ++i) {
        const u64 d = u64(a[i]) - (i < b.size() ? b[i] : 0) - borrow;
        a[i] = static_cast<u32>(d);
        borrow = (d >> 63) & 1;
    }
    trim(a);
    return a;
}
Big mul(const Big& a, const Big& b) {
    Big r(a.size() + b.size(), 0);
    for (std::size_t i = 0; i < a.size(); ++i) {
        u64 carry = 0;
        for (std::size_t j = 0; j < b.size(); ++j) {
            const u64 t = u64(a[i]) * b[j] + r[i + j] + carry;
            r[i + j] = static_cast<u32>(t);
            carry = t >> 32;
        }
        r[i + b.size()] += static_cast<u32>(carry);
    }
    trim(r);
    return r;
}
Big addSmall(Big a, u32 v) {
    u64 carry = v;
    for (std::size_t i = 0; i < a.size() && carry; ++i) {
        const u64 t = u64(a[i]) + carry;
        a[i] = static_cast<u32>(t);
        carry = t >> 32;
    }
    if (carry) a.push_back(static_cast<u32>(carry));
    return a;
}
Big subSmall(Big a, u32 v) { return sub(std::move(a), Big{v}); }
u32 modSmall(const Big& a, u32 m) {
    u64 r = 0;
    for (std::size_t i = a.size(); i-- > 0;) r = ((r << 32) | a[i]) % m;
    return static_cast<u32>(r);
}
Big divSmall(const Big& a, u32 m) {
    Big q(a.size(), 0);
    u64 r = 0;
    for (std::size_t i = a.size(); i-- > 0;) {
        const u64 cur = (r << 32) | a[i];
        q[i] = static_cast<u32>(cur / m);
        r = cur % m;
    }
    trim(q);
    return q;
}
std::size_t bits(const Big& a) {
    if (a.empty()) return 0;
    std::size_t n = (a.size() - 1) * 32;
    for (u32 top = a.back(); top; top >>= 1) ++n;
    return n;
}
bool bit(const Big& a, std::size_t i) { return i / 32 < a.size() && ((a[i / 32] >> (i % 32)) & 1); }
Big mod(const Big& a, const Big& m) {  // shift and subtract: fine for the few big divisions a key needs
    Big r;
    for (std::size_t i = bits(a); i-- > 0;) {
        // r = r * 2 + bit
        u32 carry = bit(a, i) ? 1 : 0;
        for (auto& limb : r) {
            const u32 next = limb >> 31;
            limb = (limb << 1) | carry;
            carry = next;
        }
        if (carry) r.push_back(carry);
        if (cmp(r, m) >= 0) r = sub(r, m);
    }
    return r;
}

// Montgomery arithmetic modulo an odd n
struct Mont {
    Big n;
    std::size_t k = 0;
    u32 n0 = 0;  // -n^-1 mod 2^32
    Big r2;      // R^2 mod n
    explicit Mont(const Big& modulus) : n(modulus), k(modulus.size()) {
        u32 inv = 1;  // Newton: inv = n0^-1 mod 2^32
        for (int i = 0; i < 5; ++i) inv *= 2 - n[0] * inv;
        n0 = ~inv + 1;
        Big x{1};
        for (std::size_t i = 0; i < 2 * 32 * k; ++i) {
            u32 carry = 0;
            for (auto& limb : x) {
                const u32 next = limb >> 31;
                limb = (limb << 1) | carry;
                carry = next;
            }
            if (carry) x.push_back(carry);
            if (cmp(x, n) >= 0) x = sub(x, n);
        }
        r2 = x;
    }
    Big mulm(const Big& a, const Big& b) const {  // a * b / R mod n (CIOS)
        std::vector<u32> t(k + 2, 0);
        for (std::size_t i = 0; i < k; ++i) {
            const u64 ai = i < a.size() ? a[i] : 0;
            u64 c = 0;
            for (std::size_t j = 0; j < k; ++j) {
                const u64 s = u64(t[j]) + ai * (j < b.size() ? b[j] : 0) + c;
                t[j] = static_cast<u32>(s);
                c = s >> 32;
            }
            u64 s = u64(t[k]) + c;
            t[k] = static_cast<u32>(s);
            t[k + 1] = static_cast<u32>(s >> 32);
            const u32 m = t[0] * n0;
            c = (u64(t[0]) + u64(m) * n[0]) >> 32;
            for (std::size_t j = 1; j < k; ++j) {
                const u64 s2 = u64(t[j]) + u64(m) * n[j] + c;
                t[j - 1] = static_cast<u32>(s2);
                c = s2 >> 32;
            }
            s = u64(t[k]) + c;
            t[k - 1] = static_cast<u32>(s);
            t[k] = t[k + 1] + static_cast<u32>(s >> 32);
        }
        Big r(t.begin(), t.begin() + static_cast<std::ptrdiff_t>(k + 1));
        trim(r);
        if (cmp(r, n) >= 0) r = sub(r, n);
        return r;
    }
    Big pow(const Big& base, const Big& e) const {  // base < n
        const Big b = mulm(base, r2);
        Big x = mulm(Big{1}, r2);
        for (std::size_t i = bits(e); i-- > 0;) {
            x = mulm(x, x);
            if (bit(e, i)) x = mulm(x, b);
        }
        return mulm(x, Big{1});
    }
};

Big randomBig(std::size_t nbits) {
    std::string bytes = cr::randomBytes((nbits + 7) / 8);
    const int extra = static_cast<int>(bytes.size() * 8 - nbits);
    bytes[0] = static_cast<char>(static_cast<unsigned char>(bytes[0]) & (0xFF >> extra));
    return fromBytes(bytes);
}

bool probablyPrime(const Big& n) {
    static const std::vector<u32> small = [] {
        std::vector<u32> p;
        for (u32 i = 3; i < 3000; i += 2) {
            bool ok = true;
            for (u32 d : p) {
                if (d * d > i) break;
                if (i % d == 0) {
                    ok = false;
                    break;
                }
            }
            if (ok) p.push_back(i);
        }
        return p;
    }();
    if (n.empty() || !(n[0] & 1)) return false;
    for (u32 p : small)
        if (modSmall(n, p) == 0) return false;
    const Big nm1 = subSmall(n, 1);
    std::size_t s = 0;
    while (!bit(nm1, s)) ++s;
    Big d = nm1;  // d = (n - 1) >> s
    for (std::size_t i = 0; i < s; ++i) d = divSmall(d, 2);
    const Mont m(n);
    for (int round = 0; round < 24; ++round) {
        Big a = randomBig(bits(n) - 2);
        a = addSmall(a, 2);
        Big x = m.pow(a, d);
        if (x == Big{1} || x == nm1) continue;
        bool composite = true;
        for (std::size_t r = 1; r < s && composite; ++r) {
            x = m.mulm(m.mulm(x, x), m.r2);  // x^2 mod n
            if (x == nm1) composite = false;
        }
        if (composite) return false;
    }
    return true;
}

Big randomPrime(std::size_t nbits, u32 e) {
    for (;;) {
        Big p = randomBig(nbits);
        p[(nbits - 1) / 32] |= u32(1) << ((nbits - 1) % 32);  // the top two bits, so p*q has all its bits
        p[(nbits - 2) / 32] |= u32(1) << ((nbits - 2) % 32);
        p[0] |= 1;
        for (int step = 0; step < 4000; ++step, p = addSmall(p, 2)) {
            if (modSmall(p, e) == 1) continue;  // e must be invertible mod p - 1
            if (probablyPrime(p)) return p;
        }
    }
}

// ---- DER ----
std::string tlv(unsigned char tag, const std::string& content) {
    std::string s(1, static_cast<char>(tag));
    const std::size_t n = content.size();
    if (n < 128) {
        s += static_cast<char>(n);
    } else {
        std::string len;
        for (std::size_t v = n; v; v >>= 8) len.insert(len.begin(), static_cast<char>(v & 0xFF));
        s += static_cast<char>(0x80 | len.size());
        s += len;
    }
    return s + content;
}
std::string seq(const std::string& c) { return tlv(0x30, c); }
std::string setOf(const std::string& c) { return tlv(0x31, c); }
std::string derInt(const Big& v) {
    std::string b = toBytes(v);
    if (b.empty() || (static_cast<unsigned char>(b[0]) & 0x80)) b.insert(b.begin(), '\0');
    return tlv(0x02, b);
}
std::string derIntSmall(long v) { return derInt(Big{static_cast<u32>(v)}); }
std::string oid(const std::string& dotted) {
    std::vector<unsigned long> parts;
    std::size_t i = 0;
    while (i < dotted.size()) {
        std::size_t j = dotted.find('.', i);
        if (j == std::string::npos) j = dotted.size();
        parts.push_back(std::stoul(dotted.substr(i, j - i)));
        i = j + 1;
    }
    std::string c(1, static_cast<char>(parts[0] * 40 + parts[1]));
    for (std::size_t k = 2; k < parts.size(); ++k) {
        std::string enc;
        unsigned long v = parts[k];
        enc.insert(enc.begin(), static_cast<char>(v & 0x7F));
        for (v >>= 7; v; v >>= 7) enc.insert(enc.begin(), static_cast<char>(0x80 | (v & 0x7F)));
        c += enc;
    }
    return tlv(0x06, c);
}
std::string derNull() { return std::string("\x05\x00", 2); }

const char* kRsaEncryption = "1.2.840.113549.1.1.1";
const char* kSha256WithRsa = "1.2.840.113549.1.1.11";
const char* kSha256 = "2.16.840.1.101.3.4.2.1";
const char* kData = "1.2.840.113549.1.7.1";
const char* kSignedData = "1.2.840.113549.1.7.2";
const char* kContentType = "1.2.840.113549.1.9.3";
const char* kMessageDigest = "1.2.840.113549.1.9.4";
const char* kSigningTime = "1.2.840.113549.1.9.5";

struct Der {
    unsigned char tag = 0;
    std::string content, whole;
};
// Reads one element at `pos` (moves past it). Throws on bad data.
Der readDer(const std::string& s, std::size_t& pos) {
    if (pos + 2 > s.size()) throw std::runtime_error("truncated DER");
    const std::size_t start = pos;
    Der d;
    d.tag = static_cast<unsigned char>(s[pos++]);
    std::size_t len = static_cast<unsigned char>(s[pos++]);
    if (len & 0x80) {
        const std::size_t n = len & 0x7F;
        if (n == 0 || n > 4 || pos + n > s.size()) throw std::runtime_error("bad DER length");
        len = 0;
        for (std::size_t i = 0; i < n; ++i) len = (len << 8) | static_cast<unsigned char>(s[pos++]);
    }
    if (pos + len > s.size()) throw std::runtime_error("truncated DER");
    d.content = s.substr(pos, len);
    pos += len;
    d.whole = s.substr(start, pos - start);
    return d;
}
std::vector<Der> children(const std::string& content) {
    std::vector<Der> out;
    std::size_t p = 0;
    while (p < content.size()) out.push_back(readDer(content, p));
    return out;
}
Der parseOne(const std::string& s) {
    std::size_t p = 0;
    return readDer(s, p);
}
std::string oidText(const std::string& content) {
    if (content.empty()) return {};
    std::string s = std::to_string(static_cast<unsigned char>(content[0]) / 40) + "." + std::to_string(static_cast<unsigned char>(content[0]) % 40);
    unsigned long v = 0;
    for (std::size_t i = 1; i < content.size(); ++i) {
        v = (v << 7) | (static_cast<unsigned char>(content[i]) & 0x7F);
        if (!(static_cast<unsigned char>(content[i]) & 0x80)) s += "." + std::to_string(v), v = 0;
    }
    return s;
}
Big intOf(const Der& d) { return fromBytes(d.content); }

// ---- PEM ----
const char* kB64 = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
std::string base64(const std::string& in) {
    std::string out;
    for (std::size_t i = 0; i < in.size(); i += 3) {
        u32 v = u32(static_cast<unsigned char>(in[i])) << 16;
        if (i + 1 < in.size()) v |= u32(static_cast<unsigned char>(in[i + 1])) << 8;
        if (i + 2 < in.size()) v |= static_cast<unsigned char>(in[i + 2]);
        out += kB64[(v >> 18) & 63];
        out += kB64[(v >> 12) & 63];
        out += i + 1 < in.size() ? kB64[(v >> 6) & 63] : '=';
        out += i + 2 < in.size() ? kB64[v & 63] : '=';
    }
    return out;
}
std::string unbase64(const std::string& in) {
    std::string out;
    u32 v = 0;
    int n = 0;
    for (char c : in) {
        const char* p = std::strchr(kB64, c);
        if (!p || !c) continue;
        v = (v << 6) | static_cast<u32>(p - kB64);
        if (++n == 4) {
            out += static_cast<char>((v >> 16) & 0xFF), out += static_cast<char>((v >> 8) & 0xFF), out += static_cast<char>(v & 0xFF);
            v = 0, n = 0;
        }
    }
    if (n == 3) out += static_cast<char>((v >> 10) & 0xFF), out += static_cast<char>((v >> 2) & 0xFF);
    if (n == 2) out += static_cast<char>((v >> 4) & 0xFF);
    return out;
}
std::string pem(const std::string& label, const std::string& der) {
    std::string b = base64(der), out = "-----BEGIN " + label + "-----\n";
    for (std::size_t i = 0; i < b.size(); i += 64) out += b.substr(i, 64) + "\n";
    return out + "-----END " + label + "-----\n";
}
// The DER inside the first PEM block whose label is one of `labels` (empty when there's none).
std::string pemBlock(const std::string& text, std::initializer_list<const char*> labels, std::string* found = nullptr) {
    for (const char* label : labels) {
        const std::string begin = std::string("-----BEGIN ") + label + "-----", end = std::string("-----END ") + label + "-----";
        const std::size_t a = text.find(begin);
        if (a == std::string::npos) continue;
        const std::size_t b = text.find(end, a);
        if (b == std::string::npos) continue;
        if (found) *found = label;
        return unbase64(text.substr(a + begin.size(), b - a - begin.size()));
    }
    return {};
}

// ---- keys and certificates ----
struct RsaKey {
    Big n, e, d, p, q;
};
RsaKey parseKey(const std::string& pemText) {
    std::string label;
    std::string der = pemBlock(pemText, {"PRIVATE KEY", "RSA PRIVATE KEY"}, &label);
    if (der.empty()) {
        if (pemText.find("ENCRYPTED PRIVATE KEY") != std::string::npos) throw std::runtime_error("the private key is password-protected; export it without a password");
        throw std::runtime_error("no private key in that file (expected -----BEGIN PRIVATE KEY-----)");
    }
    if (label == "PRIVATE KEY") {  // PKCS#8: version, algorithm, the PKCS#1 key in an octet string
        const auto parts = children(parseOne(der).content);
        if (parts.size() < 3 || oidText(children(parts[1].content).at(0).content) != kRsaEncryption) throw std::runtime_error("only RSA keys are supported");
        der = parts[2].content;
    }
    const auto f = children(parseOne(der).content);
    if (f.size() < 6) throw std::runtime_error("damaged RSA key");
    return {intOf(f[1]), intOf(f[2]), intOf(f[3]), intOf(f[4]), intOf(f[5])};
}

struct Cert {
    std::string der, tbs, issuer, subject, serial;  // issuer/subject: whole Name DER; serial: the INTEGER DER
    Big n, e;
    std::string name;
};
std::string commonName(const std::string& nameDer) {
    for (const auto& rdn : children(parseOne(nameDer).content))
        for (const auto& atv : children(rdn.content)) {
            const auto kv = children(atv.content);
            if (kv.size() >= 2 && oidText(kv[0].content) == "2.5.4.3") return kv[1].content;
        }
    return {};
}
Cert parseCertDer(const std::string& der) {
    Cert c;
    c.der = der;
    const auto top = children(parseOne(der).content);
    if (top.empty()) throw std::runtime_error("damaged certificate");
    c.tbs = top[0].whole;
    auto t = children(top[0].content);
    std::size_t i = 0;
    if (!t.empty() && t[0].tag == 0xA0) ++i;  // [0] version
    if (t.size() < i + 6) throw std::runtime_error("damaged certificate");
    c.serial = t[i].whole;
    c.issuer = t[i + 2].whole;
    c.subject = t[i + 4].whole;
    const auto spki = children(t[i + 5].content);
    if (spki.size() < 2 || oidText(children(spki[0].content).at(0).content) != kRsaEncryption) throw std::runtime_error("only RSA certificates are supported");
    const std::string bitstr = spki[1].content.substr(1);  // skip the unused-bits byte
    const auto key = children(parseOne(bitstr).content);
    c.n = intOf(key.at(0));
    c.e = intOf(key.at(1));
    c.name = commonName(c.subject);
    return c;
}
Cert parseCert(const std::string& pemText) {
    const std::string der = pemBlock(pemText, {"CERTIFICATE"});
    if (der.empty()) throw std::runtime_error("no certificate in that file (expected -----BEGIN CERTIFICATE-----)");
    return parseCertDer(der);
}

const std::string kSha256Prefix("\x30\x31\x30\x0d\x06\x09\x60\x86\x48\x01\x65\x03\x04\x02\x01\x05\x00\x04\x20", 19);

std::string rsaSignRaw(const RsaKey& k, const std::string& message) {
    const std::size_t len = (bits(k.n) + 7) / 8;
    const std::string t = kSha256Prefix + cr::sha256(message);
    if (len < t.size() + 11) throw std::runtime_error("RSA key too small");
    const std::string em = std::string("\x00\x01", 2) + std::string(len - t.size() - 3, '\xFF') + std::string(1, '\0') + t;
    const Mont m(k.n);
    return toBytes(m.pow(fromBytes(em), k.d), len);
}

// PKCS#1 v1.5 check for SHA-1/256/384/512 signatures (other apps sign with these too)
bool rsaVerifyRaw(const Big& n, const Big& e, const std::string& message, const std::string& sig, const std::string& digestOid) {
    const std::size_t len = (bits(n) + 7) / 8;
    const Big s = fromBytes(sig);
    if (cmp(s, n) >= 0) return false;
    const std::string em = toBytes(Mont(n).pow(s, e), len);
    if (em.size() < 11 || em[0] != 0 || em[1] != 1) return false;
    std::size_t i = 2;
    while (i < em.size() && static_cast<unsigned char>(em[i]) == 0xFF) ++i;
    if (i >= em.size() || em[i] != 0 || i < 10) return false;
    try {
        const Der info = parseOne(em.substr(i + 1));
        const auto parts = children(info.content);
        const std::string alg = oidText(children(parts.at(0).content).at(0).content);
        const std::string digest = parts.at(1).content;
        const std::string want = alg == "1.3.14.3.2.26" ? cr::sha1(message) : alg == "2.16.840.1.101.3.4.2.2" ? cr::sha384(message)
                               : alg == "2.16.840.1.101.3.4.2.3" ? cr::sha512(message) : alg == kSha256 ? cr::sha256(message) : std::string();
        (void)digestOid;
        return !want.empty() && want == digest;
    } catch (const std::exception&) {
        return false;
    }
}

std::string utcTime(std::time_t t) {
    std::tm g{};
#ifdef _WIN32
    gmtime_s(&g, &t);
#else
    gmtime_r(&t, &g);
#endif
    char b[32];
    if (g.tm_year + 1900 >= 2050) {
        std::strftime(b, sizeof b, "%Y%m%d%H%M%SZ", &g);
        return tlv(0x18, b);  // GeneralizedTime from 2050 on
    }
    std::strftime(b, sizeof b, "%y%m%d%H%M%SZ", &g);
    return tlv(0x17, b);
}

std::string nameDer(const std::string& cn, const std::string& org, const std::string& email) {
    std::string rdns;
    if (!org.empty()) rdns += setOf(seq(oid("2.5.4.10") + tlv(0x0C, org)));
    rdns += setOf(seq(oid("2.5.4.3") + tlv(0x0C, cn)));
    if (!email.empty()) rdns += setOf(seq(oid("1.2.840.113549.1.9.1") + tlv(0x16, email)));
    return seq(rdns);
}

std::string textString(const std::string& utf8) {
    bool ascii = true;
    for (char c : utf8) ascii = ascii && static_cast<unsigned char>(c) < 128 && c != '\n';
    if (ascii) return utf8;
    std::string s = "\xFE\xFF";
    std::size_t i = 0;
    while (i < utf8.size()) {  // UTF-8 -> UTF-16BE
        unsigned cp = static_cast<unsigned char>(utf8[i]);
        int extra = cp >= 0xF0 ? 3 : cp >= 0xE0 ? 2 : cp >= 0xC0 ? 1 : 0;
        cp &= extra == 3 ? 0x07 : extra == 2 ? 0x0F : extra == 1 ? 0x1F : 0x7F;
        ++i;
        for (int k = 0; k < extra && i < utf8.size(); ++k, ++i) cp = (cp << 6) | (static_cast<unsigned char>(utf8[i]) & 0x3F);
        auto unit = [&](unsigned u) { s += static_cast<char>((u >> 8) & 0xFF), s += static_cast<char>(u & 0xFF); };
        if (cp >= 0x10000) {
            cp -= 0x10000;
            unit(0xD800 + (cp >> 10));
            unit(0xDC00 + (cp & 0x3FF));
        } else {
            unit(cp);
        }
    }
    return s;
}
Object str(const std::string& bytes) {
    Object o;
    o.kind = Object::String;
    o.str = bytes;
    return o;
}

std::string pdfDate(std::time_t t) {
    std::tm g{};
#ifdef _WIN32
    gmtime_s(&g, &t);
#else
    gmtime_r(&t, &g);
#endif
    char b[40];
    std::strftime(b, sizeof b, "D:%Y%m%d%H%M%S+00'00'", &g);
    return b;
}

std::string readableTime(const Der& t) {
    std::string s = t.content;
    if (t.tag == 0x17 && s.size() >= 12) s = (s.substr(0, 2) < "50" ? "20" : "19") + s;
    if (s.size() < 14) return s;
    return s.substr(0, 4) + "-" + s.substr(4, 2) + "-" + s.substr(6, 2) + " " + s.substr(8, 2) + ":" + s.substr(10, 2) + " UTC";
}

}  // namespace

namespace rsa {
std::string sign(const std::string& keyPem, const std::string& message) { return rsaSignRaw(parseKey(keyPem), message); }
bool verify(const std::string& certPem, const std::string& message, const std::string& signature) {
    const Cert c = parseCert(certPem);
    return rsaVerifyRaw(c.n, c.e, message, signature, kSha256);
}
}  // namespace rsa

Identity makeSelfSignedIdentity(const std::string& name, const std::string& email, const std::string& organisation) {
    const u32 e = 65537;
    Big p = randomPrime(1024, e), q = randomPrime(1024, e);
    while (p == q) q = randomPrime(1024, e);
    if (cmp(p, q) < 0) std::swap(p, q);
    const Big n = mul(p, q);
    const Big p1 = subSmall(p, 1), q1 = subSmall(q, 1);
    const Big phi = mul(p1, q1);
    // d = (1 + k*phi) / e with k = -phi^-1 mod e, all small-number work
    const u64 phiMod = modSmall(phi, e);
    long long t = 0, newT = 1, r = e, newR = static_cast<long long>(phiMod);
    while (newR) {
        const long long qq = r / newR;
        std::tie(t, newT) = std::make_pair(newT, t - qq * newT);
        std::tie(r, newR) = std::make_pair(newR, r - qq * newR);
    }
    const long long inv = ((t % static_cast<long long>(e)) + e) % e;  // phi^-1 mod e
    const u32 k = static_cast<u32>((e - inv) % e);
    Big kphi = mul(phi, Big{k});
    const Big d = divSmall(addSmall(kphi, 1), e);
    const Big dp = mod(d, p1), dq = mod(d, q1);
    const Big qinv = Mont(p).pow(mod(q, p), subSmall(p, 2));  // Fermat: q^(p-2) mod p
    const std::string pkcs1 = seq(derIntSmall(0) + derInt(n) + derInt(Big{e}) + derInt(d) + derInt(p) + derInt(q) + derInt(dp) + derInt(dq) + derInt(qinv));
    const std::string pkcs8 = seq(derIntSmall(0) + seq(oid(kRsaEncryption) + derNull()) + tlv(0x04, pkcs1));

    const std::string algo = seq(oid(kSha256WithRsa) + derNull());
    const std::string subject = nameDer(name.empty() ? std::string("wetwaresuite user") : name, organisation, email);
    const std::time_t now = std::time(nullptr);
    std::string serial = cr::randomBytes(16);
    serial[0] = static_cast<char>(static_cast<unsigned char>(serial[0]) & 0x7F);
    const std::string spki = seq(seq(oid(kRsaEncryption) + derNull()) + tlv(0x03, std::string(1, '\0') + seq(derInt(n) + derInt(Big{e}))));
    // extensions: key usage = digital signature + non-repudiation
    const std::string keyUsage = seq(oid("2.5.29.15") + tlv(0x01, std::string(1, '\xFF')) + tlv(0x04, tlv(0x03, std::string("\x06\xC0", 2))));
    const std::string tbs = seq(tlv(0xA0, derIntSmall(2)) + tlv(0x02, serial) + algo + subject + seq(utcTime(now - 60) + utcTime(now + 10LL * 365 * 86400)) +
                                subject + spki + tlv(0xA3, seq(keyUsage)));
    const RsaKey key{n, Big{e}, d, p, q};
    const std::string cert = seq(tbs + algo + tlv(0x03, std::string(1, '\0') + rsaSignRaw(key, tbs)));
    Identity id;
    id.keyPem = pem("PRIVATE KEY", pkcs8);
    id.certPem = pem("CERTIFICATE", cert);
    id.name = name;
    return id;
}

Identity loadIdentity(const std::string& pemText) {
    Identity id;
    const RsaKey k = parseKey(pemText);
    const Cert c = parseCert(pemText);
    if (cmp(k.n, c.n) != 0) throw std::runtime_error("the private key doesn't belong to that certificate");
    id.keyPem = pemText;
    id.certPem = pem("CERTIFICATE", c.der);
    id.name = c.name;
    return id;
}

std::string signPdf(const std::string& pdf, const SignOptions& o) {
    const PdfFile file(pdf);
    if (file.wasEncrypted()) throw std::runtime_error("password-protected PDFs can't be signed here; save it unprotected, sign, then protect");
    if (file.pageCount() == 0) throw std::runtime_error("the PDF has no pages");
    const RsaKey key = parseKey(o.identity.keyPem);
    const Cert cert = parseCert(o.identity.certPem);
    if (cmp(key.n, cert.n) != 0) throw std::runtime_error("the private key doesn't belong to the certificate");

    const std::size_t prevXref = [&] {
        const std::size_t sx = pdf.rfind("startxref");
        if (sx == std::string::npos) throw std::runtime_error("the PDF has no cross-reference table");
        return static_cast<std::size_t>(std::strtoull(pdf.c_str() + sx + 9, nullptr, 10));
    }();
    const Object* rootRef = file.trailer().get("Root");
    if (!rootRef || rootRef->kind != Object::Ref) throw std::runtime_error("the PDF has no catalog");
    const int catNum = static_cast<int>(rootRef->number);
    const int pageIndex = std::clamp(o.page, 0, file.pageCount() - 1);
    const int pageNum = file.pages()[static_cast<std::size_t>(pageIndex)].object;
    int next = file.maxObject() + 1;
    const int fieldNum = next++, sigNum = next++;

    // how many signatures are there already, for the field's name
    int existing = 0;
    for (const auto& [num, ob] : file.objects()) {
        const Object* ft = ob.get("FT");
        if (ft && ft->isName("Sig")) ++existing;
    }
    const std::string fieldName = "Signature" + std::to_string(existing + 1);

    // the catalog with the field in its AcroForm
    Object cat = file.catalog();
    cat.kind = Object::Dict;
    cat.str.clear();
    Object form;
    form.kind = Object::Dict;
    if (const Object* af = cat.get("AcroForm")) form = file.resolve(*af);
    form.kind = Object::Dict;
    Object fields;
    fields.kind = Object::Array;
    if (const Object* f = form.get("Fields")) fields = file.resolve(*f);
    fields.kind = Object::Array;
    fields.items.push_back(Object::makeRef(fieldNum));
    form.set("Fields", fields);
    form.set("SigFlags", Object::makeNumber(3));
    cat.set("AcroForm", form);

    // the page with the field's widget in its annotations
    Object page = file.object(pageNum);
    page.kind = Object::Dict;
    page.str.clear();
    Object annots;
    annots.kind = Object::Array;
    if (const Object* a = page.get("Annots")) annots = file.resolve(*a);
    annots.kind = Object::Array;
    annots.items.push_back(Object::makeRef(fieldNum));
    page.set("Annots", annots);

    std::string fieldBody = "<</Type /Annot /Subtype /Widget /FT /Sig /T " + detail::writeObject(str(fieldName)) + " /V " + std::to_string(sigNum) +
                            " 0 R /Rect [0 0 0 0] /F 132 /P " + std::to_string(pageNum) + " 0 R>>";
    const std::time_t now = std::time(nullptr);
    const std::size_t kSpace = 16384;  // bytes of signature room (hex doubles it)
    std::string sigBody = "<</Type /Sig /Filter /Adobe.PPKLite /SubFilter /adbe.pkcs7.detached /ByteRange [0 0000000000 0000000000 0000000000] /Contents <" +
                          std::string(kSpace * 2, '0') + "> /M " + detail::writeObject(str(pdfDate(now)));
    const std::string who = cert.name.empty() ? o.identity.name : cert.name;
    if (!who.empty()) sigBody += " /Name " + detail::writeObject(str(textString(who)));
    if (!o.reason.empty()) sigBody += " /Reason " + detail::writeObject(str(textString(o.reason)));
    if (!o.location.empty()) sigBody += " /Location " + detail::writeObject(str(textString(o.location)));
    if (!o.contact.empty()) sigBody += " /ContactInfo " + detail::writeObject(str(textString(o.contact)));
    sigBody += ">>";

    std::string out = pdf;
    if (!out.empty() && out.back() != '\n') out += '\n';
    std::vector<std::pair<int, std::size_t>> offsets;
    auto put = [&](int num, const std::string& body) {
        offsets.emplace_back(num, out.size());
        out += std::to_string(num) + " 0 obj\n" + body + "\nendobj\n";
    };
    put(catNum, detail::writeObject(cat));
    put(pageNum, detail::writeObject(page));
    put(fieldNum, fieldBody);
    const std::size_t sigAt = out.size() + std::to_string(sigNum).size() + 7;
    put(sigNum, sigBody);
    std::sort(offsets.begin(), offsets.end());
    const std::size_t xref = out.size();
    out += "xref\n0 1\n0000000000 65535 f \n";
    for (const auto& [num, off] : offsets) {
        char line[64];
        std::snprintf(line, sizeof line, "%d 1\n%010zu 00000 n \n", num, off);
        out += line;
    }
    out += "trailer\n<</Size " + std::to_string(next) + " /Root " + std::to_string(catNum) + " 0 R";
    if (const Object* info = file.trailer().get("Info")) out += " /Info " + detail::writeObject(*info);
    if (const Object* ids = file.trailer().get("ID")) out += " /ID " + detail::writeObject(*ids);
    out += " /Prev " + std::to_string(prevXref) + ">>\nstartxref\n" + std::to_string(xref) + "\n%%EOF\n";

    // the byte ranges: everything but the hex inside /Contents < ... >
    const std::size_t lt = out.find("/Contents <", sigAt) + 10, gt = out.find('>', lt);
    const std::size_t brAt = out.find("/ByteRange [0 ", sigAt) + 14;
    char br[40];
    std::snprintf(br, sizeof br, "%010zu %010zu %010zu", lt, gt + 1, out.size() - gt - 1);
    out.replace(brAt, 32, br);
    const std::string signedBytes = out.substr(0, lt) + out.substr(gt + 1);

    // CMS SignedData, detached, with content type, signing time and the digest as signed attributes
    const std::string digestAlg = seq(oid(kSha256) + derNull());
    const std::string attrs = seq(oid(kContentType) + setOf(oid(kData))) + seq(oid(kSigningTime) + setOf(utcTime(now))) +
                              seq(oid(kMessageDigest) + setOf(tlv(0x04, cr::sha256(signedBytes))));
    const std::string signature = rsaSignRaw(key, setOf(attrs));
    const std::string signerInfo = seq(derIntSmall(1) + seq(cert.issuer + cert.serial) + digestAlg + tlv(0xA0, attrs) + seq(oid(kRsaEncryption) + derNull()) +
                                       tlv(0x04, signature));
    const std::string signedData = seq(derIntSmall(1) + setOf(digestAlg) + seq(oid(kData)) + tlv(0xA0, cert.der) + setOf(signerInfo));
    const std::string cms = seq(oid(kSignedData) + tlv(0xA0, signedData));
    if (cms.size() > kSpace) throw std::runtime_error("the signature doesn't fit (certificate too big)");
    const std::string hexSig = cr::hex(cms);
    out.replace(lt + 1, hexSig.size(), hexSig);
    return out;
}

std::vector<SignatureInfo> checkSignatures(const std::string& pdf) {
    const PdfFile file(pdf);
    std::map<int, std::string> fieldOf;  // signature dict object -> field name
    for (const auto& [num, ob] : file.objects()) {
        const Object* ft = ob.get("FT");
        const Object* v = ob.get("V");
        if (ft && ft->isName("Sig") && v && v->kind == Object::Ref) fieldOf[static_cast<int>(v->number)] = decodeTextString(ob.get("T") ? ob.get("T")->str : "");
    }
    std::vector<SignatureInfo> out;
    for (const auto& [num, ob] : file.objects()) {
        const Object* br = ob.get("ByteRange");
        const Object* contents = ob.get("Contents");
        if (!br || br->kind != Object::Array || br->items.size() != 4 || !contents || contents->kind != Object::String) continue;
        SignatureInfo s;
        s.field = fieldOf.count(num) ? fieldOf[num] : "signature";
        if (const Object* r = ob.get("Reason")) s.reason = decodeTextString(r->str);
        if (const Object* l = ob.get("Location")) s.location = decodeTextString(l->str);
        if (const Object* n = ob.get("Name")) s.signer = decodeTextString(n->str);
        if (const Object* m = ob.get("M")) s.time = m->str;
        const auto a = static_cast<std::size_t>(br->items[0].number), b = static_cast<std::size_t>(br->items[1].number);
        const auto c = static_cast<std::size_t>(br->items[2].number), d = static_cast<std::size_t>(br->items[3].number);
        if (a + b > pdf.size() || c + d > pdf.size()) {
            s.problem = "its byte ranges run past the end of the file";
            out.push_back(s);
            continue;
        }
        std::size_t endPos = pdf.size();
        while (endPos > c + d && (pdf[endPos - 1] == '\n' || pdf[endPos - 1] == '\r' || pdf[endPos - 1] == ' ')) --endPos;
        s.coversFile = c + d >= endPos;
        const std::string signedBytes = pdf.substr(a, b) + pdf.substr(c, d);
        try {
            const auto ci = children(parseOne(contents->str).content);
            if (ci.size() < 2 || oidText(ci[0].content) != kSignedData) throw std::runtime_error("not a CMS signature");
            const auto sd = children(children(ci[1].content).at(0).content);
            std::vector<Cert> certs;
            const Der* signerSet = nullptr;
            for (const auto& part : sd) {
                if (part.tag == 0xA0)
                    for (const auto& cd : children(part.content)) {
                        try {
                            certs.push_back(parseCertDer(cd.whole));
                        } catch (const std::exception&) {
                        }
                    }
                if (part.tag == 0x31 && &part == &sd.back()) signerSet = &part;
            }
            if (!signerSet) throw std::runtime_error("no signer");
            const auto si = children(children(signerSet->content).at(0).content);
            // sid, digest algorithm, [0] signed attributes, signature algorithm, signature
            const auto sid = children(si.at(1).content);
            const Cert* signer = nullptr;
            for (const auto& ct : certs)
                if (sid.size() == 2 && ct.issuer == sid[0].whole && ct.serial == sid[1].whole) signer = &ct;
            if (!signer && !certs.empty()) signer = &certs.front();
            if (!signer) throw std::runtime_error("the signer's certificate isn't in the file");
            s.signer = signer->name.empty() ? s.signer : signer->name;
            s.selfSigned = signer->issuer == signer->subject;
            const std::string digestOid = oidText(children(si.at(2).content).at(0).content);
            auto hashOf = [&](const std::string& data) {
                return digestOid == "1.3.14.3.2.26" ? cr::sha1(data) : digestOid == "2.16.840.1.101.3.4.2.2" ? cr::sha384(data)
                     : digestOid == "2.16.840.1.101.3.4.2.3" ? cr::sha512(data) : cr::sha256(data);
            };
            std::size_t idx = 3;
            std::string signedPart = signedBytes;
            if (si.at(3).tag == 0xA0) {
                std::string md;
                for (const auto& attr : children(si[3].content)) {
                    const auto kv = children(attr.content);
                    const std::string id = oidText(kv.at(0).content);
                    const auto vals = children(kv.at(1).content);
                    if (id == kMessageDigest && !vals.empty()) md = vals[0].content;
                    if (id == kSigningTime && !vals.empty()) s.time = readableTime(vals[0]);
                }
                if (md != hashOf(signedBytes)) {
                    s.problem = "the document was changed after it was signed";
                    out.push_back(s);
                    continue;
                }
                signedPart = std::string(1, '\x31') + si[3].whole.substr(1);  // signed attributes are signed as a SET
                idx = 4;
            }
            const std::string sig = si.at(idx + 1).content;
            s.intact = rsaVerifyRaw(signer->n, signer->e, signedPart, sig, digestOid);
            if (!s.intact) s.problem = "the signature doesn't match the signer's certificate";
        } catch (const std::exception& e) {
            s.problem = std::string("couldn't read the signature: ") + e.what();
        }
        out.push_back(s);
    }
    return out;
}

}  // namespace wetwaresuite::pdf
