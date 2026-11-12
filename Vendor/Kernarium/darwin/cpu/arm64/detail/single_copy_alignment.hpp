// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <cstddef>
#include <cstdint>

namespace uml::cpu::arm64::detail {

/*
 * The alignment rule for the accesses the architecture makes single-copy
 * atomic: LSE atomics, exclusives, and the load-acquire/store-release forms.
 * Without FEAT_LSE2 any misaligned one is an alignment fault. With it, one
 * whose bytes all lie in one 16-byte aligned block completes single-copy
 * atomically and only one across a block boundary faults; Linux leaves
 * SCTLR_EL1.nAA clear, which gives the ordered forms the same rule. `size`
 * is the whole access, a pair included: a power of two up to 16.
 */
[[nodiscard]] constexpr bool single_copy_alignment_faults(
	std::uint64_t address, std::size_t size, bool lse2) noexcept
{
	if ((address & (size - 1U)) == 0U)
		return false;
	return !lse2 || (address & 15U) + size > 16U;
}

/*
 * Host acquire loads and release stores for a guest access the rule above
 * admits with FEAT_LSE2. The host must implement FEAT_LSE2 itself, whose LDAR
 * and STLR are single-copy atomic anywhere inside one 16-byte block, so a
 * misaligned host pointer stays atomic where std::atomic_ref would demand
 * natural alignment. `size` is 1, 2, 4 or 8.
 */
[[nodiscard]] inline std::uint64_t lse2_acquire_load(
	const void *pointer, std::size_t size) noexcept
{
	std::uint64_t value;
	switch (size) {
	case 1:
		__asm__ volatile("ldarb %w0, [%1]"
				 : "=r"(value) : "r"(pointer) : "memory");
		break;
	case 2:
		__asm__ volatile("ldarh %w0, [%1]"
				 : "=r"(value) : "r"(pointer) : "memory");
		break;
	case 4:
		__asm__ volatile("ldar %w0, [%1]"
				 : "=r"(value) : "r"(pointer) : "memory");
		break;
	default:
		__asm__ volatile("ldar %0, [%1]"
				 : "=r"(value) : "r"(pointer) : "memory");
		break;
	}
	return value;
}

/* One host CASAL of 2, 4 or 8 bytes under the same requirement; returns the
 * value observed in memory. */
[[nodiscard]] inline std::uint64_t lse2_compare_swap(void *pointer,
	std::size_t size, std::uint64_t expected,
	std::uint64_t desired) noexcept
{
	switch (size) {
	case 2:
		__asm__ volatile("casalh %w0, %w2, [%1]"
				 : "+r"(expected)
				 : "r"(pointer), "r"(desired) : "memory");
		return expected & 0xffffU;
	case 4:
		__asm__ volatile("casal %w0, %w2, [%1]"
				 : "+r"(expected)
				 : "r"(pointer), "r"(desired) : "memory");
		return expected & 0xffff'ffffU;
	default:
		__asm__ volatile("casal %0, %2, [%1]"
				 : "+r"(expected)
				 : "r"(pointer), "r"(desired) : "memory");
		return expected;
	}
}

inline void lse2_release_store(void *pointer, std::size_t size,
			       std::uint64_t value) noexcept
{
	switch (size) {
	case 1:
		__asm__ volatile("stlrb %w0, [%1]"
				 : : "r"(value), "r"(pointer) : "memory");
		break;
	case 2:
		__asm__ volatile("stlrh %w0, [%1]"
				 : : "r"(value), "r"(pointer) : "memory");
		break;
	case 4:
		__asm__ volatile("stlr %w0, [%1]"
				 : : "r"(value), "r"(pointer) : "memory");
		break;
	default:
		__asm__ volatile("stlr %0, [%1]"
				 : : "r"(value), "r"(pointer) : "memory");
		break;
	}
}

} // namespace uml::cpu::arm64::detail
