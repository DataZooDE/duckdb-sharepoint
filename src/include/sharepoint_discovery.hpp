#pragma once

namespace duckdb {

class ExtensionLoader;

namespace sharepoint {

//! Registers sharepoint_sites(search := ...) and sharepoint_drives([site]):
//! the sites and document libraries the credentials can see, with the
//! addresses and ids a secret or a path needs (#95).
void RegisterDiscoveryFunctions(ExtensionLoader &loader);

} // namespace sharepoint
} // namespace duckdb
