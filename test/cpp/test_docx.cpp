// read_docx (#103): word/document.xml as paragraphs and tables.
#include "sharepoint_docx.hpp"

#include <catch2/catch_test_macros.hpp>

using namespace duckdb::sharepoint;

TEST_CASE("ParseDocx: paragraphs with styles, runs, tabs, breaks and entities", "[docx]") {
	const std::string xml = R"(<?xml version="1.0"?><w:document xmlns:w="x"><w:body>
	  <w:p><w:pPr><w:pStyle w:val="Heading1"/></w:pPr><w:r><w:t>Contract</w:t></w:r></w:p>
	  <w:p><w:r><w:t xml:space="preserve">Notice period: </w:t></w:r><w:r><w:rPr><w:b/></w:rPr><w:t>3 months</w:t></w:r></w:p>
	  <w:p><w:r><w:t>a</w:t><w:tab/><w:t>b</w:t><w:br/><w:t>A &amp; B &lt;x&gt; &#228;</w:t></w:r></w:p>
	  <w:p/>
	  <w:sectPr/></w:body></w:document>)";
	DocxBody body;
	std::string error;
	REQUIRE(ParseDocx(xml, body, error));
	REQUIRE(body.paragraphs.size() == 4);
	REQUIRE(body.paragraphs[0].style == "Heading1");
	REQUIRE(body.paragraphs[0].heading_level == 1);
	REQUIRE(body.paragraphs[0].text == "Contract");
	REQUIRE(body.paragraphs[1].text == "Notice period: 3 months");
	REQUIRE(body.paragraphs[1].heading_level == 0);
	REQUIRE(body.paragraphs[2].text == "a\tb\nA & B <x> \xC3\xA4");
	REQUIRE(body.paragraphs[3].text.empty());
}

TEST_CASE("ParseDocx: tables, kept apart from the body's paragraphs", "[docx]") {
	const std::string xml = R"(<w:document><w:body><w:p><w:r><w:t>Before</w:t></w:r></w:p>
	  <w:tbl><w:tblPr/><w:tr><w:tc><w:p><w:r><w:t>Region</w:t></w:r></w:p></w:tc><w:tc><w:p><w:r><w:t>Units</w:t></w:r></w:p></w:tc></w:tr>
	  <w:tr><w:tc><w:p><w:r><w:t>North</w:t></w:r></w:p><w:p><w:r><w:t>East</w:t></w:r></w:p></w:tc><w:tc><w:p><w:r><w:t>10</w:t></w:r></w:p></w:tc></w:tr></w:tbl>
	  <w:p><w:r><w:t>After</w:t></w:r></w:p></w:body></w:document>)";
	DocxBody body;
	std::string error;
	REQUIRE(ParseDocx(xml, body, error));
	REQUIRE(body.paragraphs.size() == 2);
	REQUIRE(body.paragraphs[1].text == "After");
	REQUIRE(body.tables.size() == 1);
	REQUIRE(body.tables[0].rows.size() == 2);
	REQUIRE(body.tables[0].rows[0] == std::vector<std::string> {"Region", "Units"});
	REQUIRE(body.tables[0].rows[1] == std::vector<std::string> {"North\nEast", "10"});
	REQUIRE_FALSE(ParseDocx("<w:document/>", body, error));
}
