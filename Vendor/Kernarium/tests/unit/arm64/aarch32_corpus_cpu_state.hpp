// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

/* The AArch32 corpus on the virtual CPU's state: a case's registers in the
 * CpuState an engine or the semantic library starts from, the APSR and FPSCR
 * read back out of one, and the comparison of a final CpuState with what the
 * oracle recorded. The lanes that compare field by field share it; the
 * decoder lane, which has no CpuState, does not include it.
 *
 * The FPSCR masks are written out here, not taken from the library under
 * test, so the model lane checks the library's split against an independent
 * one. */

#include "aarch32_corpus_pack.hpp"
#include "cpu/arm64/aarch32/guest_key.hpp"
#include "cpu/arm64/cpu_state.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <optional>
#include <span>
#include <string>

namespace uml::testing::aarch32_corpus {

/* The FPSCR halves the virtual CPU holds: AHP, DN, FZ, RMode and FZ16 in
 * fpcr; QC and the cumulative flags in fpsr; N, Z, C and V in
 * a32_fpscr_nzcv. */
inline constexpr std::uint32_t kFpscrControlMask = 0x07c8'0000U;
inline constexpr std::uint32_t kFpscrStatusMask = 0x0800'009fU;

/* What an AArch64 task leaves in the general-register slots that are no
 * AArch32 register: the runtime loads R0-R14 for an AArch32 entry and keeps
 * x15-x30 and SP of whatever ran on the virtual CPU before. Both halves of
 * every value are set, so an engine that reads one, whole or as a 32-bit
 * value, depends on it. The lanes start every case from this, as a task does
 * on a virtual CPU that has run 64-bit code. */
inline void leave_aarch64_task_state(::uml::cpu::arm64::CpuState &cpu)
{
	for (unsigned index = kRegisters; index < cpu.registers.size(); ++index)
		cpu.registers[index] = 0xa64d'0000'0000'0badULL |
			std::uint64_t{index} << 32 | std::uint64_t{index} << 16;
	cpu.sp = 0xa64d'5050'5050'0badULL;
}

/* The CpuState a case starts in, entered at `entry` in the case's state. */
[[nodiscard]] inline ::uml::cpu::arm64::CpuState cpu_from(const State &state, bool thumb, std::uint32_t entry)
{
	namespace dec = ::uml::cpu::arm64::aarch32;
	::uml::cpu::arm64::CpuState cpu{};
	leave_aarch64_task_state(cpu);
	cpu.a32 = 1;
	cpu.pc = thumb ? dec::t32_key(entry) : dec::a32_key(entry);
	for (unsigned index = 0; index < kRegisters; ++index)
		cpu.registers[index] = state.r[index];
	cpu.nzcv = state.apsr & 0xf000'0000U;
	cpu.a32_q = static_cast<std::uint8_t>(state.apsr >> 27 & 1U);
	for (unsigned lane = 0; lane < 4; ++lane)
		if (state.apsr >> (16 + lane) & 1U)
			cpu.a32_ge_mask |= 0xffU << (8 * lane);
	cpu.a32_cp15ben = 1;
	cpu.a32_tpidruro = state.tpidruro;
	cpu.tls = state.tpidrurw;
	for (unsigned d = 0; d < kVectors; ++d)
		std::memcpy(cpu.vectors[d / 2].bytes.data() + 8U * (d & 1U), &state.d[d], 8);
	cpu.fpcr = state.fpscr & kFpscrControlMask;
	cpu.fpsr = state.fpscr & kFpscrStatusMask;
	cpu.a32_fpscr_nzcv = state.fpscr & 0xf000'0000U;
	return cpu;
}

[[nodiscard]] inline std::uint32_t fpscr_of(const ::uml::cpu::arm64::CpuState &cpu)
{
	return (cpu.fpcr & kFpscrControlMask) | (cpu.fpsr & kFpscrStatusMask) |
		(cpu.a32_fpscr_nzcv & 0xf000'0000U);
}

[[nodiscard]] inline std::uint32_t apsr_of(const ::uml::cpu::arm64::CpuState &cpu)
{
	std::uint32_t apsr = static_cast<std::uint32_t>(cpu.nzcv) & 0xf000'0000U;
	if (cpu.a32_q != 0U)
		apsr |= 1U << 27;
	for (unsigned lane = 0; lane < 4; ++lane)
		if ((cpu.a32_ge_mask >> (8 * lane) & 0xffU) != 0U)
			apsr |= 1U << (16 + lane);
	return apsr;
}

/* The first way the state after the registers differs from the oracle's: the
 * APSR, the thread registers, D0-D31, the FPSCR, then the data region. */
[[nodiscard]] inline std::optional<std::string> state_difference(const ::uml::cpu::arm64::CpuState &cpu,
	const Case &test, std::span<const std::uint8_t> actual_data, std::span<const std::uint8_t> expected_data)
{
	char text[512];
	if (apsr_of(cpu) != test.final.apsr) {
		std::snprintf(text, sizeof text, "apsr %08x, expected %08x", apsr_of(cpu), test.final.apsr);
		return text;
	}
	if (cpu.a32_tpidruro != test.final.tpidruro || cpu.tls != test.final.tpidrurw)
		return "a thread register changed";
	for (unsigned d = 0; d < kVectors; ++d) {
		std::uint64_t value;
		std::memcpy(&value, cpu.vectors[d / 2].bytes.data() + 8U * (d & 1U), 8);
		if (value != test.final.d[d]) {
			std::snprintf(text, sizeof text, "d%u = %016llx, expected %016llx", d,
				static_cast<unsigned long long>(value),
				static_cast<unsigned long long>(test.final.d[d]));
			return text;
		}
	}
	if (fpscr_of(cpu) != test.final.fpscr) {
		std::snprintf(text, sizeof text, "fpscr %08x, expected %08x", fpscr_of(cpu), test.final.fpscr);
		return text;
	}
	if (!std::equal(actual_data.begin(), actual_data.end(), expected_data.begin(), expected_data.end())) {
		std::size_t at = 0;
		while (expected_data[at] == actual_data[at])
			++at;
		std::snprintf(text, sizeof text, "data differs at +%zx: %02x, expected %02x", at,
			static_cast<unsigned>(actual_data[at]), static_cast<unsigned>(expected_data[at]));
		return text;
	}
	return std::nullopt;
}

} // namespace uml::testing::aarch32_corpus
