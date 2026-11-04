// sharepoint_permissions() (#98): Graph's permission objects as rows. The
// shapes follow the Microsoft Graph permission resource; the live answer is
// the e2e test.
#include "sharepoint_permissions.hpp"

#include <catch2/catch_test_macros.hpp>

#include <string>
#include <vector>

using namespace duckdb::sharepoint;

namespace {

std::vector<PermissionRow> Parse(const std::string &body, std::string *next = nullptr) {
	std::vector<PermissionRow> rows;
	std::string next_link, error;
	REQUIRE(ParsePermissions(body, rows, next_link, error));
	if (next) {
		*next = next_link;
	}
	return rows;
}

} // namespace

TEST_CASE("ParsePermissions: a direct grant, an inherited one, and links", "[permissions]") {
	const std::string body = R"({"value": [
	  {"id": "p1", "roles": ["owner"],
	   "grantedToV2": {"user": {"displayName": "Anna", "email": "anna@contoso.com", "id": "u1"},
	                   "siteUser": {"displayName": "Anna", "loginName": "i:0#.f|membership|anna@contoso.com"}}},
	  {"id": "p2", "roles": ["write"], "inheritedFrom": {"path": "/drives/b!x/root:", "id": "r"},
	   "grantedToV2": {"siteGroup": {"displayName": "Sales Members", "id": "5"}}},
	  {"id": "p3", "roles": ["read"], "expirationDateTime": "2026-12-31T00:00:00Z",
	   "link": {"type": "view", "scope": "organization", "webUrl": "https://contoso.sharepoint.com/:x:/s/Sales/E1"}},
	  {"id": "p4", "roles": ["write"],
	   "link": {"type": "edit", "scope": "anonymous", "webUrl": "https://contoso.sharepoint.com/:x:/g/E2"},
	   "grantedToIdentitiesV2": [{"user": {"displayName": "Bob", "email": "bob@fabrikam.com"}},
	                             {"application": {"displayName": "ci-app", "id": "a1"}}]}
	], "@odata.nextLink": "https://graph.microsoft.com/v1.0/drives/b!x/items/1/permissions?$skiptoken=2"})";
	std::string next;
	auto rows = Parse(body, &next);
	REQUIRE(next.find("$skiptoken=2") != std::string::npos);
	REQUIRE(rows.size() == 5); // p4 has two grantees

	REQUIRE(rows[0].granted_by == "direct");
	REQUIRE(rows[0].grantee_type == "user"); // user wins over siteUser
	REQUIRE(rows[0].grantee == "Anna");
	REQUIRE(rows[0].grantee_email == "anna@contoso.com");
	REQUIRE(rows[0].roles == std::vector<std::string> {"owner"});

	REQUIRE(rows[1].granted_by == "inherited");
	REQUIRE(rows[1].grantee_type == "site_group");
	REQUIRE(rows[1].inherited_from == "/drives/b!x/root:");

	REQUIRE(rows[2].granted_by == "link");
	REQUIRE(rows[2].grantee_type.empty()); // an organization link nobody has used
	REQUIRE(rows[2].link_scope == "organization");
	REQUIRE(rows[2].link_type == "view");
	REQUIRE(rows[2].expires == "2026-12-31T00:00:00Z");

	REQUIRE(rows[3].link_scope == "anonymous");
	REQUIRE(rows[3].grantee == "Bob");
	REQUIRE(rows[4].grantee_type == "application");
	REQUIRE(rows[4].permission_id == "p4");
}

TEST_CASE("ParsePermissions: not a permissions page", "[permissions]") {
	std::vector<PermissionRow> rows;
	std::string next, error;
	REQUIRE_FALSE(ParsePermissions("[]", rows, next, error));
	REQUIRE_FALSE(ParsePermissions("{\"error\": {}}", rows, next, error));
	REQUIRE(error == "no 'value' array");
	REQUIRE(ParsePermissions("{\"value\": []}", rows, next, error));
	REQUIRE(rows.empty());
}
