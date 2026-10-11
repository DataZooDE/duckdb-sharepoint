#pragma once

#include <string>

namespace duckdb {
namespace sharepoint {

// The extension's own version string, independent of DuckDB's. Pure logic so
// the Catch2 binary can assert on it without linking DuckDB.
std::string SharepointVersion();

//! The same string as a C string with static storage, for the load banner
//! (datazoo::BannerInfo holds `const char *`).
const char *SharepointVersionCString();

} // namespace sharepoint
} // namespace duckdb
