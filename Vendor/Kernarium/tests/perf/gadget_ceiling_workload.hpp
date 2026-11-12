// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

#if defined(__APPLE__)
#define UML_GADGET_CEILING_ASM_NAME(value) __asm__(#value)
#else
#define UML_GADGET_CEILING_ASM_NAME(value)
#endif

/* Every workload shares one calling convention: the iteration count, a zeroed
 * 64 KiB page-aligned buffer, and two path counters written back. The return
 * value is the checksum the loop accumulated. */
using UmlGadgetCeilingNative = std::uint64_t (*)(
	std::uint64_t iterations, void *buffer, std::uint64_t *paths);

#define UML_GADGET_CEILING_WORKLOAD(name) \
	extern "C" std::uint64_t uml_gadget_ceiling_native_##name( \
		std::uint64_t iterations, void *buffer, std::uint64_t *paths) \
		UML_GADGET_CEILING_ASM_NAME(uml_gadget_ceiling_native_##name); \
	extern "C" const std::uint32_t uml_gadget_ceiling_##name##_begin[] \
		UML_GADGET_CEILING_ASM_NAME(uml_gadget_ceiling_##name##_begin); \
	extern "C" const std::uint32_t uml_gadget_ceiling_##name##_end[] \
		UML_GADGET_CEILING_ASM_NAME(uml_gadget_ceiling_##name##_end);

UML_GADGET_CEILING_WORKLOAD(add_immediate)
UML_GADGET_CEILING_WORKLOAD(scalar_mix)
UML_GADGET_CEILING_WORKLOAD(simd_mix)

#undef UML_GADGET_CEILING_WORKLOAD
#undef UML_GADGET_CEILING_ASM_NAME

inline constexpr std::size_t uml_gadget_ceiling_buffer_bytes = 65536;
inline constexpr std::size_t uml_gadget_ceiling_buffer_alignment = 16384;

/* What one run of a workload leaves behind: the checksum and the fall-through
 * counts of its data-dependent branches, which the dynamic instruction count
 * derives from. Native and ideal runs of the same count must agree exactly. */
struct UmlGadgetCeilingResult final {
	std::uint64_t checksum{};
	std::array<std::uint64_t, 2> paths{};

	[[nodiscard]] bool operator==(
		const UmlGadgetCeilingResult &) const noexcept = default;
};
