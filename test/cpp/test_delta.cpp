// sharepoint_changes() (#62): the pure parts -- a delta item's path, the
// token in a deltaLink, and whether a path is under the folder asked about.
#include "sharepoint_delta.hpp"

#include <catch2/catch_test_macros.hpp>

#include <string>

using namespace duckdb::sharepoint;

TEST_CASE("DeltaItemPath: root-relative from parentReference.path", "[delta]") {
	REQUIRE(DeltaItemPath("/drive/root:", "a.csv") == "a.csv");
	REQUIRE(DeltaItemPath("/drive/root:/Finance/2026", "a.csv") == "Finance/2026/a.csv");
	REQUIRE(DeltaItemPath("/drives/b!x/root:/A%20B", "c.csv") == "A B/c.csv");
	REQUIRE(DeltaItemPath("", "a.csv").empty()); // a deleted item often has no path
	REQUIRE(DeltaItemPath("/drive/root:", "").empty());
}

TEST_CASE("DeltaToken: the token query parameter of a deltaLink", "[delta]") {
	REQUIRE(DeltaToken("https://graph.microsoft.com/v1.0/drives/b!x/root/delta?token=abc123") == "abc123");
	REQUIRE(DeltaToken("https://graph.microsoft.com/v1.0/drives/b!x/root/delta?$select=id&token=a%3Db") == "a%3Db");
	REQUIRE(DeltaToken("https://graph.microsoft.com/v1.0/drives/b!x/root/delta?token=t&$top=5") == "t");
	REQUIRE(DeltaToken("https://graph.microsoft.com/v1.0/drives/b!x/root/delta").empty());
}

TEST_CASE("PathUnder: whole segments, the folder itself included", "[delta]") {
	REQUIRE(PathUnder("Finance/a.csv", "Finance"));
	REQUIRE(PathUnder("Finance/2026/a.csv", "Finance/"));
	REQUIRE(PathUnder("Finance", "Finance"));
	REQUIRE_FALSE(PathUnder("Financials/a.csv", "Finance"));
	REQUIRE(PathUnder("anything/at/all", ""));
}
