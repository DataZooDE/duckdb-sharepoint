// read_sharepoint_list_attachments() (#97): SharePoint REST paths, pages and
// errors. Pure: no duckdb.hpp and no I/O (see sharepoint_attachments.hpp).
#include "sharepoint_attachments.hpp"
#include "sharepoint_weburl.hpp"

#include <cctype>
#include <cstdint>

#include <picojson.h>

namespace duckdb {
namespace sharepoint {

namespace {

std::string Str(const picojson::object &o, const char *key) {
	auto it = o.find(key);
	return it != o.end() && it->second.is<std::string>() ? it->second.get<std::string>() : std::string();
}

bool Int(const picojson::object &o, const char *key, int64_t &out) {
	auto it = o.find(key);
	if (it == o.end()) {
		return false;
	}
	if (it->second.is<int64_t>()) {
		out = it->second.get<int64_t>();
		return true;
	}
	if (it->second.is<double>()) {
		out = static_cast<int64_t>(it->second.get<double>());
		return true;
	}
	return false;
}

//! Percent-encoded for a URL path; '/' kept.
std::string EncodePath(const std::string &path) {
	std::string out;
	std::size_t start = 0;
	while (true) {
		const auto slash = path.find('/', start);
		out += PercentEncodeSegment(path.substr(start, slash == std::string::npos ? std::string::npos : slash - start));
		if (slash == std::string::npos) {
			return out;
		}
		out += '/';
		start = slash + 1;
	}
}

std::string Lower(std::string s) {
	for (auto &c : s) {
		if (c >= 'A' && c <= 'Z') {
			c = static_cast<char>(c - 'A' + 'a');
		}
	}
	return s;
}

//! A list id is a GUID: anything else must not reach an OData literal.
bool IsGuid(const std::string &id) {
	if (id.size() != 36) {
		return false;
	}
	for (std::size_t i = 0; i < id.size(); i++) {
		const char c = id[i];
		const bool dash = i == 8 || i == 13 || i == 18 || i == 23;
		if (dash ? c != '-' : !std::isxdigit(static_cast<unsigned char>(c))) {
			return false;
		}
	}
	return true;
}

void Attachments(const picojson::array &files, int64_t item_id, std::vector<ListAttachment> &out) {
	for (auto &f : files) {
		if (!f.is<picojson::object>()) {
			continue;
		}
		ListAttachment a;
		a.item_id = item_id;
		a.file_name = Str(f.get<picojson::object>(), "FileName");
		a.server_relative_url = Str(f.get<picojson::object>(), "ServerRelativeUrl");
		if (!a.file_name.empty()) {
			out.push_back(std::move(a));
		}
	}
}

} // namespace

bool SplitSiteWebUrl(const std::string &web_url, SharepointSite &out) {
	if (!IsSharepointWebUrl(web_url)) {
		return false;
	}
	const auto path_at = web_url.find('/', 8);
	out.host = Lower(web_url.substr(8, path_at == std::string::npos ? std::string::npos : path_at - 8));
	out.path = path_at == std::string::npos ? std::string() : StripUrlQuery(web_url.substr(path_at));
	while (!out.path.empty() && out.path.back() == '/') {
		out.path.pop_back();
	}
	return true;
}

std::string AttachmentItemsPath(const SharepointSite &site, const std::string &list_id, int64_t item_id) {
	if (!IsGuid(list_id)) {
		return std::string();
	}
	const std::string list = site.path + "/_api/web/lists(guid'" + list_id + "')/items";
	if (item_id > 0) {
		return list + "(" + std::to_string(item_id) + ")/AttachmentFiles";
	}
	return list + "?$select=Id,AttachmentFiles&$expand=AttachmentFiles&$top=1000";
}

bool ParseAttachmentPage(const std::string &body, const SharepointSite &site, int64_t item_id,
                         std::vector<ListAttachment> &out, std::string &next_path, std::string &error) {
	next_path.clear();
	picojson::value v;
	const auto parse_error = picojson::parse(v, body);
	if (!parse_error.empty() || !v.is<picojson::object>()) {
		error = "SharePoint's answer is not JSON";
		return false;
	}
	const auto &o = v.get<picojson::object>();
	auto values = o.find("value");
	if (values == o.end() || !values->second.is<picojson::array>()) {
		error = "SharePoint's answer has no \"value\" list";
		return false;
	}
	if (item_id > 0) {
		Attachments(values->second.get<picojson::array>(), item_id, out);
	} else {
		for (auto &item : values->second.get<picojson::array>()) {
			if (!item.is<picojson::object>()) {
				continue;
			}
			const auto &i = item.get<picojson::object>();
			int64_t id = 0;
			auto files = i.find("AttachmentFiles");
			if ((Int(i, "Id", id) || Int(i, "ID", id)) && files != i.end() && files->second.is<picojson::array>()) {
				Attachments(files->second.get<picojson::array>(), id, out);
			}
		}
	}
	std::string next = Str(o, "odata.nextLink");
	if (next.empty()) {
		next = Str(o, "@odata.nextLink");
	}
	const std::string origin = "https://" + site.host + "/";
	if (next.size() > origin.size() && Lower(next.substr(0, origin.size())) == origin) {
		next_path = next.substr(origin.size() - 1);
	}
	return true;
}

std::string AttachmentContentPath(const SharepointSite &site, const std::string &server_relative_url) {
	std::string quoted; // an OData string literal doubles its quotes
	for (char c : server_relative_url) {
		quoted += c;
		if (c == '\'') {
			quoted += '\'';
		}
	}
	return site.path + "/_api/web/GetFileByServerRelativePath(decodedurl='" + EncodePath(quoted) + "')/$value";
}

std::string AttachmentWebUrl(const SharepointSite &site, const std::string &server_relative_url) {
	return "https://" + site.host + EncodePath(server_relative_url);
}

std::string ExplainSharepointRestError(int status, const std::string &body, bool client_secret_app) {
	// SharePoint answers in several shapes: OData ("odata.error"), the newer
	// "error", or the token check's "error_description"; else plain text.
	std::string said;
	picojson::value v;
	if (picojson::parse(v, body).empty() && v.is<picojson::object>()) {
		const auto &o = v.get<picojson::object>();
		for (const char *key : {"odata.error", "error"}) {
			auto e = o.find(key);
			if (e == o.end() || !e->second.is<picojson::object>()) {
				continue;
			}
			const auto &eo = e->second.get<picojson::object>();
			auto m = eo.find("message");
			if (m != eo.end() && m->second.is<picojson::object>()) {
				said = Str(m->second.get<picojson::object>(), "value");
			} else {
				said = Str(eo, "message");
			}
		}
		if (said.empty()) {
			said = Str(o, "error_description");
		}
	} else {
		said = body.substr(0, 200);
	}
	const std::string answer = "HTTP " + std::to_string(status) + (said.empty() ? "" : ": " + said);
	if ((status == 401 || status == 403) && client_secret_app) {
		return "SharePoint refused (" + answer +
		       "). List attachments come from SharePoint's own API (Microsoft Graph has none), which does not accept "
		       "app-only tokens from a client secret: use PROVIDER client_certificate with the same app (README: "
		       "Certificate), or a user sign-in (device_code / authorization_code)";
	}
	if (status == 401 || status == 403) {
		return "SharePoint refused (" + answer +
		       "). List attachments come from SharePoint's own API (Microsoft Graph has none): the app needs a "
		       "permission of the SharePoint API itself -- \"Office 365 SharePoint Online\": Sites.Read.All, or "
		       "Sites.Selected with a grant -- not only Microsoft Graph's; a user needs access to the list";
	}
	if (status == 404) {
		return "SharePoint found no such list, item or attachment (" + answer + ")";
	}
	return "SharePoint answered " + answer;
}

} // namespace sharepoint
} // namespace duckdb
