#include "sharepoint_errors.hpp"

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
	case GraphErrorKind::THROTTLED:
		return "throttled";
	case GraphErrorKind::SERVER:
		return "server-error";
	case GraphErrorKind::OTHER:
	default:
		return "error";
	}
}

} // namespace sharepoint
} // namespace duckdb
