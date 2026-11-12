// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <cstdint>

namespace uml::cpu::arm64::detail {

[[nodiscard]] constexpr bool instruction_accesses_fp_state(
	std::uint32_t instruction) noexcept
{
	/* A64 assigns scalar floating-point and SIMD data processing encodings to
	 * op0=x111. SIMD loads/stores use the ordinary load/store classes with
	 * their V bit set. Include the corresponding system-register accesses.
	 *
	 * Reads and vector stores are intentionally classified as accesses too:
	 * production engines can materialize lazy state once per translated block
	 * without adding instruction-level checks to their runtime hot path.
	 */
	const bool data_processing =
		(instruction & 0x0e00'0000U) == 0x0e00'0000U;
	const bool memory_copy_set =
		(instruction & 0x3f20'0c00U) == 0x1900'0400U ||
		(instruction & 0x3f20'0c00U) == 0x1d00'0400U ||
		(instruction & 0x3fe0'0c00U) == 0x19c0'0400U ||
		(instruction & 0x3fe0'0c00U) == 0x1dc0'0400U;
	const bool vector_load_store =
		(instruction & 0x0a00'0000U) == 0x0800'0000U &&
		(instruction & 0x0400'0000U) != 0U && !memory_copy_set;
	const auto system = instruction & 0xffff'ffe0U;
	const bool fp_system_register =
		system == 0xd53b'4400U || system == 0xd51b'4400U ||
		system == 0xd53b'4420U || system == 0xd51b'4420U ||
		system == 0xd53b'4440U || system == 0xd51b'4440U;
	return data_processing || vector_load_store || fp_system_register;
}

} // namespace uml::cpu::arm64::detail
