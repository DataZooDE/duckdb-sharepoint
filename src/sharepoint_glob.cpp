#include "sharepoint_glob.hpp"

#include <unordered_map>

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

namespace {

// Cache each folder's listing for the lifetime of one GlobFiles() call: a `**`
// expansion visits the "consume zero folders" and "descend" branches of the
// same folder, and this keeps that to a single lister call per folder.
using ListCache = std::unordered_map<std::string, std::vector<GlobEntry>>;

const std::vector<GlobEntry> &ListOnce(const std::string &folder, const GlobLister &list, ListCache &cache) {
	auto it = cache.find(folder);
	if (it != cache.end()) {
		return it->second;
	}
	return cache.emplace(folder, list(folder)).first->second;
}

std::string Join(const std::string &folder, const std::string &name) {
	return folder.empty() ? name : folder + "/" + name;
}

// Match segments[i..] against the contents of `folder`. A folder tree has no
// cycles, so the descent always terminates.
void GlobRecurse(const std::vector<std::string> &segs, std::size_t i, const std::string &folder,
                 const GlobLister &list, ListCache &cache, std::vector<std::string> &out) {
	const auto &children = ListOnce(folder, list, cache);
	const std::string &seg = segs[i];
	const bool is_last = (i + 1 == segs.size());

	if (seg == "**") {
		if (is_last) {
			// A trailing ** is every file at or below this folder.
			for (const auto &c : children) {
				if (c.is_folder) {
					GlobRecurse(segs, i, Join(folder, c.name), list, cache, out); // ** persists downward
				} else {
					out.push_back(Join(folder, c.name));
				}
			}
			return;
		}
		// ** may consume zero folders (match the next segment right here) ...
		GlobRecurse(segs, i + 1, folder, list, cache, out);
		// ... or one or more (descend into each subfolder with ** still active).
		for (const auto &c : children) {
			if (c.is_folder) {
				GlobRecurse(segs, i, Join(folder, c.name), list, cache, out);
			}
		}
		return;
	}

	for (const auto &c : children) {
		if (is_last) {
			if (!c.is_folder && MatchGlob(seg, c.name)) {
				out.push_back(Join(folder, c.name));
			}
		} else if (c.is_folder && MatchGlob(seg, c.name)) {
			GlobRecurse(segs, i + 1, Join(folder, c.name), list, cache, out);
		}
	}
}

} // namespace

std::vector<std::string> GlobFiles(const std::vector<std::string> &segments, const GlobLister &list) {
	std::vector<std::string> out;
	if (segments.empty()) {
		return out;
	}
	ListCache cache;
	GlobRecurse(segments, 0, "", list, cache, out);
	return out;
}

} // namespace sharepoint
} // namespace duckdb
