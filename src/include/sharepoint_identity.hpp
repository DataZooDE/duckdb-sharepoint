#pragma once

#include <string>
#include <vector>

namespace duckdb {
namespace sharepoint {

// ---------------------------------------------------------------------------
// Pure identity helpers (no I/O, no OpenSSL): which endpoints may receive
// credentials, and the unambiguous framing of credential material that
// sharepoint_auth.cpp hashes (SHA-256) into cache keys.
// ---------------------------------------------------------------------------

//! A Graph endpoint the extension will send bearer tokens to: https, an exact
//! Microsoft Graph host of a known cloud (global, US Gov, US DoD, China), no
//! port, path, query or userinfo (a trailing '/' is tolerated). The endpoint
//! is a session setting, so this is what stops a SQL user redirecting another
//! connection's tokens to a host they control.
bool IsAllowedGraphEndpoint(const std::string &url);

//! Same for the Entra ID login endpoint, which receives the CLIENT_SECRET.
bool IsAllowedLoginEndpoint(const std::string &url);

//! An upload session's uploadUrl the extension will send file content to
//! (PUT chunks) and query / commit / discard (GET / POST / DELETE). The URL is
//! pre-authenticated -- it is sent without a bearer token -- so this is what
//! keeps a tampered or unexpected uploadUrl from receiving the file. Accepted
//! only if it is https, its authority has no userinfo and no port, and its
//! host is "<one label>.<suffix>" for the SharePoint suffix of the cloud
//! `graph_endpoint` belongs to (docs/hld.md 7a):
//!   graph.microsoft.com             -> sharepoint.com
//!   graph.microsoft.us (GCC High)   -> sharepoint.us
//!   dod-graph.microsoft.us (DoD)    -> sharepoint-mil.us
//!   microsoftgraph.chinacloudapi.cn -> sharepoint.cn
//! A `graph_endpoint` that is no known Graph cloud allows nothing. Custom
//! (vanity) SharePoint domains are refused: Graph hands out the tenant's
//! *.sharepoint.<cloud> host for a drive, so none has been seen in practice.
bool IsAllowedUploadUrl(const std::string &url, const std::string &graph_endpoint);

//! "scheme://host[:port]" of `url` -- no userinfo, path, query or fragment --
//! for error messages about a pre-authenticated URL, whose signed query string
//! is a credential. "<unparseable URL>" if there is no "scheme://".
std::string UrlOriginForDisplay(const std::string &url);

//! TENANT_ID is a GUID or a domain ("contoso.onmicrosoft.com"): letters,
//! digits, '-' and '.', not "." / "..". It is spliced into the token URL path.
bool IsValidTenantId(const std::string &tenant);

//! Length-prefixed framing ("<len>:<value>;" per part) so distinct part lists
//! can never produce the same bytes -- unlike joining with a separator that
//! may itself occur in a value.
std::string FrameCredentialMaterial(const std::vector<std::string> &parts);

//! "<graph endpoint>/.default": the app-only scope for the cloud the Graph
//! endpoint belongs to (a global-cloud scope is wrong for US Gov / China).
std::string DefaultGraphScope(const std::string &graph_endpoint);

} // namespace sharepoint
} // namespace duckdb
