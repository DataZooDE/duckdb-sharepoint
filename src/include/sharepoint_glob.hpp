#pragma once

#include <functional>
#include <string>
#include <vector>

namespace duckdb {
namespace sharepoint {

// ---------------------------------------------------------------------------
// Pure glob matching, the same syntax and matcher as the sibling gdrive:
//   *        any run of characters within one path segment (never '/')
//   ?        exactly one byte within one segment
//   [abc]    character class, with [a-z] ranges and [!abc] / [^abc] negation
//   **       any number of segments, including zero (GlobFiles below)
//   {a,b}    alternation (ExpandBraces, applied to the whole pattern first)
// No I/O, so the Catch2 binary exercises it directly (test/cpp/test_glob.cpp).
// ---------------------------------------------------------------------------

//! True iff the single segment `name` matches `pattern` (whole string,
//! case-sensitive: SharePoint names are case-preserving). An unclosed '['
//! matches itself literally rather than failing a whole folder scan.
bool MatchSegment(const std::string &pattern, const std::string &name);

//! True iff the pattern contains any metacharacter (* ? [ {) -- i.e. it is not
//! a literal path, which is the difference between one stat and a listing.
bool HasGlobMetacharacters(const std::string &segment);

//! Expand {a,b}{c,d} alternations into the concrete patterns they denote; a
//! pattern without braces yields itself. Bounded (pattern length, total
//! expansions, depth) because the cartesian product is otherwise exponential.
//!
//! CALLER OBLIGATION: an EMPTY result means "pattern rejected" (over a cap),
//! NOT "zero matches" -- a normal expansion always has at least one element.
//! FileSystem::Glob turns it into an error naming the pattern.
std::vector<std::string> ExpandBraces(const std::string &pattern);

// ---------------------------------------------------------------------------
// Recursive glob across folders.
//
// The single-folder matcher above answers "which names in ONE folder match a
// segment". Real patterns span folders and may recurse:
//   sharepoint://Finance/**/*.parquet   -- every *.parquet at any depth
//   sharepoint://data/*/part-*.csv       -- one folder level down
// Walking the tree is I/O, so it is injected as a lister callback -- keeping the
// matcher pure and letting the Catch2 binary drive it over a synthetic tree
// with no Graph calls. The impure FileSystem::Glob() supplies a lister backed
// by GraphClient::ListChildren.
//
// Segment semantics:
//   *  / ?   match WITHIN a single segment (never crossing '/')
//   **       matches zero or more whole folder levels
// Only FILES are emitted (a trailing `**` yields every file beneath the folder,
// never the folders themselves), mirroring what read_parquet/read_csv want.
// ---------------------------------------------------------------------------

//! One immediate child of a folder, as seen by the matcher.
struct GlobEntry {
	std::string name;
	bool is_folder = false;
};

//! Lists the immediate children of a folder given by its root-relative path
//! ("" == the drive root). Injected so the matcher does no I/O itself.
using GlobLister = std::function<std::vector<GlobEntry>(const std::string &folder)>;

//! Expand a segmented glob `segments` into the root-relative paths of matching
//! files, walking folders through `list`. Each distinct folder is listed at
//! most once per call. Returns paths like "Finance/2026/x.parquet" (no scheme,
//! no leading slash); the caller prepends the scheme.
std::vector<std::string> GlobFiles(const std::vector<std::string> &segments, const GlobLister &list);

} // namespace sharepoint
} // namespace duckdb
