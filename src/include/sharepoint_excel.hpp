#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace duckdb {
namespace sharepoint {

// ---------------------------------------------------------------------------
// Excel workbook logic behind read_excel_table() / read_excel_range(). PURE:
// no duckdb.hpp, no I/O, so the Catch2 binary compiles it directly.
//
// The Graph workbook API (/drives/{d}/items/{id}/workbook/...) answers a range
// with three parallel 2-D arrays: `values` (raw numbers, text, booleans),
// `valueTypes` ("Empty", "String", "Double", "Boolean", "Error", ...) and
// `numberFormat` ("General", "yyyy-mm-dd", "0.00", ...). Dates are plain
// serial numbers there; only the number format says a cell is a date. This
// file turns those cells into typed columns. See docs/hld.md §9.
// ---------------------------------------------------------------------------

//! A1-style range address, as Graph returns it ("Sheet1!A1:F200") and as a
//! user writes it ("Input!A1:F200", "'My Sheet'!B2:C9", "Input"). Rows and
//! columns are 1-based; 0 means "not given" (a bare sheet name).
struct ExcelAddress {
	std::string sheet;
	int64_t first_row = 0;
	int64_t first_col = 0;
	int64_t last_row = 0;
	int64_t last_col = 0;
	bool HasCells() const {
		return first_row > 0;
	}
	int64_t RowCount() const {
		return HasCells() ? last_row - first_row + 1 : 0;
	}
	int64_t ColCount() const {
		return HasCells() ? last_col - first_col + 1 : 0;
	}
};

struct ExcelAddressParse {
	bool ok = false;
	std::string error;
	ExcelAddress address;
};

//! "A" -> 1, "Z" -> 26, "AA" -> 27, "XFD" -> 16384. 0 if not a column.
int64_t ExcelColumnIndex(const std::string &letters);
//! 1 -> "A", 27 -> "AA". Empty for < 1.
std::string ExcelColumnLetters(int64_t index);

//! Parse "Sheet!A1:B2", "Sheet!A1", "'It''s'!A1:B2" or a bare sheet name.
//! A single cell is a 1x1 range; corners in either order are normalised.
//! `require_sheet`: an address without a sheet ("A1:B2") is an error, since
//! the workbook API needs to know which worksheet to read.
ExcelAddressParse ParseExcelAddress(const std::string &text, bool require_sheet = true);

//! "Sheet1!A1:F200", quoting the sheet ('My Sheet') when it needs it.
std::string FormatExcelAddress(const ExcelAddress &address);

//! Split a range into row windows of at most `max_cells` cells each (at least
//! one row per window), for reading a large range a block at a time: Graph
//! rejects or times out on very large single range requests.
std::vector<ExcelAddress> SplitExcelRows(const ExcelAddress &address, int64_t max_cells);

//! What a number format makes of a numeric cell.
enum class ExcelNumberKind { NUMBER, DATE, DATETIME, TIME };

//! Classify a number format code. Date/time tokens (y, m, d, h, s, AM/PM)
//! outside quoted literals, escapes and [...] sections make it a date; "m"
//! is minutes when it follows h or precedes s, else months. "General",
//! "0.00", "#,##0" and currency formats are NUMBER. Only the first section of
//! a multi-section format ("pos;neg;zero") is consulted.
ExcelNumberKind ClassifyExcelNumberFormat(const std::string &format);

//! Excel's 1900 date system: serial 1 is 1900-01-01, and serial 60 is the
//! non-existent 1900-02-29 Lotus kept. Days since 1970-01-01 for the date
//! part of `serial` (floor), and microseconds into that day (rounded to the
//! millisecond, as Excel stores no more). Serials below 1 are times of day
//! on 1899-12-30, Excel's day 0.
void ExcelSerialToEpoch(double serial, int64_t &days_since_epoch, int64_t &micros_of_day);

//! The inverse of ExcelSerialToEpoch: the serial Excel stores for a date
//! (days since 1970-01-01) and a time of day. 1900-01-01..1900-02-28 count
//! from 1899-12-31, as Excel does.
double ExcelSerialFromEpoch(int64_t days_since_epoch, int64_t micros_of_day);

//! The input that stores `text` as text. Graph sets values as if typed, so
//! "=1+1" would become a formula and "00123" a number; a leading apostrophe
//! keeps the rest as text and is not part of the value. Empty stays empty
//! (it clears the cell).
std::string ExcelTextInput(const std::string &text);

//! The block of `rows` x `cols` cells a write anchored at `anchor` covers:
//! the anchor's top-left cell (A1 for a bare sheet) down and right. False,
//! with `error` set, when there is nothing to write or the block would run
//! past Excel's last row (1048576) or column (XFD).
bool ExcelBlockAt(const ExcelAddress &anchor, int64_t rows, int64_t cols, ExcelAddress &block, std::string &error);

//! One cell from the workbook API.
enum class ExcelCellKind { EMPTY, NUMBER, STRING, BOOLEAN, ERROR };

struct ExcelCell {
	ExcelCellKind kind = ExcelCellKind::EMPTY;
	double number = 0;
	bool boolean = false;
	//! The text of a STRING or ERROR ("#DIV/0!") cell.
	std::string text;
	//! The cell's number format (empty or "General" when it has none).
	std::string format;
};

//! The DuckDB type a column becomes.
enum class ExcelColumnType { VARCHAR, DOUBLE, BIGINT, BOOLEAN, DATE, TIMESTAMP, TIME };

//! The narrowest type that holds every non-empty cell of a column:
//!  - only booleans -> BOOLEAN
//!  - only numbers  -> by number format: all DATE -> DATE; DATE/DATETIME mix
//!    -> TIMESTAMP; all TIME -> TIME; all NUMBER and integral -> BIGINT, else
//!    DOUBLE; numbers mixing dates with plain numbers -> DOUBLE
//!  - anything else, or no cell at all -> VARCHAR
//! ERROR cells (#N/A, #DIV/0!) do not vote: they read as NULL in a typed
//! column. A column of nothing but errors is VARCHAR and shows them.
ExcelColumnType InferExcelColumnType(const std::vector<const ExcelCell *> &cells);

//! The text a cell shows in a VARCHAR column: strings as-is, booleans as
//! TRUE/FALSE (Excel's spelling), date/time-formatted numbers as ISO text
//! ("2025-01-01", "2025-01-01 18:00:00", "13:30:00"), integral numbers
//! without ".0", other numbers with up to 15 significant digits (Excel's
//! precision), errors as their code. EMPTY is not representable; callers emit
//! NULL for it.
std::string ExcelCellText(const ExcelCell &cell);

//! Column names from a header row: a cell's text, trimmed; an empty header
//! becomes "column<N>" (1-based, its position); duplicates get "_2", "_3"
//! (compared case-insensitively, since DuckDB column names are).
std::vector<std::string> ExcelHeaderNames(const std::vector<ExcelCell> &header);

} // namespace sharepoint
} // namespace duckdb
