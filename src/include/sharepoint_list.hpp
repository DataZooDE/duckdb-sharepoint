#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>
#include <unordered_set>
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
	//! Graph may filter on it: past 5,000 items, only indexed columns can be.
	bool indexed = false;
	//! CHOICE shown as a drop-down or radio buttons: one value at most. A
	//! multi-select (check boxes) or unknown display may hold several.
	bool single_choice = false;
	//! PERSON allowing several people (allowMultipleSelection).
	bool multi = false;
	//! DATETIME (or a calculated date) with format dateOnly: a DATE.
	bool date_only = false;
};

//! How a WHERE predicate on a column may become Graph's $filter (#61):
//! NONE, or the kind of literal it compares with.
enum class ListFilterKind { NONE, TEXT, NUMBER, TIMESTAMP };

//! Which part of a person a column is (#63): Graph gives the person's id in
//! the site's User Information List; the name and email are looked up there.
enum class ListPersonPart { NONE, NAME, EMAIL, ID };

//! The DuckDB type a result column becomes.
enum class ListColumnType { VARCHAR, VARCHAR_LIST, DOUBLE, DECIMAL, BOOLEAN, TIMESTAMP, BIGINT, BIGINT_LIST, DATE };

struct ListOutputColumn {
	std::string name;  // result column name (unique, case-insensitively)
	std::string field; // key in the item's fields
	ListColumnType type = ListColumnType::VARCHAR;
	//! CHOICE: becomes VARCHAR_LIST if any item holds several values.
	bool may_be_multi = false;
	//! A lookup's display value: read-only (it is written through `<name>_id`).
	bool lookup_value = false;
	//! Set for an indexed text, choice, number, currency or date column, a
	//! lookup's or person's id, and created/modified when indexed.
	ListFilterKind filter = ListFilterKind::NONE;
	//! A person's name, email or id; `field` is then `<internal>LookupId` and
	//! `person_field` `<internal>` (where Graph may also send the people).
	ListPersonPart person = ListPersonPart::NONE;
	std::string person_field;
	//! Never written: Graph sets it, or it is written through another column.
	bool read_only = false;
};

//! The result columns for a list:
//!  - `id` (BIGINT) first, `created` / `modified` (TIMESTAMP, UTC) last --
//!    the system columns kept;
//!  - every other column that is neither hidden nor read-only, in Graph's
//!    order, named by its display name. ContentType and Attachments are left
//!    out: list plumbing, not data. The built-in Title keeps the name `Title`
//!    (its display name is localised, e.g. "Titel");
//!  - text -> VARCHAR, number -> DOUBLE, currency -> DECIMAL, boolean ->
//!    BOOLEAN, dateTime -> TIMESTAMP (DATE when date-only), choice -> VARCHAR (VARCHAR[] when
//!    multi-valued), hyperlink -> VARCHAR (the URL), calculated -> by its
//!    output type, anything else -> VARCHAR;
//!  - lookup -> its display value (VARCHAR) plus `<name>_id` (BIGINT, field
//!    `<internal>LookupId`); person -> `<name>` and `<name>_email` (VARCHAR)
//!    and `<name>_id` (BIGINT), all lists for a column of several people;
//!  - after created/modified: Created By and Modified By as `created_by`,
//!    `created_by_email`, `created_by_id` and the same for `modified_by`.
//! Names repeated (case-insensitively) get "_2", "_3".
std::vector<ListOutputColumn> PlanListColumns(const std::vector<ListColumnDef> &defs);

//! Parse Graph's RFC 3339 timestamps ("2026-01-31T08:15:00Z", with optional
//! fraction or a +hh:mm offset) into microseconds since 1970-01-01 UTC.
bool ParseIsoTimestamp(const std::string &text, int64_t &micros_since_epoch);

//! Microseconds since 1970-01-01 UTC as Graph writes them: "2026-01-31T08:15:00Z"
//! (".250000" added when there is a fraction).
std::string FormatIsoTimestamp(int64_t micros_since_epoch);

//! One WHERE predicate on a filterable column, as read_sharepoint_list's
//! filter pushdown extracts it: `field <op> literal(s)`.
enum class ListFilterOp { EQ, LT, LE, GT, GE, IN, PREFIX };

struct ListPredicate {
	std::string field; // the item field (ListOutputColumn::field)
	ListFilterKind kind = ListFilterKind::NONE;
	ListFilterOp op = ListFilterOp::EQ;
	std::vector<std::string> texts; // TEXT: one value, several for IN
	std::vector<double> numbers;    // NUMBER: likewise
	int64_t micros = 0;             // TIMESTAMP, UTC
};

//! The OData clause for one predicate, or "" when it is not pushed. DuckDB
//! still applies every predicate itself, so a clause must never be stricter
//! than the SQL -- it may only let through more:
//!  - text: =, IN and a non-empty prefix (SharePoint compares text
//!    case-insensitively, so these match a superset); no ranges or <>;
//!  - number: =, IN, and ranges made inclusive (> becomes ge);
//!  - timestamp: inclusive ranges widened to whole seconds; no =.
std::string ODataClause(const ListPredicate &p);

//! The pushed clauses joined with " and "; "" when none is pushed.
std::string BuildListFilter(const std::vector<ListPredicate> &predicates);
//! The date a date-only value stands for. Graph sends the site's local
//! midnight as UTC, and v1.0 does not say the site's time zone; for every
//! offset from UTC-10:00 to UTC+13:30, local midnight falls before 10:30 UTC
//! on that date or from 10:30 UTC on the day before. (Not right for UTC-11,
//! UTC-12, UTC+13:45 and UTC+14.) Days since 1970-01-01.
int32_t DateOnlyDays(int64_t micros_since_epoch);

//! What to send for a date-only value: 10:15 UTC on that date, which is on
//! that date in every time zone from UTC-10:00 to UTC+13:30.
std::string FormatDateOnly(int32_t days_since_epoch);

//! Where one COPY column goes when writing items.
enum class ListWriteTarget { FIELD, ID, SKIP };

struct ListWriteColumn {
	ListWriteTarget target = ListWriteTarget::SKIP;
	std::string field; // FIELD: the key in the item's fields
	bool date_only = false; // FIELD: a date-only column (FormatDateOnly)
};

//! Map COPY column names onto a list's columns (as PlanListColumns names them,
//! case-insensitively; the internal field name works too):
//!  - `id` -> ID: a row with an id updates that item, a NULL id creates one;
//!  - `created`, `modified` and a lookup's display column -> SKIP: Graph sets
//!    them (a lookup is written through `<name>_id`), so a list read back with
//!    read_sharepoint_list can be written back unchanged;
//!  - any other list column -> FIELD.
//! False, with `error` naming the column and the list's columns, for a name
//! the list does not have.
bool PlanListWrite(const std::vector<std::string> &copy_names, const std::vector<ListOutputColumn> &plan,
                   std::vector<ListWriteColumn> &out, std::string &error);

//! The text a KEY column's value is matched by (#97): a number in one
//! canonical form (17, 17.0 and 1.7e1 are all "17"), so a key read from Graph
//! as JSON and one written from SQL agree.
std::string ListKeyOfNumber(double value);

//! Upsert by key (#97): which existing item a row's key names. Built from the
//! list's items, then asked once per written row.
class ListKeyIndex {
public:
	//! An existing item and its key ("" = no key: never matched).
	void AddItem(const std::string &key, const std::string &item_id);

	enum class Match { CREATE, UPDATE, AMBIGUOUS, REPEATED };
	//! For one row's key: UPDATE `item_id`, or CREATE (no item has it), or
	//! AMBIGUOUS (several items have it), or REPEATED (an earlier row of this
	//! write had it -- which of the two should win is not ours to guess).
	Match Lookup(const std::string &key, std::string &item_id);

private:
	std::unordered_map<std::string, std::string> items_;
	std::unordered_set<std::string> ambiguous_;
	std::unordered_set<std::string> seen_;
};

} // namespace sharepoint
} // namespace duckdb
