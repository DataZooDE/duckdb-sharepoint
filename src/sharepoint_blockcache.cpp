#include "sharepoint_blockcache.hpp"

#include <algorithm>
#include <cstdint>

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

void SharepointBlockCache::AccountAndEvict(const std::string &just_loaded_key, uint64_t added_bytes) {
	std::lock_guard<std::mutex> g(mtx);
	resident_bytes += added_bytes;
	// Evict least-recently-used LOADED blocks until back within budget. An
	// in-flight (not-yet-loaded) block at the tail stops eviction rather than
	// being dropped mid-load -- its bytes are not yet counted, so leaving it is
	// safe and avoids a use-after-evict accounting leak.
	while (resident_bytes > max_bytes && !lru.empty()) {
		const std::string victim = lru.back();
		auto vit = blocks.find(victim);
		if (vit == blocks.end()) {
			lru.pop_back();
			lru_pos.erase(victim);
			continue;
		}
		if (!vit->second->loaded || victim == just_loaded_key) {
			break;
		}
		resident_bytes -= vit->second->data.size();
		blocks.erase(vit);
		lru_pos.erase(victim);
		lru.pop_back();
	}
}

void SharepointBlockCache::Read(const std::string &key_prefix, uint64_t block_size, uint64_t file_size,
                                uint64_t offset, uint64_t len, char *out, const Fetch &fetch) {
	if (len == 0) {
		return;
	}
	for (const auto &bc : PlanBlockCopies(offset, len, block_size, file_size)) {
		const std::string key = key_prefix + "@" + std::to_string(bc.block_index);
		auto blk = AcquireBlock(key);
		{
			// Single-flight: only the first arrival fetches; the rest wait here
			// and then find loaded == true.
			std::lock_guard<std::mutex> lg(blk->load_mtx);
			if (!blk->loaded) {
				blk->data = fetch(bc.block_offset, bc.block_length);
				blk->loaded = true;
				AccountAndEvict(key, blk->data.size());
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
