// SharePoint list logic for read_sharepoint_list(). Pure: no duckdb.hpp and
// no I/O (see sharepoint_list.hpp).
#include "sharepoint_list.hpp"

#include <cctype>
#include <cstdio>
#include <cmath>
#include <cstdint>
#include <cstdlib>
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

ListFilterKind FilterKindFor(ListFacet facet) {
	switch (facet) {
	case ListFacet::TEXT:
	case ListFacet::CHOICE:
		return ListFilterKind::TEXT;
	case ListFacet::NUMBER:
	case ListFacet::CURRENCY:
		return ListFilterKind::NUMBER;
	case ListFacet::DATETIME:
		return ListFilterKind::TIMESTAMP;
	default:
		return ListFilterKind::NONE;
	}
}

std::string ODataText(const std::string &text) {
	std::string out = "'";
	for (char c : text) {
		out += c;
		if (c == '\'') {
			out += '\'';
		}
	}
	return out + "'";
}

//! The shortest decimal that reads back as `v`; "" for NaN and infinities.
std::string ODataNumber(double v) {
	if (!std::isfinite(v)) {
		return "";
	}
	char buf[40];
	for (int precision = 15; precision <= 17; precision++) {
		std::snprintf(buf, sizeof(buf), "%.*g", precision, v);
		if (std::strtod(buf, nullptr) == v) {
			break;
		}
	}
	return buf;
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
	std::set<std::string> used = {"id",         "created",          "modified",      "created_by",
	                              "created_by_email", "created_by_id", "modified_by", "modified_by_email",
	                              "modified_by_id"};
	std::vector<ListOutputColumn> out;
	auto person = [&](const std::string &base, const std::string &field, bool multi, bool indexed, bool claim) {
		static const char *suffix[] = {"", "_email", "_id"};
		static const ListPersonPart parts[] = {ListPersonPart::NAME, ListPersonPart::EMAIL, ListPersonPart::ID};
		for (int i = 0; i < 3; i++) {
			ListOutputColumn col;
			col.name = claim ? Claim(used, base + suffix[i]) : base + suffix[i];
			col.field = field + "LookupId";
			col.person = parts[i];
			col.person_field = field;
			const bool id = parts[i] == ListPersonPart::ID;
			col.type = id ? (multi ? ListColumnType::BIGINT_LIST : ListColumnType::BIGINT)
			              : (multi ? ListColumnType::VARCHAR_LIST : ListColumnType::VARCHAR);
			col.read_only = !id || !claim; // set a person through <name>_id; Graph sets Author/Editor
			col.filter = id && indexed && !multi ? ListFilterKind::NUMBER : ListFilterKind::NONE;
			out.push_back(col);
		}
	};
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
		if (def.facet == ListFacet::PERSON) {
			person(base, def.name, def.multi, def.indexed, true);
			continue;
		}
		if (def.facet == ListFacet::LOOKUP) {
			{
				ListOutputColumn value;
				value.name = Claim(used, base);
				value.field = def.name;
				value.lookup_value = true;
				value.read_only = true;
				out.push_back(value);
			}
			ListOutputColumn key;
			key.name = Claim(used, base + "_id");
			key.field = def.name + "LookupId";
			key.type = ListColumnType::BIGINT;
			key.filter = def.indexed ? ListFilterKind::NUMBER : ListFilterKind::NONE;
			out.push_back(key);
			continue;
		}
		ListOutputColumn col;
		col.name = Claim(used, base);
		col.field = def.name;
		col.type = TypeFor(def.facet == ListFacet::CALCULATED ? def.calculated_output : def.facet);
		col.may_be_multi = def.facet == ListFacet::CHOICE && !def.single_choice;
		col.filter = def.indexed && !col.may_be_multi ? FilterKindFor(def.facet) : ListFilterKind::NONE;
		out.push_back(col);
	}
	// Created and Modified are read-only, so skipped above; they can be indexed.
	auto system_column = [&](const std::string &name, const std::string &field) {
		ListOutputColumn col;
		col.name = name;
		col.field = field;
		col.type = ListColumnType::TIMESTAMP;
		for (auto &def : defs) {
			if (def.name == field && def.indexed) {
				col.filter = ListFilterKind::TIMESTAMP;
			}
		}
		out.push_back(col);
	};
	system_column("created", "Created");
	system_column("modified", "Modified");
	person("created_by", "Author", false, false, false);
	person("modified_by", "Editor", false, false, false);
	return out;
}

std::string FormatIsoTimestamp(int64_t micros) {
	int64_t seconds = micros / 1000000;
	int64_t frac = micros % 1000000;
	if (frac < 0) {
		frac += 1000000;
		seconds--;
	}
	int64_t days = seconds / 86400;
	int64_t rest = seconds % 86400;
	if (rest < 0) {
		rest += 86400;
		days--;
	}
	// Civil from days (Howard Hinnant).
	int64_t z = days + 719468;
	const int64_t era = (z >= 0 ? z : z - 146096) / 146097;
	const int64_t doe = z - era * 146097;
	const int64_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
	const int64_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
	const int64_t mp = (5 * doy + 2) / 153;
	const int64_t d = doy - (153 * mp + 2) / 5 + 1;
	const int64_t m = mp < 10 ? mp + 3 : mp - 9;
	const int64_t y = yoe + era * 400 + (m <= 2 ? 1 : 0);
	char buf[48];
	std::snprintf(buf, sizeof(buf), "%04lld-%02lld-%02lldT%02lld:%02lld:%02lld", static_cast<long long>(y),
	              static_cast<long long>(m), static_cast<long long>(d), static_cast<long long>(rest / 3600),
	              static_cast<long long>(rest / 60 % 60), static_cast<long long>(rest % 60));
	std::string out = buf;
	if (frac) {
		std::snprintf(buf, sizeof(buf), ".%06lld", static_cast<long long>(frac));
		out += buf;
	}
	return out + "Z";
}

std::string ODataClause(const ListPredicate &p) {
	const std::string field = "fields/" + p.field;
	switch (p.kind) {
	case ListFilterKind::TEXT:
		if ((p.op == ListFilterOp::EQ || p.op == ListFilterOp::PREFIX) && p.texts.size() != 1) {
			return "";
		}
		if (p.op == ListFilterOp::EQ) {
			return field + " eq " + ODataText(p.texts[0]);
		}
		if (p.op == ListFilterOp::PREFIX) {
			return p.texts[0].empty() ? "" : "startswith(" + field + "," + ODataText(p.texts[0]) + ")";
		}
		if (p.op == ListFilterOp::IN && !p.texts.empty()) {
			std::string out;
			for (auto &t : p.texts) {
				out += (out.empty() ? "(" : " or ") + field + " eq " + ODataText(t);
			}
			return out + ")";
		}
		return "";
	case ListFilterKind::NUMBER: {
		if (p.numbers.empty() || (p.op != ListFilterOp::IN && p.numbers.size() != 1)) {
			return "";
		}
		std::vector<std::string> literals;
		for (auto v : p.numbers) {
			literals.push_back(ODataNumber(v));
			if (literals.back().empty()) {
				return "";
			}
		}
		switch (p.op) {
		case ListFilterOp::EQ:
			return field + " eq " + literals[0];
		case ListFilterOp::LT:
		case ListFilterOp::LE:
			return field + " le " + literals[0];
		case ListFilterOp::GT:
		case ListFilterOp::GE:
			return field + " ge " + literals[0];
		case ListFilterOp::IN: {
			std::string out;
			for (auto &l : literals) {
				out += (out.empty() ? "(" : " or ") + field + " eq " + l;
			}
			return out + ")";
		}
		default:
			return "";
		}
	}
	case ListFilterKind::TIMESTAMP: {
		// SharePoint keeps whole seconds: widen to them, floor below, ceil above.
		constexpr int64_t kSecond = 1000000;
		int64_t floor = p.micros / kSecond * kSecond;
		if (floor > p.micros) {
			floor -= kSecond;
		}
		const int64_t ceil = floor == p.micros ? floor : floor + kSecond;
		switch (p.op) {
		case ListFilterOp::LT:
		case ListFilterOp::LE:
			return field + " le '" + FormatIsoTimestamp(ceil) + "'";
		case ListFilterOp::GT:
		case ListFilterOp::GE:
			return field + " ge '" + FormatIsoTimestamp(floor) + "'";
		default:
			return "";
		}
	}
	default:
		return "";
	}
}

std::string BuildListFilter(const std::vector<ListPredicate> &predicates) {
	std::string out;
	for (auto &p : predicates) {
		auto clause = ODataClause(p);
		if (!clause.empty()) {
			out += (out.empty() ? "" : " and ") + clause;
		}
	}
	return out;
}

bool PlanListWrite(const std::vector<std::string> &copy_names, const std::vector<ListOutputColumn> &plan,
                   std::vector<ListWriteColumn> &out, std::string &error) {
	out.clear();
	for (auto &name : copy_names) {
		const auto lname = Lower(name);
		const ListOutputColumn *match = nullptr;
		for (auto &col : plan) {
			if (Lower(col.name) == lname) {
				match = &col;
				break;
			}
		}
		if (!match) {
			for (auto &col : plan) {
				if (Lower(col.field) == lname && col.field != "id") {
					match = &col;
					break;
				}
			}
		}
		ListWriteColumn w;
		if (!match) {
			std::string have;
			for (auto &col : plan) {
				have += (have.empty() ? "" : ", ") + col.name;
			}
			error = "the list has no column \"" + name + "\" (it has: " + have + ")";
			return false;
		}
		if (match->field == "id") {
			w.target = ListWriteTarget::ID;
		} else if (match->read_only || match->name == "created" || match->name == "modified") {
			w.target = ListWriteTarget::SKIP;
		} else {
			w.target = ListWriteTarget::FIELD;
			w.field = match->field;
		}
		out.push_back(w);
	}
	return true;
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
