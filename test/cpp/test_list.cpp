// SharePoint list logic behind read_sharepoint_list(): which columns a list
// yields, their names and types, and Graph's timestamps. Pure -- the Graph
// calls are proven by the live harness.
#include "sharepoint_list.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <limits>
#include <string>
#include <vector>

using namespace duckdb::sharepoint;

namespace {

ListColumnDef Col(const std::string &name, const std::string &display, ListFacet facet, bool hidden = false,
                  bool read_only = false) {
	ListColumnDef d;
	d.name = name;
	d.display_name = display;
	d.facet = facet;
	d.hidden = hidden;
	d.read_only = read_only;
	return d;
}

std::string Describe(const std::vector<ListOutputColumn> &cols) {
	std::string out;
	for (auto &c : cols) {
		static const char *types[] = {"VARCHAR", "VARCHAR[]", "DOUBLE", "DECIMAL", "BOOLEAN", "TIMESTAMP", "BIGINT", "BIGINT[]", "DATE"};
		out += c.name + ":" + types[static_cast<int>(c.type)] + "<-" + c.field + (c.may_be_multi ? "*" : "") + " ";
	}
	return out;
}

//! Created By / Modified By, after created and modified on every list.
const std::string kPeople = "created_by:VARCHAR<-AuthorLookupId created_by_email:VARCHAR<-AuthorLookupId "
                            "created_by_id:BIGINT<-AuthorLookupId modified_by:VARCHAR<-EditorLookupId "
                            "modified_by_email:VARCHAR<-EditorLookupId modified_by_id:BIGINT<-EditorLookupId ";

} // namespace

TEST_CASE("PlanListColumns: system columns kept, plumbing left out", "[list]") {
	std::vector<ListColumnDef> defs = {
	    Col("Title", "Title", ListFacet::TEXT),
	    Col("ContentType", "Content Type", ListFacet::OTHER),
	    Col("Attachments", "Attachments", ListFacet::BOOLEAN),
	    Col("Created", "Created", ListFacet::DATETIME, false, true),
	    Col("Modified", "Modified", ListFacet::DATETIME, false, true),
	    Col("Author", "Created By", ListFacet::PERSON, false, true),
	    Col("_UIVersionString", "Version", ListFacet::TEXT, false, true),
	    Col("Hidden1", "Hidden", ListFacet::TEXT, true),
	    Col("Amount", "Amount", ListFacet::NUMBER),
	};
	REQUIRE(Describe(PlanListColumns(defs)) ==
	        "id:BIGINT<-id Title:VARCHAR<-Title Amount:DOUBLE<-Amount created:TIMESTAMP<-Created "
	        "modified:TIMESTAMP<-Modified " +
	            kPeople);
}

TEST_CASE("PlanListColumns: one type per facet", "[list]") {
	auto calc = Col("Margin", "Margin", ListFacet::CALCULATED);
	calc.calculated_output = ListFacet::NUMBER;
	std::vector<ListColumnDef> defs = {
	    Col("Price", "Price", ListFacet::CURRENCY),  Col("Done", "Done", ListFacet::BOOLEAN),
	    Col("Due", "Due Date", ListFacet::DATETIME), Col("Status", "Status", ListFacet::CHOICE),
	    Col("Link", "Link", ListFacet::HYPERLINK),   calc,
	    Col("Geo", "Where", ListFacet::OTHER),
	};
	REQUIRE(Describe(PlanListColumns(defs)) ==
	        "id:BIGINT<-id Price:DECIMAL<-Price Done:BOOLEAN<-Done Due Date:TIMESTAMP<-Due Status:VARCHAR<-Status* "
	        "Link:VARCHAR<-Link Margin:DOUBLE<-Margin Where:VARCHAR<-Geo created:TIMESTAMP<-Created "
	        "modified:TIMESTAMP<-Modified " +
	            kPeople);
}

TEST_CASE("PlanListColumns: lookups and people by id", "[list]") {
	std::vector<ListColumnDef> defs = {
	    Col("Customer", "Customer", ListFacet::LOOKUP),
	    Col("Owner", "Owner", ListFacet::PERSON),
	};
	REQUIRE(Describe(PlanListColumns(defs)) ==
	        "id:BIGINT<-id Customer:VARCHAR<-Customer Customer_id:BIGINT<-CustomerLookupId "
	        "Owner:VARCHAR<-OwnerLookupId Owner_email:VARCHAR<-OwnerLookupId Owner_id:BIGINT<-OwnerLookupId "
	        "created:TIMESTAMP<-Created modified:TIMESTAMP<-Modified " +
	            kPeople);
}

TEST_CASE("PlanListColumns: a person is name, email and id; several people are lists", "[list][people]") {
	auto team = Col("Team", "Team", ListFacet::PERSON);
	team.multi = true;
	auto owner = Col("Owner", "Owner", ListFacet::PERSON);
	owner.indexed = true;
	auto plan = PlanListColumns({owner, team, Col("created_by", "created_by", ListFacet::TEXT)});
	REQUIRE(Describe(plan) == "id:BIGINT<-id Owner:VARCHAR<-OwnerLookupId Owner_email:VARCHAR<-OwnerLookupId "
	                          "Owner_id:BIGINT<-OwnerLookupId Team:VARCHAR[]<-TeamLookupId "
	                          "Team_email:VARCHAR[]<-TeamLookupId Team_id:BIGINT[]<-TeamLookupId "
	                          "created_by_2:VARCHAR<-created_by created:TIMESTAMP<-Created "
	                          "modified:TIMESTAMP<-Modified " +
	                              kPeople);
	REQUIRE(plan[1].person == ListPersonPart::NAME);
	REQUIRE(plan[1].person_field == "Owner");
	REQUIRE(plan[2].person == ListPersonPart::EMAIL);
	REQUIRE(plan[3].person == ListPersonPart::ID);
	REQUIRE(plan[3].filter == ListFilterKind::NUMBER); // an indexed single person, by id
	REQUIRE(plan[6].filter == ListFilterKind::NONE);   // several people: not pushed down

	// Written back as read: names, emails and the system people are skipped,
	// a person is set through its _id.
	std::vector<ListWriteColumn> out;
	std::string error;
	REQUIRE(PlanListWrite({"Owner", "Owner_email", "Owner_id", "created_by", "modified_by_id"}, plan, out, error));
	REQUIRE(out[0].target == ListWriteTarget::SKIP);
	REQUIRE(out[1].target == ListWriteTarget::SKIP);
	REQUIRE((out[2].target == ListWriteTarget::FIELD && out[2].field == "OwnerLookupId"));
	REQUIRE(out[3].target == ListWriteTarget::SKIP);
	REQUIRE(out[4].target == ListWriteTarget::SKIP);
}

TEST_CASE("PlanListColumns: names unique case-insensitively", "[list]") {
	std::vector<ListColumnDef> defs = {
	    Col("Title", "Title", ListFacet::TEXT),
	    Col("Title0", "title", ListFacet::TEXT),
	    Col("ID0", "ID", ListFacet::TEXT),           // collides with the system id
	    Col("Created0", "Created", ListFacet::TEXT), // and with created (system names win)
	};
	REQUIRE(Describe(PlanListColumns(defs)) ==
	        "id:BIGINT<-id Title:VARCHAR<-Title title_2:VARCHAR<-Title0 ID_2:VARCHAR<-ID0 "
	        "Created_2:VARCHAR<-Created0 created:TIMESTAMP<-Created modified:TIMESTAMP<-Modified " +
	            kPeople);
}

TEST_CASE("ParseIsoTimestamp", "[list]") {
	int64_t us = 0;
	REQUIRE(ParseIsoTimestamp("1970-01-01T00:00:00Z", us));
	REQUIRE(us == 0);
	REQUIRE(ParseIsoTimestamp("2026-01-31T08:15:00Z", us));
	REQUIRE(us == 1769847300LL * 1000000);
	REQUIRE(ParseIsoTimestamp("2026-01-31T08:15:00.25Z", us));
	REQUIRE(us == 1769847300LL * 1000000 + 250000);
	REQUIRE(ParseIsoTimestamp("2026-01-31T10:15:00+02:00", us)); // offsets normalised to UTC
	REQUIRE(us == 1769847300LL * 1000000);
	REQUIRE(ParseIsoTimestamp("1900-03-01T00:00:00Z", us));
	REQUIRE(us == -2203891200LL * 1000000);
	REQUIRE_FALSE(ParseIsoTimestamp("", us));
	REQUIRE_FALSE(ParseIsoTimestamp("2026-01-31", us));
	REQUIRE_FALSE(ParseIsoTimestamp("2026-13-01T00:00:00Z", us));
	REQUIRE_FALSE(ParseIsoTimestamp("yesterday", us));
}

TEST_CASE("FormatIsoTimestamp round-trips ParseIsoTimestamp", "[list][write]") {
	REQUIRE(FormatIsoTimestamp(0) == "1970-01-01T00:00:00Z");
	REQUIRE(FormatIsoTimestamp(1769847300LL * 1000000) == "2026-01-31T08:15:00Z");
	REQUIRE(FormatIsoTimestamp(1769847300LL * 1000000 + 250000) == "2026-01-31T08:15:00.250000Z");
	REQUIRE(FormatIsoTimestamp(-2203891200LL * 1000000) == "1900-03-01T00:00:00Z");
	REQUIRE(FormatIsoTimestamp(-1) == "1969-12-31T23:59:59.999999Z");
	for (int64_t us : {0LL, 1769847300250000LL, -2203891200000000LL, 253402300799000000LL}) {
		int64_t back = 0;
		REQUIRE(ParseIsoTimestamp(FormatIsoTimestamp(us), back));
		REQUIRE(back == us);
	}
}

TEST_CASE("PlanListWrite: COPY columns onto list fields", "[list][write]") {
	auto plan = PlanListColumns({Col("Title", "Title", ListFacet::TEXT), Col("Amount0", "Amount", ListFacet::NUMBER),
	                             Col("Customer", "Customer", ListFacet::LOOKUP)});
	std::vector<ListWriteColumn> out;
	std::string error;
	// A read_sharepoint_list result written back as-is.
	REQUIRE(
	    PlanListWrite({"id", "Title", "amount", "Customer", "Customer_id", "created", "modified"}, plan, out, error));
	REQUIRE(out.size() == 7);
	REQUIRE(out[0].target == ListWriteTarget::ID);
	REQUIRE((out[1].target == ListWriteTarget::FIELD && out[1].field == "Title"));
	REQUIRE((out[2].target == ListWriteTarget::FIELD && out[2].field == "Amount0")); // by display name, any case
	REQUIRE(out[3].target == ListWriteTarget::SKIP);                                 // lookup value: via _id
	REQUIRE((out[4].target == ListWriteTarget::FIELD && out[4].field == "CustomerLookupId"));
	REQUIRE(out[5].target == ListWriteTarget::SKIP);
	REQUIRE(out[6].target == ListWriteTarget::SKIP);

	// The internal name works too.
	REQUIRE(PlanListWrite({"Amount0"}, plan, out, error));
	REQUIRE(out[0].field == "Amount0");

	REQUIRE_FALSE(PlanListWrite({"Title", "Nope"}, plan, out, error));
	REQUIRE(error.find("\"Nope\"") != std::string::npos);
	REQUIRE(error.find("Amount") != std::string::npos); // lists what there is
}

TEST_CASE("PlanListColumns: built-in Title keeps its name in any site language", "[list]") {
	// A German site shows Title as "Titel"; queries must not depend on that.
	auto plan = PlanListColumns({Col("Title", "Titel", ListFacet::TEXT), Col("Betrag", "Betrag", ListFacet::NUMBER)});
	REQUIRE(plan[1].name == "Title");
	REQUIRE(plan[1].field == "Title");
	REQUIRE(plan[2].name == "Betrag"); // user columns keep their display name
}

// ---------------------------------------------------------------------------
// WHERE pushdown (#61): which columns Graph may filter on, and the $filter
// text. DuckDB keeps every predicate, so a pushed clause only has to be never
// stricter than the SQL it came from.
// ---------------------------------------------------------------------------
namespace {

ListColumnDef Indexed(ListColumnDef d) {
	d.indexed = true;
	return d;
}

ListPredicate Pred(const std::string &field, ListFilterKind kind, ListFilterOp op) {
	ListPredicate p;
	p.field = field;
	p.kind = kind;
	p.op = op;
	return p;
}

} // namespace

TEST_CASE("PlanListColumns: only indexed columns of a safe kind are filterable", "[list][pushdown]") {
	std::vector<ListColumnDef> defs = {
	    Indexed(Col("Title", "Title", ListFacet::TEXT)),
	    Col("Note", "Note", ListFacet::TEXT), // not indexed
	    Indexed(Col("Amount", "Amount", ListFacet::NUMBER)),
	    Indexed(Col("Price", "Price", ListFacet::CURRENCY)),
	    Indexed(Col("Due", "Due", ListFacet::DATETIME)),
	    Indexed(Col("Done", "Done", ListFacet::BOOLEAN)),
	    Indexed(Col("Customer", "Customer", ListFacet::LOOKUP)),
	    Indexed(Col("Calc", "Calc", ListFacet::CALCULATED)),
	    Indexed(Col("Modified", "Modified", ListFacet::DATETIME, false, true)),
	    Col("Created", "Created", ListFacet::DATETIME, false, true),
	};
	std::string out;
	for (auto &c : PlanListColumns(defs)) {
		static const char *kinds[] = {"-", "text", "number", "timestamp"};
		out += c.name + "=" + kinds[static_cast<int>(c.filter)] + " ";
	}
	REQUIRE(out == "id=- Title=text Note=- Amount=number Price=number Due=timestamp Done=- Customer=- "
	               "Customer_id=number Calc=- created=- modified=timestamp created_by=- created_by_email=- "
	               "created_by_id=- modified_by=- modified_by_email=- modified_by_id=- ");
}

TEST_CASE("ODataClause: text equality, IN and prefix; quotes doubled", "[list][pushdown]") {
	auto eq = Pred("Status", ListFilterKind::TEXT, ListFilterOp::EQ);
	eq.texts = {"it's"};
	REQUIRE(ODataClause(eq) == "fields/Status eq 'it''s'");
	auto in = Pred("Status", ListFilterKind::TEXT, ListFilterOp::IN);
	in.texts = {"a", "b"};
	REQUIRE(ODataClause(in) == "(fields/Status eq 'a' or fields/Status eq 'b')");
	auto prefix = Pred("Title", ListFilterKind::TEXT, ListFilterOp::PREFIX);
	prefix.texts = {"ord"};
	REQUIRE(ODataClause(prefix) == "startswith(fields/Title,'ord')");
	// SharePoint compares text case-insensitively and in its own collation:
	// a range or an empty prefix is not pushed.
	auto range = Pred("Title", ListFilterKind::TEXT, ListFilterOp::GT);
	range.texts = {"m"};
	REQUIRE(ODataClause(range).empty());
	auto empty_prefix = Pred("Title", ListFilterKind::TEXT, ListFilterOp::PREFIX);
	empty_prefix.texts = {""};
	REQUIRE(ODataClause(empty_prefix).empty());
}

TEST_CASE("ODataClause: number ranges are made inclusive", "[list][pushdown]") {
	auto gt = Pred("Amount", ListFilterKind::NUMBER, ListFilterOp::GT);
	gt.numbers = {5};
	REQUIRE(ODataClause(gt) == "fields/Amount ge 5");
	auto lt = Pred("Amount", ListFilterKind::NUMBER, ListFilterOp::LT);
	lt.numbers = {0.1};
	REQUIRE(ODataClause(lt) == "fields/Amount le 0.1");
	auto in = Pred("CustomerLookupId", ListFilterKind::NUMBER, ListFilterOp::IN);
	in.numbers = {1, 3};
	REQUIRE(ODataClause(in) == "(fields/CustomerLookupId eq 1 or fields/CustomerLookupId eq 3)");
	auto neg = Pred("Amount", ListFilterKind::NUMBER, ListFilterOp::EQ);
	neg.numbers = {-2.5};
	REQUIRE(ODataClause(neg) == "fields/Amount eq -2.5");
	auto nan = Pred("Amount", ListFilterKind::NUMBER, ListFilterOp::EQ);
	nan.numbers = {std::numeric_limits<double>::quiet_NaN()};
	REQUIRE(ODataClause(nan).empty());
}

TEST_CASE("ODataClause: timestamps as inclusive whole-second ranges only", "[list][pushdown]") {
	int64_t t;
	REQUIRE(ParseIsoTimestamp("2026-01-01T10:00:00.500000Z", t));
	auto ge = Pred("Modified", ListFilterKind::TIMESTAMP, ListFilterOp::GT);
	ge.micros = t;
	REQUIRE(ODataClause(ge) == "fields/Modified ge '2026-01-01T10:00:00Z'");
	auto le = Pred("Modified", ListFilterKind::TIMESTAMP, ListFilterOp::LT);
	le.micros = t;
	REQUIRE(ODataClause(le) == "fields/Modified le '2026-01-01T10:00:01Z'");
	auto eq = Pred("Modified", ListFilterKind::TIMESTAMP, ListFilterOp::EQ);
	eq.micros = t;
	REQUIRE(ODataClause(eq).empty());
}

TEST_CASE("BuildListFilter joins the pushable clauses with and", "[list][pushdown]") {
	auto a = Pred("Status", ListFilterKind::TEXT, ListFilterOp::EQ);
	a.texts = {"open"};
	auto skipped = Pred("Status", ListFilterKind::TEXT, ListFilterOp::LT);
	skipped.texts = {"x"};
	auto b = Pred("Amount", ListFilterKind::NUMBER, ListFilterOp::GE);
	b.numbers = {10};
	REQUIRE(BuildListFilter({a, skipped, b}) == "fields/Status eq 'open' and fields/Amount ge 10");
	REQUIRE(BuildListFilter({skipped}).empty());
	REQUIRE(BuildListFilter({}).empty());
}

TEST_CASE("PlanListColumns: a drop-down or radio choice is single-valued and filterable", "[list][pushdown]") {
	auto single = Indexed(Col("Status", "Status", ListFacet::CHOICE));
	single.single_choice = true;
	auto multi = Indexed(Col("Tags", "Tags", ListFacet::CHOICE)); // check boxes, or unknown
	auto plan = PlanListColumns({single, multi});
	REQUIRE(Describe(plan) == "id:BIGINT<-id Status:VARCHAR<-Status Tags:VARCHAR<-Tags* created:TIMESTAMP<-Created "
	                          "modified:TIMESTAMP<-Modified " +
	            kPeople);
	REQUIRE(plan[1].filter == ListFilterKind::TEXT);
	REQUIRE(plan[2].filter == ListFilterKind::NONE);
}

// ---------------------------------------------------------------------------
// Date-only columns (#64). Graph returns a date-only value as midnight in the
// site's time zone, in UTC: 2026-03-15 on a UTC+1 site is
// "2026-03-14T23:00:00Z". Graph v1.0 does not expose the site's time zone.
// ---------------------------------------------------------------------------
TEST_CASE("PlanListColumns: a date-only column is a DATE", "[list][date]") {
	auto due = Col("Due", "Due", ListFacet::DATETIME);
	due.date_only = true;
	due.indexed = true; // still not pushed down: Graph compares the site-local midnight
	auto calc = Col("Next", "Next", ListFacet::CALCULATED);
	calc.calculated_output = ListFacet::DATETIME;
	calc.date_only = true;
	auto plan = PlanListColumns({due, calc, Col("At", "At", ListFacet::DATETIME)});
	REQUIRE(Describe(plan) == "id:BIGINT<-id Due:DATE<-Due Next:DATE<-Next At:TIMESTAMP<-At "
	                          "created:TIMESTAMP<-Created modified:TIMESTAMP<-Modified " +
	                              kPeople);
	std::vector<ListWriteColumn> out;
	std::string error;
	REQUIRE(PlanListWrite({"Due", "At"}, plan, out, error));
	REQUIRE(plan[1].filter == ListFilterKind::NONE);
	REQUIRE(out[0].date_only);
	REQUIRE_FALSE(out[1].date_only);
}

TEST_CASE("DateOnlyDays: the site-local date of Graph's UTC midnight", "[list][date]") {
	auto days = [](const char *iso) {
		int64_t us;
		REQUIRE(ParseIsoTimestamp(iso, us));
		return FormatIsoTimestamp(int64_t(DateOnlyDays(us)) * 86400 * 1000000).substr(0, 10);
	};
	REQUIRE(days("2026-03-15T00:00:00Z") == "2026-03-15"); // UTC
	REQUIRE(days("2026-03-14T23:00:00Z") == "2026-03-15"); // UTC+1 (Berlin)
	REQUIRE(days("2026-03-14T11:00:00Z") == "2026-03-15"); // UTC+13 (Auckland, summer)
	REQUIRE(days("2026-03-14T13:30:00Z") == "2026-03-15"); // UTC+10:30
	REQUIRE(days("2026-03-15T05:00:00Z") == "2026-03-15"); // UTC-5 (New York)
	REQUIRE(days("2026-03-15T10:00:00Z") == "2026-03-15"); // UTC-10 (Hawaii)
	REQUIRE(days("1969-12-31T23:00:00Z") == "1970-01-01"); // before the epoch
}

TEST_CASE("FormatDateOnly: an instant on that date from UTC-10 to UTC+13:30", "[list][date][write]") {
	REQUIRE(FormatDateOnly(0) == "1970-01-01T10:15:00Z");
	int64_t us;
	REQUIRE(ParseIsoTimestamp("2026-03-15T00:00:00Z", us));
	const auto d = static_cast<int32_t>(us / 86400 / 1000000);
	REQUIRE(FormatDateOnly(d) == "2026-03-15T10:15:00Z");
	// Read back as the same date, whatever Graph does with it in between.
	REQUIRE(ParseIsoTimestamp(FormatDateOnly(d), us));
	REQUIRE(DateOnlyDays(us) == d);
}
