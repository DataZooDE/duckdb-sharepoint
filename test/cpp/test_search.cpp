// search_sharepoint() (#67): the pure parts -- a hit's sharepoint:// path, and
// the regions app-only search may need. The search itself is the e2e test.
#include "sharepoint_search.hpp"

#include <catch2/catch_test_macros.hpp>

#include <string>

using namespace duckdb::sharepoint;

TEST_CASE("SearchHitPath: a hit in the secret's drive gets a root-relative path", "[search]") {
	REQUIRE(SearchHitPath("/drives/b!abc/root:/Finance/2026", "plan.xlsx") == "Finance/2026/plan.xlsx");
	REQUIRE(SearchHitPath("/drive/root:/Finance", "plan.xlsx") == "Finance/plan.xlsx");
	REQUIRE(SearchHitPath("/drives/b!abc/root:", "top.csv") == "top.csv");
	REQUIRE(SearchHitPath("/drives/b!abc/root:/A%20B", "x.csv") == "A B/x.csv"); // Graph percent-encodes
	REQUIRE(SearchHitPath("", "x.csv").empty());                                  // no parent path: id only
	REQUIRE(SearchHitPath("/drives/b!abc/items/01XYZ", "x.csv").empty());
}

TEST_CASE("SearchRegions: the data-location codes tried for app-only search", "[search]") {
	const auto &regions = SearchRegions();
	REQUIRE(regions.size() > 10);
	REQUIRE(regions.front() == "NAM");
	bool eur = false;
	for (auto &r : regions) {
		eur = eur || r == "EUR";
	}
	REQUIRE(eur);
}

TEST_CASE("SearchNeedsRegion recognises Graph's app-only complaint", "[search]") {
	REQUIRE(SearchNeedsRegion(400, "Region is required when request with application permission."));
	REQUIRE(SearchNeedsRegion(400, "SearchRequest Invalid (Region is required when request with application "
	                                "permission)"));
	REQUIRE_FALSE(SearchNeedsRegion(400, "Invalid query string"));
	REQUIRE_FALSE(SearchNeedsRegion(403, "Region is required"));
}

TEST_CASE("SearchRegionIsWrong recognises a region the tenant is not in", "[search]") {
	REQUIRE(SearchRegionIsWrong(400, "Requested region NAM is not a valid region for the tenant"));
	REQUIRE(SearchRegionIsWrong(400, "The region EUR is invalid."));
	REQUIRE_FALSE(SearchRegionIsWrong(400, "Region is required when request with application permission."));
	REQUIRE_FALSE(SearchRegionIsWrong(500, "region"));
}
