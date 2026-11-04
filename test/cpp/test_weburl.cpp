// Browser URLs as paths (#95): the pure parts -- which URLs are ours, how a
// direct URL splits into site / library / path, sharing links, and matching a
// library's webUrl. Resolving them against Graph is the e2e test.
#include "sharepoint_weburl.hpp"

#include <catch2/catch_test_macros.hpp>

#include <string>
#include <vector>

using namespace duckdb::sharepoint;

TEST_CASE("IsSharepointWebUrl: https and a SharePoint host only", "[weburl]") {
	REQUIRE(IsSharepointWebUrl("https://contoso.sharepoint.com/sites/Sales/Shared%20Documents/x.csv"));
	REQUIRE(IsSharepointWebUrl("HTTPS://Contoso.SharePoint.com/x.csv"));
	REQUIRE(IsSharepointWebUrl("https://contoso-my.sharepoint.com/personal/anna_contoso_com/Documents/x.csv"));
	REQUIRE(IsSharepointWebUrl("https://agency.sharepoint.us/sites/x/y.csv"));
	REQUIRE(IsSharepointWebUrl("https://contoso.sharepoint.cn/x"));
	REQUIRE(IsSharepointWebUrl("https://contoso.sharepoint.com"));
	REQUIRE_FALSE(IsSharepointWebUrl("http://contoso.sharepoint.com/x.csv"));          // never plain http
	REQUIRE_FALSE(IsSharepointWebUrl("https://example.com/x.sharepoint.com/x.csv"));   // host, not path
	REQUIRE_FALSE(IsSharepointWebUrl("https://sharepoint.com/x.csv"));                 // no tenant
	REQUIRE_FALSE(IsSharepointWebUrl("https://contoso.sharepoint.com.evil.io/x.csv")); // suffix must end the host
	REQUIRE_FALSE(IsSharepointWebUrl("sharepoint://Finance/x.csv"));
	REQUIRE_FALSE(IsSharepointWebUrl("s3://bucket/x.csv"));
}

TEST_CASE("ParseSharepointWebUrl: a direct URL splits into site and path", "[weburl]") {
	auto p = ParseSharepointWebUrl("https://Contoso.sharepoint.com/sites/Sales/Shared%20Documents/2026/Q%201.csv");
	REQUIRE(p.ok);
	REQUIRE(p.url.kind == WebUrlKind::DIRECT);
	REQUIRE(p.url.host == "contoso.sharepoint.com");
	REQUIRE(p.url.site_path == "sites/Sales");
	REQUIRE(p.url.rest == std::vector<std::string> {"Shared Documents", "2026", "Q 1.csv"});

	p = ParseSharepointWebUrl("https://contoso.sharepoint.com/teams/Ops/Docs/a.parquet?web=1#top");
	REQUIRE(p.ok);
	REQUIRE(p.url.site_path == "teams/Ops");
	REQUIRE(p.url.rest == std::vector<std::string> {"Docs", "a.parquet"}); // query and fragment dropped

	p = ParseSharepointWebUrl("https://contoso.sharepoint.com/Shared%20Documents//x.csv");
	REQUIRE(p.ok);
	REQUIRE(p.url.site_path.empty()); // the root site
	REQUIRE(p.url.rest == std::vector<std::string> {"Shared Documents", "x.csv"});

	p = ParseSharepointWebUrl("https://contoso-my.sharepoint.com/personal/anna_contoso_com/Documents/x.csv");
	REQUIRE(p.ok);
	REQUIRE(p.url.site_path == "personal/anna_contoso_com");
	REQUIRE(p.url.rest == std::vector<std::string> {"Documents", "x.csv"});
}

TEST_CASE("ParseSharepointWebUrl: sharing links and Office addresses go to Graph", "[weburl]") {
	const std::string link = "https://contoso.sharepoint.com/:x:/s/Sales/EabCdEf123?e=XyZ";
	auto p = ParseSharepointWebUrl(link);
	REQUIRE(p.ok);
	REQUIRE(p.url.kind == WebUrlKind::SHARING);
	REQUIRE(p.url.share_url == link); // whole, query included

	REQUIRE(ParseSharepointWebUrl("https://contoso.sharepoint.com/:f:/g/personal/x/EQ").url.kind ==
	        WebUrlKind::SHARING);
	REQUIRE(ParseSharepointWebUrl("https://contoso.sharepoint.com/sites/Sales/_layouts/15/Doc.aspx?"
	                              "sourcedoc=%7B1%7D&file=x.xlsx")
	            .url.kind == WebUrlKind::SHARING);

	// A redirect link (r) carries the real path.
	p = ParseSharepointWebUrl("https://contoso.sharepoint.com/:x:/r/sites/Sales/Shared%20Documents/x.xlsx?d=w1");
	REQUIRE(p.ok);
	REQUIRE(p.url.kind == WebUrlKind::DIRECT);
	REQUIRE(p.url.site_path == "sites/Sales");
	REQUIRE(p.url.rest == std::vector<std::string> {"Shared Documents", "x.xlsx"});

	REQUIRE_FALSE(ParseSharepointWebUrl("https://example.com/x").ok);
}

TEST_CASE("ParseSharepointWebUrl: version= names an earlier version; other parameters are ignored", "[weburl]") {
	auto p = ParseSharepointWebUrl("https://contoso.sharepoint.com/sites/Sales/Docs/x.csv?web=1&version=3.0#a");
	REQUIRE(p.ok);
	REQUIRE(p.url.version == "3.0");
	REQUIRE(p.url.rest == std::vector<std::string> {"Docs", "x.csv"});
	REQUIRE(ParseSharepointWebUrl("https://contoso.sharepoint.com/sites/Sales/Docs/x.csv?d=w1").url.version.empty());
	REQUIRE(StripUrlQuery("https://h.sharepoint.com/a/b.csv?version=1.0#x") == "https://h.sharepoint.com/a/b.csv");
	REQUIRE(StripUrlQuery("https://h.sharepoint.com/a/b.csv") == "https://h.sharepoint.com/a/b.csv");
}

TEST_CASE("SharesId: u! + unpadded base64url", "[weburl]") {
	// Graph's own example (Microsoft Graph docs, "Accessing shared items").
	REQUIRE(SharesId("https://onedrive.live.com/redir?resid=1231244193912!12&authKey=1201919!12921!1") ==
	        "u!aHR0cHM6Ly9vbmVkcml2ZS5saXZlLmNvbS9yZWRpcj9yZXNpZD0xMjMxMjQ0MTkzOTEyITEyJmF1dGhLZXk9MTIwMTkxOSExMjkyMSEx");
}

TEST_CASE("MatchDriveWebUrl: the library a URL is in", "[weburl]") {
	auto url = ParseSharepointWebUrl("https://contoso.sharepoint.com/sites/Sales/shared%20documents/2026/x.csv").url;
	REQUIRE(MatchDriveWebUrl(url, "https://contoso.sharepoint.com/sites/Sales/Shared%20Documents") == 1);
	REQUIRE(MatchDriveWebUrl(url, "https://contoso.sharepoint.com/sites/sales/Shared%20Documents/") == 1);
	REQUIRE(MatchDriveWebUrl(url, "https://contoso.sharepoint.com/sites/Sales/Reports") == -1);
	REQUIRE(MatchDriveWebUrl(url, "https://contoso.sharepoint.com/sites/Other/Shared%20Documents") == -1);
	REQUIRE(MatchDriveWebUrl(url, "https://fabrikam.sharepoint.com/sites/Sales/Shared%20Documents") == -1);
	REQUIRE(MatchDriveWebUrl(url, "https://contoso.sharepoint.com/sites/Sales") == -1); // a site, no library

	auto root = ParseSharepointWebUrl("https://contoso.sharepoint.com/Shared%20Documents/x.csv").url;
	REQUIRE(MatchDriveWebUrl(root, "https://contoso.sharepoint.com/Shared%20Documents") == 1);
	REQUIRE(MatchDriveWebUrl(root, "https://contoso.sharepoint.com/sites/Sales/Shared%20Documents") == -1);
}

TEST_CASE("Percent encoding round-trips a path segment", "[weburl]") {
	REQUIRE(PercentEncodeSegment("Q 1 & 2.csv") == "Q%201%20%26%202.csv");
	REQUIRE(PercentEncodeSegment("a-b_c.d~e") == "a-b_c.d~e");
	REQUIRE(PercentDecodeUrl(PercentEncodeSegment("Ä ö/x?")) == "Ä ö/x?");
	REQUIRE(PercentDecodeUrl("100%") == "100%"); // malformed escape kept
	REQUIRE(PercentDecodeUrl("%zz") == "%zz");
}
