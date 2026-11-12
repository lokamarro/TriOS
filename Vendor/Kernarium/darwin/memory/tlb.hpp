// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "memory/guest_address_space.hpp"

#include <array>
#include <cstddef>
#include <cstdint>

namespace uml::memory {

/* The guest runs 4 KiB pages inside the host's 16 KiB pages. */
static_assert(DARWIN_UML_GUEST_PAGE_SIZE == 4096,
	"the guest page size is 4 KiB");

enum class Access : std::uint32_t {
	read,
	write,
	execute,
};

[[nodiscard]] constexpr Access translated_access(
	MemoryAccess access) noexcept
{
	switch (access) {
	case MemoryAccess::read:
		return Access::read;
	case MemoryAccess::write:
		return Access::write;
	case MemoryAccess::execute:
		return Access::execute;
	}
	return Access::read;
}

[[nodiscard]] constexpr MemoryAccess guest_access(Access access) noexcept
{
	switch (access) {
	case Access::read:
		return MemoryAccess::read;
	case Access::write:
		return MemoryAccess::write;
	case Access::execute:
		return MemoryAccess::execute;
	}
	return MemoryAccess::read;
}

/* Keys are guest page numbers (address >> page shift), which lets a hot path
 * compare a shifted address without masking it first. No page number reaches
 * all ones, so that value marks an empty way. */
inline constexpr std::uint64_t tlb_empty_key = ~std::uint64_t{0};

struct alignas(32) TlbEntry final {
	std::uint64_t read_page{tlb_empty_key};
	std::uintptr_t read_data_minus_address{};
	std::uint64_t write_page{tlb_empty_key};
	std::uintptr_t write_data_minus_address{};
};

/* The table has 16,384 entries, indexed directly by the low bits of the guest
 * page number: a cache line holds two neighbouring pages' 32-byte entries. A
 * direct index never places pages n and n + 1 in one entry, which lets a
 * transfer key its probe by the page of its final byte and find a crossing as
 * a miss. */
inline constexpr std::size_t tlb_index_bits = 14U;
inline constexpr std::size_t tlb_entry_count = 1U << tlb_index_bits;
/* Entries a flush clears one by one before clearing the whole table is
 * cheaper. A vCPU switching address spaces flushes; after a short run in the
 * other space only a few entries hold anything. */
inline constexpr std::size_t tlb_dirty_capacity = 1024;
static_assert(tlb_entry_count <= 65536, "dirty entry indexes are 16 bits");

/*
 * This layout is shared by the gadget and JIT engines and their assembly
 * helpers. Keep it in the memory layer so either engine can consume the TLB
 * contract without depending on the other engine's implementation headers.
 */
struct alignas(32) Tlb final {
	std::array<TlbEntry, tlb_entry_count> entries{};
	std::uint64_t generation{};
	std::uint64_t changes{};
	std::uint64_t fault_address{};
	Access fault_access{Access::read};
	/* Cold fault cause; each executor clears it at the next block entry. */
	std::uint32_t fault_alignment{};
	/* Whether the address space owning the current entries ignores the top
	 * address byte (TBI). Set by the executor that fills the TLB
	 * from its guest state; consumers that never present tagged addresses
	 * leave it false. */
	bool tagged_addresses{};
	/* Whether the host completes a misaligned atomic, exclusive or
	 * ordered access inside one 16-byte block (FEAT_LSE2), which the guest
	 * then sees as USCAT. Set once by the executor that owns the TLB. */
	bool unaligned_atomics{};
	/* Entries filled since the last flush; a count above the capacity means
	 * the next flush clears the whole table. Not read by generated code. */
	std::uint32_t dirty_count{};
	std::array<std::uint16_t, tlb_dirty_capacity> dirty_entries{};
	/* The owning executor's static-MMU execution context, or null. A miss
	 * resolves through it while it is entered. Not read by generated code. */
	void *execution_context{};

	Tlb();
	void prepare(std::uint64_t next_generation, std::uint64_t next_changes);
	void discard(std::uint64_t address);
	[[nodiscard]] std::byte *translate(std::uint64_t address, Access access);
	/* Translate an address that may still carry a logical tag. The entry is
	 * keyed by the address as presented, so a later lookup with the same
	 * tag hits without stripping it; a fault reports the untagged address.
	 * Invalidation ignores the tag byte of every key, so tagged entries
	 * follow their page's mapping changes. */
	[[nodiscard]] std::byte *translate_tagged(
		std::uint64_t address, Access access);
	/* Translate the address of an exclusive load: a read, which faults like
	 * any load, that also fills the page's write entry when the page is
	 * writable now, so the store-exclusive that completes the pair finds it.
	 * A page that is not writable keeps its read entry, and the failed write
	 * lookup records no fault. */
	[[nodiscard]] std::byte *translate_exclusive_load(std::uint64_t address);

private:
	void flush();
	void invalidate(std::uint64_t address, std::uint64_t length);
	void reconcile_changes(
		std::uint64_t next_generation, std::uint64_t next_changes);
};

} // namespace uml::memory
