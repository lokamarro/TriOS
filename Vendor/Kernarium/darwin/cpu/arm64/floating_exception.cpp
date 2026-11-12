// SPDX-License-Identifier: GPL-2.0-or-later
#include "cpu/arm64/detail/floating_exception.hpp"

namespace uml::cpu::arm64::detail {

FloatingExceptionDecision decide_a64_floating_exception(
	std::uint32_t guest_fpcr, std::uint32_t original_fpsr,
	std::uint32_t isolated_fpsr, std::uint32_t instruction,
	const std::array<std::byte, 16> &source,
	const std::array<std::byte, 16> &destination) noexcept
{
	return decide_floating_exception(guest_fpcr, original_fpsr,
		isolated_fpsr, instruction, source, destination);
}

} // namespace uml::cpu::arm64::detail
