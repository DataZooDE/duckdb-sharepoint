#pragma once

#include <cstdint>
#include <functional>
#include <list>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace duckdb {
namespace sharepoint {

// ---------------------------------------------------------------------------
// Read block cache.
//
// A ranged GET pays a full round trip regardless of size, and a Parquet scan
// issues many small, often-adjacent reads. Serving those from a few larger
// cached blocks amortises the latency -- the same reason the sibling gdrive
// caches blocks. This is PURE (no duckdb.hpp, no httplib): the byte fetch is
// injected, so the Catch2 binary drives it with a synthetic fetcher and asserts
// exactly how many fetches happen. FileSystem::Read supplies a fetch backed by
// GraphClient::ReadRange.
//
// The cache is process-wide (one per FileSystem, which spans every
// ClientContext), so its keys MUST carry the caller's identity + the item's
// content version -- one tenant must never be served another's bytes, and a
// changed file (new cTag) must not hit stale blocks. FileSystem::Read builds the
// key from AuthContext::identity + item id + cTag.
// ---------------------------------------------------------------------------

//! One block's contribution to a read: copy `length` bytes from `src_in_block`
//! of block `block_index` (which spans [block_offset, block_offset+block_length)
//! in the file) to `dest_offset` in the destination buffer.
struct BlockCopy {
	uint64_t block_index;
	uint64_t block_offset;
	uint64_t block_length;
	uint64_t src_in_block;
	uint64_t dest_offset;
	uint64_t length;
};

//! Plan the per-block copies covering [offset, offset+len) over fixed-size
//! blocks, with the final block clamped to `file_size`. The caller guarantees
//! block_size >= 1 and offset+len <= file_size.
std::vector<BlockCopy> PlanBlockCopies(uint64_t offset, uint64_t len, uint64_t block_size, uint64_t file_size);

//! Process-wide, identity-keyed, single-flight LRU cache of file blocks.
class SharepointBlockCache {
public:
	//! Fetch the whole block [block_offset, block_offset+block_length) and return
	//! exactly that many bytes (the injected impl throws on a short read).
	using Fetch = std::function<std::string(uint64_t block_offset, uint64_t block_length)>;

	explicit SharepointBlockCache(uint64_t max_bytes = 256ULL * 1024 * 1024) : max_bytes(max_bytes) {
	}

	//! Serve [offset, offset+len) of the file identified by `key_prefix` into
	//! `out` (which must have room for `len` bytes). Blocks not resident are
	//! fetched exactly once, even if several threads request the same block at
	//! once (single-flight per block). `fetch` only ever sees whole-block ranges.
	void Read(const std::string &key_prefix, uint64_t block_size, uint64_t file_size, uint64_t offset,
	          uint64_t len, char *out, const Fetch &fetch);

	//! Current resident bytes -- for tests and introspection.
	uint64_t ResidentBytes();

private:
	//! A cached block. `load_mtx` serialises the one-time fill so concurrent
	//! readers of the same block share a single fetch.
	struct Block {
		std::mutex load_mtx;
		bool loaded = false;
		std::string data;
	};

	std::shared_ptr<Block> AcquireBlock(const std::string &key);
	void AccountAndEvict(const std::string &just_loaded_key, uint64_t added_bytes);

	std::mutex mtx; // guards blocks, lru, lru_pos, resident_bytes
	std::unordered_map<std::string, std::shared_ptr<Block>> blocks;
	std::list<std::string> lru; // front = most recently used
	std::unordered_map<std::string, std::list<std::string>::iterator> lru_pos;
	uint64_t max_bytes;
	uint64_t resident_bytes = 0;
};

} // namespace sharepoint
} // namespace duckdb
