// SPDX-License-Identifier: GPL-2.0-or-later
#include "domain_heap.hpp"

#include <sys/mman.h>
#include <unistd.h>

#include <mach/vm_statistics.h>

#include <cstring>

#ifndef UML_HEAP_REGION_BYTES
#define UML_HEAP_REGION_BYTES (std::size_t{8} << 30)
#endif

namespace uml::memory {
namespace {

constexpr std::size_t region_bytes = UML_HEAP_REGION_BYTES;
constexpr std::size_t first_chunk = std::size_t{256} << 10;
constexpr std::size_t last_chunk = std::size_t{4} << 20;
/* Space ahead of a large block's payload, for its header. */
constexpr std::size_t large_header = 64;

std::atomic<std::size_t> live_heap_count{};

[[nodiscard]] std::size_t host_page() noexcept
{
	return static_cast<std::size_t>(getpagesize());
}

[[nodiscard]] std::size_t page_round(std::size_t bytes) noexcept
{
	const auto page = host_page();
	return (bytes + page - 1) & ~(page - 1);
}

[[nodiscard]] bool valid_alignment(std::size_t alignment) noexcept
{
	return alignment != 0 && (alignment & (alignment - 1)) == 0 &&
		alignment <= host_page();
}

[[nodiscard]] std::size_t payload_offset(std::size_t alignment) noexcept
{
	return alignment > large_header ? alignment : large_header;
}

} // namespace

HeapRegion &HeapRegion::instance() noexcept
{
	[[clang::no_destroy]] static HeapRegion region;
	return region;
}

std::size_t HeapRegion::reservation() noexcept
{
	return region_bytes;
}

bool HeapRegion::contains(const void *pointer) const noexcept
{
	const auto start = base();
	const auto address = reinterpret_cast<std::uintptr_t>(pointer);
	return start != 0 && address >= start && address - start < region_bytes;
}

bool HeapRegion::reserve_locked() noexcept
{
	if (base_.load(std::memory_order_relaxed) != 0)
		return true;
	void *memory = mmap(nullptr, region_bytes, PROT_NONE,
		MAP_PRIVATE | MAP_ANON | MAP_NORESERVE, -1, 0);
	if (memory == MAP_FAILED)
		return false;
	tail_ = reinterpret_cast<std::uintptr_t>(memory);
	base_.store(tail_, std::memory_order_release);
	return true;
}

void *HeapRegion::take(std::size_t bytes, int tag) noexcept
{
	if (bytes == 0 || (bytes & (host_page() - 1)) != 0)
		return nullptr;
	os_unfair_lock_lock(&lock_);
	std::uintptr_t found = 0;
	for (std::size_t index = 0; index < free_count_; ++index) {
		if (free_[index].size < bytes)
			continue;
		found = free_[index].start;
		free_[index].start += bytes;
		free_[index].size -= bytes;
		if (free_[index].size == 0) {
			for (auto move = index + 1; move < free_count_; ++move)
				free_[move - 1] = free_[move];
			--free_count_;
		}
		break;
	}
	if (found == 0 && reserve_locked()) {
		const auto start = base_.load(std::memory_order_relaxed);
		if (bytes <= region_bytes && tail_ - start <= region_bytes - bytes) {
			found = tail_;
			tail_ += bytes;
		}
	}
	os_unfair_lock_unlock(&lock_);
	if (found == 0)
		return nullptr;
	void *memory = mmap(reinterpret_cast<void *>(found), bytes,
		PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON | MAP_FIXED,
		VM_MAKE_TAG(tag), 0);
	if (memory == MAP_FAILED) {
		give_range(found, bytes);
		return nullptr;
	}
	committed_.fetch_add(bytes, std::memory_order_relaxed);
	return memory;
}

void HeapRegion::give(void *memory, std::size_t bytes) noexcept
{
	/* The pages go with the new mapping. A failure leaves them committed and
	 * the range out of the table, which keeps a later block from landing on
	 * pages that still hold data. */
	if (mmap(memory, bytes, PROT_NONE,
		    MAP_PRIVATE | MAP_ANON | MAP_FIXED | MAP_NORESERVE, -1, 0) ==
	    MAP_FAILED)
		return;
	committed_.fetch_sub(bytes, std::memory_order_relaxed);
	give_range(reinterpret_cast<std::uintptr_t>(memory), bytes);
}

void HeapRegion::give_range(std::uintptr_t start, std::size_t size) noexcept
{
	os_unfair_lock_lock(&lock_);
	if (start + size == tail_) {
		tail_ = start;
		while (free_count_ != 0 &&
		       free_[free_count_ - 1].start + free_[free_count_ - 1].size ==
			       tail_) {
			tail_ = free_[free_count_ - 1].start;
			--free_count_;
		}
		os_unfair_lock_unlock(&lock_);
		return;
	}
	std::size_t index = 0;
	while (index < free_count_ && free_[index].start < start)
		++index;
	const bool merge_before = index > 0 &&
		free_[index - 1].start + free_[index - 1].size == start;
	const bool merge_after = index < free_count_ &&
		start + size == free_[index].start;
	if (merge_before && merge_after) {
		free_[index - 1].size += size + free_[index].size;
		for (auto move = index + 1; move < free_count_; ++move)
			free_[move - 1] = free_[move];
		--free_count_;
	} else if (merge_before) {
		free_[index - 1].size += size;
	} else if (merge_after) {
		free_[index].start = start;
		free_[index].size += size;
	} else if (free_count_ < capacity) {
		for (auto move = free_count_; move > index; --move)
			free_[move] = free_[move - 1];
		free_[index] = {start, size};
		++free_count_;
	}
	os_unfair_lock_unlock(&lock_);
}

DomainHeap::DomainHeap() noexcept : next_chunk_{first_chunk}
{
	live_heap_count.fetch_add(1, std::memory_order_relaxed);
}

DomainHeap::~DomainHeap()
{
	release();
	live_heap_count.fetch_sub(1, std::memory_order_relaxed);
}

std::size_t DomainHeap::live_heaps() noexcept
{
	return live_heap_count.load(std::memory_order_relaxed);
}

void *DomainHeap::map(std::size_t bytes) noexcept
{
	void *memory = HeapRegion::instance().take(bytes, vm_tag);
	if (memory != nullptr)
		mapped_.fetch_add(bytes, std::memory_order_relaxed);
	return memory;
}

void DomainHeap::unmap(void *memory, std::size_t bytes) noexcept
{
	HeapRegion::instance().give(memory, bytes);
	mapped_.fetch_sub(bytes, std::memory_order_relaxed);
}

bool DomainHeap::add_chunk_locked() noexcept
{
	const auto bytes = next_chunk_;
	auto *chunk = static_cast<Chunk *>(map(bytes));
	if (chunk == nullptr)
		return false;
	chunk->next = chunks_;
	chunk->size = bytes;
	chunks_ = chunk;
	cursor_ = reinterpret_cast<char *>(chunk) + granule;
	limit_ = reinterpret_cast<char *>(chunk) + bytes;
	if (next_chunk_ < last_chunk)
		next_chunk_ *= 2;
	return true;
}

void *DomainHeap::allocate_large(std::size_t bytes, std::size_t alignment)
{
	if (!valid_alignment(alignment))
		throw std::bad_alloc{};
	const auto offset = payload_offset(alignment);
	if (bytes > region_bytes - offset)
		throw std::bad_alloc{};
	const auto total = page_round(bytes + offset);
	auto *header = static_cast<Large *>(map(total));
	if (header == nullptr)
		throw std::bad_alloc{};
	header->size = total;
	header->previous = nullptr;
	os_unfair_lock_lock(&lock_);
	header->next = large_;
	if (large_ != nullptr)
		large_->previous = header;
	large_ = header;
	os_unfair_lock_unlock(&lock_);
	return reinterpret_cast<char *>(header) + offset;
}

void DomainHeap::deallocate_large(void *pointer,
	std::size_t alignment) noexcept
{
	auto *header = reinterpret_cast<Large *>(
		static_cast<char *>(pointer) - payload_offset(alignment));
	os_unfair_lock_lock(&lock_);
	if (header->previous != nullptr)
		header->previous->next = header->next;
	else
		large_ = header->next;
	if (header->next != nullptr)
		header->next->previous = header->previous;
	os_unfair_lock_unlock(&lock_);
	unmap(header, header->size);
}

void DomainHeap::release() noexcept
{
	os_unfair_lock_lock(&lock_);
	Chunk *chunks = chunks_;
	Large *large = large_;
	chunks_ = nullptr;
	large_ = nullptr;
	cursor_ = nullptr;
	limit_ = nullptr;
	next_chunk_ = first_chunk;
	for (auto &head : free_)
		head = nullptr;
	os_unfair_lock_unlock(&lock_);
	while (chunks != nullptr) {
		Chunk *next = chunks->next;
		unmap(chunks, chunks->size);
		chunks = next;
	}
	while (large != nullptr) {
		Large *next = large->next;
		unmap(large, large->size);
		large = next;
	}
}

LeasedHeap &LeasedHeap::engine() noexcept
{
	[[clang::no_destroy]] static LeasedHeap heap;
	return heap;
}

void *ScratchArena::map(std::size_t bytes)
{
	void *memory = HeapRegion::instance().take(bytes, vm_tag);
	if (memory == nullptr)
		throw std::bad_alloc{};
	return memory;
}

ScratchArena::~ScratchArena()
{
	reset();
	if (first_ != nullptr) {
		HeapRegion::instance().give(first_, kept_bytes);
		mapped_ -= kept_bytes;
	}
}

void ScratchArena::reset() noexcept
{
	while (extra_ != nullptr) {
		/* The header goes with the pages. */
		Extra *next = extra_->next;
		const auto size = extra_->size;
		HeapRegion::instance().give(extra_, size);
		mapped_ -= size;
		extra_ = next;
	}
	cursor_ = static_cast<char *>(first_);
	limit_ = first_ != nullptr ? cursor_ + kept_bytes : nullptr;
}

void *ScratchArena::allocate_slow(std::size_t bytes, std::size_t alignment)
{
	if (!valid_alignment(alignment))
		throw std::bad_alloc{};
	if (first_ == nullptr) {
		first_ = map(kept_bytes);
		mapped_ += kept_bytes;
		cursor_ = static_cast<char *>(first_);
		limit_ = cursor_ + kept_bytes;
		const auto position = (reinterpret_cast<std::uintptr_t>(cursor_) +
			alignment - 1) & ~(alignment - 1);
		const auto limit = reinterpret_cast<std::uintptr_t>(limit_);
		if (position <= limit && bytes <= limit - position) {
			cursor_ = reinterpret_cast<char *>(position + bytes);
			return reinterpret_cast<void *>(position);
		}
	}
	/* A chunk of its own; what is left of the current one waits for the
	 * reset. */
	if (bytes > region_bytes - alignment - large_header)
		throw std::bad_alloc{};
	std::size_t size = page_round(bytes + alignment + large_header);
	if (size < kept_bytes)
		size = kept_bytes;
	auto *extra = static_cast<Extra *>(map(size));
	mapped_ += size;
	extra->next = extra_;
	extra->size = size;
	extra_ = extra;
	const auto position = (reinterpret_cast<std::uintptr_t>(extra) +
		large_header + alignment - 1) & ~(alignment - 1);
	cursor_ = reinterpret_cast<char *>(position + bytes);
	limit_ = reinterpret_cast<char *>(extra) + size;
	return reinterpret_cast<void *>(position);
}

ScratchArena &thread_scratch_arena() noexcept
{
	thread_local ScratchArena arena;
	return arena;
}

} // namespace uml::memory
