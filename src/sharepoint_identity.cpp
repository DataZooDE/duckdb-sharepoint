#include "sharepoint_identity.hpp"

#include <cstddef>
#include <initializer_list>

namespace duckdb {
namespace sharepoint {

namespace {

std::string Lower(std::string s) {
	for (auto &c : s) {
		if (c >= 'A' && c <= 'Z') {
			c = static_cast<char>(c - 'A' + 'a');
		}
	}
	return s;
}

// The lowercased host of "https://<host>[/]", or empty if `url` has any other
// shape (scheme, port, path, query, userinfo).
std::string HttpsBareHost(const std::string &url) {
	static const std::string kScheme = "https://";
	std::string u = Lower(url);
	if (u.compare(0, kScheme.size(), kScheme) != 0) {
		return std::string();
	}
	std::string host = u.substr(kScheme.size());
	if (!host.empty() && host.back() == '/') {
		host.pop_back();
	}
	if (host.empty() || host.find_first_of("/:?#@[] ") != std::string::npos) {
		return std::string();
	}
	return host;
}

bool HostIn(const std::string &host, std::initializer_list<const char *> allowed) {
	if (host.empty()) {
		return false;
	}
	for (const char *a : allowed) {
		if (host == a) {
			return true;
		}
	}
	return false;
}

} // namespace

bool IsAllowedGraphEndpoint(const std::string &url) {
	return HostIn(HttpsBareHost(url), {"graph.microsoft.com", "graph.microsoft.us", "dod-graph.microsoft.us",
	                                   "microsoftgraph.chinacloudapi.cn"});
}

bool IsAllowedLoginEndpoint(const std::string &url) {
	return HostIn(HttpsBareHost(url), {"login.microsoftonline.com", "login.microsoftonline.us",
	                                   "login.partner.microsoftonline.cn", "login.chinacloudapi.cn"});
}

namespace {

// The SharePoint host suffix (with its leading '.') for the Graph cloud
// `graph_endpoint` belongs to, or empty for an unknown endpoint. Suffixes are
// Microsoft's documented SharePoint Online endpoints per cloud (docs/hld.md 7a).
std::string SharePointSuffixFor(const std::string &graph_endpoint) {
	const std::string g = HttpsBareHost(graph_endpoint);
	if (g == "graph.microsoft.com") {
		return ".sharepoint.com";
	}
	if (g == "graph.microsoft.us") {
		return ".sharepoint.us";
	}
	if (g == "dod-graph.microsoft.us") {
		return ".sharepoint-mil.us";
	}
	if (g == "microsoftgraph.chinacloudapi.cn") {
		return ".sharepoint.cn";
	}
	return std::string();
}

bool IsHostLabelChar(char c) {
	return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-';
}

} // namespace

bool IsAllowedUploadUrl(const std::string &url, const std::string &graph_endpoint) {
	const std::string suffix = SharePointSuffixFor(graph_endpoint);
	if (suffix.empty()) {
		return false;
	}
	static const std::string kScheme = "https://";
	if (url.size() < kScheme.size() || Lower(url.substr(0, kScheme.size())) != kScheme) {
		return false;
	}
	// The authority runs to the first '/', '?' or '#'. It must be a bare
	// hostname: any '@' (userinfo), ':' (port), '[' (IPv6), '%' or '\' fails
	// the label-character check below.
	const std::size_t end = url.find_first_of("/?#", kScheme.size());
	const std::string host =
	    Lower(url.substr(kScheme.size(), end == std::string::npos ? std::string::npos : end - kScheme.size()));
	if (host.size() <= suffix.size() || host.compare(host.size() - suffix.size(), suffix.size(), suffix) != 0) {
		return false;
	}
	// Exactly one tenant label in front of the suffix ("contoso",
	// "contoso-my"): letters, digits and '-', not starting or ending with '-'.
	const std::string label = host.substr(0, host.size() - suffix.size());
	if (label.empty() || label.size() > 63 || label.front() == '-' || label.back() == '-') {
		return false;
	}
	for (char c : label) {
		if (!IsHostLabelChar(c)) {
			return false;
		}
	}
	return true;
}

std::string UrlOriginForDisplay(const std::string &url) {
	const std::size_t sep = url.find("://");
	if (sep == std::string::npos || sep == 0 || url.find_first_of("/?#") < sep) {
		return "<unparseable URL>";
	}
	const std::size_t start = sep + 3;
	const std::size_t end = url.find_first_of("/?#", start);
	std::string authority = url.substr(start, end == std::string::npos ? std::string::npos : end - start);
	const std::size_t at = authority.rfind('@');
	if (at != std::string::npos) {
		authority = authority.substr(at + 1);
	}
	return Lower(url.substr(0, sep)) + "://" + authority;
}

bool IsValidTenantId(const std::string &tenant) {
	if (tenant.empty() || tenant == "." || tenant == "..") {
		return false;
	}
	for (char c : tenant) {
		if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '.')) {
			return false;
		}
	}
	return true;
}

std::string FrameCredentialMaterial(const std::vector<std::string> &parts) {
	std::string out;
	for (const auto &p : parts) {
		out += std::to_string(p.size());
		out += ':';
		out += p;
		out += ';';
	}
	return out;
}

std::string DefaultGraphScope(const std::string &graph_endpoint) {
	std::string base = graph_endpoint;
	while (!base.empty() && base.back() == '/') {
		base.pop_back();
	}
	return base + "/.default";
}

} // namespace sharepoint
} // namespace duckdb
