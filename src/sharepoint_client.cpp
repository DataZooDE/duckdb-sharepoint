// Microsoft Graph HTTP client. Every request shape here is proven against a
// real tenant by the live SQL suite and the e2e harness; the non-obvious Graph
// behaviours it works around are recorded in docs/hld.md section 7a.
#include "sharepoint_client.hpp"
#include "sharepoint_counters.hpp"
#include "sharepoint_errors.hpp"
#include "sharepoint_preauth.hpp"
#include "sharepoint_rawhttp.hpp"
#include "sharepoint_upload.hpp"

#include "duckdb/common/exception.hpp"
#include "duckdb/common/unique_ptr.hpp"
#include "duckdb/common/unordered_map.hpp"

#define CPPHTTPLIB_OPENSSL_SUPPORT
#include "httplib.hpp"

#include <picojson.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <random>
#include <thread>

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

// One keep-alive connection per Graph origin per thread, as in the sibling
// gdrive: a TLS handshake per request roughly doubles the latency of every
// small call (stat, list, a cached block miss). Per thread, so no locking and
// no connection is ever shared between concurrent requests.
//
// The holder exists for its destructor. At thread exit httplib closes TLS
// GRACEFULLY: it sends close_notify and then blocks reading the peer's reply
// on a socket with no receive timeout. Microsoft's front door never replies,
// so every DuckDB worker thread hung for ~120 s at shutdown (found with gdb:
// __call_tls_dtors -> ssl3_shutdown -> read). Shutting down the READ side
// first makes that read return EOF at once; writes are untouched, so there is
// no SIGPIPE risk.
struct GraphConnectionHolder {
	unique_ptr<duckdb_httplib_openssl::Client> client;
	~GraphConnectionHolder() {
		if (!client) {
			return;
		}
		const auto sock = client->socket();
		if (sock != INVALID_SOCKET) {
#ifdef _WIN32
			::shutdown(sock, SD_RECEIVE);
#else
			::shutdown(sock, SHUT_RD);
#endif
		}
	}
};

duckdb_httplib_openssl::Client &GraphConnection(const string &base_url) {
	static thread_local unordered_map<string, GraphConnectionHolder> connections;
	auto &holder = connections[base_url];
	if (!holder.client) {
		holder.client = make_uniq<duckdb_httplib_openssl::Client>(base_url.c_str());
		// Redirects are handled explicitly where Graph issues them (the
		// /content download): the target is a pre-authenticated URL that must
		// be https and must not receive the bearer token.
		holder.client->set_follow_location(false);
		holder.client->set_keep_alive(true);
		holder.client->set_connection_timeout(30);
		holder.client->set_read_timeout(300);
	}
	return *holder.client;
}

// Graph's error.code and error.message from a JSON error body (empty if
// absent or not JSON).
void ParseGraphError(const string &body, string &code, string &message) {
	code.clear();
	message.clear();
	picojson::value v;
	if (!picojson::parse(v, body).empty() || !v.is<picojson::object>()) {
		return;
	}
	auto &o = v.get<picojson::object>();
	auto it = o.find("error");
	if (it == o.end() || !it->second.is<picojson::object>()) {
		return;
	}
	auto &e = it->second.get<picojson::object>();
	auto c = e.find("code");
	if (c != e.end() && c->second.is<string>()) {
		code = c->second.get<string>();
	}
	auto m = e.find("message");
	if (m != e.end() && m->second.is<string>()) {
		message = m->second.get<string>();
	}
}

string ErrorCode(const string &body) {
	string code, message;
	ParseGraphError(body, code, message);
	return code;
}

// Throw the DuckDB exception type the error's kind maps to, with Graph's code
// and message: "403 accessDenied" reads very differently from "HTTP 403".
[[noreturn]] void ThrowGraph(const string &op, int status, const string &body) {
	string code, message;
	ParseGraphError(body, code, message);
	const string text = FormatGraphError(op, status, code, message);
	switch (ExceptionTypeFor(ClassifyGraphError(status, code))) {
	case GraphExceptionType::PERMISSION:
		throw PermissionException(text);
	case GraphExceptionType::INVALID_INPUT:
		throw InvalidInputException(text);
	default:
		throw IOException(text);
	}
}

// Send a Graph request, retrying transport failures, 5xx and throttling
// (429/503, Graph's throttling codes) with backoff -- Graph throttles
// SharePoint aggressively, and one 429 must not fail a whole scan. Every
// request routed here is safe to repeat: GET/DELETE/PUT-replace are
// idempotent, folder creation tolerates the 409 of a lost first response,
// and a move or upload-session POST that is repeated lands in the same state.
//
// A 401 means the token was revoked or expired early: the token source mints
// a fresh one (once) and the request is re-sent with it. `h` carries the
// Authorization header and is updated in place.
template <class Send>
duckdb_httplib_openssl::Result SendWithRetry(TokenSource &tokens, duckdb_httplib_openssl::Headers &h, Send send) {
	bool refreshed = false;
	for (int attempt = 1;; attempt++) {
		auto res = send();
		const auto kind = res ? ClassifyGraphError(res->status, ErrorCode(res->body)) : GraphErrorKind::OTHER;
		if (kind == GraphErrorKind::UNAUTHENTICATED && !refreshed) {
			refreshed = true;
			IncrementCounter(Counter::AUTH_REFRESH);
			string rejected;
			auto auth_header = h.find("Authorization");
			if (auth_header != h.end() && auth_header->second.size() > 7) {
				rejected = auth_header->second.substr(7); // after "Bearer "
			}
			h.erase("Authorization");
			h.emplace("Authorization", "Bearer " + tokens.Refresh(rejected));
			attempt--; // a refresh is not a failed attempt
			continue;
		}
		if (kind == GraphErrorKind::THROTTLED) {
			IncrementCounter(Counter::THROTTLED);
		}
		const bool retryable = !res || IsRetryable(kind);
		if (!retryable || attempt >= kMaxGraphAttempts) {
			return res;
		}
		IncrementCounter(Counter::RETRY);
		SleepBeforeRetry(attempt, res ? ParseRetryAfterSeconds(res->get_header_value("Retry-After")) : 0);
	}
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
	m.last_modified = get_str("lastModifiedDateTime");
	auto sz = o.find("size");
	if (sz != o.end() && sz->second.is<int64_t>()) {
		m.size = sz->second.get<int64_t>();
	}
	m.is_folder = o.find("folder") != o.end();
	return m;
}

//! How many times a single resumable-upload chunk may fail (transport error,
//! 5xx or 429) before the upload is abandoned. Each retry first asks the
//! session where to resume from.
constexpr int kMaxUploadChunkRetries = 5;

//! Timeout for one request to a pre-authenticated URL (download or upload
//! chunk), matching the Graph client's read timeout.
constexpr int kPreauthTimeoutSeconds = 300;

//! Body cap for upload-session replies (small JSON: a driveItem or the next
//! expected ranges).
constexpr std::size_t kUploadReplyMaxBytes = 1 << 20;

//! GET an upload session's state. Sent verbatim, without an Authorization
//! header -- the uploadUrl is pre-authenticated. On success `complete` says
//! the session holds every byte (nextExpectedRanges is empty, as after the
//! final chunk of a deferCommit session), else `next` is the first byte Graph
//! still expects. Returns false if the request fails or the body is
//! unexpected, leaving both untouched.
bool QueryUploadSession(const string &upload_url, uint64_t &next, bool &complete) {
	RawHttpResponse res;
	try {
		res = PreauthenticatedRequest("GET", upload_url, {}, "", kPreauthTimeoutSeconds, kUploadReplyMaxBytes);
	} catch (std::exception &) {
		return false;
	}
	if (res.status < 200 || res.status >= 300) {
		return false;
	}
	picojson::value v;
	if (!picojson::parse(v, res.body).empty() || !v.is<picojson::object>()) {
		return false;
	}
	auto &o = v.get<picojson::object>();
	auto it = o.find("nextExpectedRanges");
	if (it == o.end() || !it->second.is<picojson::array>()) {
		return false;
	}
	auto &arr = it->second.get<picojson::array>();
	if (arr.empty()) {
		complete = true;
		return true;
	}
	uint64_t start = 0;
	if (!arr[0].is<string>() || !ParseRangeStart(arr[0].get<string>(), start)) {
		return false;
	}
	complete = false;
	next = start;
	return true;
}

} // namespace

// Per-thread jitter so concurrent scan threads spread out.
void SleepBeforeRetry(int failed_attempt, int retry_after_seconds) {
	static thread_local std::mt19937 rng(std::random_device {}());
	std::uniform_real_distribution<double> jitter(0.75, 1.25);
	std::this_thread::sleep_for(
	    std::chrono::milliseconds(BackoffDelayMs(failed_attempt, retry_after_seconds, jitter(rng))));
}

DriveItemMeta GraphClient::StatByPath(const string &root_relative_path) {
	IncrementCounter(Counter::STAT);
	auto &cli = GraphConnection(auth.graph_endpoint);
	duckdb_httplib_openssl::Headers h = {{"Authorization", "Bearer " + auth.tokens->Token()}};
	string path = root_relative_path.empty()
	                  ? "/v1.0/drives/" + auth.drive_id + "/root?$select=id,name,size,file,folder,cTag,eTag,lastModifiedDateTime"
	                  : "/v1.0/drives/" + auth.drive_id + "/root:/" + EncodePath(root_relative_path) +
	                        "?$select=id,name,size,file,folder,cTag,eTag,lastModifiedDateTime";
	auto res = SendWithRetry(*auth.tokens, h, [&] { return cli.Get(path.c_str(), h); });
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
	IncrementCounter(Counter::STAT);
	auto &cli = GraphConnection(auth.graph_endpoint);
	duckdb_httplib_openssl::Headers h = {{"Authorization", "Bearer " + auth.tokens->Token()}};
	string path = "/v1.0/drives/" + auth.drive_id + "/items/" + item_id +
	              "?$select=id,name,size,file,folder,cTag,eTag,lastModifiedDateTime";
	auto res = SendWithRetry(*auth.tokens, h, [&] { return cli.Get(path.c_str(), h); });
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
	out.clear();
	if (length <= 0) {
		return; // "bytes=o-(o-1)" is not a valid range; nothing to fetch.
	}
	IncrementCounter(Counter::READ_RANGE);
	auto &cli = GraphConnection(auth.graph_endpoint);
	const string range = "bytes=" + std::to_string(offset) + "-" + std::to_string(offset + length - 1);
	const string path = "/v1.0/drives/" + auth.drive_id + "/items/" + item_id + "/content";
	// Body cap for the download: a 206 must be exactly `length`. A host that
	// ignores Range answers 200 with the WHOLE file; that is only tolerated
	// (and sliced) when the range ends within the first 64 MiB -- otherwise one
	// block read could buffer gigabytes.
	constexpr uint64_t kMaxWholeFileFallback = 64ULL * 1024 * 1024;
	const uint64_t range_end = static_cast<uint64_t>(offset) + static_cast<uint64_t>(length);
	const std::size_t body_cap =
	    static_cast<std::size_t>(range_end <= kMaxWholeFileFallback ? range_end : static_cast<uint64_t>(length));

	int status = 0;
	string content_range, body;
	// Round 2 exists for one case: the signed download URL from round 1 expired
	// (the download host says 401/403) -- e.g. a long backoff sequence on a
	// throttled host. A fresh Graph GET yields a fresh redirect.
	for (int round = 1; round <= 2; round++) {
		duckdb_httplib_openssl::Headers h = {{"Authorization", "Bearer " + auth.tokens->Token()}, {"Range", range}};
		auto res = SendWithRetry(*auth.tokens, h, [&] { return cli.Get(path.c_str(), h); });
		if (!res) {
			throw IOException("sharepoint: ranged read of id '%s' failed at the transport layer", item_id.c_str());
		}
		status = res->status;
		content_range = res->get_header_value("Content-Range");
		body = std::move(res->body);
		if (status < 300 || status >= 400) {
			break;
		}
		// Graph answers /content with a 302 to a short-lived pre-authenticated
		// download URL on the tenant's SharePoint host. Follow it ourselves: over
		// https only, carrying the Range but NOT the bearer token (the URL's
		// tempauth query is the credential), and verbatim -- it is signed over
		// its exact query string, which httplib 0.27 would re-sort (401).
		const string location = res->get_header_value("Location");
		if (!IsHttpsUrl(location)) {
			throw IOException("sharepoint: read id '%s' redirected to a non-https location; refusing to follow",
			                  item_id.c_str());
		}
		// The download host throttles and fails like Graph does; the signed URL
		// stays valid for minutes, so the same request can simply be repeated.
		RawHttpResponse dl;
		for (int attempt = 1;; attempt++) {
			string transport_error;
			try {
				dl = PreauthenticatedRequest("GET", location, {{"Range", range}}, "", kPreauthTimeoutSeconds,
				                             body_cap);
			} catch (IOException &e) {
				transport_error = e.what();
			}
			const bool retryable =
			    !transport_error.empty() || IsRetryable(ClassifyGraphError(dl.status, ErrorCode(dl.body)));
			if (!retryable) {
				break;
			}
			if (attempt >= kMaxGraphAttempts) {
				if (!transport_error.empty()) {
					throw IOException(transport_error);
				}
				break;
			}
			IncrementCounter(Counter::RETRY);
			SleepBeforeRetry(attempt, transport_error.empty() ? ParseRetryAfterSeconds(dl.Header("Retry-After")) : 0);
		}
		status = dl.status;
		content_range = dl.Header("Content-Range");
		body = std::move(dl.body);
		if ((status == 401 || status == 403) && round == 1) {
			IncrementCounter(Counter::RETRY);
			continue; // the signed URL expired: ask Graph for a fresh one
		}
		break;
	}
	// 206 must be exactly our range; a 200 carries the whole file and is sliced
	// (returning it as-is would hand a Parquet footer read the file's header).
	if (status == 200 || status == 206) {
		if (!AcceptRangeReply(status, content_range, body, static_cast<uint64_t>(offset),
		                      static_cast<uint64_t>(length))) {
			throw IOException("sharepoint: read id '%s' returned HTTP %d that does not cover bytes %lld-%lld "
			                  "(got %llu bytes)",
			                  item_id.c_str(), status, (long long)offset, (long long)(offset + length - 1),
			                  (unsigned long long)body.size());
		}
		out = std::move(body);
		IncrementCounter(Counter::BYTES_READ, out.size());
		return;
	}
	ThrowGraph("read id '" + item_id + "'", status, body);
}

DriveItemMeta GraphClient::PutContent(const string &root_relative_path, const string &bytes) {
	// A file no larger than one upload chunk: a single PUT to the content
	// endpoint. Larger files never get here -- the write handle streams them
	// through an upload session chunk by chunk (sharepoint_filesystem.cpp), so
	// `bytes` is at most sharepoint_upload_chunk_bytes, itself normalised under
	// Graph's 60 MiB request cap.
	auto &cli = GraphConnection(auth.graph_endpoint);
	duckdb_httplib_openssl::Headers h = {{"Authorization", "Bearer " + auth.tokens->Token()}};
	string path = "/v1.0/drives/" + auth.drive_id + "/root:/" + EncodePath(root_relative_path) + ":/content";
	IncrementCounter(Counter::UPLOAD_PUT);
	IncrementCounter(Counter::BYTES_WRITTEN, bytes.size());
	auto res = SendWithRetry(*auth.tokens, h, [&] { return cli.Put(path.c_str(), h, bytes, "application/octet-stream"); });
	if (!res) {
		throw IOException("sharepoint: upload of '%s' failed at the transport layer", root_relative_path.c_str());
	}
	if (res->status < 200 || res->status >= 300) {
		ThrowGraph("upload '" + root_relative_path + "'", res->status, res->body);
	}
	picojson::value v;
	if (!picojson::parse(v, res->body).empty() || !v.is<picojson::object>()) {
		throw IOException("sharepoint: upload of '%s' returned no item metadata", root_relative_path.c_str());
	}
	return ParseMeta(v.get<picojson::object>());
}

void GraphClient::OpenUploadSession(const string &root_relative_path, GraphUploadSession &session) {
	// What was there before, so a lost reply to the COMMIT can be told apart
	// from a failed upload (see CommitUploadSession).
	session.before = StatByPath(root_relative_path);
	session.root_relative_path = root_relative_path;

	// deferCommit: Graph stores the chunks but publishes NOTHING until an
	// explicit commit POST. That is what lets a streaming write (whose total
	// size is unknown until close) send its non-final chunks with a
	// "bytes a-b/*" Content-Range -- WITHOUT deferCommit Graph refuses "*"
	// ("Content-Range must include a total length"), verified 2026-10-14 --
	// and it keeps an overwritten file's OLD content visible until the commit.
	auto &cli = GraphConnection(auth.graph_endpoint);
	duckdb_httplib_openssl::Headers h = {{"Authorization", "Bearer " + auth.tokens->Token()}};
	string session_path =
	    "/v1.0/drives/" + auth.drive_id + "/root:/" + EncodePath(root_relative_path) + ":/createUploadSession";
	string session_body = "{\"item\":{\"@microsoft.graph.conflictBehavior\":\"replace\"},\"deferCommit\":true}";
	IncrementCounter(Counter::UPLOAD_SESSION);
	auto sres = SendWithRetry(*auth.tokens, h,
	                          [&] { return cli.Post(session_path.c_str(), h, session_body, "application/json"); });
	if (!sres || sres->status < 200 || sres->status >= 300) {
		ThrowGraph("createUploadSession '" + root_relative_path + "'", sres ? sres->status : 0,
		           sres ? sres->body : string());
	}
	picojson::value v;
	if (!picojson::parse(v, sres->body).empty() || !v.is<picojson::object>()) {
		throw IOException("sharepoint: createUploadSession returned no uploadUrl");
	}
	auto &o = v.get<picojson::object>();
	auto url_it = o.find("uploadUrl");
	if (url_it == o.end() || !url_it->second.is<string>()) {
		throw IOException("sharepoint: createUploadSession returned no uploadUrl");
	}
	// The uploadUrl is a pre-authenticated absolute URL, typically on a
	// DIFFERENT host than graph_endpoint. It carries NO Authorization header --
	// the URL itself is the credential -- and is sent verbatim
	// (PreauthenticatedRequest): it is signed over its exact query string.
	const string upload_url = url_it->second.get<string>();
	if (!IsHttpsUrl(upload_url)) {
		throw IOException("sharepoint: createUploadSession returned a non-https uploadUrl");
	}
	session.upload_url = upload_url;
}

void GraphClient::SendUploadChunk(GraphUploadSession &session, const ChunkedUploadBuffer &buf, bool final) {
	const string &pending = buf.Pending();
	const string &path = session.root_relative_path;
	std::size_t skip = 0;
	int failures = 0;
	string last_transport_error;
	for (;;) {
		if (skip == pending.size()) {
			return; // the session already holds every byte of this chunk
		}
		RawHttpResponse res;
		bool transport_ok = true;
		IncrementCounter(Counter::UPLOAD_CHUNK);
		try {
			// Non-final chunks say "/*": the total is unknown until close
			// (allowed because the session was opened with deferCommit).
			res = PreauthenticatedRequest(
			    "PUT", session.upload_url,
			    {{"Content-Range", buf.ContentRange(final, skip)}, {"Content-Type", "application/octet-stream"}},
			    skip == 0 ? pending : pending.substr(skip), kPreauthTimeoutSeconds, kUploadReplyMaxBytes);
		} catch (IOException &e) {
			// Transport failures are retried below. A REFUSAL (InvalidInputException:
			// bad URL or request shape) is not caught here: retrying cannot fix it.
			transport_ok = false;
			last_transport_error = e.what();
		}
		// 202 Accepted: stored. With deferCommit even the FINAL chunk answers
		// 202 (nextExpectedRanges: []); the item appears only on the commit.
		// 200/201 would mean Graph committed anyway -- also fine.
		if (transport_ok && res.status >= 200 && res.status < 300) {
			IncrementCounter(Counter::BYTES_WRITTEN, pending.size() - skip);
			return;
		}

		// Transport failure, 5xx, 429 -- or 416, which is what a re-sent range
		// gets after a LOST 202: ask the session which byte it expects next
		// and resend from there, bounded. Only bytes of THIS chunk are still in
		// memory; a resume point outside it is unrecoverable.
		const bool range_conflict = transport_ok && res.status == 416;
		const bool retryable =
		    !transport_ok || range_conflict || IsRetryable(ClassifyGraphError(res.status, ErrorCode(res.body)));
		if (retryable && ++failures <= kMaxUploadChunkRetries) {
			IncrementCounter(Counter::RETRY);
			if (!range_conflict) {
				SleepBeforeRetry(failures, transport_ok ? ParseRetryAfterSeconds(res.Header("Retry-After")) : 0);
			}
			uint64_t next = 0;
			bool complete = false;
			if (QueryUploadSession(session.upload_url, next, complete)) {
				if (complete) {
					next = buf.Offset() + pending.size(); // it holds everything, this chunk included
				}
				if (!buf.ResumeSkip(next, skip)) {
					throw IOException("sharepoint: upload session of '%s' expects byte %llu, outside the chunk "
					                  "still in memory (%llu-%llu); the upload cannot be resumed",
					                  path.c_str(), (unsigned long long)next, (unsigned long long)buf.Offset(),
					                  (unsigned long long)(buf.Offset() + pending.size()));
				}
			}
			continue;
		}
		if (!transport_ok) {
			throw IOException("sharepoint: upload chunk of '%s' failed after %d attempts: %s", path.c_str(), failures,
			                  last_transport_error.c_str());
		}
		ThrowGraph("upload chunk of '" + path + "'", res.status, res.body);
	}
}

DriveItemMeta GraphClient::CommitUploadSession(GraphUploadSession &session, uint64_t total) {
	const string &path = session.root_relative_path;
	// The commit's reply can be lost after Graph committed the file; the
	// session is then gone, so a re-POST 404s. If the item now has our size
	// and a different eTag than before, it is ours. (A concurrent writer of
	// exactly the same size is the residual ambiguity.)
	auto verify_committed = [&](DriveItemMeta &out) -> bool {
		auto now = StatByPath(path);
		if (now.found && !now.is_folder && static_cast<uint64_t>(now.size) == total &&
		    (!session.before.found || now.etag != session.before.etag)) {
			out = now;
			return true;
		}
		return false;
	};
	int failures = 0;
	string last_error;
	for (;;) {
		RawHttpResponse res;
		bool transport_ok = true;
		IncrementCounter(Counter::UPLOAD_COMMIT);
		try {
			res = PreauthenticatedRequest("POST", session.upload_url, {}, "", kPreauthTimeoutSeconds,
			                              kUploadReplyMaxBytes);
		} catch (IOException &e) {
			transport_ok = false;
			last_error = e.what();
		}
		if (transport_ok && (res.status == 200 || res.status == 201)) {
			session.upload_url.clear(); // consumed: nothing left to abort
			picojson::value v;
			if (!picojson::parse(v, res.body).empty() || !v.is<picojson::object>()) {
				throw IOException("sharepoint: committing the upload of '%s' returned no item metadata",
				                  path.c_str());
			}
			return ParseMeta(v.get<picojson::object>());
		}
		const bool retryable = !transport_ok || IsRetryable(ClassifyGraphError(res.status, ErrorCode(res.body)));
		if (retryable && ++failures <= kMaxUploadChunkRetries) {
			IncrementCounter(Counter::RETRY);
			SleepBeforeRetry(failures, transport_ok ? ParseRetryAfterSeconds(res.Header("Retry-After")) : 0);
			continue;
		}
		// A lost reply to an earlier commit attempt leaves the session gone
		// (404) while the file is in place.
		DriveItemMeta committed;
		if (failures > 0 && verify_committed(committed)) {
			session.upload_url.clear();
			return committed;
		}
		if (!transport_ok) {
			throw IOException("sharepoint: committing the upload of '%s' failed after %d attempts: %s", path.c_str(),
			                  failures, last_error.c_str());
		}
		ThrowGraph("commit upload of '" + path + "'", res.status, res.body);
	}
}

void GraphClient::AbortUploadSession(GraphUploadSession &session) noexcept {
	if (session.upload_url.empty()) {
		return;
	}
	// DELETE on the uploadUrl discards the stored chunks AND the 0-byte
	// placeholder SharePoint shows for a NEW file while its session is open;
	// an overwritten file keeps its old content (verified 2026-10-14).
	// Best effort: this runs on error paths and from a destructor, so it must
	// never throw. An abandoned session also expires on its own.
	const string url = std::move(session.upload_url);
	session.upload_url.clear();
	IncrementCounter(Counter::UPLOAD_ABORT);
	try {
		PreauthenticatedRequest("DELETE", url, {}, "", 30, kUploadReplyMaxBytes);
	} catch (...) {
	}
}

bool GraphClient::DeleteItem(const string &item_id, bool permanent) {
	IncrementCounter(Counter::DELETE_ITEM);
	auto &cli = GraphConnection(auth.graph_endpoint);
	duckdb_httplib_openssl::Headers h = {{"Authorization", "Bearer " + auth.tokens->Token()}};
	const string item_path = "/v1.0/drives/" + auth.drive_id + "/items/" + item_id;
	// A plain DELETE moves the item to the site's recycle bin. Permanent deletion
	// is its own Graph action, POST .../permanentDelete (a `Prefer` header on
	// DELETE is not part of the contract and was silently ignored).
	auto res = SendWithRetry(*auth.tokens, h, [&] {
		return permanent ? cli.Post((item_path + "/permanentDelete").c_str(), h, "", "application/json")
		                 : cli.Delete(item_path.c_str(), h);
	});
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

namespace {

//! Split a root-relative path into its parent path and final name.
void SplitParent(const string &root_relative, string &parent, string &name) {
	auto slash = root_relative.find_last_of('/');
	parent = (slash == string::npos) ? string() : root_relative.substr(0, slash);
	name = (slash == string::npos) ? root_relative : root_relative.substr(slash + 1);
}

} // namespace

void GraphClient::MoveByPath(const string &source_root_relative, const string &target_root_relative) {
	auto src = StatByPath(source_root_relative);
	if (!src.found) {
		throw IOException("sharepoint: move source '%s' does not exist", source_root_relative.c_str());
	}
	string parent, name;
	SplitParent(target_root_relative, parent, name);
	// Address the new parent by drive + item id rather than by a path string:
	// nothing user-controlled is interpreted as a path inside the body, and the
	// item can only ever move within this secret's drive.
	auto dest_parent = StatByPath(parent);
	if (!dest_parent.found || !dest_parent.is_folder) {
		throw IOException("sharepoint: move target folder '%s' does not exist", parent.c_str());
	}
	picojson::object parent_ref;
	parent_ref["driveId"] = picojson::value(auth.drive_id);
	parent_ref["id"] = picojson::value(dest_parent.id);
	picojson::object body;
	body["parentReference"] = picojson::value(parent_ref);
	body["name"] = picojson::value(name);

	auto &cli = GraphConnection(auth.graph_endpoint);
	duckdb_httplib_openssl::Headers h = {{"Authorization", "Bearer " + auth.tokens->Token()}};
	// Replace an existing target, as a local rename does. DuckDB's COPY writes a
	// tmp_ file and moves it onto the final name, so without this every re-run
	// of a COPY fails with 409 nameAlreadyExists.
	string path = "/v1.0/drives/" + auth.drive_id + "/items/" + src.id + "?@microsoft.graph.conflictBehavior=replace";
	IncrementCounter(Counter::MOVE);
	auto res = SendWithRetry(*auth.tokens, h, [&] { return cli.Patch(path.c_str(), h, picojson::value(body).serialize(), "application/json"); });
	if (!res || res->status < 200 || res->status >= 300) {
		ThrowGraph("move '" + source_root_relative + "' -> '" + target_root_relative + "'",
		           res ? res->status : 0, res ? res->body : string());
	}
}

void GraphClient::CreateFolder(const string &root_relative_path) {
	IncrementCounter(Counter::CREATE_FOLDER);
	string parent, name;
	SplitParent(root_relative_path, parent, name);

	auto &cli = GraphConnection(auth.graph_endpoint);
	duckdb_httplib_openssl::Headers h = {{"Authorization", "Bearer " + auth.tokens->Token()}};
	string path = parent.empty()
	                  ? "/v1.0/drives/" + auth.drive_id + "/root/children"
	                  : "/v1.0/drives/" + auth.drive_id + "/root:/" + EncodePath(parent) + ":/children";
	// conflictBehavior=fail so a pre-existing item is a 409 we then inspect,
	// never a duplicate.
	picojson::object body;
	body["name"] = picojson::value(name);
	body["folder"] = picojson::value(picojson::object());
	body["@microsoft.graph.conflictBehavior"] = picojson::value(string("fail"));
	auto res = SendWithRetry(*auth.tokens, h, [&] { return cli.Post(path.c_str(), h, picojson::value(body).serialize(), "application/json"); });
	if (!res) {
		throw IOException("sharepoint: create folder '%s' failed at the transport layer",
		                  root_relative_path.c_str());
	}
	if (res->status == 409) {
		// Idempotent only if what exists is a FOLDER; a file of that name is a
		// real conflict.
		auto existing = StatByPath(root_relative_path);
		if (existing.found && existing.is_folder) {
			return;
		}
		throw IOException("sharepoint: cannot create folder '%s': a file with that name exists",
		                  root_relative_path.c_str());
	}
	if (res->status < 200 || res->status >= 300) {
		ThrowGraph("create folder '" + root_relative_path + "'", res->status, res->body);
	}
}

vector<ChildEntry> GraphClient::ListChildren(const string &folder_root_relative_path, bool *found) {
	vector<ChildEntry> out;
	auto &cli = GraphConnection(auth.graph_endpoint);
	duckdb_httplib_openssl::Headers h = {{"Authorization", "Bearer " + auth.tokens->Token()}};
	string next = folder_root_relative_path.empty()
	                  ? "/v1.0/drives/" + auth.drive_id + "/root/children?$select=name,folder,file&$top=200"
	                  : "/v1.0/drives/" + auth.drive_id + "/root:/" + EncodePath(folder_root_relative_path) +
	                        ":/children?$select=name,folder,file&$top=200";
	if (found) {
		*found = true;
	}
	// 200 entries a page: 5000 pages is a million children in one folder, far
	// beyond anything SharePoint serves well. A nextLink chain longer than that
	// is a server bug, and must not loop forever.
	constexpr int kMaxListPages = 5000;
	int pages = 0;
	while (!next.empty()) {
		if (++pages > kMaxListPages) {
			throw IOException("sharepoint: listing '%s' exceeded %d pages", folder_root_relative_path.c_str(),
			                  kMaxListPages);
		}
		IncrementCounter(Counter::LIST_PAGE);
		auto res = SendWithRetry(*auth.tokens, h, [&] { return cli.Get(next.c_str(), h); });
		if (!res) {
			throw IOException("sharepoint: list of '%s' failed at the transport layer",
			                  folder_root_relative_path.c_str());
		}
		if (res->status == 404) {
			if (found) {
				*found = false;
			}
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
