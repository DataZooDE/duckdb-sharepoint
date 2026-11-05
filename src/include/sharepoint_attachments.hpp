#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace duckdb {
namespace sharepoint {

// ---------------------------------------------------------------------------
// read_sharepoint_list_attachments() (#97). Microsoft Graph v1.0 has no API
// for list-item attachments, so they come from SharePoint's own REST API
// (/_api/web/lists(guid'...')/items...AttachmentFiles). PURE (picojson, no
// duckdb.hpp): the request paths, a page of the answer, and what to say when
// SharePoint refuses.
// ---------------------------------------------------------------------------

//! A site as SharePoint's REST API addresses it.
struct SharepointSite {
	std::string host; // "contoso.sharepoint.com", lower case
	std::string path; // "/sites/Sales" (percent-encoded as in the webUrl), "" for the root site
};

//! From a site's webUrl (Graph's). False unless it is https on a SharePoint host.
bool SplitSiteWebUrl(const std::string &web_url, SharepointSite &out);

struct ListAttachment {
	int64_t item_id = 0;
	std::string file_name;
	std::string server_relative_url; // "/sites/Sales/Lists/Orders/Attachments/7/a.pdf", not encoded
};

//! The first page of a list's attachments: every item with its AttachmentFiles
//! (items without any give none), or one item's (`item_id` > 0).
std::string AttachmentItemsPath(const SharepointSite &site, const std::string &list_id, int64_t item_id);

//! One page of AttachmentItemsPath's answer (odata=nometadata). `item_id` > 0:
//! the answer is that item's AttachmentFiles. `next_path` gets the next
//! page's path when there is one on the same host (else empty). False, with
//! `error`, for a body of another shape.
bool ParseAttachmentPage(const std::string &body, const SharepointSite &site, int64_t item_id,
                         std::vector<ListAttachment> &out, std::string &next_path, std::string &error);

//! The request for one attachment's bytes.
std::string AttachmentContentPath(const SharepointSite &site, const std::string &server_relative_url);

//! The attachment's address in a browser.
std::string AttachmentWebUrl(const SharepointSite &site, const std::string &server_relative_url);

//! What to tell the user when SharePoint's REST API answers `status`.
//! `client_secret_app`: the token is an app-only one from a client secret,
//! which that API refuses.
std::string ExplainSharepointRestError(int status, const std::string &body, bool client_secret_app);

} // namespace sharepoint
} // namespace duckdb
