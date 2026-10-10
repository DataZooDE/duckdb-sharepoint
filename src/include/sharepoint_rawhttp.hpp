#pragma once

#include <string>
#include <utility>
#include <vector>

namespace duckdb {
namespace sharepoint {

// ---------------------------------------------------------------------------
// Verbatim HTTP/1.1 framing for Graph's PRE-AUTHENTICATED URLs -- the /content
// download redirect and the upload-session uploadUrl. Pure (no I/O): the TLS
// transport that sends these bytes lives in sharepoint_preauth.cpp.
//
// Why not httplib: the copy DuckDB 1.5 bundles (cpp-httplib 0.27) parses every
// request's query into a key-SORTED multimap and re-serialises it. SharePoint
// signs those URLs over the exact query string, so the reordered request is
// rejected with 401 (verified live: only the original order is accepted).
// DuckDB 1.4's httplib 0.14 sent the path verbatim, which is why this only
// bites on the 1.5 line.
// ---------------------------------------------------------------------------

using RawHeaders = std::vector<std::pair<std::string, std::string>>;

//! Build a complete HTTP/1.1 request with `target` sent byte for byte. Always
//! adds Host, `Connection: close` (the transport reads to EOF) and
//! Content-Length. Returns false -- refusing to build -- if the method, host,
//! target or any header could inject into the request line or headers (CR,
//! LF, space, control bytes; a target that does not start with '/').
bool BuildRawHttpRequest(const std::string &method, const std::string &host, const std::string &target,
                         const RawHeaders &headers, const std::string &body, std::string &out);

struct RawHttpResponse {
	int status = 0;
	RawHeaders headers;
	std::string body;

	//! Case-insensitive header lookup; empty if absent.
	std::string Header(const std::string &name) const;
};

//! Parse a complete response read up to connection close: status line,
//! headers, then a body framed by Transfer-Encoding: chunked, Content-Length,
//! or (neither) the rest of the stream. Returns false on a malformed or
//! truncated response.
bool ParseRawHttpResponse(const std::string &raw, RawHttpResponse &out);

//! Split "https://host[:port]" into host and port (default 443). False for any
//! other scheme, an empty host, or a port outside 1..65535.
bool SplitHttpsOrigin(const std::string &origin, std::string &host, int &port);

} // namespace sharepoint
} // namespace duckdb
