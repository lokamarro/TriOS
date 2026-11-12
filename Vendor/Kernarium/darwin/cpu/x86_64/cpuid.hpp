/* SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once

#include "cpu/guest_identity.hpp"

#include <cstddef>
#include <cstdint>

namespace uml::cpu::x86_64 {

/* The values the CPUID gadget returns, in a form the rest of the host can
 * read. A guest has two ways to ask what this CPU is: it can execute CPUID,
 * or it can read what the kernel wrote into /proc/cpuinfo, and those two must
 * agree. Only the gadget can answer the first, so this table exists to answer
 * the second, and `cpuid_table_matches_the_gadget` in the
 * `x86-64-gadget-bootstrap` test executes the gadget for every leaf below and
 * fails if the two ever disagree.
 *
 * The same contract governs arm64's identification registers; see the comment
 * on `emulated_id_register_value` in cpu/arm64/id_registers.hpp. A capability
 * is advertised only where an engine has committed to executing it, and every
 * bit set here is covered by the instruction corpus.
 */
struct CpuidResult final {
	std::uint32_t eax;
	std::uint32_t ebx;
	std::uint32_t ecx;
	std::uint32_t edx;
};

/* Leaves the gadget answers. Anything else reads as zero, which is what CPUID
 * reports for an unimplemented leaf below the maximum. */
inline constexpr std::uint32_t cpuid_maximum_basic_leaf = 13U;
inline constexpr std::uint32_t cpuid_maximum_extended_leaf = 0x8000'0008U;

namespace detail {

/* One register of the CPUID brand string, packed from `guest_cpu_model_name`.
 * The architecture spells the brand as plain little-endian bytes, so byte
 * `offset` is the low byte of its register and the tail past the name reads as
 * the NUL padding CPUID requires. Deriving the words rather than transcribing
 * them keeps the name in exactly one place.
 */
[[nodiscard]] constexpr std::uint32_t brand_word(std::size_t offset) noexcept
{
	std::uint32_t word = 0U;
	for (std::size_t index = 0U; index < 4U; ++index) {
		const std::size_t at = offset + index;
		if (at >= guest_cpu_model_name.size())
			break;
		const auto byte = static_cast<unsigned char>(
			guest_cpu_model_name[at]);
		word |= static_cast<std::uint32_t>(byte) << (8U * index);
	}
	return word;
}

} // namespace detail

[[nodiscard]] constexpr CpuidResult cpuid(
	std::uint32_t leaf, std::uint32_t subleaf) noexcept
{
	switch (leaf) {
	case 0U:
		/* "GenuineIntel", spelled EBX, EDX, ECX as the architecture
		 * orders it. */
		return {cpuid_maximum_basic_leaf, 0x756e'6547U, 0x6c65'746eU,
			0x4965'6e69U};
	case 1U:
		/* Family 6, model 0x3c, stepping 3. The feature words carry
		 * SSE3 through SSE4.2, AES, PCLMULQDQ, CMPXCHG16B, MOVBE,
		 * POPCNT, XSAVE, AVX, F16C, FMA and RDRAND. */
		return {0x0003'06c3U, 0x0000'0800U, 0x7ed8'3203U,
			0x0788'8111U};
	case 7U:
		if (subleaf == 1U)
			/* AVX-VNNI, the VEX forms of the dot products VPDPBUSD,
			 * VPDPBUSDS, VPDPWSSD and VPDPWSSDS. */
			return {0x0000'0010U, 0U, 0U, 0U};
		if (subleaf != 0U)
			return {};
		/* EAX is the highest subleaf, 1 for AVX-VNNI's. FSGSBASE,
		 * BMI1, AVX2, BMI2, ERMS, RDSEED, ADX, CLFLUSHOPT, CLWB, SHA,
		 * RDPID and FSRM. RTM/HLE and MPX stay absent. */
		return {1U, 0x218c'0329U, 0x0040'0000U, 0x0000'0010U};
	case 13U:
		if (subleaf == 0U)
			/* XCR0 enables x87, SSE and AVX; the standard image is
			 * 832 bytes for both the enabled and supported sets. */
			return {7U, 832U, 832U, 0U};
		if (subleaf == 2U)
			/* AVX contributes 256 bytes of YMM_Hi128 at 576. */
			return {256U, 576U, 0U, 0U};
		return {};
	case 0x8000'0000U:
		return {cpuid_maximum_extended_leaf, 0U, 0U, 0U};
	case 0x8000'0001U:
		/* LZCNT, and SYSCALL, NX and long mode. */
		return {0U, 0U, 0x0000'0020U, 0x2010'0800U};
	case 0x8000'0002U:
	case 0x8000'0003U:
	case 0x8000'0004U: {
		/* Sixteen brand bytes per leaf, in EAX, EBX, ECX, EDX order. */
		const std::size_t offset = (leaf - 0x8000'0002U) * 16U;
		return {detail::brand_word(offset),
			detail::brand_word(offset + 4U),
			detail::brand_word(offset + 8U),
			detail::brand_word(offset + 12U)};
	}
	default:
		return {};
	}
}

} // namespace uml::cpu::x86_64
