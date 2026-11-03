// search_sharepoint() (#67), the pure parts: see sharepoint_search.hpp.
#include "sharepoint_search.hpp"

#include <cctype>

namespace duckdb {
namespace sharepoint {

namespace {

std::string Lower(std::string s) {
	for (auto &c : s) {
		c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
	}
	return s;
}

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

std::string SearchHitPath(const std::string &parent_path, const std::string &name) {
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

const std::vector<std::string> &SearchRegions() {
	static const std::vector<std::string> regions = {"NAM", "EUR", "APC", "GBR", "DEU", "FRA", "CHE", "NOR",
	                                                 "CAN", "AUS", "JPN", "IND", "KOR", "ZAF", "ARE", "BRA",
	                                                 "ITA", "POL", "ESP", "SWE", "QAT", "ISR", "MEX", "TWN",
	                                                 "NZL", "LAM"};
	return regions;
}

bool SearchNeedsRegion(int status, const std::string &message) {
	const auto m = Lower(message);
	return status == 400 && m.find("region is required") != std::string::npos;
}

bool SearchRegionIsWrong(int status, const std::string &message) {
	return status == 400 && Lower(message).find("region") != std::string::npos && !SearchNeedsRegion(status, message);
}

} // namespace sharepoint
} // namespace duckdb
