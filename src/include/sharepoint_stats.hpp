#pragma once

namespace duckdb {

class ExtensionLoader;

namespace sharepoint {

//! Registers sharepoint_stats() -- one row per counter (metric VARCHAR,
//! value BIGINT) -- and sharepoint_reset_stats(). See sharepoint_counters.hpp.
void RegisterSharepointStats(ExtensionLoader &loader);

} // namespace sharepoint
} // namespace duckdb
