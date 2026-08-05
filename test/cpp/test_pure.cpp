// Catch2 pure-logic tests. No DuckDB linkage, no network, no credentials --
// runnable by anyone who can build. `make unit_test`.
#include "sharepoint_errors.hpp"
#include "sharepoint_glob.hpp"
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

TEST_CASE("nextExpectedRanges start offset parses", "[upload]") {
	std::uint64_t s = 999;
	REQUIRE(ParseRangeStart("512-1023", s));
	REQUIRE(s == 512);
	REQUIRE(ParseRangeStart("655360-", s));
	REQUIRE(s == 655360);
	REQUIRE_FALSE(ParseRangeStart("-1", s));
	REQUIRE_FALSE(ParseRangeStart("", s));
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
