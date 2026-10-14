#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace duckdb {
namespace sharepoint {

// ---------------------------------------------------------------------------
// Pure helpers for the resumable upload-session chunk plan. No I/O, so the
// Catch2 binary exercises them directly. The PUT loop that drives them lives in
// the (impure, Graph-facing) GraphClient upload-session methods.
//
// Graph's upload-session contract: every chunk except the last MUST be a
// multiple of 320 KiB (327680 bytes), each PUT carries a Content-Range header,
// and a 5xx/timeout mid-session is resumed by asking the session which byte it
// still expects (nextExpectedRanges).
// ---------------------------------------------------------------------------

//! 320 KiB -- the granularity Graph requires for every non-final chunk.
constexpr std::uint64_t kGraphUploadChunkMultiple = 327680;

//! Largest chunk Graph accepts: every request must be under 60 MiB, so the
//! biggest 320-KiB multiple below that (192 * 320 KiB = 60 MiB is already too
//! big).
constexpr std::uint64_t kGraphUploadChunkMax = 191 * kGraphUploadChunkMultiple;

//! Round `requested` DOWN to a positive multiple of 320 KiB, at most
//! kGraphUploadChunkMax. Never returns 0: a request below one multiple is
//! clamped up to a single 320-KiB chunk, so the loop always makes progress.
std::uint64_t NormalizeUploadChunkSize(std::uint64_t requested);

//! The value for a chunk's Content-Range request header, end-inclusive:
//!   UploadContentRange(0, 327680, 1000000) -> "bytes 0-327679/1000000"
//! `length` must be >= 1 (the loop never sends an empty chunk).
std::string UploadContentRange(std::uint64_t start, std::uint64_t length, std::uint64_t total);

//! Split an absolute "scheme://host[:port]/path?query" into its origin
//! ("scheme://host[:port]") and the remainder ("/path?query"). Returns false if
//! `url` has no "://" scheme separator. For a URL with no path the remainder
//! defaults to "/" (and a query with no path keeps its query). Graph's
//! pre-authenticated URLs (uploadUrl, the /content redirect) are absolute and
//! usually on another host; PreauthenticatedRequest splits them with this and
//! sends the remainder verbatim (never through httplib, which would re-sort
//! the signed query).
bool SplitAbsoluteUrl(const std::string &url, std::string &origin, std::string &path_and_query);

//! True iff `url` is an absolute https:// URL (scheme case-insensitive). Graph
//! hands out pre-authenticated URLs -- the uploadUrl and the /content download
//! redirect -- whose query string IS the credential, so they are only ever
//! followed over TLS; a downgrade to http:// is refused.
bool IsHttpsUrl(const std::string &url);

//! Parse the start offset of a Graph nextExpectedRanges entry:
//!   "512-1023" -> 512 ; "655360-" -> 655360.
//! Returns false if `range` has no leading decimal integer.
bool ParseRangeStart(const std::string &range, std::uint64_t &start);

//! The streaming-write state machine behind a write handle: holds AT MOST one
//! chunk of not-yet-sent bytes, so a write of any size needs ~one chunk of
//! memory. The caller (SharepointFileSystem::Write) loops:
//!
//!   while (bytes left) { if (Full()) { send Pending() as a NON-final chunk;
//!                                      MarkChunkSent(); }
//!                        consume Fill(...) bytes; }
//!
//! A full chunk is sent only when MORE bytes arrive -- until then it may be
//! the whole file (Close() sends one PUT) or the final chunk (which must carry
//! the total). So the final chunk is never empty, and a file of exactly one
//! chunk never opens a session. Pure: no I/O, the caller does the sending.
class ChunkedUploadBuffer {
public:
	enum class CloseAction {
		SINGLE_REQUEST, //!< nothing was sent yet: Pending() is the whole file
		FINAL_CHUNK     //!< a session is open: Pending() is its non-empty last chunk
	};

	//! `chunk_bytes` is normalised (NormalizeUploadChunkSize).
	explicit ChunkedUploadBuffer(std::uint64_t chunk_bytes);

	//! Append as much of [data, data+len) as fits in the chunk; returns the
	//! number of bytes taken (0 only when Full() or len == 0).
	std::size_t Fill(const char *data, std::size_t len);
	//! The pending chunk is complete; another byte needs a send first.
	bool Full() const {
		return pending.size() >= chunk;
	}
	//! The pending chunk was accepted by the server as a non-final chunk.
	void MarkChunkSent();
	CloseAction OnClose() const {
		return sent_any ? CloseAction::FINAL_CHUNK : CloseAction::SINGLE_REQUEST;
	}

	//! Content-Range for Pending() minus its first `skip` bytes (a resume):
	//! "bytes a-b/*" for a non-final chunk (total unknown), "bytes a-b/total"
	//! for the final one.
	std::string ContentRange(bool final, std::size_t skip = 0) const;
	//! Given the next byte the server expects, how many bytes of Pending() it
	//! already holds. False when that offset lies outside the pending chunk:
	//! bytes before it are no longer in memory, so it cannot be resumed.
	bool ResumeSkip(std::uint64_t next_expected, std::size_t &skip) const;

	const std::string &Pending() const {
		return pending;
	}
	std::uint64_t ChunkBytes() const {
		return chunk;
	}
	//! Bytes already sent (the absolute offset of Pending()).
	std::uint64_t Offset() const {
		return offset;
	}
	std::uint64_t Total() const {
		return offset + pending.size();
	}
	bool SessionStarted() const {
		return sent_any;
	}
	//! The most bytes ever held at once -- the memory bound, made observable.
	std::uint64_t Peak() const {
		return peak;
	}

private:
	std::uint64_t chunk;
	std::string pending;
	std::uint64_t offset = 0;
	std::uint64_t peak = 0;
	bool sent_any = false;
};

} // namespace sharepoint
} // namespace duckdb
