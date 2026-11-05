#pragma once

#include <cstdint>
#include <map>
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

// ---------------------------------------------------------------------------
// COPY ... TO 'x.docx' (FORMAT docx, TEMPLATE 't.docx') (#104). PURE: one
// part of the template (word/document.xml, a header or footer) filled in.
//
// - `{{name}}` (spaces inside the braces allowed) is replaced by a value, in
//   the run where it starts, so it keeps that run's formatting. Word often
//   splits typed text across runs; a placeholder may span them within one
//   paragraph.
// - A table row holding `{{#rows}}` is repeated once per query row (and
//   removed for none); in it, names come from that row first. The marker,
//   and an optional `{{/rows}}`, are dropped.
// - Values are text; '\n' becomes a line break, '\t' a tab.
// ---------------------------------------------------------------------------

//! Names are matched case-insensitively; keys here are lower case (ASCII).
using DocxValues = std::map<std::string, std::string>;

struct DocxFillInput {
	DocxValues scalars;           // outside a repeated row (VARIABLES, else the first query row)
	std::vector<DocxValues> rows; // one per query row, for the repeated row(s)
};

//! Fill one part. False with `error` for a placeholder without a value, or a
//! {{#rows}} outside a table row. `repeated` is set when the part had a
//! {{#rows}} row.
bool FillDocxPart(const std::string &xml, const DocxFillInput &input, std::string &out, bool &repeated,
                  std::string &error);

//! Lower case (ASCII), as DocxValues keys are.
std::string DocxKey(const std::string &name);

//! A zip member, as stored: `data` is still compressed when method is 8.
struct ZipMember {
	std::string name;
	uint16_t method = 0; // 0 stored, 8 deflate
	uint16_t mod_time = 0;
	uint16_t mod_date = 0;
	uint32_t crc32 = 0;
	uint64_t uncompressed_size = 0;
	std::string data;
};

//! Every member of a zip, from its central directory (no zip64).
bool ReadZipMembers(const std::string &zip, std::vector<ZipMember> &out, std::string &error);

//! A zip of `members`, in order (local headers without data descriptors).
std::string WriteZip(const std::vector<ZipMember> &members);

//! The zip CRC-32 of `data`.
uint32_t ZipCrc32(const std::string &data);

//! Whether a .docx member is one FillDocxPart fills: the body, headers, footers.
bool IsDocxTemplatePart(const std::string &member_name);

} // namespace sharepoint
} // namespace duckdb
