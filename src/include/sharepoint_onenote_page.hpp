#pragma once

#include <string>
#include <vector>

namespace duckdb {
namespace sharepoint {

// ---------------------------------------------------------------------------
// COPY ... TO '<notebook>/<section>' (FORMAT onenote) (#105): what is sent to
// OneNote. PURE: no duckdb.hpp, no I/O, so the Catch2 binary compiles it
// directly. The Graph calls are in sharepoint_onenote.cpp.
// ---------------------------------------------------------------------------

struct OneNoteTarget {
	std::string notebook;
	std::string section;
};

//! "Projects/Q4" -> notebook "Projects", section "Q4". Exactly one '/'
//! (section groups are not addressed); surrounding slashes are dropped.
bool ParseOneNoteTarget(const std::string &target, OneNoteTarget &out, std::string &error);

//! Which COPY column is which; -1 when absent.
struct OneNoteWritePlan {
	int title = -1;
	int content = -1;
	int id = -1;
};

//! Plan a COPY's columns (case-insensitive): content (the page body's HTML)
//! is required, title and id are optional, nothing else is allowed.
bool PlanOneNoteWrite(const std::vector<std::string> &names, OneNoteWritePlan &plan, std::string &error);

//! `text` safe in HTML text and attribute values.
std::string HtmlEscape(const std::string &text);

//! The HTML document POSTed to create a page: `content` is the body. If
//! `content` already is a whole document (<!DOCTYPE or <html), it is sent as
//! it is and `title` must be empty (the document carries its own).
bool OneNotePageDocument(const std::string &title, const std::string &content, std::string &document,
                         std::string &error);

//! The PATCH body that appends `content` to the end of a page's body.
std::string OneNoteAppendCommands(const std::string &content);

//! `s` as a JSON string literal, quotes included.
std::string JsonQuote(const std::string &s);

} // namespace sharepoint
} // namespace duckdb
