#pragma once

#include <string>

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

} // namespace sharepoint
} // namespace duckdb
