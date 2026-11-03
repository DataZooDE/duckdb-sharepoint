#pragma once

#include <string>
#include <vector>

namespace duckdb {
namespace sharepoint {

// ---------------------------------------------------------------------------
// search_sharepoint() (#67), the pure parts. PURE: no duckdb.hpp, no I/O.
// ---------------------------------------------------------------------------

//! A search hit's path relative to its drive's root, from its
//! parentReference.path ("/drives/{id}/root:/Finance" or "/drive/root:") and
//! name; "" when there is no root-relative parent path (then: id only).
std::string SearchHitPath(const std::string &parent_path, const std::string &name);

//! Microsoft Search's data-location codes. App-only search must name the
//! tenant's; without a `region :=`, they are tried in this order (NAM, EUR,
//! APC first) and the one that works is remembered.
const std::vector<std::string> &SearchRegions();

//! A 400 saying an app-only search needs a region.
bool SearchNeedsRegion(int status, const std::string &message);

//! A 400 saying the region given is not the tenant's.
bool SearchRegionIsWrong(int status, const std::string &message);

} // namespace sharepoint
} // namespace duckdb
