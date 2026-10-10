#include "sharepoint_blockcache.hpp"

#include <algorithm>
#include <cstdint>
#include <stdexcept>

namespace duckdb {
namespace sharepoint {

std::vector<BlockCopy> PlanBlockCopies(uint64_t offset, uint64_t len, uint64_t block_size, uint64_t file_size) {
	std::vector<BlockCopy> plan;
	if (len == 0 || block_size == 0) {
		return plan;
	}
	const uint64_t end = offset + len; // caller guarantees end <= file_size
	uint64_t pos = offset;
	while (pos < end) {
		const uint64_t index = pos / block_size;
		const uint64_t block_offset = index * block_size;
		// The final block is short when the file does not fill it.
		const uint64_t block_length = std::min(block_size, file_size - block_offset);
		const uint64_t src_in_block = pos - block_offset;
		const uint64_t avail = block_length - src_in_block;
		const uint64_t take = std::min(avail, end - pos);
		plan.push_back(BlockCopy {index, block_offset, block_length, src_in_block, pos - offset, take});
		pos += take;
	}
	return plan;
}

std::shared_ptr<SharepointBlockCache::Block> SharepointBlockCache::AcquireBlock(const std::string &key) {
	std::lock_guard<std::mutex> g(mtx);
	auto it = blocks.find(key);
	if (it != blocks.end()) {
		// Touch: move to the front of the LRU.
		lru.erase(lru_pos[key]);
		lru.push_front(key);
		lru_pos[key] = lru.begin();
		return it->second;
	}
	auto blk = std::make_shared<Block>();
	blocks[key] = blk;
	lru.push_front(key);
	lru_pos[key] = lru.begin();
	return blk;
}

void SharepointBlockCache::AccountAndEvict(const std::string &just_loaded_key, const std::shared_ptr<Block> &blk) {
	std::lock_guard<std::mutex> g(mtx);
	// A block a failed fetch already Forget()-ed -- or one evicted by now, see
	// below -- is no longer in the map: it serves only the read that loaded it
	// and must not be counted.
	auto cur = blocks.find(just_loaded_key);
	if (cur == blocks.end() || cur->second != blk) {
		return;
	}
	// Counting and becoming evictable happen together, under mtx: eviction only
	// ever subtracts bytes that were added. (Deciding eviction on `loaded`,
	// which is written under the block's load_mtx, let another thread evict a
	// block and subtract its bytes before they were added -- the unsigned
	// counter then wrapped. It was also a data race; TSan flagged it.)
	blk->accounted = true;
	resident_bytes += blk->data.size();
	// Evict least-recently-used accounted blocks until back within budget.
	// Blocks still loading are not yet accounted and are skipped, so one slow or
	// failing fetch at the LRU tail cannot wedge eviction.
	auto it = lru.end();
	while (resident_bytes > max_bytes && it != lru.begin()) {
		--it;
		const std::string victim = *it;
		auto vit = blocks.find(victim);
		if (vit == blocks.end() || !vit->second->accounted || victim == just_loaded_key) {
			continue;
		}
		resident_bytes -= vit->second->data.size();
		blocks.erase(vit);
		lru_pos.erase(victim);
		it = lru.erase(it);
	}
}

void SharepointBlockCache::Forget(const std::string &key, const std::shared_ptr<Block> &blk) {
	std::lock_guard<std::mutex> g(mtx);
	auto it = blocks.find(key);
	if (it == blocks.end() || it->second != blk) {
		return;
	}
	blocks.erase(it);
	lru.erase(lru_pos[key]);
	lru_pos.erase(key);
}

void SharepointBlockCache::Read(const std::string &key_prefix, uint64_t block_size, uint64_t file_size,
                                uint64_t offset, uint64_t len, char *out, const Fetch &fetch) {
	if (len == 0) {
		return;
	}
	for (const auto &bc : PlanBlockCopies(offset, len, block_size, file_size)) {
		// The block size is part of the key: block N means a different byte span
		// at a different size, and two sessions may use different sizes.
		const std::string key = key_prefix + "#" + std::to_string(block_size) + "@" + std::to_string(bc.block_index);
		auto blk = AcquireBlock(key);
		{
			// Single-flight: only the first arrival fetches; the rest wait here
			// and then find loaded == true.
			std::lock_guard<std::mutex> lg(blk->load_mtx);
			if (!blk->loaded) {
				try {
					blk->data = fetch(bc.block_offset, bc.block_length);
				} catch (...) {
					// Leave nothing behind: a waiter on this block retries the fetch
					// itself, and the next reader starts clean.
					Forget(key, blk);
					throw;
				}
				if (blk->data.size() != bc.block_length) {
					Forget(key, blk);
					throw std::runtime_error("sharepoint: block fetch returned " + std::to_string(blk->data.size()) +
					                         " bytes, expected " + std::to_string(bc.block_length));
				}
				blk->loaded = true;
				AccountAndEvict(key, blk);
			}
		}
		std::copy(blk->data.begin() + static_cast<std::ptrdiff_t>(bc.src_in_block),
		          blk->data.begin() + static_cast<std::ptrdiff_t>(bc.src_in_block + bc.length),
		          out + bc.dest_offset);
	}
}

uint64_t SharepointBlockCache::ResidentBytes() {
	std::lock_guard<std::mutex> g(mtx);
	return resident_bytes;
}

} // namespace sharepoint
} // namespace duckdb
