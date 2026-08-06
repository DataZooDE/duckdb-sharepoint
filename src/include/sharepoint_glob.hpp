#pragma once

#include <functional>
#include <string>
#include <vector>

namespace duckdb {
namespace sharepoint {

// ---------------------------------------------------------------------------
// Pure glob matching for a single path segment. `*` matches any run of
// characters (never crossing '/'), `?` matches exactly one. No I/O, so the
// Catch2 binary exercises it directly.
//
// The FileSystem::Glob() implementation splits a pattern into its longest
// literal parent prefix and a trailing wildcard segment, lists that ONE Graph
// folder's children, and keeps the names this predicate accepts. Recursive
// `**` across folders is a documented follow-up (see docs/hld.md).
// ---------------------------------------------------------------------------

//! True iff `name` matches the wildcard `pattern` (`*` and `?`), matching the
//! whole string. Case-sensitive: SharePoint item names are case-preserving.
bool MatchGlob(const std::string &pattern, const std::string &name);

//! True iff the segment contains a `*` or `?` -- i.e. it is a wildcard rather
//! than a literal name.
bool HasGlobMeta(const std::string &segment);

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
