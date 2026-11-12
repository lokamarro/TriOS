// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_engine_test_support.hpp"
#include "require.hpp"

#include "cpu/arm64/gadgets/program.hpp"

#include <array>
#include <cstdint>
#include <vector>

namespace arm64 = uml::cpu::arm64;
namespace engine_tests = uml::emulator::tests;
namespace gadgets = uml::cpu::arm64::gadgets;

namespace {

using uml::testing::require;

[[nodiscard]] constexpr std::uint32_t rprfm(unsigned operation,
	unsigned metadata, unsigned base)
{
	/* operation = option<2>:option<0>:S:Rt<2:0>; option<1> and
	 * Rt<4:3> are the fixed RPRFM allocation bits. */
	const unsigned option = (((operation >> 5) & 1U) << 2) | 2U |
		((operation >> 4) & 1U);
	const unsigned scale = (operation >> 3) & 1U;
	const unsigned rt = 24U | (operation & 7U);
	return 0xf8a0'0800U | ((metadata & 31U) << 16) |
		(option << 13) | (scale << 12) | ((base & 31U) << 5) | rt;
}

void encoding_examples()
{
	require(rprfm(0, 3, 2) == 0xf8a3'4858U); // PLDKEEP
	require(rprfm(1, 3, 2) == 0xf8a3'4859U); // PSTKEEP
	require(rprfm(4, 3, 2) == 0xf8a3'485cU); // PLDSTRM
	require(rprfm(5, 3, 2) == 0xf8a3'485dU); // PSTSTRM
	require(rprfm(63, 30, 31) == 0xf8be'fbffU);
}

void production_is_a_direct_nop_gadget()
{
	const auto nop = gadgets::Builder{}.build(
		engine_tests::engine_test_code_base,
		std::array{0xd503'201fU});
	for (unsigned operation = 0; operation < 64; ++operation) {
		const auto built = gadgets::Builder{}.build(
			engine_tests::engine_test_code_base,
			std::array{rprfm(operation, 30, 31)});
		require(built.complete && built.program.words().front() ==
			nop.program.words().front());
	}
}

} // namespace

int main()
{
	encoding_examples();
	production_is_a_direct_nop_gadget();
}
