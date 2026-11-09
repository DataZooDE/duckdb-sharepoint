#include "sharepoint_mail.hpp"

#include "sharepoint_list.hpp"
#include "sharepoint_weburl.hpp"

#include <cctype>

namespace duckdb {
namespace sharepoint {

std::string MailboxBase(const std::string &user) {
	if (user.empty()) {
		return "/v1.0/me";
	}
	// '@' is kept: Graph takes a UPN as it is; everything else that is not
	// unreserved is encoded, so no user name can end the path or add a query.
	std::string out = "/v1.0/users/";
	const auto encoded = PercentEncodeSegment(user);
	for (size_t i = 0; i < encoded.size(); i++) {
		if (encoded.compare(i, 3, "%40") == 0) {
			out += '@';
			i += 2;
		} else {
			out += encoded[i];
		}
	}
	return out;
}

bool WellKnownMailFolder(const std::string &folder, std::string &segment) {
	static const char *names[] = {"inbox",       "sentitems",           "drafts",       "deleteditems",
	                              "archive",     "junkemail",           "outbox",       "conversationhistory",
	                              "clutter",     "scheduled",           "searchfolders", "msgfolderroot",
	                              "recoverableitemsdeletions"};
	std::string lower;
	for (char c : folder) {
		lower += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
	}
	for (const char *name : names) {
		if (lower == name) {
			segment = lower;
			return true;
		}
	}
	return false;
}

std::string MailMessagesPath(const MailQuery &q) {
	std::string path = q.mailbox;
	if (!q.folder.empty()) {
		path += "/mailFolders/" + PercentEncodeSegment(q.folder);
	}
	path += "/messages?$select=id,receivedDateTime,sentDateTime,subject,from,toRecipients,ccRecipients,isRead,"
	        "isDraft,hasAttachments,importance,categories,conversationId,internetMessageId,bodyPreview,webLink";
	if (q.body) {
		path += ",body";
	}
	if (q.has_since) {
		path += "&$filter=" + PercentEncodeSegment("receivedDateTime ge " + FormatIsoTimestamp(q.since_micros));
	}
	// $orderby on the filtered property is what Graph accepts with a filter.
	path += "&$orderby=receivedDateTime%20desc&$top=" + std::to_string(q.page_size);
	return path;
}

} // namespace sharepoint
} // namespace duckdb
