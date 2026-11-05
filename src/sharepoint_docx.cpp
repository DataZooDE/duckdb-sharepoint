#include "sharepoint_docx.hpp"

#include <cstddef>
#include <cstdlib>

namespace duckdb {
namespace sharepoint {

namespace {

// The tag starting at `pos` ('<' there): its name ("w:p", "/w:p"), and where
// it ends ('>' + 1). Self-closing is reported through `self_closing`.
bool TagAt(const std::string &x, std::size_t pos, std::string &name, std::size_t &end, bool &self_closing) {
	std::size_t i = pos + 1;
	std::size_t start = i;
	while (i < x.size() && x[i] != ' ' && x[i] != '>' && x[i] != '/' && x[i] != '\t' && x[i] != '\n' &&
	       x[i] != '\r') {
		i++;
	}
	if (i < x.size() && x[i] == '/' && i == start) { // closing tag "</name>"
		i++;
		while (i < x.size() && x[i] != '>' && x[i] != ' ') {
			i++;
		}
	}
	name = x.substr(start, i - start);
	end = x.find('>', i);
	if (end == std::string::npos) {
		return false;
	}
	self_closing = end > 0 && x[end - 1] == '/';
	end++;
	return true;
}

std::string Attr(const std::string &tag, const std::string &attr) {
	const auto at = tag.find(attr + "=\"");
	if (at == std::string::npos) {
		return std::string();
	}
	const auto start = at + attr.size() + 2;
	return tag.substr(start, tag.find('"', start) - start);
}

std::string Unescape(const std::string &s) {
	std::string out;
	for (std::size_t i = 0; i < s.size(); i++) {
		if (s[i] != '&') {
			out += s[i];
			continue;
		}
		const auto semi = s.find(';', i);
		if (semi == std::string::npos) {
			out += s[i];
			continue;
		}
		const auto entity = s.substr(i + 1, semi - i - 1);
		if (entity == "amp") {
			out += '&';
		} else if (entity == "lt") {
			out += '<';
		} else if (entity == "gt") {
			out += '>';
		} else if (entity == "quot") {
			out += '"';
		} else if (entity == "apos") {
			out += '\'';
		} else if (!entity.empty() && entity[0] == '#') {
			const long code = entity.size() > 1 && (entity[1] == 'x' || entity[1] == 'X')
			                      ? std::strtol(entity.c_str() + 2, nullptr, 16)
			                      : std::strtol(entity.c_str() + 1, nullptr, 10);
			if (code < 0x80) {
				out += static_cast<char>(code);
			} else if (code < 0x800) {
				out += static_cast<char>(0xC0 | (code >> 6));
				out += static_cast<char>(0x80 | (code & 0x3F));
			} else {
				out += static_cast<char>(0xE0 | (code >> 12));
				out += static_cast<char>(0x80 | ((code >> 6) & 0x3F));
				out += static_cast<char>(0x80 | (code & 0x3F));
			}
		} else {
			out += s.substr(i, semi - i + 1);
		}
		i = semi;
	}
	return out;
}

// One paragraph, from just after its opening tag to its </w:p>.
DocxParagraph ParseParagraph(const std::string &x, std::size_t &pos) {
	DocxParagraph p;
	int depth = 1;
	while (pos < x.size() && depth > 0) {
		const auto lt = x.find('<', pos);
		if (lt == std::string::npos) {
			pos = x.size();
			break;
		}
		std::string name;
		std::size_t end;
		bool self;
		if (!TagAt(x, lt, name, end, self)) {
			pos = x.size();
			break;
		}
		if (name == "w:p" && !self) {
			depth++;
		} else if (name == "/w:p") {
			depth--;
		} else if (name == "w:pStyle") {
			p.style = Attr(x.substr(lt, end - lt), "w:val");
		} else if (name == "w:t" && !self) {
			const auto close = x.find("</w:t>", end);
			p.text += Unescape(x.substr(end, close - end));
			end = close == std::string::npos ? x.size() : close + 6;
		} else if (name == "w:tab") {
			p.text += '\t';
		} else if (name == "w:br" || name == "w:cr") {
			p.text += '\n';
		}
		pos = end;
	}
	if (p.style.size() == 8 && p.style.compare(0, 7, "Heading") == 0 && p.style[7] >= '1' && p.style[7] <= '9') {
		p.heading_level = p.style[7] - '0';
	}
	return p;
}

// One table, from just after <w:tbl> to its </w:tbl>; nested tables' text
// stays in the cell holding them.
DocxTable ParseTable(const std::string &x, std::size_t &pos) {
	DocxTable t;
	int depth = 1;
	std::vector<std::string> *row = nullptr;
	std::string *cell = nullptr;
	while (pos < x.size() && depth > 0) {
		const auto lt = x.find('<', pos);
		if (lt == std::string::npos) {
			pos = x.size();
			break;
		}
		std::string name;
		std::size_t end;
		bool self;
		if (!TagAt(x, lt, name, end, self)) {
			pos = x.size();
			break;
		}
		pos = end;
		if (name == "w:tbl" && !self) {
			depth++;
		} else if (name == "/w:tbl") {
			depth--;
		} else if (depth == 1 && name == "w:tr" && !self) {
			t.rows.emplace_back();
			row = &t.rows.back();
		} else if (depth == 1 && name == "w:tc" && !self && row) {
			row->emplace_back();
			cell = &row->back();
		} else if (name == "w:p" && !self && cell) {
			auto para = ParseParagraph(x, pos);
			*cell += (cell->empty() ? "" : "\n") + para.text;
		}
	}
	return t;
}

} // namespace

bool ParseDocx(const std::string &document_xml, DocxBody &out, std::string &error) {
	const auto body = document_xml.find("<w:body");
	if (body == std::string::npos) {
		error = "no <w:body> in word/document.xml";
		return false;
	}
	std::size_t pos = document_xml.find('>', body);
	if (pos == std::string::npos) {
		error = "a truncated <w:body>";
		return false;
	}
	pos++;
	while (pos < document_xml.size()) {
		const auto lt = document_xml.find('<', pos);
		if (lt == std::string::npos) {
			break;
		}
		std::string name;
		std::size_t end;
		bool self;
		if (!TagAt(document_xml, lt, name, end, self)) {
			break;
		}
		pos = end;
		if (name == "/w:body") {
			break;
		}
		if (name == "w:p" && !self) {
			out.paragraphs.push_back(ParseParagraph(document_xml, pos));
		} else if (name == "w:p") {
			out.paragraphs.emplace_back(); // an empty paragraph
		} else if (name == "w:tbl" && !self) {
			out.tables.push_back(ParseTable(document_xml, pos));
		}
	}
	return true;
}

} // namespace sharepoint
} // namespace duckdb
