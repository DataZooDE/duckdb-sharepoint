#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace duckdb {
namespace sharepoint {

// ---------------------------------------------------------------------------
// SharePoint list logic behind read_sharepoint_list(). PURE: no duckdb.hpp,
// no I/O, so the Catch2 binary compiles it directly.
//
// Graph describes a list's columns (GET /sites/{s}/lists/{l}/columns) as
// columnDefinitions -- an internal `name` (the key in each item's `fields`),
// a `displayName`, `hidden` / `readOnly` flags and one facet object saying
// the kind (text, number, dateTime, choice, lookup, ...). This file plans the
// result columns from them. See docs/hld.md §10.
// ---------------------------------------------------------------------------

//! A column's kind: the facet present on its columnDefinition.
enum class ListFacet {
	TEXT,
	NUMBER,
	CURRENCY,
	BOOLEAN,
	DATETIME,
	CHOICE,
	LOOKUP,
	PERSON,
	HYPERLINK,
	CALCULATED,
	OTHER
};

struct ListColumnDef {
	std::string name;         // internal name: the key in an item's fields
	std::string display_name; // what users see, and the result column's name
	ListFacet facet = ListFacet::OTHER;
	//! For CALCULATED: the facet of its output type (TEXT, NUMBER, BOOLEAN, DATETIME).
	ListFacet calculated_output = ListFacet::TEXT;
	bool hidden = false;
	bool read_only = false;
};

//! The DuckDB type a result column becomes.
enum class ListColumnType { VARCHAR, VARCHAR_LIST, DOUBLE, DECIMAL, BOOLEAN, TIMESTAMP, BIGINT };

struct ListOutputColumn {
	std::string name;  // result column name (unique, case-insensitively)
	std::string field; // key in the item's fields
	ListColumnType type = ListColumnType::VARCHAR;
	//! CHOICE: becomes VARCHAR_LIST if any item holds several values.
	bool may_be_multi = false;
};

//! The result columns for a list:
//!  - `id` (BIGINT) first, `created` / `modified` (TIMESTAMP, UTC) last --
//!    the system columns kept;
//!  - every other column that is neither hidden nor read-only, in Graph's
//!    order, named by its display name. ContentType and Attachments are left
//!    out: list plumbing, not data. The built-in Title keeps the name `Title`
//!    (its display name is localised, e.g. "Titel");
//!  - text -> VARCHAR, number -> DOUBLE, currency -> DECIMAL, boolean ->
//!    BOOLEAN, dateTime -> TIMESTAMP, choice -> VARCHAR (VARCHAR[] when
//!    multi-valued), hyperlink -> VARCHAR (the URL), calculated -> by its
//!    output type, anything else -> VARCHAR;
//!  - lookup -> its display value (VARCHAR) plus `<name>_id` (BIGINT, field
//!    `<internal>LookupId`); person -> only `<name>_id`, since Graph gives a
//!    person's id, not their name.
//! Names repeated (case-insensitively) get "_2", "_3".
std::vector<ListOutputColumn> PlanListColumns(const std::vector<ListColumnDef> &defs);

//! Parse Graph's RFC 3339 timestamps ("2026-01-31T08:15:00Z", with optional
//! fraction or a +hh:mm offset) into microseconds since 1970-01-01 UTC.
bool ParseIsoTimestamp(const std::string &text, int64_t &micros_since_epoch);

} // namespace sharepoint
} // namespace duckdb
