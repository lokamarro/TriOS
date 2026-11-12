// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <array>
#include <cstdint>
#include <span>

namespace uml::cpu::arm64::gadgets {

inline constexpr std::uint16_t scalar_no_register = 0xffff;

struct ScalarContract final {
	std::array<std::uintptr_t, 8> entries;
	std::uint16_t destination;
	std::array<std::uint16_t, 3> sources;
	/* The family number each scalar_entry declaration records. It named
	 * a bucket of the removed dependency coverage and nothing reads it;
	 * the slot keeps the row at its fixed size. */
	std::uint16_t family;
	std::uint16_t operand_words;
	std::uint32_t reserved;
};
static_assert(sizeof(ScalarContract) == 80);

struct ScalarAccess final {
	const ScalarContract *contract{};
	std::uint16_t destination{scalar_no_register};
	std::array<std::uint16_t, 3> sources{
		scalar_no_register, scalar_no_register, scalar_no_register};
	[[nodiscard]] unsigned source_mask(std::uint16_t previous) const noexcept
	{
		if (previous == scalar_no_register) return 0;
		unsigned mask{};
		for (unsigned i = 0; i < sources.size(); ++i)
			if (sources[i] == previous) mask |= 1U << i;
		return mask;
	}
};

[[nodiscard]] std::span<const ScalarContract> scalar_contracts() noexcept;
[[nodiscard]] ScalarAccess scalar_access(std::span<const std::uintptr_t> words);

/* Vector contracts use the same immutable descriptor format, with bit 14
 * encoding a five-bit vector index relative to CpuState::vectors. Results
 * remain write-through and are live in native v2 only at declared edges. */
[[nodiscard]] std::span<const ScalarContract> vector_contracts() noexcept;
[[nodiscard]] ScalarAccess vector_access(std::span<const std::uintptr_t> words);

} // namespace uml::cpu::arm64::gadgets
