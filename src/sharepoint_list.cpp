// SharePoint list logic for read_sharepoint_list(). Pure: no duckdb.hpp and
// no I/O (see sharepoint_list.hpp).
#include "sharepoint_list.hpp"

#include <cctype>
#include <cstdint>
#include <set>

namespace duckdb {
namespace sharepoint {

namespace {

std::string Lower(std::string s) {
	for (auto &c : s) {
		c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
	}
	return s;
}

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

//! `base`, or base_2, base_3, ... -- the first not yet used (case-insensitive).
std::string Claim(std::set<std::string> &used, const std::string &base) {
	std::string name = base;
	for (int n = 2; used.count(Lower(name)); n++) {
		name = base + "_" + std::to_string(n);
	}
	used.insert(Lower(name));
	return name;
}

ListColumnType TypeFor(ListFacet facet) {
	switch (facet) {
	case ListFacet::NUMBER:
		return ListColumnType::DOUBLE;
	case ListFacet::CURRENCY:
		return ListColumnType::DECIMAL;
	case ListFacet::BOOLEAN:
		return ListColumnType::BOOLEAN;
	case ListFacet::DATETIME:
		return ListColumnType::TIMESTAMP;
	default:
		return ListColumnType::VARCHAR;
	}
}

// Days since 1970-01-01 for a proleptic Gregorian date (Howard Hinnant).
int64_t DaysFromCivil(int64_t y, int64_t m, int64_t d) {
	y -= m <= 2;
	const int64_t era = (y >= 0 ? y : y - 399) / 400;
	const int64_t yoe = y - era * 400;
	const int64_t doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
	const int64_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
	return era * 146097 + doe - 719468;
}

bool Digits(const std::string &s, size_t pos, size_t n, int64_t &out) {
	if (pos + n > s.size()) {
		return false;
	}
	out = 0;
	for (size_t i = pos; i < pos + n; i++) {
		if (!std::isdigit(static_cast<unsigned char>(s[i]))) {
			return false;
		}
		out = out * 10 + (s[i] - '0');
	}
	return true;
}

} // namespace

std::vector<ListOutputColumn> PlanListColumns(const std::vector<ListColumnDef> &defs) {
	// The system columns claim their names first: a user column called "ID"
	// or "Created" is the one that gets the suffix.
	std::set<std::string> used = {"id", "created", "modified"};
	std::vector<ListOutputColumn> out;
	ListOutputColumn id;
	id.name = "id";
	id.field = "id";
	id.type = ListColumnType::BIGINT;
	out.push_back(id);
	for (auto &def : defs) {
		if (def.hidden || def.read_only || def.name == "ContentType" || def.name == "Attachments") {
			continue;
		}
		// Built-in Title keeps its internal name: its display name is the
		// site's language ("Titel" on a German site, found live).
		std::string base = def.name == "Title" ? def.name : Trim(def.display_name);
		if (base.empty()) {
			base = def.name;
		}
		if (def.facet == ListFacet::PERSON || def.facet == ListFacet::LOOKUP) {
			if (def.facet == ListFacet::LOOKUP) {
				ListOutputColumn value;
				value.name = Claim(used, base);
				value.field = def.name;
				out.push_back(value);
			}
			ListOutputColumn key;
			key.name = Claim(used, base + "_id");
			key.field = def.name + "LookupId";
			key.type = ListColumnType::BIGINT;
			out.push_back(key);
			continue;
		}
		ListOutputColumn col;
		col.name = Claim(used, base);
		col.field = def.name;
		col.type = TypeFor(def.facet == ListFacet::CALCULATED ? def.calculated_output : def.facet);
		col.may_be_multi = def.facet == ListFacet::CHOICE;
		out.push_back(col);
	}
	ListOutputColumn created;
	created.name = "created";
	created.field = "Created";
	created.type = ListColumnType::TIMESTAMP;
	out.push_back(created);
	ListOutputColumn modified = created;
	modified.name = "modified";
	modified.field = "Modified";
	out.push_back(modified);
	return out;
}

bool ParseIsoTimestamp(const std::string &s, int64_t &micros) {
	// YYYY-MM-DDTHH:MM:SS[.fff...](Z|+hh:mm|-hh:mm)
	int64_t y, mo, d, h, mi, sec;
	if (!Digits(s, 0, 4, y) || s.size() < 19 || s[4] != '-' || !Digits(s, 5, 2, mo) || s[7] != '-' ||
	    !Digits(s, 8, 2, d) || (s[10] != 'T' && s[10] != ' ') || !Digits(s, 11, 2, h) || s[13] != ':' ||
	    !Digits(s, 14, 2, mi) || s[16] != ':' || !Digits(s, 17, 2, sec)) {
		return false;
	}
	if (mo < 1 || mo > 12 || d < 1 || d > 31 || h > 23 || mi > 59 || sec > 60) {
		return false;
	}
	size_t i = 19;
	int64_t frac = 0;
	if (i < s.size() && s[i] == '.') {
		int64_t scale = 100000;
		for (i++; i < s.size() && std::isdigit(static_cast<unsigned char>(s[i])); i++) {
			frac += (s[i] - '0') * scale;
			scale /= 10;
		}
	}
	int64_t offset_minutes = 0;
	if (i < s.size() && (s[i] == '+' || s[i] == '-')) {
		int64_t oh, om;
		if (!Digits(s, i + 1, 2, oh) || i + 3 >= s.size() || s[i + 3] != ':' || !Digits(s, i + 4, 2, om)) {
			return false;
		}
		offset_minutes = (oh * 60 + om) * (s[i] == '-' ? -1 : 1);
		i += 6;
	} else if (i < s.size() && s[i] == 'Z') {
		i++;
	}
	if (i != s.size()) {
		return false;
	}
	const int64_t seconds = DaysFromCivil(y, mo, d) * 86400 + h * 3600 + mi * 60 + sec - offset_minutes * 60;
	micros = seconds * 1000000 + frac;
	return true;
}

} // namespace sharepoint
} // namespace duckdb
