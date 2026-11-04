#pragma once

#include <string>

namespace duckdb {
namespace sharepoint {

// ---------------------------------------------------------------------------
// sharepoint_changes() (#62), the pure parts. PURE: no duckdb.hpp, no I/O.
// ---------------------------------------------------------------------------

//! An item's path relative to the drive root, from its parentReference.path
//! ("/drive/root:/Finance", percent-encoded) and name; "" without a path.
std::string DeltaItemPath(const std::string &parent_path, const std::string &name);

//! The `token` query parameter of an @odata.deltaLink, as Graph wrote it
//! (still percent-encoded, ready to send back); "" when there is none.
std::string DeltaToken(const std::string &delta_link);

//! True if root-relative `path` is `folder` or lies under it (whole
//! segments; a trailing '/' on `folder` is ignored; "" is the whole drive).
bool PathUnder(const std::string &path, std::string folder);

} // namespace sharepoint
} // namespace duckdb
