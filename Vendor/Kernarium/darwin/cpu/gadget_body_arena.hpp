// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <memory>
#include <memory_resource>
#include <new>
#include <vector>

#include <sys/mman.h>

#include "cpu/gadget_cache_budget.hpp"

namespace uml::cpu::gadget_cache {

/* Storage for stored translations.
 *
 * A translation is one allocation: its body, the stream words behind it, and
 * the guest words it is validated against behind those, sized exactly. The
 * cache holds tens of thousands of them per executor, so a rounding allocator
 * costs more than the streams do: a pmr pool asked its upstream for 204 MiB to
 * hold 64 MiB of programs on a measured six-vCPU guest, between its own size
 * classes and the capacity a stream reached by doubling. Sixteen-byte classes
 * over bump-allocated chunks hold the same programs in 65 MiB.
 *
 * Each address space has an arena of its own, and an arena reuses freed space
 * by best fit: a block comes from the smallest free block that holds it, split
 * off the front, and a freed block merges with the free blocks on either side,
 * so a hole serves bodies of any size rather than only bodies of the size that
 * left it. A chunk whose last block is freed is unmapped, and the address
 * space's remaining chunks go with it; one that runs little code costs one
 * chunk. With freed blocks kept only for their own sixteen-byte size and
 * chunks never returned, a browser that cycles through more address spaces
 * than the cache keeps held an executor's arena at its high-water mark, about
 * twice its live bodies. See G226 and G257. A free touches a few map words
 * and at most two neighbours, so retiring an address space still costs in
 * proportion to what it owned. See G167.
 *
 * Chunks are carved from anonymous mappings of their own rather than heap
 * blocks: the heap keeps freed blocks of this size resident, in the
 * process's footprint, until it hands them out again. An arena maps a few
 * chunks at a time, doubling to sixteen, because a browser retires address
 * spaces by the thousand and a mapping per chunk made their mapping and
 * unmapping the arena's largest cost; an address space that dies unmaps its
 * regions whole, without freeing a body of it.
 *
 * The arena also answers which live body holds an address. Each chunk keeps
 * one bit per granule, set at the first granule of every live block, so the
 * block holding an address is the nearest set bit at or below it. The cache
 * asks this when a terminal exit lands in a linked successor, which a
 * measured guest did about once per hundred and twenty stores; the index by
 * stream address that this replaces was maintained at every one of those
 * stores, five set installs per program. See G203. A second map marks where
 * each free block starts; the blocks of a chunk tile it up to where it is
 * still being carved, and no two free blocks are adjacent, so the two maps
 * give every block's extent.
 *
 * An arena given an Account (`meter`) charges it with what bytes() and
 * index_bytes() count, as chunks are carved and released and blocks too big
 * for a class come and go; its end refunds nothing, since the account goes
 * with the address space.
 */
class BodyArena final {
public:
	explicit BodyArena(std::pmr::memory_resource *upstream) noexcept
		: upstream_{upstream}
	{}

	BodyArena(const BodyArena &) = delete;
	BodyArena &operator=(const BodyArena &) = delete;

	~BodyArena()
	{
		for (const auto &region : regions_) {
			for (std::uint32_t at = 0; at < region.chunks;) {
				if ((region.mapped >> at & 1U) == 0) {
					++at;
					continue;
				}
				auto end = at;
				while (end < region.chunks &&
					(region.mapped >> end & 1U) != 0)
					++end;
				unmap(region.base + std::size_t{at} * chunk_bytes,
					std::size_t{end - at} * chunk_bytes);
				at = end;
			}
		}
		for (const auto &direct : direct_)
			upstream_->deallocate(direct.base, direct.bytes,
				alignof(std::max_align_t));
	}

	[[nodiscard]] void *allocate(std::size_t bytes)
	{
		const auto size = rounded(bytes);
		if (size > largest_class) {
			/* Too big for a class to recycle, so it is not carved
			 * out of a chunk either; it is freed on its own. */
			auto *block = static_cast<std::byte *>(
				upstream_->allocate(size,
					alignof(std::max_align_t)));
			try {
				direct_.push_back({block, size});
			} catch (...) {
				upstream_->deallocate(block, size,
					alignof(std::max_align_t));
				throw;
			}
			charge(static_cast<std::ptrdiff_t>(size));
			return block;
		}
		const auto granules = size / granule;
		if (auto *block = reuse(granules); block != nullptr)
			return block;
		return carve(granules);
	}

	void deallocate(void *block, std::size_t bytes) noexcept
	{
		const auto size = rounded(bytes);
		auto *freed = static_cast<std::byte *>(block);
		if (size > largest_class) {
			const auto held = std::find_if(direct_.begin(),
				direct_.end(), [freed](const Direct &direct) {
					return direct.base == freed;
				});
			if (held != direct_.end()) {
				direct_.erase(held);
				charge(-static_cast<std::ptrdiff_t>(size));
			}
			upstream_->deallocate(block, size,
				alignof(std::max_align_t));
			return;
		}
		const auto &chunk = *chunk_of(freed);
		auto first = static_cast<std::size_t>(freed - chunk.base) /
			granule;
		auto count = size / granule;
		clear(chunk.starts(), first);
		const auto end = tiled(chunk);
		if (first + count < end && test(chunk.free(), first + count)) {
			const auto next = first + count;
			const auto next_count = extent(chunk, next, end);
			unlist(chunk.base + next * granule, next_count);
			clear(chunk.free(), next);
			count += next_count;
		}
		if (first != 0) {
			const auto previous = previous_start(chunk, first);
			if (test(chunk.free(), previous)) {
				unlist(chunk.base + previous * granule,
					first - previous);
				clear(chunk.free(), previous);
				count += first - previous;
				first = previous;
			}
		}
		if (chunk.base == current_ && first + count == used_) {
			/* Back into the part of the chunk still being
			 * carved. */
			used_ = first;
			return;
		}
		if (count == chunk_granules) {
			release(chunk);
			return;
		}
		set(chunk.free(), first);
		list(chunk.base + first * granule, count);
	}

	/* The live block holding an address, or nullptr. */
	[[nodiscard]] void *owner(const void *address) const noexcept
	{
		const auto *at = static_cast<const std::byte *>(address);
		if (const auto *chunk = chunk_of(at); chunk != nullptr) {
			const auto index =
				static_cast<std::size_t>(at - chunk->base) / granule;
			/* No live block spans more than the largest class, so
			 * a start any further below is another block's. */
			const auto floor = index >= largest_class / granule ?
				(index - largest_class / granule) / 64 : 0;
			const auto *starts = chunk->starts();
			auto word = index / 64;
			auto bits = starts[word] &
				(~std::uint64_t{0} >> (63 - index % 64));
			for (;;) {
				if (bits != 0)
					return chunk->base +
						(word * 64 + 63 -
							static_cast<std::size_t>(
								std::countl_zero(bits))) *
						granule;
				if (word == floor)
					return nullptr;
				bits = starts[--word];
			}
		}
		for (const auto &direct : direct_)
			if (at >= direct.base && at < direct.base + direct.bytes)
				return direct.base;
		return nullptr;
	}

	/* What the arena holds, which is what the cache costs. */
	[[nodiscard]] std::size_t bytes() const noexcept
	{
		std::size_t total = chunks_.size() * chunk_bytes;
		for (const auto &direct : direct_)
			total += direct.bytes;
		return total;
	}

	/* What a block for `bytes` takes from the arena. */
	[[nodiscard]] static std::size_t block_bytes(std::size_t bytes) noexcept
	{
		return rounded(bytes);
	}

	/* The start maps, which are what owner() and reuse cost. */
	[[nodiscard]] std::size_t index_bytes() const noexcept
	{
		return chunks_.size() * chunk_map_bytes;
	}

	/* Charge `account`, null for none, with bytes() and index_bytes() from
	 * here on, and refund the account it charged until now. */
	void meter(gadget_cache::Account *account) noexcept
	{
		const auto held = static_cast<std::ptrdiff_t>(bytes() +
			index_bytes());
		charge(-held);
		meter_ = account;
		charge(held);
	}

#if UML_GADGET_CACHE_CENSUS
	/* Chunks that hold no live body. A chunk is unmapped when its last
	 * block is freed, so only the one being carved can be empty. */
	[[nodiscard]] std::size_t empty_bytes() const noexcept
	{
		std::size_t total = 0;
		for (const auto &chunk : chunks_) {
			if (chunk.base == current_)
				continue;
			const auto *starts = chunk.starts();
			if (std::all_of(starts, starts + map_words,
				    [](std::uint64_t word) { return word == 0; }))
				total += chunk_bytes;
		}
		return total;
	}
#endif

private:
	/* Eight bytes would fit the free-list link, but sixteen keeps every
	 * body's stream tail aligned for a pair access and still rounds a mean
	 * body by eight bytes. */
	static constexpr std::size_t granule = 16;
	/* Up to 16 KiB in classes, which a 64-instruction block reaches only at
	 * 32 stream words per guest instruction. */
	static constexpr std::size_t classes = 1024;
	static constexpr std::size_t largest_class = (classes - 1) * granule;
	/* Small enough that a chunk empties, large enough to hold the largest
	 * class several times over; whole host pages. */
	static constexpr std::size_t chunk_bytes = std::size_t{64} << 10;
	static constexpr std::size_t chunk_granules = chunk_bytes / granule;
	static constexpr std::size_t map_words = chunk_granules / 64;
	static constexpr std::size_t chunk_map_bytes =
		2 * map_words * sizeof(std::uint64_t);
	/* A region doubles the arena's reach, from four chunks to sixteen: an
	 * address space that runs a browser's code holds about seventeen, so
	 * it maps four times where one chunk at a time mapped seventeen. */
	static constexpr std::size_t first_region_chunks = 4;
	static constexpr std::size_t region_chunks = 16;
	static_assert(region_chunks <= 32, "a region's chunks fit its mask");
	/* Free blocks by size: one list per class, and the last for the runs
	 * merging makes longer than the largest class. */
	static constexpr std::size_t lists = classes + 1;
	static_assert(chunk_bytes % (std::size_t{16} << 10) == 0 &&
			chunk_bytes >= 2 * largest_class,
		"chunks are whole host pages and hold the largest class");

	[[nodiscard]] static std::size_t rounded(std::size_t bytes) noexcept
	{
		return (std::max(bytes, granule) + granule - 1) & ~(granule - 1);
	}

	/* What a free block holds while it is free: its list links. */
	struct FreeBlock final {
		FreeBlock *next;
		FreeBlock *previous;
	};
	static_assert(sizeof(FreeBlock) <= granule,
		"a one-granule free block holds its links");

	struct Chunk final {
		std::byte *base;
		/* Two maps of one bit per granule: where each live block
		 * starts, then where each free block starts. */
		std::unique_ptr<std::uint64_t[]> maps;

		[[nodiscard]] std::uint64_t *starts() const noexcept
		{
			return maps.get();
		}
		[[nodiscard]] std::uint64_t *free() const noexcept
		{
			return maps.get() + map_words;
		}
	};

	/* A block above the largest class, held outside the chunks. */
	struct Direct final {
		std::byte *base;
		std::size_t bytes;
	};

	/* One mapping of up to region_chunks chunks, in address order among
	 * the arena's regions. The ranges of two regions can interleave: the
	 * system fits a new mapping into the first hole that holds it, and the
	 * chunks an arena released from the end of one region and the start of
	 * the next leave a hole that holds a whole region. */
	struct Region final {
		std::byte *base;
		std::uint32_t chunks;
		/* One bit per chunk still mapped: a chunk released while the
		 * arena lives is unmapped alone, and the arena's end unmaps
		 * only what is left, since the address space of the others
		 * may be another mapping's by then. */
		std::uint32_t mapped;
	};

	[[nodiscard]] static bool test(const std::uint64_t *map,
		std::size_t index) noexcept
	{
		return (map[index / 64] >> (index % 64) & 1U) != 0;
	}

	static void set(std::uint64_t *map, std::size_t index) noexcept
	{
		map[index / 64] |= std::uint64_t{1} << (index % 64);
	}

	static void clear(std::uint64_t *map, std::size_t index) noexcept
	{
		map[index / 64] &= ~(std::uint64_t{1} << (index % 64));
	}

	[[nodiscard]] static std::size_t list_of(std::size_t granules) noexcept
	{
		return std::min(granules, lists - 1);
	}

	/* Where the blocks of a chunk end: the carving point of the chunk
	 * being carved, and the chunk's end for every other. */
	[[nodiscard]] std::size_t tiled(const Chunk &chunk) const noexcept
	{
		return chunk.base == current_ ? used_ : chunk_granules;
	}

	/* The granules of the free block at `first`: up to the next live
	 * block, since free blocks are never adjacent. */
	[[nodiscard]] static std::size_t extent(const Chunk &chunk,
		std::size_t first, std::size_t end) noexcept
	{
		if (first + 1 >= end)
			return end - first;
		const auto *starts = chunk.starts();
		auto word = (first + 1) / 64;
		auto bits = starts[word] &
			(~std::uint64_t{0} << ((first + 1) % 64));
		for (;;) {
			if (bits != 0) {
				const auto next = word * 64 +
					static_cast<std::size_t>(
						std::countr_zero(bits));
				return std::min(end, next) - first;
			}
			if (++word * 64 >= end)
				return end - first;
			bits = starts[word];
		}
	}

	/* Where the block before granule `first` starts, live or free. */
	[[nodiscard]] static std::size_t previous_start(const Chunk &chunk,
		std::size_t first) noexcept
	{
		const auto *starts = chunk.starts();
		const auto *free = chunk.free();
		auto word = (first - 1) / 64;
		auto bits = (starts[word] | free[word]) &
			(~std::uint64_t{0} >> (63 - (first - 1) % 64));
		while (bits == 0) {
			--word;
			bits = starts[word] | free[word];
		}
		return word * 64 + 63 -
			static_cast<std::size_t>(std::countl_zero(bits));
	}

	void list(std::byte *block, std::size_t granules) noexcept
	{
		const auto index = list_of(granules);
		auto *entry = reinterpret_cast<FreeBlock *>(block);
		entry->next = free_[index];
		entry->previous = nullptr;
		if (entry->next != nullptr)
			entry->next->previous = entry;
		free_[index] = entry;
		occupied_[index / 64] |= std::uint64_t{1} << (index % 64);
	}

	void unlist(std::byte *block, std::size_t granules) noexcept
	{
		const auto index = list_of(granules);
		auto *entry = reinterpret_cast<FreeBlock *>(block);
		if (entry->previous != nullptr)
			entry->previous->next = entry->next;
		else
			free_[index] = entry->next;
		if (entry->next != nullptr)
			entry->next->previous = entry->previous;
		if (free_[index] == nullptr)
			occupied_[index / 64] &=
				~(std::uint64_t{1} << (index % 64));
	}

	/* The smallest free block that holds `granules`, with what it does
	 * not need left free behind it, or nullptr. */
	[[nodiscard]] std::byte *reuse(std::size_t granules) noexcept
	{
		auto word = granules / 64;
		auto bits = occupied_[word] &
			(~std::uint64_t{0} << (granules % 64));
		while (bits == 0) {
			if (++word == occupied_.size())
				return nullptr;
			bits = occupied_[word];
		}
		const auto index = word * 64 +
			static_cast<std::size_t>(std::countr_zero(bits));
		auto *block = reinterpret_cast<std::byte *>(free_[index]);
		const auto &chunk = *chunk_of(block);
		const auto first =
			static_cast<std::size_t>(block - chunk.base) / granule;
		const auto count = index < lists - 1 ? index :
			extent(chunk, first, tiled(chunk));
		unlist(block, count);
		clear(chunk.free(), first);
		set(chunk.starts(), first);
		if (count > granules) {
			set(chunk.free(), first + granules);
			list(block + granules * granule, count - granules);
		}
		return block;
	}

	/* A block from the part of the current chunk not yet carved, mapping
	 * a new chunk when it is used up. The rest of the old one becomes a
	 * free block; the block before it is live, since a block freed at the
	 * carving point moves the point back instead. */
	[[nodiscard]] std::byte *carve(std::size_t granules)
	{
		if (current_ == nullptr || granules > chunk_granules - used_) {
			auto maps = std::make_unique<std::uint64_t[]>(
				2 * map_words);
			auto *starts = maps.get();
			auto *base = take_chunk();
			try {
				/* Kept in address order, which is what owner()
				 * searches. */
				const auto at = std::upper_bound(
					chunks_.begin(), chunks_.end(), base,
					before_chunk);
				chunks_.insert(at,
					Chunk{base, std::move(maps)});
			} catch (...) {
				/* The chunk just taken, so the region has it back. */
				spare_ = base;
				++spare_left_;
				--carved_;
				throw;
			}
			charge(static_cast<std::ptrdiff_t>(chunk_bytes +
				chunk_map_bytes));
			if (current_ != nullptr && used_ < chunk_granules) {
				set(current_starts_ + map_words, used_);
				list(current_ + used_ * granule,
					chunk_granules - used_);
			}
			current_ = base;
			current_starts_ = starts;
			used_ = 0;
		}
		auto *block = current_ + used_ * granule;
		set(current_starts_, used_);
		used_ += granules;
		return block;
	}

	/* A chunk whose blocks are all free, other than the current one:
	 * unmap it. Its region is the nearest at or below it that still maps
	 * it, not simply the nearest, since ranges interleave (see Region);
	 * a bit cleared in another region would leave the chunk's own region
	 * believing it mapped, and the arena's end would unmap the address
	 * again from whatever holds it by then. */
	void release(const Chunk &chunk) noexcept
	{
		auto *base = chunk.base;
		auto region = std::upper_bound(regions_.begin(), regions_.end(),
			base, [](const std::byte *address, const Region &held) {
				return address < held.base;
			});
		std::uint32_t bit = 0;
		while (bit == 0)
			bit = chunk_bit(*--region, base);
		region->mapped &= ~bit;
		unmap(base, chunk_bytes);
		if (region->mapped == 0)
			regions_.erase(region);
		chunks_.erase(std::lower_bound(chunks_.begin(), chunks_.end(),
			base, [](const Chunk &held, const std::byte *address) {
				return held.base < address;
			}));
		charge(-static_cast<std::ptrdiff_t>(chunk_bytes +
			chunk_map_bytes));
	}

	/* The next chunk of the arena, from the region being carved or from a
	 * new one. A region is one mapping of several chunks, which only
	 * reserves address space until its chunks are carved, so an arena that
	 * grows maps a few times rather than once per chunk. */
	[[nodiscard]] std::byte *take_chunk()
	{
		if (spare_left_ == 0) {
			auto want = std::clamp(
				carved_ == 0 ? first_region_chunks : carved_,
				std::size_t{1}, region_chunks);
			void *base = MAP_FAILED;
			for (;; want = 1) {
				base = mmap(nullptr, want * chunk_bytes,
					PROT_READ | PROT_WRITE,
					MAP_PRIVATE | MAP_ANON, -1, 0);
				if (base != MAP_FAILED)
					break;
				if (want == 1)
					throw std::bad_alloc{};
			}
			auto *at = static_cast<std::byte *>(base);
			try {
				regions_.insert(std::upper_bound(regions_.begin(),
						regions_.end(), at,
						[](const std::byte *address,
							const Region &held) {
							return address < held.base;
						}),
					Region{at,
						static_cast<std::uint32_t>(want),
						static_cast<std::uint32_t>(
							(std::uint64_t{1} << want) - 1)});
			} catch (...) {
				(void)munmap(base, want * chunk_bytes);
				throw;
			}
			spare_ = at;
			spare_left_ = want;
		}
		auto *chunk = spare_;
		spare_ += chunk_bytes;
		--spare_left_;
		++carved_;
		return chunk;
	}

	/* The bit of the chunk at `base` in a region at or below it, or 0
	 * when that region does not map the chunk. */
	[[nodiscard]] static std::uint32_t chunk_bit(const Region &region,
		const std::byte *base) noexcept
	{
		const auto index = static_cast<std::size_t>(base - region.base) /
			chunk_bytes;
		return index < region.chunks ?
			(region.mapped & std::uint32_t{1} << index) : 0;
	}

	static void unmap(std::byte *base, std::size_t bytes) noexcept
	{
		(void)munmap(base, bytes);
	}

	void charge(std::ptrdiff_t bytes) noexcept
	{
		if (meter_ != nullptr)
			meter_->charge(bytes);
	}

	[[nodiscard]] static bool before_chunk(
		const std::byte *address, const Chunk &chunk) noexcept
	{
		return address < chunk.base;
	}

	[[nodiscard]] const Chunk *chunk_of(
		const std::byte *address) const noexcept
	{
		const auto after = std::upper_bound(
			chunks_.begin(), chunks_.end(), address, before_chunk);
		if (after == chunks_.begin())
			return nullptr;
		const auto &chunk = *std::prev(after);
		return address < chunk.base + chunk_bytes ? &chunk : nullptr;
	}

	std::pmr::memory_resource *upstream_;
	gadget_cache::Account *meter_{};
	/* In address order. */
	std::vector<Chunk> chunks_;
	std::vector<Region> regions_;
	std::vector<Direct> direct_;
	/* The chunks of the newest region not yet taken. */
	std::byte *spare_{};
	std::size_t spare_left_{};
	/* Chunks taken so far, which sizes the next region. */
	std::size_t carved_{};
	std::byte *current_{};
	std::uint64_t *current_starts_{};
	/* Granules of the current chunk carved so far. */
	std::size_t used_{};
	std::array<FreeBlock *, lists> free_{};
	/* One bit per list that holds a block. */
	std::array<std::uint64_t, (lists + 63) / 64> occupied_{};
};

} // namespace uml::cpu::gadget_cache
