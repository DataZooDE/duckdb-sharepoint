// SharePoint list logic behind read_sharepoint_list(): which columns a list
// yields, their names and types, and Graph's timestamps. Pure -- the Graph
// calls are proven by the live harness.
#include "sharepoint_list.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
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
		static const char *types[] = {"VARCHAR", "VARCHAR[]", "DOUBLE", "DECIMAL", "BOOLEAN", "TIMESTAMP", "BIGINT"};
		out += c.name + ":" + types[static_cast<int>(c.type)] + "<-" + c.field + (c.may_be_multi ? "*" : "") + " ";
	}
	return out;
}

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
	        "modified:TIMESTAMP<-Modified ");
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
	        "modified:TIMESTAMP<-Modified ");
}

TEST_CASE("PlanListColumns: lookups and people by id", "[list]") {
	std::vector<ListColumnDef> defs = {
	    Col("Customer", "Customer", ListFacet::LOOKUP),
	    Col("Owner", "Owner", ListFacet::PERSON),
	};
	REQUIRE(Describe(PlanListColumns(defs)) ==
	        "id:BIGINT<-id Customer:VARCHAR<-Customer Customer_id:BIGINT<-CustomerLookupId "
	        "Owner_id:BIGINT<-OwnerLookupId created:TIMESTAMP<-Created modified:TIMESTAMP<-Modified ");
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
	        "Created_2:VARCHAR<-Created0 created:TIMESTAMP<-Created modified:TIMESTAMP<-Modified ");
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
