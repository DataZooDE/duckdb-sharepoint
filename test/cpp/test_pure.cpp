// Catch2 pure-logic tests. No DuckDB linkage, no network, no credentials --
// runnable by anyone who can build. `make unit_test`.
#include "sharepoint_blockcache.hpp"
#include "sharepoint_errors.hpp"
#include "sharepoint_glob.hpp"
#include "sharepoint_identity.hpp"
#include "sharepoint_rawhttp.hpp"
#include "sharepoint_upload.hpp"
#include "sharepoint_uri.hpp"
#include "sharepoint_version.hpp"

#include <catch2/catch_test_macros.hpp>
#include <algorithm>
#include <cstdint>
#include <atomic>
#include <map>
#include <thread>
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

TEST_CASE("id: values are restricted to Graph's id alphabet", "[uri]") {
	// The id is spliced into /items/<id> URLs (including DELETE), so anything
	// that could change the URL's meaning is refused at parse time.
	REQUIRE(ParseSharepointUri("sharepoint://id:01KNPSQFNSEEL3YBPKXNGZ3JSQ5W7MTKIC").ok);
	REQUIRE(ParseSharepointUri("sharepoint://id:b!AbC-d_e.f").ok);
	for (const char *bad : {"sharepoint://id:..", "sharepoint://id:.", "sharepoint://id:x?$expand=children",
	                        "sharepoint://id:x#frag", "sharepoint://id:%2F..", "sharepoint://id:a b",
	                        "sharepoint://id:x&y", "sharepoint://id:x;y"}) {
		INFO(bad);
		REQUIRE_FALSE(ParseSharepointUri(bad).ok);
	}
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
	// Graph rejects any single request of 60 MiB or more: clamp to the largest
	// 320-KiB multiple below that, so one SET cannot make every upload fail.
	REQUIRE(kGraphUploadChunkMax < 60ull * 1024 * 1024);
	REQUIRE(kGraphUploadChunkMax % kGraphUploadChunkMultiple == 0);
	REQUIRE(NormalizeUploadChunkSize(1ull << 40) == kGraphUploadChunkMax);
	REQUIRE(NormalizeUploadChunkSize(60ull * 1024 * 1024) == kGraphUploadChunkMax);
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

TEST_CASE("block cache accounting stays exact under concurrent loads", "[blockcache][concurrency]") {
	// Many threads load and evict blocks of several files through a cache that
	// holds only two blocks. Accounting and eviction must agree exactly: a
	// block evicted before its own bytes were counted used to drive the
	// unsigned resident counter below zero (wrapping) or past the budget.
	const std::uint64_t kBlock = 64;
	const std::string file(kBlock * 4, 'b');
	auto fetch = [&](std::uint64_t off, std::uint64_t len) {
		std::this_thread::yield();
		return file.substr(off, len);
	};
	SharepointBlockCache cache(2 * kBlock);
	std::atomic<bool> bad {false};
	std::vector<std::thread> threads;
	for (int t = 0; t < 8; t++) {
		threads.emplace_back([&, t] {
			std::string out(kBlock, '\0');
			for (int i = 0; i < 3000; i++) {
				const std::string key = "f" + std::to_string((t + i) % 5);
				const std::uint64_t block = static_cast<std::uint64_t>((i * 7 + t) % 4);
				cache.Read(key, kBlock, file.size(), block * kBlock, kBlock, &out[0], fetch);
				if (out != file.substr(block * kBlock, kBlock)) {
					bad = true;
				}
				const auto resident = cache.ResidentBytes();
				if (resident > 4 * kBlock || resident % kBlock != 0) {
					bad = true; // wrapped, or over budget by more than in-flight loads
				}
			}
		});
	}
	for (auto &th : threads) {
		th.join();
	}
	REQUIRE_FALSE(bad);
	REQUIRE(cache.ResidentBytes() <= 2 * kBlock);
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
	REQUIRE(BuildRawHttpRequest("GET", "tenant.sharepoint.com", 443, target, {{"Range", "bytes=0-4"}}, "", req));
	REQUIRE(req == "GET " + target +
	                   " HTTP/1.1\r\nHost: tenant.sharepoint.com\r\nConnection: close\r\nRange: bytes=0-4\r\n"
	                   "Content-Length: 0\r\n\r\n");

	REQUIRE(BuildRawHttpRequest("PUT", "h", 443, "/u?a=1", {}, "xyz", req));
	REQUIRE(req.size() >= 3);
	REQUIRE(req.substr(req.size() - 3) == "xyz");
	REQUIRE(req.find("Content-Length: 3\r\n") != std::string::npos);
}

TEST_CASE("raw request refuses header and request-line injection", "[rawhttp]") {
	std::string req;
	REQUIRE_FALSE(BuildRawHttpRequest("GET", "h", 443, "/a b", {}, "", req));
	REQUIRE_FALSE(BuildRawHttpRequest("GET", "h", 443, "/a\r\nX: y", {}, "", req));
	REQUIRE_FALSE(BuildRawHttpRequest("GET", "h", 443, "no-leading-slash", {}, "", req));
	REQUIRE_FALSE(BuildRawHttpRequest("GET", "h\r\nX: y", 443, "/", {}, "", req));
	REQUIRE_FALSE(BuildRawHttpRequest("GET", "h", 443, "/", {{"Range", "bytes=0-1\r\nEvil: 1"}}, "", req));
	REQUIRE_FALSE(BuildRawHttpRequest("GET", "h", 443, "/", {{"Bad Name", "v"}}, "", req));
	REQUIRE_FALSE(BuildRawHttpRequest("G T", "h", 443, "/", {}, "", req));
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

TEST_CASE("raw request: reserved headers, non-ASCII values, Host port", "[rawhttp]") {
	std::string req;
	// The builder owns framing and routing headers; a caller copy would smuggle.
	for (const char *h : {"Content-Length", "transfer-encoding", "Host", "Connection"}) {
		INFO(h);
		REQUIRE_FALSE(BuildRawHttpRequest("GET", "h", 443, "/", {{h, "1"}}, "", req));
	}
	REQUIRE_FALSE(BuildRawHttpRequest("GET", "h", 443, "/", {{"X", "caf\xc3\xa9"}}, "", req));
	REQUIRE(BuildRawHttpRequest("GET", "h", 8443, "/", {}, "", req));
	REQUIRE(req.find("Host: h:8443\r\n") != std::string::npos);
	REQUIRE_FALSE(BuildRawHttpRequest("GET", "h", 0, "/", {}, "", req));
}

TEST_CASE("raw response framing is strict", "[rawhttp]") {
	RawHttpResponse r;
	// Conflicting framing is refused rather than guessed at.
	REQUIRE_FALSE(ParseRawHttpResponse("HTTP/1.1 200 OK\r\nContent-Length: 1\r\nContent-Length: 2\r\n\r\nab", r));
	REQUIRE_FALSE(ParseRawHttpResponse(
	    "HTTP/1.1 200 OK\r\nContent-Length: 5\r\nTransfer-Encoding: chunked\r\n\r\n0\r\n\r\n", r));
	REQUIRE_FALSE(
	    ParseRawHttpResponse("HTTP/1.1 200 OK\r\nContent-Length: 99999999999999999999999\r\n\r\n", r));
	// Identical duplicates are harmless.
	REQUIRE(ParseRawHttpResponse("HTTP/1.1 200 OK\r\nContent-Length: 2\r\nContent-Length: 2\r\n\r\nab", r));
	REQUIRE(r.body == "ab");
}

TEST_CASE("raw response completeness lets the reader stop before EOF", "[rawhttp]") {
	// A peer that ignores `Connection: close` must not make every request wait
	// for the socket timeout: a framed response is done when its body is.
	REQUIRE_FALSE(RawHttpResponseComplete("HTTP/1.1 200 OK\r\nContent-Length: 5\r\n"));
	REQUIRE_FALSE(RawHttpResponseComplete("HTTP/1.1 200 OK\r\nContent-Length: 5\r\n\r\nhel"));
	REQUIRE(RawHttpResponseComplete("HTTP/1.1 200 OK\r\nContent-Length: 5\r\n\r\nhello"));
	REQUIRE_FALSE(RawHttpResponseComplete("HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n5\r\nhello\r\n"));
	REQUIRE(RawHttpResponseComplete("HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n5\r\nhello\r\n0\r\n\r\n"));
	// Unframed: only EOF ends it.
	REQUIRE_FALSE(RawHttpResponseComplete("HTTP/1.1 200 OK\r\n\r\nabc"));
}

TEST_CASE("https origin and absolute URL edge cases", "[rawhttp]") {
	std::string host;
	int port = 0;
	for (const char *bad : {"https://user@h", "https://h?x", "https://h#x", "https://[::1]", "https://h/p"}) {
		INFO(bad);
		REQUIRE_FALSE(SplitHttpsOrigin(bad, host, port));
	}
	std::string origin, rest;
	// A query with no path keeps its query.
	REQUIRE(SplitAbsoluteUrl("https://h?a=1&b=2", origin, rest));
	REQUIRE(origin == "https://h");
	REQUIRE(rest == "/?a=1&b=2");
}

TEST_CASE("ranged reply acceptance", "[rawhttp]") {
	std::string body = "hello";
	REQUIRE(AcceptRangeReply(206, "bytes 10-14/100", body, 10, 5));
	REQUIRE(body == "hello");
	// 206 of the wrong length or for the wrong start is not our range.
	body = "hell";
	REQUIRE_FALSE(AcceptRangeReply(206, "", body, 10, 5));
	body = "hello";
	REQUIRE_FALSE(AcceptRangeReply(206, "bytes 0-4/100", body, 10, 5));
	// 200 = whole file: sliced, or refused if too short.
	body = "0123456789abcdef";
	REQUIRE(AcceptRangeReply(200, "", body, 10, 5));
	REQUIRE(body == "abcde");
	body = "0123";
	REQUIRE_FALSE(AcceptRangeReply(200, "", body, 10, 5));
	body = "x";
	REQUIRE_FALSE(AcceptRangeReply(404, "", body, 0, 1));
}

// ---------------------------------------------------------------------------
// Identity: which endpoints may receive credentials, and how credential
// material is framed before it is hashed into a cache key.
// ---------------------------------------------------------------------------
TEST_CASE("only Microsoft cloud endpoints may receive credentials", "[identity]") {
	// sharepoint_graph_endpoint / sharepoint_login_endpoint are session
	// settings: without an allowlist any SQL user could point them at a host
	// they control and receive bearer tokens -- or the client secret itself.
	for (const char *ok : {"https://graph.microsoft.com", "https://graph.microsoft.com/", "https://graph.microsoft.us",
	                       "https://dod-graph.microsoft.us", "https://microsoftgraph.chinacloudapi.cn",
	                       "HTTPS://Graph.Microsoft.com"}) {
		INFO(ok);
		REQUIRE(IsAllowedGraphEndpoint(ok));
	}
	for (const char *ok : {"https://login.microsoftonline.com", "https://login.microsoftonline.us",
	                       "https://login.partner.microsoftonline.cn", "https://login.chinacloudapi.cn"}) {
		INFO(ok);
		REQUIRE(IsAllowedLoginEndpoint(ok));
	}
	for (const char *bad :
	     {"http://graph.microsoft.com", "https://graph.microsoft.com.evil.example", "https://evil.example",
	      "https://graph.microsoft.com:8443", "https://graph.microsoft.com/v1.0", "https://user@graph.microsoft.com",
	      "https://login.microsoftonline.com", ""}) {
		INFO(bad);
		REQUIRE_FALSE(IsAllowedGraphEndpoint(bad));
	}
	REQUIRE_FALSE(IsAllowedLoginEndpoint("https://graph.microsoft.com"));
	REQUIRE_FALSE(IsAllowedLoginEndpoint("http://login.microsoftonline.com"));
}

TEST_CASE("credential material framing is unambiguous", "[identity]") {
	// "a|b" + "c" and "a" + "b|c" must never frame to the same key material.
	REQUIRE(FrameCredentialMaterial({"a|b", "c"}) != FrameCredentialMaterial({"a", "b|c"}));
	REQUIRE(FrameCredentialMaterial({"ab", ""}) != FrameCredentialMaterial({"a", "b"}));
	REQUIRE(FrameCredentialMaterial({"", ""}) != FrameCredentialMaterial({""}));
	REQUIRE(FrameCredentialMaterial({"x", "y"}) == FrameCredentialMaterial({"x", "y"}));
}

TEST_CASE("the default scope follows the Graph cloud", "[identity]") {
	REQUIRE(DefaultGraphScope("https://graph.microsoft.com") == "https://graph.microsoft.com/.default");
	REQUIRE(DefaultGraphScope("https://graph.microsoft.us/") == "https://graph.microsoft.us/.default");
}

TEST_CASE("tenant ids are a GUID or a domain", "[identity]") {
	// The tenant is spliced into the token URL path: "/<tenant>/oauth2/...".
	REQUIRE(IsValidTenantId("72f988bf-86f1-41af-91ab-2d7cd011db47"));
	REQUIRE(IsValidTenantId("contoso.onmicrosoft.com"));
	REQUIRE(IsValidTenantId("organizations"));
	for (const char *bad : {"", "a/b", "x?y", "x#y", "..", "a b", "contoso.com/../evil", "%2e%2e"}) {
		INFO(bad);
		REQUIRE_FALSE(IsValidTenantId(bad));
	}
}
