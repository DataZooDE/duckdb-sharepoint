#pragma once

namespace duckdb {

class ExtensionLoader;

namespace sharepoint {

//! Registers read_outlook_messages(user :=, folder :=, since :=, body :=) (#126).
void RegisterMailFunctions(ExtensionLoader &loader);

} // namespace sharepoint
} // namespace duckdb
