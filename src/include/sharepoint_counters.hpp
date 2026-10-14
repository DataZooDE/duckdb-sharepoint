#pragma once

#include <cstdint>
#include <utility>
#include <vector>

namespace duckdb {
namespace sharepoint {

// ---------------------------------------------------------------------------
// Process-wide Graph call counters behind sharepoint_stats(), as gdrive_stats()
// is for the sibling. Pure (std::atomic only) so the Catch2 binary covers it;
// the table functions live in sharepoint_stats.cpp.
//
// They turn "this should cost fewer calls" into an assertion: reset, run one
// query, read the counters (see test/sql/sharepoint.live.template).
// ---------------------------------------------------------------------------
enum class Counter {
	TOKEN_REQUEST,   // POST to the Entra ID token endpoint
	STAT,            // GET a driveItem's metadata (by path or id)
	READ_RANGE,      // ranged GET of file content (one per uncached read / block fetch)
	LIST_PAGE,       // GET one page of a folder's children
	UPLOAD_PUT,      // single-request upload (file <= upload chunk size)
	UPLOAD_SESSION,  // createUploadSession
	UPLOAD_CHUNK,    // one chunk PUT inside a session
	UPLOAD_COMMIT,   // the commit POST that publishes a streamed (deferCommit) upload
	UPLOAD_ABORT,    // an upload session discarded (error mid-stream, or a handle never closed)
	DELETE_ITEM,     // DELETE / permanentDelete
	MOVE,            // PATCH parentReference + name
	CREATE_FOLDER,   // POST children (folder)
	RETRY,           // any retried attempt (transport, 5xx, throttling)
	THROTTLED,       // a 429/503 or throttling-coded reply was received
	BYTES_READ,      // content bytes received by READ_RANGE
	BYTES_WRITTEN,   // content bytes sent by UPLOAD_PUT / UPLOAD_CHUNK
	META_CACHE_HIT,  // an open served from the immutable-prefix metadata cache
	AUTH_REFRESH,    // a 401 made a request re-mint its token and re-send
	UPLOAD_BUFFER_PEAK, // HIGH-WATER MARK, not a count: most bytes one write handle held in memory
	COUNT_           // sentinel
};

//! Stable snake_case name of a counter ("stat", "read_range", ...).
const char *CounterName(Counter c);

void IncrementCounter(Counter c, uint64_t by = 1);

//! Every counter in declaration order with its current value.
std::vector<std::pair<const char *, uint64_t>> SnapshotCounters();

//! For a high-water-mark metric (UPLOAD_BUFFER_PEAK): raise it to `value` if
//! that is larger. Reset like every other counter.
void RecordMaxCounter(Counter c, uint64_t value);

void ResetCounters();

} // namespace sharepoint
} // namespace duckdb
