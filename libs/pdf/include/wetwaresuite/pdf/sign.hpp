// Digital signatures for PDFs: RSA keys and X.509 certificates (PEM), a self-signed identity
// made on the spot, signing (PAdES-style detached CMS, SHA-256) and checking the signatures a
// file carries. Everything is built in: no OpenSSL. Self-signed identities prove the file wasn't
// changed and who claims to have signed it; a certificate from an authority people trust can be
// loaded from PEM for more than that.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace wetwaresuite::pdf {

struct Identity {
    std::string keyPem, certPem;  // "-----BEGIN PRIVATE KEY-----" (or RSA PRIVATE KEY) and "-----BEGIN CERTIFICATE-----"
    std::string name;             // the certificate's common name
};

// A new 2048-bit RSA key and a self-signed certificate for it (valid ten years). Takes a second or two.
Identity makeSelfSignedIdentity(const std::string& name, const std::string& email = "", const std::string& organisation = "");
// Reads a key and certificate from PEM text (both in one file, or keyPem + certPem). Throws on anything else.
Identity loadIdentity(const std::string& pem);

struct SignOptions {
    Identity identity;
    std::string reason, location, contact;
    int page = 0;  // 0-based page the (invisible) signature field sits on
};
// Appends a signature to a finished PDF as an incremental update. The PDF must not be encrypted.
std::string signPdf(const std::string& pdf, const SignOptions& options);

struct SignatureInfo {
    std::string field, signer, time, reason, location;
    bool intact = false;       // the signed bytes hash to what was signed and the RSA signature checks out
    bool coversFile = false;   // nothing was added after it (a later revision means the file changed after signing)
    bool selfSigned = false;   // the certificate vouches for itself, so the name is only a claim
    std::string problem;       // why it isn't intact, if it isn't
};
std::vector<SignatureInfo> checkSignatures(const std::string& pdf);

namespace rsa {  // exposed for tests
std::string sign(const std::string& keyPem, const std::string& message);                // PKCS#1 v1.5 with SHA-256
bool verify(const std::string& certPem, const std::string& message, const std::string& signature);
}  // namespace rsa

}  // namespace wetwaresuite::pdf
