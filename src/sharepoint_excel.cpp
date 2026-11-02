// Excel workbook logic for read_excel_table() / read_excel_range(). Pure: no
// duckdb.hpp and no I/O (see sharepoint_excel.hpp).
#include "sharepoint_excel.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <set>

namespace duckdb {
namespace sharepoint {

namespace {

constexpr int64_t kExcelMaxColumns = 16384; // XFD
constexpr int64_t kExcelMaxRows = 1048576;

std::string Trim(const std::string &s) {
	size_t b = 0, e = s.size();
	while (b < e && std::isspace(static_cast<unsigned char>(s[b]))) {
		b++;
	}
	while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1]))) {
		e--;
	}
	return s.substr(b, e - b);
}

std::string Lower(std::string s) {
	for (auto &c : s) {
		c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
	}
	return s;
}

// One corner: "$B$3" / "b3" -> row 3, column 2. False if not a cell.
bool ParseCell(const std::string &text, int64_t &row, int64_t &col) {
	size_t i = 0;
	if (i < text.size() && text[i] == '$') {
		i++;
	}
	size_t letters_begin = i;
	while (i < text.size() && std::isalpha(static_cast<unsigned char>(text[i]))) {
		i++;
	}
	const std::string letters = text.substr(letters_begin, i - letters_begin);
	if (i < text.size() && text[i] == '$') {
		i++;
	}
	size_t digits_begin = i;
	while (i < text.size() && std::isdigit(static_cast<unsigned char>(text[i]))) {
		i++;
	}
	const std::string digits = text.substr(digits_begin, i - digits_begin);
	if (i != text.size() || letters.empty() || digits.empty() || digits.size() > 7) {
		return false;
	}
	col = ExcelColumnIndex(letters);
	row = std::stoll(digits);
	return col > 0 && row >= 1 && row <= kExcelMaxRows;
}

// "A1:B2" or "A1" into `out`'s corners. False if not a range of cells.
bool ParseCells(const std::string &text, ExcelAddress &out) {
	const auto colon = text.find(':');
	if (colon != std::string::npos && text.find(':', colon + 1) != std::string::npos) {
		return false;
	}
	int64_t r1, c1, r2, c2;
	if (colon == std::string::npos) {
		if (!ParseCell(text, r1, c1)) {
			return false;
		}
		r2 = r1;
		c2 = c1;
	} else if (!ParseCell(text.substr(0, colon), r1, c1) || !ParseCell(text.substr(colon + 1), r2, c2)) {
		return false;
	}
	out.first_row = std::min(r1, r2);
	out.last_row = std::max(r1, r2);
	out.first_col = std::min(c1, c2);
	out.last_col = std::max(c1, c2);
	return true;
}

bool NeedsQuoting(const std::string &sheet) {
	if (sheet.empty() || !(std::isalpha(static_cast<unsigned char>(sheet[0])) || sheet[0] == '_')) {
		return true;
	}
	for (unsigned char c : sheet) {
		if (!(std::isalnum(c) || c == '_' || c == '.')) {
			return true;
		}
	}
	// A sheet named like a cell ("A1") would read back as a cell.
	ExcelAddress probe;
	return ParseCells(sheet, probe);
}

// Days since 1970-01-01 -> civil date (Howard Hinnant's days_from_civil
// inverse; exact for the proleptic Gregorian calendar).
void CivilFromDays(int64_t z, int64_t &y, int &m, int &d) {
	z += 719468;
	const int64_t era = (z >= 0 ? z : z - 146096) / 146097;
	const int64_t doe = z - era * 146097;
	const int64_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
	const int64_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
	const int64_t mp = (5 * doy + 2) / 153;
	d = static_cast<int>(doy - (153 * mp + 2) / 5 + 1);
	m = static_cast<int>(mp < 10 ? mp + 3 : mp - 9);
	y = yoe + era * 400 + (m <= 2 ? 1 : 0);
}

bool IsIntegral(double v) {
	// 2^53: beyond it a double no longer holds every integer.
	return std::isfinite(v) && std::floor(v) == v && std::fabs(v) <= 9007199254740992.0;
}

} // namespace

int64_t ExcelColumnIndex(const std::string &letters) {
	if (letters.empty() || letters.size() > 3) {
		return 0;
	}
	int64_t index = 0;
	for (unsigned char c : letters) {
		if (!std::isalpha(c)) {
			return 0;
		}
		index = index * 26 + (std::toupper(c) - 'A' + 1);
	}
	return index <= kExcelMaxColumns ? index : 0;
}

std::string ExcelColumnLetters(int64_t index) {
	std::string out;
	while (index > 0) {
		index--;
		out.insert(out.begin(), static_cast<char>('A' + index % 26));
		index /= 26;
	}
	return out;
}

ExcelAddressParse ParseExcelAddress(const std::string &input, bool require_sheet) {
	ExcelAddressParse result;
	const std::string text = Trim(input);
	if (text.empty()) {
		result.error = "the range address is empty";
		return result;
	}
	std::string sheet;
	std::string cells;
	bool has_sheet = false;
	if (text[0] == '\'') {
		// 'Sheet name'!A1 -- '' inside the quotes is one quote.
		size_t i = 1;
		bool closed = false;
		while (i < text.size()) {
			if (text[i] == '\'') {
				if (i + 1 < text.size() && text[i + 1] == '\'') {
					sheet += '\'';
					i += 2;
					continue;
				}
				closed = true;
				i++;
				break;
			}
			sheet += text[i++];
		}
		if (!closed) {
			result.error = "unterminated quote in the sheet name of '" + text + "'";
			return result;
		}
		has_sheet = true;
		if (i < text.size()) {
			if (text[i] != '!') {
				result.error = "expected '!' after the quoted sheet name in '" + text + "'";
				return result;
			}
			cells = text.substr(i + 1);
			if (cells.empty()) {
				result.error = "no cells after '!' in '" + text + "'";
				return result;
			}
		}
	} else {
		const auto bang = text.rfind('!');
		if (bang == std::string::npos) {
			ExcelAddress probe;
			if (ParseCells(text, probe)) {
				if (require_sheet) {
					result.error = "'" + text + "' names no worksheet; write it as 'Sheet1!" + text + "'";
					return result;
				}
				result.address = probe;
				result.ok = true;
				return result;
			}
			sheet = text; // a bare sheet name
		} else {
			sheet = text.substr(0, bang);
			cells = text.substr(bang + 1);
			if (cells.empty()) {
				result.error = "no cells after '!' in '" + text + "'";
				return result;
			}
		}
		has_sheet = true;
	}
	if (has_sheet && sheet.empty()) {
		result.error = "empty sheet name in '" + text + "'";
		return result;
	}
	result.address.sheet = sheet;
	if (!cells.empty() && !ParseCells(cells, result.address)) {
		result.error = "'" + cells + "' is not a range of cells such as A1:F200 (whole rows or columns are " +
		               "not supported; give both corners)";
		return result;
	}
	result.ok = true;
	return result;
}

std::string FormatExcelAddress(const ExcelAddress &address) {
	std::string sheet;
	if (NeedsQuoting(address.sheet)) {
		sheet = "'";
		for (char c : address.sheet) {
			sheet += c;
			if (c == '\'') {
				sheet += '\'';
			}
		}
		sheet += "'";
	} else {
		sheet = address.sheet;
	}
	if (!address.HasCells()) {
		return sheet;
	}
	return sheet + "!" + ExcelColumnLetters(address.first_col) + std::to_string(address.first_row) + ":" +
	       ExcelColumnLetters(address.last_col) + std::to_string(address.last_row);
}

std::vector<ExcelAddress> SplitExcelRows(const ExcelAddress &address, int64_t max_cells) {
	std::vector<ExcelAddress> windows;
	if (!address.HasCells()) {
		return windows;
	}
	const int64_t rows_per_window = std::max<int64_t>(1, max_cells / std::max<int64_t>(1, address.ColCount()));
	for (int64_t row = address.first_row; row <= address.last_row; row += rows_per_window) {
		ExcelAddress w = address;
		w.first_row = row;
		w.last_row = std::min(address.last_row, row + rows_per_window - 1);
		windows.push_back(w);
	}
	return windows;
}

ExcelNumberKind ClassifyExcelNumberFormat(const std::string &format) {
	const std::string f = Lower(format);
	// Date/time tokens in order; 'm' is resolved to month or minute afterwards.
	enum Tok { Y, D, H, S, M };
	std::vector<Tok> toks;
	bool ampm = false;
	for (size_t i = 0; i < f.size(); i++) {
		const char c = f[i];
		if (c == ';') {
			break; // only the first (positive) section
		}
		if (c == '"') {
			const auto close = f.find('"', i + 1);
			if (close == std::string::npos) {
				break;
			}
			i = close;
			continue;
		}
		if (c == '\\' || c == '_' || c == '*') {
			i++; // escaped literal, width-of-char, fill char
			continue;
		}
		if (c == '[') {
			const auto close = f.find(']', i + 1);
			if (close == std::string::npos) {
				break;
			}
			const std::string inner = f.substr(i + 1, close - i - 1);
			// [h], [mm], [ss]: elapsed time. Anything else ([Red], [$-407]) is
			// a colour, locale or condition.
			if (!inner.empty() && inner.find_first_not_of(inner[0]) == std::string::npos &&
			    (inner[0] == 'h' || inner[0] == 'm' || inner[0] == 's')) {
				toks.push_back(inner[0] == 'h' ? H : inner[0] == 's' ? S : M);
			}
			i = close;
			continue;
		}
		if (f.compare(i, 7, "general") == 0) {
			i += 6;
			continue;
		}
		if (f.compare(i, 5, "am/pm") == 0) {
			ampm = true;
			i += 4;
			continue;
		}
		if (f.compare(i, 3, "a/p") == 0) {
			ampm = true;
			i += 2;
			continue;
		}
		Tok t;
		switch (c) {
		case 'y':
			t = Y;
			break;
		case 'd':
			t = D;
			break;
		case 'h':
			t = H;
			break;
		case 's':
			t = S;
			break;
		case 'm':
			t = M;
			break;
		default:
			continue;
		}
		// A run of one letter ("yyyy", "mm") is one token.
		while (i + 1 < f.size() && f[i + 1] == c) {
			i++;
		}
		toks.push_back(t);
	}
	bool date = false, time = ampm;
	for (size_t i = 0; i < toks.size(); i++) {
		switch (toks[i]) {
		case Y:
		case D:
			date = true;
			break;
		case H:
		case S:
			time = true;
			break;
		case M: {
			const bool after_h = i > 0 && toks[i - 1] == H;
			const bool before_s = i + 1 < toks.size() && toks[i + 1] == S;
			if (after_h || before_s) {
				time = true;
			} else {
				date = true;
			}
			break;
		}
		}
	}
	if (date && time) {
		return ExcelNumberKind::DATETIME;
	}
	if (date) {
		return ExcelNumberKind::DATE;
	}
	if (time) {
		return ExcelNumberKind::TIME;
	}
	return ExcelNumberKind::NUMBER;
}

void ExcelSerialToEpoch(double serial, int64_t &days_since_epoch, int64_t &micros_of_day) {
	constexpr int64_t kMillisPerDay = 86400000;
	double day = std::floor(serial);
	int64_t millis = static_cast<int64_t>(std::llround((serial - day) * kMillisPerDay));
	if (millis >= kMillisPerDay) {
		millis -= kMillisPerDay;
		day += 1;
	}
	const auto d = static_cast<int64_t>(day);
	// Serial 25569 is 1970-01-01 counting from 1899-12-30 -- right from
	// 1900-03-01 (serial 61) on. Below that, Lotus' phantom 1900-02-29 has not
	// happened yet, so serials 1..60 count from 1899-12-31. Serial 0 (a time
	// alone) stays on 1899-12-30, Excel's day 0.
	days_since_epoch = (d >= 1 && d < 61) ? d - 25568 : d - 25569;
	micros_of_day = millis * 1000;
}

double ExcelSerialFromEpoch(int64_t days_since_epoch, int64_t micros_of_day) {
	// Inverse of ExcelSerialToEpoch: serials 1..59 are one day "later".
	const bool before_phantom = days_since_epoch >= -25567 && days_since_epoch <= -25509;
	const auto serial_day = static_cast<double>(days_since_epoch + (before_phantom ? 25568 : 25569));
	return serial_day + static_cast<double>(micros_of_day) / 86400000000.0;
}

std::string ExcelTextInput(const std::string &text) {
	return text.empty() ? text : "'" + text;
}

bool ExcelBlockAt(const ExcelAddress &anchor, int64_t rows, int64_t cols, ExcelAddress &block, std::string &error) {
	if (rows < 1 || cols < 1) {
		error = "nothing to write";
		return false;
	}
	block.sheet = anchor.sheet;
	block.first_row = anchor.HasCells() ? anchor.first_row : 1;
	block.first_col = anchor.HasCells() ? anchor.first_col : 1;
	block.last_row = block.first_row + rows - 1;
	block.last_col = block.first_col + cols - 1;
	if (block.last_row > kExcelMaxRows) {
		error = std::to_string(rows) + " rows from row " + std::to_string(block.first_row) +
		        " run past Excel's last row (" + std::to_string(kExcelMaxRows) + ")";
		return false;
	}
	if (block.last_col > kExcelMaxColumns) {
		error = std::to_string(cols) + " columns from column " + ExcelColumnLetters(block.first_col) +
		        " run past Excel's last column (XFD)";
		return false;
	}
	return true;
}

bool ExcelClearRegion(const ExcelAddress &anchor, const ExcelAddress &used, ExcelAddress &region) {
	region.sheet = anchor.sheet;
	region.first_row = anchor.HasCells() ? anchor.first_row : 1;
	region.first_col = anchor.HasCells() ? anchor.first_col : 1;
	region.last_row = used.last_row;
	region.last_col = used.last_col;
	return used.HasCells() && region.last_row >= region.first_row && region.last_col >= region.first_col;
}

ExcelColumnType InferExcelColumnType(const std::vector<const ExcelCell *> &cells) {
	bool any_string = false, any_bool = false, any_number = false;
	bool plain = false, date = false, datetime = false, time = false, all_integral = true;
	for (auto *cell : cells) {
		switch (cell->kind) {
		case ExcelCellKind::EMPTY:
		case ExcelCellKind::ERROR:
			break;
		case ExcelCellKind::STRING:
			any_string = true;
			break;
		case ExcelCellKind::BOOLEAN:
			any_bool = true;
			break;
		case ExcelCellKind::NUMBER:
			any_number = true;
			switch (ClassifyExcelNumberFormat(cell->format)) {
			case ExcelNumberKind::NUMBER:
				plain = true;
				all_integral = all_integral && IsIntegral(cell->number);
				break;
			case ExcelNumberKind::DATE:
				date = true;
				break;
			case ExcelNumberKind::DATETIME:
				datetime = true;
				break;
			case ExcelNumberKind::TIME:
				time = true;
				break;
			}
			break;
		}
	}
	if (any_string || (any_bool && any_number) || (!any_bool && !any_number)) {
		return ExcelColumnType::VARCHAR;
	}
	if (any_bool) {
		return ExcelColumnType::BOOLEAN;
	}
	if (plain) {
		return (date || datetime || time) ? ExcelColumnType::DOUBLE
		       : all_integral             ? ExcelColumnType::BIGINT
		                                  : ExcelColumnType::DOUBLE;
	}
	if (datetime || (date && time)) {
		return ExcelColumnType::TIMESTAMP;
	}
	return date ? ExcelColumnType::DATE : ExcelColumnType::TIME;
}

std::string ExcelCellText(const ExcelCell &cell) {
	switch (cell.kind) {
	case ExcelCellKind::EMPTY:
		return std::string();
	case ExcelCellKind::STRING:
	case ExcelCellKind::ERROR:
		return cell.text;
	case ExcelCellKind::BOOLEAN:
		return cell.boolean ? "TRUE" : "FALSE";
	case ExcelCellKind::NUMBER:
		break;
	}
	char buf[64];
	const auto kind = ClassifyExcelNumberFormat(cell.format);
	if (kind != ExcelNumberKind::NUMBER && std::isfinite(cell.number)) {
		int64_t days, micros;
		ExcelSerialToEpoch(cell.number, days, micros);
		const int64_t secs = micros / 1000000;
		char time[16];
		std::snprintf(time, sizeof(time), "%02d:%02d:%02d", static_cast<int>(secs / 3600),
		              static_cast<int>(secs / 60 % 60), static_cast<int>(secs % 60));
		if (kind == ExcelNumberKind::TIME) {
			return time;
		}
		int64_t y;
		int m, d;
		CivilFromDays(days, y, m, d);
		std::snprintf(buf, sizeof(buf), "%04lld-%02d-%02d", static_cast<long long>(y), m, d);
		return kind == ExcelNumberKind::DATE ? std::string(buf) : std::string(buf) + " " + time;
	}
	if (IsIntegral(cell.number) && std::fabs(cell.number) < 1e15) {
		std::snprintf(buf, sizeof(buf), "%lld", static_cast<long long>(cell.number));
	} else {
		std::snprintf(buf, sizeof(buf), "%.15g", cell.number);
	}
	return buf;
}

std::vector<std::string> ExcelHeaderNames(const std::vector<ExcelCell> &header) {
	std::vector<std::string> names;
	std::set<std::string> used; // lower-cased
	for (size_t i = 0; i < header.size(); i++) {
		std::string base = Trim(ExcelCellText(header[i]));
		if (base.empty()) {
			base = "column" + std::to_string(i + 1);
		}
		std::string name = base;
		for (int n = 2; used.count(Lower(name)); n++) {
			name = base + "_" + std::to_string(n);
		}
		used.insert(Lower(name));
		names.push_back(name);
	}
	return names;
}

} // namespace sharepoint
} // namespace duckdb
