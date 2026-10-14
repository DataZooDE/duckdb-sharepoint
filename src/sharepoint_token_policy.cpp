#include "sharepoint_token_policy.hpp"

#include <limits>

namespace duckdb {
namespace sharepoint {

namespace {
int64_t SaturatingAdd(int64_t a, int64_t b) {
	return a > std::numeric_limits<int64_t>::max() - b ? std::numeric_limits<int64_t>::max() : a + b;
}
} // namespace

bool NormalizeMaxAgeSeconds(int64_t requested, int64_t &effective) {
	if (requested < 0) {
		return false;
	}
	effective = (requested > 0 && requested <= kTokenRefreshSlackSeconds) ? kTokenRefreshSlackSeconds + 1 : requested;
	return true;
}

bool TokenIsFresh(int64_t minted_at, int64_t expires_at, int64_t now, int64_t max_age_seconds) {
	const int64_t horizon = SaturatingAdd(now, kTokenRefreshSlackSeconds);
	if (expires_at <= horizon) {
		return false;
	}
	// Age is measured from minting, so a token cached before the cap was set
	// counts too.
	return max_age_seconds <= 0 || SaturatingAdd(minted_at, max_age_seconds) > horizon;
}

} // namespace sharepoint
} // namespace duckdb
