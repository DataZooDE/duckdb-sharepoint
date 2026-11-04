// sharepoint_changes() (#62), the pure parts: see sharepoint_delta.hpp.
#include "sharepoint_delta.hpp"

#include <cctype>

namespace duckdb {
namespace sharepoint {

namespace {

int Hex(char c) {
	if (c >= '0' && c <= '9') {
		return c - '0';
	}
	c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
	return c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1;
}

std::string PercentDecode(const std::string &s) {
	std::string out;
	for (size_t i = 0; i < s.size(); i++) {
		if (s[i] == '%' && i + 2 < s.size() && Hex(s[i + 1]) >= 0 && Hex(s[i + 2]) >= 0) {
			out += static_cast<char>(Hex(s[i + 1]) * 16 + Hex(s[i + 2]));
			i += 2;
		} else {
			out += s[i];
		}
	}
	return out;
}

} // namespace

std::string DeltaItemPath(const std::string &parent_path, const std::string &name) {
	const auto root = parent_path.find("/root:");
	if (root == std::string::npos || name.empty()) {
		return "";
	}
	std::string folder = PercentDecode(parent_path.substr(root + 6));
	while (!folder.empty() && folder.front() == '/') {
		folder.erase(0, 1);
	}
	return folder.empty() ? name : folder + "/" + name;
}

std::string DeltaToken(const std::string &link) {
	const auto query = link.find('?');
	if (query == std::string::npos) {
		return "";
	}
	size_t at = query + 1;
	while (at < link.size()) {
		const auto end = link.find('&', at);
		const auto param = link.substr(at, end == std::string::npos ? std::string::npos : end - at);
		if (param.compare(0, 6, "token=") == 0) {
			return param.substr(6);
		}
		if (end == std::string::npos) {
			break;
		}
		at = end + 1;
	}
	return "";
}

bool PathUnder(const std::string &path, std::string folder) {
	while (!folder.empty() && folder.back() == '/') {
		folder.pop_back();
	}
	if (folder.empty()) {
		return true;
	}
	return path == folder || (path.size() > folder.size() && path.compare(0, folder.size(), folder) == 0 &&
	                          path[folder.size()] == '/');
}

} // namespace sharepoint
} // namespace duckdb
