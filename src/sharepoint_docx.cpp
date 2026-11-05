#include "sharepoint_docx.hpp"
#include "sharepoint_excel.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <functional>

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

// ===========================================================================
// Template filling (#104)
// ===========================================================================

namespace {

//! One <w:t> element: where its opening tag and its text are, the text
//! (unescaped), and the paragraph it belongs to.
struct TextElement {
	std::size_t open = 0;    // '<' of <w:t ...>
	std::size_t content = 0; // just after its '>'
	std::size_t close = 0;   // '<' of </w:t>
	std::string text;
	int paragraph = 0;
};

std::vector<TextElement> TextElements(const std::string &x) {
	std::vector<TextElement> out;
	std::vector<int> open_paragraphs;
	int next_paragraph = 0;
	std::size_t pos = 0;
	while (pos < x.size()) {
		const auto lt = x.find('<', pos);
		if (lt == std::string::npos) {
			break;
		}
		std::string name;
		std::size_t end;
		bool self;
		if (!TagAt(x, lt, name, end, self)) {
			break;
		}
		pos = end;
		if (name == "w:p" && !self) {
			open_paragraphs.push_back(next_paragraph++);
		} else if (name == "/w:p" && !open_paragraphs.empty()) {
			open_paragraphs.pop_back();
		} else if (name == "w:t" && !self) {
			const auto close = x.find("</w:t>", end);
			if (close == std::string::npos) {
				break;
			}
			TextElement t;
			t.open = lt;
			t.content = end;
			t.close = close;
			t.text = Unescape(x.substr(end, close - end));
			// Text outside any paragraph stands alone.
			t.paragraph = open_paragraphs.empty() ? next_paragraph++ : open_paragraphs.back();
			out.push_back(std::move(t));
			pos = close + 6;
		}
	}
	return out;
}

struct Placeholder {
	std::size_t begin = 0; // "{{"
	std::size_t end = 0;   // after "}}"
	std::string name;      // trimmed
};

std::vector<Placeholder> Placeholders(const std::string &text) {
	std::vector<Placeholder> out;
	for (auto open = text.find("{{"); open != std::string::npos; open = text.find("{{", open)) {
		const auto close = text.find("}}", open + 2);
		if (close == std::string::npos) {
			break;
		}
		Placeholder p;
		p.begin = open;
		p.end = close + 2;
		auto b = open + 2, e = close;
		while (b < e && (text[b] == ' ' || text[b] == '\t')) {
			b++;
		}
		while (e > b && (text[e - 1] == ' ' || text[e - 1] == '\t')) {
			e--;
		}
		p.name = text.substr(b, e - b);
		open = p.end;
		out.push_back(std::move(p));
	}
	return out;
}

//! Text for a <w:t>: escaped; a line break or tab ends the element, puts in
//! <w:br/> / <w:tab/>, and opens a new one.
std::string EncodeText(const std::string &s) {
	std::string out;
	for (char c : s) {
		switch (c) {
		case '&':
			out += "&amp;";
			break;
		case '<':
			out += "&lt;";
			break;
		case '>':
			out += "&gt;";
			break;
		case '\n':
			out += "</w:t><w:br/><w:t xml:space=\"preserve\">";
			break;
		case '\t':
			out += "</w:t><w:tab/><w:t xml:space=\"preserve\">";
			break;
		case '\r':
			break;
		default:
			out += c;
		}
	}
	return out;
}

using Lookup = std::function<bool(const std::string &name, std::string &value, std::string &error)>;

//! Every placeholder in `x` replaced through `lookup`, per paragraph.
bool FillText(const std::string &x, const Lookup &lookup, std::string &out, std::string &error) {
	auto elements = TextElements(x);
	std::vector<bool> changed(elements.size(), false);
	for (std::size_t first = 0; first < elements.size();) {
		std::size_t last = first;
		std::string text;
		std::vector<std::size_t> starts; // where each element's text starts in `text`
		while (last < elements.size() && elements[last].paragraph == elements[first].paragraph) {
			starts.push_back(text.size());
			text += elements[last].text;
			last++;
		}
		const auto found = Placeholders(text);
		if (!found.empty()) {
			std::vector<std::string> values;
			for (auto &p : found) {
				std::string value;
				if (!lookup(p.name, value, error)) {
					return false;
				}
				values.push_back(std::move(value));
			}
			for (std::size_t i = first; i < last; i++) {
				const auto from = starts[i - first];
				const auto to = from + elements[i].text.size();
				std::string replaced;
				std::size_t k = 0;
				for (auto at = from; at < to; at++) {
					while (k < found.size() && found[k].end <= at) {
						k++;
					}
					if (k < found.size() && at >= found[k].begin) {
						if (at == found[k].begin) {
							replaced += values[k]; // where it starts: that run's formatting
						}
						continue;
					}
					replaced += text[at];
				}
				if (replaced != elements[i].text) {
					elements[i].text = std::move(replaced);
					changed[i] = true;
				}
			}
		}
		first = last;
	}
	out.clear();
	std::size_t copied = 0;
	for (std::size_t i = 0; i < elements.size(); i++) {
		if (!changed[i]) {
			continue;
		}
		auto &e = elements[i];
		out.append(x, copied, e.open - copied);
		auto tag = x.substr(e.open, e.content - e.open);
		if (tag.find("xml:space") == std::string::npos) {
			tag.insert(tag.size() - 1, " xml:space=\"preserve\"");
		}
		out += tag;
		out += EncodeText(e.text);
		copied = e.close;
	}
	out.append(x, copied, std::string::npos);
	return true;
}

//! The text of every <w:t> in `x`, joined.
std::string AllText(const std::string &x) {
	std::string text;
	for (auto &e : TextElements(x)) {
		text += e.text;
	}
	return text;
}

bool HasRowsMarker(const std::string &x) {
	for (auto &p : Placeholders(AllText(x))) {
		if (p.name == "#rows") {
			return true;
		}
	}
	return false;
}

//! Where the real tag `name` next opens at or after `from` (not a longer name
//! with the same start, like <w:trPr> for <w:tr>), or npos.
std::size_t FindOpenTag(const std::string &x, const std::string &name, std::size_t from, std::size_t &end) {
	for (auto lt = x.find("<" + name, from); lt != std::string::npos; lt = x.find("<" + name, lt + 1)) {
		std::string found;
		bool self;
		if (TagAt(x, lt, found, end, self) && found == name && !self) {
			return lt;
		}
	}
	return std::string::npos;
}

//! Where the element opened at `open_end` (just after <w:tr ...>) closes,
//! after its </w:tr>; npos when it does not.
std::size_t ElementEnd(const std::string &x, std::size_t open_end, const std::string &name) {
	int depth = 1;
	std::size_t pos = open_end;
	while (pos < x.size()) {
		const auto lt = x.find('<', pos);
		if (lt == std::string::npos) {
			break;
		}
		std::string found;
		std::size_t end;
		bool self;
		if (!TagAt(x, lt, found, end, self)) {
			break;
		}
		pos = end;
		if (found == name && !self) {
			depth++;
		} else if (found == "/" + name && --depth == 0) {
			return end;
		}
	}
	return std::string::npos;
}

//! Word's optional paragraph ids (w14:paraId, w14:textId) dropped: a
//! repeated row would otherwise repeat them.
std::string WithoutParagraphIds(std::string x) {
	for (const char *attr : {" w14:paraId=\"", " w14:textId=\""}) {
		for (auto at = x.find(attr); at != std::string::npos; at = x.find(attr, at)) {
			const auto quote = x.find('"', at + std::string(attr).size());
			if (quote == std::string::npos) {
				break;
			}
			x.erase(at, quote + 1 - at);
		}
	}
	return x;
}

bool Find(const DocxValues &values, const std::string &name, std::string &value) {
	auto it = values.find(DocxKey(name));
	if (it == values.end()) {
		return false;
	}
	value = it->second;
	return true;
}

std::string NoValue(const std::string &name) {
	return "no value for {{" + name + "}}: it is neither a column of the query nor in VARIABLES";
}

} // namespace

std::string DocxKey(const std::string &name) {
	std::string key = name;
	for (auto &c : key) {
		if (c >= 'A' && c <= 'Z') {
			c = static_cast<char>(c - 'A' + 'a');
		}
	}
	return key;
}

bool FillDocxPart(const std::string &xml, const DocxFillInput &input, std::string &out, bool &repeated,
                  std::string &error) {
	const Lookup scalar = [&](const std::string &name, std::string &value, std::string &err) {
		if (name == "/rows") {
			value.clear();
			return true;
		}
		if (name == "#rows") {
			err = "{{#rows}} marks a table row to repeat, but this one is not in a table row";
			return false;
		}
		if (!Find(input.scalars, name, value)) {
			err = NoValue(name);
			return false;
		}
		return true;
	};
	out.clear();
	std::size_t pending = 0; // start of the text not yet filled
	std::size_t pos = 0;
	while (true) {
		std::size_t open_end;
		const auto row = FindOpenTag(xml, "w:tr", pos, open_end);
		if (row == std::string::npos) {
			break;
		}
		const auto row_end = ElementEnd(xml, open_end, "w:tr");
		if (row_end == std::string::npos) {
			break;
		}
		const auto row_xml = xml.substr(row, row_end - row);
		std::size_t nested_end;
		if (!HasRowsMarker(row_xml)) {
			pos = row_end;
			continue;
		}
		if (FindOpenTag(row_xml, "w:tr", 1, nested_end) != std::string::npos) {
			pos = open_end; // the marker is in a nested table's row
			continue;
		}
		std::string filled;
		if (!FillText(xml.substr(pending, row - pending), scalar, filled, error)) {
			return false;
		}
		out += filled;
		const auto copy = WithoutParagraphIds(row_xml);
		for (auto &values : input.rows) {
			const Lookup in_row = [&](const std::string &name, std::string &value, std::string &err) {
				if (name == "#rows" || name == "/rows") {
					value.clear();
					return true;
				}
				if (Find(values, name, value) || Find(input.scalars, name, value)) {
					return true;
				}
				err = NoValue(name);
				return false;
			};
			if (!FillText(copy, in_row, filled, error)) {
				return false;
			}
			out += filled;
		}
		repeated = true;
		pending = pos = row_end;
	}
	std::string filled;
	if (!FillText(xml.substr(pending), scalar, filled, error)) {
		return false;
	}
	out += filled;
	return true;
}

bool IsDocxTemplatePart(const std::string &name) {
	if (name == "word/document.xml" || name == "word/footnotes.xml" || name == "word/endnotes.xml") {
		return true;
	}
	const bool xml = name.size() > 4 && name.compare(name.size() - 4, 4, ".xml") == 0;
	return xml && name.find('/', 5) == std::string::npos &&
	       (name.compare(0, 11, "word/header") == 0 || name.compare(0, 11, "word/footer") == 0);
}

namespace {

uint64_t ReadLe(const std::string &s, std::size_t pos, int bytes) {
	uint64_t v = 0;
	for (int i = bytes - 1; i >= 0; i--) {
		v = (v << 8) | static_cast<unsigned char>(s[pos + i]);
	}
	return v;
}

void PutLe(std::string &s, uint64_t v, int bytes) {
	for (int i = 0; i < bytes; i++) {
		s += static_cast<char>((v >> (8 * i)) & 0xFF);
	}
}

} // namespace

bool ReadZipMembers(const std::string &zip, std::vector<ZipMember> &out, std::string &error) {
	uint64_t cd_offset, cd_size;
	if (!ZipCentralDirectory(zip, zip.size(), cd_offset, cd_size)) {
		error = "not a zip file";
		return false;
	}
	constexpr std::size_t kRecord = 46;
	for (std::size_t pos = cd_offset; pos < cd_offset + cd_size;) {
		if (pos + kRecord > zip.size() || ReadLe(zip, pos, 4) != 0x02014b50) {
			error = "a damaged zip central directory";
			return false;
		}
		const std::size_t name_len = ReadLe(zip, pos + 28, 2);
		ZipMember m;
		m.method = static_cast<uint16_t>(ReadLe(zip, pos + 10, 2));
		m.mod_time = static_cast<uint16_t>(ReadLe(zip, pos + 12, 2));
		m.mod_date = static_cast<uint16_t>(ReadLe(zip, pos + 14, 2));
		m.crc32 = static_cast<uint32_t>(ReadLe(zip, pos + 16, 4));
		const uint64_t compressed = ReadLe(zip, pos + 20, 4);
		m.uncompressed_size = ReadLe(zip, pos + 24, 4);
		const uint64_t local = ReadLe(zip, pos + 42, 4);
		if (pos + kRecord + name_len > zip.size()) {
			error = "a damaged zip central directory";
			return false;
		}
		m.name = zip.substr(pos + kRecord, name_len);
		uint64_t header = 0;
		if (local >= zip.size() || !ZipLocalHeaderSize(zip.substr(local, 30 + 0xFFFF * 2), header) ||
		    local + header + compressed > zip.size()) {
			error = "a damaged zip member '" + m.name + "'";
			return false;
		}
		m.data = zip.substr(local + header, compressed);
		out.push_back(std::move(m));
		pos += kRecord + name_len + ReadLe(zip, pos + 30, 2) + ReadLe(zip, pos + 32, 2);
	}
	return true;
}

std::string WriteZip(const std::vector<ZipMember> &members) {
	std::string zip, cd;
	for (auto &m : members) {
		bool utf8 = false;
		for (unsigned char c : m.name) {
			utf8 = utf8 || c >= 0x80;
		}
		const uint64_t offset = zip.size();
		for (int central = 0; central < 2; central++) {
			auto &s = central ? cd : zip;
			PutLe(s, central ? 0x02014b50 : 0x04034b50, 4);
			if (central) {
				PutLe(s, 20, 2); // made by
			}
			PutLe(s, 20, 2);                // needed
			PutLe(s, utf8 ? 0x0800 : 0, 2); // flags: names in UTF-8
			PutLe(s, m.method, 2);
			PutLe(s, m.mod_time, 2);
			PutLe(s, m.mod_date, 2);
			PutLe(s, m.crc32, 4);
			PutLe(s, m.data.size(), 4);
			PutLe(s, m.uncompressed_size, 4);
			PutLe(s, m.name.size(), 2);
			PutLe(s, 0, 2); // extra
			if (central) {
				PutLe(s, 0, 2); // comment
				PutLe(s, 0, 2); // disk
				PutLe(s, 0, 2); // internal attributes
				PutLe(s, 0, 4); // external attributes
				PutLe(s, offset, 4);
			}
			s += m.name;
		}
		zip += m.data;
	}
	const uint64_t cd_offset = zip.size();
	zip += cd;
	PutLe(zip, 0x06054b50, 4);
	PutLe(zip, 0, 2);
	PutLe(zip, 0, 2);
	PutLe(zip, members.size(), 2);
	PutLe(zip, members.size(), 2);
	PutLe(zip, cd.size(), 4);
	PutLe(zip, cd_offset, 4);
	PutLe(zip, 0, 2);
	return zip;
}

uint32_t ZipCrc32(const std::string &data) {
	static const auto table = [] {
		std::array<uint32_t, 256> t {};
		for (uint32_t i = 0; i < 256; i++) {
			uint32_t c = i;
			for (int k = 0; k < 8; k++) {
				c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
			}
			t[i] = c;
		}
		return t;
	}();
	uint32_t crc = 0xFFFFFFFFu;
	for (unsigned char c : data) {
		crc = table[(crc ^ c) & 0xFF] ^ (crc >> 8);
	}
	return crc ^ 0xFFFFFFFFu;
}

} // namespace sharepoint
} // namespace duckdb
