// Excel workbook logic behind read_excel_table() / read_excel_range():
// addresses, row windows, number formats, serial dates and column typing.
// Pure -- the Graph calls themselves are proven by the live SQL suite.
#include "sharepoint_excel.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <string>
#include <vector>

using namespace duckdb::sharepoint;

namespace {

ExcelCell Num(double v, const std::string &format = "General") {
	ExcelCell c;
	c.kind = ExcelCellKind::NUMBER;
	c.number = v;
	c.format = format;
	return c;
}

ExcelCell Str(const std::string &s) {
	ExcelCell c;
	c.kind = ExcelCellKind::STRING;
	c.text = s;
	return c;
}

ExcelCell Bool(bool b) {
	ExcelCell c;
	c.kind = ExcelCellKind::BOOLEAN;
	c.boolean = b;
	return c;
}

ExcelCell Err(const std::string &code) {
	ExcelCell c;
	c.kind = ExcelCellKind::ERROR;
	c.text = code;
	return c;
}

ExcelCell Empty() {
	return ExcelCell {};
}

ExcelColumnType Infer(const std::vector<ExcelCell> &cells) {
	std::vector<const ExcelCell *> ptrs;
	for (auto &c : cells) {
		ptrs.push_back(&c);
	}
	return InferExcelColumnType(ptrs);
}

} // namespace

TEST_CASE("Excel column letters round-trip", "[excel][address]") {
	REQUIRE(ExcelColumnIndex("A") == 1);
	REQUIRE(ExcelColumnIndex("Z") == 26);
	REQUIRE(ExcelColumnIndex("AA") == 27);
	REQUIRE(ExcelColumnIndex("az") == 52); // case-insensitive
	REQUIRE(ExcelColumnIndex("XFD") == 16384);
	REQUIRE(ExcelColumnIndex("") == 0);
	REQUIRE(ExcelColumnIndex("A1") == 0);
	REQUIRE(ExcelColumnIndex("XFE") == 0); // beyond Excel's last column
	for (int64_t i : {1, 26, 27, 52, 702, 703, 16384}) {
		REQUIRE(ExcelColumnIndex(ExcelColumnLetters(i)) == i);
	}
	REQUIRE(ExcelColumnLetters(0).empty());
}

TEST_CASE("ParseExcelAddress: sheet, range, single cell, bare sheet", "[excel][address]") {
	auto p = ParseExcelAddress("Input!A1:F200");
	REQUIRE(p.ok);
	REQUIRE(p.address.sheet == "Input");
	REQUIRE(p.address.first_row == 1);
	REQUIRE(p.address.first_col == 1);
	REQUIRE(p.address.last_row == 200);
	REQUIRE(p.address.last_col == 6);
	REQUIRE(p.address.RowCount() == 200);
	REQUIRE(p.address.ColCount() == 6);

	p = ParseExcelAddress("Sheet1!$B$3");
	REQUIRE(p.ok);
	REQUIRE(p.address.first_row == 3);
	REQUIRE(p.address.last_row == 3);
	REQUIRE(p.address.first_col == 2);
	REQUIRE(p.address.last_col == 2);

	p = ParseExcelAddress("Input");
	REQUIRE(p.ok);
	REQUIRE(p.address.sheet == "Input");
	REQUIRE_FALSE(p.address.HasCells());

	// Corners in either order.
	p = ParseExcelAddress("S!F200:A1");
	REQUIRE(p.ok);
	REQUIRE(p.address.first_row == 1);
	REQUIRE(p.address.last_col == 6);
}

TEST_CASE("ParseExcelAddress: quoted sheet names", "[excel][address]") {
	auto p = ParseExcelAddress("'My Sheet'!A1:B2");
	REQUIRE(p.ok);
	REQUIRE(p.address.sheet == "My Sheet");
	p = ParseExcelAddress("'It''s'!A1");
	REQUIRE(p.ok);
	REQUIRE(p.address.sheet == "It's");
	p = ParseExcelAddress("'Q1!Plan'");
	REQUIRE(p.ok);
	REQUIRE(p.address.sheet == "Q1!Plan");
	REQUIRE_FALSE(p.address.HasCells());
}

TEST_CASE("ParseExcelAddress: rejects what the workbook API cannot read", "[excel][address]") {
	REQUIRE_FALSE(ParseExcelAddress("").ok);
	REQUIRE_FALSE(ParseExcelAddress("A1:B2").ok); // no sheet
	REQUIRE(ParseExcelAddress("A1:B2", false).ok);
	REQUIRE_FALSE(ParseExcelAddress("S!").ok);
	REQUIRE_FALSE(ParseExcelAddress("S!A0").ok);
	REQUIRE_FALSE(ParseExcelAddress("S!1:2").ok);    // whole rows: no column bound
	REQUIRE_FALSE(ParseExcelAddress("S!A:B").ok);    // whole columns: unbounded
	REQUIRE_FALSE(ParseExcelAddress("'open!A1").ok); // unterminated quote
	REQUIRE_FALSE(ParseExcelAddress("S!A1:B2:C3").ok);
	auto p = ParseExcelAddress("S!A1:B");
	REQUIRE_FALSE(p.ok);
	REQUIRE_FALSE(p.error.empty());
}

TEST_CASE("FormatExcelAddress quotes sheet names that need it", "[excel][address]") {
	ExcelAddress a;
	a.sheet = "Sheet1";
	a.first_row = 2;
	a.first_col = 1;
	a.last_row = 10;
	a.last_col = 28;
	REQUIRE(FormatExcelAddress(a) == "Sheet1!A2:AB10");
	a.sheet = "My Sheet";
	REQUIRE(FormatExcelAddress(a) == "'My Sheet'!A2:AB10");
	a.sheet = "It's";
	REQUIRE(FormatExcelAddress(a) == "'It''s'!A2:AB10");
	// Round trip.
	auto p = ParseExcelAddress(FormatExcelAddress(a));
	REQUIRE(p.ok);
	REQUIRE(p.address.sheet == "It's");
	REQUIRE(p.address.last_col == 28);
}

TEST_CASE("SplitExcelRows: windows cover the range exactly once", "[excel][chunk]") {
	auto a = ParseExcelAddress("S!B1:E10001").address; // 4 columns, 10001 rows
	auto windows = SplitExcelRows(a, 4000);            // 1000 rows each
	REQUIRE(windows.size() == 11);
	int64_t next = 1;
	for (auto &w : windows) {
		REQUIRE(w.sheet == "S");
		REQUIRE(w.first_col == 2);
		REQUIRE(w.last_col == 5);
		REQUIRE(w.first_row == next);
		REQUIRE(w.RowCount() * w.ColCount() <= 4000);
		next = w.last_row + 1;
	}
	REQUIRE(next == 10002);

	// A row wider than the budget still makes progress: one row per window.
	windows = SplitExcelRows(ParseExcelAddress("S!A1:Z3").address, 10);
	REQUIRE(windows.size() == 3);

	// Nothing to split.
	REQUIRE(SplitExcelRows(ParseExcelAddress("S").address, 100).empty());
}

TEST_CASE("ClassifyExcelNumberFormat", "[excel][format]") {
	REQUIRE(ClassifyExcelNumberFormat("General") == ExcelNumberKind::NUMBER);
	REQUIRE(ClassifyExcelNumberFormat("") == ExcelNumberKind::NUMBER);
	REQUIRE(ClassifyExcelNumberFormat("0.00") == ExcelNumberKind::NUMBER);
	REQUIRE(ClassifyExcelNumberFormat("#,##0") == ExcelNumberKind::NUMBER);
	REQUIRE(ClassifyExcelNumberFormat("0%") == ExcelNumberKind::NUMBER);
	REQUIRE(ClassifyExcelNumberFormat("0.00E+00") == ExcelNumberKind::NUMBER);
	REQUIRE(ClassifyExcelNumberFormat("[$€-407] #,##0.00") == ExcelNumberKind::NUMBER);
	REQUIRE(ClassifyExcelNumberFormat("\"days: \"0") == ExcelNumberKind::NUMBER); // 'd' quoted
	REQUIRE(ClassifyExcelNumberFormat("0\\d") == ExcelNumberKind::NUMBER);        // 'd' escaped
	REQUIRE(ClassifyExcelNumberFormat("[Red]0.00;[Blue]-0.00") == ExcelNumberKind::NUMBER);

	REQUIRE(ClassifyExcelNumberFormat("yyyy-mm-dd") == ExcelNumberKind::DATE);
	REQUIRE(ClassifyExcelNumberFormat("m/d/yyyy") == ExcelNumberKind::DATE);
	REQUIRE(ClassifyExcelNumberFormat("dd.mm.yyyy") == ExcelNumberKind::DATE);
	REQUIRE(ClassifyExcelNumberFormat("mmm yy") == ExcelNumberKind::DATE);
	REQUIRE(ClassifyExcelNumberFormat("[$-407]dddd, d. mmmm yyyy") == ExcelNumberKind::DATE);
	REQUIRE(ClassifyExcelNumberFormat("YYYY-MM-DD") == ExcelNumberKind::DATE);

	REQUIRE(ClassifyExcelNumberFormat("yyyy-mm-dd hh:mm:ss") == ExcelNumberKind::DATETIME);
	REQUIRE(ClassifyExcelNumberFormat("m/d/yy h:mm AM/PM") == ExcelNumberKind::DATETIME);

	REQUIRE(ClassifyExcelNumberFormat("h:mm") == ExcelNumberKind::TIME);  // m = minutes after h
	REQUIRE(ClassifyExcelNumberFormat("mm:ss") == ExcelNumberKind::TIME); // m = minutes before s
	REQUIRE(ClassifyExcelNumberFormat("hh:mm:ss") == ExcelNumberKind::TIME);
	REQUIRE(ClassifyExcelNumberFormat("[h]:mm:ss") == ExcelNumberKind::TIME); // elapsed hours
	REQUIRE(ClassifyExcelNumberFormat("h:mm AM/PM") == ExcelNumberKind::TIME);
}

TEST_CASE("ExcelSerialToEpoch: the 1900 date system", "[excel][date]") {
	int64_t days = 0, micros = 0;
	ExcelSerialToEpoch(25569, days, micros); // 1970-01-01
	REQUIRE(days == 0);
	REQUIRE(micros == 0);

	ExcelSerialToEpoch(45658, days, micros); // 2025-01-01
	REQUIRE(days == 20089);

	ExcelSerialToEpoch(45658.75, days, micros); // 2025-01-01 18:00
	REQUIRE(days == 20089);
	REQUIRE(micros == 18LL * 3600 * 1000000);

	// Lotus' phantom 1900-02-29 (serial 60): serials before it are one day
	// later than plain day arithmetic from 1899-12-30 says.
	ExcelSerialToEpoch(1, days, micros); // 1900-01-01
	REQUIRE(days == -25567);
	ExcelSerialToEpoch(59, days, micros); // 1900-02-28
	REQUIRE(days == -25509);
	ExcelSerialToEpoch(61, days, micros); // 1900-03-01
	REQUIRE(days == -25508);

	// A time of day alone: day 0, 1899-12-30.
	ExcelSerialToEpoch(0.5, days, micros);
	REQUIRE(days == -25569);
	REQUIRE(micros == 12LL * 3600 * 1000000);

	// Rounded to the millisecond; 23:59:59.9996 rounds into the next day.
	ExcelSerialToEpoch(45658 + 0.99999999999, days, micros);
	REQUIRE(days == 20090);
	REQUIRE(micros == 0);
}

TEST_CASE("InferExcelColumnType", "[excel][types]") {
	REQUIRE(Infer({Num(1), Num(2), Empty(), Num(3)}) == ExcelColumnType::BIGINT);
	REQUIRE(Infer({Num(1), Num(2.5)}) == ExcelColumnType::DOUBLE);
	REQUIRE(Infer({Num(1e300)}) == ExcelColumnType::DOUBLE); // integral but beyond BIGINT
	REQUIRE(Infer({Bool(true), Empty(), Bool(false)}) == ExcelColumnType::BOOLEAN);
	REQUIRE(Infer({Str("a"), Num(1)}) == ExcelColumnType::VARCHAR);
	REQUIRE(Infer({Bool(true), Num(1)}) == ExcelColumnType::VARCHAR);
	REQUIRE(Infer({}) == ExcelColumnType::VARCHAR);
	REQUIRE(Infer({Empty(), Empty()}) == ExcelColumnType::VARCHAR);

	REQUIRE(Infer({Num(45658, "yyyy-mm-dd"), Num(45659, "yyyy-mm-dd")}) == ExcelColumnType::DATE);
	REQUIRE(Infer({Num(45658, "yyyy-mm-dd"), Num(45659.5, "yyyy-mm-dd hh:mm")}) == ExcelColumnType::TIMESTAMP);
	REQUIRE(Infer({Num(0.5, "h:mm"), Num(0.25, "hh:mm:ss")}) == ExcelColumnType::TIME);
	REQUIRE(Infer({Num(45658, "yyyy-mm-dd"), Num(3)}) == ExcelColumnType::DOUBLE);

	// Errors read as NULL in a typed column; only errors stay VARCHAR.
	REQUIRE(Infer({Num(1), Err("#DIV/0!"), Num(2)}) == ExcelColumnType::BIGINT);
	REQUIRE(Infer({Err("#N/A"), Err("#REF!")}) == ExcelColumnType::VARCHAR);
}

TEST_CASE("ExcelCellText", "[excel][types]") {
	REQUIRE(ExcelCellText(Str("hello")) == "hello");
	REQUIRE(ExcelCellText(Bool(true)) == "TRUE");
	REQUIRE(ExcelCellText(Bool(false)) == "FALSE");
	REQUIRE(ExcelCellText(Num(42)) == "42");
	REQUIRE(ExcelCellText(Num(-3)) == "-3");
	REQUIRE(ExcelCellText(Num(2.5)) == "2.5");
	REQUIRE(ExcelCellText(Num(0.1 + 0.2)) == "0.3"); // 15 significant digits, like Excel
	REQUIRE(ExcelCellText(Err("#DIV/0!")) == "#DIV/0!");
	// Date-formatted numbers read as ISO text, not as serials.
	REQUIRE(ExcelCellText(Num(45658, "yyyy-mm-dd")) == "2025-01-01");
	REQUIRE(ExcelCellText(Num(45658.75, "dd.mm.yyyy hh:mm")) == "2025-01-01 18:00:00");
	REQUIRE(ExcelCellText(Num(0.5625, "h:mm")) == "13:30:00");
	REQUIRE(ExcelCellText(Num(59, "yyyy-mm-dd")) == "1900-02-28");
	REQUIRE(ExcelCellText(Num(45658.0000116, "yyyy-mm-dd hh:mm:ss")) == "2025-01-01 00:00:01");
}

TEST_CASE("ExcelHeaderNames", "[excel][header]") {
	auto names =
	    ExcelHeaderNames({Str(" Region "), Str("Revenue"), Empty(), Str("revenue"), Num(2025), Str("Revenue")});
	REQUIRE(names == std::vector<std::string> {"Region", "Revenue", "column3", "revenue_2", "2025", "Revenue_3"});
	// A generated name that collides with a real one is suffixed too.
	names = ExcelHeaderNames({Str("column2"), Empty()});
	REQUIRE(names == std::vector<std::string> {"column2", "column2_2"});
}

TEST_CASE("ExcelSerialFromEpoch inverts ExcelSerialToEpoch", "[excel][write]") {
	REQUIRE(ExcelSerialFromEpoch(0, 0) == 25569);     // 1970-01-01
	REQUIRE(ExcelSerialFromEpoch(20089, 0) == 45658); // 2025-01-01
	REQUIRE(ExcelSerialFromEpoch(20089, 18LL * 3600 * 1000000) == 45658.75);
	REQUIRE(ExcelSerialFromEpoch(-25509, 0) == 59);                      // 1900-02-28, before the phantom day
	REQUIRE(ExcelSerialFromEpoch(-25508, 0) == 61);                      // 1900-03-01
	REQUIRE(ExcelSerialFromEpoch(-25569, 12LL * 3600 * 1000000) == 0.5); // a time of day alone
	for (double serial : {1.0, 59.0, 61.0, 45658.0, 45658.5, 2958465.0}) {
		int64_t days, micros;
		ExcelSerialToEpoch(serial, days, micros);
		REQUIRE(ExcelSerialFromEpoch(days, micros) == serial);
	}
}

TEST_CASE("ExcelTextInput keeps text as text", "[excel][write]") {
	// Graph sets values as if typed: '=1+1' would become a formula, '00123'
	// a number, 'TRUE' a boolean, '1/2' a date. A leading apostrophe makes
	// Excel store the rest as text; it is not part of the value.
	REQUIRE(ExcelTextInput("=1+1") == "'=1+1");
	REQUIRE(ExcelTextInput("00123") == "'00123");
	REQUIRE(ExcelTextInput("North") == "'North");
	REQUIRE(ExcelTextInput("'quoted") == "''quoted");
	REQUIRE(ExcelTextInput("").empty()); // empty clears the cell
}

TEST_CASE("ExcelBlockAt: the cells a write covers from its anchor", "[excel][write]") {
	ExcelAddress block;
	std::string error;
	auto anchor = ParseExcelAddress("Out!B3").address;
	REQUIRE(ExcelBlockAt(anchor, 10, 4, block, error));
	REQUIRE(FormatExcelAddress(block) == "Out!B3:E12");

	// A full range as anchor: only its top-left cell counts.
	REQUIRE(ExcelBlockAt(ParseExcelAddress("Out!C5:Z99").address, 1, 1, block, error));
	REQUIRE(FormatExcelAddress(block) == "Out!C5:C5");

	// A bare sheet anchors at A1.
	REQUIRE(ExcelBlockAt(ParseExcelAddress("Out").address, 2, 2, block, error));
	REQUIRE(FormatExcelAddress(block) == "Out!A1:B2");

	// Past Excel's last row or column is an error, not a wrap.
	REQUIRE_FALSE(ExcelBlockAt(ParseExcelAddress("S!A1048576").address, 2, 1, block, error));
	REQUIRE(error.find("1048576") != std::string::npos);
	REQUIRE_FALSE(ExcelBlockAt(ParseExcelAddress("S!XFD1").address, 1, 2, block, error));
	REQUIRE_FALSE(ExcelBlockAt(anchor, 0, 4, block, error)); // nothing to write
}

TEST_CASE("ExcelClearRegion: from the anchor down and right, within the used range", "[excel][write]") {
	ExcelAddress region;
	// Used A1:F40, anchor B3: clear B3:F40 -- column A and rows 1-2 stay.
	REQUIRE(ExcelClearRegion(ParseExcelAddress("Out!B3").address, ParseExcelAddress("Out!A1:F40").address, region));
	REQUIRE(FormatExcelAddress(region) == "Out!B3:F40");
	// A bare sheet anchors at A1: the whole used range.
	REQUIRE(ExcelClearRegion(ParseExcelAddress("Out").address, ParseExcelAddress("Out!C2:D9").address, region));
	REQUIRE(FormatExcelAddress(region) == "Out!A1:D9");
	// Only the anchor's top-left cell counts.
	REQUIRE(ExcelClearRegion(ParseExcelAddress("Out!B3:C4").address, ParseExcelAddress("Out!A1:F40").address, region));
	REQUIRE(FormatExcelAddress(region) == "Out!B3:F40");
	// Nothing below or right of the anchor: nothing to clear.
	REQUIRE_FALSE(
	    ExcelClearRegion(ParseExcelAddress("Out!G1").address, ParseExcelAddress("Out!A1:F40").address, region));
	REQUIRE_FALSE(
	    ExcelClearRegion(ParseExcelAddress("Out!A41").address, ParseExcelAddress("Out!A1:F40").address, region));
}
