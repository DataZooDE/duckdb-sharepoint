#pragma once

namespace duckdb {

class ExtensionLoader;

namespace sharepoint {

//! Registers read_sharepoint_list_attachments(list, site :=, item_id :=) (#97).
void RegisterAttachmentFunctions(ExtensionLoader &loader);

} // namespace sharepoint
} // namespace duckdb
