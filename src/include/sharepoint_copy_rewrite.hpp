#pragma once

// The in-place COPY rewrite shared by the writers that change a remote
// object through an API instead of writing a file: FORMAT excel_table /
// excel_range (sharepoint_excel_functions.cpp) and sharepoint_list
// (sharepoint_list_functions.cpp). See docs/hld.md §9.1.
//
// DuckDB's COPY TO an EXISTING target defaults to USE_TMP_FILE: the writer is
// handed "tmp_<name>" and the operator MoveFile()s it over the target at the
// end. For these writers that rename would hit the user's object (a workbook
// via our filesystem). So the public format carries only CopyFunction::plan,
// which rebinds the statement as an internal format with USE_TMP_FILE false
// and a marker option; the internal writer refuses to bind without the marker
// (the binder consumes USE_TMP_FILE itself before copy_to_bind). Same hook and
// option flow on DuckDB 1.5.6 and 2.0 (whose options are keyed by Identifier).

#include "duckdb.hpp"
#include "duckdb/function/copy_function.hpp"
#include "duckdb/parser/statement/copy_statement.hpp"
#include "duckdb/planner/binder.hpp"

#include <type_traits>

namespace duckdb {
namespace sharepoint {

//! Declared by every internal writer, set only by RewriteCopyInPlace.
constexpr const char *kInPlaceMarker = "__sharepoint_in_place";
//! COPY's own OVERWRITE, carried past the binder (which would consume it as a
//! file option) for the writers that support it (the Excel ones).
constexpr const char *kOverwriteMarker = "__sharepoint_overwrite";

//! A COPY option's values, or nullptr; the map's key is a string before 2.0
//! and an Identifier in 2.0.
template <class MAP>
const vector<Value> *FindCopyOption(const MAP &options, const string &name) {
	auto entry = options.find(typename MAP::key_type(name));
	return entry == options.end() ? nullptr : &entry->second;
}

template <class MAP>
void AddCopyOption(MAP &options, const char *name, const LogicalType &type) {
	options[typename MAP::key_type(name)] = CopyOption(type, CopyOptionMode::WRITE_ONLY);
}

//! True if the COPY came through RewriteCopyInPlace.
inline bool IsRewrittenInPlace(const CopyInfo &info) {
	return FindCopyOption(info.options, kInPlaceMarker) != nullptr;
}

//! Rebind `stmt` as `internal_format`, in place (USE_TMP_FILE false). `what`
//! names the target in errors. With `supports_overwrite`, COPY's OVERWRITE is
//! carried as kOverwriteMarker (it never reaches DuckDB's file handling);
//! without, it is refused. OVERWRITE_OR_IGNORE and APPEND are always refused.
inline BoundStatement RewriteCopyInPlace(Binder &binder, CopyStatement &stmt, const char *internal_format,
                                         const char *what, bool supports_overwrite = false) {
	auto copy = stmt.Copy();
	auto &info = *copy->Cast<CopyStatement>().info;
	auto tmp = FindCopyOption(info.options, "use_tmp_file");
	if (tmp && (tmp->empty() || (*tmp)[0].DefaultCastAs(LogicalType::BOOLEAN).GetValue<bool>())) {
		throw InvalidInputException("sharepoint: USE_TMP_FILE is not supported for %s writes: they change the %s "
		                            "in place",
		                            what, what);
	}
	info.format = internal_format;
	using Key = typename std::decay<decltype(info.options)>::type::key_type;
	for (const char *mode : {"overwrite_or_ignore", "append"}) {
		if (FindCopyOption(info.options, mode)) {
			throw InvalidInputException("sharepoint: %s is a file option, not supported for %s writes", mode, what);
		}
	}
	auto overwrite = FindCopyOption(info.options, "overwrite");
	const bool replace =
	    overwrite && (overwrite->empty() || (*overwrite)[0].DefaultCastAs(LogicalType::BOOLEAN).GetValue<bool>());
	if (overwrite && !supports_overwrite) {
		throw InvalidInputException("sharepoint: OVERWRITE is not supported for %s writes", what);
	}
	info.options.erase(Key("overwrite"));
	if (replace) {
		info.options[Key(kOverwriteMarker)] = {Value::BOOLEAN(true)};
	}
	info.options[Key("use_tmp_file")] = {Value::BOOLEAN(false)};
	info.options[Key(kInPlaceMarker)] = {Value::BOOLEAN(true)};
	return binder.Bind(*copy);
}

} // namespace sharepoint
} // namespace duckdb
