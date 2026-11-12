// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_engine_test_support.hpp"

#include "cpu/arm64/gadgets/program.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <span>

namespace arm64_base_cache_zero_test {

namespace engine_tests = uml::emulator::tests;

[[nodiscard]] constexpr std::uint32_t instruction(unsigned source)
{
	return 0xd50b'7420U | (source & 31U);
}

} // namespace arm64_base_cache_zero_test

namespace test = arm64_base_cache_zero_test;
namespace gadgets = uml::cpu::arm64::gadgets;

namespace {

void production_specializes_source()
{
	constexpr auto pc = test::engine_tests::engine_test_code_base;
	const auto reg = gadgets::Builder{}.build(
		pc, std::array{test::instruction(0), 0xd400'0001U});
	const auto zero = gadgets::Builder{}.build(
		pc, std::array{test::instruction(31), 0xd400'0001U});
	if (!reg.complete || !zero.complete ||
	    reg.program.words().front() == zero.program.words().front())
		std::abort();
}

} // namespace

int main()
{
	production_specializes_source();
	return 0;
}
