#pragma once

#include "sharepoint_rawhttp.hpp"

#include <cstddef>
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
//! Every phase is bounded by `timeout_seconds` (TCP connect via a deadline,
//! then socket timeouts installed before the TLS handshake), and the response
//! is capped at `max_body_bytes` of body: a peer that sends more -- e.g. a
//! download host ignoring Range -- is cut off with an error instead of being
//! buffered whole.
//!
//! Returns the parsed response for ANY HTTP status. Throws:
//!   * InvalidInputException -- the request was REFUSED before anything was
//!     sent (not an https URL, malformed URL, unsafe request shape). Retrying
//!     cannot help.
//!   * IOException -- connect/TLS/timeout/oversize/truncation failures.
//! The exception text never contains the URL's path or query (a credential).
RawHttpResponse PreauthenticatedRequest(const std::string &method, const std::string &url,
                                        const RawHeaders &headers, const std::string &body, int timeout_seconds,
                                        std::size_t max_body_bytes);

//! Same, with the body as a non-owning (pointer, length): it is written to the
//! socket straight from the caller's memory, after the head, so an upload
//! chunk is never copied into a request string.
RawHttpResponse PreauthenticatedRequest(const std::string &method, const std::string &url,
                                        const RawHeaders &headers, const char *body, std::size_t body_size,
                                        int timeout_seconds, std::size_t max_body_bytes);

} // namespace sharepoint
} // namespace duckdb
