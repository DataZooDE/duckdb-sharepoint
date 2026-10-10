#pragma once

#include <cstdint>
#include <string>

namespace duckdb {
namespace sharepoint {

// ---------------------------------------------------------------------------
// Pure helpers for the resumable upload-session chunk plan. No I/O, so the
// Catch2 binary exercises them directly. The PUT loop that drives them lives in
// the (impure, Graph-facing) GraphClient::PutContent.
//
// Graph's upload-session contract: every chunk except the last MUST be a
// multiple of 320 KiB (327680 bytes), each PUT carries a Content-Range header,
// and a 5xx/timeout mid-session is resumed by asking the session which byte it
// still expects (nextExpectedRanges).
// ---------------------------------------------------------------------------

//! 320 KiB -- the granularity Graph requires for every non-final chunk.
constexpr std::uint64_t kGraphUploadChunkMultiple = 327680;

//! Round `requested` DOWN to a positive multiple of 320 KiB. Never returns 0: a
//! request below one multiple is clamped up to a single 320-KiB chunk, so the
//! loop always makes progress.
std::uint64_t NormalizeUploadChunkSize(std::uint64_t requested);

//! The value for a chunk's Content-Range request header, end-inclusive:
//!   UploadContentRange(0, 327680, 1000000) -> "bytes 0-327679/1000000"
//! `length` must be >= 1 (the loop never sends an empty chunk).
std::string UploadContentRange(std::uint64_t start, std::uint64_t length, std::uint64_t total);

//! Split an absolute "scheme://host[:port]/path?query" into its origin
//! ("scheme://host[:port]") and the remainder ("/path?query"). Returns false if
//! `url` has no "://" scheme separator. For a URL with no path the remainder
//! defaults to "/". The Graph uploadUrl is a pre-authenticated absolute URL,
//! usually on a different host than the Graph endpoint, so it needs its own
//! httplib client bound to this origin.
bool SplitAbsoluteUrl(const std::string &url, std::string &origin, std::string &path_and_query);

//! True iff `url` is an absolute https:// URL (scheme case-insensitive). Graph
//! hands out pre-authenticated URLs -- the uploadUrl and the /content download
//! redirect -- whose query string IS the credential, so they are only ever
//! followed over TLS; a downgrade to http:// is refused.
bool IsHttpsUrl(const std::string &url);

//! A ranged GET answered 200 (Range ignored) carries the WHOLE file. Cut `body`
//! down to [offset, offset+length) in place. Returns false, leaving `body`
//! untouched, if the body is too short to contain that range.
bool SliceRangeFromFullBody(std::string &body, std::uint64_t offset, std::uint64_t length);

//! Parse the start offset of a Graph nextExpectedRanges entry:
//!   "512-1023" -> 512 ; "655360-" -> 655360.
//! Returns false if `range` has no leading decimal integer.
bool ParseRangeStart(const std::string &range, std::uint64_t &start);

} // namespace sharepoint
} // namespace duckdb
