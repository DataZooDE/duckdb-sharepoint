#include "sharepoint_glob.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <unordered_map>

namespace duckdb {
namespace sharepoint {

bool HasGlobMetacharacters(const std::string &segment) {
	return segment.find_first_of("*?[{") != std::string::npos;
}

namespace {

// Ported from gdrive_glob.cpp, unchanged: both extensions must accept the
// same patterns the same way.
// ---------------------------------------------------------------------------
// Character class matching: "[abc]", "[a-z]", "[!abc]"/"[^abc]", with the
// classic glob carve-outs for a literal ']' as the first member and a
// literal '-' at either end of the class.
//
// Returns whether the class starting at pattern[start] (the character right
// after '[') is well-formed; on success `end` is set to the index one past
// the closing ']', and `matched` reports whether `c` is a class member.
// On failure (no closing ']' found) the class is not well-formed and the
// caller falls back to treating '[' as a literal character -- see
// the decision note below.
// ---------------------------------------------------------------------------
bool TryMatchClass(const std::string &pattern, size_t start, char c, size_t &end, bool &matched) {
	size_t i = start;
	const size_t n = pattern.size();
	bool negate = false;
	if (i < n && (pattern[i] == '!' || pattern[i] == '^')) {
		negate = true;
		i++;
	}
	bool found = false;
	bool first = true;
	while (true) {
		if (i >= n) {
			// unterminated class -- no closing ']'.
			return false;
		}
		if (pattern[i] == ']' && !first) {
			// closing bracket.
			end = i + 1;
			matched = negate ? !found : found;
			return true;
		}
		first = false;
		// a range like "a-z", but not when '-' is the last class member
		// (immediately followed by ']') or when it starts the class body.
		if (i + 2 < n && pattern[i + 1] == '-' && pattern[i + 2] != ']') {
			char lo = pattern[i];
			char hi = pattern[i + 2];
			if (c >= lo && c <= hi) {
				found = true;
			}
			i += 3;
		} else {
			if (pattern[i] == c) {
				found = true;
			}
			i++;
		}
	}
}

// ---------------------------------------------------------------------------
// Two-pointer backtracking matcher, the same shape as DuckDB's own
// scalar `Glob` (src/function/scalar/string/like.cpp): O(pattern * name)
// worst case, never exponential, because a failed match only ever rewinds
// to the most recent '*' and advances its start position by one -- it does
// not re-explore already-failed suffixes.
// ---------------------------------------------------------------------------
bool MatchSegmentImpl(const std::string &pattern, const std::string &name) {
	const size_t plen = pattern.size();
	const size_t slen = name.size();
	size_t pidx = 0, sidx = 0;
	size_t star_pidx = std::string::npos, star_sidx = 0;

	while (sidx < slen) {
		bool matched = false;
		size_t next_pidx = pidx;

		if (pidx < plen) {
			char p = pattern[pidx];
			if (p == '*') {
				pidx++;
				while (pidx < plen && pattern[pidx] == '*') {
					pidx++;
				}
				if (pidx == plen) {
					return true; // trailing '*' matches the remainder unconditionally
				}
				star_pidx = pidx;
				star_sidx = sidx;
				continue;
			} else if (p == '?') {
				matched = true; // matches exactly one byte, whatever it is
				next_pidx = pidx + 1;
			} else if (p == '[') {
				size_t class_end;
				bool class_matched;
				if (TryMatchClass(pattern, pidx + 1, name[sidx], class_end, class_matched)) {
					matched = class_matched;
					next_pidx = class_end;
				} else {
					// Decision: an unclosed '[' is not an error. It degrades
					// to matching itself as a literal character. A thrown
					// exception here would abort an entire folder scan
					// because of one oddly-named file; staying total is
					// worth more than being strict.
					matched = (name[sidx] == '[');
					next_pidx = pidx + 1;
				}
			} else {
				matched = (name[sidx] == p);
				next_pidx = pidx + 1;
			}
		}

		if (matched) {
			sidx++;
			pidx = next_pidx;
			continue;
		}

		if (star_pidx == std::string::npos) {
			return false;
		}
		// backtrack: the last '*' consumes one more character and we retry.
		star_sidx++;
		sidx = star_sidx;
		pidx = star_pidx;
	}

	// consume any trailing '*'s -- they match the empty remainder.
	while (pidx < plen && pattern[pidx] == '*') {
		pidx++;
	}
	return pidx == plen;
}


constexpr size_t kMaxBracePatternLength = 4096;
constexpr size_t kMaxBraceExpansions = 1024;
constexpr int kMaxBraceDepth = 64;

//! Returns false the instant `budget` would be exceeded or `depth` runs past
//! kMaxBraceDepth. On false, `out` is left in an unspecified partial state --
//! callers must propagate the failure rather than use it.
//! Returns false the instant `budget` would be exceeded or `depth` runs past
//! kMaxBraceDepth. On false, `out` is left in an unspecified partial state --
//! callers must propagate the failure rather than use it.
bool ExpandBracesInto(const std::string &pattern, int depth, size_t &budget, std::vector<std::string> &out) {
	if (depth > kMaxBraceDepth) {
		return false;
	}

	// Find the first top-level (unnested) balanced brace group.
	int brace_depth = 0;
	size_t brace_start = std::string::npos;
	size_t brace_end = std::string::npos;
	for (size_t i = 0; i < pattern.size(); i++) {
		if (pattern[i] == '{') {
			if (brace_depth == 0) {
				brace_start = i;
			}
			brace_depth++;
		} else if (pattern[i] == '}') {
			if (brace_depth > 0) {
				brace_depth--;
				if (brace_depth == 0 && brace_start != std::string::npos) {
					brace_end = i;
					break;
				}
			}
		}
	}

	if (brace_start == std::string::npos || brace_end == std::string::npos) {
		// No top-level brace group found (including an unmatched '{' or a
		// stray '}'): left as a literal, no expansion performed.
		if (budget == 0) {
			return false;
		}
		budget--;
		out.push_back(pattern);
		return true;
	}

	std::string prefix = pattern.substr(0, brace_start);
	std::string inner = pattern.substr(brace_start + 1, brace_end - brace_start - 1);
	std::string suffix = pattern.substr(brace_end + 1);

	// Split `inner` on top-level commas (commas inside a nested brace group
	// do not separate alternatives at this level).
	std::vector<std::string> alternatives;
	int inner_depth = 0;
	size_t start = 0;
	for (size_t i = 0; i < inner.size(); i++) {
		if (inner[i] == '{') {
			inner_depth++;
		} else if (inner[i] == '}') {
			inner_depth--;
		} else if (inner[i] == ',' && inner_depth == 0) {
			alternatives.push_back(inner.substr(start, i - start));
			start = i + 1;
		}
	}
	alternatives.push_back(inner.substr(start));

	std::vector<std::string> suffix_expansions;
	if (!ExpandBracesInto(suffix, depth + 1, budget, suffix_expansions)) {
		return false;
	}

	for (auto &alt : alternatives) {
		std::vector<std::string> alt_expanded;
		if (!ExpandBracesInto(alt, depth + 1, budget, alt_expanded)) {
			return false;
		}
		for (auto &alt_str : alt_expanded) {
			for (auto &suffix_str : suffix_expansions) {
				if (budget == 0) {
					return false;
				}
				budget--;
				out.push_back(prefix + alt_str + suffix_str);
			}
		}
	}
	return true;
}

} // namespace

bool MatchSegment(const std::string &pattern, const std::string &name) {
	return MatchSegmentImpl(pattern, name);
}

std::vector<std::string> ExpandBraces(const std::string &pattern) {
	if (pattern.size() > kMaxBracePatternLength) {
		// See the overflow-signalling convention above: empty means
		// rejected, not "matches nothing".
		return {};
	}
	size_t budget = kMaxBraceExpansions;
	std::vector<std::string> result;
	if (!ExpandBracesInto(pattern, 0, budget, result)) {
		return {};
	}
	return result;
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
			if (!c.is_folder && MatchSegment(seg, c.name)) {
				out.push_back(Join(folder, c.name));
			}
		} else if (c.is_folder && MatchSegment(seg, c.name)) {
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
