#pragma once

#include <cstdint>
#include <string>

namespace duckdb {
namespace sharepoint {

// ---------------------------------------------------------------------------
// The client assertion for PROVIDER client_certificate (#71). PURE: no
// duckdb.hpp, no I/O, no OpenSSL -- the shape of the JWT. Signing it is
// sharepoint_certificate.hpp.
//
// Entra's client-credentials grant takes, instead of client_secret,
//   client_assertion_type=urn:ietf:params:oauth:client-assertion-type:jwt-bearer
//   client_assertion=<header>.<claims>.<RS256 signature>
// with the certificate's SHA-1 thumbprint (base64url) as the header's x5t.
// ---------------------------------------------------------------------------

//! Unpadded base64url (RFC 4648 §5), as JWTs use.
std::string Base64Url(const std::string &bytes);

//! base64url(header) "." base64url(claims): what gets signed. The claims:
//! aud (the token endpoint), iss = sub = the client id, jti, nbf = iat =
//! `now`, exp = now + 10 minutes.
std::string ClientAssertionSigningInput(const std::string &client_id, const std::string &audience,
                                        const std::string &x5t, const std::string &jti, int64_t now);

} // namespace sharepoint
} // namespace duckdb
