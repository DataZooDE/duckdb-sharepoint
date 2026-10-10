#pragma once

#include "sharepoint_rawhttp.hpp"

#include <string>

namespace duckdb {
namespace sharepoint {

//! Send one request to a Graph PRE-AUTHENTICATED https URL (the /content
//! download redirect, an upload session's uploadUrl) with the URL's path and
//! query sent byte for byte -- see sharepoint_rawhttp.hpp for why httplib
//! cannot. TLS with peer and hostname verification against the system trust
//! store; one connection per request (`Connection: close`). No Authorization
//! header is ever added: the URL is the credential.
//!
//! Returns the parsed response for ANY HTTP status; throws IOException on a
//! non-https URL, a refused request shape, or a transport/TLS failure. The
//! exception text never contains the URL (its query is a credential).
RawHttpResponse PreauthenticatedRequest(const std::string &method, const std::string &url,
                                        const RawHeaders &headers, const std::string &body,
                                        int timeout_seconds);

} // namespace sharepoint
} // namespace duckdb
