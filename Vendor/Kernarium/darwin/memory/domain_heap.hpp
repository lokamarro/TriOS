// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

/*
 * Heaps that a unit of the engine maps for itself and gives back whole.
 *
 * The default malloc zone returns what a restarted guest freed on its own
 * schedule: it keeps the chunks a boot emptied and moves their pages between
 * "dirty" and "reclaimable" in the footprint at moments the process cannot
 * choose, so the memory a guest's metadata took is not reliably gone when the
 * guest is. Memory that never enters the zone is returned at once and
 * deterministically. A DomainHeap takes its memory from one process-wide
 * reservation (HeapRegion) and gives all of it back when the unit it serves
 * ends; a ScratchArena holds the temporaries of one operation on one thread.
 *
 * Every mapping carries a VM tag (heap 241, scratch 242), so vmmap and
 * footprint name the memory.
 *
 * None of these may be used from a signal handler that can interrupt the same
 * thread inside them, as with malloc.
 */
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <new>
#include <type_traits>

#include <os/lock.h>

namespace uml::memory {

/*
 * The process's one reservation, made on first use and never unmapped.
 *
 * A block is committed by mapping fresh pages over its reserved range and is
 * given back by reserving the range again, so its pages leave the footprint at
 * once; the range is then free for the next block, lowest address first. The
 * heaps therefore neither squat in a hole that someone else unmapped nor
 * punch holes for the next mmap(NULL) to prefer, which a heap that mapped each
 * chunk with mmap(NULL) and unmapped it did to a test that reserves, unmaps
 * and then maps at fixed addresses.
 *
 * Exhaustion is reported as a null block, and callers throw std::bad_alloc.
 */
class HeapRegion final {
public:
	[[nodiscard]] static HeapRegion &instance() noexcept;

	/* Address space reserved, in bytes. */
	[[nodiscard]] static std::size_t reservation() noexcept;

	/* `bytes` is a multiple of the host page. Null when the region has no
	 * such range or the reservation could not be made. */
	[[nodiscard]] void *take(std::size_t bytes, int tag) noexcept;

	/* Returns a range `take` returned, with the same size. */
	void give(void *memory, std::size_t bytes) noexcept;

	/* Bytes committed now, across every heap and arena of the process. */
	[[nodiscard]] std::size_t committed() const noexcept
	{
		return committed_.load(std::memory_order_relaxed);
	}

	/* The reservation's first byte, or 0 before the first take. */
	[[nodiscard]] std::uintptr_t base() const noexcept
	{
		return base_.load(std::memory_order_acquire);
	}

	[[nodiscard]] bool contains(const void *pointer) const noexcept;

private:
	HeapRegion() noexcept = default;
	HeapRegion(const HeapRegion &) = delete;
	HeapRegion &operator=(const HeapRegion &) = delete;

	struct Range final {
		std::uintptr_t start;
		std::size_t size;
	};

	/* Sorted by address, adjacent ranges merged. A range at the tail lowers
	 * the tail instead. A full table drops the range: its address space is
	 * lost, its memory is not. */
	void give_range(std::uintptr_t start, std::size_t size) noexcept;
	[[nodiscard]] bool reserve_locked() noexcept;

	static constexpr std::size_t capacity = 1024;

	os_unfair_lock lock_ = OS_UNFAIR_LOCK_INIT;
	std::atomic<std::uintptr_t> base_{};
	std::uintptr_t tail_{};
	std::size_t free_count_{};
	Range free_[capacity]{};
	std::atomic<std::size_t> committed_{};
};

/*
 * A heap for the objects of one unit (one memory domain, one executor), which
 * gives every byte back in release().
 *
 * Blocks of up to 8 KiB come from size classes carved out of chunks of
 * 256 KiB that double to 4 MiB, and freed blocks wait on per-class lists for
 * the heap's next allocation. Larger blocks are mappings of their own.
 * allocate() and deallocate() are thread safe under one lock per heap. Heaps
 * whose owners already serialize their writers take it uncontended, and a
 * heap that several writers share takes it for its few allocations.
 *
 * release() is for a heap with no live block and no concurrent caller; the
 * heap is then as new. The destructor releases.
 */
class DomainHeap final {
public:
	/* The tag of the mappings, VM_MEMORY_APPLICATION_SPECIFIC_1 + 1. */
	static constexpr int vm_tag = 241;

	DomainHeap() noexcept;
	~DomainHeap();
	DomainHeap(const DomainHeap &) = delete;
	DomainHeap &operator=(const DomainHeap &) = delete;

	/* `alignment` is a power of two of at most the host page. Throws
	 * std::bad_alloc when the process's region has no room. */
	[[nodiscard]] void *allocate(std::size_t bytes, std::size_t alignment)
	{
		if (bytes == 0)
			bytes = 1;
		if (alignment <= granule && bytes <= medium_limit) {
			const auto index = class_index(bytes);
			os_unfair_lock_lock(&lock_);
			void *block = take_locked(index);
			os_unfair_lock_unlock(&lock_);
			if (block == nullptr)
				throw std::bad_alloc{};
			return block;
		}
		return allocate_large(bytes, alignment);
	}

	/* `bytes` and `alignment` are those of the allocation. */
	void deallocate(void *pointer, std::size_t bytes,
		std::size_t alignment) noexcept
	{
		if (pointer == nullptr)
			return;
		if (bytes == 0)
			bytes = 1;
		if (alignment <= granule && bytes <= medium_limit) {
			const auto index = class_index(bytes);
			auto *block = static_cast<FreeBlock *>(pointer);
			os_unfair_lock_lock(&lock_);
			block->next = free_[index];
			free_[index] = block;
			os_unfair_lock_unlock(&lock_);
			return;
		}
		deallocate_large(pointer, alignment);
	}

	/* Gives every mapping back. */
	void release() noexcept;

	/* Bytes this heap has mapped. */
	[[nodiscard]] std::size_t mapped_bytes() const noexcept
	{
		return mapped_.load(std::memory_order_relaxed);
	}

	/* Heaps alive in the process. */
	[[nodiscard]] static std::size_t live_heaps() noexcept;

private:
	static constexpr std::size_t granule = 16;
	static constexpr std::size_t small_limit = 1024;
	static constexpr std::size_t medium_step = 256;
	static constexpr std::size_t medium_limit = 8192;
	static constexpr std::size_t small_classes = small_limit / granule;
	static constexpr std::size_t classes =
		small_classes + (medium_limit - small_limit) / medium_step;

	struct FreeBlock final {
		FreeBlock *next;
	};
	struct Chunk final {
		Chunk *next;
		std::size_t size;
	};
	struct Large final {
		Large *previous;
		Large *next;
		std::size_t size;
	};
	static_assert(sizeof(Chunk) <= granule);

	[[nodiscard]] static std::size_t class_index(std::size_t bytes) noexcept
	{
		if (bytes <= small_limit)
			return (bytes + granule - 1) / granule - 1;
		return small_classes +
			(bytes - small_limit + medium_step - 1) / medium_step - 1;
	}

	[[nodiscard]] static std::size_t class_size(std::size_t index) noexcept
	{
		if (index < small_classes)
			return (index + 1) * granule;
		return small_limit + (index - small_classes + 1) * medium_step;
	}

	[[nodiscard]] void *take_locked(std::size_t index) noexcept
	{
		if (FreeBlock *block = free_[index]; block != nullptr) {
			free_[index] = block->next;
			return block;
		}
		const auto size = class_size(index);
		if (static_cast<std::size_t>(limit_ - cursor_) < size &&
		    !add_chunk_locked())
			return nullptr;
		void *block = cursor_;
		cursor_ += size;
		return block;
	}

	[[nodiscard]] bool add_chunk_locked() noexcept;
	[[nodiscard]] void *allocate_large(std::size_t bytes,
		std::size_t alignment);
	void deallocate_large(void *pointer, std::size_t alignment) noexcept;
	[[nodiscard]] void *map(std::size_t bytes) noexcept;
	void unmap(void *memory, std::size_t bytes) noexcept;

	os_unfair_lock lock_ = OS_UNFAIR_LOCK_INIT;
	FreeBlock *free_[classes]{};
	char *cursor_{};
	char *limit_{};
	Chunk *chunks_{};
	Large *large_{};
	std::size_t next_chunk_;
	std::atomic<std::size_t> mapped_{};
};

/*
 * A standard allocator over one heap. Without a heap, a container built
 * outside any unit (a unit test) uses the global allocator, and equal
 * allocators compare by heap. The allocator propagates with its container, so
 * a moved or swapped container keeps the heap its nodes came from.
 */
template <typename T>
class DomainAllocator final {
public:
	using value_type = T;
	using propagate_on_container_copy_assignment = std::true_type;
	using propagate_on_container_move_assignment = std::true_type;
	using propagate_on_container_swap = std::true_type;
	using is_always_equal = std::false_type;

	DomainAllocator() noexcept = default;
	explicit DomainAllocator(DomainHeap *heap) noexcept : heap_{heap} {}
	template <typename U>
	DomainAllocator(const DomainAllocator<U> &other) noexcept
		: heap_{other.heap()}
	{
	}

	[[nodiscard]] T *allocate(std::size_t count)
	{
		if (heap_ == nullptr)
			return std::allocator<T>{}.allocate(count);
		if (count > static_cast<std::size_t>(-1) / sizeof(T))
			throw std::bad_alloc{};
		return static_cast<T *>(
			heap_->allocate(count * sizeof(T), alignof(T)));
	}

	void deallocate(T *pointer, std::size_t count) noexcept
	{
		if (heap_ == nullptr) {
			std::allocator<T>{}.deallocate(pointer, count);
			return;
		}
		heap_->deallocate(pointer, count * sizeof(T), alignof(T));
	}

	[[nodiscard]] DomainHeap *heap() const noexcept { return heap_; }

	template <typename U>
	[[nodiscard]] bool operator==(
		const DomainAllocator<U> &other) const noexcept
	{
		return heap_ == other.heap();
	}

private:
	DomainHeap *heap_{};
};

/*
 * The temporaries of one operation on one thread: a bump arena that never
 * frees a block and resets when the outermost ScratchScope ends. It keeps one
 * 64 KiB chunk for the thread's life, so an operation that fits in it
 * allocates from the system nothing at all once the thread has run one;
 * anything beyond comes from the region and goes back at the reset. The
 * kept chunk goes back when the thread ends.
 */
class ScratchArena final {
public:
	/* The tag of the mappings, VM_MEMORY_APPLICATION_SPECIFIC_1 + 2. */
	static constexpr int vm_tag = 242;
	static constexpr std::size_t kept_bytes = std::size_t{64} << 10;

	ScratchArena() noexcept = default;
	~ScratchArena();
	ScratchArena(const ScratchArena &) = delete;
	ScratchArena &operator=(const ScratchArena &) = delete;

	/* `alignment` is a power of two of at most the host page. Throws
	 * std::bad_alloc when the process's region has no room. */
	[[nodiscard]] void *allocate(std::size_t bytes, std::size_t alignment)
	{
		if (alignment < granule)
			alignment = granule;
		auto position = (reinterpret_cast<std::uintptr_t>(cursor_) +
			alignment - 1) & ~(alignment - 1);
		const auto limit = reinterpret_cast<std::uintptr_t>(limit_);
		if (cursor_ == nullptr || position > limit ||
		    bytes > limit - position)
			return allocate_slow(bytes, alignment);
		cursor_ = reinterpret_cast<char *>(position + bytes);
		return reinterpret_cast<void *>(position);
	}

	/* Rewinds to the kept chunk and gives every other chunk back. */
	void reset() noexcept;

	/* Bytes this arena has mapped, the kept chunk included. */
	[[nodiscard]] std::size_t mapped_bytes() const noexcept
	{
		return mapped_;
	}

	unsigned depth{};

private:
	static constexpr std::size_t granule = 16;

	struct Extra final {
		Extra *next;
		std::size_t size;
	};

	[[nodiscard]] void *allocate_slow(std::size_t bytes,
		std::size_t alignment);
	[[nodiscard]] static void *map(std::size_t bytes);

	void *first_{};
	char *cursor_{};
	char *limit_{};
	Extra *extra_{};
	std::size_t mapped_{};
};

/* The calling thread's arena. */
[[nodiscard]] ScratchArena &thread_scratch_arena() noexcept;

/* Resets the thread's arena when the outermost scope ends. A scope may nest. */
class ScratchScope final {
public:
	ScratchScope() noexcept : arena_{thread_scratch_arena()}
	{
		++arena_.depth;
	}
	~ScratchScope()
	{
		if (--arena_.depth == 0)
			arena_.reset();
	}
	ScratchScope(const ScratchScope &) = delete;
	ScratchScope &operator=(const ScratchScope &) = delete;

	[[nodiscard]] ScratchArena &arena() const noexcept { return arena_; }

private:
	ScratchArena &arena_;
};

/* A standard allocator over a scratch arena; deallocate does nothing, so a
 * container that grows leaves its old storage to the reset. */
template <typename T>
class ScratchAllocator final {
public:
	using value_type = T;
	using is_always_equal = std::false_type;

	explicit ScratchAllocator(ScratchArena &arena) noexcept : arena_{&arena} {}
	template <typename U>
	ScratchAllocator(const ScratchAllocator<U> &other) noexcept
		: arena_{other.arena()}
	{
	}

	[[nodiscard]] T *allocate(std::size_t count)
	{
		if (count > static_cast<std::size_t>(-1) / sizeof(T))
			throw std::bad_alloc{};
		return static_cast<T *>(
			arena_->allocate(count * sizeof(T), alignof(T)));
	}
	void deallocate(T *, std::size_t) noexcept {}

	[[nodiscard]] ScratchArena *arena() const noexcept { return arena_; }

	template <typename U>
	[[nodiscard]] bool operator==(
		const ScratchAllocator<U> &other) const noexcept
	{
		return arena_ == other.arena();
	}

private:
	ScratchArena *arena_;
};

/*
 * One heap that many owners share and that goes back when the last of them
 * ends. The gadget caches' record tables are such owners: an address space
 * has sixteen of them and a boot has hundreds of address spaces, so a heap
 * each would keep a first chunk each. Allocation there is rare, a chunk per
 * doubling and an index per growth, so the heap's lock is not contended.
 *
 * Every owner holds a HeapLease for as long as it holds a block of the heap.
 * Counting a lease and releasing the heap are one critical section: a lease
 * taken while the last one ends either comes first and keeps the heap, or
 * comes after the release and allocates from the heap as new.
 */
class LeasedHeap final {
public:
	/* The heap the engine's gadget caches share. */
	[[nodiscard]] static LeasedHeap &engine() noexcept;

	[[nodiscard]] DomainHeap &heap() noexcept { return heap_; }

	void acquire() noexcept
	{
		os_unfair_lock_lock(&lock_);
		++leases_;
		os_unfair_lock_unlock(&lock_);
	}

	void drop() noexcept
	{
		os_unfair_lock_lock(&lock_);
		if (--leases_ == 0)
			heap_.release();
		os_unfair_lock_unlock(&lock_);
	}

	/* Leases held now. */
	[[nodiscard]] std::size_t leases() const noexcept
	{
		os_unfair_lock_lock(&lock_);
		const auto count = leases_;
		os_unfair_lock_unlock(&lock_);
		return count;
	}

private:
	DomainHeap heap_;
	mutable os_unfair_lock lock_ = OS_UNFAIR_LOCK_INIT;
	std::size_t leases_{};
};

/* A hold on the engine's leased heap; a copy holds one of its own. */
class HeapLease final {
public:
	HeapLease() noexcept { LeasedHeap::engine().acquire(); }
	HeapLease(const HeapLease &) noexcept : HeapLease{} {}
	HeapLease &operator=(const HeapLease &) noexcept { return *this; }
	~HeapLease() { LeasedHeap::engine().drop(); }

	[[nodiscard]] static DomainHeap &heap() noexcept
	{
		return LeasedHeap::engine().heap();
	}
};

/* An array of trivial elements from the leased heap. Its owner holds a
 * HeapLease that outlives it. */
template <typename T>
struct LeasedArrayDelete final {
	std::size_t count{};
	void operator()(T *pointer) const noexcept
	{
		HeapLease::heap().deallocate(
			pointer, count * sizeof(T), alignof(T));
	}
};

template <typename T>
using LeasedArray = std::unique_ptr<T[], LeasedArrayDelete<T>>;

/* Zero-filled when `zero`, as make_unique<T[]> is, and left as it was
 * otherwise, as make_unique_for_overwrite leaves it. Throws std::bad_alloc. */
template <typename T>
[[nodiscard]] LeasedArray<T> make_leased_array(std::size_t count, bool zero)
{
	static_assert(std::is_trivially_default_constructible_v<T> &&
		std::is_trivially_destructible_v<T>);
	if (count > static_cast<std::size_t>(-1) / sizeof(T))
		throw std::bad_alloc{};
	void *raw = HeapLease::heap().allocate(count * sizeof(T), alignof(T));
	if (zero)
		std::memset(raw, 0, count * sizeof(T));
	return LeasedArray<T>{
		static_cast<T *>(raw), LeasedArrayDelete<T>{count}};
}

} // namespace uml::memory
