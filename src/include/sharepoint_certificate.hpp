#pragma once

#include <string>

namespace duckdb {
namespace sharepoint {

// ---------------------------------------------------------------------------
// Certificate credentials (#71): OpenSSL over PEM text, no duckdb.hpp and no
// file I/O (the caller reads CERTIFICATE_PATH). Never puts key material in an
// error message.
// ---------------------------------------------------------------------------

//! The base64url SHA-1 thumbprint of the certificate in `pem` (the JWT `x5t`).
bool CertificateThumbprint(const std::string &pem, std::string &x5t, std::string &error);

//! RS256 (RSASSA-PKCS1-v1_5, SHA-256) signature of `input` with the RSA
//! private key in `pem`, decrypted with `password` when it is encrypted.
bool SignRs256(const std::string &pem, const std::string &password, const std::string &input,
               std::string &signature, std::string &error);

} // namespace sharepoint
} // namespace duckdb
