#pragma once

#include <cstdint>
#include <string>

namespace duckdb {
namespace sharepoint {

// ---------------------------------------------------------------------------
// read_outlook_messages() (#126): which Graph request reads a mailbox's
// messages. PURE: no duckdb.hpp, no I/O, so the Catch2 binary compiles it
// directly. The requests and rows are in sharepoint_mail_functions.cpp.
// ---------------------------------------------------------------------------

//! "/v1.0/me" for the signed-in user (empty `user`), else
//! "/v1.0/users/<user, percent-encoded but for '@'>".
std::string MailboxBase(const std::string &user);

//! True if `folder` is one of Graph's well-known mail folder names (any case;
//! "inbox", "sentitems", "drafts", "archive", ...); `segment` gets it in
//! Graph's spelling. False: a display name, to be looked up.
bool WellKnownMailFolder(const std::string &folder, std::string &segment);

struct MailQuery {
	//! MailboxBase().
	std::string mailbox;
	//! Empty: every message of the mailbox; else a well-known name or a
	//! folder id (percent-encoded here).
	std::string folder;
	//! receivedDateTime >= since (microseconds since the epoch, UTC).
	bool has_since = false;
	int64_t since_micros = 0;
	//! Also select the body.
	bool body = false;
	int page_size = 100;
};

//! The first page's path, newest first, with the columns read selected.
std::string MailMessagesPath(const MailQuery &q);

} // namespace sharepoint
} // namespace duckdb
