#pragma once

#include <string>

namespace duckdb {
namespace sharepoint {

// The extension's own version string, independent of DuckDB's. Pure logic so
// the Catch2 binary can assert on it without linking DuckDB.
std::string SharepointVersion();

} // namespace sharepoint
} // namespace duckdb
