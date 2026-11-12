/* SPDX-License-Identifier: GPL-2.0-or-later */
/* How a single rounds to a half decides VCVTPS2PH's underflow, overflow and
 * precision flags, and the engines compute it in integers. This test holds
 * that to a second derivation that computes with a double: every finite
 * single, in all four rounding controls, must give both the same four
 * answers; the negative singles, whose sign only picks a direction, are
 * covered by a stride and by the neighbourhoods of every threshold. */
#include "cpu/x86_64/sse_float.hpp"
#include "require.hpp"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <thread>
#include <vector>

namespace {

using uml::cpu::x86_64::sse_float::Rounded;
using uml::cpu::x86_64::sse_float::half_round;
using uml::testing::require;

/* The four flags of a rounding, without its result bits. */
struct HalfRound final {
	bool tiny{};
	bool inexact{};
	bool overflow{};
	bool significand_inexact{};

	[[nodiscard]] bool operator==(const HalfRound &) const noexcept = default;
};

[[nodiscard]] HalfRound integer_half_round(std::uint32_t x,
	unsigned control) noexcept
{
	const Rounded rounded = half_round(x, control);
	return {rounded.tiny, rounded.inexact, rounded.overflow,
		rounded.significand_inexact};
}

/* The double derivation of a single `x` rounded to a half, which SoftFloat
 * here lacks. Just below the smallest normal half, 2^-14, eleven bits step
 * by 2^-25: rounding to nearest reaches it from within 2^-26 and rounding
 * away from zero from within 2^-25. The halves delivered there step by
 * 2^-24. Just below 2^16, they step by 2^5, and the largest is 65504:
 * rounding to nearest overflows from 65520, rounding away from zero above
 * 65504, and rounding toward zero from 2^16. A double holds the single
 * exactly, and these bounds and scalings too. */
[[nodiscard]] HalfRound reference_half_round(std::uint32_t x,
	unsigned control) noexcept
{
	float single{};
	std::memcpy(&single, &x, sizeof single);
	const double magnitude = std::fabs(static_cast<double>(single));
	if (!(magnitude < 0x1p128) || magnitude == 0)
		return {};
	const bool negative = (x >> 31U) != 0;
	/* Rounding to nearest, away from zero, or toward zero. */
	const unsigned direction = (control & 3U) == 0 ? 0U :
		(control & 3U) == 3U ? 2U :
		((control & 3U) == 1U) == negative ? 1U : 2U;
	int exponent{};
	const double fraction = std::frexp(magnitude, &exponent);
	HalfRound result{};
	result.significand_inexact =
		fraction * 0x1p11 != std::floor(fraction * 0x1p11) ||
		((x >> 23U) & 0xffU) == 0;
	if (magnitude < 0x1p-14) {
		const double bound = direction == 0 ? 0x1p-14 - 0x1p-26 :
			0x1p-14 - 0x1p-25;
		result.tiny = direction == 2 || (direction == 0 ?
			magnitude < bound : !(magnitude > bound));
		const double steps = magnitude * 0x1p24;
		result.inexact = steps != std::floor(steps);
		return result;
	}
	result.overflow = direction == 0 ? magnitude >= 65520.0 :
		direction == 1 ? magnitude > 65504.0 : magnitude >= 0x1p16;
	result.inexact = result.overflow || result.significand_inexact;
	return result;
}

/* Whether the derivations agree on `bits`, a finite single that is not zero,
 * under every rounding control. */
[[nodiscard]] bool agree(std::uint32_t bits) noexcept
{
	for (unsigned control = 0; control < 4; ++control)
		if (reference_half_round(bits, control) !=
			integer_half_round(bits, control))
			return false;
	return true;
}

/* A finite, nonzero single is a magnitude below the infinity's. */
constexpr std::uint32_t magnitudes = 0x7f800000U;

void test_every_positive_single()
{
	const auto workers = std::clamp<unsigned>(
		std::thread::hardware_concurrency(), 1U, 8U);
	std::atomic<std::uint32_t> next{1};
	std::atomic<std::uint32_t> failed{0};
	constexpr std::uint32_t block = 1U << 16U;
	std::vector<std::thread> threads;
	for (unsigned worker = 0; worker < workers; ++worker)
		threads.emplace_back([&] {
			for (;;) {
				const auto begin = next.fetch_add(block);
				if (begin >= magnitudes)
					return;
				const auto end = std::min(begin + block, magnitudes);
				for (auto bits = begin; bits < end; ++bits)
					if (!agree(bits))
						failed.store(bits);
			}
		});
	for (auto &thread : threads)
		thread.join();
	require(failed.load() == 0, "a positive single rounds differently");
}

/* A negative single differs from its positive twin only in the direction a
 * rounding control picks, so a stride covers it. */
void test_negative_singles()
{
	for (std::uint32_t bits = 1; bits < magnitudes; bits += 251U)
		require(agree(bits | 0x80000000U), "a negative single rounds "
			"differently");
}

/* The neighbourhood of each threshold the half's range sets, of both signs:
 * the smallest normal half and the steps below it, the largest half, and the
 * point where it overflows. */
void test_thresholds()
{
	const std::uint32_t centres[]{
		0x38800000U, /* 2^-14 */
		0x387fffffU, 0x387fe000U, 0x387fc000U, /* just below it */
		0x33800000U, 0x33000000U, 0x33000001U, /* 2^-24, 2^-25 */
		0x32ffffffU, 0x32800000U, /* below 2^-25 */
		0x477fe000U, /* 65504 */
		0x477ff000U, /* 65520 */
		0x47800000U, /* 2^16 */
		0x00000001U, 0x007fffffU, 0x00800000U, /* single denormals */
		0x7f7fffffU, /* the largest single */
	};
	for (const auto centre : centres)
		for (std::uint32_t offset = 0; offset < 64U; ++offset)
			for (const bool above : {false, true}) {
				const auto bits = above ? centre + offset : centre - offset;
				if (bits == 0 || bits >= magnitudes)
					continue;
				require(agree(bits) && agree(bits | 0x80000000U),
					"a threshold single rounds differently");
			}
}

} // namespace

int main()
{
	test_thresholds();
	test_negative_singles();
	test_every_positive_single();
	return 0;
}
