#pragma once

#include <string>
#include <vector>

namespace duckdb {
namespace sharepoint {

// ---------------------------------------------------------------------------
// SharePoint / OneDrive browser URLs as paths (#95). PURE: no duckdb.hpp, no
// I/O, so the Catch2 binary compiles it directly.
//
//   https://contoso.sharepoint.com/sites/Sales/Shared%20Documents/2026/x.csv
//       DIRECT: a site, a document library and a path in it. Which library is
//       found by matching the libraries' webUrl against the URL (I/O, in the
//       filesystem); the rest is an ordinary root-relative path, so reads,
//       writes and globs all work.
//   https://contoso.sharepoint.com/:x:/s/Sales/EabC...?e=xyz
//       SHARING: a sharing link. Only Graph can say what it points at
//       (GET /shares/{SharesId(url)}/driveItem), so it addresses one existing
//       item, by id.
//   https://contoso.sharepoint.com/:x:/r/sites/Sales/Shared%20Documents/x.xlsx?d=...
//       A "redirect" link (r) carries the real path: read as DIRECT.
//   .../_layouts/15/Doc.aspx?sourcedoc={guid}&file=x.xlsx
//       The address bar of a document open in Office for the web: SHARING
//       (Graph resolves it).
// ---------------------------------------------------------------------------

enum class WebUrlKind { DIRECT, SHARING };

struct SharepointWebUrl {
	WebUrlKind kind = WebUrlKind::DIRECT;
	//! Lower-case, e.g. "contoso.sharepoint.com".
	std::string host;
	//! DIRECT: "sites/Sales", "teams/Ops", "personal/anna_contoso_com", or ""
	//! for the root site.
	std::string site_path;
	//! DIRECT: the percent-decoded segments after the site (library first).
	std::vector<std::string> rest;
	//! DIRECT: an earlier version, from a "version=<label>" query parameter
	//! (the form sharepoint_versions() returns for a URL); every other query
	//! parameter (web=1, d=..., e=...) is the browser's and ignored.
	std::string version;
	//! SHARING: the URL as given, for SharesId.
	std::string share_url;
};

struct SharepointWebUrlParse {
	bool ok = false;
	std::string error;
	SharepointWebUrl url;
};

//! Cheap test used by FileSystem::CanHandleFile: "https://" and a SharePoint
//! host (*.sharepoint.com, the US Gov *.sharepoint.us / *.sharepoint-mil.us,
//! or the China cloud *.sharepoint.cn). Case-insensitive.
bool IsSharepointWebUrl(const std::string &path);

//! Full parse. Never throws.
SharepointWebUrlParse ParseSharepointWebUrl(const std::string &url);

//! Graph's id for a sharing URL: "u!" + unpadded base64url of the URL.
std::string SharesId(const std::string &url);

//! How many leading segments of `url.rest` the library at `drive_web_url`
//! (Graph's drive webUrl, percent-encoded) covers, or -1 if it is not that
//! library. Host, site and library compare case-insensitively, as SharePoint
//! does. The library is the one with the most segments when several match.
int MatchDriveWebUrl(const SharepointWebUrl &url, const std::string &drive_web_url);

//! `segment` percent-encoded for a URL path (unreserved characters kept).
std::string PercentEncodeSegment(const std::string &segment);

//! `url` without its query and fragment.
std::string StripUrlQuery(const std::string &url);

//! %XX decoded; malformed escapes are kept as they are.
std::string PercentDecodeUrl(const std::string &s);

} // namespace sharepoint
} // namespace duckdb
