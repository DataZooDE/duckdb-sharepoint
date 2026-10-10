#include "sharepoint_upload.hpp"

#include <cstdint>
#include <cstdlib>

namespace duckdb {
namespace sharepoint {

std::uint64_t NormalizeUploadChunkSize(std::uint64_t requested) {
	std::uint64_t rounded = (requested / kGraphUploadChunkMultiple) * kGraphUploadChunkMultiple;
	return rounded == 0 ? kGraphUploadChunkMultiple : rounded;
}

std::string UploadContentRange(std::uint64_t start, std::uint64_t length, std::uint64_t total) {
	std::uint64_t end = start + length - 1;
	return "bytes " + std::to_string(start) + "-" + std::to_string(end) + "/" + std::to_string(total);
}

bool SplitAbsoluteUrl(const std::string &url, std::string &origin, std::string &path_and_query) {
	auto scheme = url.find("://");
	if (scheme == std::string::npos) {
		return false;
	}
	auto slash = url.find('/', scheme + 3);
	if (slash == std::string::npos) {
		origin = url;
		path_and_query = "/";
	} else {
		origin = url.substr(0, slash);
		path_and_query = url.substr(slash);
	}
	return true;
}

bool IsHttpsUrl(const std::string &url) {
	static const char kScheme[] = "https://";
	const std::size_t n = sizeof(kScheme) - 1;
	if (url.size() <= n) {
		return false;
	}
	for (std::size_t i = 0; i < n; i++) {
		char c = url[i];
		if (c >= 'A' && c <= 'Z') {
			c = static_cast<char>(c - 'A' + 'a');
		}
		if (c != kScheme[i]) {
			return false;
		}
	}
	return true;
}

bool SliceRangeFromFullBody(std::string &body, std::uint64_t offset, std::uint64_t length) {
	if (offset > body.size() || length > body.size() - offset) {
		return false;
	}
	body = body.substr(offset, length);
	return true;
}

bool ParseRangeStart(const std::string &range, std::uint64_t &start) {
	std::size_t i = 0;
	while (i < range.size() && range[i] >= '0' && range[i] <= '9') {
		i++;
	}
	if (i == 0) {
		return false;
	}
	start = std::strtoull(range.substr(0, i).c_str(), nullptr, 10);
	return true;
}

} // namespace sharepoint
} // namespace duckdb
