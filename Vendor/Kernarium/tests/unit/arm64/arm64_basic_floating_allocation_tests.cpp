// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_basic_floating_test_support.hpp"
#include "cpu/arm64/detail/floating_round.hpp"

namespace test = arm64_basic_floating_test;

namespace {

void neighboring_allocation_remains_disjoint()
{
	using test::detail::decode_basic_floating_arithmetic;
	using test::detail::decode_floating_round;

	constexpr auto fsqrt_2s =
		test::instruction({test::Operation::square_root, false, 32U, 8U},
			{0U, 0U, 0U});
	constexpr auto fsqrt_4s =
		test::instruction({test::Operation::square_root, false, 32U, 16U},
			{0U, 0U, 0U});
	constexpr auto fsqrt_2d =
		test::instruction({test::Operation::square_root, false, 64U, 16U},
			{0U, 0U, 0U});
	test::common::require(fsqrt_2s == 0x2ea1'f800U);
	test::common::require(fsqrt_4s == 0x6ea1'f800U);
	test::common::require(fsqrt_2d == 0x6ee1'f800U);
	test::common::require(
		decode_basic_floating_arithmetic(fsqrt_2s).recognized);

	constexpr std::uint32_t neighboring_2s = 0x2e21'f800U;
	test::common::require(
		!decode_basic_floating_arithmetic(neighboring_2s).recognized);
	const auto rounded = decode_floating_round(neighboring_2s);
	test::common::require(rounded.recognized);
	test::common::require(rounded.valid);
}

} // namespace

int main()
{
	neighboring_allocation_remains_disjoint();
	return 0;
}
