#include <catch2/catch_test_macros.hpp>

#include "sharepoint_token_policy.hpp"

#include <cstdint>
#include <limits>

using duckdb::sharepoint::kTokenRefreshSlackSeconds;
using duckdb::sharepoint::NormalizeMaxAgeSeconds;
using duckdb::sharepoint::TokenIsFresh;

TEST_CASE("the debug token cap refuses negatives and clamps caps inside the refresh slack", "[token]") {
	int64_t effective = -7;
	REQUIRE_FALSE(NormalizeMaxAgeSeconds(-1, effective));
	REQUIRE(NormalizeMaxAgeSeconds(0, effective));
	REQUIRE(effective == 0);
	REQUIRE(NormalizeMaxAgeSeconds(1, effective));
	REQUIRE(effective == kTokenRefreshSlackSeconds + 1);
	REQUIRE(NormalizeMaxAgeSeconds(kTokenRefreshSlackSeconds, effective));
	REQUIRE(effective == kTokenRefreshSlackSeconds + 1);
	REQUIRE(NormalizeMaxAgeSeconds(kTokenRefreshSlackSeconds + 1, effective));
	REQUIRE(effective == kTokenRefreshSlackSeconds + 1);
	REQUIRE(NormalizeMaxAgeSeconds(3600, effective));
	REQUIRE(effective == 3600);
}

TEST_CASE("a clamped cap still yields a usable token", "[token]") {
	int64_t cap = 0;
	REQUIRE(NormalizeMaxAgeSeconds(5, cap));
	// Freshly minted, valid for an hour: fresh under the clamped cap.
	REQUIRE(TokenIsFresh(1000, 1000 + 3600, 1000, cap));
	// Once the cap is used up (minus the slack) it is stale.
	REQUIRE_FALSE(TokenIsFresh(1000, 1000 + 3600, 1000 + cap, cap));
}

TEST_CASE("token freshness never overflows", "[token]") {
	const int64_t max = std::numeric_limits<int64_t>::max();
	REQUIRE(TokenIsFresh(1000, max, 1000, max));
	REQUIRE(TokenIsFresh(1000, max, 1000, 0));
	// expiry inside the slack: stale
	REQUIRE_FALSE(TokenIsFresh(1000, 1000 + kTokenRefreshSlackSeconds, 1000, 0));
}
