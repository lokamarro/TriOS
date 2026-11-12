// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "cpu/arm64/cpu_state.hpp"

#include <cinttypes>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace uml::emulator::tests {

/* The first architectural field in which `actual` differs from `expected`,
 * described with both values where one value names it, or nothing when the
 * states agree. The instruction corpus and the focused engine harness both
 * compare through this, so a CpuState field added here is checked by both. */
[[nodiscard]] inline std::optional<std::string> first_architecture_difference(
	const ::uml::cpu::arm64::CpuState &expected,
	const ::uml::cpu::arm64::CpuState &actual)
{
	const auto hex = [](std::uint64_t value) {
		char text[24];
		std::snprintf(text, sizeof(text), "%#" PRIx64, value);
		return std::string{text};
	};
	const auto values = [&](std::string_view field, std::uint64_t want,
				std::uint64_t have) {
		return std::string{field} + " expected " + hex(want) +
			" actual " + hex(have);
	};

	for (std::size_t index = 0; index < expected.registers.size(); ++index)
		if (expected.registers[index] != actual.registers[index])
			return values("x" + std::to_string(index),
				expected.registers[index],
				actual.registers[index]);
	if (expected.sp != actual.sp)
		return values("sp", expected.sp, actual.sp);
	if (expected.pc != actual.pc)
		return values("pc", expected.pc, actual.pc);
	if (expected.nzcv != actual.nzcv)
		return values("nzcv", expected.nzcv, actual.nzcv);
	for (std::size_t index = 0; index < expected.vectors.size(); ++index)
		for (std::size_t byte = 0; byte < 16U; ++byte)
			if (expected.vectors[index].bytes[byte] !=
			    actual.vectors[index].bytes[byte])
				return values("v" + std::to_string(index) +
						"[" + std::to_string(byte) + "]",
					std::to_integer<unsigned>(
						expected.vectors[index].bytes[byte]),
					std::to_integer<unsigned>(
						actual.vectors[index].bytes[byte]));
	if (expected.fpcr != actual.fpcr)
		return values("fpcr", expected.fpcr, actual.fpcr);
	if (expected.fpsr != actual.fpsr)
		return values("fpsr", expected.fpsr, actual.fpsr);
	if (expected.tls != actual.tls)
		return values("tls", expected.tls, actual.tls);
	if (expected.tbi_enabled != actual.tbi_enabled)
		return values("tbi", expected.tbi_enabled, actual.tbi_enabled);
	if (!(expected.pstate_dit == actual.pstate_dit &&
	      expected.pstate_daif_access == actual.pstate_daif_access &&
	      expected.pstate_daif == actual.pstate_daif))
		return std::string{"pstate"};
	if (expected.svcr != actual.svcr)
		return values("svcr", expected.svcr, actual.svcr);
	if (expected.tpidr2 != actual.tpidr2)
		return values("tpidr2", expected.tpidr2, actual.tpidr2);
	if (!(expected.exclusive_address == actual.exclusive_address &&
	      expected.exclusive_key == actual.exclusive_key &&
	      expected.exclusive_value == actual.exclusive_value &&
	      expected.exclusive_value2 == actual.exclusive_value2 &&
	      expected.exclusive_version == actual.exclusive_version &&
	      expected.exclusive_size == actual.exclusive_size))
		return std::string{"exclusive monitor"};
	return std::nullopt;
}

/* The first byte in which two SME register files of `svl` bytes per vector
 * differ, in the parts `svcr` makes live: Z and P while PSTATE.SM is set, ZA
 * and ZT0 while PSTATE.ZA is. The files have the layout of
 * cpu/arm64/sme_profile.hpp. */
[[nodiscard]] inline std::optional<std::string> first_sme_register_difference(
	unsigned svl, std::uint8_t svcr, std::span<const std::byte> expected,
	std::span<const std::byte> actual)
{
	namespace arm64 = ::uml::cpu::arm64;
	const auto compare = [&](std::string_view name, unsigned index,
				 std::size_t offset,
				 std::size_t size) -> std::optional<std::string> {
		for (std::size_t byte = 0; byte < size; ++byte)
			if (expected[offset + byte] != actual[offset + byte]) {
				char text[96];
				std::snprintf(text, sizeof(text),
					"%.*s%u[%zu] expected %#x actual %#x",
					static_cast<int>(name.size()), name.data(),
					index, byte,
					std::to_integer<unsigned>(
						expected[offset + byte]),
					std::to_integer<unsigned>(
						actual[offset + byte]));
				return std::string{text};
			}
		return std::nullopt;
	};
	if ((svcr & arm64::svcr_sm) != 0U) {
		for (unsigned n = 0; n < arm64::sme_z_registers; ++n)
			if (auto failure = compare("z", n,
					arm64::sme_z_offset(svl, n), svl))
				return failure;
		for (unsigned n = 0; n < arm64::sme_p_registers; ++n)
			if (auto failure = compare("p", n,
					arm64::sme_p_offset(svl, n), svl / 8U))
				return failure;
	}
	if ((svcr & arm64::svcr_za) != 0U) {
		for (unsigned row = 0; row < svl; ++row)
			if (auto failure = compare("za", row,
					arm64::sme_za_offset(svl, row), svl))
				return failure;
		if (auto failure = compare("zt", 0,
				arm64::sme_zt0_offset(svl),
				arm64::sme_zt0_bytes))
			return failure;
	}
	return std::nullopt;
}

} // namespace uml::emulator::tests
