// SPDX-License-Identifier: GPL-2.0-or-later
#include "software_helpers.hpp"

#include "../detail/floating_round.hpp"
#include "../detail/instruction_primitives.hpp"
#include "../detail/integer_multiply.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

namespace {

using uml::cpu::arm64::CpuState;

extern "C" [[gnu::noinline]] void uml_arm64_gadget_access_fp_state(
	CpuState *cpu) noexcept
{
	cpu->access_fp_state();
}

extern "C" [[gnu::noinline]] std::uint64_t
uml_arm64_simd_rdm_software(CpuState *cpu,
	std::uint32_t instruction) noexcept
{
	/* The builder validates this encoding before selecting the cold software
	 * helper. Keep a defensive result for corrupt/stale program metadata: the
	 * assembly wrapper turns it into the same precise guest illegal event as a
	 * translation-time rejection instead of terminating the host process. */
	return uml::cpu::arm64::detail::
		execute_saturating_rounding_multiply_accumulate(*cpu, instruction);
}

} // namespace

extern "C" [[gnu::noinline]] void uml_arm64_floating_round_sized_software(
	uml::cpu::arm64::CpuState *cpu, std::uintptr_t operand) noexcept
{
	const unsigned destination = operand & 31U;
	const unsigned source_register = (operand >> 8) & 31U;
	const unsigned width = (operand & (std::uintptr_t{1} << 16)) != 0U ?
		64U : 32U;
	const unsigned integer_bits =
		(operand & (std::uintptr_t{1} << 17)) != 0U ? 64U : 32U;
	const bool current = (operand & (std::uintptr_t{1} << 18)) != 0U;
	const bool scalar = (operand & (std::uintptr_t{1} << 19)) != 0U;
	const unsigned vector_bytes = scalar ? width / 8U :
		(operand & (std::uintptr_t{1} << 20)) != 0U ? 16U : 8U;
	const auto source = cpu->vectors[source_register].bytes;
	std::array<std::byte, 16> result{};
	const unsigned lane_bytes = width / 8U;
	const unsigned lanes = scalar ? 1U : vector_bytes / lane_bytes;
	for (unsigned lane = 0; lane < lanes; ++lane) {
		const auto raw = uml::cpu::arm64::detail::decode_little_endian(
			std::span{source}.subspan(lane * lane_bytes, lane_bytes));
		const auto rounded = width == 32U ?
			uml::cpu::arm64::detail::round_floating_sized<float>(
				raw, integer_bits, current) :
			uml::cpu::arm64::detail::round_floating_sized<double>(
				raw, integer_bits, current);
		uml::cpu::arm64::detail::encode_little_endian(rounded.bits,
			std::span{result}.subspan(lane * lane_bytes, lane_bytes));
		cpu->fpsr |= rounded.exceptions;
	}
	cpu->vectors[destination].bytes = result;
}
