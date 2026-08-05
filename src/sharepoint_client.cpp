// Microsoft Graph HTTP client.
//
// STATUS: network frontier of the scaffold (see sharepoint_auth.cpp's header).
// Request shapes follow the Graph v1.0 driveItem docs; none has yet run against
// a live tenant. The resumable upload-session path in PutContent is the least
// exercised and is marked inline.
#include "sharepoint_client.hpp"
#include "sharepoint_errors.hpp"

#include "duckdb/common/exception.hpp"

#define CPPHTTPLIB_OPENSSL_SUPPORT
#include "httplib.hpp"

#include <picojson.h>

namespace duckdb {
namespace sharepoint {

namespace {

// Percent-encode a root-relative path for the Graph `root:/{path}` form,
// leaving '/' as the segment separator. Graph rejects raw spaces and several
// reserved characters in the addressing segment.
string EncodePath(const string &path) {
	static const char *hex = "0123456789ABCDEF";
	string out;
	for (unsigned char c : path) {
		if (c == '/' || c == '-' || c == '_' || c == '.' || c == '~' || (c >= '0' && c <= '9') ||
		    (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z')) {
			out += static_cast<char>(c);
		} else {
			out += '%';
			out += hex[c >> 4];
			out += hex[c & 0xF];
		}
	}
	return out;
}

duckdb_httplib_openssl::Client MakeClient(const string &base_url) {
	duckdb_httplib_openssl::Client cli(base_url.c_str());
	cli.set_follow_location(true);
	cli.set_connection_timeout(30);
	cli.set_read_timeout(300);
	return cli;
}

// Extract Graph's error.code from a JSON error body (empty if none).
string ErrorCode(const string &body) {
	picojson::value v;
	if (picojson::parse(v, body).empty() && v.is<picojson::object>()) {
		auto &o = v.get<picojson::object>();
		auto it = o.find("error");
		if (it != o.end() && it->second.is<picojson::object>()) {
			auto &e = it->second.get<picojson::object>();
			auto c = e.find("code");
			if (c != e.end() && c->second.is<string>()) {
				return c->second.get<string>();
			}
		}
	}
	return string();
}

[[noreturn]] void ThrowGraph(const string &op, int status, const string &body) {
	auto kind = ClassifyGraphError(status, ErrorCode(body));
	throw IOException("sharepoint: %s failed: HTTP %d (%s)", op.c_str(), status, GraphErrorKindName(kind));
}

DriveItemMeta ParseMeta(const picojson::object &o) {
	DriveItemMeta m;
	m.found = true;
	auto get_str = [&](const char *k) -> string {
		auto it = o.find(k);
		return (it != o.end() && it->second.is<string>()) ? it->second.get<string>() : string();
	};
	m.id = get_str("id");
	m.name = get_str("name");
	m.ctag = get_str("cTag");
	m.etag = get_str("eTag");
	auto sz = o.find("size");
	if (sz != o.end() && sz->second.is<int64_t>()) {
		m.size = sz->second.get<int64_t>();
	}
	m.is_folder = o.find("folder") != o.end();
	return m;
}

} // namespace

DriveItemMeta GraphClient::StatByPath(const string &root_relative_path) {
	auto cli = MakeClient(auth.graph_endpoint);
	duckdb_httplib_openssl::Headers h = {{"Authorization", "Bearer " + auth.access_token}};
	string path = root_relative_path.empty()
	                  ? "/v1.0/drives/" + auth.drive_id + "/root?$select=id,name,size,file,folder,cTag,eTag"
	                  : "/v1.0/drives/" + auth.drive_id + "/root:/" + EncodePath(root_relative_path) +
	                        "?$select=id,name,size,file,folder,cTag,eTag";
	auto res = cli.Get(path.c_str(), h);
	if (!res) {
		throw IOException("sharepoint: stat '%s' failed at the transport layer", root_relative_path.c_str());
	}
	if (res->status == 404) {
		return DriveItemMeta {}; // found=false
	}
	if (res->status < 200 || res->status >= 300) {
		ThrowGraph("stat '" + root_relative_path + "'", res->status, res->body);
	}
	picojson::value v;
	if (!picojson::parse(v, res->body).empty() || !v.is<picojson::object>()) {
		throw IOException("sharepoint: unexpected stat response for '%s'", root_relative_path.c_str());
	}
	return ParseMeta(v.get<picojson::object>());
}

DriveItemMeta GraphClient::StatById(const string &item_id) {
	auto cli = MakeClient(auth.graph_endpoint);
	duckdb_httplib_openssl::Headers h = {{"Authorization", "Bearer " + auth.access_token}};
	string path = "/v1.0/drives/" + auth.drive_id + "/items/" + item_id +
	              "?$select=id,name,size,file,folder,cTag,eTag";
	auto res = cli.Get(path.c_str(), h);
	if (!res) {
		throw IOException("sharepoint: stat id '%s' failed at the transport layer", item_id.c_str());
	}
	if (res->status == 404) {
		return DriveItemMeta {};
	}
	if (res->status < 200 || res->status >= 300) {
		ThrowGraph("stat id '" + item_id + "'", res->status, res->body);
	}
	picojson::value v;
	if (!picojson::parse(v, res->body).empty() || !v.is<picojson::object>()) {
		throw IOException("sharepoint: unexpected stat response for id '%s'", item_id.c_str());
	}
	return ParseMeta(v.get<picojson::object>());
}

void GraphClient::ReadRange(const string &item_id, int64_t offset, int64_t length, string &out) {
	auto cli = MakeClient(auth.graph_endpoint);
	string range = "bytes=" + std::to_string(offset) + "-" + std::to_string(offset + length - 1);
	duckdb_httplib_openssl::Headers h = {{"Authorization", "Bearer " + auth.access_token}, {"Range", range}};
	string path = "/v1.0/drives/" + auth.drive_id + "/items/" + item_id + "/content";
	auto res = cli.Get(path.c_str(), h);
	if (!res) {
		throw IOException("sharepoint: ranged read of id '%s' failed at the transport layer", item_id.c_str());
	}
	// 206 Partial Content on a satisfied Range; 200 if the server ignored it.
	if (res->status != 200 && res->status != 206) {
		ThrowGraph("read id '" + item_id + "'", res->status, res->body);
	}
	out = std::move(res->body);
}

DriveItemMeta GraphClient::PutContent(const string &root_relative_path, const string &bytes, idx_t chunk_bytes) {
	auto cli = MakeClient(auth.graph_endpoint);
	duckdb_httplib_openssl::Headers h = {{"Authorization", "Bearer " + auth.access_token}};

	// Small file: a single PUT to the content endpoint. Graph permits up to
	// 250 MiB this way, but the resumable session is more robust past a few
	// MiB, so switch at the configured chunk size.
	if (bytes.size() <= chunk_bytes) {
		string path = "/v1.0/drives/" + auth.drive_id + "/root:/" + EncodePath(root_relative_path) + ":/content";
		auto res = cli.Put(path.c_str(), h, bytes, "application/octet-stream");
		if (!res) {
			throw IOException("sharepoint: upload of '%s' failed at the transport layer",
			                  root_relative_path.c_str());
		}
		if (res->status < 200 || res->status >= 300) {
			ThrowGraph("upload '" + root_relative_path + "'", res->status, res->body);
		}
		picojson::value v;
		if (!picojson::parse(v, res->body).empty() || !v.is<picojson::object>()) {
			throw IOException("sharepoint: upload of '%s' returned no item metadata",
			                  root_relative_path.c_str());
		}
		return ParseMeta(v.get<picojson::object>());
	}

	// Large file: resumable upload session.
	//   POST .../root:/{path}:/createUploadSession  -> { uploadUrl }
	//   then PUT each chunk to uploadUrl with a Content-Range header; the final
	//   chunk's response carries the driveItem.
	// Chunks MUST be a multiple of 320 KiB except the last (Graph contract).
	//
	// TODO(live): exercised only on paper so far. The uploadUrl is a
	// pre-authenticated absolute URL on a *different* host than graph_endpoint,
	// so it needs its own httplib client parsed from that URL, and NO
	// Authorization header (the URL carries the auth). Retry/resume on a 5xx
	// mid-session (query the session for expectedRanges) is not yet wired.
	{
		string path =
		    "/v1.0/drives/" + auth.drive_id + "/root:/" + EncodePath(root_relative_path) + ":/createUploadSession";
		string body = "{\"item\":{\"@microsoft.graph.conflictBehavior\":\"replace\"}}";
		auto res = cli.Post(path.c_str(), h, body, "application/json");
		if (!res || res->status < 200 || res->status >= 300) {
			ThrowGraph("createUploadSession '" + root_relative_path + "'", res ? res->status : 0,
			           res ? res->body : string());
		}
		picojson::value v;
		if (!picojson::parse(v, res->body).empty() || !v.is<picojson::object>()) {
			throw IOException("sharepoint: createUploadSession returned no uploadUrl");
		}
		auto url_it = v.get<picojson::object>().find("uploadUrl");
		if (url_it == v.get<picojson::object>().end() || !url_it->second.is<string>()) {
			throw IOException("sharepoint: createUploadSession returned no uploadUrl");
		}
		// The remainder (chunked PUT loop against the absolute uploadUrl) is the
		// one path still to be implemented and verified live.
		throw NotImplementedException(
		    "sharepoint: resumable upload for files larger than the chunk size (%llu bytes) is not yet "
		    "implemented in this scaffold; the createUploadSession call succeeds but the chunk PUT loop is a "
		    "TODO. Raise sharepoint_upload_chunk_bytes to cover the file as a single PUT in the meantime.",
		    (unsigned long long)chunk_bytes);
	}
}

bool GraphClient::DeleteItem(const string &item_id, bool permanent) {
	auto cli = MakeClient(auth.graph_endpoint);
	duckdb_httplib_openssl::Headers h = {{"Authorization", "Bearer " + auth.access_token}};
	// A plain DELETE moves the item to the recycle bin. "Permanent" delete has
	// no single documented Graph verb for SharePoint drives; the closest is
	// DELETE with the Prefer: permanent-delete header (OneDrive) -- kept behind
	// the flag and marked for live verification.
	if (permanent) {
		h.emplace("Prefer", "permanent-delete");
	}
	string path = "/v1.0/drives/" + auth.drive_id + "/items/" + item_id;
	auto res = cli.Delete(path.c_str(), h);
	if (!res) {
		throw IOException("sharepoint: delete of id '%s' failed at the transport layer", item_id.c_str());
	}
	if (res->status == 404) {
		return false;
	}
	if (res->status < 200 || res->status >= 300) {
		ThrowGraph("delete id '" + item_id + "'", res->status, res->body);
	}
	return true;
}

void GraphClient::MoveByPath(const string &source_root_relative, const string &target_root_relative) {
	// Resolve the source id, then PATCH its parentReference + name. Split the
	// target into parent path and final name.
	auto src = StatByPath(source_root_relative);
	if (!src.found) {
		throw IOException("sharepoint: move source '%s' does not exist", source_root_relative.c_str());
	}
	auto slash = target_root_relative.find_last_of('/');
	string parent = (slash == string::npos) ? string() : target_root_relative.substr(0, slash);
	string name = (slash == string::npos) ? target_root_relative : target_root_relative.substr(slash + 1);

	// parentReference by path: "/drives/{drive}/root:/{parent}"
	string parent_ref = "/drives/" + auth.drive_id + "/root:";
	if (!parent.empty()) {
		parent_ref += "/" + parent;
	}
	// A minimal JSON body. picojson could build it, but the shape is fixed.
	string body = "{\"parentReference\":{\"path\":\"" + parent_ref + "\"},\"name\":\"" + name + "\"}";

	auto cli = MakeClient(auth.graph_endpoint);
	duckdb_httplib_openssl::Headers h = {{"Authorization", "Bearer " + auth.access_token}};
	string path = "/v1.0/drives/" + auth.drive_id + "/items/" + src.id;
	auto res = cli.Patch(path.c_str(), h, body, "application/json");
	if (!res || res->status < 200 || res->status >= 300) {
		ThrowGraph("move '" + source_root_relative + "' -> '" + target_root_relative + "'",
		           res ? res->status : 0, res ? res->body : string());
	}
}

void GraphClient::CreateFolder(const string &root_relative_path) {
	auto slash = root_relative_path.find_last_of('/');
	string parent = (slash == string::npos) ? string() : root_relative_path.substr(0, slash);
	string name = (slash == string::npos) ? root_relative_path : root_relative_path.substr(slash + 1);

	auto cli = MakeClient(auth.graph_endpoint);
	duckdb_httplib_openssl::Headers h = {{"Authorization", "Bearer " + auth.access_token}};
	string path = parent.empty()
	                  ? "/v1.0/drives/" + auth.drive_id + "/root/children"
	                  : "/v1.0/drives/" + auth.drive_id + "/root:/" + EncodePath(parent) + ":/children";
	// conflictBehavior=fail so a pre-existing folder is a 409 we can tolerate,
	// never a duplicate.
	string body = "{\"name\":\"" + name +
	              "\",\"folder\":{},\"@microsoft.graph.conflictBehavior\":\"fail\"}";
	auto res = cli.Post(path.c_str(), h, body, "application/json");
	if (!res) {
		throw IOException("sharepoint: create folder '%s' failed at the transport layer",
		                  root_relative_path.c_str());
	}
	if (res->status == 409) {
		return; // already exists -- idempotent
	}
	if (res->status < 200 || res->status >= 300) {
		ThrowGraph("create folder '" + root_relative_path + "'", res->status, res->body);
	}
}

vector<ChildEntry> GraphClient::ListChildren(const string &folder_root_relative_path) {
	vector<ChildEntry> out;
	auto cli = MakeClient(auth.graph_endpoint);
	duckdb_httplib_openssl::Headers h = {{"Authorization", "Bearer " + auth.access_token}};
	string next = folder_root_relative_path.empty()
	                  ? "/v1.0/drives/" + auth.drive_id + "/root/children?$select=name,folder,file&$top=200"
	                  : "/v1.0/drives/" + auth.drive_id + "/root:/" + EncodePath(folder_root_relative_path) +
	                        ":/children?$select=name,folder,file&$top=200";
	while (!next.empty()) {
		auto res = cli.Get(next.c_str(), h);
		if (!res) {
			throw IOException("sharepoint: list of '%s' failed at the transport layer",
			                  folder_root_relative_path.c_str());
		}
		if (res->status == 404) {
			return out; // an absent folder lists as empty
		}
		if (res->status < 200 || res->status >= 300) {
			ThrowGraph("list '" + folder_root_relative_path + "'", res->status, res->body);
		}
		picojson::value v;
		if (!picojson::parse(v, res->body).empty() || !v.is<picojson::object>()) {
			throw IOException("sharepoint: unexpected children response");
		}
		auto &o = v.get<picojson::object>();
		auto vals = o.find("value");
		if (vals != o.end() && vals->second.is<picojson::array>()) {
			for (auto &e : vals->second.get<picojson::array>()) {
				if (!e.is<picojson::object>()) {
					continue;
				}
				auto &eo = e.get<picojson::object>();
				ChildEntry ce;
				auto nm = eo.find("name");
				if (nm != eo.end() && nm->second.is<string>()) {
					ce.name = nm->second.get<string>();
				}
				ce.is_folder = eo.find("folder") != eo.end();
				out.push_back(std::move(ce));
			}
		}
		// Paging: @odata.nextLink is an ABSOLUTE URL. httplib's Client is bound
		// to one host; a nextLink on the same Graph host works if we strip the
		// origin. Graph keeps the same host, so take the path+query.
		next.clear();
		auto nl = o.find("@odata.nextLink");
		if (nl != o.end() && nl->second.is<string>()) {
			string link = nl->second.get<string>();
			auto scheme = link.find("://");
			if (scheme != string::npos) {
				auto slash = link.find('/', scheme + 3);
				next = (slash == string::npos) ? string() : link.substr(slash);
			} else {
				next = link;
			}
		}
	}
	return out;
}

} // namespace sharepoint
} // namespace duckdb
