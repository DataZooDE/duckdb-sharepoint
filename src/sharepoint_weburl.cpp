#include "sharepoint_weburl.hpp"

#include "sharepoint_assertion.hpp"

#include <cctype>
#include <cstddef>

namespace duckdb {
namespace sharepoint {

namespace {

constexpr const char *kHttps = "https://";

std::string Lower(std::string s) {
	for (auto &c : s) {
		c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
	}
	return s;
}

bool EndsWith(const std::string &s, const std::string &suffix) {
	return s.size() >= suffix.size() && s.compare(s.size() - suffix.size(), suffix.size(), suffix) == 0;
}

bool IsSharepointHost(const std::string &lower_host) {
	for (const char *suffix : {".sharepoint.com", ".sharepoint.us", ".sharepoint-mil.us", ".sharepoint.cn"}) {
		if (EndsWith(lower_host, suffix) && lower_host.size() > std::string(suffix).size()) {
			return true;
		}
	}
	return false;
}

// "a//b/" -> {"a", "b"}, each percent-decoded.
std::vector<std::string> DecodedSegments(const std::string &path) {
	std::vector<std::string> out;
	std::size_t i = 0;
	while (i < path.size()) {
		while (i < path.size() && path[i] == '/') {
			i++;
		}
		const std::size_t start = i;
		while (i < path.size() && path[i] != '/') {
			i++;
		}
		if (i > start) {
			out.push_back(PercentDecodeUrl(path.substr(start, i - start)));
		}
	}
	return out;
}

// ":x:", ":w:", ":u:", ":f:" ... -- the item-kind marker of a sharing link.
bool IsLinkMarker(const std::string &segment) {
	if (segment.size() < 3 || segment.front() != ':' || segment.back() != ':') {
		return false;
	}
	for (std::size_t i = 1; i + 1 < segment.size(); i++) {
		if (!std::isalpha(static_cast<unsigned char>(segment[i]))) {
			return false;
		}
	}
	return true;
}

bool EqualsIgnoreCase(const std::string &a, const std::string &b) {
	return Lower(a) == Lower(b);
}

} // namespace

std::string PercentDecodeUrl(const std::string &s) {
	auto hex = [](char c) -> int {
		if (c >= '0' && c <= '9') {
			return c - '0';
		}
		if (c >= 'a' && c <= 'f') {
			return c - 'a' + 10;
		}
		if (c >= 'A' && c <= 'F') {
			return c - 'A' + 10;
		}
		return -1;
	};
	std::string out;
	out.reserve(s.size());
	for (std::size_t i = 0; i < s.size(); i++) {
		if (s[i] == '%' && i + 2 < s.size() && hex(s[i + 1]) >= 0 && hex(s[i + 2]) >= 0) {
			out += static_cast<char>(hex(s[i + 1]) * 16 + hex(s[i + 2]));
			i += 2;
		} else {
			out += s[i];
		}
	}
	return out;
}

std::string PercentEncodeSegment(const std::string &segment) {
	static const char *digits = "0123456789ABCDEF";
	std::string out;
	for (unsigned char c : segment) {
		if (std::isalnum(c) || c == '-' || c == '.' || c == '_' || c == '~') {
			out += static_cast<char>(c);
		} else {
			out += '%';
			out += digits[c >> 4];
			out += digits[c & 15];
		}
	}
	return out;
}

bool IsSharepointWebUrl(const std::string &path) {
	if (path.size() <= 8 || Lower(path.substr(0, 8)) != kHttps) {
		return false;
	}
	const auto end = path.find_first_of("/?#", 8);
	return IsSharepointHost(Lower(path.substr(8, end == std::string::npos ? std::string::npos : end - 8)));
}

SharepointWebUrlParse ParseSharepointWebUrl(const std::string &url) {
	SharepointWebUrlParse result;
	if (!IsSharepointWebUrl(url)) {
		result.error = "not a SharePoint or OneDrive web address (https://<tenant>.sharepoint.com/...)";
		return result;
	}
	const auto host_end = url.find_first_of("/?#", 8);
	result.url.host = Lower(url.substr(8, host_end == std::string::npos ? std::string::npos : host_end - 8));
	std::string path = host_end == std::string::npos ? std::string() : url.substr(host_end);
	const auto query = path.find_first_of("?#");
	std::string query_string;
	if (query != std::string::npos) {
		if (path[query] == '?') {
			query_string = path.substr(query + 1, path.find('#', query) == std::string::npos
			                                          ? std::string::npos
			                                          : path.find('#', query) - query - 1);
		}
		path = path.substr(0, query);
	}
	auto segments = DecodedSegments(path);

	bool sharing = false;
	if (!segments.empty() && IsLinkMarker(segments[0])) {
		if (segments.size() >= 2 && segments[1] == "r") {
			// A redirect link names the real path after "/:x:/r".
			segments.erase(segments.begin(), segments.begin() + 2);
		} else {
			sharing = true;
		}
	}
	for (auto &s : segments) {
		if (EqualsIgnoreCase(s, "_layouts")) {
			sharing = true; // Doc.aspx and friends: only Graph knows the item
		}
	}
	if (sharing) {
		result.url.kind = WebUrlKind::SHARING;
		result.url.share_url = url;
		result.ok = true;
		return result;
	}
	result.url.kind = WebUrlKind::DIRECT;
	std::size_t start = 0;
	while (start <= query_string.size() && !query_string.empty()) {
		auto end = query_string.find('&', start);
		const auto param = query_string.substr(start, end == std::string::npos ? std::string::npos : end - start);
		if (param.compare(0, 8, "version=") == 0) {
			result.url.version = PercentDecodeUrl(param.substr(8));
		}
		if (end == std::string::npos) {
			break;
		}
		start = end + 1;
	}
	std::size_t first = 0;
	if (segments.size() >= 2) {
		const auto managed = Lower(segments[0]);
		if (managed == "sites" || managed == "teams" || managed == "personal") {
			result.url.site_path = segments[0] + "/" + segments[1];
			first = 2;
		}
	}
	result.url.rest.assign(segments.begin() + static_cast<std::ptrdiff_t>(first), segments.end());
	result.ok = true;
	return result;
}

std::string StripUrlQuery(const std::string &url) {
	return url.substr(0, url.find_first_of("?#"));
}

std::string SharesId(const std::string &url) {
	return "u!" + Base64Url(url);
}

int MatchDriveWebUrl(const SharepointWebUrl &url, const std::string &drive_web_url) {
	if (url.kind != WebUrlKind::DIRECT) {
		return -1;
	}
	auto drive = ParseSharepointWebUrl(drive_web_url);
	if (!drive.ok || drive.url.kind != WebUrlKind::DIRECT || drive.url.host != url.host ||
	    !EqualsIgnoreCase(drive.url.site_path, url.site_path) || drive.url.rest.empty() ||
	    drive.url.rest.size() > url.rest.size()) {
		return -1;
	}
	for (std::size_t i = 0; i < drive.url.rest.size(); i++) {
		if (!EqualsIgnoreCase(drive.url.rest[i], url.rest[i])) {
			return -1;
		}
	}
	return static_cast<int>(drive.url.rest.size());
}

} // namespace sharepoint
} // namespace duckdb
