#include "sharepoint_rawhttp.hpp"

#include <cstdint>
#include <cstdlib>

namespace duckdb {
namespace sharepoint {

namespace {

// Visible ASCII only: anything else (CR, LF, space, other controls, DEL,
// non-ASCII) could split the request line or a header.
bool IsVisibleAscii(const std::string &s) {
	for (unsigned char c : s) {
		if (c <= 0x20 || c >= 0x7F) {
			return false;
		}
	}
	return !s.empty();
}

// A header value may contain spaces and tabs, but never CR, LF, other control
// bytes or non-ASCII (obs-text is legal but no caller needs it).
bool IsSafeHeaderValue(const std::string &s) {
	for (unsigned char c : s) {
		if ((c < 0x20 && c != '\t') || c >= 0x7F) {
			return false;
		}
	}
	return true;
}

char Lower(char c) {
	return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
}

bool EqualsIgnoreCase(const std::string &a, const std::string &b) {
	if (a.size() != b.size()) {
		return false;
	}
	for (std::size_t i = 0; i < a.size(); i++) {
		if (Lower(a[i]) != Lower(b[i])) {
			return false;
		}
	}
	return true;
}

std::string Trim(const std::string &s) {
	std::size_t b = 0, e = s.size();
	while (b < e && (s[b] == ' ' || s[b] == '\t')) {
		b++;
	}
	while (e > b && (s[e - 1] == ' ' || s[e - 1] == '\t')) {
		e--;
	}
	return s.substr(b, e - b);
}

// Parse a non-negative decimal (Content-Length) or hex (chunk size) number
// occupying all of `s`. Rejects empty input, signs and overflow.
bool ParseNumber(const std::string &s, int base, std::uint64_t &out) {
	if (s.empty()) {
		return false;
	}
	std::uint64_t v = 0;
	for (char c : s) {
		int d;
		if (c >= '0' && c <= '9') {
			d = c - '0';
		} else if (base == 16 && Lower(c) >= 'a' && Lower(c) <= 'f') {
			d = Lower(c) - 'a' + 10;
		} else {
			return false;
		}
		if (v > (UINT64_MAX - static_cast<std::uint64_t>(d)) / static_cast<std::uint64_t>(base)) {
			return false;
		}
		v = v * static_cast<std::uint64_t>(base) + static_cast<std::uint64_t>(d);
	}
	out = v;
	return true;
}

bool DecodeChunked(const std::string &raw, std::size_t pos, std::string &body) {
	body.clear();
	while (true) {
		auto eol = raw.find("\r\n", pos);
		if (eol == std::string::npos) {
			return false;
		}
		std::string size_line = raw.substr(pos, eol - pos);
		auto semi = size_line.find(';'); // chunk extensions are ignored
		std::uint64_t size = 0;
		if (!ParseNumber(Trim(size_line.substr(0, semi)), 16, size)) {
			return false;
		}
		pos = eol + 2;
		if (size == 0) {
			return true; // trailers, if any, are ignored
		}
		if (size > raw.size() - pos || raw.size() - pos - size < 2 || raw.compare(pos + size, 2, "\r\n") != 0) {
			return false;
		}
		body.append(raw, pos, size);
		pos += size + 2;
	}
}

} // namespace

bool BuildRawHttpRequest(const std::string &method, const std::string &host, int port, const std::string &target,
                         const RawHeaders &headers, const std::string &body, std::string &out) {
	if (!IsVisibleAscii(method) || !IsVisibleAscii(host) || !IsVisibleAscii(target) || target[0] != '/' ||
	    port < 1 || port > 65535) {
		return false;
	}
	const std::string host_header = port == 443 ? host : host + ":" + std::to_string(port);
	std::string req = method + " " + target + " HTTP/1.1\r\nHost: " + host_header + "\r\nConnection: close\r\n";
	for (const auto &h : headers) {
		if (!IsVisibleAscii(h.first) || h.first.find(':') != std::string::npos || !IsSafeHeaderValue(h.second)) {
			return false;
		}
		// The builder owns framing and routing; a second copy from the caller
		// would be request smuggling.
		for (const char *owned : {"Host", "Connection", "Content-Length", "Transfer-Encoding"}) {
			if (EqualsIgnoreCase(h.first, owned)) {
				return false;
			}
		}
		req += h.first + ": " + h.second + "\r\n";
	}
	req += "Content-Length: " + std::to_string(body.size()) + "\r\n\r\n";
	req += body;
	out = std::move(req);
	return true;
}

std::string RawHttpResponse::Header(const std::string &name) const {
	for (const auto &h : headers) {
		if (EqualsIgnoreCase(h.first, name)) {
			return h.second;
		}
	}
	return std::string();
}

// Status line and headers of `raw`, whose head ends at `head_end` (the
// "\r\n\r\n"). Body untouched.
static bool ParseHead(const std::string &raw, std::size_t head_end, RawHttpResponse &out) {
	out = RawHttpResponse();
	// Status line: "HTTP/1.x NNN reason"
	auto line_end = raw.find("\r\n");
	const std::string status_line = raw.substr(0, line_end);
	if (status_line.compare(0, 5, "HTTP/") != 0) {
		return false;
	}
	auto sp = status_line.find(' ');
	std::uint64_t status = 0;
	if (sp == std::string::npos || status_line.size() < sp + 4 ||
	    !ParseNumber(status_line.substr(sp + 1, 3), 10, status) || status < 100 || status > 999) {
		return false;
	}
	out.status = static_cast<int>(status);

	std::size_t pos = line_end + 2;
	while (pos < head_end) {
		auto eol = raw.find("\r\n", pos);
		std::string line = raw.substr(pos, eol - pos);
		auto colon = line.find(':');
		if (colon == std::string::npos || colon == 0) {
			return false;
		}
		out.headers.emplace_back(Trim(line.substr(0, colon)), Trim(line.substr(colon + 1)));
		pos = eol + 2;
	}
	return true;
}

bool ParseRawHttpResponse(const std::string &raw, RawHttpResponse &out) {
	out = RawHttpResponse();
	auto head_end = raw.find("\r\n\r\n");
	if (head_end == std::string::npos || !ParseHead(raw, head_end, out)) {
		return false;
	}

	const std::size_t body_start = head_end + 4;
	// Conflicting framing is refused, never guessed at (a classic smuggling
	// vector): chunked together with Content-Length, or two different lengths.
	std::string cl;
	for (const auto &h : out.headers) {
		if (EqualsIgnoreCase(h.first, "Content-Length")) {
			if (!cl.empty() && cl != h.second) {
				return false;
			}
			cl = h.second;
		}
	}
	const std::string te = out.Header("Transfer-Encoding");
	if (!te.empty()) {
		if (!EqualsIgnoreCase(te, "chunked") || !cl.empty()) {
			return false;
		}
		return DecodeChunked(raw, body_start, out.body);
	}
	if (!cl.empty()) {
		std::uint64_t len = 0;
		if (!ParseNumber(cl, 10, len) || len > raw.size() - body_start) {
			return false;
		}
		out.body = raw.substr(body_start, static_cast<std::size_t>(len));
		return true;
	}
	out.body = raw.substr(body_start);
	return true;
}

bool RawHttpResponseComplete(const std::string &raw) {
	// Called after every read of a download, so it must not re-parse the body:
	// look at the head only, then compare lengths (or, for chunked, check for
	// the terminating chunk before decoding once).
	const auto head_end = raw.find("\r\n\r\n");
	if (head_end == std::string::npos) {
		return false;
	}
	RawHttpResponse head;
	if (!ParseHead(raw, head_end, head)) {
		return false;
	}
	const std::string cl = head.Header("Content-Length");
	if (!cl.empty()) {
		std::uint64_t len = 0;
		return ParseNumber(cl, 10, len) && raw.size() - (head_end + 4) >= len;
	}
	if (!head.Header("Transfer-Encoding").empty()) {
		static const std::string kLastChunk = "0\r\n\r\n";
		if (raw.size() < kLastChunk.size() ||
		    raw.compare(raw.size() - kLastChunk.size(), kLastChunk.size(), kLastChunk) != 0) {
			return false;
		}
		RawHttpResponse full;
		return ParseRawHttpResponse(raw, full);
	}
	return false; // unframed: complete only at EOF
}

bool AcceptRangeReply(int status, const std::string &content_range, std::string &body, std::uint64_t offset,
                      std::uint64_t length) {
	if (status == 206) {
		if (body.size() != length) {
			return false;
		}
		if (!content_range.empty()) {
			// "bytes <start>-<end>/<total>"
			static const std::string kUnit = "bytes ";
			auto dash = content_range.find('-');
			std::uint64_t start = 0;
			if (content_range.compare(0, kUnit.size(), kUnit) != 0 || dash == std::string::npos ||
			    !ParseNumber(content_range.substr(kUnit.size(), dash - kUnit.size()), 10, start) || start != offset) {
				return false;
			}
		}
		return true;
	}
	if (status == 200) {
		if (offset > body.size() || length > body.size() - offset) {
			return false;
		}
		body = body.substr(offset, length);
		return true;
	}
	return false;
}

bool SplitHttpsOrigin(const std::string &origin, std::string &host, int &port) {
	static const std::string kScheme = "https://";
	if (origin.size() <= kScheme.size() || !EqualsIgnoreCase(origin.substr(0, kScheme.size()), kScheme)) {
		return false;
	}
	std::string authority = origin.substr(kScheme.size());
	auto colon = authority.rfind(':');
	if (colon == std::string::npos) {
		host = authority;
		port = 443;
	} else {
		std::uint64_t p = 0;
		if (!ParseNumber(authority.substr(colon + 1), 10, p) || p < 1 || p > 65535) {
			return false;
		}
		host = authority.substr(0, colon);
		port = static_cast<int>(p);
	}
	return IsVisibleAscii(host) && host.find_first_of("/?#@[]") == std::string::npos;
}

} // namespace sharepoint
} // namespace duckdb
