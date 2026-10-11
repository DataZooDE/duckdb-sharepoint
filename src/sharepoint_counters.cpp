#include "sharepoint_counters.hpp"

#include <atomic>
#include <cstdint>
#include <cstddef>

namespace duckdb {
namespace sharepoint {

namespace {

constexpr std::size_t kCounters = static_cast<std::size_t>(Counter::COUNT_);

std::atomic<uint64_t> &Slot(Counter c) {
	static std::atomic<uint64_t> slots[kCounters] = {};
	return slots[static_cast<std::size_t>(c)];
}

} // namespace

const char *CounterName(Counter c) {
	switch (c) {
	case Counter::TOKEN_REQUEST:
		return "token_request";
	case Counter::STAT:
		return "stat";
	case Counter::READ_RANGE:
		return "read_range";
	case Counter::LIST_PAGE:
		return "list_page";
	case Counter::UPLOAD_PUT:
		return "upload_put";
	case Counter::UPLOAD_SESSION:
		return "upload_session";
	case Counter::UPLOAD_CHUNK:
		return "upload_chunk";
	case Counter::DELETE_ITEM:
		return "delete";
	case Counter::MOVE:
		return "move";
	case Counter::CREATE_FOLDER:
		return "create_folder";
	case Counter::RETRY:
		return "retries";
	case Counter::THROTTLED:
		return "throttled";
	case Counter::BYTES_READ:
		return "bytes_read";
	case Counter::BYTES_WRITTEN:
		return "bytes_written";
	case Counter::META_CACHE_HIT:
		return "meta_cache_hits";
	case Counter::AUTH_REFRESH:
		return "auth_refresh";
	default:
		return "unknown";
	}
}

void IncrementCounter(Counter c, uint64_t by) {
	Slot(c).fetch_add(by, std::memory_order_relaxed);
}

std::vector<std::pair<const char *, uint64_t>> SnapshotCounters() {
	std::vector<std::pair<const char *, uint64_t>> out;
	out.reserve(kCounters);
	for (std::size_t i = 0; i < kCounters; i++) {
		const auto c = static_cast<Counter>(i);
		out.emplace_back(CounterName(c), Slot(c).load(std::memory_order_relaxed));
	}
	return out;
}

void ResetCounters() {
	for (std::size_t i = 0; i < kCounters; i++) {
		Slot(static_cast<Counter>(i)).store(0, std::memory_order_relaxed);
	}
}

} // namespace sharepoint
} // namespace duckdb
