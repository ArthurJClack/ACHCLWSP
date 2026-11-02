// Hashes and ciphers for PDF security, written from the public specs (RFC 1321, FIPS 180-4,
// FIPS 197). Small and portable over fast.
#include "wetwaresuite/pdf/crypt.hpp"

#include <array>
#include <cstring>
#include <random>
#include <stdexcept>

namespace wetwaresuite::pdf::crypto {

namespace {

using u8 = std::uint8_t;
using u32 = std::uint32_t;
using u64 = std::uint64_t;

u32 rol(u32 x, int n) { return (x << n) | (x >> (32 - n)); }
u32 ror(u32 x, int n) { return (x >> n) | (x << (32 - n)); }
u64 ror64(u64 x, int n) { return (x >> n) | (x << (64 - n)); }

// Merkle-Damgard padding: 0x80, zeros, then the bit length (big or little endian, 8 or 16 bytes)
std::string mdPad(const std::string& data, std::size_t block, std::size_t lenBytes, bool bigEndian) {
    std::string m = data;
    const u64 bits = static_cast<u64>(data.size()) * 8;
    m += '\x80';
    while (m.size() % block != block - lenBytes) m += '\0';
    for (std::size_t i = 0; i < lenBytes - 8; ++i) m += '\0';
    for (int i = 0; i < 8; ++i) m += static_cast<char>(bigEndian ? (bits >> (56 - 8 * i)) & 0xFF : (bits >> (8 * i)) & 0xFF);
    return m;
}

u32 be32(const char* p) {
    return (u32(u8(p[0])) << 24) | (u32(u8(p[1])) << 16) | (u32(u8(p[2])) << 8) | u32(u8(p[3]));
}
u64 be64(const char* p) { return (u64(be32(p)) << 32) | be32(p + 4); }
void putBe32(std::string& s, u32 v) {
    for (int i = 3; i >= 0; --i) s += static_cast<char>((v >> (8 * i)) & 0xFF);
}
void putBe64(std::string& s, u64 v) {
    for (int i = 7; i >= 0; --i) s += static_cast<char>((v >> (8 * i)) & 0xFF);
}

const u32 kSha256K[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be,
    0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa,
    0x5cb0a9dc, 0x76f988da, 0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967, 0x27b70a85,
    0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3,
    0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070, 0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f,
    0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};

const u64 kSha512K[80] = {
    0x428a2f98d728ae22ULL, 0x7137449123ef65cdULL, 0xb5c0fbcfec4d3b2fULL, 0xe9b5dba58189dbbcULL, 0x3956c25bf348b538ULL, 0x59f111f1b605d019ULL,
    0x923f82a4af194f9bULL, 0xab1c5ed5da6d8118ULL, 0xd807aa98a3030242ULL, 0x12835b0145706fbeULL, 0x243185be4ee4b28cULL, 0x550c7dc3d5ffb4e2ULL,
    0x72be5d74f27b896fULL, 0x80deb1fe3b1696b1ULL, 0x9bdc06a725c71235ULL, 0xc19bf174cf692694ULL, 0xe49b69c19ef14ad2ULL, 0xefbe4786384f25e3ULL,
    0x0fc19dc68b8cd5b5ULL, 0x240ca1cc77ac9c65ULL, 0x2de92c6f592b0275ULL, 0x4a7484aa6ea6e483ULL, 0x5cb0a9dcbd41fbd4ULL, 0x76f988da831153b5ULL,
    0x983e5152ee66dfabULL, 0xa831c66d2db43210ULL, 0xb00327c898fb213fULL, 0xbf597fc7beef0ee4ULL, 0xc6e00bf33da88fc2ULL, 0xd5a79147930aa725ULL,
    0x06ca6351e003826fULL, 0x142929670a0e6e70ULL, 0x27b70a8546d22ffcULL, 0x2e1b21385c26c926ULL, 0x4d2c6dfc5ac42aedULL, 0x53380d139d95b3dfULL,
    0x650a73548baf63deULL, 0x766a0abb3c77b2a8ULL, 0x81c2c92e47edaee6ULL, 0x92722c851482353bULL, 0xa2bfe8a14cf10364ULL, 0xa81a664bbc423001ULL,
    0xc24b8b70d0f89791ULL, 0xc76c51a30654be30ULL, 0xd192e819d6ef5218ULL, 0xd69906245565a910ULL, 0xf40e35855771202aULL, 0x106aa07032bbd1b8ULL,
    0x19a4c116b8d2d0c8ULL, 0x1e376c085141ab53ULL, 0x2748774cdf8eeb99ULL, 0x34b0bcb5e19b48a8ULL, 0x391c0cb3c5c95a63ULL, 0x4ed8aa4ae3418acbULL,
    0x5b9cca4f7763e373ULL, 0x682e6ff3d6b2b8a3ULL, 0x748f82ee5defb2fcULL, 0x78a5636f43172f60ULL, 0x84c87814a1f0ab72ULL, 0x8cc702081a6439ecULL,
    0x90befffa23631e28ULL, 0xa4506cebde82bde9ULL, 0xbef9a3f7b2c67915ULL, 0xc67178f2e372532bULL, 0xca273eceea26619cULL, 0xd186b8c721c0c207ULL,
    0xeada7dd6cde0eb1eULL, 0xf57d4f7fee6ed178ULL, 0x06f067aa72176fbaULL, 0x0a637dc5a2c898a6ULL, 0x113f9804bef90daeULL, 0x1b710b35131c471bULL,
    0x28db77f523047d84ULL, 0x32caab7b40c72493ULL, 0x3c9ebe0a15c9bebcULL, 0x431d67c49c100d4cULL, 0x4cc5d4becb3e42b6ULL, 0x597f299cfc657e2aULL,
    0x5fcb6fab3ad6faecULL, 0x6c44198c4a475817ULL};

std::string sha512Family(const std::string& data, const u64 init[8], std::size_t outBytes) {
    const std::string m = mdPad(data, 128, 16, true);
    u64 h[8];
    std::memcpy(h, init, sizeof h);
    u64 w[80];
    for (std::size_t off = 0; off < m.size(); off += 128) {
        for (int i = 0; i < 16; ++i) w[i] = be64(m.data() + off + 8 * i);
        for (int i = 16; i < 80; ++i) {
            const u64 s0 = ror64(w[i - 15], 1) ^ ror64(w[i - 15], 8) ^ (w[i - 15] >> 7);
            const u64 s1 = ror64(w[i - 2], 19) ^ ror64(w[i - 2], 61) ^ (w[i - 2] >> 6);
            w[i] = w[i - 16] + s0 + w[i - 7] + s1;
        }
        u64 a = h[0], b = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], hh = h[7];
        for (int i = 0; i < 80; ++i) {
            const u64 t1 = hh + (ror64(e, 14) ^ ror64(e, 18) ^ ror64(e, 41)) + ((e & f) ^ (~e & g)) + kSha512K[i] + w[i];
            const u64 t2 = (ror64(a, 28) ^ ror64(a, 34) ^ ror64(a, 39)) + ((a & b) ^ (a & c) ^ (b & c));
            hh = g, g = f, f = e, e = d + t1, d = c, c = b, b = a, a = t1 + t2;
        }
        h[0] += a, h[1] += b, h[2] += c, h[3] += d, h[4] += e, h[5] += f, h[6] += g, h[7] += hh;
    }
    std::string out;
    for (u64 v : h) putBe64(out, v);
    return out.substr(0, outBytes);
}

// ---- AES (FIPS 197) ----
struct Aes {
    std::array<u8, 256> sbox{}, inv{};
    Aes() {
        // the S-box from the multiplicative inverse in GF(2^8) and the affine map
        u8 p = 1, q = 1;
        do {
            p = static_cast<u8>(p ^ (p << 1) ^ (p & 0x80 ? 0x1B : 0));
            q ^= static_cast<u8>(q << 1);
            q ^= static_cast<u8>(q << 2);
            q ^= static_cast<u8>(q << 4);
            if (q & 0x80) q ^= 0x09;
            const u8 x = static_cast<u8>(q ^ (q << 1 | q >> 7) ^ (q << 2 | q >> 6) ^ (q << 3 | q >> 5) ^ (q << 4 | q >> 4) ^ 0x63);
            sbox[p] = x;
        } while (p != 1);
        sbox[0] = 0x63;
        for (int i = 0; i < 256; ++i) inv[sbox[static_cast<std::size_t>(i)]] = static_cast<u8>(i);
    }
};
const Aes& aesTables() {
    static const Aes t;
    return t;
}
u8 xt(u8 x) { return static_cast<u8>((x << 1) ^ (x & 0x80 ? 0x1B : 0)); }
u8 mul(u8 a, u8 b) {
    u8 r = 0;
    while (b) {
        if (b & 1) r ^= a;
        a = xt(a);
        b >>= 1;
    }
    return r;
}

struct AesKey {
    int rounds = 0;
    std::array<u8, 240> w{};
    explicit AesKey(const std::string& key) {
        const int nk = static_cast<int>(key.size()) / 4;
        if (key.size() != 16 && key.size() != 24 && key.size() != 32) throw std::runtime_error("AES key must be 16, 24 or 32 bytes");
        rounds = nk + 6;
        const auto& s = aesTables().sbox;
        std::memcpy(w.data(), key.data(), key.size());
        u8 rcon = 1;
        for (int i = nk; i < 4 * (rounds + 1); ++i) {
            u8 t[4] = {w[static_cast<std::size_t>(4 * (i - 1))], w[static_cast<std::size_t>(4 * (i - 1) + 1)], w[static_cast<std::size_t>(4 * (i - 1) + 2)],
                       w[static_cast<std::size_t>(4 * (i - 1) + 3)]};
            if (i % nk == 0) {
                const u8 t0 = t[0];
                t[0] = static_cast<u8>(s[t[1]] ^ rcon), t[1] = s[t[2]], t[2] = s[t[3]], t[3] = s[t0];
                rcon = xt(rcon);
            } else if (nk > 6 && i % nk == 4) {
                for (u8& b : t) b = s[b];
            }
            for (int j = 0; j < 4; ++j) w[static_cast<std::size_t>(4 * i + j)] = static_cast<u8>(w[static_cast<std::size_t>(4 * (i - nk) + j)] ^ t[j]);
        }
    }
    void encrypt(u8* b) const {
        const auto& s = aesTables().sbox;
        auto addKey = [&](int r) {
            for (int i = 0; i < 16; ++i) b[i] ^= w[static_cast<std::size_t>(16 * r + i)];
        };
        addKey(0);
        for (int r = 1; r <= rounds; ++r) {
            for (int i = 0; i < 16; ++i) b[i] = s[b[i]];
            u8 t[16];  // shift rows (state is column-major)
            for (int c = 0; c < 4; ++c)
                for (int row = 0; row < 4; ++row) t[4 * c + row] = b[4 * ((c + row) % 4) + row];
            std::memcpy(b, t, 16);
            if (r != rounds) {
                for (int c = 0; c < 4; ++c) {
                    u8* col = b + 4 * c;
                    const u8 a0 = col[0], a1 = col[1], a2 = col[2], a3 = col[3];
                    col[0] = static_cast<u8>(xt(a0) ^ xt(a1) ^ a1 ^ a2 ^ a3);
                    col[1] = static_cast<u8>(a0 ^ xt(a1) ^ xt(a2) ^ a2 ^ a3);
                    col[2] = static_cast<u8>(a0 ^ a1 ^ xt(a2) ^ xt(a3) ^ a3);
                    col[3] = static_cast<u8>(xt(a0) ^ a0 ^ a1 ^ a2 ^ xt(a3));
                }
            }
            addKey(r);
        }
    }
    void decrypt(u8* b) const {
        const auto& inv = aesTables().inv;
        auto addKey = [&](int r) {
            for (int i = 0; i < 16; ++i) b[i] ^= w[static_cast<std::size_t>(16 * r + i)];
        };
        addKey(rounds);
        for (int r = rounds - 1; r >= 0; --r) {
            u8 t[16];  // inverse shift rows
            for (int c = 0; c < 4; ++c)
                for (int row = 0; row < 4; ++row) t[4 * ((c + row) % 4) + row] = b[4 * c + row];
            std::memcpy(b, t, 16);
            for (int i = 0; i < 16; ++i) b[i] = inv[b[i]];
            addKey(r);
            if (r != 0) {
                for (int c = 0; c < 4; ++c) {
                    u8* col = b + 4 * c;
                    const u8 a0 = col[0], a1 = col[1], a2 = col[2], a3 = col[3];
                    col[0] = static_cast<u8>(mul(a0, 14) ^ mul(a1, 11) ^ mul(a2, 13) ^ mul(a3, 9));
                    col[1] = static_cast<u8>(mul(a0, 9) ^ mul(a1, 14) ^ mul(a2, 11) ^ mul(a3, 13));
                    col[2] = static_cast<u8>(mul(a0, 13) ^ mul(a1, 9) ^ mul(a2, 14) ^ mul(a3, 11));
                    col[3] = static_cast<u8>(mul(a0, 11) ^ mul(a1, 13) ^ mul(a2, 9) ^ mul(a3, 14));
                }
            }
        }
    }
};

}  // namespace

std::string md5(const std::string& data) {
    static const u32 k[64] = {
        0xd76aa478, 0xe8c7b756, 0x242070db, 0xc1bdceee, 0xf57c0faf, 0x4787c62a, 0xa8304613, 0xfd469501, 0x698098d8, 0x8b44f7af, 0xffff5bb1,
        0x895cd7be, 0x6b901122, 0xfd987193, 0xa679438e, 0x49b40821, 0xf61e2562, 0xc040b340, 0x265e5a51, 0xe9b6c7aa, 0xd62f105d, 0x02441453,
        0xd8a1e681, 0xe7d3fbc8, 0x21e1cde6, 0xc33707d6, 0xf4d50d87, 0x455a14ed, 0xa9e3e905, 0xfcefa3f8, 0x676f02d9, 0x8d2a4c8a, 0xfffa3942,
        0x8771f681, 0x6d9d6122, 0xfde5380c, 0xa4beea44, 0x4bdecfa9, 0xf6bb4b60, 0xbebfbc70, 0x289b7ec6, 0xeaa127fa, 0xd4ef3085, 0x04881d05,
        0xd9d4d039, 0xe6db99e5, 0x1fa27cf8, 0xc4ac5665, 0xf4292244, 0x432aff97, 0xab9423a7, 0xfc93a039, 0x655b59c3, 0x8f0ccc92, 0xffeff47d,
        0x85845dd1, 0x6fa87e4f, 0xfe2ce6e0, 0xa3014314, 0x4e0811a1, 0xf7537e82, 0xbd3af235, 0x2ad7d2bb, 0xeb86d391};
    static const int r[64] = {7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 5, 9,  14, 20, 5, 9,  14, 20, 5, 9,  14, 20, 5, 9,  14, 20,
                              4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21};
    const std::string m = mdPad(data, 64, 8, false);
    u32 h[4] = {0x67452301, 0xefcdab89, 0x98badcfe, 0x10325476};
    for (std::size_t off = 0; off < m.size(); off += 64) {
        u32 w[16];
        for (int i = 0; i < 16; ++i) {
            const char* p = m.data() + off + 4 * i;
            w[i] = u32(u8(p[0])) | (u32(u8(p[1])) << 8) | (u32(u8(p[2])) << 16) | (u32(u8(p[3])) << 24);
        }
        u32 a = h[0], b = h[1], c = h[2], d = h[3];
        for (int i = 0; i < 64; ++i) {
            u32 f;
            int g;
            if (i < 16) f = (b & c) | (~b & d), g = i;
            else if (i < 32) f = (d & b) | (~d & c), g = (5 * i + 1) % 16;
            else if (i < 48) f = b ^ c ^ d, g = (3 * i + 5) % 16;
            else f = c ^ (b | ~d), g = (7 * i) % 16;
            const u32 t = d;
            d = c, c = b;
            b = b + rol(a + f + k[i] + w[g], r[i]);
            a = t;
        }
        h[0] += a, h[1] += b, h[2] += c, h[3] += d;
    }
    std::string out;
    for (u32 v : h)
        for (int i = 0; i < 4; ++i) out += static_cast<char>((v >> (8 * i)) & 0xFF);
    return out;
}

std::string sha1(const std::string& data) {
    const std::string m = mdPad(data, 64, 8, true);
    u32 h[5] = {0x67452301, 0xEFCDAB89, 0x98BADCFE, 0x10325476, 0xC3D2E1F0};
    u32 w[80];
    for (std::size_t off = 0; off < m.size(); off += 64) {
        for (int i = 0; i < 16; ++i) w[i] = be32(m.data() + off + 4 * i);
        for (int i = 16; i < 80; ++i) w[i] = rol(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
        u32 a = h[0], b = h[1], c = h[2], d = h[3], e = h[4];
        for (int i = 0; i < 80; ++i) {
            u32 f, k;
            if (i < 20) f = (b & c) | (~b & d), k = 0x5A827999;
            else if (i < 40) f = b ^ c ^ d, k = 0x6ED9EBA1;
            else if (i < 60) f = (b & c) | (b & d) | (c & d), k = 0x8F1BBCDC;
            else f = b ^ c ^ d, k = 0xCA62C1D6;
            const u32 t = rol(a, 5) + f + e + k + w[i];
            e = d, d = c, c = rol(b, 30), b = a, a = t;
        }
        h[0] += a, h[1] += b, h[2] += c, h[3] += d, h[4] += e;
    }
    std::string out;
    for (u32 v : h) putBe32(out, v);
    return out;
}

std::string sha256(const std::string& data) {
    const std::string m = mdPad(data, 64, 8, true);
    u32 h[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    u32 w[64];
    for (std::size_t off = 0; off < m.size(); off += 64) {
        for (int i = 0; i < 16; ++i) w[i] = be32(m.data() + off + 4 * i);
        for (int i = 16; i < 64; ++i) {
            const u32 s0 = ror(w[i - 15], 7) ^ ror(w[i - 15], 18) ^ (w[i - 15] >> 3);
            const u32 s1 = ror(w[i - 2], 17) ^ ror(w[i - 2], 19) ^ (w[i - 2] >> 10);
            w[i] = w[i - 16] + s0 + w[i - 7] + s1;
        }
        u32 a = h[0], b = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], hh = h[7];
        for (int i = 0; i < 64; ++i) {
            const u32 t1 = hh + (ror(e, 6) ^ ror(e, 11) ^ ror(e, 25)) + ((e & f) ^ (~e & g)) + kSha256K[i] + w[i];
            const u32 t2 = (ror(a, 2) ^ ror(a, 13) ^ ror(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
            hh = g, g = f, f = e, e = d + t1, d = c, c = b, b = a, a = t1 + t2;
        }
        h[0] += a, h[1] += b, h[2] += c, h[3] += d, h[4] += e, h[5] += f, h[6] += g, h[7] += hh;
    }
    std::string out;
    for (u32 v : h) putBe32(out, v);
    return out;
}

std::string sha384(const std::string& data) {
    static const u64 init[8] = {0xcbbb9d5dc1059ed8ULL, 0x629a292a367cd507ULL, 0x9159015a3070dd17ULL, 0x152fecd8f70e5939ULL,
                                0x67332667ffc00b31ULL, 0x8eb44a8768581511ULL, 0xdb0c2e0d64f98fa7ULL, 0x47b5481dbefa4fa4ULL};
    return sha512Family(data, init, 48);
}

std::string sha512(const std::string& data) {
    static const u64 init[8] = {0x6a09e667f3bcc908ULL, 0xbb67ae8584caa73bULL, 0x3c6ef372fe94f82bULL, 0xa54ff53a5f1d36f1ULL,
                                0x510e527fade682d1ULL, 0x9b05688c2b3e6c1fULL, 0x1f83d9abfb41bd6bULL, 0x5be0cd19137e2179ULL};
    return sha512Family(data, init, 64);
}

std::string hmacSha256(const std::string& key, const std::string& data) {
    std::string k = key.size() > 64 ? sha256(key) : key;
    k.resize(64, '\0');
    std::string ipad(64, '\0'), opad(64, '\0');
    for (int i = 0; i < 64; ++i) ipad[i] = static_cast<char>(k[i] ^ 0x36), opad[i] = static_cast<char>(k[i] ^ 0x5c);
    return sha256(opad + sha256(ipad + data));
}

std::string rc4(const std::string& key, const std::string& data) {
    if (key.empty()) return data;
    u8 s[256];
    for (int i = 0; i < 256; ++i) s[i] = static_cast<u8>(i);
    for (int i = 0, j = 0; i < 256; ++i) {
        j = (j + s[i] + u8(key[static_cast<std::size_t>(i) % key.size()])) & 255;
        std::swap(s[i], s[j]);
    }
    std::string out = data;
    int i = 0, j = 0;
    for (char& c : out) {
        i = (i + 1) & 255;
        j = (j + s[i]) & 255;
        std::swap(s[i], s[j]);
        c = static_cast<char>(u8(c) ^ s[(s[i] + s[j]) & 255]);
    }
    return out;
}

std::string aesEncryptCbc(const std::string& key, const std::string& iv, const std::string& data, bool pad) {
    const AesKey k(key);
    std::string in = data;
    if (pad) in.append(16 - in.size() % 16, static_cast<char>(16 - in.size() % 16));
    if (in.size() % 16) throw std::runtime_error("AES-CBC data must be whole 16-byte blocks");
    std::string out = in;
    u8 prev[16];
    std::memcpy(prev, iv.data(), 16);
    for (std::size_t off = 0; off < out.size(); off += 16) {
        u8* b = reinterpret_cast<u8*>(&out[off]);
        for (int i = 0; i < 16; ++i) b[i] ^= prev[i];
        k.encrypt(b);
        std::memcpy(prev, b, 16);
    }
    return out;
}

std::string aesDecryptCbc(const std::string& key, const std::string& iv, const std::string& data, bool pad) {
    const AesKey k(key);
    std::string out = data.substr(0, data.size() - data.size() % 16);
    u8 prev[16], cur[16];
    std::memcpy(prev, iv.data(), 16);
    for (std::size_t off = 0; off < out.size(); off += 16) {
        u8* b = reinterpret_cast<u8*>(&out[off]);
        std::memcpy(cur, b, 16);
        k.decrypt(b);
        for (int i = 0; i < 16; ++i) b[i] ^= prev[i];
        std::memcpy(prev, cur, 16);
    }
    if (pad && !out.empty()) {
        const unsigned n = u8(out.back());
        if (n >= 1 && n <= 16 && n <= out.size()) out.resize(out.size() - n);
    }
    return out;
}

std::string aesEncryptEcb(const std::string& key, const std::string& block16) {
    const AesKey k(key);
    std::string out = block16.substr(0, 16);
    out.resize(16, '\0');
    k.encrypt(reinterpret_cast<u8*>(&out[0]));
    return out;
}

std::string randomBytes(std::size_t n) {
    // the system's random source (/dev/urandom, BCryptGenRandom...): keys and salts come from here
    static std::random_device rd;
    std::string out(n, '\0');
    for (std::size_t i = 0; i < n; i += 4) {
        const u32 v = rd();
        for (std::size_t j = 0; j < 4 && i + j < n; ++j) out[i + j] = static_cast<char>((v >> (8 * j)) & 0xFF);
    }
    return out;
}

std::string hex(const std::string& bytes) {
    static const char* d = "0123456789abcdef";
    std::string s;
    for (char c : bytes) s += d[u8(c) >> 4], s += d[u8(c) & 15];
    return s;
}

}  // namespace wetwaresuite::pdf::crypto
