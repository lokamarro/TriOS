// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "cpu/x86_64/layout/assembly_offsets.h"

#include <cstddef>
#include <cstdint>

namespace uml::cpu::x86_64 {

/* One entry of an executor's software TLB. The code-page registry retires
 * participants' write translations in this layout. */
struct alignas(32) NativeTlbEntry final {
	std::uint64_t read_page{1};
	std::uintptr_t read_data_minus_address{};
	std::uint64_t write_page{1};
	std::uintptr_t write_data_minus_address{};
};

/* The table is direct-mapped. The index below is also written out in the
 * gadget engine's assembly (`translate` and `translate_plain` in
 * gadgets/engine/common.inc, and the baked entries in
 * gadgets/engine/generate_baked.py), so a change to this count or to the fold
 * changes all three. */
inline constexpr std::size_t native_tlb_entry_count = 1U << 15U;

/* The executor retires a write translation when a page gains cached code, so
 * it needs the same index the miss handler fills.
 */
[[nodiscard]] inline constexpr std::size_t native_tlb_index(
	std::uint64_t address) noexcept
{
	const auto page = address >> 12U;
	return static_cast<std::size_t>(
		(page ^ (page >> 15U)) & (native_tlb_entry_count - 1U));
}

static_assert(sizeof(NativeTlbEntry) == 32);
static_assert(offsetof(NativeTlbEntry, read_page) ==
	UML_X86_64_TLB_READ_PAGE);
static_assert(offsetof(NativeTlbEntry, read_data_minus_address) ==
	UML_X86_64_TLB_READ_DATA_MINUS_ADDRESS);
static_assert(offsetof(NativeTlbEntry, write_page) ==
	UML_X86_64_TLB_WRITE_PAGE);
static_assert(offsetof(NativeTlbEntry, write_data_minus_address) ==
	UML_X86_64_TLB_WRITE_DATA_MINUS_ADDRESS);

} // namespace uml::cpu::x86_64
