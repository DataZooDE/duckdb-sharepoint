#include "sharepoint_errors.hpp"

#include <algorithm>
#include <cstddef>

namespace duckdb {
namespace sharepoint {

GraphErrorKind ClassifyGraphError(int http_status, const std::string &graph_error_code) {
	// Graph occasionally reports throttling as a 400/403 carrying a specific
	// code rather than a clean 429; trust the code when it is one of these.
	if (graph_error_code == "activityLimitReached" || graph_error_code == "quotaLimitReached" ||
	    graph_error_code == "serviceNotAvailable") {
		return GraphErrorKind::THROTTLED;
	}

	if (http_status >= 200 && http_status < 300) {
		return GraphErrorKind::OK;
	}
	switch (http_status) {
	case 401:
		return GraphErrorKind::UNAUTHENTICATED;
	case 403:
		return GraphErrorKind::FORBIDDEN;
	case 404:
		return GraphErrorKind::NOT_FOUND;
	case 409:
		return GraphErrorKind::CONFLICT;
	case 429:
		return GraphErrorKind::THROTTLED;
	case 503:
		// 503 is Graph's other throttling signal; it too carries Retry-After.
		return GraphErrorKind::THROTTLED;
	default:
		break;
	}
	if (http_status == 400) {
		return GraphErrorKind::BAD_REQUEST;
	}
	if (http_status >= 500) {
		return GraphErrorKind::SERVER;
	}
	return GraphErrorKind::OTHER;
}

bool IsRetryable(GraphErrorKind kind) {
	return kind == GraphErrorKind::THROTTLED || kind == GraphErrorKind::SERVER;
}

const char *GraphErrorKindName(GraphErrorKind kind) {
	switch (kind) {
	case GraphErrorKind::OK:
		return "ok";
	case GraphErrorKind::UNAUTHENTICATED:
		return "unauthenticated";
	case GraphErrorKind::FORBIDDEN:
		return "forbidden";
	case GraphErrorKind::NOT_FOUND:
		return "not-found";
	case GraphErrorKind::CONFLICT:
		return "conflict";
	case GraphErrorKind::BAD_REQUEST:
		return "bad-request";
	case GraphErrorKind::THROTTLED:
		return "throttled";
	case GraphErrorKind::SERVER:
		return "server-error";
	case GraphErrorKind::OTHER:
	default:
		return "error";
	}
}

GraphExceptionType ExceptionTypeFor(GraphErrorKind kind) {
	switch (kind) {
	case GraphErrorKind::FORBIDDEN:
		return GraphExceptionType::PERMISSION;
	case GraphErrorKind::BAD_REQUEST:
		return GraphExceptionType::INVALID_INPUT;
	default:
		return GraphExceptionType::IO;
	}
}

std::string FormatGraphError(const std::string &op, int http_status, const std::string &graph_error_code,
                             const std::string &graph_message) {
	const auto kind = ClassifyGraphError(http_status, graph_error_code);
	std::string out = "sharepoint: " + op + " failed: HTTP " + std::to_string(http_status) + " (" +
	                  GraphErrorKindName(kind);
	if (!graph_error_code.empty()) {
		out += ", " + graph_error_code;
	}
	out += ")";
	if (!graph_message.empty()) {
		constexpr std::size_t kMaxMessage = 200;
		std::string msg = graph_message.substr(0, kMaxMessage);
		for (auto &c : msg) {
			if (c == '\n' || c == '\r' || c == '\t') {
				c = ' ';
			}
		}
		if (graph_message.size() > kMaxMessage) {
			msg += "...";
		}
		out += ": " + msg;
	}
	return out;
}

int ParseRetryAfterSeconds(const std::string &header) {
	const auto b = header.find_first_not_of(" \t");
	if (b == std::string::npos) {
		return 0;
	}
	const auto e = header.find_last_not_of(" \t");
	long long v = 0;
	for (std::size_t i = b; i <= e; i++) {
		const char c = header[i];
		if (c < '0' || c > '9') {
			return 0; // an HTTP-date, a sign, junk: not the seconds form
		}
		v = v * 10 + (c - '0');
		if (v > kMaxRetryAfterSeconds) {
			return kMaxRetryAfterSeconds;
		}
	}
	return static_cast<int>(v);
}

long long BackoffDelayMs(int failed_attempt, int retry_after_seconds, double jitter) {
	constexpr long long kInitialMs = 500;
	constexpr long long kMaxMs = 20000;
	long long base = kMaxMs;
	if (failed_attempt >= 1 && failed_attempt <= 16) {
		base = std::min(kMaxMs, kInitialMs << (failed_attempt - 1));
	}
	jitter = std::max(0.75, std::min(1.25, jitter));
	long long delay = static_cast<long long>(static_cast<double>(base) * jitter + 0.5);
	const long long floor_ms = static_cast<long long>(retry_after_seconds) * 1000;
	return std::max(delay, floor_ms);
}

} // namespace sharepoint
} // namespace duckdb
