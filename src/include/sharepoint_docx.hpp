#pragma once

#include <string>
#include <vector>

namespace duckdb {
namespace sharepoint {

// ---------------------------------------------------------------------------
// read_docx() / read_docx_table() (#103). PURE: word/document.xml of a .docx
// (a zip of XML; the zip side is sharepoint_excel's Zip* helpers) turned into
// the body's paragraphs and tables, in document order. No Graph Word API.
// ---------------------------------------------------------------------------

struct DocxParagraph {
	std::string style;     // the paragraph style id ("Heading1", "Normal", ...), "" if none
	int heading_level = 0; // 1-9 for Heading1..Heading9, 0 otherwise
	std::string text;      // runs joined; <w:tab/> is '\t', <w:br/> is '\n'
};

struct DocxTable {
	std::vector<std::vector<std::string>> rows; // each cell's paragraphs, joined by '\n'
};

struct DocxBody {
	std::vector<DocxParagraph> paragraphs; // top-level paragraphs only (not those in tables)
	std::vector<DocxTable> tables;         // top-level tables, in order
};

//! Parse word/document.xml. False with `error` when it has no <w:body>.
bool ParseDocx(const std::string &document_xml, DocxBody &out, std::string &error);

} // namespace sharepoint
} // namespace duckdb
