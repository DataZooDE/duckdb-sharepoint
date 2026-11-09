#include "sharepoint_onenote_page.hpp"

#include <cctype>
#include <cstddef>
#include <cstdio>

namespace duckdb {
namespace sharepoint {

namespace {

bool CIEquals(const std::string &a, const char *b) {
	size_t i = 0;
	for (; i < a.size() && b[i]; i++) {
		if (std::tolower(static_cast<unsigned char>(a[i])) != std::tolower(static_cast<unsigned char>(b[i]))) {
			return false;
		}
	}
	return i == a.size() && !b[i];
}

//! True if `s`, after leading whitespace, starts with `prefix` (ASCII, any case).
bool StartsWithCI(const std::string &s, const char *prefix) {
	size_t i = 0;
	while (i < s.size() && std::isspace(static_cast<unsigned char>(s[i]))) {
		i++;
	}
	for (size_t j = 0; prefix[j]; j++, i++) {
		if (i >= s.size() ||
		    std::tolower(static_cast<unsigned char>(s[i])) != std::tolower(static_cast<unsigned char>(prefix[j]))) {
			return false;
		}
	}
	return true;
}

} // namespace

bool ParseOneNoteTarget(const std::string &target, OneNoteTarget &out, std::string &error) {
	size_t begin = 0, end = target.size();
	while (begin < end && target[begin] == '/') {
		begin++;
	}
	while (end > begin && target[end - 1] == '/') {
		end--;
	}
	const auto inner = target.substr(begin, end - begin);
	const auto slash = inner.find('/');
	if (inner.empty() || slash == std::string::npos || slash == 0 || slash + 1 == inner.size() ||
	    inner.find('/', slash + 1) != std::string::npos) {
		error = "a OneNote target is '<notebook>/<section>', not '" + target + "'";
		return false;
	}
	out.notebook = inner.substr(0, slash);
	out.section = inner.substr(slash + 1);
	return true;
}

bool PlanOneNoteWrite(const std::vector<std::string> &names, OneNoteWritePlan &plan, std::string &error) {
	plan = OneNoteWritePlan();
	for (size_t i = 0; i < names.size(); i++) {
		int *slot = CIEquals(names[i], "title")     ? &plan.title
		            : CIEquals(names[i], "content") ? &plan.content
		            : CIEquals(names[i], "id")      ? &plan.id
		                                            : nullptr;
		if (!slot) {
			error = "FORMAT onenote writes the columns title, content (the page body's HTML) and id (a page to "
			        "append to), not \"" +
			        names[i] + "\"";
			return false;
		}
		if (*slot >= 0) {
			error = "FORMAT onenote got the column \"" + names[i] + "\" twice";
			return false;
		}
		*slot = static_cast<int>(i);
	}
	if (plan.content < 0) {
		error = "FORMAT onenote needs a content column (the page body's HTML)";
		return false;
	}
	return true;
}

std::string HtmlEscape(const std::string &text) {
	std::string out;
	out.reserve(text.size());
	for (char c : text) {
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
		case '"':
			out += "&quot;";
			break;
		case '\'':
			out += "&#39;";
			break;
		default:
			out += c;
		}
	}
	return out;
}

bool OneNotePageDocument(const std::string &title, const std::string &content, std::string &document,
                         std::string &error) {
	if (StartsWithCI(content, "<!doctype") || StartsWithCI(content, "<html")) {
		if (!title.empty()) {
			error = "the content is a whole HTML document, which carries its own <title>: leave the title column "
			        "NULL or drop it";
			return false;
		}
		document = content;
		return true;
	}
	document = "<!DOCTYPE html><html><head><title>" + HtmlEscape(title) +
	           "</title><meta charset=\"utf-8\"></head><body>" + content + "</body></html>";
	return true;
}

std::string JsonQuote(const std::string &s) {
	std::string out = "\"";
	for (char c : s) {
		switch (c) {
		case '"':
			out += "\\\"";
			break;
		case '\\':
			out += "\\\\";
			break;
		case '\n':
			out += "\\n";
			break;
		case '\r':
			out += "\\r";
			break;
		case '\t':
			out += "\\t";
			break;
		default:
			if (static_cast<unsigned char>(c) < 0x20) {
				char buf[8];
				std::snprintf(buf, sizeof(buf), "\\u%04x", static_cast<unsigned char>(c));
				out += buf;
			} else {
				out += c;
			}
		}
	}
	return out + "\"";
}

std::string OneNoteAppendCommands(const std::string &content) {
	return "[{\"target\":\"body\",\"action\":\"append\",\"content\":" + JsonQuote(content) + "}]";
}

} // namespace sharepoint
} // namespace duckdb
