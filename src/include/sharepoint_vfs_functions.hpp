#pragma once

// ---------------------------------------------------------------------------
// Scheme-generic file-mutation SQL functions.
//
// DuckDB's SQL surface can READ a file's bytes from any registered filesystem
// (`read_blob`, `read_text`) and ENUMERATE it (`glob`), but offers no way from
// SQL to WRITE, DELETE or RENAME a file. `duckdb::FileSystem` has always had
// `Write`, `RemoveFile` and `MoveFile` -- they are simply not reachable from
// SQL. These four functions close that gap:
//
//     write_blob(path, blob) -> BIGINT   -- bytes written
//     remove_file(path)      -> BOOLEAN  -- true if it existed and was removed
//     move_file(src, dst)    -> BOOLEAN  -- rename within one filesystem
//     file_size(path)        -> BIGINT   -- byte length, NULL if absent
//
// They are deliberately NOT named `sharepoint_*`: they resolve through
// `FileSystem::GetFileSystem(context)`, the VIRTUAL filesystem, so they
// dispatch on the path's scheme and work for `sharepoint://`, `s3://`,
// `gs://`, `hf://` and plain local paths alike.
//
// They are duplicated, character for character, from the sibling
// duckdb-gdrive's gdrive_vfs_functions.cpp -- the same code the escurel
// LaneStore already drives on gdrive://. That is intentional: a LaneStore
// pointed at sharepoint:// needs the identical write/delete/rename/size
// surface, and one process loads exactly one VFS-providing extension. The one
// caveat: do NOT load this extension AND duckdb-gdrive into the same DuckDB,
// or both will try to register these four names. If a standalone VFS-functions
// extension is ever factored out, both extensions drop this file and depend on
// it instead.
// ---------------------------------------------------------------------------

namespace duckdb {

class ExtensionLoader;

namespace sharepoint {

//! Registers write_blob/remove_file/move_file/file_size. Call once from
//! sharepoint_extension.cpp's LoadInternal().
void RegisterVfsFunctions(ExtensionLoader &loader);

} // namespace sharepoint
} // namespace duckdb
