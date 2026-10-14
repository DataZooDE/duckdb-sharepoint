#pragma once

// Pure token-freshness rules, shared by the token source and Catch2. Times are
// unix seconds; no clock, no I/O.

#include <cstdint>

namespace duckdb {
namespace sharepoint {

//! A token is re-minted this long before Entra ID's expiry.
constexpr int64_t kTokenRefreshSlackSeconds = 60;

//! Validate the sharepoint_debug_token_max_age_seconds test hook. False for a
//! negative value. A cap inside the refresh slack would make every token stale
//! the moment it is minted (a token request per Graph call), so it is clamped
//! to just past the slack; 0 means "no cap".
bool NormalizeMaxAgeSeconds(int64_t requested, int64_t &effective);

//! Valid for at least another kTokenRefreshSlackSeconds -- by its expiry and,
//! under a cap, by age since minting. Saturating: no input overflows.
bool TokenIsFresh(int64_t minted_at, int64_t expires_at, int64_t now, int64_t max_age_seconds);

} // namespace sharepoint
} // namespace duckdb
