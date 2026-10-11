#pragma once

#include <cstdint>
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
//! adds Host (with the port unless it is 443), `Connection: close` and
//! Content-Length. Returns false -- refusing to build -- if the method, host,
//! target or any header could inject into the request line or headers (CR,
//! LF, space, control or non-ASCII bytes; a target that does not start with
//! '/'), if a caller passes one of the headers the builder owns (Host,
//! Connection, Content-Length, Transfer-Encoding), or if the port is invalid.
bool BuildRawHttpRequest(const std::string &method, const std::string &host, int port, const std::string &target,
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

//! True once `raw` holds a whole response framed by Content-Length or chunked
//! encoding, so the reader can stop without waiting for the peer to close.
//! An unframed response (neither header) is only ever complete at EOF.
bool RawHttpResponseComplete(const std::string &raw);

//! Accept the reply to a ranged GET of [offset, offset+length): a 206 must be
//! exactly `length` bytes and, if `content_range` is given, start at `offset`;
//! a 200 carries the whole file and is sliced in place. Any other status, or a
//! reply that does not cover the range, returns false.
bool AcceptRangeReply(int status, const std::string &content_range, std::string &body, std::uint64_t offset,
                      std::uint64_t length);

//! Split "https://host[:port]" into host and port (default 443). False for any
//! other scheme, an empty host, userinfo, '?', '#', '/', an IPv6 literal, or a
//! port outside 1..65535.
bool SplitHttpsOrigin(const std::string &origin, std::string &host, int &port);

} // namespace sharepoint
} // namespace duckdb
