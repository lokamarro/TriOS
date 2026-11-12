// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

/* A64 instruction classes the gadget and JIT body tests look for when they
 * walk emitted host code, and the walk itself. Test-local on purpose: the
 * oracle stays independent of the program builder's own encodings. */

#include <cstdint>

namespace uml::testing::a64 {

/* B.cond, CBZ/CBNZ, and TBZ/TBNZ. */
[[nodiscard]] constexpr bool conditional_branch(std::uint32_t instruction) noexcept
{
	return (instruction & 0xff00'0010U) == 0x5400'0000U ||
		(instruction & 0x7e00'0000U) == 0x3400'0000U ||
		(instruction & 0x7e00'0000U) == 0x3600'0000U;
}

/* BR Xn, the threaded dispatch that ends a gadget body. */
[[nodiscard]] constexpr bool indirect_branch(std::uint32_t instruction) noexcept
{
	return (instruction & 0xffff'fc1fU) == 0xd61f'0000U;
}

/* How far a body walk looks, and which way it goes. */
struct BodyScan final {
	/* Words inspected from the entry before the walk gives up on reaching
	 * the dispatch BR. */
	unsigned window{};
	/* Leading words passed over unseen, such as a prefix every gadget of a
	 * family shares. */
	unsigned skip{};
	/* Continue at an unconditional B's target instead of the next word. */
	bool follow_branches{};
};

/* What a walk found before the dispatch BR. */
struct BodyShape final {
	unsigned instructions{};
	unsigned conditional_branches{};
	bool reached_dispatch{};
};

/* Walks the gadget body at `entry` to its dispatch BR or the end of the
 * window. `visit` sees every word before the BR in the order the walk takes,
 * so a test can count or reject further instruction classes in place. The
 * walk itself never fails: a test that needs the BR checks
 * `reached_dispatch` and reports the miss in its own words. */
template<typename Visit>
[[nodiscard]] BodyShape body_shape(std::uintptr_t entry, BodyScan scan,
	Visit &&visit)
{
	const auto *word = reinterpret_cast<const std::uint32_t *>(entry) +
		scan.skip;
	BodyShape shape{};
	for (unsigned step = scan.skip; step < scan.window; ++step) {
		const auto instruction = *word;
		if (indirect_branch(instruction)) {
			shape.reached_dispatch = true;
			return shape;
		}
		visit(instruction);
		++shape.instructions;
		shape.conditional_branches += conditional_branch(instruction);
		if (scan.follow_branches &&
		    (instruction & 0xfc00'0000U) == 0x1400'0000U)
			word += static_cast<std::int32_t>(instruction << 6) >> 6;
		else
			++word;
	}
	return shape;
}

[[nodiscard]] inline BodyShape body_shape(std::uintptr_t entry, BodyScan scan)
{
	return body_shape(entry, scan, [](std::uint32_t) noexcept {});
}

} // namespace uml::testing::a64
