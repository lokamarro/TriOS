// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "jit_runtime_contract.hpp"

#include "cpu/arm64/cpu_state.hpp"
#include "cpu/arm64/memory.hpp"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <span>

/*
 * The JIT's software reservation state. A versioned table of host
 * reservation granules backs the guest exclusive monitor. The semantic slow
 * path performs each atomic, exclusive and ordered access as one host access
 * on the translated page (GuestMemory::atomic_pointer), serializes its
 * reservation checks and stores with exclusive_memory_mutex, and invalidates
 * every granule it stores to. Tracked native store blocks invalidate the
 * granules they wrote before their publication ends.
 *
 * The monitor ends at a store-exclusive, a CLREX, an exception and a store to
 * the reserved granule, whoever makes it. A store of the reserving vCPU's own
 * to another granule leaves it standing, as on hardware, so a register spill
 * between a pair does not fail the pair; every store here therefore names the
 * vCPU that made it.
 */

namespace uml::cpu::arm64::jit::semantic {

class ExclusiveMemoryMutex final {
public:
	void lock() noexcept
	{
		while (locked_.test_and_set(std::memory_order_acquire)) {
			while (locked_.test(std::memory_order_relaxed))
				__asm__ volatile("yield");
		}
	}

	void unlock() noexcept
	{
		locked_.clear(std::memory_order_release);
	}

private:
	std::atomic_flag locked_ = ATOMIC_FLAG_INIT;
};

extern ExclusiveMemoryMutex exclusive_memory_mutex;

enum class AtomicStoreResult {
	stored,
	reservation_mismatch,
	fault,
};

/* What a store-exclusive completes against: the host granule its
 * load-exclusive reserved and the value it read, in address order (a pair of
 * 4-byte elements is one 8-byte `first`; a pair of 8-byte elements is `first`
 * and `second`). */
struct MonitoredValue final {
	std::uintptr_t key;
	std::uint64_t first;
	std::uint64_t second;
};

/* Invalidates reservation granule `key` for a store of `writer`'s. Another
 * vCPU's reservation of the granule, or of any granule that shares its slot,
 * fails. `writer`'s own reservation fails only if the store is to its
 * granule, or another vCPU stored to its slot since it loaded. */
void reservation_store(CpuState &writer, std::uintptr_t key) noexcept;
[[nodiscard]] std::uint64_t reservation_version(std::uintptr_t key) noexcept;

/* The version slot the reservation granule of host location `host` hashes to.
 * Granules that share a slot share one version. */
[[nodiscard]] std::size_t reservation_slot_index(const std::byte *host) noexcept;

[[nodiscard]] bool atomic_load_value(GuestMemory &memory,
	std::uint64_t address, std::size_t size, std::memory_order order,
	std::uint64_t &value, std::uintptr_t &key);

[[nodiscard]] AtomicStoreResult atomic_store_value(GuestMemory &memory,
	std::uint64_t address, std::size_t size, std::uint64_t value,
	std::memory_order order, std::uintptr_t &key);

/* The store of a store-exclusive: one compare-and-swap of the monitored value
 * for `value`. It stores only if the location is still the reserved granule
 * and still holds what the load-exclusive read, so a write that does not take
 * the exclusive gate (the kernel's futex and SWP emulation, DMA) fails the
 * store-exclusive instead of being overwritten. The check is by value: a write
 * that restored the value is not seen. */
[[nodiscard]] AtomicStoreResult atomic_store_exclusive_value(
	GuestMemory &memory, std::uint64_t address, std::size_t size,
	const MonitoredValue &monitored, std::uint64_t value,
	std::memory_order order, std::uintptr_t &key);

[[nodiscard]] bool atomic_compare_swap_value(GuestMemory &memory,
	std::uint64_t address, std::size_t size, std::uint64_t compare,
	std::uint64_t replacement, bool acquire, bool release,
	std::uint64_t &observed, std::uintptr_t &key);

[[nodiscard]] bool atomic_compare_swap_pair_value(GuestMemory &memory,
	std::uint64_t address, std::size_t element_size,
	std::uint64_t compare_first, std::uint64_t compare_second,
	std::uint64_t replacement_first, std::uint64_t replacement_second,
	bool acquire, bool release,
	std::uint64_t &observed_first, std::uint64_t &observed_second,
	std::uintptr_t &key);

[[nodiscard]] bool atomic_rmw_value(GuestMemory &memory,
	std::uint64_t address, std::size_t size, unsigned operation,
	std::uint64_t operand, bool acquire, bool release,
	std::uint64_t &observed, std::uintptr_t &key);

[[nodiscard]] bool atomic_load_pair_value(GuestMemory &memory,
	std::uint64_t address, std::size_t element_size, std::memory_order order,
	std::uint64_t &first, std::uint64_t &second, std::uintptr_t &key);

/* The same for a pair of elements. */
[[nodiscard]] AtomicStoreResult atomic_store_exclusive_pair_value(
	GuestMemory &memory, std::uint64_t address, std::size_t element_size,
	const MonitoredValue &monitored, std::uint64_t first,
	std::uint64_t second, std::memory_order order, std::uintptr_t &key);

[[nodiscard]] bool ordinary_store(CpuState &writer, GuestMemory &memory,
	std::uint64_t address, std::span<const std::byte> bytes);

[[nodiscard]] bool simd_structure_address(CpuState &cpu,
	GuestMemory &memory, std::uint64_t tagged_address,
	std::size_t transfer_size, std::size_t element_size,
	MemoryAccess access, std::uint64_t &address, std::uint64_t &fault_address);

/* Invalidates every reservation granule that a tracked native store block of
 * `writer`'s recorded. Each range names the translated host backing address,
 * so guest aliases of one location share its reservation identity. */
void invalidate_store_ranges(CpuState &writer,
	std::span<const JitStoreRange> ranges) noexcept;

} // namespace uml::cpu::arm64::jit::semantic
