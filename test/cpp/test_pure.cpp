// Catch2 pure-logic tests. No DuckDB linkage, no network, no credentials --
// runnable by anyone who can build. `make unit_test`.
#include "sharepoint_blockcache.hpp"
#include "sharepoint_errors.hpp"
#include "sharepoint_glob.hpp"
#include "sharepoint_rawhttp.hpp"
#include "sharepoint_upload.hpp"
#include "sharepoint_uri.hpp"
#include "sharepoint_version.hpp"

#include <catch2/catch_test_macros.hpp>
#include <algorithm>
#include <cstdint>
#include <map>
#include <vector>

using namespace duckdb::sharepoint;

TEST_CASE("version is CalVer", "[version]") {
	auto v = SharepointVersion();
	REQUIRE(v.size() == std::string("2026.08.05").size());
	REQUIRE(v[4] == '.');
}

TEST_CASE("path URIs parse into segments", "[uri]") {
	auto r = ParseSharepointUri("sharepoint://Finance/2026/actuals.parquet");
	REQUIRE(r.ok);
	REQUIRE(r.uri.kind == SharepointUriKind::PATH);
	REQUIRE(r.uri.segments.size() == 3);
	REQUIRE(r.uri.FileName() == "actuals.parquet");
	REQUIRE(r.uri.RootRelativePath() == "Finance/2026/actuals.parquet");
	REQUIRE(r.uri.ParentPath() == "sharepoint://Finance/2026");
}

TEST_CASE("repeated and trailing slashes collapse", "[uri]") {
	auto r = ParseSharepointUri("sharepoint://a//b/c/");
	REQUIRE(r.ok);
	REQUIRE(r.uri.segments.size() == 3);
	REQUIRE(r.uri.RootRelativePath() == "a/b/c");
}

TEST_CASE("empty path is the drive root", "[uri]") {
	auto r = ParseSharepointUri("sharepoint://");
	REQUIRE(r.ok);
	REQUIRE(r.uri.segments.empty());
	REQUIRE(r.uri.FileName().empty());
}

TEST_CASE("dot and dotdot segments are rejected", "[uri]") {
	REQUIRE_FALSE(ParseSharepointUri("sharepoint://a/../b").ok);
	REQUIRE_FALSE(ParseSharepointUri("sharepoint://./b").ok);
}

TEST_CASE("id: form parses and forbids trailing segments", "[uri]") {
	auto ok = ParseSharepointUri("sharepoint://id:01ABCDEF");
	REQUIRE(ok.ok);
	REQUIRE(ok.uri.kind == SharepointUriKind::FILE_ID);
	REQUIRE(ok.uri.item_id == "01ABCDEF");
	REQUIRE(ok.uri.ToString() == "sharepoint://id:01ABCDEF");

	REQUIRE(ParseSharepointUri("sharepoint://id:01ABCDEF/").ok); // trailing slash tolerated
	REQUIRE_FALSE(ParseSharepointUri("sharepoint://id:").ok);
	REQUIRE_FALSE(ParseSharepointUri("sharepoint://id:abc/oops").ok); // a real typo still rejects
}

TEST_CASE("non-sharepoint URIs are rejected", "[uri]") {
	REQUIRE_FALSE(IsSharepointUri("s3://bucket/x"));
	REQUIRE_FALSE(ParseSharepointUri("gdrive://a/b").ok);
}

TEST_CASE("core's fallback-glob probe on an id: URI is recognised", "[uri]") {
	// The synthesized shape must be recognised so Glob returns empty ...
	REQUIRE(IsFileIdFallbackGlobProbe("sharepoint://id:01ABC/**/*.parquet"));
	// ... but a genuine user typo must NOT be, so it keeps surfacing the error.
	REQUIRE_FALSE(IsFileIdFallbackGlobProbe("sharepoint://id:01ABC/oops"));
	REQUIRE_FALSE(IsFileIdFallbackGlobProbe("sharepoint://Finance/**/*.parquet"));
}

TEST_CASE("immutable-prefix matching is per-segment", "[uri]") {
	REQUIRE(PathMatchesAnyPrefix("sharepoint://lake/data/x.parquet", "sharepoint://lake"));
	REQUIRE(PathMatchesAnyPrefix("sharepoint://lake", "sharepoint://lake"));
	REQUIRE_FALSE(PathMatchesAnyPrefix("sharepoint://lakehouse/x", "sharepoint://lake"));
	REQUIRE(PathMatchesAnyPrefix("sharepoint://b/x", " a/ , sharepoint://b "));
	REQUIRE_FALSE(PathMatchesAnyPrefix("sharepoint://a/x", ""));
}

TEST_CASE("glob matches within a segment", "[glob]") {
	REQUIRE(MatchGlob("*.parquet", "actuals.parquet"));
	REQUIRE(MatchGlob("part-*.csv", "part-0001.csv"));
	REQUIRE(MatchGlob("file-??.txt", "file-07.txt"));
	REQUIRE_FALSE(MatchGlob("*.parquet", "actuals.csv"));
	REQUIRE_FALSE(MatchGlob("file-??.txt", "file-7.txt"));
	REQUIRE(HasGlobMeta("*.parquet"));
	REQUIRE_FALSE(HasGlobMeta("actuals.parquet"));
}

// A synthetic drive tree for the recursive-glob tests. No Graph, no I/O: the
// lister just reads this map, so GlobFiles is exercised end to end in the pure
// binary. Keys are root-relative folder paths ("" == the drive root).
static GlobLister TreeLister(const std::map<std::string, std::vector<GlobEntry>> &tree) {
	return [tree](const std::string &folder) -> std::vector<GlobEntry> {
		auto it = tree.find(folder);
		return it == tree.end() ? std::vector<GlobEntry> {} : it->second;
	};
}

static std::vector<std::string> Sorted(std::vector<std::string> v) {
	std::sort(v.begin(), v.end());
	return v;
}

TEST_CASE("recursive ** glob walks folders", "[glob]") {
	// ""              -> Finance/, readme.md
	// Finance         -> 2025/, 2026/, summary.parquet
	// Finance/2025    -> q1.parquet, q2.csv
	// Finance/2026    -> q1.parquet
	const std::map<std::string, std::vector<GlobEntry>> tree = {
	    {"", {{"Finance", true}, {"readme.md", false}}},
	    {"Finance", {{"2025", true}, {"2026", true}, {"summary.parquet", false}}},
	    {"Finance/2025", {{"q1.parquet", false}, {"q2.csv", false}}},
	    {"Finance/2026", {{"q1.parquet", false}}},
	};
	auto list = TreeLister(tree);

	// ** matches any depth (including zero: summary.parquet sits directly in Finance).
	REQUIRE(Sorted(GlobFiles({"Finance", "**", "*.parquet"}, list)) ==
	        std::vector<std::string> {"Finance/2025/q1.parquet", "Finance/2026/q1.parquet",
	                                  "Finance/summary.parquet"});

	// A single * is exactly one folder level -- summary.parquet (depth 0) is excluded.
	REQUIRE(Sorted(GlobFiles({"Finance", "*", "*.parquet"}, list)) ==
	        std::vector<std::string> {"Finance/2025/q1.parquet", "Finance/2026/q1.parquet"});

	// ** from the root, matching by extension across the whole tree.
	REQUIRE(Sorted(GlobFiles({"**", "*.csv"}, list)) == std::vector<std::string> {"Finance/2025/q2.csv"});

	// A trailing ** is every file beneath the folder (folders themselves excluded).
	REQUIRE(Sorted(GlobFiles({"Finance", "**"}, list)) ==
	        std::vector<std::string> {"Finance/2025/q1.parquet", "Finance/2025/q2.csv", "Finance/2026/q1.parquet",
	                                  "Finance/summary.parquet"});

	// No match -> empty, and each file is emitted exactly once (no ** double-count).
	REQUIRE(GlobFiles({"Finance", "**", "*.orc"}, list).empty());
	REQUIRE(GlobFiles({"**", "q1.parquet"}, list).size() == 2);
}

TEST_CASE("upload chunk size rounds down to a 320 KiB multiple", "[upload]") {
	// A default that is already a multiple is unchanged.
	REQUIRE(NormalizeUploadChunkSize(10ull * 1024 * 1024) == 10ull * 1024 * 1024);
	REQUIRE(NormalizeUploadChunkSize(kGraphUploadChunkMultiple) == kGraphUploadChunkMultiple);
	// A non-multiple rounds DOWN (the last chunk carries the remainder).
	REQUIRE(NormalizeUploadChunkSize(kGraphUploadChunkMultiple * 3 + 5) == kGraphUploadChunkMultiple * 3);
	// Below one multiple clamps up, never to zero -- the loop must progress.
	REQUIRE(NormalizeUploadChunkSize(1) == kGraphUploadChunkMultiple);
	REQUIRE(NormalizeUploadChunkSize(0) == kGraphUploadChunkMultiple);
}

TEST_CASE("content-range header is byte-inclusive", "[upload]") {
	REQUIRE(UploadContentRange(0, kGraphUploadChunkMultiple, 1000000) == "bytes 0-327679/1000000");
	// A final, ragged chunk: start 655360, length 344640 -> last byte 999999.
	REQUIRE(UploadContentRange(655360, 344640, 1000000) == "bytes 655360-999999/1000000");
}

TEST_CASE("absolute upload URL splits into origin and path", "[upload]") {
	std::string origin, rest;
	REQUIRE(SplitAbsoluteUrl("https://host.sharepoint.com/_api/v2.0/drive/items/x/upload?guid=abc", origin, rest));
	REQUIRE(origin == "https://host.sharepoint.com");
	REQUIRE(rest == "/_api/v2.0/drive/items/x/upload?guid=abc");

	REQUIRE(SplitAbsoluteUrl("https://host:8443/p", origin, rest));
	REQUIRE(origin == "https://host:8443");
	REQUIRE(rest == "/p");

	// No path -> the remainder defaults to "/".
	REQUIRE(SplitAbsoluteUrl("https://host.example.com", origin, rest));
	REQUIRE(origin == "https://host.example.com");
	REQUIRE(rest == "/");

	// A bare path is not an absolute URL.
	REQUIRE_FALSE(SplitAbsoluteUrl("/just/a/path", origin, rest));
}

TEST_CASE("only https URLs are followed or uploaded to", "[upload]") {
	// A redirect target / uploadUrl is itself a credential (tempauth), so a
	// downgrade to plain http must never be followed.
	REQUIRE(IsHttpsUrl("https://tenant.sharepoint.com/_layouts/15/download.aspx?tempauth=x"));
	REQUIRE(IsHttpsUrl("HTTPS://tenant.sharepoint.com/"));
	REQUIRE_FALSE(IsHttpsUrl("http://tenant.sharepoint.com/"));
	REQUIRE_FALSE(IsHttpsUrl("/relative/path"));
	REQUIRE_FALSE(IsHttpsUrl("https:/missing-slash"));
	REQUIRE_FALSE(IsHttpsUrl(""));
}

TEST_CASE("a 200 full-body reply is sliced to the requested range", "[upload]") {
	// A server that ignores Range returns 200 with the WHOLE file; returning it
	// as-is would hand a Parquet footer read the file's header bytes.
	std::string body = "0123456789";
	REQUIRE(SliceRangeFromFullBody(body, 3, 4));
	REQUIRE(body == "3456");

	body = "0123456789";
	REQUIRE(SliceRangeFromFullBody(body, 0, 10));
	REQUIRE(body == "0123456789");

	// Too short to contain the range: refuse rather than return short bytes.
	body = "0123";
	REQUIRE_FALSE(SliceRangeFromFullBody(body, 2, 5));
	body = "0123";
	REQUIRE_FALSE(SliceRangeFromFullBody(body, 10, 1));
}

TEST_CASE("nextExpectedRanges start offset parses", "[upload]") {
	std::uint64_t s = 999;
	REQUIRE(ParseRangeStart("512-1023", s));
	REQUIRE(s == 512);
	REQUIRE(ParseRangeStart("655360-", s));
	REQUIRE(s == 655360);
	REQUIRE_FALSE(ParseRangeStart("-1", s));
	REQUIRE_FALSE(ParseRangeStart("", s));
}

TEST_CASE("block plan splits a read across block boundaries", "[blockcache]") {
	// A 25-byte file in blocks of 10: [0,10) [10,20) [20,25).
	auto p = PlanBlockCopies(8, 7, 10, 25); // 8..14 -> 2 bytes from block0, 5 from block1
	REQUIRE(p.size() == 2);
	REQUIRE(p[0].block_index == 0);
	REQUIRE(p[0].block_offset == 0);
	REQUIRE(p[0].block_length == 10);
	REQUIRE(p[0].src_in_block == 8);
	REQUIRE(p[0].dest_offset == 0);
	REQUIRE(p[0].length == 2);
	REQUIRE(p[1].block_index == 1);
	REQUIRE(p[1].block_offset == 10);
	REQUIRE(p[1].src_in_block == 0);
	REQUIRE(p[1].dest_offset == 2);
	REQUIRE(p[1].length == 5);

	// A read wholly inside the short final block.
	auto q = PlanBlockCopies(22, 3, 10, 25);
	REQUIRE(q.size() == 1);
	REQUIRE(q[0].block_index == 2);
	REQUIRE(q[0].block_offset == 20);
	REQUIRE(q[0].block_length == 5); // clamped to file
	REQUIRE(q[0].src_in_block == 2);
	REQUIRE(q[0].length == 3);
}

TEST_CASE("block cache fetches each block once and serves hits", "[blockcache]") {
	const std::string file = "0123456789abcdefghijklmno"; // 25 bytes
	int fetches = 0;
	auto fetch = [&](std::uint64_t off, std::uint64_t len) {
		fetches++;
		return file.substr(off, len);
	};
	SharepointBlockCache cache(1024);
	auto read = [&](std::uint64_t off, std::uint64_t len) {
		std::string out(len, '\0');
		cache.Read("id|ctag", 10, file.size(), off, len, &out[0], fetch);
		return out;
	};

	REQUIRE(read(0, 5) == "01234"); // block 0 fetched
	REQUIRE(fetches == 1);
	REQUIRE(read(5, 5) == "56789"); // block 0 hit
	REQUIRE(fetches == 1);
	REQUIRE(read(8, 7) == "89abcde"); // block 0 hit + block 1 fetch
	REQUIRE(fetches == 2);
	REQUIRE(read(20, 5) == "klmno"); // short block 2 fetch
	REQUIRE(fetches == 3);
	REQUIRE(read(0, 25) == file); // fully cached now
	REQUIRE(fetches == 3);
}

TEST_CASE("block cache never serves a block cut for another block size", "[blockcache]") {
	// Two sessions with different sharepoint_block_size_bytes read the same
	// file. Block 0 at size 4 must not be served for block 0 at size 10 -- the
	// copy would run past the end of the 4-byte block.
	const std::string file = "0123456789abcdefghij"; // 20 bytes
	int fetches = 0;
	auto fetch = [&](std::uint64_t off, std::uint64_t len) {
		fetches++;
		return file.substr(off, len);
	};
	SharepointBlockCache cache(1024);
	std::string small(4, '\0'), big(10, '\0');
	cache.Read("id|ctag", 4, file.size(), 0, 4, &small[0], fetch);
	cache.Read("id|ctag", 10, file.size(), 0, 10, &big[0], fetch);
	REQUIRE(small == "0123");
	REQUIRE(big == "0123456789");
	REQUIRE(fetches == 2);
}

TEST_CASE("a failed block fetch is not cached and does not wedge eviction", "[blockcache]") {
	const std::string file(300, 'q'); // 3 blocks of 100
	bool fail = true;
	int fetches = 0;
	auto fetch = [&](std::uint64_t off, std::uint64_t len) -> std::string {
		fetches++;
		if (fail) {
			throw std::runtime_error("HTTP 503");
		}
		return file.substr(off, len);
	};
	SharepointBlockCache cache(150); // room for one block
	std::string out(100, '\0');
	REQUIRE_THROWS(cache.Read("f", 100, 300, 200, 100, &out[0], fetch));
	REQUIRE(cache.ResidentBytes() == 0);

	// The failure left nothing behind: later loads still evict down to budget
	// (an unloaded block at the LRU tail must not block eviction forever)...
	fail = false;
	cache.Read("f", 100, 300, 0, 100, &out[0], fetch);
	cache.Read("f", 100, 300, 100, 100, &out[0], fetch);
	REQUIRE(cache.ResidentBytes() <= 150);
	// ...and a retry of the failed block fetches it afresh.
	cache.Read("f", 100, 300, 200, 100, &out[0], fetch);
	REQUIRE(fetches == 4);
	REQUIRE(cache.ResidentBytes() <= 150);
}

TEST_CASE("block cache evicts LRU when over budget", "[blockcache]") {
	const std::string file(300, 'z'); // 3 blocks of 100
	int fetches = 0;
	auto fetch = [&](std::uint64_t off, std::uint64_t len) {
		fetches++;
		return file.substr(off, len);
	};
	// Budget holds only two 100-byte blocks.
	SharepointBlockCache cache(200);
	auto touch = [&](std::uint64_t block) {
		std::string out(100, '\0');
		cache.Read("k", 100, file.size(), block * 100, 100, &out[0], fetch);
	};

	touch(0);
	touch(1);
	REQUIRE(fetches == 2);
	REQUIRE(cache.ResidentBytes() == 200);
	touch(2); // evicts block 0 (LRU)
	REQUIRE(fetches == 3);
	REQUIRE(cache.ResidentBytes() == 200);
	touch(0); // block 0 was evicted -> refetched
	REQUIRE(fetches == 4);
}

TEST_CASE("graph errors classify by status and code", "[errors]") {
	REQUIRE(ClassifyGraphError(404, "itemNotFound") == GraphErrorKind::NOT_FOUND);
	REQUIRE(ClassifyGraphError(401, "") == GraphErrorKind::UNAUTHENTICATED);
	REQUIRE(ClassifyGraphError(429, "") == GraphErrorKind::THROTTLED);
	REQUIRE(ClassifyGraphError(503, "") == GraphErrorKind::THROTTLED);
	// A 400 that is really throttling, per the code.
	REQUIRE(ClassifyGraphError(400, "activityLimitReached") == GraphErrorKind::THROTTLED);
	REQUIRE(ClassifyGraphError(200, "") == GraphErrorKind::OK);
	REQUIRE(IsRetryable(GraphErrorKind::THROTTLED));
	REQUIRE_FALSE(IsRetryable(GraphErrorKind::UNAUTHENTICATED));
}

// ---------------------------------------------------------------------------
// Verbatim HTTP/1.1 for Graph's pre-authenticated URLs. httplib 0.27 (DuckDB
// 1.5) re-sorts a request's query parameters, and SharePoint's signed
// download/upload URLs then answer 401 -- so these are sent byte for byte.
// ---------------------------------------------------------------------------
TEST_CASE("raw request keeps the target byte for byte", "[rawhttp]") {
	std::string req;
	const std::string target = "/_layouts/15/download.aspx?UniqueId=abc&Translate=false&tempauth=v1.x&ApiVersion=2.0";
	REQUIRE(BuildRawHttpRequest("GET", "tenant.sharepoint.com", target, {{"Range", "bytes=0-4"}}, "", req));
	REQUIRE(req == "GET " + target +
	                   " HTTP/1.1\r\nHost: tenant.sharepoint.com\r\nConnection: close\r\nRange: bytes=0-4\r\n"
	                   "Content-Length: 0\r\n\r\n");

	REQUIRE(BuildRawHttpRequest("PUT", "h", "/u?a=1", {}, "xyz", req));
	REQUIRE(req.size() >= 3);
	REQUIRE(req.substr(req.size() - 3) == "xyz");
	REQUIRE(req.find("Content-Length: 3\r\n") != std::string::npos);
}

TEST_CASE("raw request refuses header and request-line injection", "[rawhttp]") {
	std::string req;
	REQUIRE_FALSE(BuildRawHttpRequest("GET", "h", "/a b", {}, "", req));
	REQUIRE_FALSE(BuildRawHttpRequest("GET", "h", "/a\r\nX: y", {}, "", req));
	REQUIRE_FALSE(BuildRawHttpRequest("GET", "h", "no-leading-slash", {}, "", req));
	REQUIRE_FALSE(BuildRawHttpRequest("GET", "h\r\nX: y", "/", {}, "", req));
	REQUIRE_FALSE(BuildRawHttpRequest("GET", "h", "/", {{"Range", "bytes=0-1\r\nEvil: 1"}}, "", req));
	REQUIRE_FALSE(BuildRawHttpRequest("GET", "h", "/", {{"Bad Name", "v"}}, "", req));
	REQUIRE_FALSE(BuildRawHttpRequest("G T", "h", "/", {}, "", req));
}

TEST_CASE("raw response parses content-length, chunked and EOF bodies", "[rawhttp]") {
	RawHttpResponse r;
	REQUIRE(ParseRawHttpResponse("HTTP/1.1 206 Partial Content\r\nContent-Length: 5\r\n"
	                             "Content-Range: bytes 0-4/11\r\n\r\nhelloEXTRA",
	                             r));
	REQUIRE(r.status == 206);
	REQUIRE(r.body == "hello");
	REQUIRE(r.Header("content-range") == "bytes 0-4/11");

	REQUIRE(ParseRawHttpResponse("HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n"
	                             "5\r\nhello\r\n6;ext=1\r\n world\r\n0\r\n\r\n",
	                             r));
	REQUIRE(r.status == 200);
	REQUIRE(r.body == "hello world");

	// Neither header: the body runs to connection close.
	REQUIRE(ParseRawHttpResponse("HTTP/1.1 202 Accepted\r\nX: y\r\n\r\n{\"a\":1}", r));
	REQUIRE(r.status == 202);
	REQUIRE(r.body == "{\"a\":1}");

	// Truncated: promised more bytes than arrived.
	REQUIRE_FALSE(ParseRawHttpResponse("HTTP/1.1 200 OK\r\nContent-Length: 10\r\n\r\nshort", r));
	REQUIRE_FALSE(ParseRawHttpResponse("HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n5\r\nhel", r));
	REQUIRE_FALSE(ParseRawHttpResponse("garbage", r));
	REQUIRE_FALSE(ParseRawHttpResponse("HTTP/1.1 200 OK\r\nContent-Length: 5", r)); // no end of head
}

TEST_CASE("https origin splits into host and port", "[rawhttp]") {
	std::string host;
	int port = 0;
	REQUIRE(SplitHttpsOrigin("https://tenant.sharepoint.com", host, port));
	REQUIRE(host == "tenant.sharepoint.com");
	REQUIRE(port == 443);
	REQUIRE(SplitHttpsOrigin("https://h:8443", host, port));
	REQUIRE(host == "h");
	REQUIRE(port == 8443);
	REQUIRE_FALSE(SplitHttpsOrigin("http://h", host, port));
	REQUIRE_FALSE(SplitHttpsOrigin("https://", host, port));
	REQUIRE_FALSE(SplitHttpsOrigin("https://h:0", host, port));
	REQUIRE_FALSE(SplitHttpsOrigin("https://h:99999", host, port));
	REQUIRE_FALSE(SplitHttpsOrigin("https://h:x", host, port));
}
