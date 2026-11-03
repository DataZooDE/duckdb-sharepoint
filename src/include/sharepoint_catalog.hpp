#pragma once

namespace duckdb {

class ExtensionLoader;

namespace sharepoint {

//! Registers the `sharepoint` storage extension (#60):
//!   ATTACH 'sharepoint://sites/Sales' AS sales (TYPE sharepoint);
//! makes the site's lists the tables of catalog `sales` (schema `main`). See
//! docs/hld.md §17.
void RegisterSharepointCatalog(ExtensionLoader &loader);

} // namespace sharepoint
} // namespace duckdb
