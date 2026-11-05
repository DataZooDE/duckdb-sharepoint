// read_sharepoint_list_attachments (#97): SharePoint REST paths, pages, errors.
#include "sharepoint_attachments.hpp"

#include <catch2/catch_test_macros.hpp>

using namespace duckdb::sharepoint;

namespace {

const std::string kList = "0e1f2a3b-4c5d-6e7f-8091-a2b3c4d5e6f7";

SharepointSite Site() {
	SharepointSite s;
	REQUIRE(SplitSiteWebUrl("https://Contoso.sharepoint.com/sites/My%20Sales/", s));
	return s;
}

} // namespace

TEST_CASE("SplitSiteWebUrl: host and site path, SharePoint hosts only", "[attachments]") {
	auto s = Site();
	REQUIRE(s.host == "contoso.sharepoint.com");
	REQUIRE(s.path == "/sites/My%20Sales");
	SharepointSite root;
	REQUIRE(SplitSiteWebUrl("https://contoso.sharepoint.com", root));
	REQUIRE(root.path.empty());
	REQUIRE_FALSE(SplitSiteWebUrl("https://evil.example.com/sites/x", root));
	REQUIRE_FALSE(SplitSiteWebUrl("http://contoso.sharepoint.com/sites/x", root));
}

TEST_CASE("AttachmentItemsPath: every item's, or one item's; a list id must be a GUID", "[attachments]") {
	REQUIRE(AttachmentItemsPath(Site(), kList, 0) ==
	        "/sites/My%20Sales/_api/web/lists(guid'" + kList +
	            "')/items?$select=Id,AttachmentFiles&$expand=AttachmentFiles&$top=1000");
	REQUIRE(AttachmentItemsPath(Site(), kList, 7) ==
	        "/sites/My%20Sales/_api/web/lists(guid'" + kList + "')/items(7)/AttachmentFiles");
	REQUIRE(AttachmentItemsPath(Site(), "x')/foo", 0).empty());
}

TEST_CASE("ParseAttachmentPage: items with their files, and the next page on the same host", "[attachments]") {
	const std::string body = R"({"value":[
	  {"AttachmentFiles":[{"FileName":"a.pdf","ServerRelativeUrl":"/sites/My Sales/Lists/O/Attachments/1/a.pdf"},
	                      {"FileName":"b c.txt","ServerRelativeUrl":"/sites/My Sales/Lists/O/Attachments/1/b c.txt"}],
	   "Id":1,"ID":1},
	  {"AttachmentFiles":[],"Id":2,"ID":2}],
	  "odata.nextLink":"https://contoso.sharepoint.com/sites/My%20Sales/_api/web/lists(guid'x')/items?%24skiptoken=Paged%3dTRUE"})";
	std::vector<ListAttachment> out;
	std::string next, error;
	REQUIRE(ParseAttachmentPage(body, Site(), 0, out, next, error));
	REQUIRE(out.size() == 2);
	REQUIRE(out[1].item_id == 1);
	REQUIRE(out[1].file_name == "b c.txt");
	REQUIRE(next == "/sites/My%20Sales/_api/web/lists(guid'x')/items?%24skiptoken=Paged%3dTRUE");

	// Another host's link is never followed (it would get the token).
	REQUIRE(ParseAttachmentPage(R"({"value":[],"odata.nextLink":"https://evil.example.com/x"})", Site(), 0, out, next,
	                            error));
	REQUIRE(next.empty());

	out.clear();
	REQUIRE(ParseAttachmentPage(R"({"value":[{"FileName":"x.png","ServerRelativeUrl":"/u/x.png"}]})", Site(), 9, out,
	                            next, error));
	REQUIRE(out.size() == 1);
	REQUIRE(out[0].item_id == 9);
	REQUIRE_FALSE(ParseAttachmentPage("<html/>", Site(), 0, out, next, error));
}

TEST_CASE("AttachmentContentPath and AttachmentWebUrl: encoded, quotes doubled", "[attachments]") {
	REQUIRE(AttachmentContentPath(Site(), "/sites/My Sales/Lists/O/Attachments/1/it's #1.txt") ==
	        "/sites/My%20Sales/_api/web/GetFileByServerRelativePath(decodedurl='"
	        "/sites/My%20Sales/Lists/O/Attachments/1/it%27%27s%20%231.txt')/$value");
	REQUIRE(AttachmentWebUrl(Site(), "/sites/My Sales/Lists/O/Attachments/1/a b.pdf") ==
	        "https://contoso.sharepoint.com/sites/My%20Sales/Lists/O/Attachments/1/a%20b.pdf");
}

TEST_CASE("ExplainSharepointRestError: the fix for a client secret, a missing permission", "[attachments]") {
	const auto secret = ExplainSharepointRestError(401, R"({"error_description":"Unsupported app only token."})", true);
	REQUIRE(secret.find("Unsupported app only token.") != std::string::npos);
	REQUIRE(secret.find("client_certificate") != std::string::npos);
	const auto perm = ExplainSharepointRestError(
	    403, R"({"odata.error":{"code":"-2147024891","message":{"lang":"en-US","value":"Access denied."}}})", false);
	REQUIRE(perm.find("Access denied.") != std::string::npos);
	REQUIRE(perm.find("Office 365 SharePoint Online") != std::string::npos);
	REQUIRE(ExplainSharepointRestError(404, "", false).find("no such list") != std::string::npos);
	REQUIRE(ExplainSharepointRestError(500, "boom", false) == "SharePoint answered HTTP 500: boom");
}
