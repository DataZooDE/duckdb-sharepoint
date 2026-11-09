// COPY ... (FORMAT onenote) (#105): the pure parts -- the target, the column
// plan, and the HTML / JSON sent. Creating and appending to pages against
// Graph is the live delegated test.
#include "sharepoint_onenote_page.hpp"

#include <catch2/catch_test_macros.hpp>

#include <string>
#include <vector>

using namespace duckdb::sharepoint;

TEST_CASE("ParseOneNoteTarget: notebook/section", "[onenote]") {
	OneNoteTarget t;
	std::string error;
	REQUIRE(ParseOneNoteTarget("Projects/Q4", t, error));
	REQUIRE(t.notebook == "Projects");
	REQUIRE(t.section == "Q4");
	REQUIRE(ParseOneNoteTarget("/Team Notes/Status 2026/", t, error));
	REQUIRE(t.notebook == "Team Notes");
	REQUIRE(t.section == "Status 2026");

	REQUIRE_FALSE(ParseOneNoteTarget("Projects", t, error));
	REQUIRE(error.find("'<notebook>/<section>'") != std::string::npos);
	REQUIRE_FALSE(ParseOneNoteTarget("Projects/Group/Q4", t, error)); // section groups: not addressed
	REQUIRE_FALSE(ParseOneNoteTarget("Projects/", t, error));
	REQUIRE_FALSE(ParseOneNoteTarget("/Q4", t, error));
	REQUIRE_FALSE(ParseOneNoteTarget("", t, error));
}

TEST_CASE("PlanOneNoteWrite: content required, title and id optional", "[onenote]") {
	OneNoteWritePlan plan;
	std::string error;
	REQUIRE(PlanOneNoteWrite({"Title", "CONTENT"}, plan, error));
	REQUIRE(plan.title == 0);
	REQUIRE(plan.content == 1);
	REQUIRE(plan.id == -1);
	REQUIRE(PlanOneNoteWrite({"id", "content"}, plan, error));
	REQUIRE(plan.id == 0);
	REQUIRE(plan.title == -1);

	REQUIRE_FALSE(PlanOneNoteWrite({"title"}, plan, error));
	REQUIRE(error.find("content") != std::string::npos);
	REQUIRE_FALSE(PlanOneNoteWrite({"content", "body"}, plan, error));
	REQUIRE(error.find("\"body\"") != std::string::npos);
	REQUIRE_FALSE(PlanOneNoteWrite({"content", "Content"}, plan, error)); // twice
}

TEST_CASE("HtmlEscape", "[onenote]") {
	REQUIRE(HtmlEscape("a < b & \"c\" > 'd'") == "a &lt; b &amp; &quot;c&quot; &gt; &#39;d&#39;");
	REQUIRE(HtmlEscape("Grüße") == "Grüße"); // UTF-8 passes
}

TEST_CASE("OneNotePageDocument: a body is wrapped, a document sent as is", "[onenote]") {
	std::string doc, error;
	REQUIRE(OneNotePageDocument("Q4 <draft>", "<p>All green</p>", doc, error));
	REQUIRE(doc.find("<title>Q4 &lt;draft&gt;</title>") != std::string::npos);
	REQUIRE(doc.find("<body><p>All green</p></body>") != std::string::npos);
	REQUIRE(doc.rfind("<!DOCTYPE html>", 0) == 0);

	REQUIRE(OneNotePageDocument("", "x", doc, error)); // untitled page
	REQUIRE(doc.find("<title></title>") != std::string::npos);

	const std::string whole = "  <!doctype html><html><head><title>T</title></head><body>b</body></html>";
	REQUIRE(OneNotePageDocument("", whole, doc, error));
	REQUIRE(doc == whole);
	REQUIRE(OneNotePageDocument("", "<HTML><body>b</body></HTML>", doc, error));
	REQUIRE(doc == "<HTML><body>b</body></HTML>");
	REQUIRE_FALSE(OneNotePageDocument("T", whole, doc, error)); // two titles
	REQUIRE(error.find("title") != std::string::npos);
}

TEST_CASE("OneNoteAppendCommands / JsonQuote", "[onenote]") {
	REQUIRE(JsonQuote("a\"b\\c\n\t\x01") == "\"a\\\"b\\\\c\\n\\t\\u0001\"");
	REQUIRE(JsonQuote("Grüße") == "\"Grüße\"");
	REQUIRE(OneNoteAppendCommands("<p>x</p>") ==
	        "[{\"target\":\"body\",\"action\":\"append\",\"content\":\"<p>x</p>\"}]");
}
