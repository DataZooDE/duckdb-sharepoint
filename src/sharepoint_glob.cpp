#include "sharepoint_glob.hpp"

namespace duckdb {
namespace sharepoint {

bool HasGlobMeta(const std::string &segment) {
	return segment.find('*') != std::string::npos || segment.find('?') != std::string::npos;
}

// Iterative backtracking matcher -- O(n*m) worst case, no recursion, no
// allocation. `*` never crosses a '/', but since this is only ever handed a
// single segment (never a path), that is automatic.
bool MatchGlob(const std::string &pattern, const std::string &name) {
	std::size_t p = 0, n = 0;
	std::size_t star = std::string::npos;
	std::size_t match = 0;
	while (n < name.size()) {
		if (p < pattern.size() && (pattern[p] == '?' || pattern[p] == name[n])) {
			p++;
			n++;
		} else if (p < pattern.size() && pattern[p] == '*') {
			star = p;
			match = n;
			p++;
		} else if (star != std::string::npos) {
			p = star + 1;
			match++;
			n = match;
		} else {
			return false;
		}
	}
	while (p < pattern.size() && pattern[p] == '*') {
		p++;
	}
	return p == pattern.size();
}

} // namespace sharepoint
} // namespace duckdb
