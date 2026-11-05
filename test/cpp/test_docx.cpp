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

// --- COPY ... (FORMAT docx, TEMPLATE ...) (#104) ------------------------------

namespace {

std::string Body(const std::string &inner) {
	return "<w:document><w:body>" + inner + "<w:sectPr/></w:body></w:document>";
}

std::string Fill(const std::string &xml, const DocxFillInput &input) {
	std::string out, error;
	bool repeated = false;
	REQUIRE(FillDocxPart(xml, input, out, repeated, error));
	return out;
}

} // namespace

TEST_CASE("FillDocxPart: a placeholder split across runs keeps the first run's formatting", "[docx]") {
	DocxFillInput input;
	input.scalars["customer"] = "A & B";
	const auto out = Fill(Body("<w:p><w:r><w:t>Dear {{cus</w:t></w:r><w:r><w:rPr><w:b/></w:rPr>"
	                           "<w:t>tomer }}, hi</w:t></w:r></w:p>"),
	                      input);
	REQUIRE(out.find("<w:t xml:space=\"preserve\">Dear A &amp; B</w:t>") != std::string::npos);
	REQUIRE(out.find("<w:b/></w:rPr><w:t xml:space=\"preserve\">, hi</w:t>") != std::string::npos);
	DocxBody body;
	std::string error;
	REQUIRE(ParseDocx(out, body, error));
	REQUIRE(body.paragraphs[0].text == "Dear A & B, hi");
}

TEST_CASE("FillDocxPart: names are case-insensitive; line breaks and tabs become Word's", "[docx]") {
	DocxFillInput input;
	input.scalars[DocxKey("Address")] = "Main St 1\n12345 Town\tDE";
	const auto out = Fill(Body("<w:p><w:r><w:t>{{ADDRESS}}</w:t></w:r></w:p>"), input);
	REQUIRE(out.find("<w:br/>") != std::string::npos);
	DocxBody body;
	std::string error;
	REQUIRE(ParseDocx(out, body, error));
	REQUIRE(body.paragraphs[0].text == "Main St 1\n12345 Town\tDE");
}

TEST_CASE("FillDocxPart: the {{#rows}} row is repeated per row, other text from the scalars", "[docx]") {
	const auto xml = Body("<w:p><w:r><w:t>Quote for {{customer}}</w:t></w:r></w:p><w:tbl>"
	                      "<w:tr w14:paraId=\"1A2B\"><w:tc><w:p><w:r><w:t>Region</w:t></w:r></w:p></w:tc>"
	                      "<w:tc><w:p><w:r><w:t>Units</w:t></w:r></w:p></w:tc></w:tr>"
	                      "<w:tr><w:trPr/><w:tc><w:p w14:paraId=\"3C4D\" w14:textId=\"77\"><w:r><w:t>{{#rows}}{{"
	                      "</w:t></w:r><w:r><w:t>region}}</w:t></w:r></w:p></w:tc>"
	                      "<w:tc><w:p><w:r><w:t>{{units}} for {{customer}}{{/rows}}</w:t></w:r></w:p></w:tc></w:tr>"
	                      "</w:tbl>");
	DocxFillInput input;
	input.scalars["customer"] = "ACME";
	input.rows = {{{"region", "North"}, {"units", "10"}}, {{"region", "South"}, {"units", "7"}}};
	std::string out, error;
	bool repeated = false;
	REQUIRE(FillDocxPart(xml, input, out, repeated, error));
	REQUIRE(repeated);
	REQUIRE(out.find("3C4D") == std::string::npos);
	REQUIRE(out.find("1A2B") != std::string::npos); // rows that are not repeated keep their ids
	DocxBody body;
	REQUIRE(ParseDocx(out, body, error));
	REQUIRE(body.paragraphs[0].text == "Quote for ACME");
	REQUIRE(body.tables[0].rows == std::vector<std::vector<std::string>> {
	                                   {"Region", "Units"}, {"North", "10 for ACME"}, {"South", "7 for ACME"}});

	input.rows.clear();
	REQUIRE(FillDocxPart(xml, input, out, repeated, error));
	REQUIRE(ParseDocx(out, body = DocxBody(), error));
	REQUIRE(body.tables[0].rows.size() == 1);
}

TEST_CASE("FillDocxPart: a row-local value wins inside the repeated row", "[docx]") {
	DocxFillInput input;
	input.scalars["x"] = "outer";
	input.rows = {{{"x", "inner"}}};
	const auto out = Fill(Body("<w:p><w:r><w:t>{{x}}</w:t></w:r></w:p><w:tbl><w:tr><w:tc><w:p><w:r>"
	                           "<w:t>{{#rows}}{{x}}</w:t></w:r></w:p></w:tc></w:tr></w:tbl>"),
	                      input);
	DocxBody body;
	std::string error;
	REQUIRE(ParseDocx(out, body, error));
	REQUIRE(body.paragraphs[0].text == "outer");
	REQUIRE(body.tables[0].rows[0][0] == "inner");
}

TEST_CASE("FillDocxPart: refuses unknown names and a {{#rows}} outside a table row", "[docx]") {
	DocxFillInput input;
	std::string out, error;
	bool repeated = false;
	REQUIRE_FALSE(FillDocxPart(Body("<w:p><w:r><w:t>{{ nope }}</w:t></w:r></w:p>"), input, out, repeated, error));
	REQUIRE(error.find("{{nope}}") != std::string::npos);
	REQUIRE_FALSE(FillDocxPart(Body("<w:p><w:r><w:t>{{#rows}}</w:t></w:r></w:p>"), input, out, repeated, error));
	REQUIRE(error.find("not in a table row") != std::string::npos);
	// Text without placeholders is left byte for byte.
	const auto plain = Body("<w:p><w:r><w:t>a { b } c</w:t></w:r></w:p>");
	REQUIRE(FillDocxPart(plain, input, out, repeated, error));
	REQUIRE(out == plain);
	REQUIRE_FALSE(repeated);
}

TEST_CASE("Zip: CRC-32, members written and read back", "[docx]") {
	REQUIRE(ZipCrc32("123456789") == 0xCBF43926u);
	std::vector<ZipMember> members(2);
	members[0].name = "[Content_Types].xml";
	members[0].data = "<Types/>";
	members[1].name = "word/document.xml";
	members[1].data = "<w:document/>";
	members[1].mod_date = 0x5A21;
	for (auto &m : members) {
		m.crc32 = ZipCrc32(m.data);
		m.uncompressed_size = m.data.size();
	}
	std::vector<ZipMember> back;
	std::string error;
	REQUIRE(ReadZipMembers(WriteZip(members), back, error));
	REQUIRE(back.size() == 2);
	REQUIRE(back[1].name == "word/document.xml");
	REQUIRE(back[1].data == "<w:document/>");
	REQUIRE(back[1].crc32 == members[1].crc32);
	REQUIRE(back[1].mod_date == 0x5A21);
	REQUIRE_FALSE(ReadZipMembers("not a zip", back, error));
}

TEST_CASE("IsDocxTemplatePart: the body, headers, footers and notes", "[docx]") {
	REQUIRE(IsDocxTemplatePart("word/document.xml"));
	REQUIRE(IsDocxTemplatePart("word/header1.xml"));
	REQUIRE(IsDocxTemplatePart("word/footer2.xml"));
	REQUIRE(IsDocxTemplatePart("word/footnotes.xml"));
	REQUIRE_FALSE(IsDocxTemplatePart("word/styles.xml"));
	REQUIRE_FALSE(IsDocxTemplatePart("word/_rels/header1.xml.rels"));
	REQUIRE_FALSE(IsDocxTemplatePart("word/media/header.png"));
}
