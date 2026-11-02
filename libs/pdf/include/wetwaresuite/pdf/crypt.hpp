// The small amount of cryptography PDFs need, with no outside library: MD5, SHA-1/256/384/512,
// RC4 and AES (128/256, ECB and CBC), plus the PDF standard security handler (passwords on
// open, RC4 40/128-bit and AES-128/256 files) built on them. Not constant-time; it's for
// opening and protecting documents, not for a server facing attackers.
#pragma once

#include <cstdint>
#include <string>

namespace wetwaresuite::pdf::crypto {

std::string md5(const std::string& data);
std::string sha1(const std::string& data);
std::string sha256(const std::string& data);
std::string sha384(const std::string& data);
std::string sha512(const std::string& data);
std::string hmacSha256(const std::string& key, const std::string& data);
std::string rc4(const std::string& key, const std::string& data);
// AES with a 16 or 32 byte key. CBC: data a multiple of 16 unless padded; pad = PKCS#7.
std::string aesEncryptCbc(const std::string& key, const std::string& iv, const std::string& data, bool pad);
std::string aesDecryptCbc(const std::string& key, const std::string& iv, const std::string& data, bool pad);
std::string aesEncryptEcb(const std::string& key, const std::string& block16);
std::string randomBytes(std::size_t n);
std::string hex(const std::string& bytes);

}  // namespace wetwaresuite::pdf::crypto
