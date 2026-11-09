// read_outlook_messages() (#126): the pure parts -- the mailbox, well-known
// folders and the request path. Reading a real mailbox is the e2e test.
#include "sharepoint_mail.hpp"

#include <catch2/catch_test_macros.hpp>

#include <string>

using namespace duckdb::sharepoint;

TEST_CASE("MailboxBase: me, or a user by UPN or id", "[mail]") {
	REQUIRE(MailboxBase("") == "/v1.0/me");
	REQUIRE(MailboxBase("invoices@contoso.com") == "/v1.0/users/invoices@contoso.com");
	REQUIRE(MailboxBase("4f1c0b5e-1111-2222-3333-444455556666") ==
	        "/v1.0/users/4f1c0b5e-1111-2222-3333-444455556666");
	// A guest's UPN has '#'; nothing may end the path early or add a query.
	REQUIRE(MailboxBase("anna_x.com#EXT#@contoso.onmicrosoft.com") ==
	        "/v1.0/users/anna_x.com%23EXT%23@contoso.onmicrosoft.com");
	REQUIRE(MailboxBase("a/../b?x") == "/v1.0/users/a%2F..%2Fb%3Fx");
}

TEST_CASE("WellKnownMailFolder", "[mail]") {
	std::string seg;
	REQUIRE(WellKnownMailFolder("Inbox", seg));
	REQUIRE(seg == "inbox");
	REQUIRE(WellKnownMailFolder("SENTITEMS", seg));
	REQUIRE(seg == "sentitems");
	REQUIRE(WellKnownMailFolder("junkemail", seg));
	REQUIRE(WellKnownMailFolder("archive", seg));
	REQUIRE_FALSE(WellKnownMailFolder("Sent Items", seg)); // a display name: looked up
	REQUIRE_FALSE(WellKnownMailFolder("Invoices", seg));
	REQUIRE_FALSE(WellKnownMailFolder("", seg));
}

TEST_CASE("MailMessagesPath", "[mail]") {
	MailQuery q;
	q.mailbox = "/v1.0/me";
	auto path = MailMessagesPath(q);
	REQUIRE(path.rfind("/v1.0/me/messages?$select=", 0) == 0);
	REQUIRE(path.find("receivedDateTime") != std::string::npos);
	REQUIRE(path.find(",body,") == std::string::npos);
	REQUIRE(path.find("&$top=100") != std::string::npos);
	REQUIRE(path.find("&$orderby=receivedDateTime%20desc") != std::string::npos);
	REQUIRE(path.find("$filter") == std::string::npos);

	q.folder = "inbox";
	q.has_since = true;
	q.since_micros = 1793491200000000LL; // 2026-11-01T00:00:00Z
	q.body = true;
	q.page_size = 50;
	path = MailMessagesPath(q);
	REQUIRE(path.rfind("/v1.0/me/mailFolders/inbox/messages?$select=", 0) == 0);
	REQUIRE(path.find(",body") != std::string::npos);
	REQUIRE(path.find("&$filter=receivedDateTime%20ge%202026-11-01T00%3A00%3A00Z") != std::string::npos);
	REQUIRE(path.find("&$top=50") != std::string::npos);

	q.folder = "AAMk/AD=+x"; // a folder id: encoded
	path = MailMessagesPath(q);
	REQUIRE(path.find("/mailFolders/AAMk%2FAD%3D%2Bx/messages") != std::string::npos);
}
