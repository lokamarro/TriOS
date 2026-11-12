// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "arm64_engine_test_support.hpp"
#include "require.hpp"

#include <cstddef>
#include <cstdint>
#include <string_view>

namespace arm64_system_test {

namespace arm64 = uml::cpu::arm64;
namespace engine_tests = uml::emulator::tests;

using uml::testing::require;

[[nodiscard]] inline arm64::CpuState state()
{
	arm64::CpuState cpu{};
	for (std::size_t index = 0; index < cpu.registers.size(); ++index)
		cpu.registers[index] = 0x9182'7364'5546'3728ULL ^
			(static_cast<std::uint64_t>(index) *
				0x0101'0101'0101'0101ULL);
	for (std::size_t reg = 0; reg < cpu.vectors.size(); ++reg)
		for (std::size_t byte = 0;
		     byte < cpu.vectors[reg].bytes.size(); ++byte)
			cpu.vectors[reg].bytes[byte] =
				static_cast<std::byte>(reg * 11U + byte * 23U);
	cpu.sp = 0xfeed'face'cafe'bee0ULL;
	cpu.nzcv = 0xa000'0000U;
	cpu.fpcr = arm64::fpcr_writable_mask;
	cpu.fpsr = arm64::fpsr_writable_mask;
	cpu.tls = 0x1234'5678'9abc'def0ULL;
	cpu.tbi_enabled = true;
	cpu.pstate_dit = true;
	cpu.pstate_daif = 0xaU;
	cpu.exclusive_address = 0x2000;
	cpu.exclusive_key = 0x3456;
	cpu.exclusive_value = 0x1020'3040'5060'7080ULL;
	cpu.exclusive_value2 = 0x8877'6655'4433'2211ULL;
	cpu.exclusive_version = 17;
	cpu.exclusive_size = 8;
	return cpu;
}

class Harness final {
public:
	void check(std::string_view name, std::uint32_t instruction,
		const arm64::CpuState &initial, const arm64::CpuState &expected,
		engine_tests::NativeRequirements native)
	{
		engines_.check_one(name, instruction, initial, expected, native);
	}

	void check_unchanged(std::string_view name, std::uint32_t instruction,
		engine_tests::NativeRequirements native =
			engine_tests::NativeRequirements::baseline())
	{
		const auto initial = state();
		check(name, instruction, initial, initial, native);
	}

	void require_illegal(std::uint32_t instruction)
	{
		const auto initial = state();
		engines_.check_illegal("base-system-illegal", instruction, initial);
	}

	void print_summary(std::string_view name) const
	{
		engines_.print_summary(name);
	}

	engine_tests::EngineHarness &engines()
	{
		return engines_;
	}

private:
	engine_tests::EngineHarness engines_;
};

[[nodiscard]] constexpr std::uint32_t system_instruction(bool result,
	unsigned op1, unsigned crn, unsigned crm, unsigned op2, unsigned rt)
{
	return (result ? 0xd528'0000U : 0xd508'0000U) |
		((op1 & 7U) << 16) | ((crn & 15U) << 12) |
		((crm & 15U) << 8) | ((op2 & 7U) << 5) | (rt & 31U);
}

} // namespace arm64_system_test
