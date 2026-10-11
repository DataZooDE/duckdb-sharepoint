#include "sharepoint_uri.hpp"

#include <cstddef>

namespace duckdb {
namespace sharepoint {

namespace {

constexpr const char *kIdPrefix = "id:";

// Split on '/', collapsing repeated slashes and dropping a trailing one.
// Rejection of empty/"."/".." segments is done by the caller so the reason
// can be reported precisely.
std::vector<std::string> SplitSegments(const std::string &body) {
	std::vector<std::string> out;
	std::size_t i = 0;
	while (i < body.size()) {
		while (i < body.size() && body[i] == '/') {
			i++;
		}
		std::size_t start = i;
		while (i < body.size() && body[i] != '/') {
			i++;
		}
		if (i > start) {
			out.push_back(body.substr(start, i - start));
		}
	}
	return out;
}

} // namespace

std::string SharepointUri::ToString() const {
	if (kind == SharepointUriKind::FILE_ID) {
		return std::string(SHAREPOINT_SCHEME) + kIdPrefix + item_id;
	}
	return std::string(SHAREPOINT_SCHEME) + RootRelativePath();
}

std::string SharepointUri::RootRelativePath() const {
	std::string out;
	for (std::size_t i = 0; i < segments.size(); i++) {
		if (i) {
			out += '/';
		}
		out += segments[i];
	}
	return out;
}

std::string SharepointUri::ParentPath() const {
	if (segments.empty()) {
		return std::string(SHAREPOINT_SCHEME);
	}
	std::string out(SHAREPOINT_SCHEME);
	for (std::size_t i = 0; i + 1 < segments.size(); i++) {
		if (i) {
			out += '/';
		}
		out += segments[i];
	}
	return out;
}

std::string SharepointUri::FileName() const {
	return segments.empty() ? std::string() : segments.back();
}

bool IsSharepointUri(const std::string &uri) {
	// Cheap prefix test only -- no allocation, no validation.
	const std::string scheme = SHAREPOINT_SCHEME;
	return uri.size() >= scheme.size() && uri.compare(0, scheme.size(), scheme) == 0;
}

SharepointUriParse ParseSharepointUri(const std::string &uri) {
	SharepointUriParse result;
	const std::string scheme = SHAREPOINT_SCHEME;
	if (!IsSharepointUri(uri)) {
		result.error = "not a sharepoint:// URI: '" + uri + "'";
		return result;
	}
	std::string body = uri.substr(scheme.size());

	// id: form -- a single already-resolved driveItem id, no path segments.
	if (body.rfind(kIdPrefix, 0) == 0) {
		std::string id = body.substr(std::string(kIdPrefix).size());
		// A trailing slash is tolerated; anything else after the id is a path,
		// which the id: form cannot carry.
		while (!id.empty() && id.back() == '/') {
			id.pop_back();
		}
		if (id.empty()) {
			result.error = "the sharepoint://id: form needs an item id, e.g. "
			               "sharepoint://id:01ABCDEF";
			return result;
		}
		if (id.find('/') != std::string::npos) {
			result.error = "the sharepoint://id: form addresses a single item and cannot carry "
			               "path segments: '" +
			               uri + "'";
			return result;
		}
		// The id is spliced into /items/<id> URLs (DELETE included), so only
		// Graph's id alphabet passes: anything else (?, #, %, ., ..) could
		// change which URL is requested.
		bool id_ok = id != "." && id != "..";
		for (char c : id) {
			if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '!' ||
			      c == '-' || c == '_' || c == '.')) {
				id_ok = false;
			}
		}
		if (!id_ok) {
			result.error = "the sharepoint://id: form takes a Graph item id (letters, digits, '!', '-', '_', "
			               "'.'): '" +
			               uri + "'";
			return result;
		}
		result.uri.kind = SharepointUriKind::FILE_ID;
		result.uri.item_id = id;
		result.ok = true;
		return result;
	}

	// path form.
	auto segments = SplitSegments(body);
	for (const auto &seg : segments) {
		if (seg == "." || seg == "..") {
			result.error = "sharepoint:// paths may not contain '.' or '..' segments: '" + uri + "'";
			return result;
		}
	}
	result.uri.kind = SharepointUriKind::PATH;
	result.uri.segments = std::move(segments);
	result.ok = true;
	return result;
}

bool IsFileIdFallbackGlobProbe(const std::string &uri) {
	// Exactly "sharepoint://id:<id>/**/*.<ext>": one literal "**" segment,
	// then one "*.<ext>" segment, and nothing else.
	const std::string scheme = SHAREPOINT_SCHEME;
	if (!IsSharepointUri(uri)) {
		return false;
	}
	std::string body = uri.substr(scheme.size());
	if (body.rfind(kIdPrefix, 0) != 0) {
		return false;
	}
	auto star = body.find("/**/");
	if (star == std::string::npos) {
		return false;
	}
	// id must be non-empty and contain no slash of its own.
	std::string id = body.substr(std::string(kIdPrefix).size(), star - std::string(kIdPrefix).size());
	if (id.empty() || id.find('/') != std::string::npos) {
		return false;
	}
	std::string tail = body.substr(star + 4); // past "/**/"
	// tail must be exactly "*.<ext>" with a non-empty extension and no slash.
	if (tail.rfind("*.", 0) != 0) {
		return false;
	}
	std::string ext = tail.substr(2);
	return !ext.empty() && ext.find('/') == std::string::npos && ext.find('*') == std::string::npos;
}

bool IsDriveRoot(const SharepointUri &uri) {
	return uri.kind == SharepointUriKind::PATH && uri.segments.empty();
}

char ForbiddenNameChar(const std::string &name) {
	for (char c : name) {
		switch (c) {
		case '"':
		case '*':
		case ':':
		case '<':
		case '>':
		case '?':
		case '\\':
		case '|':
			return c;
		default:
			break;
		}
	}
	return '\0';
}

bool PathMatchesAnyPrefix(const std::string &path, const std::string &csv_prefixes) {
	if (csv_prefixes.empty()) {
		return false;
	}
	std::size_t i = 0;
	while (i < csv_prefixes.size()) {
		std::size_t comma = csv_prefixes.find(',', i);
		std::string entry = csv_prefixes.substr(i, comma == std::string::npos ? std::string::npos : comma - i);
		i = (comma == std::string::npos) ? csv_prefixes.size() : comma + 1;

		// Trim whitespace and a trailing slash.
		std::size_t b = entry.find_first_not_of(" \t");
		std::size_t e = entry.find_last_not_of(" \t");
		if (b == std::string::npos) {
			continue;
		}
		entry = entry.substr(b, e - b + 1);
		while (!entry.empty() && entry.back() == '/') {
			entry.pop_back();
		}
		if (entry.empty()) {
			continue;
		}
		// Whole-segment match: equal, or prefix followed by '/'.
		if (path == entry) {
			return true;
		}
		if (path.size() > entry.size() && path.compare(0, entry.size(), entry) == 0 &&
		    path[entry.size()] == '/') {
			return true;
		}
	}
	return false;
}

} // namespace sharepoint
} // namespace duckdb
