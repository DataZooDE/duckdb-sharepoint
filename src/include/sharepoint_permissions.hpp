#pragma once

#include <string>
#include <vector>

namespace duckdb {
namespace sharepoint {

// ---------------------------------------------------------------------------
// sharepoint_permissions() (#98): who can reach an item. PURE (picojson, no
// duckdb.hpp): a page of GET /drives/{d}/items/{id}/permissions turned into
// rows -- one per grantee of each permission, or one with no grantee (an
// organization link nobody has used yet).
// ---------------------------------------------------------------------------

struct PermissionRow {
	std::string permission_id;
	std::vector<std::string> roles; // "read", "write", "owner", ...
	//! "link" (a sharing link), "inherited" (from a parent: inheritedFrom
	//! set), or "direct".
	std::string granted_by;
	//! "user", "group", "application", "site_group", "site_user", or "" for
	//! a link without grantees.
	std::string grantee_type;
	std::string grantee;
	std::string grantee_email;
	std::string link_type;  // "view", "edit", ...
	std::string link_scope; // "anonymous", "organization", "users", ...
	std::string link_url;
	std::string inherited_from; // the parent's path, as Graph gives it
	std::string expires;        // RFC 3339, or ""
};

//! The rows of one permissions page. False, with `error`, for a body that is
//! not a permissions collection. `next_link` gets @odata.nextLink, if any.
bool ParsePermissions(const std::string &body, std::vector<PermissionRow> &out, std::string &next_link,
                      std::string &error);

} // namespace sharepoint
} // namespace duckdb
