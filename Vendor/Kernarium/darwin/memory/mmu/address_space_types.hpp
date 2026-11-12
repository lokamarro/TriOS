// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "backing_slots.hpp"
#include "../domain_heap.hpp"
#include "fill_index.hpp"
#include "mapped_backing.hpp"
#include "uml_darwin_static_mmu.h"
#include "word_shared_mutex.hpp"
#include "page_index.hpp"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <span>
#include <unordered_map>
#include <vector>

namespace uml::memory {

/* The batch planning index is keyed by guest page address. */
using GuestPageHash = PageAddressHash;

struct Mapping final {
	std::shared_ptr<MappedBacking> backing;
	std::uint64_t offset;
	int fd;
	int protection;
	bool guest_code;
	bool guest_writable;
};

struct AddressSpaceLifetime final {
	std::atomic_bool retired{};
};

struct GenerationState final {
	GenerationState() noexcept = default;
	explicit GenerationState(std::shared_ptr<DomainHeap> domain_heap) noexcept
		: heap{std::move(domain_heap)}, fill_index{heap.get()}
	{
	}
	/* The domain's heap holds the fill index's nodes and the mapping history,
	 * so a retired domain gives that bookkeeping back to the host whole. It is
	 * declared first: it outlives everything allocated from it. */
	std::shared_ptr<DomainHeap> heap;
	mutable WordSharedMutex mappings_mutex;
	/* Every page the generation maps, as one word each, which the lock-free
	 * fill of an entered context reads, and the backings the words' slots
	 * name. */
	FillIndex fill_index;
	BackingSlots backings;
	/* The pages mapped. */
	std::size_t mapped_pages{};
	std::atomic_uint64_t mapping_changes{1};
	std::atomic_uint64_t code_changes{1};
	std::atomic_uint64_t invalidation_epoch{};
	std::atomic_uint64_t execution_interrupts{};
	std::atomic_uint64_t execution_gate{};
	std::atomic_bool retired{};
	std::shared_ptr<AddressSpaceLifetime> address_space_lifetime;
	struct MappingChange final {
		std::uint64_t sequence;
		std::uint64_t address;
		std::uint64_t length;
	};
	std::deque<MappingChange, DomainAllocator<MappingChange>> mapping_history{
		DomainAllocator<MappingChange>{heap.get()}};
	/* Which bytes each code change touched. A reader only a few changes
	 * behind retires exactly the translations covering them instead of every
	 * translation it holds.
	 *
	 * The records are a sequence-stamped ring rather than a locked queue: a
	 * writer is a guest code write inside translated execution, so it cannot
	 * afford a lock every vCPU contends for, and a reader that finds the stamp
	 * it wanted missing or moved simply retires everything, which is the
	 * answer it would have had without a history at all.
	 */
	struct CodeChangeRecord final {
		std::atomic_uint64_t sequence{};
		std::uint64_t address{};
		std::uint64_t length{};
	};
	std::array<CodeChangeRecord, 1024> code_history{};
};

/* The bridge's own type, so the history is written straight into a caller's
 * buffer of it. */
using CodeChangeRange = darwin_uml_static_code_change;

[[nodiscard]] inline GenerationState::CodeChangeRecord &code_change_record(
	GenerationState &state, std::uint64_t sequence) noexcept
{
	return state.code_history[sequence & (state.code_history.size() - 1U)];
}

static_assert((std::tuple_size_v<decltype(GenerationState::code_history)> &
	(std::tuple_size_v<decltype(GenerationState::code_history)> - 1U)) == 0,
	"the ring index masks the sequence");

/* Reserve the sequences first, then stamp each record last, so a reader either
 * sees the whole record or sees a stamp that is not the one it asked for. A
 * stale record cannot impersonate a live one: sequences never repeat, and the
 * ring index is the sequence itself. */
inline void publish_code_changes(GenerationState &state,
	std::span<const CodeChangeRange> ranges) noexcept
{
	if (ranges.empty())
		return;
	const auto first = state.code_changes.fetch_add(
		ranges.size(), std::memory_order_acq_rel) + 1;
	for (std::size_t index = 0; index < ranges.size(); ++index) {
		const auto sequence = first + index;
		auto &record = code_change_record(state, sequence);
		record.sequence.store(0, std::memory_order_relaxed);
		std::atomic_thread_fence(std::memory_order_release);
		record.address = ranges[index].address;
		record.length = ranges[index].length;
		record.sequence.store(sequence, std::memory_order_release);
	}
}

inline void publish_code_change(GenerationState &state,
	std::uint64_t address, std::uint64_t length) noexcept
{
	const CodeChangeRange range{address, length};
	publish_code_changes(state, {&range, 1});
}

/* A batch too wide to describe advances the counter and stamps nothing, which
 * tells every reader of that span to retire everything exactly once. */
inline void publish_untracked_code_changes(
	GenerationState &state, std::size_t count) noexcept
{
	if (count != 0)
		state.code_changes.fetch_add(count, std::memory_order_release);
}

/* The ranges changed after `sequence`, or -1 when the ring no longer describes
 * that whole span and the caller has to retire everything. */
[[nodiscard]] inline int code_changes_since(GenerationState &state,
	std::uint64_t sequence, CodeChangeRange *output, std::size_t capacity,
	std::uint64_t *current_sequence) noexcept
{
	const auto current = state.code_changes.load(std::memory_order_acquire);
	if (current_sequence != nullptr)
		*current_sequence = current;
	if (sequence >= current)
		return 0;
	if (current - sequence > capacity ||
	    current - sequence > state.code_history.size())
		return -1;
	std::size_t count = 0;
	for (auto wanted = sequence + 1; wanted <= current; ++wanted) {
		auto &record = code_change_record(state, wanted);
		if (record.sequence.load(std::memory_order_acquire) != wanted)
			return -1;
		const CodeChangeRange range{record.address, record.length};
		std::atomic_thread_fence(std::memory_order_acquire);
		if (record.sequence.load(std::memory_order_relaxed) != wanted)
			return -1;
		output[count++] = range;
	}
	return static_cast<int>(count);
}

struct MappingOperation final {
	bool map;
	std::uint64_t address;
	std::uint64_t length;
	Mapping mapping;
};

struct MappingBatch final {
	const void *address_space{};
	unsigned depth{};
	std::unordered_map<int, std::shared_ptr<MappedBacking>> backings;
	std::vector<MappingOperation> operations;
	std::uint64_t generation{};
	std::uint64_t invalidation_epoch{};
	std::shared_ptr<GenerationState> state;
};

/* A Linux page-table walker binds its thread to one generation for the walk.
 * The binding keeps the state alive and spares the batch a second lookup; it
 * does not exclude other walkers.  Walkers of one generation run together, and
 * the invalidation epoch a batch is published under is what orders them
 * against a change to the page tables they sampled. */
struct ActiveWalker final {
	const void *address_space{};
	std::uint64_t generation{};
	std::shared_ptr<GenerationState> state;
};

struct ActiveGeneration final {
	const void *address_space{};
	std::uint64_t generation{};
	std::shared_ptr<GenerationState> state;
};

struct GenerationReference final {
	std::shared_ptr<GenerationState> owner;
	GenerationState *state{};
};

struct ExecutionContext final {
	std::shared_ptr<GenerationState> state;
	std::uint64_t generation{};
	std::uint64_t execution_interrupts{};
	bool entered{};
};

} // namespace uml::memory
